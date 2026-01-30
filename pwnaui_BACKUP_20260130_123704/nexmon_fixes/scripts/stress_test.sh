#!/bin/bash
#
# Nexmon Stress Test - Test firmware stability under load
#
# This script puts the WiFi firmware through various stress tests
# to verify the patches are working correctly.
#
# WARNING: This will cause interference. Use only in controlled
# environments where you have permission to transmit.
#

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

INTERFACE="${1:-wlan0}"
TEST_DURATION="${2:-60}"
LOG_FILE="/tmp/nexmon_stress_test_$(date +%Y%m%d_%H%M%S).log"

log() { echo -e "${GREEN}[+]${NC} $1" | tee -a "$LOG_FILE"; }
warn() { echo -e "${YELLOW}[!]${NC} $1" | tee -a "$LOG_FILE"; }
error() { echo -e "${RED}[-]${NC} $1" | tee -a "$LOG_FILE"; }
info() { echo -e "${BLUE}[*]${NC} $1" | tee -a "$LOG_FILE"; }

# Check root
if [[ $EUID -ne 0 ]]; then
    error "This script must be run as root"
    exit 1
fi

# Check interface
if ! ip link show "$INTERFACE" &>/dev/null; then
    error "Interface $INTERFACE not found"
    exit 1
fi

echo ""
echo -e "${GREEN}╔══════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║           Nexmon Firmware Stress Test Suite              ║${NC}"
echo -e "${GREEN}╚══════════════════════════════════════════════════════════╝${NC}"
echo ""
echo "Interface: $INTERFACE"
echo "Duration: ${TEST_DURATION}s per test"
echo "Log file: $LOG_FILE"
echo ""

