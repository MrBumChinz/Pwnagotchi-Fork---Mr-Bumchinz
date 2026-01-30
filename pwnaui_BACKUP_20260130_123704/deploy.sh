#!/bin/bash
# PwnaUI Deployment Script for Pwnagotchi
# Run this on the Raspberry Pi after copying pwnaui folder

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PWNAGOTCHI_UI="/usr/local/lib/python3.*/dist-packages/pwnagotchi/ui"

echo "╔═══════════════════════════════════════════════════════════╗"
echo "║           PwnaUI Deployment for Pwnagotchi                ║"
echo "╚═══════════════════════════════════════════════════════════╝"
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "Please run as root: sudo ./deploy.sh"
    exit 1
fi

# Step 1: Install build dependencies
echo "[1/5] Installing build dependencies..."
apt-get update
apt-get install -y build-essential libc6-dev

# Step 2: Build pwnaui daemon
echo "[2/5] Building pwnaui daemon..."
cd "$SCRIPT_DIR"
make clean
make

# Step 3: Install daemon and systemd service
echo "[3/5] Installing pwnaui daemon..."
make install

# Step 4: Copy Python modules
echo "[4/5] Installing Python modules..."
cp "$SCRIPT_DIR/python/pwnaui_client.py" /usr/local/lib/python3.*/dist-packages/
cp "$SCRIPT_DIR/python/pwnaui_view.py" /usr/local/lib/python3.*/dist-packages/

# Step 5: Patch Pwnagotchi to use pwnaui
echo "[5/5] Patching Pwnagotchi..."
DISPLAY_PY=$(find /usr/local/lib -name "display.py" -path "*/pwnagotchi/ui/*" 2>/dev/null | head -1)

if [ -n "$DISPLAY_PY" ]; then
    # Backup original
    cp "$DISPLAY_PY" "$DISPLAY_PY.backup"
    
    # Replace the view import
    sed -i 's/from pwnagotchi.ui.view import View/from pwnaui_view import PwnaUIView as View/' "$DISPLAY_PY"
    
    echo "✓ Patched: $DISPLAY_PY"
else
    echo "⚠ Could not find display.py - manual patching required"
    echo "  Edit pwnagotchi/ui/display.py and change:"
    echo "    from pwnagotchi.ui.view import View"
    echo "  to:"
    echo "    from pwnaui_view import PwnaUIView as View"
fi

# Enable and start the daemon
echo ""
echo "Starting pwnaui daemon..."
systemctl daemon-reload
systemctl enable pwnaui
systemctl start pwnaui

echo ""
echo "╔═══════════════════════════════════════════════════════════╗"
echo "║           Deployment Complete!                            ║"
echo "╚═══════════════════════════════════════════════════════════╝"
echo ""
echo "PwnaUI daemon status:"
systemctl status pwnaui --no-pager || true
echo ""
echo "To test: sudo systemctl restart pwnagotchi"
echo "To revert: cp $DISPLAY_PY.backup $DISPLAY_PY"
echo ""
