#!/usr/bin/env python3
"""
bcap_gps_init.py - Start bettercap GPS module once PTY is ready.

Runs at boot (after pwnaui starts) to:
1. Wait for /dev/ttyUSB0 (PTY created by pwnaui GPS plugin) to exist
2. Configure bettercap GPS module with correct baudrate
3. Start the GPS module
4. Watch for new handshake .pcap files and create .gps.json companion files

Install: Copy to /usr/local/bin/ and enable the service.
"""

import http.client
import base64
import json
import os
import sys
import time
import signal

HOST = '127.0.0.1'
PORT = 8081
USER = 'pwnagotchi'
PASS = 'pwnagotchi'
PTY_DEVICE = '/dev/ttyUSB0'
GPS_JSON = '/tmp/gps.json'
HANDSHAKE_DIR = '/home/pi/handshakes'
BAUDRATE = '19200'
CHECK_INTERVAL = 30  # seconds between handshake checks

auth = base64.b64encode(f'{USER}:{PASS}'.encode()).decode()
running = True


def signal_handler(sig, frame):
    global running
    running = False


def send_cmd(cmd):
    """Send a command to bettercap REST API."""
    try:
        conn = http.client.HTTPConnection(HOST, PORT, timeout=10)
        body = json.dumps({"cmd": cmd})
        headers = {
            'Content-Type': 'application/json',
            'Authorization': f'Basic {auth}'
        }
        conn.request('POST', '/api/session', body=body, headers=headers)
        resp = conn.getresponse()
        data = resp.read().decode()
        conn.close()
        return resp.status == 200, data
    except Exception as e:
        return False, str(e)


def get_gps_state():
    """Get GPS coordinates from bettercap session."""
    try:
        conn = http.client.HTTPConnection(HOST, PORT, timeout=10)
        headers = {'Authorization': f'Basic {auth}'}
        conn.request('GET', '/api/session', headers=headers)
        resp = conn.getresponse()
        data = json.loads(resp.read().decode())
        conn.close()
        return data.get('gps', {})
    except Exception:
        return {}


def read_gps_file():
    """Read current GPS data from /tmp/gps.json (written by bt_gps_receiver)."""
    try:
        with open(GPS_JSON, 'r') as f:
            data = json.load(f)
        # Check if data is recent (within 30 seconds)
        updated = data.get('Updated', 0)
        if time.time() - updated > 30:
            return None
        return data
    except (json.JSONDecodeError, FileNotFoundError, OSError):
        return None


def create_gps_companion(pcap_path, gps_data):
    """Create a .gps.json companion file for a handshake pcap."""
    gps_json_path = pcap_path.replace('.pcap', '.gps.json')
    if os.path.exists(gps_json_path):
        return False  # Already exists

    companion = {
        "Latitude": gps_data.get('Latitude', 0),
        "Longitude": gps_data.get('Longitude', 0),
        "Altitude": gps_data.get('Altitude', 0),
        "Accuracy": gps_data.get('Accuracy', 0),
        "Updated": time.strftime("%Y-%m-%dT%H:%M:%S%z")
    }

    try:
        with open(gps_json_path, 'w') as f:
            json.dump(companion, f, indent=2)
        print(f"[gps] Created companion: {os.path.basename(gps_json_path)}")
        return True
    except OSError as e:
        print(f"[gps] Error creating {gps_json_path}: {e}")
        return False


def tag_existing_handshakes(gps_data):
    """Tag any handshake pcap files that don't have GPS companion files."""
    if not os.path.isdir(HANDSHAKE_DIR):
        return

    for fname in os.listdir(HANDSHAKE_DIR):
        if fname.endswith('.pcap'):
            pcap_path = os.path.join(HANDSHAKE_DIR, fname)
            create_gps_companion(pcap_path, gps_data)


def wait_for_pty():
    """Wait for the PTY device to become available."""
    print(f"[gps] Waiting for {PTY_DEVICE}...")
    while running:
        if os.path.exists(PTY_DEVICE):
            # Verify it's readable
            try:
                with open(PTY_DEVICE, 'r') as f:
                    pass
                print(f"[gps] {PTY_DEVICE} is available")
                return True
            except OSError:
                pass
        time.sleep(5)
    return False


