#!/bin/bash
#
# Safe Monitor Mode Setup Script
#
# This script sets up monitor mode with proper delays to prevent
# the immediate firmware crash that occurs on some Pi models.
#
# Critical: The delays between operations are ESSENTIAL.
# Removing them will likely cause firmware crashes.
#

set -e

INTERFACE="${1:-wlan0mon}"
BASE_INTERFACE="${2:-wlan0}"

echo "[*] Safe Monitor Mode Setup"
echo "[*] Base Interface: $BASE_INTERFACE"
echo "[*] Monitor Interface: $INTERFACE"

# Check if running as root
if [[ $EUID -ne 0 ]]; then
    echo "[!] This script must be run as root"
    exit 1
fi

# Cleanup existing monitor interface
if ip link show "$INTERFACE" &>/dev/null; then
    echo "[*] Removing existing monitor interface"
    ip link set "$INTERFACE" down 2>/dev/null || true
    iw dev "$INTERFACE" del 2>/dev/null || true
    sleep 2
fi

# Unblock all radios
echo "[*] Unblocking RF"
rfkill unblock all
sleep 1

# Bring up base interface
echo "[*] Bringing up $BASE_INTERFACE"
ip link set "$BASE_INTERFACE" up
sleep 3  # CRITICAL DELAY - prevents immediate crash on Pi Zero 2W

# Disable power save
echo "[*] Disabling power save"
iw dev "$BASE_INTERFACE" set power_save off 2>/dev/null || true
sleep 1

# Get phy name
PHY=$(iw dev "$BASE_INTERFACE" info | grep wiphy | awk '{print "phy"$2}')
echo "[*] Using phy: $PHY"

# Create monitor interface
echo "[*] Creating monitor interface"
iw phy "$PHY" interface add "$INTERFACE" type monitor
sleep 2  # CRITICAL DELAY

# Unblock again
rfkill unblock all
sleep 1

# Bring down base interface (optional but can help stability)
echo "[*] Bringing down $BASE_INTERFACE"
ip link set "$BASE_INTERFACE" down 2>/dev/null || true
sleep 1

# Bring up monitor interface
echo "[*] Bringing up $INTERFACE"
ip link set "$INTERFACE" up
sleep 1

# Disable power save on monitor interface
iw dev "$INTERFACE" set power_save off 2>/dev/null || true

# Verify
if iw dev "$INTERFACE" info | grep -q "type monitor"; then
    echo "[+] Monitor mode successfully enabled on $INTERFACE"
    echo ""
    iw dev "$INTERFACE" info
else
    echo "[-] Failed to enable monitor mode"
    exit 1
fi
