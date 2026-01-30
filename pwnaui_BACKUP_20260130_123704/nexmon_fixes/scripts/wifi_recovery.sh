#!/bin/bash
#
# Nexmon WiFi Recovery Script
# 
# This script monitors for the "blindness bug" and firmware crashes,
# then performs proper recovery procedures.
#
# Usage: ./wifi_recovery.sh [monitor|recover|watch]
#
# The script handles:
# 1. Channel hop failures (-110 errors)
# 2. Firmware crashes (firmware halted)
# 3. Device disappearance (No such device)
#
# Based on issues:
# - https://github.com/seemoo-lab/nexmon/issues/335
# - https://github.com/evilsocket/pwnagotchi/issues/267
#

set -e

# Configuration
INTERFACE="${NEXMON_INTERFACE:-wlan0mon}"
BASE_INTERFACE="${NEXMON_BASE_INTERFACE:-wlan0}"
DRIVER="brcmfmac"
DRIVER_UTIL="brcmutil"
LOG_FILE="/var/log/nexmon_recovery.log"
MAX_RECOVERY_ATTEMPTS=3
BLIND_THRESHOLD=5  # Number of consecutive channel hop failures
CHECK_INTERVAL=5   # Seconds between checks in watch mode

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Logging function
log() {
    local level="$1"
    local message="$2"
    local timestamp=$(date '+%Y-%m-%d %H:%M:%S')
    echo -e "${timestamp} [${level}] ${message}" | tee -a "$LOG_FILE"
}

# Check if running as root
check_root() {
    if [[ $EUID -ne 0 ]]; then
        echo -e "${RED}This script must be run as root${NC}"
        exit 1
    fi
}

# Get current WiFi firmware info
get_firmware_info() {
    if dmesg | grep -q "brcmfmac:.*version"; then
        dmesg | grep "brcmfmac:.*version" | tail -1 | sed 's/.*version \([^ ]*\).*/\1/'
    else
        echo "unknown"
    fi
}

# Check for blindness bug (channel hop failures)
check_blindness() {
    local error_count
    error_count=$(dmesg | grep -c "brcmf_cfg80211_nexmon_set_channel: Set Channel failed" 2>/dev/null || echo "0")
    
    # Check recent errors (last 30 seconds)
    local recent_errors
    recent_errors=$(journalctl --since "30 seconds ago" 2>/dev/null | grep -c "Set Channel failed" || echo "0")
    
    if [[ $recent_errors -ge $BLIND_THRESHOLD ]]; then
        log "ERROR" "Blindness bug detected! $recent_errors recent channel hop failures"
        return 0  # Bug detected
    fi
    return 1  # No bug
}

# Check for firmware crash
check_firmware_crash() {
    local crash_detected
    crash_detected=$(journalctl --since "60 seconds ago" 2>/dev/null | grep -c "Firmware has halted or crashed" || echo "0")
    
    if [[ $crash_detected -gt 0 ]]; then
        log "ERROR" "Firmware crash detected!"
        return 0  # Crash detected
    fi
    return 1  # No crash
}

# Check if interface exists
check_interface() {
    local iface="$1"
    if ip link show "$iface" &>/dev/null; then
        return 0  # Interface exists
    fi
    return 1  # Interface doesn't exist
}

# Check if monitor mode is active
check_monitor_mode() {
    if iw dev "$INTERFACE" info 2>/dev/null | grep -q "type monitor"; then
        return 0  # Monitor mode active
    fi
    return 1  # Not in monitor mode
}

# Reload the brcmfmac driver (soft recovery)
reload_driver() {
    log "INFO" "Attempting soft recovery - reloading driver"
    
    # Stop any processes using the interface
    if systemctl is-active --quiet pwnagotchi; then
        log "INFO" "Stopping pwnagotchi service"
        systemctl stop pwnagotchi
    fi
    
    # Remove monitor interface if exists
    if check_interface "$INTERFACE"; then
        log "INFO" "Removing monitor interface $INTERFACE"
        ip link set "$INTERFACE" down 2>/dev/null || true
        iw dev "$INTERFACE" del 2>/dev/null || true
        sleep 1
    fi
    
    # Unload driver
    log "INFO" "Unloading $DRIVER driver"
    rmmod "$DRIVER" 2>/dev/null || true
    sleep 2
    
    # Reload driver
    log "INFO" "Reloading drivers"
    modprobe "$DRIVER_UTIL"
    modprobe "$DRIVER"
    sleep 3
    
    # Wait for interface to come up
    local count=0
    while ! check_interface "$BASE_INTERFACE" && [[ $count -lt 10 ]]; do
        sleep 1
        ((count++))
    done
    
    if check_interface "$BASE_INTERFACE"; then
        log "INFO" "Driver reload successful, $BASE_INTERFACE is up"
        return 0
    else
        log "ERROR" "Driver reload failed, $BASE_INTERFACE not found"
        return 1
    fi
}

