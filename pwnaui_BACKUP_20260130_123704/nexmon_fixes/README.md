# Nexmon Pwnagotchi Fixes

## Overview

This repository contains fixes, patches, and workarounds for the common issues encountered when using Nexmon with Pwnagotchi on Raspberry Pi devices.

## Known Issues

### 1. "Blindness Bug" (Channel Hop Failure)
**Error:** `brcmf_cfg80211_nexmon_set_channel: Set Channel failed: chspec=XXXX, -110`

**Description:** The WiFi chip becomes unresponsive to channel change commands. The firmware doesn't crash but stops responding to channel hopping requests, making the pwnagotchi effectively "blind."

**Affected Devices:**
- Raspberry Pi Zero W (BCM43430A1)
- Raspberry Pi 3B+ (BCM43455C0)
- Raspberry Pi 4 (BCM43455C0)
- Raspberry Pi Zero 2W (BCM43436B0)

**Root Cause:** The error code -110 indicates a timeout when communicating with the WiFi chip. This happens during rapid channel switching combined with frame injection.

### 2. Firmware Crash on Injection
**Error:** `brcmf_fw_crashed: Firmware has halted or crashed`

**Description:** The WiFi firmware completely crashes when injecting frames (especially deauth frames) at high rates. This is caused by a NULL pointer dereference in the `sendframe` function when the SCB (Station Control Block) structure is null during packet dequeue.

**Root Cause:** When injecting frames quickly, a race condition can occur where `pkt->scb` becomes null during packet dequeue, causing a crash when trying to access `scb->cfg->flags`.

### 3. Driver Reload Failure
**Error:** `brcmfmac: brcmf_sdio_readshared: invalid sdpcm_shared address`

**Description:** After a firmware crash, simply reloading the driver with `modprobe -r brcmfmac && modprobe brcmfmac` may not work. A full power cycle is often required.

## Chip/Firmware Mapping

| Device | WiFi Chip | Firmware Version |
|--------|-----------|------------------|
| Pi Zero W / Pi 3B | BCM43430A1 | 7_45_41_46 |
| Pi 3B+ / Pi 4 | BCM43455C0 | 7_45_206 / 7_45_234 |
| Pi Zero 2W | BCM43436B0 | 9_88_4_65 |
| Pi 5 | BCM43455C0 | 7_45_241 |

## Fixes Included

### 1. SCB Null Pointer Fix (`sendframe_fix.c`)
Patches the firmware to check for null SCB pointers before accessing them, preventing the most common crash scenario.

### 2. Injection Rate Limiter (`injection_throttle.c`)
Adds configurable rate limiting to frame injection to prevent overwhelming the firmware.

### 3. Channel Hop Recovery Script (`wifi_recovery.sh`)
A robust recovery script that monitors for the blindness bug and performs proper recovery.

### 4. Pwnagotchi Plugin (`nexmon_watchdog.py`)
Enhanced watchdog plugin that monitors for both blindness and firmware crashes, with proper recovery.

### 5. Safe Monitor Mode Script (`safe_monitor.sh`)
A script to safely enable monitor mode with proper delays to prevent immediate crashes.

## Installation

See [INSTALL.md](INSTALL.md) for detailed installation instructions.

## References

- [Nexmon GitHub](https://github.com/seemoo-lab/nexmon)
- [Nexmon Issue #335](https://github.com/seemoo-lab/nexmon/issues/335) - BCM43455c0 crash on injection
- [Nexmon Issue #595](https://github.com/seemoo-lab/nexmon/issues/595) - BCM43455C0 firmware crash
- [Pwnagotchi Issue #267](https://github.com/evilsocket/pwnagotchi/issues/267) - Blindness bug

## Contributing

PRs welcome! Please test on actual hardware before submitting.

## License

GPL-3.0 (matching Nexmon's license)
