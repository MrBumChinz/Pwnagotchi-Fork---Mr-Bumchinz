# Nexmon Pwnagotchi Fixes - Installation Guide

## Prerequisites

- Raspberry Pi (Zero W, 3B+, 4, or Zero 2W)
- Pwnagotchi installed (or clean Raspberry Pi OS)
- Root access
- Git installed

## Quick Start

### 1. Clone the Repositories

```bash
# Clone nexmon (if not already done)
cd ~
git clone https://github.com/seemoo-lab/nexmon.git

# Clone the fixes repository
git clone https://github.com/YOUR_REPO/nexmon_pwnagotchi_fixes.git
```

### 2. Install Dependencies

```bash
sudo apt update
sudo apt install -y raspberrypi-kernel-headers git libgmp3-dev gawk qpdf bison flex make autoconf libtool texinfo
```

### 3. Build Nexmon

```bash
cd ~/nexmon
source setup_env.sh
make
```

### 4. Apply the Firmware Patches

#### For Raspberry Pi 4 / 3B+ (BCM43455c0)

```bash
# Navigate to the correct firmware directory
cd ~/nexmon/patches/bcm43455c0/7_45_206/nexmon

# Copy the sendframe fix
cp ~/nexmon_pwnagotchi_fixes/patches/sendframe_fix.c src/

# Build the patched firmware
make clean
make
sudo make install-firmware
```

#### For Raspberry Pi Zero W / 3B (BCM43430a1)

```bash
cd ~/nexmon/patches/bcm43430a1/7_45_41_46/nexmon
# Apply patches similarly
make clean
make
sudo make install-firmware
```

#### For Raspberry Pi Zero 2W (BCM43436b0)

```bash
cd ~/nexmon/patches/bcm43436b0/9_88_4_65/nexmon
# Apply patches similarly
make clean
make
sudo make install-firmware
```

### 5. Install Scripts

```bash
# Copy recovery scripts
sudo cp ~/nexmon_pwnagotchi_fixes/scripts/wifi_recovery.sh /usr/local/bin/
sudo cp ~/nexmon_pwnagotchi_fixes/scripts/safe_monitor.sh /usr/local/bin/
sudo chmod +x /usr/local/bin/wifi_recovery.sh
sudo chmod +x /usr/local/bin/safe_monitor.sh
```

### 6. Install Pwnagotchi Plugin (if using Pwnagotchi)

```bash
# Create custom plugins directory if it doesn't exist
sudo mkdir -p /etc/pwnagotchi/custom-plugins

# Copy the plugin
sudo cp ~/nexmon_pwnagotchi_fixes/plugins/nexmon_watchdog.py /etc/pwnagotchi/custom-plugins/

# Edit your config.toml
sudo nano /etc/pwnagotchi/config.toml
```

Add the following to your config.toml:

```toml
main.custom_plugins = "/etc/pwnagotchi/custom-plugins/"
main.plugins.nexmon_watchdog.enabled = true
main.plugins.nexmon_watchdog.blind_epochs = 10
main.plugins.nexmon_watchdog.crash_recovery = true
main.plugins.nexmon_watchdog.soft_recovery_first = true
main.plugins.nexmon_watchdog.max_recovery_attempts = 3
main.plugins.nexmon_watchdog.reboot_on_failure = true
```

### 7. Setup Systemd Service for Continuous Monitoring

```bash
sudo tee /etc/systemd/system/nexmon-watchdog.service > /dev/null << 'EOF'
[Unit]
Description=Nexmon WiFi Watchdog
After=network.target

[Service]
Type=simple
ExecStart=/usr/local/bin/wifi_recovery.sh watch
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable nexmon-watchdog
sudo systemctl start nexmon-watchdog
```

## Usage

### Check WiFi Status

```bash
sudo wifi_recovery.sh status
```

### Manual Recovery

```bash
sudo wifi_recovery.sh recover
```

### Start Continuous Monitoring

```bash
sudo wifi_recovery.sh watch
```

### Safe Monitor Mode Setup

```bash
sudo safe_monitor.sh wlan0mon wlan0
```

## Configuration Options

### Pwnagotchi Plugin Options

| Option | Default | Description |
|--------|---------|-------------|
| `blind_epochs` | 10 | Number of failed epochs before recovery |
| `crash_recovery` | true | Automatically recover from firmware crashes |
| `soft_recovery_first` | true | Try driver reload before hard reset |
| `max_recovery_attempts` | 3 | Max recovery tries before reboot |
| `check_interval` | 30 | Seconds between error checks |
| `reboot_on_failure` | true | Reboot if all recovery fails |

### Environment Variables for Scripts

| Variable | Default | Description |
|----------|---------|-------------|
| `NEXMON_INTERFACE` | wlan0mon | Monitor interface name |
| `NEXMON_BASE_INTERFACE` | wlan0 | Base WiFi interface |

## Troubleshooting

### Firmware Immediately Crashes After Monitor Mode

This is common on Pi Zero 2W. Use the safe_monitor.sh script which includes critical delays:

```bash
sudo safe_monitor.sh
```

### Driver Won't Reload

Try a full SDIO reset:

```bash
sudo wifi_recovery.sh reset
```

### Nothing Works

Power cycle (unplug and replug) the Pi. Warm reboots don't always reset the WiFi chip properly.

### Channel Hop Errors Keep Happening

1. Check for heat issues - add heatsink/cooling
2. Reduce channel hopping frequency in bettercap config
3. Limit injection rate in config.toml:
   ```toml
   main.plugins.nexmon_watchdog.enabled = true
   ```

## Building from Source (Advanced)

If you need to modify the firmware patches:

1. Edit the source files in `~/nexmon/patches/bcmXXXX/VERSION/nexmon/src/`
2. Rebuild:
   ```bash
   cd ~/nexmon/patches/bcmXXXX/VERSION/nexmon
   make clean
   make
   sudo make install-firmware
   ```

### Understanding the Patches

- **sendframe_fix.c** - Prevents NULL pointer dereference during packet dequeue
- **injection_throttle.c** - Adds rate limiting to injection (optional)

## Known Limitations

1. **Cannot completely prevent crashes** - The firmware can still crash under extreme load
2. **Recovery requires driver reload** - This briefly disconnects all WiFi
3. **Some crashes need power cycle** - Soft recovery doesn't always work

## Contributing

Test on actual hardware and submit PRs with:
1. Device model (Pi 3B+, 4, Zero 2W, etc.)
2. Kernel version (`uname -a`)
3. Nexmon version
4. Steps to reproduce any issues

## License

GPL-3.0 (same as Nexmon)
