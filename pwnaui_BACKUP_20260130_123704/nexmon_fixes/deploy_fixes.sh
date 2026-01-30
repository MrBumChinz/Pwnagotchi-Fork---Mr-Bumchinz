#!/bin/bash
#
# Nexmon Pwnagotchi Fixes - One-Click Installer
#
# This script automatically applies all fixes for Nexmon stability issues
# on Raspberry Pi devices running Pwnagotchi.
#
# Supported devices:
#   - Raspberry Pi Zero W (BCM43430A1)
#   - Raspberry Pi 3B/3B+ (BCM43455C0)
#   - Raspberry Pi 4 (BCM43455C0)
#   - Raspberry Pi Zero 2W (BCM43436B0)
#
# Usage: sudo ./deploy_fixes.sh [--skip-build] [--no-reboot]
#

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NEXMON_DIR="${NEXMON_DIR:-$HOME/nexmon}"
SKIP_BUILD=false
NO_REBOOT=false

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --skip-build)
            SKIP_BUILD=true
            shift
            ;;
        --no-reboot)
            NO_REBOOT=true
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [--skip-build] [--no-reboot]"
            echo ""
            echo "Options:"
            echo "  --skip-build  Skip firmware rebuild (just install scripts/plugins)"
            echo "  --no-reboot   Don't reboot after installation"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Logging
log() {
    echo -e "${GREEN}[+]${NC} $1"
}

warn() {
    echo -e "${YELLOW}[!]${NC} $1"
}

error() {
    echo -e "${RED}[-]${NC} $1"
    exit 1
}

info() {
    echo -e "${BLUE}[*]${NC} $1"
}

# Check root
if [[ $EUID -ne 0 ]]; then
    error "This script must be run as root (use sudo)"
fi

# Banner
echo ""
echo -e "${GREEN}╔══════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║       Nexmon Pwnagotchi Fixes - Auto Installer           ║${NC}"
echo -e "${GREEN}║                                                          ║${NC}"
echo -e "${GREEN}║  Fixes: Blindness Bug, Firmware Crashes, Recovery        ║${NC}"
echo -e "${GREEN}╚══════════════════════════════════════════════════════════╝${NC}"
echo ""

# Detect Pi model and chip
detect_chip() {
    log "Detecting Raspberry Pi model and WiFi chip..."
    
    if [[ -f /proc/device-tree/model ]]; then
        PI_MODEL=$(cat /proc/device-tree/model | tr -d '\0')
        info "Detected: $PI_MODEL"
    else
        warn "Could not detect Pi model"
        PI_MODEL="unknown"
    fi
    
    # Detect chip from dmesg
    if dmesg | grep -q "BCM43430"; then
        CHIP="bcm43430a1"
        FW_VERSION="7_45_41_46"
        log "WiFi Chip: BCM43430A1 (Pi Zero W / Pi 3B)"
    elif dmesg | grep -q "BCM4345"; then
        CHIP="bcm43455c0"
        FW_VERSION="7_45_206"
        log "WiFi Chip: BCM43455C0 (Pi 3B+ / Pi 4)"
    elif dmesg | grep -q "BCM43436"; then
        CHIP="bcm43436b0"
        FW_VERSION="9_88_4_65"
        log "WiFi Chip: BCM43436B0 (Pi Zero 2W)"
    else
        warn "Could not auto-detect WiFi chip"
        echo ""
        echo "Please select your chip:"
        echo "  1) BCM43430A1 (Pi Zero W, Pi 3B)"
        echo "  2) BCM43455C0 (Pi 3B+, Pi 4)"
        echo "  3) BCM43436B0 (Pi Zero 2W)"
        read -p "Selection [1-3]: " selection
        
        case $selection in
            1)
                CHIP="bcm43430a1"
                FW_VERSION="7_45_41_46"
                ;;
            2)
                CHIP="bcm43455c0"
                FW_VERSION="7_45_206"
                ;;
            3)
                CHIP="bcm43436b0"
                FW_VERSION="9_88_4_65"
                ;;
            *)
                error "Invalid selection"
                ;;
        esac
    fi
    
    PATCH_DIR="$NEXMON_DIR/patches/$CHIP/$FW_VERSION/nexmon"
}

