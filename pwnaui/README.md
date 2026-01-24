# PwnaUI - High-Performance C UI Renderer for Pwnagotchi

A complete C-based UI renderer that replaces Pwnagotchi's Python/PIL UI system with a high-performance native implementation, delivering 10-30× performance improvement on Raspberry Pi Zero hardware.

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                     Pwnagotchi Python Core                      │
│  (agent.py, plugins, bettercap integration, AI, mesh, etc.)     │
└─────────────────────────────────────────────────────────────────┘
                              │
                    IPC Commands via UNIX Socket
                    /var/run/pwnaui.sock
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                      PwnaUI C Daemon                            │
│  ┌───────────┐  ┌───────────┐  ┌───────────┐  ┌───────────┐    │
│  │   IPC     │  │ Renderer  │  │   Font    │  │  Icons    │    │
│  │  Server   │──│  Engine   │──│  Engine   │──│  Engine   │    │
│  └───────────┘  └───────────┘  └───────────┘  └───────────┘    │
│                       │                                         │
│              ┌────────┴────────┐                                │
│              │ Display Driver  │                                │
│              │   Abstraction   │                                │
│              └────────┬────────┘                                │
└───────────────────────┼─────────────────────────────────────────┘
                        │
        ┌───────────────┼───────────────┐
        │               │               │
        ▼               ▼               ▼
  ┌──────────┐   ┌──────────┐   ┌──────────┐
  │ Waveshare│   │   Inky   │   │Framebuffer│
  │  e-ink   │   │  e-ink   │   │  /dev/fb0 │
  └──────────┘   └──────────┘   └──────────┘
```

## IPC Protocol

Simple line-based protocol over UNIX domain socket at `/var/run/pwnaui.sock`:

| Command | Format | Description |
|---------|--------|-------------|
| `CLEAR` | `CLEAR` | Clear display buffer |
| `UPDATE` | `UPDATE` | Flush buffer to display |
| `DRAW_TEXT` | `DRAW_TEXT x y font_id text` | Draw text at position |
| `DRAW_ICON` | `DRAW_ICON name x y` | Draw named icon |
| `SET_FACE` | `SET_FACE face_string` | Set face widget |
| `SET_STATUS` | `SET_STATUS text` | Set status text |
| `SET_CHANNEL` | `SET_CHANNEL value` | Set channel display |
| `SET_APS` | `SET_APS value` | Set APS count |
| `SET_UPTIME` | `SET_UPTIME value` | Set uptime |
| `SET_SHAKES` | `SET_SHAKES value` | Set handshake count |
| `SET_MODE` | `SET_MODE mode` | Set mode (AUTO/MANU/AI) |
| `SET_NAME` | `SET_NAME name` | Set pwnagotchi name |
| `SET_FRIEND` | `SET_FRIEND name` | Set friend info |
| `DRAW_LINE` | `DRAW_LINE x1 y1 x2 y2` | Draw line |
| `FULL_UPDATE` | `FULL_UPDATE` | Force full e-ink refresh |
| `PARTIAL_UPDATE` | `PARTIAL_UPDATE` | Do partial refresh |

## File Structure

```
/usr/local/bin/pwnaui           - Main daemon binary
/usr/local/lib/pwnaui/          - Support files (fonts, icons)
/etc/systemd/system/pwnaui.service - Systemd unit
/var/run/pwnaui.sock            - UNIX socket (runtime)
/usr/lib/python3/dist-packages/pwnaui_client.py - Python client
```

## Performance Expectations

| Metric | Python/PIL | PwnaUI (C) | Improvement |
|--------|------------|------------|-------------|
| CPU per update | 10-30% | 1-3% | ~10× |
| CPU spikes | 40-50% | 5% | ~8× |
| Idle CPU | 1-2% | ~0% | ~10× |
| Memory | ~30MB | <1MB | ~30× |
| Update latency | 50-200ms | 1-5ms | ~40× |

## Building

```bash
make
sudo make install
sudo systemctl enable pwnaui
sudo systemctl start pwnaui
```

## Integration with Pwnagotchi

Replace the display rendering in Pwnagotchi by modifying view.py to use pwnaui_client instead of PIL. The Python side only sends commands; all rendering happens in C.

## License

MIT License - Same as Pwnagotchi