# Full SDIO reset (hard recovery)
reset_sdio() {
    log "INFO" "Attempting hard recovery - SDIO reset"
    
    # Find the MMC host for WiFi (usually mmc1)
    local mmc_host="/sys/bus/sdio/devices/mmc1:0001:1"
    local mmc_unbind="/sys/bus/mmc/drivers/mmc_core/unbind"
    local mmc_bind="/sys/bus/mmc/drivers/mmc_core/bind"
    local mmc_device="mmc1:0001"
    
    # First try driver reload
    reload_driver
    
    # If that doesn't work, try SDIO unbind/bind
    if ! check_interface "$BASE_INTERFACE"; then
        log "INFO" "Attempting SDIO unbind/bind"
        
        if [[ -f "$mmc_unbind" ]]; then
            echo "$mmc_device" > "$mmc_unbind" 2>/dev/null || true
            sleep 2
        fi
        
        if [[ -f "$mmc_bind" ]]; then
            echo "$mmc_device" > "$mmc_bind" 2>/dev/null || true
            sleep 3
        fi
        
        # Reload driver again
        modprobe "$DRIVER"
        sleep 3
    fi
    
    if check_interface "$BASE_INTERFACE"; then
        log "INFO" "SDIO reset successful"
        return 0
    else
        log "ERROR" "SDIO reset failed - may need full reboot"
        return 1
    fi
}

# Setup monitor mode safely
setup_monitor_mode() {
    log "INFO" "Setting up monitor mode safely"
    
    # Make sure interface is up
    if ! check_interface "$BASE_INTERFACE"; then
        log "ERROR" "Base interface $BASE_INTERFACE not found"
        return 1
    fi
    
    # Unblock WiFi
    rfkill unblock wifi 2>/dev/null || true
    rfkill unblock all 2>/dev/null || true
    
    # Bring up base interface
    ip link set "$BASE_INTERFACE" up
    sleep 2  # Critical delay - prevents immediate crash on some devices
    
    # Disable power save
    iw dev "$BASE_INTERFACE" set power_save off 2>/dev/null || true
    sleep 1
    
    # Get phy name
    local phy
    phy=$(iw dev "$BASE_INTERFACE" info 2>/dev/null | grep wiphy | awk '{print "phy"$2}')
    
    if [[ -z "$phy" ]]; then
        log "ERROR" "Could not determine phy for $BASE_INTERFACE"
        return 1
    fi
    
    # Create monitor interface
    log "INFO" "Creating monitor interface on $phy"
    iw phy "$phy" interface add "$INTERFACE" type monitor 2>/dev/null || {
        log "ERROR" "Failed to create monitor interface"
        return 1
    }
    sleep 2  # Another critical delay
    
    # Unblock again after creating interface
    rfkill unblock all 2>/dev/null || true
    
    # Bring down base interface (optional but recommended)
    ip link set "$BASE_INTERFACE" down 2>/dev/null || true
    
    # Bring up monitor interface
    ip link set "$INTERFACE" up
    sleep 1
    
    # Disable power save on monitor interface
    iw dev "$INTERFACE" set power_save off 2>/dev/null || true
    
    if check_monitor_mode; then
        log "INFO" "Monitor mode setup successful on $INTERFACE"
        return 0
    else
        log "ERROR" "Monitor mode setup failed"
        return 1
    fi
}

# Full recovery procedure
perform_recovery() {
    local attempt=1
    
    while [[ $attempt -le $MAX_RECOVERY_ATTEMPTS ]]; do
        log "INFO" "Recovery attempt $attempt of $MAX_RECOVERY_ATTEMPTS"
        
        # Try soft recovery first
        if reload_driver; then
            sleep 2
            if setup_monitor_mode; then
                log "INFO" "Recovery successful on attempt $attempt"
                
                # Restart pwnagotchi if it was running
                if systemctl is-enabled --quiet pwnagotchi 2>/dev/null; then
                    log "INFO" "Restarting pwnagotchi service"
                    systemctl start pwnagotchi
                fi
                
                return 0
            fi
        fi
        
        # If soft recovery failed, try hard recovery
        if [[ $attempt -ge 2 ]]; then
            log "INFO" "Soft recovery failed, attempting SDIO reset"
            if reset_sdio; then
                sleep 2
                if setup_monitor_mode; then
                    log "INFO" "Recovery successful after SDIO reset"
                    
                    if systemctl is-enabled --quiet pwnagotchi 2>/dev/null; then
                        systemctl start pwnagotchi
                    fi
                    
                    return 0
                fi
            fi
        fi
        
        ((attempt++))
        sleep 5
    done
    
    log "ERROR" "All recovery attempts failed. System reboot recommended."
    return 1
}

