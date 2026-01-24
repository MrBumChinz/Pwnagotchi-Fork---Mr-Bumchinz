#!/bin/bash
#
# PwnaUI Nexmon Recovery Script
#
# Full WiFi recovery for nexmon firmware crashes on Pwnagotchi.
# Can be called manually or by the nexmon_stability plugin.
#
# Usage:
#   sudo pwnaui_wifi_recovery.sh [status|recover|monitor|diagnose]
#

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

INTERFACE="${PWNAUI_INTERFACE:-wlan0}"
MON_INTERFACE="${PWNAUI_MON_INTERFACE:-wlan0mon}"
LOG_FILE="/var/log/pwnaui_wifi_recovery.log"
STATE_FILE="/tmp/pwnaui_wifi_state"

log() {
    local msg="[$(date '+%Y-%m-%d %H:%M:%S')] $1"
    echo -e "${GREEN}[+]${NC} $1"
    echo "$msg" >> "$LOG_FILE"
}

warn() {
    local msg="[$(date '+%Y-%m-%d %H:%M:%S')] WARNING: $1"
    echo -e "${YELLOW}[!]${NC} $1"
    echo "$msg" >> "$LOG_FILE"
}

error() {
    local msg="[$(date '+%Y-%m-%d %H:%M:%S')] ERROR: $1"
    echo -e "${RED}[-]${NC} $1"
    echo "$msg" >> "$LOG_FILE"
}

info() {
    echo -e "${BLUE}[*]${NC} $1"
}

# Check if running as root
check_root() {
    if [[ $EUID -ne 0 ]]; then
        error "This script must be run as root"
        exit 1
    fi
}

# Get WiFi status
get_status() {
    echo ""
    echo -e "${BLUE}╔══════════════════════════════════════════════════════════╗${NC}"
    echo -e "${BLUE}║              PwnaUI WiFi Status                          ║${NC}"
    echo -e "${BLUE}╚══════════════════════════════════════════════════════════╝${NC}"
    echo ""
    
    # Check interfaces
    info "Interfaces:"
    if ip link show "$INTERFACE" &>/dev/null; then
        local state=$(ip link show "$INTERFACE" | grep -oP 'state \K\w+')
        echo "  $INTERFACE: $state"
    else
        echo "  $INTERFACE: NOT FOUND"
    fi
    
    if ip link show "$MON_INTERFACE" &>/dev/null; then
        local state=$(ip link show "$MON_INTERFACE" | grep -oP 'state \K\w+')
        local type=$(iw dev "$MON_INTERFACE" info 2>/dev/null | grep -oP 'type \K\w+' || echo "unknown")
        echo "  $MON_INTERFACE: $state ($type mode)"
    else
        echo "  $MON_INTERFACE: NOT FOUND"
    fi
    
    echo ""
    
    # Check driver
    info "Driver:"
    if lsmod | grep -q brcmfmac; then
        echo "  brcmfmac: LOADED"
        local fw_ver=$(cat /sys/module/brcmfmac/parameters/alternative_fw_path 2>/dev/null || echo "default")
        echo "  Firmware path: $fw_ver"
    else
        echo "  brcmfmac: NOT LOADED"
    fi
    
    echo ""
    
    # Check for errors
    info "Recent errors:"
    local errors=$(dmesg | tail -50 | grep -iE 'brcm|wlan|sdio|firmware' | grep -iE 'error|fail|down|timeout' | tail -5)
    if [[ -n "$errors" ]]; then
        echo "$errors" | while read line; do
            echo -e "  ${RED}$line${NC}"
        done
    else
        echo -e "  ${GREEN}No recent errors${NC}"
    fi
    
    echo ""
    
    # Check pwnagotchi/bettercap
    info "Services:"
    for svc in pwnagotchi bettercap; do
        if systemctl is-active --quiet "$svc" 2>/dev/null; then
            echo -e "  $svc: ${GREEN}RUNNING${NC}"
        else
            echo -e "  $svc: ${RED}STOPPED${NC}"
        fi
    done
    
    echo ""
}

