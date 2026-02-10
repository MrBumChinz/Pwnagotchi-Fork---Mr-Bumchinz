#!/bin/bash
# wifi_watchdog.sh - Auto-detect and recover from BCM43430 nexmon firmware crashes
# Monitors dmesg for channel set failures and restarts wifi stack when detected
# Designed for Pwnagotchi on Raspberry Pi Zero W

LOGFILE="/var/log/wifi_watchdog.log"
CHECK_INTERVAL=30          # Check every 30 seconds
FAIL_THRESHOLD=10          # Number of recent channel failures to trigger recovery
COOLDOWN=120               # Minimum seconds between recovery attempts
LAST_RECOVERY=0

log() {
    echo "$(date '+%Y-%m-%d %H:%M:%S') $1" >> "$LOGFILE"
    logger -t wifi_watchdog "$1"
}

recover_wifi() {
    local NOW
    NOW=$(date +%s)
    local ELAPSED=$(( NOW - LAST_RECOVERY ))

    if [ "$ELAPSED" -lt "$COOLDOWN" ]; then
        log "SKIP: Recovery attempted ${ELAPSED}s ago (cooldown: ${COOLDOWN}s)"
        return
    fi

    LAST_RECOVERY=$NOW
    log "RECOVERY: WiFi firmware crash detected - initiating recovery..."

    # Stop bettercap gracefully
    log "RECOVERY: Stopping bettercap..."
    systemctl stop bettercap 2>/dev/null
    sleep 2

    # Kill any lingering bettercap processes
    killall -9 bettercap 2>/dev/null
    sleep 1

    # Remove monitor interface
    log "RECOVERY: Removing wlan0mon..."
    ip link set wlan0mon down 2>/dev/null
    iw dev wlan0mon del 2>/dev/null
    sleep 1

    # Reload the wifi driver to reset firmware
    log "RECOVERY: Reloading brcmfmac driver..."
    modprobe -r brcmfmac 2>/dev/null
    sleep 3
    modprobe brcmfmac 2>/dev/null
    sleep 5

    # Wait for wlan0 to come back
    local RETRIES=0
    while [ ! -d /sys/class/net/wlan0 ] && [ "$RETRIES" -lt 15 ]; do
        sleep 1
        RETRIES=$((RETRIES + 1))
    done

    if [ ! -d /sys/class/net/wlan0 ]; then
        log "RECOVERY FAILED: wlan0 did not come back after driver reload"
        log "RECOVERY: Attempting full reboot as last resort..."
        sync
        reboot
        return
    fi

    # Re-enable monitor mode
    log "RECOVERY: Re-enabling monitor mode..."
    /usr/bin/monstart 2>/dev/null
    sleep 3

    # Verify monitor mode
    MONITOR_STATUS=$(nexutil -m 2>/dev/null)
    if echo "$MONITOR_STATUS" | grep -q "monitor: [1-9]"; then
        log "RECOVERY: Monitor mode active: $MONITOR_STATUS"
    else
        log "RECOVERY WARNING: Monitor mode may not be active: $MONITOR_STATUS"
    fi

    # Restart bettercap
    log "RECOVERY: Starting bettercap..."
    systemctl start bettercap 2>/dev/null
    sleep 5

    # Restart pwnaui to reconnect to bettercap
    log "RECOVERY: Restarting pwnaui..."
    systemctl restart pwnaui 2>/dev/null
    sleep 3

    # Verify services
    BCAP_STATUS=$(systemctl is-active bettercap 2>/dev/null)
    PWNAUI_STATUS=$(systemctl is-active pwnaui 2>/dev/null)
    log "RECOVERY COMPLETE: bettercap=$BCAP_STATUS pwnaui=$PWNAUI_STATUS"

    # Clear kernel ring buffer of old errors so we don't re-trigger
    dmesg -C 2>/dev/null
    log "RECOVERY: dmesg cleared to prevent re-trigger"
}

check_wifi_health() {
    # Count recent channel set failures in dmesg
    local FAIL_COUNT
    FAIL_COUNT=$(dmesg | grep -c "Set Channel failed" 2>/dev/null)

    # Check if nexmon monitor mode is gone
    local MONITOR_MODE
    MONITOR_MODE=$(nexutil -m 2>/dev/null | grep -oP 'monitor: \K[0-9]+')

    # Check for SDIO bus timeouts (error -110)
    local TIMEOUT_COUNT
    TIMEOUT_COUNT=$(dmesg | grep -c "failed w/status -110" 2>/dev/null)

    # Check if MAC has zeroed out (firmware crash indicator)
    local WIFI_MAC
    WIFI_MAC=$(cat /sys/class/net/wlan0mon/address 2>/dev/null)

    if [ "$FAIL_COUNT" -ge "$FAIL_THRESHOLD" ]; then
        log "ALERT: $FAIL_COUNT channel failures detected (threshold: $FAIL_THRESHOLD)"
        recover_wifi
        return
    fi

    if [ "$MONITOR_MODE" = "0" ] && [ -d /sys/class/net/wlan0mon ]; then
        log "ALERT: Monitor mode reports 0 but wlan0mon exists - firmware may be hung"
        recover_wifi
        return
    fi

    if [ "$TIMEOUT_COUNT" -ge 5 ]; then
        log "ALERT: $TIMEOUT_COUNT SDIO bus timeouts detected"
        recover_wifi
        return
    fi

    # Everything OK - log occasionally (every 10 mins = ~20 checks)
    if [ $(( RANDOM % 20 )) -eq 0 ]; then
        log "OK: monitor=$MONITOR_MODE ch_fails=$FAIL_COUNT timeouts=$TIMEOUT_COUNT mac=$WIFI_MAC"
    fi
}

# --- Main ---
log "=== WiFi Watchdog started (interval=${CHECK_INTERVAL}s, threshold=${FAIL_THRESHOLD}) ==="

while true; do
    check_wifi_health
    sleep "$CHECK_INTERVAL"
done