# Continuous monitoring mode
watch_mode() {
    log "INFO" "Starting continuous monitoring (interval: ${CHECK_INTERVAL}s)"
    
    local consecutive_failures=0
    
    while true; do
        # Check for firmware crash first (more severe)
        if check_firmware_crash; then
            log "WARNING" "Firmware crash detected, initiating recovery"
            consecutive_failures=0
            perform_recovery
            sleep 10  # Wait a bit after recovery
            continue
        fi
        
        # Check for blindness bug
        if check_blindness; then
            ((consecutive_failures++))
            log "WARNING" "Channel hop failures detected ($consecutive_failures consecutive)"
            
            if [[ $consecutive_failures -ge 3 ]]; then
                log "ERROR" "Too many consecutive failures, initiating recovery"
                consecutive_failures=0
                perform_recovery
                sleep 10
            fi
        else
            consecutive_failures=0
        fi
        
        # Check if interface still exists
        if ! check_interface "$INTERFACE"; then
            log "ERROR" "Monitor interface disappeared!"
            perform_recovery
            sleep 10
            continue
        fi
        
        sleep "$CHECK_INTERVAL"
    done
}

# Show current status
show_status() {
    echo -e "${GREEN}=== Nexmon WiFi Status ===${NC}"
    echo ""
    
    # Firmware version
    local fw_version
    fw_version=$(get_firmware_info)
    echo -e "Firmware Version: ${YELLOW}$fw_version${NC}"
    
    # Interface status
    if check_interface "$BASE_INTERFACE"; then
        echo -e "Base Interface ($BASE_INTERFACE): ${GREEN}UP${NC}"
    else
        echo -e "Base Interface ($BASE_INTERFACE): ${RED}DOWN${NC}"
    fi
    
    if check_interface "$INTERFACE"; then
        echo -e "Monitor Interface ($INTERFACE): ${GREEN}UP${NC}"
        if check_monitor_mode; then
            echo -e "Monitor Mode: ${GREEN}ACTIVE${NC}"
        else
            echo -e "Monitor Mode: ${RED}INACTIVE${NC}"
        fi
    else
        echo -e "Monitor Interface ($INTERFACE): ${RED}NOT FOUND${NC}"
    fi
    
    # Error counts
    local channel_errors
    local crash_count
    channel_errors=$(dmesg | grep -c "Set Channel failed" 2>/dev/null || echo "0")
    crash_count=$(dmesg | grep -c "Firmware has halted" 2>/dev/null || echo "0")
    
    echo ""
    echo -e "Channel Hop Errors: ${YELLOW}$channel_errors${NC}"
    echo -e "Firmware Crashes: ${YELLOW}$crash_count${NC}"
    
    echo ""
}

# Main function
main() {
    check_root
    
    case "${1:-status}" in
        monitor|status)
            show_status
            ;;
        recover)
            log "INFO" "Manual recovery initiated"
            perform_recovery
            ;;
        watch)
            watch_mode
            ;;
        setup)
            setup_monitor_mode
            ;;
        reload)
            reload_driver
            ;;
        reset)
            reset_sdio
            ;;
        *)
            echo "Usage: $0 [status|recover|watch|setup|reload|reset]"
            echo ""
            echo "Commands:"
            echo "  status  - Show current WiFi status (default)"
            echo "  recover - Perform full recovery procedure"
            echo "  watch   - Continuous monitoring with auto-recovery"
            echo "  setup   - Setup monitor mode safely"
            echo "  reload  - Reload brcmfmac driver"
            echo "  reset   - Full SDIO reset"
            echo ""
            echo "Environment Variables:"
            echo "  NEXMON_INTERFACE      - Monitor interface name (default: wlan0mon)"
            echo "  NEXMON_BASE_INTERFACE - Base interface name (default: wlan0)"
            exit 1
            ;;
    esac
}

main "$@"