# Full recovery procedure
do_recover() {
    log "Starting WiFi recovery..."
    
    # Stop services
    log "Stopping services..."
    systemctl stop pwnagotchi 2>/dev/null || true
    systemctl stop bettercap 2>/dev/null || true
    sleep 2
    
    # Kill any remaining processes using interface
    log "Killing processes using interface..."
    pkill -9 -f "wlan0" 2>/dev/null || true
    pkill -9 -f "bettercap" 2>/dev/null || true
    sleep 1
    
    # Remove monitor interface
    log "Removing monitor interface..."
    ip link set "$MON_INTERFACE" down 2>/dev/null || true
    iw dev "$MON_INTERFACE" del 2>/dev/null || true
    sleep 1
    
    # Unload driver
    log "Unloading brcmfmac driver..."
    rmmod brcmfmac 2>/dev/null || modprobe -r brcmfmac 2>/dev/null || true
    sleep 3
    
    # Clear any firmware state
    log "Clearing firmware state..."
    echo 1 > /sys/bus/sdio/devices/*/device/remove 2>/dev/null || true
    sleep 1
    echo 1 > /sys/bus/sdio/rescan 2>/dev/null || true
    sleep 2
    
    # Reload driver
    log "Reloading brcmfmac driver..."
    modprobe brcmfmac
    sleep 5
    
    # Wait for interface
    log "Waiting for interface..."
    for i in $(seq 1 15); do
        if ip link show "$INTERFACE" &>/dev/null; then
            log "Interface $INTERFACE appeared"
            break
        fi
        sleep 1
    done
    
    if ! ip link show "$INTERFACE" &>/dev/null; then
        error "Interface $INTERFACE did not appear after driver reload"
        return 1
    fi
    
    # Create monitor interface
    log "Creating monitor interface..."
    ip link set "$INTERFACE" down
    sleep 0.5
    
    iw phy phy0 interface add "$MON_INTERFACE" type monitor 2>/dev/null || \
        iw dev "$INTERFACE" interface add "$MON_INTERFACE" type monitor
    sleep 0.5
    
    ip link set "$MON_INTERFACE" up
    sleep 1
    
    # Verify monitor mode
    if iw dev "$MON_INTERFACE" info | grep -q "type monitor"; then
        log "Monitor interface $MON_INTERFACE ready"
    else
        error "Failed to create monitor interface"
        return 1
    fi
    
    # Start services
    log "Starting services..."
    systemctl start bettercap 2>/dev/null || true
    sleep 3
    systemctl start pwnagotchi 2>/dev/null || true
    
    log "Recovery complete!"
    
    # Save state
    echo "$(date +%s)" > "$STATE_FILE"
    
    return 0
}

# Setup monitor mode safely
setup_monitor() {
    log "Setting up monitor mode..."
    
    # Remove existing mon interface
    ip link set "$MON_INTERFACE" down 2>/dev/null || true
    iw dev "$MON_INTERFACE" del 2>/dev/null || true
    sleep 1
    
    # Bring down main interface
    ip link set "$INTERFACE" down
    sleep 0.5
    
    # Create monitor interface with airmon-ng style if available
    if command -v airmon-ng &>/dev/null; then
        airmon-ng start "$INTERFACE"
    else
        iw phy phy0 interface add "$MON_INTERFACE" type monitor
        ip link set "$MON_INTERFACE" up
    fi
    
    sleep 1
    
    # Verify
    if iw dev "$MON_INTERFACE" info | grep -q "type monitor"; then
        log "Monitor mode active on $MON_INTERFACE"
        return 0
    else
        error "Failed to setup monitor mode"
        return 1
    fi
}

# Run diagnostics
run_diagnose() {
    local diag_dir="/tmp/pwnaui_diagnostics_$(date +%Y%m%d_%H%M%S)"
    mkdir -p "$diag_dir"
    
    log "Collecting diagnostics to $diag_dir..."
    
    # System info
    uname -a > "$diag_dir/system.txt"
    cat /proc/device-tree/model >> "$diag_dir/system.txt" 2>/dev/null || true
    
    # Interface info
    ip link > "$diag_dir/interfaces.txt"
    iw dev > "$diag_dir/iw_dev.txt" 2>&1
    iw phy > "$diag_dir/iw_phy.txt" 2>&1
    
    # Driver info
    lsmod | grep -E 'brcm|cfg80211' > "$diag_dir/modules.txt"
    modinfo brcmfmac > "$diag_dir/brcmfmac_info.txt" 2>&1
    
    # Firmware info
    ls -la /lib/firmware/brcm/ > "$diag_dir/firmware_files.txt" 2>&1
    
    # dmesg
    dmesg | grep -iE 'brcm|wlan|wifi|sdio|firmware' > "$diag_dir/dmesg_wifi.txt"
    dmesg | tail -200 > "$diag_dir/dmesg_recent.txt"
    
    # Services
    systemctl status pwnagotchi > "$diag_dir/pwnagotchi_status.txt" 2>&1 || true
    systemctl status bettercap > "$diag_dir/bettercap_status.txt" 2>&1 || true
    
    # Config
    cp /etc/pwnagotchi/config.toml "$diag_dir/" 2>/dev/null || true
    
    # Recovery log
    cp "$LOG_FILE" "$diag_dir/" 2>/dev/null || true
    
    # Package
    local archive="/tmp/pwnaui_diagnostics_$(date +%Y%m%d_%H%M%S).tar.gz"
    tar -czf "$archive" -C /tmp "$(basename $diag_dir)"
    rm -rf "$diag_dir"
    
    log "Diagnostics saved to: $archive"
    echo "$archive"
}

# Main
case "${1:-status}" in
    status)
        get_status
        ;;
    recover|recovery|fix)
        check_root
        do_recover
        ;;
    monitor|mon)
        check_root
        setup_monitor
        ;;
    diagnose|diag|debug)
        check_root
        run_diagnose
        ;;
    *)
        echo "Usage: $0 {status|recover|monitor|diagnose}"
        echo ""
        echo "Commands:"
        echo "  status   - Show WiFi and service status"
        echo "  recover  - Full WiFi recovery procedure"
        echo "  monitor  - Setup monitor mode safely"
        echo "  diagnose - Collect diagnostic information"
        exit 1
        ;;
esac
