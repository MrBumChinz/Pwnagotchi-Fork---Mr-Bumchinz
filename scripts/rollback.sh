#!/bin/bash
set -e

BACKUP_DIR="/etc/pwnagotchi/backups"
INSTALL_DIR="/home/pi/.pwn/lib/python3.11/site-packages"

if [ "$EUID" -ne 0 ]; then
    echo "Error: Please run as root (use sudo)"
    exit 1
fi

if [ -z "$1" ]; then
    echo "Available backups:"
    ls -lht "$BACKUP_DIR"/*.tar.gz 2>/dev/null || echo "No backups found"
    echo ""
    echo "Usage: $0 latest"
    echo "       $0 /path/to/backup.tar.gz"
    exit 0
fi

BACKUP="$1"
if [ "$BACKUP" = "latest" ]; then
    BACKUP=$(ls -t "$BACKUP_DIR"/*.tar.gz 2>/dev/null | head -1)
    if [ -z "$BACKUP" ]; then
        echo "Error: No backups found"
        exit 1
    fi
fi

if [ ! -f "$BACKUP" ]; then
    echo "Error: Backup not found: $BACKUP"
    exit 1
fi

echo "Restoring from: $BACKUP"

systemctl stop pwnagotchi 2>/dev/null || true
rm -rf "$INSTALL_DIR/pwnagotchi"
tar -xzf "$BACKUP" -C "$INSTALL_DIR/"
systemctl start pwnagotchi

echo "✅ Rollback complete!"
