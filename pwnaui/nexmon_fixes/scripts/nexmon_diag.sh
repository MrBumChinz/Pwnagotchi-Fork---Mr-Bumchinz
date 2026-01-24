#!/bin/bash
#
# Nexmon Diagnostic Tool
#
# Collects diagnostic information about the WiFi subsystem
# for troubleshooting Nexmon-related issues.
#

echo "=============================================="
echo "   NEXMON DIAGNOSTIC REPORT"
echo "   $(date)"
echo "=============================================="
echo ""

# System Info
echo "=== System Information ==="
echo "Hostname: $(hostname)"
uname -a
echo ""

# Pi Model
echo "=== Raspberry Pi Model ==="
if [[ -f /proc/device-tree/model ]]; then
    cat /proc/device-tree/model
    echo ""
else
    echo "Not a Raspberry Pi or model info not available"
fi
echo ""

# WiFi Chip Detection
echo "=== WiFi Chip Information ==="
dmesg | grep -E "brcmfmac:.*BCM" | tail -5
echo ""

# Current Firmware Version
echo "=== Nexmon Firmware Version ==="
dmesg | grep -E "brcmfmac:.*version.*nexmon" | tail -1
echo ""

# Kernel Modules
echo "=== WiFi Kernel Modules ==="
lsmod | grep -E "brcm|cfg80211|mac80211" || echo "No WiFi modules loaded"
echo ""

# Network Interfaces
echo "=== Network Interfaces ==="
ip link show | grep -E "wlan|mon"
echo ""

# Interface Details
echo "=== Interface Details ==="
for iface in wlan0 wlan0mon mon0; do
    if ip link show "$iface" &>/dev/null; then
        echo "--- $iface ---"
        iw dev "$iface" info 2>/dev/null || echo "  No iw info available"
        echo ""
    fi
done

# Power Save Status
echo "=== Power Save Status ==="
for iface in wlan0 wlan0mon mon0; do
    if iw dev "$iface" get power_save 2>/dev/null; then
        echo "$iface: $(iw dev "$iface" get power_save 2>/dev/null)"
    fi
done
echo ""

# RF Kill Status
echo "=== RF Kill Status ==="
rfkill list wifi 2>/dev/null || echo "rfkill not available"
echo ""

# Recent WiFi Errors
echo "=== Recent WiFi Errors (last 5 minutes) ==="
journalctl -k --since "5 minutes ago" --no-pager 2>/dev/null | grep -E "brcmfmac|wifi|wlan" | tail -20
echo ""

# Channel Hop Errors
echo "=== Channel Hop Error Count ==="
error_count=$(dmesg | grep -c "Set Channel failed" 2>/dev/null || echo "0")
echo "Total channel hop errors: $error_count"
echo ""

# Recent Channel Errors
echo "=== Recent Channel Hop Errors ==="
dmesg | grep "Set Channel failed" | tail -5
echo ""

# Firmware Crashes
echo "=== Firmware Crash Count ==="
crash_count=$(dmesg | grep -c "Firmware has halted" 2>/dev/null || echo "0")
echo "Total firmware crashes: $crash_count"
echo ""

# Recent Firmware Crashes
echo "=== Recent Firmware Crashes ==="
dmesg | grep -E "Firmware has halted|brcmf_fw_crashed" | tail -5
echo ""

# Firmware Files
echo "=== Installed Firmware Files ==="
ls -la /lib/firmware/brcm/brcmfmac43* 2>/dev/null | head -10
echo ""

# Driver Files
echo "=== Driver Location ==="
modinfo brcmfmac 2>/dev/null | grep -E "filename|version" || echo "brcmfmac not loaded"
echo ""

# Pwnagotchi Status (if installed)
echo "=== Pwnagotchi Status ==="
if systemctl list-unit-files | grep -q pwnagotchi; then
    systemctl status pwnagotchi --no-pager 2>/dev/null | head -15
else
    echo "Pwnagotchi not installed"
fi
echo ""

# Bettercap Status (if installed)
echo "=== Bettercap Status ==="
if pgrep -x bettercap &>/dev/null; then
    echo "Bettercap is running (PID: $(pgrep -x bettercap))"
else
    echo "Bettercap is not running"
fi
echo ""

# Memory Usage
echo "=== Memory Usage ==="
free -h
echo ""

# CPU Temperature
echo "=== CPU Temperature ==="
if [[ -f /sys/class/thermal/thermal_zone0/temp ]]; then
    temp=$(cat /sys/class/thermal/thermal_zone0/temp)
    echo "CPU: $((temp/1000))°C"
else
    echo "Temperature reading not available"
fi
echo ""

# Uptime
echo "=== System Uptime ==="
uptime
echo ""

echo "=============================================="
echo "   END OF DIAGNOSTIC REPORT"
echo "=============================================="