# Install dependencies
install_deps() {
    log "Installing dependencies..."
    apt-get update -qq
    apt-get install -y -qq raspberrypi-kernel-headers git libgmp3-dev gawk qpdf \
        bison flex make autoconf libtool texinfo > /dev/null 2>&1
    log "Dependencies installed"
}

# Clone/update nexmon
setup_nexmon() {
    if [[ -d "$NEXMON_DIR" ]]; then
        log "Nexmon directory exists, updating..."
        cd "$NEXMON_DIR"
        git pull --quiet || warn "Could not update nexmon"
    else
        log "Cloning nexmon repository..."
        git clone --quiet https://github.com/seemoo-lab/nexmon.git "$NEXMON_DIR"
    fi
    
    cd "$NEXMON_DIR"
    
    if [[ ! -f "$NEXMON_DIR/setup_env.sh" ]]; then
        error "Nexmon setup_env.sh not found"
    fi
}

# Apply firmware patches
apply_patches() {
    log "Applying firmware patches for $CHIP..."
    
    if [[ ! -d "$PATCH_DIR" ]]; then
        error "Patch directory not found: $PATCH_DIR"
    fi
    
    # Copy sendframe fix
    if [[ -f "$SCRIPT_DIR/patches/sendframe_fix.c" ]]; then
        cp "$SCRIPT_DIR/patches/sendframe_fix.c" "$PATCH_DIR/src/"
        log "Applied sendframe_fix.c"
    else
        warn "sendframe_fix.c not found, skipping"
    fi
    
    # Copy injection throttle (optional)
    if [[ -f "$SCRIPT_DIR/patches/injection_throttle.c" ]]; then
        cp "$SCRIPT_DIR/patches/injection_throttle.c" "$PATCH_DIR/src/"
        log "Applied injection_throttle.c"
    fi
}

# Build nexmon
build_nexmon() {
    if [[ "$SKIP_BUILD" == "true" ]]; then
        warn "Skipping firmware build (--skip-build)"
        return
    fi
    
    log "Building nexmon firmware (this may take a while)..."
    
    cd "$NEXMON_DIR"
    
    # Source environment
    source setup_env.sh
    
    # Build base tools
    log "Building nexmon tools..."
    make > /dev/null 2>&1 || error "Failed to build nexmon tools"
    
    # Build firmware
    cd "$PATCH_DIR"
    log "Building patched firmware..."
    make clean > /dev/null 2>&1
    make > /dev/null 2>&1 || error "Failed to build firmware"
    
    # Install firmware
    log "Installing patched firmware..."
    make install-firmware > /dev/null 2>&1 || error "Failed to install firmware"
    
    log "Firmware installed successfully!"
}

# Install scripts
install_scripts() {
    log "Installing recovery scripts..."
    
    # Copy scripts
    for script in wifi_recovery.sh safe_monitor.sh nexmon_diag.sh; do
        if [[ -f "$SCRIPT_DIR/scripts/$script" ]]; then
            cp "$SCRIPT_DIR/scripts/$script" /usr/local/bin/
            chmod +x "/usr/local/bin/$script"
            log "Installed $script"
        fi
    done
}

