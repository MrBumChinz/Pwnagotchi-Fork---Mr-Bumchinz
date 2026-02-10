# system/ — Runtime Infrastructure Files

All the non-C files needed for a working PwnaUI pwnagotchi system.
These get installed to the Pi's filesystem alongside the compiled `pwnaui` binary.

## Directory Layout

```
system/
├── install-system.sh          # Deploy all files to correct paths
├── scripts/                   # Executable scripts
│   ├── pwnlib                 # Bash function library (sourced by others)
│   ├── monstart               # Start monitor mode on wlan0
│   ├── monstop                # Stop monitor mode
│   ├── bettercap-launcher     # Start bettercap with correct caplet
│   ├── pwnaui-set-theme       # Send theme to pwnaui via unix socket
│   ├── bt-auto-connect.sh     # Bluetooth tethering auto-reconnect loop
│   ├── simple-bt-agent        # Keep BT discoverable/pairable
│   ├── bcap_gps_init.py       # GPS module init + handshake tagging
│   ├── mode_toggle.py         # PiSugar double-tap mode toggle (smbus2)
│   └── pisugar_mode_toggle.py # PiSugar tap toggle (raw I2C)
├── services/                  # Systemd unit files
│   ├── bettercap.service      # Bettercap daemon
│   ├── bcap_gps_init.service  # GPS init (after pwnaui + bt_gps_receiver)
│   ├── pwngrid-peer.service   # Pwngrid mesh peer
│   ├── bt-pairing.service     # BT pairing agent
│   ├── bt-tether-client.service  # BT tethering client
│   ├── bt_gps_receiver.service   # BT GPS receiver (compiled C binary)
│   ├── simple-bt-agent.service   # BT agent loop
│   ├── pwnaui-theme.service   # Theme init (oneshot, after pwnaui)
│   ├── pwnaui.service         # PwnaUI main daemon
│   ├── mode-toggle.service    # Mode toggle via smbus2 (disabled)
│   └── pisugar-toggle.service # Mode toggle via raw I2C
├── caplets/                   # Bettercap caplet files
│   ├── pwnagotchi-auto.cap    # Auto mode: wifi.recon + api.rest
│   └── pwnagotchi-manual.cap  # Manual mode: api.rest only
└── config/
    └── config.toml            # System config template (fill in your values)
```

## Installation Targets

| Source | Target Path | Permissions |
|--------|-------------|-------------|
| `scripts/pwnlib` | `/usr/bin/pwnlib` | 755 |
| `scripts/monstart` | `/usr/bin/monstart` | 755 |
| `scripts/monstop` | `/usr/bin/monstop` | 755 |
| `scripts/bettercap-launcher` | `/usr/bin/bettercap-launcher` | 755 |
| `scripts/bcap_gps_init.py` | `/usr/local/bin/bcap_gps_init.py` | 755 |
| `scripts/pwnaui-set-theme` | `/usr/local/bin/pwnaui-set-theme` | 755 |
| `scripts/bt-auto-connect.sh` | `/usr/local/bin/bt-auto-connect.sh` | 755 |
| `scripts/simple-bt-agent` | `/usr/local/bin/simple-bt-agent` | 755 |
| `scripts/mode_toggle.py` | `/usr/local/bin/mode_toggle.py` | 755 |
| `scripts/pisugar_mode_toggle.py` | `/usr/local/bin/pisugar_mode_toggle.py` | 755 |
| `caplets/*.cap` | `/usr/local/share/bettercap/caplets/` | 644 |
| `config/config.toml` | `/etc/pwnagotchi/config.toml` | 644 |
| `services/*.service` | `/etc/systemd/system/` | 644 |

## Service Boot Order

```
bluetooth.target
 ├─ bt-pairing.service
 ├─ bt-tether-client.service (After bt-pairing)
 ├─ bt_gps_receiver.service (5s delay)
 └─ simple-bt-agent.service

network-online.target
 └─ bettercap.service
     ├─ pwngrid-peer.service
     └─ pwnaui.service
         ├─ pwnaui-theme.service (oneshot)
         └─ bcap_gps_init.service (After pwnaui + bt_gps_receiver, 15s delay)

multi-user.target
 └─ pisugar-toggle.service
```

## Notes

- **bt_gps_receiver** is a compiled C binary (source in `pwnaui/src/`), not a script
- **config.toml** is a template — fill in your API keys, BT MAC, WiFi passwords before use
- **Python dependencies**: `bcap_gps_init.py` and `mode_toggle.py` need Python3 (already in Raspbian)
- **pwnaui-set-theme** uses `python3 -c "import toml"` — needs `pip3 install toml`
- **mode-toggle.service** is disabled by default (uses smbus2, alternative to pisugar-toggle)
- **pisugar-toggle.service** is the active mode toggle (raw I2C, no Python deps beyond stdlib)
