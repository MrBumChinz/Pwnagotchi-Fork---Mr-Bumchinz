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

mkdir -p "$BACKUP_DIR"

echo -e "${CYAN}[1/4]${NC} Creating backup..."
TIMESTAMP=$(date +%Y%m%d-%H%M%S)
if [ -d "$INSTALL_DIR/pwnagotchi" ]; then
    tar -czf "$BACKUP_DIR/backup-$TIMESTAMP.tar.gz" -C "$INSTALL_DIR" pwnagotchi 2>/dev/null || true
    echo -e "${GREEN}      Backup created${NC}"
fi

echo -e "${CYAN}[2/4]${NC} Downloading latest release..."
cd /tmp
RELEASE_URL=$(curl -s "https://api.github.com/repos/$REPO/releases/latest" | grep "tarball_url" | cut -d '"' -f 4)
if [ -z "$RELEASE_URL" ]; then
    RELEASE_URL="https://github.com/$REPO/archive/refs/heads/main.tar.gz"
fi

wget -q --show-progress -O release.tar.gz "$RELEASE_URL"
tar -xzf release.tar.gz
cd */

echo -e "${CYAN}[3/4]${NC} Installing files..."
if [ -d "pwnagotchi" ]; then
    cp -r pwnagotchi "$INSTALL_DIR/"
    chown -R root:root "$INSTALL_DIR/pwnagotchi"
fi

echo -e "${CYAN}[4/4]${NC} Restarting services..."
systemctl restart pwnagotchi 2>/dev/null || true
systemctl restart pwnaui 2>/dev/null || true

cd /
rm -rf /tmp/release.tar.gz /tmp/*/

echo -e "${GREEN}✅ Installation complete!${NC}"
echo -e "WebUI: ${CYAN}http://$(hostname -I | awk '{print $1}'):8080${NC}"
