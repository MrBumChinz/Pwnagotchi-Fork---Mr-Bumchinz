# Nexmon Pwnagotchi Fixes - Quick Reference

## 🚀 Quick Start

```bash
# On your Raspberry Pi:
cd nexmon_pwnagotchi_fixes
sudo ./deploy_fixes.sh
```

## 📁 Package Contents

```
nexmon_pwnagotchi_fixes/
├── deploy_fixes.sh           # One-click installer
├── README.md                 # This file
├── INSTALL.md                # Detailed installation guide
├── ANALYSIS.md               # Technical analysis of the bugs
│
├── patches/                  # Firmware patches
│   ├── sendframe_fix.c       # SCB null check (C code)
│   ├── sendframe_fix.patch   # Binary patch instructions
│   ├── nexmon_scb_null_fix.c # Complete nexmon patch file
│   └── injection_throttle.c  # Rate limiting patch
│
├── scripts/                  # Recovery & utility scripts
│   ├── wifi_recovery.sh      # Main recovery script
│   ├── safe_monitor.sh       # Safe monitor mode setup
│   ├── nexmon_diag.sh        # Diagnostics collector
│   └── stress_test.sh        # Firmware stability tests
│
├── plugins/                  # Pwnagotchi plugins
│   └── nexmon_watchdog.py    # Enhanced watchdog plugin
│
├── python/                   # Python modules
│   └── channel_hop_fix.py    # Safe channel hopping
│
├── config/                   # Configuration files
│   └── config_stable_nexmon.toml  # Stable config template
│
└── systemd/                  # System services
    └── nexmon-watchdog.service    # Monitoring service
```

## 🔧 Manual Installation

### 1. Apply Firmware Patches

```bash
# Copy patch to nexmon source
cp patches/nexmon_scb_null_fix.c /path/to/nexmon/patches/bcm43455c0/7_45_206/nexmon/src/

# Edit Makefile
cd /path/to/nexmon/patches/bcm43455c0/7_45_206/nexmon
echo "LOCAL_SRCS += src/nexmon_scb_null_fix.c" >> Makefile

# Build
source /path/to/nexmon/setup_env.sh
make clean && make && make install-firmware
```

### 2. Install Scripts

```bash
sudo cp scripts/*.sh /usr/local/bin/
sudo chmod +x /usr/local/bin/wifi_recovery.sh
sudo chmod +x /usr/local/bin/safe_monitor.sh
sudo chmod +x /usr/local/bin/nexmon_diag.sh
```

### 3. Install Pwnagotchi Plugin

```bash
sudo mkdir -p /etc/pwnagotchi/custom-plugins
sudo cp plugins/nexmon_watchdog.py /etc/pwnagotchi/custom-plugins/
```

Add to `/etc/pwnagotchi/config.toml`:
```toml
main.custom_plugins = "/etc/pwnagotchi/custom-plugins/"
main.plugins.nexmon_watchdog.enabled = true
main.plugins.nexmon_watchdog.blind_epochs = 10
```

### 4. Enable Service

```bash
sudo cp systemd/nexmon-watchdog.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable nexmon-watchdog
sudo systemctl start nexmon-watchdog
```

## 🛠️ Usage

### Check WiFi Status
```bash
sudo wifi_recovery.sh status
```

### Manual Recovery
```bash
sudo wifi_recovery.sh recover
```

### Setup Monitor Mode Safely
```bash
sudo safe_monitor.sh wlan0mon
```

### Run Diagnostics
```bash
sudo nexmon_diag.sh
# Creates report in /tmp/nexmon_diagnostics_*.tar.gz
```

### Stress Test (verify fixes)
```bash
sudo ./scripts/stress_test.sh wlan0 60
```

## ⚡ Issue Summary

| Issue | Symptom | Fix |
|-------|---------|-----|
| SCB NULL Crash | Firmware crash during injection | `nexmon_scb_null_fix.c` |
| Blindness Bug | -110 timeout, no APs seen | `wifi_recovery.sh`, watchdog |
| Channel Hop Timeout | Hang on channel change | `channel_hop_fix.py` |
| Bus Down | `brcmfmac: bus is down` | Full recovery script |

## 🔗 References

- [Nexmon Issue #335](https://github.com/seemoo-lab/nexmon/issues/335) - Firmware crash
- [Pwnagotchi Issue #267](https://github.com/evilsocket/pwnagotchi/issues/267) - Blindness
- [Nexmon Issues](https://github.com/seemoo-lab/nexmon/issues) - All issues

## ⚠️ Important Notes

1. **Backup** your working firmware before applying patches
2. Patches are chip and firmware version specific
3. The deploy script auto-detects your chip
4. Reboot required after firmware installation
5. Some fixes require rebuilding nexmon from source

## 🐛 Troubleshooting

### Firmware won't load
```bash
# Check dmesg for errors
dmesg | grep -i brcm

# Restore original firmware
sudo cp /lib/firmware/brcm/brcmfmac43455-sdio.bin.orig /lib/firmware/brcm/brcmfmac43455-sdio.bin
sudo reboot
```

### Interface keeps crashing
```bash
# Run diagnostics
sudo nexmon_diag.sh

# Check the recovery service
sudo systemctl status nexmon-watchdog
sudo journalctl -u nexmon-watchdog -f
```

### Build fails
```bash
# Clean and retry
cd nexmon
make clean
source setup_env.sh
cd patches/bcm43455c0/7_45_206/nexmon
make clean
make
```

## License

MIT - For educational and authorized testing purposes only.
