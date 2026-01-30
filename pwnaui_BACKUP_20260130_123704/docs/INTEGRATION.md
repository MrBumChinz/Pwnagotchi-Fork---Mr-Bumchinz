# PwnaUI Integration Guide

Complete guide to integrating the PwnaUI C daemon with Pwnagotchi.

---

## 1. Exact Modules to Rewrite

### Modules That Change

| Module | Current Role | After Integration |
|--------|-------------|-------------------|
| `pwnagotchi/ui/view.py` | UI rendering with PIL | Sends commands to C daemon |
| `pwnagotchi/ui/display.py` | Display abstraction | Uses PwnaUIView instead of View |
| `pwnagotchi/ui/components.py` | Widget classes (Text, Line, etc.) | **Unchanged** - state still stored in Python |
| `pwnagotchi/ui/faces.py` | Face string definitions | **Unchanged** - sent to daemon as strings |
| `pwnagotchi/ui/fonts.py` | PIL font loading | **Not used** - fonts are in C daemon |
| `pwnagotchi/ui/hw/*.py` | Hardware drivers | **Not used** - display handled by C daemon |

### What Moves to C

| Functionality | Python Before | C After |
|--------------|---------------|---------|
| Text rendering | PIL ImageDraw.text() | font.c + renderer.c |
| Line drawing | PIL ImageDraw.line() | renderer.c |
| Icon blitting | PIL Image.paste() | icons.c |
| Display init | waveshare/inky libs | display.c |
| Display update | hw driver render() | display.c |
| Framebuffer mgmt | PIL Image.new() | Static uint8_t array |
| Font loading | PIL ImageFont | Embedded bitmap fonts |

### What Stays in Python

| Functionality | Why |
|--------------|-----|
| UI state management | State/LabeledValue classes track values |
| Voice/text generation | Complex string formatting |
| Plugin hooks | Plugin API must stay in Python |
| Web UI rendering | Optional, still uses PIL for web dashboard |
| Event handlers | All on_*() methods stay, just call daemon |
| Configuration | Config parsing stays in Python |

---

## 2. Minimal C Architecture

```
pwnaui/
├── src/
│   ├── pwnaui.c       # Main daemon (init, signals, main loop)
│   ├── ipc.c/h        # UNIX socket server
│   ├── renderer.c/h   # Text, lines, layout composition
│   ├── display.c/h    # Hardware abstraction (SPI, GPIO)
│   ├── font.c/h       # Bitmap font system
│   └── icons.c/h      # Static icon bitmaps
├── python/
│   ├── pwnaui_client.py   # Python client library
│   └── pwnaui_view.py     # Drop-in View replacement
├── Makefile
├── pwnaui.service
└── README.md
```

### Module Interactions

```
┌─────────────────────────────────────────────────────────────┐
│                        pwnaui.c                             │
│  - main()                                                   │
│  - Signal handlers (SIGTERM, SIGHUP)                        │
│  - Main event loop (select on socket)                       │
│  - Command dispatcher (handle_command)                      │
│  - Global UI state (ui_state_t)                             │
└─────────────────────────────────────────────────────────────┘
          │                    │                    │
          ▼                    ▼                    ▼
    ┌─────────────┐    ┌──────────────┐    ┌──────────────┐
    │   ipc.c     │    │ renderer.c   │    │  display.c   │
    │             │    │              │    │              │
    │ Socket mgmt │    │ Draw text    │    │ SPI init     │
    │ Accept conn │    │ Draw lines   │    │ GPIO control │
    │ Read/write  │    │ Compose UI   │    │ Partial/full │
    └─────────────┘    └──────────────┘    │ refresh      │
                              │            └──────────────┘
                              │                    ▲
                       ┌──────┴──────┐             │
                       ▼             ▼             │
                 ┌─────────┐   ┌─────────┐        │
                 │ font.c  │   │ icons.c │        │
                 │         │   │         │        │
                 │ Glyphs  │   │ Bitmaps │────────┘
                 │ UTF-8   │   │ Blit    │
                 └─────────┘   └─────────┘
```

### Main Loop Flow

```c
while (g_running) {
    // 1. select() on server socket + client sockets
    activity = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
    
    // 2. Accept new connections
    if (FD_ISSET(server_fd, &read_fds)) {
        client_fd = ipc_server_accept(server_fd);
    }
    
    // 3. Handle client commands
    for (each client) {
        if (FD_ISSET(client_fd, &read_fds)) {
            read(client_fd, buffer, ...);
            handle_command(buffer, response, ...);
            write(client_fd, response, ...);
        }
    }
    
    // 4. Rate-limited display updates happen inside handle_command("UPDATE")
}
```

