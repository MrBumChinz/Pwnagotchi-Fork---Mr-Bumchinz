#!/bin/bash
#
# PwnaUI Installation Script for Pwnagotchi
# Run this on your Raspberry Pi
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_PREFIX="/usr/local"
PYTHON_SITE="/usr/lib/python3/dist-packages"
SYSTEMD_DIR="/etc/systemd/system"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check if running as root
if [[ $EUID -ne 0 ]]; then
    log_error "This script must be run as root (use sudo)"
    exit 1
fi

# Check architecture
ARCH=$(uname -m)
log_info "Detected architecture: $ARCH"

# Detect display type from existing pwnagotchi config
DISPLAY_TYPE="waveshare2in13_v2"
if [[ -f /etc/pwnagotchi/config.toml ]]; then
    DETECTED=$(grep -oP 'type\s*=\s*"\K[^"]+' /etc/pwnagotchi/config.toml 2>/dev/null | head -1)
    if [[ -n "$DETECTED" ]]; then
        DISPLAY_TYPE="$DETECTED"
        log_info "Detected display type from config: $DISPLAY_TYPE"
    fi
fi

# Install build dependencies
log_info "Installing build dependencies..."
apt-get update
apt-get install -y build-essential

# Build the daemon
log_info "Building PwnaUI daemon..."
cd "$SCRIPT_DIR"

if [[ "$ARCH" == "armv6l" ]] || [[ "$ARCH" == "armv7l" ]]; then
    # ARM-optimized build
    make arm
else
    # Standard build
    make
fi

# Install files
log_info "Installing PwnaUI..."

# Daemon binary
install -d "$INSTALL_PREFIX/bin"
install -m 755 bin/pwnaui "$INSTALL_PREFIX/bin/pwnaui"

# Python client
install -d "$PYTHON_SITE"
install -m 644 python/pwnaui_client.py "$PYTHON_SITE/pwnaui_client.py"
install -m 644 python/pwnaui_view.py "$PYTHON_SITE/pwnaui_view.py"

# Nexmon channel fix module
if [[ -f "$SCRIPT_DIR/python/nexmon_channel.py" ]]; then
    log_info "Installing Nexmon channel fix module..."
    install -m 644 python/nexmon_channel.py "$PYTHON_SITE/nexmon_channel.py"
fi

# Pwnagotchi plugins
PLUGIN_DIR="/etc/pwnagotchi/custom-plugins"
install -d "$PLUGIN_DIR"

if [[ -f "$SCRIPT_DIR/plugins/pwnaui_themes.py" ]]; then
    log_info "Installing PwnaUI themes plugin..."
    install -m 644 plugins/pwnaui_themes.py "$PLUGIN_DIR/pwnaui_themes.py"
fi

if [[ -f "$SCRIPT_DIR/plugins/nexmon_stability.py" ]]; then
    log_info "Installing Nexmon stability plugin..."
    install -m 644 plugins/nexmon_stability.py "$PLUGIN_DIR/nexmon_stability.py"
fi

# Recovery scripts
if [[ -f "$SCRIPT_DIR/scripts/pwnaui_wifi_recovery.sh" ]]; then
    log_info "Installing WiFi recovery script..."
    install -m 755 scripts/pwnaui_wifi_recovery.sh "$INSTALL_PREFIX/bin/pwnaui_wifi_recovery"
fi

# Systemd service (update display type)
sed "s/waveshare2in13_v2/$DISPLAY_TYPE/g" pwnaui.service > /tmp/pwnaui.service
install -m 644 /tmp/pwnaui.service "$SYSTEMD_DIR/pwnaui.service"

# Enable SPI if not already enabled
if ! grep -q "^dtparam=spi=on" /boot/config.txt 2>/dev/null; then
    log_warn "SPI not enabled, adding to /boot/config.txt"
    echo "dtparam=spi=on" >> /boot/config.txt
    REBOOT_REQUIRED=1
fi

# Enable and start service
log_info "Enabling PwnaUI service..."
systemctl daemon-reload
systemctl enable pwnaui

# Start if not requiring reboot
if [[ -z "$REBOOT_REQUIRED" ]]; then
    systemctl start pwnaui
    sleep 2
    
    if systemctl is-active --quiet pwnaui; then
        log_info "PwnaUI daemon is running"
    else
        log_warn "PwnaUI daemon failed to start, check: journalctl -u pwnaui"
    fi
fi

# Patch pwnagotchi (optional)
log_info ""
log_info "====================================="
log_info "Installation complete!"
log_info "====================================="
log_info ""
log_info "To integrate with Pwnagotchi, edit:"
log_info "  /usr/local/lib/python3.x/dist-packages/pwnagotchi/ui/display.py"
log_info ""
log_info "Change:"
log_info "  from pwnagotchi.ui.view import View"
log_info "To:"
log_info "  from pwnaui_view import PwnaUIView as View"
log_info ""

if [[ -n "$REBOOT_REQUIRED" ]]; then
    log_warn "REBOOT REQUIRED to enable SPI!"
    log_warn "Run: sudo reboot"
fi

log_info "Test the daemon with:"
log_info "  python3 -c \"from pwnaui_client import *; print(get_client().get_state())\""

# Nexmon fixes info
log_info ""
log_info "====================================="
log_info "Nexmon Stability Fixes Installed"
log_info "====================================="
log_info ""
log_info "Add to /etc/pwnagotchi/config.toml:"
log_info ""
log_info '  main.custom_plugins = "/etc/pwnagotchi/custom-plugins/"'
log_info '  main.plugins.nexmon_stability.enabled = true'
log_info '  main.plugins.nexmon_stability.blind_epochs = 10'
log_info '  main.plugins.pwnaui_themes.enabled = true'
log_info ""
log_info "For manual WiFi recovery, run:"
log_info "  sudo pwnaui_wifi_recovery status"
log_info "  sudo pwnaui_wifi_recovery recover"
log_info ""
