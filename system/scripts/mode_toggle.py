#!/usr/bin/env python3
import time, subprocess, os
from smbus2 import SMBus

PISUGAR_ADDR = 0x57
TAP_REG = 0x08
AUTO_FILE = '/root/.pwnagotchi-auto'
MANUAL_FILE = '/root/.pwnagotchi-manual'

def log(msg):
    print(msg, flush=True)

def get_current_mode():
    try:
        result = subprocess.run(['pgrep', '-af', 'pwnagotchi'], capture_output=True, text=True)
        if '--manual' in result.stdout:
            return 'MANUAL'
        return 'AUTO'
    except:
        return 'AUTO'

def toggle_mode():
    current = get_current_mode()
    new_mode = 'MANUAL' if current == 'AUTO' else 'AUTO'
    log(f'Switching: {current} -> {new_mode}')

    # Stop pwnagotchi first
    subprocess.run(['systemctl', 'stop', 'pwnagotchi'], capture_output=True)
    time.sleep(1)

    # Remove any existing files and create new one
    for f in [AUTO_FILE, MANUAL_FILE]:
        if os.path.exists(f):
            os.remove(f)

    if new_mode == 'AUTO':
        open(AUTO_FILE, 'w').close()
        log('Created AUTO file')
    else:
        open(MANUAL_FILE, 'w').close()
        log('Created MANUAL file')

    # Small delay to ensure file is written
    time.sleep(0.5)

    # Now start pwnagotchi - launcher will read the file
    subprocess.run(['systemctl', 'start', 'pwnagotchi'], capture_output=True)
    return new_mode

log('Mode Toggle Started - DOUBLE TAP to switch')
log('Current: ' + get_current_mode())
bus = SMBus(1)

while True:
    try:
        tap = bus.read_byte_data(PISUGAR_ADDR, TAP_REG) & 0x03
        if tap > 0:
            bus.write_byte_data(PISUGAR_ADDR, TAP_REG, bus.read_byte_data(PISUGAR_ADDR, TAP_REG) & 0xFC)
            if tap == 2:
                log('DOUBLE TAP!')
                new = toggle_mode()
                time.sleep(5)
                log('Now: ' + get_current_mode())
            else:
                log('Tap ' + str(tap) + ' ignored')
            time.sleep(0.5)
    except Exception as e:
        log('Err: ' + str(e))
        time.sleep(1)
    time.sleep(0.1)
