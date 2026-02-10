#!/bin/bash -e
#
# install-system.sh - Deploy all system runtime files to a Raspberry Pi
#
# This script installs all the runtime infrastructure files needed for
# the PwnaUI C pwnagotchi to function. Run as root on the target Pi
# or use in a pi-gen stage3 build.
#
# Usage:
#   sudo ./system/install-system.sh           # Install on a running Pi
#   sudo ./system/install-system.sh $ROOTFS   # Install into a pi-gen rootfs
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOTFS="${1:-}"

echo "=== PwnaUI System Files Installer ==="

if [ "$(id -u)" != "0" ]; then
    echo "ERROR: Must run as root"
    exit 1
fi

# /usr/bin/ scripts (sourced by other scripts, must be in PATH)
echo "### Installing /usr/bin/ scripts ###"
install -v -m 755 "${SCRIPT_DIR}/scripts/pwnlib"       "${ROOTFS}/usr/bin/pwnlib"
install -v -m 755 "${SCRIPT_DIR}/scripts/monstart"      "${ROOTFS}/usr/bin/monstart"
install -v -m 755 "${SCRIPT_DIR}/scripts/monstop"       "${ROOTFS}/usr/bin/monstop"
install -v -m 755 "${SCRIPT_DIR}/scripts/bettercap-launcher" "${ROOTFS}/usr/bin/bettercap-launcher"

# /usr/local/bin/ scripts
echo "### Installing /usr/local/bin/ scripts ###"
install -v -m 755 "${SCRIPT_DIR}/scripts/bcap_gps_init.py"      "${ROOTFS}/usr/local/bin/bcap_gps_init.py"
install -v -m 755 "${SCRIPT_DIR}/scripts/pwnaui-set-theme"      "${ROOTFS}/usr/local/bin/pwnaui-set-theme"
install -v -m 755 "${SCRIPT_DIR}/scripts/bt-auto-connect.sh"    "${ROOTFS}/usr/local/bin/bt-auto-connect.sh"
install -v -m 755 "${SCRIPT_DIR}/scripts/simple-bt-agent"       "${ROOTFS}/usr/local/bin/simple-bt-agent"
install -v -m 755 "${SCRIPT_DIR}/scripts/mode_toggle.py"        "${ROOTFS}/usr/local/bin/mode_toggle.py"
install -v -m 755 "${SCRIPT_DIR}/scripts/pisugar_mode_toggle.py" "${ROOTFS}/usr/local/bin/pisugar_mode_toggle.py"

# Bettercap caplets
echo "### Installing bettercap caplets ###"
install -v -d "${ROOTFS}/usr/local/share/bettercap/caplets"
install -v -m 644 "${SCRIPT_DIR}/caplets/pwnagotchi-auto.cap"   "${ROOTFS}/usr/local/share/bettercap/caplets/pwnagotchi-auto.cap"
install -v -m 644 "${SCRIPT_DIR}/caplets/pwnagotchi-manual.cap" "${ROOTFS}/usr/local/share/bettercap/caplets/pwnagotchi-manual.cap"

# Configuration
echo "### Installing configuration ###"
install -v -d "${ROOTFS}/etc/pwnagotchi"
install -v -d "${ROOTFS}/etc/pwnagotchi/log"
install -v -d "${ROOTFS}/etc/pwnagotchi/conf.d"
if [ ! -f "${ROOTFS}/etc/pwnagotchi/config.toml" ]; then
    install -v -m 644 "${SCRIPT_DIR}/config/config.toml" "${ROOTFS}/etc/pwnagotchi/config.toml"
else
    echo "  config.toml already exists, skipping (won't overwrite user config)"
fi

# Systemd services
echo "### Installing systemd services ###"
install -v -m 644 "${SCRIPT_DIR}/services/bettercap.service"       "${ROOTFS}/etc/systemd/system/bettercap.service"
install -v -m 644 "${SCRIPT_DIR}/services/bcap_gps_init.service"   "${ROOTFS}/etc/systemd/system/bcap_gps_init.service"
install -v -m 644 "${SCRIPT_DIR}/services/pwngrid-peer.service"    "${ROOTFS}/etc/systemd/system/pwngrid-peer.service"
install -v -m 644 "${SCRIPT_DIR}/services/bt-pairing.service"      "${ROOTFS}/etc/systemd/system/bt-pairing.service"
install -v -m 644 "${SCRIPT_DIR}/services/bt-tether-client.service" "${ROOTFS}/etc/systemd/system/bt-tether-client.service"
install -v -m 644 "${SCRIPT_DIR}/services/bt_gps_receiver.service" "${ROOTFS}/etc/systemd/system/bt_gps_receiver.service"
install -v -m 644 "${SCRIPT_DIR}/services/simple-bt-agent.service" "${ROOTFS}/etc/systemd/system/simple-bt-agent.service"
install -v -m 644 "${SCRIPT_DIR}/services/pwnaui-theme.service"    "${ROOTFS}/etc/systemd/system/pwnaui-theme.service"
install -v -m 644 "${SCRIPT_DIR}/services/pwnaui.service"          "${ROOTFS}/etc/systemd/system/pwnaui.service"
install -v -m 644 "${SCRIPT_DIR}/services/mode-toggle.service"     "${ROOTFS}/etc/systemd/system/mode-toggle.service"
install -v -m 644 "${SCRIPT_DIR}/services/pisugar-toggle.service"  "${ROOTFS}/etc/systemd/system/pisugar-toggle.service"

# Create required directories
echo "### Creating required directories ###"
install -v -d -m 777 "${ROOTFS}/home/pi/handshakes"
install -v -d "${ROOTFS}/root/custom_plugins"
install -v -d "${ROOTFS}/home/pi/wordlists"
install -v -d "${ROOTFS}/var/tmp/pwnagotchi"

# Enable services (only on a running system, not in chroot/rootfs builds)
if [ -z "${ROOTFS}" ]; then
    echo "### Enabling services ###"
    systemctl daemon-reload
    systemctl enable bettercap pwngrid-peer pwnaui bt-pairing bt-tether-client \
        bt_gps_receiver simple-bt-agent bcap_gps_init pwnaui-theme pisugar-toggle
    echo ""
    echo "Services enabled. Reboot to start, or manually:"
    echo "  systemctl start pwnaui bettercap pwngrid-peer"
fi

echo ""
echo "=== Installation complete ==="
echo ""
echo "File locations:"
echo "  Scripts:   /usr/bin/pwnlib, monstart, monstop, bettercap-launcher"
echo "  Scripts:   /usr/local/bin/bcap_gps_init.py, pwnaui-set-theme, ..."
echo "  Caplets:   /usr/local/share/bettercap/caplets/pwnagotchi-{auto,manual}.cap"
echo "  Config:    /etc/pwnagotchi/config.toml"
echo "  Services:  /etc/systemd/system/{bettercap,pwnaui,pwngrid-peer,...}.service"
echo ""
echo "NOTE: bt_gps_receiver binary must be compiled from source (pwnaui/src/)"
echo "NOTE: pwnaui binary must be compiled: cd pwnaui && make && sudo make install"
echo "NOTE: bettercap and pwngrid must be installed separately (Go packages)"