# Record initial state
record_state() {
    local state_file="/tmp/nexmon_state_$(date +%s).txt"
    echo "=== State captured at $(date) ===" >> "$state_file"
    ip link show "$INTERFACE" >> "$state_file" 2>&1 || true
    iw dev "$INTERFACE" info >> "$state_file" 2>&1 || true
    dmesg | tail -50 | grep -i 'brcm\|wlan\|wlc\|sdio' >> "$state_file" 2>&1 || true
    cat /sys/kernel/debug/ieee80211/*/brcmfmac/fwsignal 2>/dev/null >> "$state_file" || true
    echo "$state_file"
}

# Test 1: Monitor Mode Toggle
test_monitor_mode() {
    log "TEST 1: Monitor Mode Toggle (10 iterations)"
    
    local failures=0
    for i in $(seq 1 10); do
        info "  Iteration $i/10..."
        
        # Down
        ip link set "$INTERFACE" down 2>/dev/null
        sleep 0.5
        
        # Set monitor
        iw dev "$INTERFACE" set type monitor 2>/dev/null
        sleep 0.5
        
        # Up
        ip link set "$INTERFACE" up 2>/dev/null
        sleep 1
        
        # Verify
        if iw dev "$INTERFACE" info | grep -q "type monitor"; then
            info "    ✓ Monitor mode set"
        else
            warn "    ✗ Monitor mode failed"
            ((failures++))
        fi
        
        # Back to managed
        ip link set "$INTERFACE" down 2>/dev/null
        sleep 0.5
        iw dev "$INTERFACE" set type managed 2>/dev/null
        sleep 0.5
        ip link set "$INTERFACE" up 2>/dev/null
        sleep 1
    done
    
    if [[ $failures -eq 0 ]]; then
        log "  ✓ Monitor mode test PASSED"
        return 0
    else
        warn "  ✗ Monitor mode test: $failures failures"
        return 1
    fi
}

# Test 2: Channel Hopping
test_channel_hop() {
    log "TEST 2: Rapid Channel Hopping (${TEST_DURATION}s)"
    
    # Ensure monitor mode
    ip link set "$INTERFACE" down
    iw dev "$INTERFACE" set type monitor
    ip link set "$INTERFACE" up
    sleep 1
    
    local channels="1 6 11 36 40 44 48 149 153 157 161"
    local start_time=$(date +%s)
    local hop_count=0
    local failures=0
    
    while true; do
        local now=$(date +%s)
        local elapsed=$((now - start_time))
        
        if [[ $elapsed -ge $TEST_DURATION ]]; then
            break
        fi
        
        for ch in $channels; do
            # Try to set channel
            if ! iw dev "$INTERFACE" set channel "$ch" 2>/dev/null; then
                ((failures++))
                warn "  Channel $ch failed"
            fi
            ((hop_count++))
            
            # Small delay (50ms)
            sleep 0.05
            
            # Check elapsed time
            now=$(date +%s)
            elapsed=$((now - start_time))
            if [[ $elapsed -ge $TEST_DURATION ]]; then
                break 2
            fi
        done
    done
    
    info "  Completed $hop_count channel hops"
    info "  Failures: $failures"
    
    # Check if interface is still responsive
    if iw dev "$INTERFACE" info &>/dev/null; then
        log "  ✓ Channel hop test PASSED (interface responsive)"
        return 0
    else
        error "  ✗ Channel hop test FAILED (interface crashed)"
        return 1
    fi
}

# Test 3: Frame Injection (if aircrack-ng available)
test_injection() {
    log "TEST 3: Frame Injection Stress (${TEST_DURATION}s)"
    
    if ! command -v aireplay-ng &>/dev/null; then
        warn "  aireplay-ng not installed, skipping injection test"
        return 0
    fi
    
    # Ensure monitor mode
    ip link set "$INTERFACE" down 2>/dev/null
    iw dev "$INTERFACE" set type monitor 2>/dev/null
    ip link set "$INTERFACE" up 2>/dev/null
    sleep 1
    
    # Run injection test
    log "  Running injection test..."
    
    # Send broadcast probes for duration
    timeout "${TEST_DURATION}" aireplay-ng --test -i "$INTERFACE" "$INTERFACE" 2>&1 | tee -a "$LOG_FILE" &
    local pid=$!
    
    # Monitor for crashes
    local crashed=0
    while kill -0 $pid 2>/dev/null; do
        sleep 5
        if ! ip link show "$INTERFACE" &>/dev/null; then
            error "  Interface disappeared during injection!"
            crashed=1
            break
        fi
        if dmesg | tail -10 | grep -q "bus is down\|card removed"; then
            error "  Firmware crash detected!"
            crashed=1
            break
        fi
    done
    
    wait $pid 2>/dev/null || true
    
    if [[ $crashed -eq 0 ]]; then
        log "  ✓ Injection test PASSED"
        return 0
    else
        error "  ✗ Injection test FAILED"
        record_state
        return 1
    fi
}

# Test 4: Continuous Scan
test_continuous_scan() {
    log "TEST 4: Continuous Scanning (${TEST_DURATION}s)"
    
    # Set to managed mode
    ip link set "$INTERFACE" down 2>/dev/null
    iw dev "$INTERFACE" set type managed 2>/dev/null
    ip link set "$INTERFACE" up 2>/dev/null
    sleep 1
    
    local start_time=$(date +%s)
    local scan_count=0
    local failures=0
    
    while true; do
        local now=$(date +%s)
        local elapsed=$((now - start_time))
        
        if [[ $elapsed -ge $TEST_DURATION ]]; then
            break
        fi
        
        # Trigger scan
        if iw dev "$INTERFACE" scan trigger 2>/dev/null; then
            ((scan_count++))
        else
            ((failures++))
        fi
        
        sleep 2
    done
    
    info "  Completed $scan_count scans"
    info "  Failures: $failures"
    
    if [[ $failures -lt $((scan_count / 5)) ]]; then
        log "  ✓ Scan test PASSED"
        return 0
    else
        warn "  ✗ Scan test had too many failures"
        return 1
    fi
}

# Test 5: Combined Load
test_combined() {
    log "TEST 5: Combined Load Test (${TEST_DURATION}s)"
    
    # Monitor mode
    ip link set "$INTERFACE" down 2>/dev/null
    iw dev "$INTERFACE" set type monitor 2>/dev/null
    ip link set "$INTERFACE" up 2>/dev/null
    sleep 1
    
    local start_time=$(date +%s)
    local total_ops=0
    local crashes=0
    
    while true; do
        local now=$(date +%s)
        local elapsed=$((now - start_time))
        
        if [[ $elapsed -ge $TEST_DURATION ]]; then
            break
        fi
        
        # Channel hop
        iw dev "$INTERFACE" set channel $((RANDOM % 11 + 1)) 2>/dev/null
        ((total_ops++))
        sleep 0.1
        
        # Check health
        if dmesg | tail -5 | grep -q "bus is down\|firmware fail\|card removed"; then
            ((crashes++))
            warn "  Firmware issue detected at op $total_ops"
            # Try recovery
            /usr/local/bin/wifi_recovery.sh recover 2>/dev/null || true
            sleep 2
        fi
        
        ((total_ops++))
    done
    
    info "  Total operations: $total_ops"
    info "  Crashes detected: $crashes"
    
    if [[ $crashes -eq 0 ]]; then
        log "  ✓ Combined test PASSED"
        return 0
    else
        warn "  ✗ Combined test had $crashes crashes"
        return 1
    fi
}

# Summary
summary() {
    echo ""
    echo -e "${GREEN}╔══════════════════════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}║                    Test Summary                          ║${NC}"
    echo -e "${GREEN}╚══════════════════════════════════════════════════════════╝${NC}"
    echo ""
    
    local total=0
    local passed=0
    
    for result in "${RESULTS[@]}"; do
        ((total++))
        if [[ "$result" == "PASS" ]]; then
            ((passed++))
            echo -e "  ${GREEN}✓${NC} Test $total: PASSED"
        else
            echo -e "  ${RED}✗${NC} Test $total: FAILED"
        fi
    done
    
    echo ""
    echo "Passed: $passed / $total"
    echo "Log saved to: $LOG_FILE"
    echo ""
    
    if [[ $passed -eq $total ]]; then
        echo -e "${GREEN}All tests passed! Firmware appears stable.${NC}"
        return 0
    else
        echo -e "${YELLOW}Some tests failed. Check logs for details.${NC}"
        return 1
    fi
}

# Main
RESULTS=()

# Initial state
info "Recording initial state..."
record_state

# Run tests
if test_monitor_mode; then
    RESULTS+=("PASS")
else
    RESULTS+=("FAIL")
fi

if test_channel_hop; then
    RESULTS+=("PASS")
else
    RESULTS+=("FAIL")
fi

if test_injection; then
    RESULTS+=("PASS")
else
    RESULTS+=("FAIL")
fi

if test_continuous_scan; then
    RESULTS+=("PASS")
else
    RESULTS+=("FAIL")
fi

if test_combined; then
    RESULTS+=("PASS")
else
    RESULTS+=("FAIL")
fi

# Final state
info "Recording final state..."
record_state

# Summary
summary