### Redraw Scheduling

1. **Dirty flag**: Set when any widget value changes
2. **Rate limiting**: Min 100ms between display updates
3. **Batching**: Multiple SET_* commands, one UPDATE
4. **Partial vs Full**: Partial by default, full on FULL_UPDATE

```c
if (g_dirty) {
    uint64_t now = get_time_ms();
    if (now - g_last_update_ms >= UPDATE_INTERVAL_MS) {
        renderer_render(&g_ui_state, g_framebuffer);
        display_update(g_framebuffer, partial);
        g_last_update_ms = now;
        g_dirty = 0;
    }
}
```

---

## 3. IPC Bridge to Python

### Protocol Format

```
Command: <CMD> [args...]\n
Response: OK|ERR [data]\n
```

### Command Reference

| Command | Args | Description |
|---------|------|-------------|
| CLEAR | - | Clear framebuffer to white |
| UPDATE | - | Partial refresh |
| FULL_UPDATE | - | Full e-ink refresh |
| SET_FACE | face_string | Set face emoticon |
| SET_STATUS | text | Set status message |
| SET_CHANNEL | value | Set channel display |
| SET_APS | value | Set APS count |
| SET_UPTIME | value | Set uptime string |
| SET_SHAKES | value | Set handshakes |
| SET_MODE | mode | Set AUTO/MANU/AI |
| SET_NAME | name | Set pwnagotchi name |
| SET_FRIEND | name | Set friend info |
| SET_INVERT | 0/1 | Color inversion |
| SET_LAYOUT | name | Set display layout |
| DRAW_TEXT | x y font text | Raw text draw |
| DRAW_LINE | x1 y1 x2 y2 | Raw line draw |
| DRAW_ICON | name x y | Draw icon |
| PING | - | Connection test |
| GET_STATE | - | Debug: get state |

### Python Client Usage

```python
from pwnaui_client import PwnaUIClient

# Context manager (recommended)
with PwnaUIClient() as ui:
    ui.set_face("(◕‿‿◕)")
    ui.set_status("Hello!")
    ui.update()

# Manual connection
ui = PwnaUIClient()
ui.connect()
ui.set_face("(◕‿‿◕)")
ui.update()
ui.disconnect()

# Module-level functions
import pwnaui_client as ui
ui.set_face("(◕‿‿◕)")
ui.update()
```

### Pwnagotchi Integration

**Option 1: Replace View class**

Edit `pwnagotchi/ui/display.py`:
```python
# from pwnagotchi.ui.view import View
from pwnaui_view import PwnaUIView as View
```

**Option 2: Modify view.py directly**

In `pwnagotchi/ui/view.py`, replace `update()` method:
```python
def update(self, force=False, new_data={}):
    # ... existing state management ...
    
    # Instead of PIL rendering:
    if self._pwnaui:
        self._send_to_daemon()
        self._pwnaui.update(full=force)
```

---

## 4. Display Driver Strategy

### Driver Selection

```c
int display_init(const char *type_str) {
    if (strcmp(type_str, "waveshare2in13_v2") == 0) {
        // Init SPI, GPIO, e-ink controller
        return epd_init_2in13_v2();
    }
    if (strcmp(type_str, "fb") == 0) {
        // Open /dev/fb0, mmap
        return fb_init();
    }
    // ... other drivers
}
```

### Hardware Support Matrix

| Display | Type | Resolution | Driver |
|---------|------|------------|--------|
| Waveshare 2.13" V2 | E-ink | 250×122 | SPI + GPIO |
| Waveshare 2.13" V3 | E-ink | 250×122 | SPI + GPIO |
| Waveshare 2.13" V4 | E-ink | 250×122 | SPI + GPIO |
| Waveshare 2.7" | E-ink | 264×176 | SPI + GPIO |
| Waveshare 1.54" | E-ink | 200×200 | SPI + GPIO |
| Inky pHAT | E-ink | 212×104 | SPI + GPIO |
| Framebuffer | LCD/HDMI | Variable | /dev/fb0 |
| Dummy | None | 250×122 | No-op |

### Partial Refresh Strategy

```c
void display_update(const uint8_t *framebuffer, int full_refresh) {
    if (full_refresh) {
        // Full refresh: Clear → Write → Refresh all
        epd_send_command(0x22);  // Display update control
        epd_send_data(0xF7);     // Full update LUT
        epd_send_command(0x20);  // Master activation
    } else {
        // Partial refresh: Write only changed regions
        epd_send_command(0x22);
        epd_send_data(0xFF);     // Partial update LUT
        epd_send_command(0x20);
    }
    epd_wait_busy();
}
```

### Dirty Region Tracking (Future)