# Install pwnagotchi plugin
install_plugin() {
    log "Installing pwnagotchi plugin..."
    
    # Check if pwnagotchi is installed
    if [[ -d /etc/pwnagotchi ]]; then
        mkdir -p /etc/pwnagotchi/custom-plugins
        
        if [[ -f "$SCRIPT_DIR/plugins/nexmon_watchdog.py" ]]; then
            cp "$SCRIPT_DIR/plugins/nexmon_watchdog.py" /etc/pwnagotchi/custom-plugins/
            log "Installed nexmon_watchdog.py"
            
            # Check if config.toml exists and add plugin config
            if [[ -f /etc/pwnagotchi/config.toml ]]; then
                if ! grep -q "nexmon_watchdog" /etc/pwnagotchi/config.toml; then
                    echo "" >> /etc/pwnagotchi/config.toml
                    echo "# Nexmon Watchdog Plugin" >> /etc/pwnagotchi/config.toml
                    echo 'main.custom_plugins = "/etc/pwnagotchi/custom-plugins/"' >> /etc/pwnagotchi/config.toml
                    echo "main.plugins.nexmon_watchdog.enabled = true" >> /etc/pwnagotchi/config.toml
                    echo "main.plugins.nexmon_watchdog.blind_epochs = 10" >> /etc/pwnagotchi/config.toml
                    log "Added plugin configuration to config.toml"
                else
                    info "Plugin already configured in config.toml"
                fi
            fi
        fi
    else
        info "Pwnagotchi not detected, skipping plugin installation"
    fi
}

# Install systemd service
install_service() {
    log "Installing systemd service..."
    
    if [[ -f "$SCRIPT_DIR/systemd/nexmon-watchdog.service" ]]; then
        cp "$SCRIPT_DIR/systemd/nexmon-watchdog.service" /etc/systemd/system/
        systemctl daemon-reload
        systemctl enable nexmon-watchdog
        log "Installed and enabled nexmon-watchdog service"
    fi
}

# Python module
install_python() {
    log "Installing Python modules..."
    
    if [[ -f "$SCRIPT_DIR/python/channel_hop_fix.py" ]]; then
        mkdir -p /usr/local/lib/python3/dist-packages/nexmon_fixes
        cp "$SCRIPT_DIR/python/channel_hop_fix.py" /usr/local/lib/python3/dist-packages/nexmon_fixes/
        touch /usr/local/lib/python3/dist-packages/nexmon_fixes/__init__.py
        log "Installed channel_hop_fix module"
    fi
}

# Verify installation
verify() {
    log "Verifying installation..."
    
    local errors=0
    
    # Check scripts
    for script in wifi_recovery.sh safe_monitor.sh; do
        if [[ -x "/usr/local/bin/$script" ]]; then
            info "✓ $script installed"
        else
            warn "✗ $script missing"
            ((errors++))
        fi
    done
    
    # Check service
    if systemctl is-enabled nexmon-watchdog &>/dev/null; then
        info "✓ nexmon-watchdog service enabled"
    else
        warn "✗ nexmon-watchdog service not enabled"
    fi
    
    # Check firmware
    if [[ -f "/lib/firmware/brcm/brcmfmac43455-sdio.bin" ]] || \
       [[ -f "/lib/firmware/brcm/brcmfmac43430-sdio.bin" ]]; then
        info "✓ Firmware files present"
    else
        warn "✗ Firmware files may be missing"
    fi
    
    if [[ $errors -eq 0 ]]; then
        log "All checks passed!"
    else
        warn "Some components may need manual installation"
    fi
}

# Main installation
main() {
    detect_chip
    echo ""
    
    install_deps
    setup_nexmon
    apply_patches
    build_nexmon
    install_scripts
    install_plugin
    install_service
    install_python
    
    echo ""
    verify
    
    echo ""
    echo -e "${GREEN}╔══════════════════════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}║              Installation Complete!                      ║${NC}"
    echo -e "${GREEN}╚══════════════════════════════════════════════════════════╝${NC}"
    echo ""
    echo "Next steps:"
    echo "  1. Reboot your Pi to load the new firmware"
    echo "  2. Run 'sudo wifi_recovery.sh status' to check status"
    echo "  3. Run 'sudo safe_monitor.sh' to setup monitor mode"
    echo ""
    
    if [[ "$NO_REBOOT" != "true" ]]; then
        read -p "Reboot now? [y/N] " answer
        if [[ "$answer" =~ ^[Yy]$ ]]; then
            log "Rebooting..."
            reboot
        fi
    fi
}

# Run
main
