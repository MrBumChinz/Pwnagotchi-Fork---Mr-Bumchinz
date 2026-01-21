#!/bin/bash
set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

echo -e "${CYAN}╔════════════════════════════════════════╗${NC}"
echo -e "${CYAN}║  Pwnagotchi Fork Installation Script  ║${NC}"
echo -e "${CYAN}╚════════════════════════════════════════╝${NC}"

if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}Error: Please run as root (use sudo)${NC}"
    exit 1
fi

REPO="MrBumChinz/Pwnagotchi-Fork---Mr-Bumchinz"
INSTALL_DIR="/home/pi/.pwn/lib/python3.11/site-packages"
BACKUP_DIR="/etc/pwnagotchi/backups"
CONFIG_DIR="/etc/pwnagotchi"

mkdir -p "$BACKUP_DIR"
mkdir -p "$CONFIG_DIR"

# Color output function
log_info() { echo -e "${CYAN}[*]${NC} $1"; }
log_success() { echo -e "${GREEN}[✓]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[!]${NC} $1"; }
log_error() { echo -e "${RED}[✗]${NC} $1"; }

# Check if command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Detect architecture
ARCH=$(uname -m)
if [ "$ARCH" = "armv6l" ]; then
    ARCH_BUILD="armv6"
elif [ "$ARCH" = "armv7l" ]; then
    ARCH_BUILD="armv7"
elif [ "$ARCH" = "aarch64" ]; then
    ARCH_BUILD="aarch64"
else
    log_error "Unsupported architecture: $ARCH"
    exit 1
fi
log_success "Detected architecture: $ARCH_BUILD"

echo -e "${CYAN}[0/5]${NC} Installing dependencies..."

# Update package list
log_info "Updating package list..."
apt-get update -qq

# Core dependencies
DEPS="python3.11 python3.11-venv python3-pip git curl wget tar gzip build-essential libssl-dev libffi-dev python3.11-dev"

# Pwnagotchi-specific dependencies
DEPS="$DEPS libglib2.0-0 libatlas-base-dev libjasper-dev libharfbuzz0b libwebp6 libtiff5"

# Display drivers (Waveshare e-ink)
DEPS="$DEPS python3-pil python3-smbus i2c-tools"

# Wireless tools
DEPS="$DEPS aircrack-ng iw wireless-tools dnsmasq hostapd"

# Optional but recommended
DEPS="$DEPS screen git jq net-tools htop"

# Install missing dependencies
log_info "Checking and installing missing packages..."
for dep in $DEPS; do
    if ! dpkg -l | grep -q "^ii  $dep"; then
        log_warn "Installing $dep..."
        apt-get install -y -qq "$dep" 2>/dev/null || log_warn "Failed to install $dep (non-critical)"
    fi
done
log_success "Dependencies installed"

# Verify Python 3.11
if ! command_exists python3.11; then
    log_error "Python 3.11 installation failed!"
    exit 1
fi
log_success "Python 3.11 verified"

# Create Python virtualenv if needed
if [ ! -d "/home/pi/.pwn" ]; then
    log_info "Creating Python virtualenv at /home/pi/.pwn..."
    mkdir -p /home/pi/.pwn
    python3.11 -m venv /home/pi/.pwn
    chown -R pi:pi /home/pi/.pwn
fi

# Install Python packages
log_info "Installing Python dependencies..."
/home/pi/.pwn/bin/pip install -q --upgrade pip setuptools wheel 2>/dev/null || true
/home/pi/.pwn/bin/pip install -q pcapy scapy cryptography numpy crcmod 2>/dev/null || true
log_success "Python dependencies installed"

# Check for bettercap
if [ ! -f "/usr/local/bin/bettercap" ]; then
    log_warn "bettercap not found. Installing..."
    if ! curl -s https://www.bettercap.org/install | bash > /dev/null 2>&1; then
        log_warn "bettercap installation may have failed - continuing anyway"
    fi
fi

TIMESTAMP=$(date +%Y%m%d-%H%M%S)
if [ -d "$INSTALL_DIR/pwnagotchi" ]; then
    tar -czf "$BACKUP_DIR/backup-$TIMESTAMP.tar.gz" -C "$INSTALL_DIR" pwnagotchi 2>/dev/null || true
    log_success "Backup created: backup-$TIMESTAMP.tar.gz"
fi

echo -e "${CYAN}[2/5]${NC} Downloading latest release..."
cd /tmp
RELEASE_URL=$(curl -s "https://api.github.com/repos/$REPO/releases/latest" | grep "tarball_url" | cut -d '"' -f 4)
if [ -z "$RELEASE_URL" ]; then
    RELEASE_URL="https://github.com/$REPO/archive/refs/heads/main.tar.gz"
fi

log_info "Downloading from $RELEASE_URL"
wget -q --show-progress -O release.tar.gz "$RELEASE_URL"
tar -xzf release.tar.gz
cd */

echo -e "${CYAN}[3/5]${NC} Installing files..."
if [ -d "pwnagotchi" ]; then
    cp -r pwnagotchi "$INSTALL_DIR/"
    chown -R pi:pi "$INSTALL_DIR/pwnagotchi"
    log_success "Pwnagotchi installed to $INSTALL_DIR"
fi

# Create default config if missing
if [ ! -f "$CONFIG_DIR/config.toml" ]; then
    log_info "Creating default config.toml..."
    cat > "$CONFIG_DIR/config.toml" << 'CONFIGEOF'
[main]
name = "pwnagotchi"
version = "2.9.5.3"
debug = false

[ai]
enabled = false

[ui]
display = "waveshare2in13_V3"
color = "bw"
orientation = 0

[plugins]
CONFIGEOF
    chown pi:pi "$CONFIG_DIR/config.toml"
    log_success "Default config created at $CONFIG_DIR/config.toml"
fi

echo -e "${CYAN}[4/5]${NC} Setting up systemd services..."
systemctl daemon-reload 2>/dev/null || true
systemctl enable pwnagotchi 2>/dev/null || true
systemctl enable pwnaui 2>/dev/null || true

echo -e "${CYAN}[5/5]${NC} Restarting services..."
systemctl restart pwnagotchi 2>/dev/null || log_warn "pwnagotchi service restart (may not exist yet)"
systemctl restart pwnaui 2>/dev/null || log_warn "pwnaui service restart (may not exist yet)"
sleep 2

cd /
rm -rf /tmp/release.tar.gz /tmp/*/

log_success "Installation complete!"
log_success "Architecture: $ARCH_BUILD"
log_success "Config: $CONFIG_DIR/config.toml"
log_success "Install: $INSTALL_DIR"
log_success "Backups: $BACKUP_DIR"
echo ""
log_info "WebUI: http://$(hostname -I | awk '{print $1}'):8080"
log_info "SSH: ssh pi@$(hostname -I | awk '{print $1}')"
echo ""
log_warn "Next: Edit $CONFIG_DIR/config.toml and customize display driver"