def wait_for_bettercap():
    """Wait for bettercap API to be available."""
    print("[gps] Waiting for bettercap API...")
    while running:
        try:
            conn = http.client.HTTPConnection(HOST, PORT, timeout=5)
            headers = {'Authorization': f'Basic {auth}'}
            conn.request('GET', '/api/session', headers=headers)
            resp = conn.getresponse()
            resp.read()
            conn.close()
            if resp.status in (200, 400):
                print("[gps] Bettercap API is available")
                return True
        except Exception:
            pass
        time.sleep(5)
    return False


def start_gps_module():
    """Configure and start the bettercap GPS module."""
    print("[gps] Configuring GPS module...")

    ok, _ = send_cmd("gps off")  # Stop if running

    time.sleep(1)

    ok, msg = send_cmd(f"set gps.device {PTY_DEVICE}")
    if ok:
        print(f"[gps] Set device: {PTY_DEVICE}")
    else:
        print(f"[gps] Failed to set device: {msg}")

    ok, msg = send_cmd(f"set gps.baudrate {BAUDRATE}")
    if ok:
        print(f"[gps] Set baudrate: {BAUDRATE}")
    else:
        print(f"[gps] Failed to set baudrate: {msg}")

    time.sleep(1)

    ok, msg = send_cmd("gps on")
    if ok:
        print("[gps] GPS module started")
    else:
        print(f"[gps] Failed to start GPS: {msg}")

    # Verify
    time.sleep(3)
    gps = get_gps_state()
    lat = gps.get('Latitude', 0)
    lon = gps.get('Longitude', 0)
    if lat != 0 or lon != 0:
        print(f"[gps] Coordinates: {lat:.6f}, {lon:.6f}")
        return True
    else:
        print("[gps] Warning: GPS coordinates still zero")
        return False


def monitor_handshakes():
    """Monitor for new handshake files and tag them with GPS."""
    print("[gps] Monitoring handshakes for GPS tagging...")
    known_files = set()

    # Initial scan
    if os.path.isdir(HANDSHAKE_DIR):
        for fname in os.listdir(HANDSHAKE_DIR):
            if fname.endswith('.pcap'):
                known_files.add(fname)

    while running:
        time.sleep(CHECK_INTERVAL)

        # Read current GPS
        gps_data = read_gps_file()
        if not gps_data:
            # Try bettercap GPS
            bcap_gps = get_gps_state()
            if bcap_gps.get('Latitude', 0) != 0:
                gps_data = {
                    'Latitude': bcap_gps['Latitude'],
                    'Longitude': bcap_gps['Longitude'],
                    'Altitude': bcap_gps.get('Altitude', 0),
                    'Accuracy': 0
                }

        if not gps_data:
            continue

        # Check for new pcap files
        if not os.path.isdir(HANDSHAKE_DIR):
            continue

        current_files = set()
        for fname in os.listdir(HANDSHAKE_DIR):
            if fname.endswith('.pcap'):
                current_files.add(fname)

        new_files = current_files - known_files
        for fname in new_files:
            pcap_path = os.path.join(HANDSHAKE_DIR, fname)
            create_gps_companion(pcap_path, gps_data)

        known_files = current_files

        # Also tag any untagged existing files
        for fname in current_files:
            pcap_path = os.path.join(HANDSHAKE_DIR, fname)
            gps_json = pcap_path.replace('.pcap', '.gps.json')
            if not os.path.exists(gps_json):
                create_gps_companion(pcap_path, gps_data)


def main():
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    print("=== Bettercap GPS Initializer ===")

    # Wait for dependencies
    if not wait_for_bettercap():
        return 1
    if not wait_for_pty():
        return 1

    # Start GPS module
    start_gps_module()

    # Tag existing handshakes with current GPS
    gps_data = read_gps_file()
    if gps_data:
        tag_existing_handshakes(gps_data)

    # Monitor for new handshakes
    monitor_handshakes()

    print("[gps] Shutting down")
    return 0


if __name__ == '__main__':
    sys.exit(main())
