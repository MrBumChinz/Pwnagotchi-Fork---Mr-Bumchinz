#!/bin/bash
set -e

REPO="MrBumChinz/Pwnagotchi-Fork---Mr-Bumchinz"
VERSION="${1:-latest}"
INSTALL_DIR="/home/pi/.pwn/lib/python3.11/site-packages"
BACKUP_DIR="/etc/pwnagotchi/backups"

if [ "$EUID" -ne 0 ]; then
    echo "Error: Please run as root (use sudo)"
    exit 1
fi

mkdir -p "$BACKUP_DIR"

CURRENT=$(cat "$INSTALL_DIR/pwnagotchi/_version.py" 2>/dev/null | grep -oP "'\K[^']+'" | tr -d "'" || echo "unknown")
echo "Current version: $CURRENT → Updating to: $VERSION"

# Backup
TIMESTAMP=$(date +%Y%m%d-%H%M%S)
tar -czf "$BACKUP_DIR/backup-$CURRENT-$TIMESTAMP.tar.gz" -C "$INSTALL_DIR" pwnagotchi 2>/dev/null

# Download
cd /tmp
rm -rf pwn-update
mkdir pwn-update
cd pwn-update

if [ "$VERSION" = "latest" ]; then
    URL=$(curl -s "https://api.github.com/repos/$REPO/releases/latest" | grep "tarball_url" | cut -d '"' -f 4)
else
    URL="https://github.com/$REPO/archive/refs/tags/$VERSION.tar.gz"
fi

wget -q --show-progress -O update.tar.gz "$URL"
tar -xzf update.tar.gz
cd */

# Stop services
systemctl stop pwnagotchi 2>/dev/null || true

# Install
cp -r pwnagotchi/* "$INSTALL_DIR/pwnagotchi/"
chown -R root:root "$INSTALL_DIR/pwnagotchi"

# Restart
systemctl start pwnagotchi

cd /
rm -rf /tmp/pwn-update

echo "✅ Update complete!"
