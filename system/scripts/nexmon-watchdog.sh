#!/bin/bash
# nexmon-watchdog.sh — External watchdog for nexmon monitor mode
# Runs as a systemd service to detect and recover from wlan0mon failures.
# Complements pwnaui's built-in wifi_recovery.c for cases where pwnaui itself
# is down or the internal recovery can't execute.

IFACE="wlan0mon"
CHECK_INTERVAL=30
FAIL_THRESHOLD=3
COOLDOWN=120
MAX_RECOVERIES=5

fail_count=0
recovery_count=0
last_recovery=0

log() {
    echo "[nexmon-watchdog] $(date '+%Y-%m-%d %H:%M:%S') $1"
}

check_interface() {
    # Check if interface exists
    if [ ! -d "/sys/class/net/$IFACE" ]; then
        return 1
    fi
    
    # Check operstate (monitor mode shows "unknown" which is normal)
    local state
    state=$(cat "/sys/class/net/$IFACE/operstate" 2>/dev/null)
    if [ "$state" = "down" ]; then
        return 1
    fi
    
    # Check flags — need UP flag (0x1)
    local flags
    flags=$(cat "/sys/class/net/$IFACE/flags" 2>/dev/null)
    if [ -z "$flags" ]; then
        return 1
    fi
    local int_flags=$((flags))
    if (( (int_flags & 1) == 0 )); then
        return 1
    fi
    
    return 0
}

check_dmesg_errors() {
    # Check for critical brcmfmac errors in last 60 seconds of dmesg
    local errors
    errors=$(dmesg --time-format=reltime 2>/dev/null | tail -50 | grep -cE \
        'Firmware has halted|Failed to initialize|error.*SDIO|brcmf.*error.*-110')
    [ "$errors" -gt 0 ] 2>/dev/null
}

recover_interface() {
    local now
    now=$(date +%s)
    
    # Cooldown check
    if (( now - last_recovery < COOLDOWN )); then
        log "WARN: In cooldown period, skipping recovery"
        return 1
    fi
    
    recovery_count=$((recovery_count + 1))
    if (( recovery_count > MAX_RECOVERIES )); then
        log "ERROR: Max recoveries ($MAX_RECOVERIES) exceeded, rebooting"
        sync
        reboot
        exit 1
    fi
    
    last_recovery=$now
    log "Recovery attempt $recovery_count/$MAX_RECOVERIES"
    
    # Step 1: Try simple interface restart
    log "Step 1: Restarting $IFACE"
    ip link set "$IFACE" down 2>/dev/null
    sleep 1
    ip link set "$IFACE" up 2>/dev/null
    sleep 2
    
    if check_interface; then
        log "Interface recovered with simple restart"
        return 0
    fi
    
    # Step 2: Full driver reload
    log "Step 2: Reloading brcmfmac driver"
    
    # Stop bettercap first
    systemctl stop bettercap 2>/dev/null
    sleep 2
    
    # Remove and reload driver
    modprobe -r brcmfmac 2>/dev/null
    sleep 3
    modprobe brcmfmac 2>/dev/null
    sleep 5
    
    # Restart monitor mode
    if [ -x /usr/bin/monstart ]; then
        /usr/bin/monstart
    else
        ip link set wlan0 down 2>/dev/null
        iw dev wlan0 interface add wlan0mon type monitor 2>/dev/null
        ip link set wlan0mon up 2>/dev/null
    fi
    sleep 3
    
    # Restart bettercap
    systemctl start bettercap 2>/dev/null
    sleep 5
    
    # Restart pwnaui to rebind sockets
    systemctl restart pwnaui 2>/dev/null
    
    if check_interface; then
        log "Interface recovered with driver reload"
        return 0
    fi
    
    log "ERROR: Recovery failed"
    return 1
}

# Main loop
log "Starting nexmon watchdog (check every ${CHECK_INTERVAL}s, threshold: $FAIL_THRESHOLD)"

# Grace period at boot — give services time to initialize
sleep 60

while true; do
    if check_interface; then
        fail_count=0
    else
        fail_count=$((fail_count + 1))
        log "WARN: $IFACE check failed ($fail_count/$FAIL_THRESHOLD)"
        
        if (( fail_count >= FAIL_THRESHOLD )); then
            log "ERROR: $IFACE down for $((fail_count * CHECK_INTERVAL))s, initiating recovery"
            recover_interface
            fail_count=0
        fi
    fi
    
    # Also check for firmware crashes in dmesg
    if check_dmesg_errors; then
        log "WARN: brcmfmac firmware errors detected in dmesg"
        if check_interface; then
            log "Interface still up despite dmesg errors — monitoring"
        else
            recover_interface
            fail_count=0
        fi
    fi
    
    sleep "$CHECK_INTERVAL"
done