```c
typedef struct {
    int x, y, w, h;
    int dirty;
} dirty_region_t;

// Only send changed pixels to display
void display_update_partial(const uint8_t *fb, dirty_region_t *region) {
    if (!region->dirty) return;
    
    epd_set_window(region->x, region->y, region->w, region->h);
    epd_write_region(fb, region);
    epd_partial_refresh();
    
    region->dirty = 0;
}
```

### Adding New Displays

1. Add enum to `display_type_t`
2. Add init function: `epd_init_newdisplay()`
3. Add display function: `epd_display_newdisplay()`
4. Add layout to `renderer.c` layouts array
5. Update `display_init()` parser

---

## 5. Performance Tuning Checklist

### Memory Optimization

- [x] **No malloc in hot paths** - All buffers statically allocated
- [x] **Static framebuffer** - `uint8_t g_framebuffer[MAX_SIZE]`
- [x] **Fixed-size strings** - `char face[64]`, `char status[256]`
- [x] **Embedded fonts** - No file I/O at runtime
- [x] **Embedded icons** - Compiled into binary

### CPU Optimization

- [x] **Lookup tables for fonts** - O(1) glyph access for ASCII
- [x] **Bresenham lines** - Integer-only line drawing
- [x] **Horizontal line optimization** - Byte-aligned fast path
- [x] **Batch draw before update** - Single display refresh per frame
- [x] **Rate limiting** - 100ms minimum between updates
- [x] **Single-threaded** - No locking overhead

### I/O Optimization

- [x] **SPI burst transfers** - Send entire framebuffer at once
- [x] **Direct GPIO access** - Memory-mapped, not sysfs
- [x] **Partial refresh default** - Only full refresh when needed
- [x] **Non-blocking socket** - select() based event loop

### String Parsing Optimization

- [x] **Fixed command format** - `CMD [args]\n`
- [x] **strcmp for command dispatch** - Simple and fast
- [x] **sscanf for numeric args** - Minimal parsing
- [x] **No regex** - All parsing is trivial

### Expected Performance

| Metric | Python/PIL | PwnaUI (C) | Notes |
|--------|------------|------------|-------|
| CPU per update | 10-30% | 1-3% | 10× improvement |
| CPU spikes | 40-50% | 5% | 8× improvement |
| Idle CPU | 1-2% | ~0% | Daemon sleeps on select() |
| Update latency | 50-200ms | 1-5ms | 40× faster |
| Memory (RSS) | ~30MB | <1MB | 30× smaller |
| Startup time | 2-3s | <100ms | 20× faster |

### Profiling Commands

```bash
# CPU usage
top -p $(pgrep pwnaui)

# Memory usage
pmap -x $(pgrep pwnaui)

# Trace system calls
strace -c -p $(pgrep pwnaui)

# SPI timing
gpio readall  # Check SPI pins
```

---

## 6. Build & Install

### Prerequisites

```bash
# On Raspberry Pi (Debian/Raspbian)
sudo apt-get install build-essential

# For cross-compilation on x86
sudo apt-get install gcc-arm-linux-gnueabihf
```

### Build

```bash
cd pwnaui

# Native build
make

# ARM-optimized build (on Pi)
make arm

# Debug build
make debug

# Cross-compile for ARM
make cross
```

### Install

```bash
sudo make install
sudo systemctl daemon-reload
sudo systemctl enable pwnaui
sudo systemctl start pwnaui
```

### Verify

```bash
# Check daemon is running
systemctl status pwnaui

# Test connection
python3 -c "from pwnaui_client import *; print(get_client().get_state())"

# Monitor logs
journalctl -u pwnaui -f
```

### Uninstall

```bash
sudo make uninstall
```

---

## 7. Troubleshooting

### Daemon won't start

```bash
# Check logs
journalctl -u pwnaui -n 50

# Run manually for debugging
sudo /usr/local/bin/pwnaui -v

# Check socket permissions
ls -la /var/run/pwnaui.sock
```

### Python can't connect

```bash
# Check socket exists
test -S /var/run/pwnaui.sock && echo "OK" || echo "Missing"

# Test with netcat
echo "PING" | nc -U /var/run/pwnaui.sock
```

### Display not updating

```bash
# Check SPI is enabled
ls /dev/spidev*

# Check GPIO permissions
groups  # Should include gpio

# Test display directly
/usr/local/bin/pwnaui -D waveshare2in13_v2 -v
```

### Wrong display type

Edit `/etc/systemd/system/pwnaui.service`:
```ini
ExecStart=/usr/local/bin/pwnaui -d -D waveshare2in13_v3
```

Then reload:
```bash
sudo systemctl daemon-reload
sudo systemctl restart pwnaui
```
