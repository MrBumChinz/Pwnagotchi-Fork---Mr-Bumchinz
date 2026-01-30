"""
PwnaUI Integration for Pwnagotchi

This module provides a drop-in replacement for Pwnagotchi's View class
that uses the PwnaUI C daemon for rendering instead of Python/PIL.

Installation:
1. Build and install the PwnaUI daemon:
   cd pwnaui && make && sudo make install
   sudo systemctl enable pwnaui && sudo systemctl start pwnaui

2. Replace the import in pwnagotchi/ui/display.py:
   # Original:
   from pwnagotchi.ui.view import View
   # New:
   from pwnaui_view import PwnaUIView as View

Or for a more gradual migration, see the integration instructions below.
"""

import logging
import time
import threading
import os
import glob
from threading import Lock
from typing import Optional, Any, Dict, Callable, List
from enum import IntEnum

import pwnagotchi
import pwnagotchi.plugins as plugins
import pwnagotchi.ui.faces as faces
import pwnagotchi.ui.fonts as fonts
import pwnagotchi.ui.web as web
import pwnagotchi.utils as utils

from pwnagotchi.ui.components import *
from pwnagotchi.ui.state import State
from pwnagotchi.voice import Voice

# Import the PwnaUI client
try:
    from pwnaui_client import PwnaUIClient
    PWNAUI_AVAILABLE = True
except ImportError:
    PWNAUI_AVAILABLE = False
    logging.warning("pwnaui_client not found, falling back to PIL rendering")

# Colors (for compatibility)
WHITE = 0x00
BLACK = 0xFF


def count_cracked_passwords(handshakes_path: str) -> int:
    """
    Count the number of cracked passwords (files with .pcap.cracked extension).
    
    Args:
        handshakes_path: Path to the handshakes directory
        
    Returns:
        Number of cracked password files found
    """
    if not handshakes_path or not os.path.isdir(handshakes_path):
        return 0
    
    cracked_pattern = os.path.join(handshakes_path, "*.pcap.cracked")
    cracked_files = glob.glob(cracked_pattern)
    return len(cracked_files)


# =============================================================================
# DEBUG LOGGING SYSTEM - Delta-filtered with categories
# =============================================================================

class DebugLevel(IntEnum):
    """Log levels for filtering"""
    CRITICAL = 1
    ERROR = 2
    WARNING = 3
    INFO = 4
    VERBOSE = 5


class DebugCategory:
    """Debug categories for filtering by plugin/component"""
    CORE = 'core'           # Core UI elements (face, status, name)
    WIFI = 'wifi'           # WiFi-related (channel, aps)
    STATS = 'stats'         # Statistics (uptime, shakes, mode)
    BT_TETHER = 'bt_tether' # Bluetooth tethering plugin
    MEMTEMP = 'memtemp'     # Memory/temp plugin
    PISUGAR = 'pisugar'     # PiSugar battery plugin
    SESSION = 'session'     # Session stats plugin
    DAEMON = 'daemon'       # PwnaUI daemon communication
    STATE = 'state'         # State changes
    ALL = 'all'             # All categories


class PwnaUIDebugger:
    """
    Delta-filtered debug logger with category support.
    Only logs when values actually change, with configurable categories and levels.
    """
    
    # Configuration - modify these to enable/disable debugging
    ENABLED = True
    LOG_LEVEL = DebugLevel.INFO
    
    # Enable specific categories (empty set = all disabled, {'all'} = all enabled)
    ENABLED_CATEGORIES = {
        DebugCategory.BT_TETHER,
        DebugCategory.MEMTEMP,
        DebugCategory.DAEMON,
        # DebugCategory.CORE,
        # DebugCategory.WIFI,
        # DebugCategory.STATS,
        # DebugCategory.STATE,
    }
    
    # Log file path (None = use standard logging)
    LOG_FILE = '/tmp/pwnaui_debug.log'
    
    def __init__(self):
        self._prev_values: Dict[str, Any] = {}
        self._lock = Lock()
        self._log_file = None
        
        if self.ENABLED and self.LOG_FILE:
            try:
                self._log_file = open(self.LOG_FILE, 'a', buffering=1)
                self._write_log(DebugLevel.INFO, DebugCategory.DAEMON, 
                               "PwnaUI Debugger initialized", 
                               f"Level={self.LOG_LEVEL.name}, Categories={self.ENABLED_CATEGORIES}")
            except Exception as e:
                logging.warning(f"Failed to open debug log: {e}")
    
    def _is_enabled(self, level: DebugLevel, category: str) -> bool:
        """Check if logging is enabled for this level and category"""
        if not self.ENABLED:
            return False
        if level > self.LOG_LEVEL:
            return False
        if DebugCategory.ALL in self.ENABLED_CATEGORIES:
            return True
        return category in self.ENABLED_CATEGORIES
    
    def _write_log(self, level: DebugLevel, category: str, message: str, detail: str = None):
        """Write to log file or standard logging"""
        timestamp = time.strftime('%H:%M:%S')
        level_str = level.name[:4].ljust(4)
        cat_str = category.ljust(10)
        
        if detail:
            log_line = f"[{timestamp}] [{level_str}] [{cat_str}] {message}: {detail}"
        else:
            log_line = f"[{timestamp}] [{level_str}] [{cat_str}] {message}"
        
        if self._log_file:
            try:
                self._log_file.write(log_line + '\n')
            except:
                pass
        else:
            logging.debug(log_line)
    
    def log(self, level: DebugLevel, category: str, message: str, detail: str = None):
        """Log a message if enabled for this level/category"""
        if self._is_enabled(level, category):
            self._write_log(level, category, message, detail)
    
    def log_delta(self, category: str, key: str, new_value: Any, 
                  level: DebugLevel = DebugLevel.INFO) -> bool:
        """
        Log only if value changed from previous call.
        Returns True if value changed, False otherwise.
        """
        if not self._is_enabled(level, category):
            return False
            
        cache_key = f"{category}:{key}"
        
        with self._lock:
            prev = self._prev_values.get(cache_key)
            
            # Normalize for comparison
            new_str = str(new_value) if new_value is not None else '<None>'
            prev_str = str(prev) if prev is not None else '<None>'
            
            if new_str != prev_str:
                self._prev_values[cache_key] = new_value
                self._write_log(level, category, f"{key} CHANGED", 
                               f"'{prev_str}' -> '{new_str}'")
                return True
        return False
    
    def log_state_snapshot(self, state_dict: Dict[str, Any], category: str = DebugCategory.STATE):
        """Log all non-None values in a state dict (useful for periodic snapshots)"""
        if not self._is_enabled(DebugLevel.VERBOSE, category):
            return
        
        items = [f"{k}={v}" for k, v in state_dict.items() if v is not None]
        if items:
            self._write_log(DebugLevel.VERBOSE, category, "State snapshot", ', '.join(items))
    
    def critical(self, category: str, message: str, detail: str = None):
        self.log(DebugLevel.CRITICAL, category, message, detail)
    
    def error(self, category: str, message: str, detail: str = None):
        self.log(DebugLevel.ERROR, category, message, detail)
    
    def warning(self, category: str, message: str, detail: str = None):
        self.log(DebugLevel.WARNING, category, message, detail)
    
    def info(self, category: str, message: str, detail: str = None):
        self.log(DebugLevel.INFO, category, message, detail)
    
    def verbose(self, category: str, message: str, detail: str = None):
        self.log(DebugLevel.VERBOSE, category, message, detail)


# Global debugger instance
_debugger: Optional[PwnaUIDebugger] = None

def get_debugger() -> PwnaUIDebugger:
    """Get or create the global debugger instance"""
    global _debugger
    if _debugger is None:
        _debugger = PwnaUIDebugger()
    return _debugger


class DummyHardwareImpl:
    """
    Dummy hardware implementation for when no real display driver exists.
    PwnaUI uses the C daemon for rendering, so we don't need the actual driver.
    """
    def __init__(self, config):
        self.config = config
        display_config = config.get('ui', {}).get('display', {})
        self._type = display_config.get('type', 'waveshare2in13_v4')
        
        # Map display types to dimensions
        self._layouts = {
            'waveshare2in13_v4': {'width': 250, 'height': 122},
            'waveshare2in13_v3': {'width': 250, 'height': 122},
            'waveshare2in13_v2': {'width': 250, 'height': 122},
            'waveshare2in13': {'width': 250, 'height': 122},
            'waveshare2in7': {'width': 264, 'height': 176},
            'waveshare1in54': {'width': 200, 'height': 200},
            'waveshare_4': {'width': 250, 'height': 122},  # alias
            'waveshare_3': {'width': 250, 'height': 122},  # alias
            'waveshare_2': {'width': 250, 'height': 122},  # alias
            'inky': {'width': 212, 'height': 104},
            'oledhat': {'width': 128, 'height': 64},
            'lcdhat': {'width': 240, 'height': 240},
            'dfrobot': {'width': 250, 'height': 122},
            'spotpear24in': {'width': 296, 'height': 128},
            'displayhatmini': {'width': 320, 'height': 240},
            'dummydisplay': {'width': 250, 'height': 122},
        }
    
    @property
    def name(self):
        return self._type
    
    def layout(self):
        dims = self._layouts.get(self._type, {'width': 250, 'height': 122})
        return {
            'width': dims['width'],
            'height': dims['height'],
            'face': (0, 40),
            'name': (5, 20),
            'channel': (0, 0),
            'aps': (40, 0),
            'uptime': (185, 0),
            'line1': [0, 14, dims['width'], 14],
            'line2': [0, dims['height'] - 14, dims['width'], dims['height'] - 14],
            'friend_face': (0, dims['height'] - 28),
            'friend_name': (40, dims['height'] - 28),
            'shakes': (0, dims['height'] - 11),
            'mode': (dims['width'] - 40, dims['height'] - 11),
            'status': {
                'pos': (125, 20),
                'font': fonts.Medium,  # Use Medium font as default
                'max': 20
            }
        }
    
    def initialize(self):
        pass
    
    def render(self, canvas):
        pass
    
    def clear(self):
        pass


class PwnaUIView:
    """
    Drop-in replacement for Pwnagotchi's View class.
    
    Uses the PwnaUI C daemon for all rendering instead of PIL.
    Maintains API compatibility with the original View class.
    """
    
    def __init__(self, config, impl=None, state=None):
        global BLACK, WHITE
        
        # If no hardware impl provided, create a dummy one
        if impl is None:
            impl = DummyHardwareImpl(config)
            logging.info("PwnaUIView: Using dummy hardware impl (C daemon handles display)")
        
        # Initialize debugger early
        dbg = get_debugger()
        dbg.info(DebugCategory.DAEMON, "PwnaUIView initializing", f"display={impl.name}")
        
        self.invert = 0
        self._black = 0xFF
        self._white = 0x00
        
        if config['ui'].get('invert', False):
            logging.debug("INVERT BLACK/WHITES enabled")
            self.invert = 1
            BLACK = 0x00
            WHITE = 0xFF
            self._black = 0x00
            self._white = 0xFF
            
        # Setup faces from config
        faces.load_from_config(config['ui']['faces'])
        
        self._agent = None
        self._render_cbs: List[Callable] = []
        self._config = config
        self._frozen = False
        self._start_time = time.time()  # For live uptime counter
        self._lock = Lock()
        self._voice = Voice(lang=config['main']['lang'])
        self._implementation = impl
        self._layout = impl.layout()
        self._rotation = config['ui']['display'].get('rotation', 0)
        
        if (self._rotation / 90) % 2 == 0:
            self._width = self._layout['width']
            self._height = self._layout['height']
        else:
            self._width = self._layout['height']
            self._height = self._layout['width']
            
        # Initialize PwnaUI client
        self._pwnaui: Optional[PwnaUIClient] = None
        if PWNAUI_AVAILABLE:
            self._pwnaui = PwnaUIClient()
            if self._pwnaui.connect():
                logging.info("Connected to PwnaUI daemon")
                # Set layout based on display type
                self._pwnaui.set_layout(impl.name)
                self._pwnaui.set_invert(self.invert == 1)
            else:
                logging.warning("Failed to connect to PwnaUI daemon, will retry")
                
        # Initialize state (same as original View)
        self._state = State(state={
            'channel': LabeledValue(color=BLACK, label='CH', value='00',
                                    position=self._layout['channel'],
                                    label_font=fonts.Bold, text_font=fonts.Medium),
            'aps': LabeledValue(color=BLACK, label='APS', value='0 (00)',
                               position=self._layout['aps'],
                               label_font=fonts.Bold, text_font=fonts.Medium),
            'uptime': LabeledValue(color=BLACK, label='UP', value='00:00:00:00',
                                   position=self._layout['uptime'],
                                   label_font=fonts.Bold, text_font=fonts.Medium),
            'line1': Line(self._layout['line1'], color=BLACK),
            'line2': Line(self._layout['line2'], color=BLACK),
            'face': Text(value=faces.SLEEP,
                        position=(config['ui']['faces']['position_x'],
                                 config['ui']['faces']['position_y']),
                        color=BLACK, font=fonts.Huge,
                        png=config['ui']['faces']['png']),
            'friend_name': Text(value=None, position=self._layout['friend_face'],
                               font=fonts.BoldSmall, color=BLACK),
            'name': Text(value='%s>' % 'pwnagotchi', position=self._layout['name'],
                        color=BLACK, font=fonts.Bold),
            'status': Text(value=self._voice.default(),
                          position=self._layout['status']['pos'],
                          color=BLACK, font=self._layout['status']['font'],
                          wrap=True, max_length=self._layout['status']['max']),
            'shakes': LabeledValue(label='PWND ', value='0 (00)', color=BLACK,
                                   position=self._layout['shakes'],
                                   label_font=fonts.Bold, text_font=fonts.Medium),
            'mode': Text(value='AUTO', position=self._layout['mode'],
                        font=fonts.Bold, color=BLACK),
        })
        
        if state:
            for key, value in state.items():
                self._state.set(key, value)
                
        plugins.on('ui_setup', self)
        
        # Start refresh handler if fps > 0
        if config['ui']['fps'] > 0.0:
            threading.Thread(target=self._refresh_handler, args=(),
                           name="PwnaUI Handler", daemon=True).start()
            self._ignore_changes = ()
        else:
            logging.warning("ui.fps is 0, display will only update for major changes")
            self._ignore_changes = ('uptime', 'name')
            
    def set_agent(self, agent):
        self._agent = agent
        
    def has_element(self, key):
        return self._state.has_element(key)
        
    def add_element(self, key, elem):
        dbg = get_debugger()
        
        # Determine category from element key
        if key.startswith('bluetooth'):
            cat = DebugCategory.BT_TETHER
        elif key.startswith('memtemp'):
            cat = DebugCategory.MEMTEMP
        elif key.startswith('bat') or key.startswith('pisugar'):
            cat = DebugCategory.PISUGAR
        elif key.startswith('session'):
            cat = DebugCategory.SESSION
        else:
            cat = DebugCategory.STATE
            
        dbg.info(cat, f"Plugin adding UI element", f"key='{key}', type={type(elem).__name__}")
        
        if self.invert == 1 and elem.color:
            if elem.color == 0xff:
                elem.color = 0x00
            elif elem.color == 0x00:
                elem.color = 0xff
        self._state.add_element(key, elem)
        
    def remove_element(self, key):
        dbg = get_debugger()
        dbg.info(DebugCategory.STATE, f"Removing UI element", f"key='{key}'")
        self._state.remove_element(key)
        
    def width(self):
        return self._width
        
    def height(self):
        return self._height
        
    def on_state_change(self, key, cb):
        self._state.add_listener(key, cb)
        
    def on_render(self, cb):
        if cb not in self._render_cbs:
            self._render_cbs.append(cb)
            
    def _refresh_handler(self):
        """Background thread that updates the display at configured FPS"""
        delay = 1.0 / self._config['ui']['fps']
        while True:
            try:
                if self._config['ui'].get('cursor', True):
                    name = self._state.get('name')
                    self.set('name', name.rstrip('█').strip() if '█' in name else (name + ' █'))
                self.update()
            except Exception as e:
                logging.warning(f"Non-fatal error in UI refresh: {e}")
            time.sleep(delay)
            
    def set(self, key, value):
        dbg = get_debugger()
        
        # Determine category from key for plugin values
        if key == 'bluetooth':
            dbg.log_delta(DebugCategory.BT_TETHER, f"set({key})", value, DebugLevel.VERBOSE)
        elif key.startswith('memtemp'):
            dbg.log_delta(DebugCategory.MEMTEMP, f"set({key})", value, DebugLevel.VERBOSE)
        elif key in ('bat', 'pisugar'):
            dbg.log_delta(DebugCategory.PISUGAR, f"set({key})", value, DebugLevel.VERBOSE)
            
        self._state.set(key, value)
        
    def get(self, key):
        return self._state.get(key)
        
    def _send_to_daemon(self):
        """Send current UI state to the PwnaUI daemon"""
        dbg = get_debugger()
        
        if not self._pwnaui:
            dbg.warning(DebugCategory.DAEMON, "No PwnaUI client available")
            return
            
        # Ensure connected
        if not self._pwnaui.connected:
            dbg.info(DebugCategory.DAEMON, "Reconnecting to daemon...")
            self._pwnaui.connect()
            if not self._pwnaui.connected:
                dbg.error(DebugCategory.DAEMON, "Failed to connect to daemon")
                return
        
        # === CORE widgets ===
        face_val = self._state.get('face')
        status_val = self._state.get('status')
        name_val = self._state.get('name')
        
        dbg.log_delta(DebugCategory.CORE, 'face', face_val)
        dbg.log_delta(DebugCategory.CORE, 'status', status_val)
        dbg.log_delta(DebugCategory.CORE, 'name', name_val)
        
        self._pwnaui.set_face(face_val)
        self._pwnaui.set_status(status_val)
        self._pwnaui.set_name(name_val)
        
        # === WIFI widgets ===
        channel_val = self._state.get('channel')
        aps_val = self._state.get('aps')
        
        dbg.log_delta(DebugCategory.WIFI, 'channel', channel_val)
        dbg.log_delta(DebugCategory.WIFI, 'aps', aps_val)
        
        self._pwnaui.set_channel(channel_val)
        self._pwnaui.set_aps(aps_val)
        
        # === STATS widgets ===
        uptime_val = self._state.get('uptime')
        shakes_val = self._state.get('shakes')
        mode_val = self._state.get('mode') or 'AUTO'
        
        # Convert uptime from 'D: XX H: XX M: XX S: XX' to 'DD:HH:MM:SS' format for display
        if uptime_val:
            import re
            # Try to parse pwnagotchi format: 'D: 00 H: 00 M: 00 S: 00'
            match = re.match(r'D:\s*(\d+)\s*H:\s*(\d+)\s*M:\s*(\d+)\s*S:\s*(\d+)', uptime_val)
            if match:
                d, h, m, s = match.groups()
                uptime_display = f'{int(d):02d}:{int(h):02d}:{int(m):02d}:{int(s):02d}'
            elif ':' in uptime_val and len(uptime_val) <= 12:
                # Already in short format
                uptime_display = uptime_val
            else:
                uptime_display = uptime_val[:11]  # Truncate if too long
        else:
            uptime_display = '00:00:00:00'
        
        # Convert shakes to show cracked passwords (total handshakes)
        # Original format from pwnagotchi: 'session_handshakes (total_handshakes)'
        # Desired format: 'cracked_passwords (total_handshakes)'
        import re
        shakes_display = shakes_val
        if shakes_val:
            # Parse existing format: 'X (Y)' or 'X (Y) [last_pwnd]'
            match = re.match(r'(\d+)\s*\((\d+)\)(?:\s*\[.*\])?', str(shakes_val))
            if match:
                session_shakes, total_shakes = match.groups()
                # Count cracked passwords from handshakes directory
                handshakes_path = self._config.get('bettercap', {}).get('handshakes', '/root/handshakes')
                cracked = count_cracked_passwords(handshakes_path)
                shakes_display = f'{cracked} ({total_shakes})'
        
        # Convert mode to full text: AUTO -> "Auto Mode", MANU -> "Manual Mode"
        if mode_val == 'AUTO':
            mode_display = 'Auto Mode'
        elif mode_val == 'MANU':
            mode_display = 'Manual Mode'
        else:
            mode_display = mode_val  # Keep AI or other values as-is
        
        dbg.log_delta(DebugCategory.STATS, 'uptime', uptime_display)
        dbg.log_delta(DebugCategory.STATS, 'shakes', shakes_display)
        dbg.log_delta(DebugCategory.STATS, 'mode', mode_display)
        
        self._pwnaui.set_uptime(uptime_display)
        self._pwnaui.set_shakes(shakes_display)
        self._pwnaui.set_mode(mode_display)

        # === Friend ===
        friend = self._state.get('friend_name')
        dbg.log_delta(DebugCategory.CORE, 'friend_name', friend)
        if friend:
            self._pwnaui.set_friend(friend)
        else:
            self._pwnaui.set_friend("")
            
        # === PLUGIN WIDGETS ===
        # BT-Tether status - convert 'C'/'-' to 'BT+' / 'BT-'
        bt_val = self._state.get('bluetooth')
        if bt_val is not None:
            # Handle both raw status codes and already-formatted strings
            if bt_val == 'C' or bt_val.upper() == 'CONNECTED':
                bt_display = 'BT+'
            elif bt_val == '-' or bt_val == '' or bt_val.upper() == 'DISCONNECTED':
                bt_display = 'BT-'
            elif bt_val.startswith('BT'):
                # Already formatted (e.g., 'BT+', 'BT-', 'BT C')
                bt_display = bt_val
            else:
                bt_display = f'BT{bt_val}'  # Fallback: prepend BT to whatever value
            dbg.log_delta(DebugCategory.BT_TETHER, 'bluetooth', bt_display)
            self._pwnaui.send_command(f"SET_BLUETOOTH {bt_display}")
        
        # GPS status - convert 'C'/'-'/'S'/'NF' to proper display format
        gps_val = self._state.get('gps')
        if gps_val is not None:
            # Handle both raw status codes and already-formatted strings
            if gps_val == 'C' or gps_val.upper() == 'CONNECTED':
                gps_display = 'GPS+'
            elif gps_val == '-' or gps_val == '' or gps_val.upper() == 'DISCONNECTED':
                gps_display = 'GPS-'
            elif gps_val == 'S':
                gps_display = 'GPSS'  # GPS Saved
            elif gps_val == 'NF':
                gps_display = 'GPS?'  # GPS Not Found
            elif gps_val.startswith('GPS'):
                # Already formatted
                gps_display = gps_val
            else:
                gps_display = f'GPS{gps_val}'  # Fallback
            dbg.log_delta(DebugCategory.WIFI, 'gps', gps_display)
            self._pwnaui.send_command(f"SET_GPS {gps_display}")
            
        # Memtemp - check for both header and data elements
        memtemp_header = self._state.get('memtemp_header')
        memtemp_data = self._state.get('memtemp_data')
        if memtemp_header:
            dbg.log_delta(DebugCategory.MEMTEMP, 'memtemp_header', memtemp_header)
            self._pwnaui.send_command(f"SET_MEMTEMP_HEADER {memtemp_header}")
        if memtemp_data:
            dbg.log_delta(DebugCategory.MEMTEMP, 'memtemp_data', memtemp_data)
            self._pwnaui.send_command(f"SET_MEMTEMP_DATA {memtemp_data}")
            
        # Battery - format as "BAT-100%" (label-value with single dash, no spaces)
        bat_obj = self._state.get('bat') or self._state.get('battery')
        if bat_obj:
            # HARDCODE TEST - remove spaces completely
            self._pwnaui.send_command("SET_BATTERY BAT100%")
            dbg.log_delta(DebugCategory.PISUGAR, 'battery', "BAT100%")
            
    def update(self, force=False, new_data={}):
        """Update the display"""
        dbg = get_debugger()
        
        for key, val in new_data.items():
            self.set(key, val)
        
        # Live uptime counter - calculate from start time
        elapsed = int(time.time() - self._start_time)
        days, remainder = divmod(elapsed, 86400)
        hours, remainder = divmod(remainder, 3600)
        mins, secs = divmod(remainder, 60)
        self.set('uptime', 'D: %02d H: %02d M: %02d S: %02d' % (days, hours, mins, secs))
            
        with self._lock:
            if self._frozen:
                dbg.verbose(DebugCategory.DAEMON, "Update skipped - display frozen")
                return
                
            state = self._state
            changes = state.changes(ignore=self._ignore_changes)
            
            if force or len(changes):
                if changes:
                    dbg.verbose(DebugCategory.STATE, f"State changes detected", 
                               f"count={len(changes)}, keys={list(changes)[:5]}")
                
                # Send state to PwnaUI daemon
                self._send_to_daemon()
                
                # Trigger update
                if self._pwnaui and self._pwnaui.connected:
                    self._pwnaui.update(full=force)
                    dbg.verbose(DebugCategory.DAEMON, "Display update sent", 
                               f"force={force}")
                    
                # Still call plugins and web update for compatibility
                plugins.on('ui_update', self)
                
                # For web UI, we still need to render with PIL
                # This is optional and can be disabled for pure daemon mode
                if self._config.get('ui', {}).get('web_ui_enabled', True):
                    self._render_for_web()
                    
                for cb in self._render_cbs:
                    cb(None)  # Canvas is None in daemon mode
                    
                self._state.reset()
                
    def _render_for_web(self):
        """Render UI for web interface (optional, uses PIL)"""
        try:
            from PIL import Image, ImageDraw
            canvas = Image.new('1', (self._width, self._height), self._white)
            drawer = ImageDraw.Draw(canvas)
            
            for key, lv in self._state.items():
                lv.draw(canvas, drawer)
                
            web.update_frame(canvas)
        except Exception as e:
            logging.debug(f"Web UI render skipped: {e}")
            
    # === Event handlers (same as original View) ===
    
    def on_starting(self):
        self.set('status', self._voice.on_starting() + f"\n(v{pwnagotchi.__version__})")
        self.set('face', faces.AWAKE)
        self.update()
        
    def on_manual_mode(self, last_session):
        self.set('mode', 'MANU')
        self.set('face', faces.SAD if (last_session.epochs > 3 and last_session.handshakes == 0) else faces.HAPPY)
        self.set('status', self._voice.on_last_session_data(last_session))
        self.set('epoch', "%04d" % last_session.epochs)
        self.set('uptime', last_session.duration)
        self.set('channel', '-')
        self.set('aps', "%d" % last_session.associated)
        self.set('shakes', '%d (%s)' % (last_session.handshakes,
                 utils.total_unique_handshakes(self._config['bettercap']['handshakes'])))
        self.set_closest_peer(last_session.last_peer, last_session.peers)
        self.update()
        
    def is_normal(self):
        return self._state.get('face') not in (
            faces.INTENSE, faces.COOL, faces.BORED, faces.HAPPY,
            faces.EXCITED, faces.MOTIVATED, faces.DEMOTIVATED,
            faces.SMART, faces.SAD, faces.LONELY)
            
    def on_keys_generation(self):
        self.set('face', faces.AWAKE)
        self.set('status', self._voice.on_keys_generation())
        self.update()
        
    def on_normal(self):
        self.set('face', faces.AWAKE)
        self.set('status', self._voice.on_normal())
        self.update()
        
    def set_closest_peer(self, peer, num_total):
        if peer is None:
            self.set('friend_face', None)
            self.set('friend_name', None)
        else:
            if peer.rssi >= -67:
                num_bars = 4
            elif peer.rssi >= -70:
                num_bars = 3
            elif peer.rssi >= -80:
                num_bars = 2
            else:
                num_bars = 1
                
            name = '▌' * num_bars + '│' * (4 - num_bars)
            name += f' {peer.name()} {peer.pwnd_run()} ({peer.pwnd_total()})'
            
            if num_total > 1:
                if num_total > 9000:
                    name += ' of over 9000'
                else:
                    name += f' of {num_total}'
                    
            self.set('friend_face', peer.face())
            self.set('friend_name', name)
        self.update()
        
    def on_new_peer(self, peer):
        import random
        if peer.first_encounter():
            face = random.choice((faces.AWAKE, faces.COOL))
        elif peer.is_good_friend(self._config):
            face = random.choice((faces.MOTIVATED, faces.FRIEND, faces.HAPPY))
        else:
            face = random.choice((faces.EXCITED, faces.HAPPY, faces.SMART))
            
        self.set('face', face)
        self.set('status', self._voice.on_new_peer(peer))
        self.update()
        time.sleep(3)
        
    def on_lost_peer(self, peer):
        self.set('face', faces.LONELY)
        self.set('status', self._voice.on_lost_peer(peer))
        self.update()
        
    def on_free_channel(self, channel):
        self.set('face', faces.SMART)
        self.set('status', self._voice.on_free_channel(channel))
        self.update()
        
    def on_reading_logs(self, lines_so_far=0):
        self.set('face', faces.SMART)
        self.set('status', self._voice.on_reading_logs(lines_so_far))
        self.update()
        
    def wait(self, secs, sleeping=True):
        was_normal = self.is_normal()
        part = secs / 10.0
        
        for step in range(10):
            if was_normal or step > 5:
                if sleeping:
                    if secs > 1:
                        self.set('face', faces.SLEEP)
                        self.set('status', self._voice.on_napping(int(secs)))
                    else:
                        self.set('face', faces.SLEEP2)
                        self.set('status', self._voice.on_awakening())
                else:
                    self.set('status', self._voice.on_waiting(int(secs)))
                    good_mood = self._agent.in_good_mood()
                    if step % 2 == 0:
                        self.set('face', faces.LOOK_R_HAPPY if good_mood else faces.LOOK_R)
                    else:
                        self.set('face', faces.LOOK_L_HAPPY if good_mood else faces.LOOK_L)
                        
            time.sleep(part)
            secs -= part
            
        self.on_normal()
        
    def on_shutdown(self):
        self.set('face', faces.SLEEP)
        self.set('status', self._voice.on_shutdown())
        self.update(force=True)
        self._frozen = True
        
    def on_bored(self):
        self.set('face', faces.BORED)
        self.set('status', self._voice.on_bored())
        self.update()
        
    def on_sad(self):
        self.set('face', faces.SAD)
        self.set('status', self._voice.on_sad())
        self.update()
        
    def on_angry(self):
        self.set('face', faces.ANGRY)
        self.set('status', self._voice.on_angry())
        self.update()
        
    def on_motivated(self, reward):
        self.set('face', faces.MOTIVATED)
        self.set('status', self._voice.on_motivated(reward))
        self.update()
        
    def on_demotivated(self, reward):
        self.set('face', faces.DEMOTIVATED)
        self.set('status', self._voice.on_demotivated(reward))
        self.update()
        
    def on_excited(self):
        self.set('face', faces.EXCITED)
        self.set('status', self._voice.on_excited())
        self.update()
        
    def on_assoc(self, ap):
        self.set('face', faces.INTENSE)
        self.set('status', self._voice.on_assoc(ap))
        self.update()
        
    def on_deauth(self, sta):
        self.set('face', faces.COOL)
        self.set('status', self._voice.on_deauth(sta))
        self.update()
        
    def on_miss(self, who):
        self.set('face', faces.SAD)
        self.set('status', self._voice.on_miss(who))
        self.update()
        
    def on_grateful(self):
        self.set('face', faces.GRATEFUL)
        self.set('status', self._voice.on_grateful())
        self.update()
        
    def on_lonely(self):
        self.set('face', faces.LONELY)
        self.set('status', self._voice.on_lonely())
        self.update()
        
    def on_handshakes(self, new_shakes):
        self.set('face', faces.HAPPY)
        self.set('status', self._voice.on_handshakes(new_shakes))
        self.update()
        
    def on_unread_messages(self, count, total):
        self.set('face', faces.EXCITED)
        self.set('status', self._voice.on_unread_messages(count, total))
        self.update()
        time.sleep(5.0)
        
    def on_uploading(self, to):
        import random
        self.set('face', random.choice((faces.UPLOAD, faces.UPLOAD1, faces.UPLOAD2)))
        self.set('status', self._voice.on_uploading(to))
        self.update(force=True)
        
    def on_rebooting(self):
        self.set('face', faces.BROKEN)
        self.set('status', self._voice.on_rebooting())
        self.update()
        
    def on_custom(self, text):
        self.set('face', faces.DEBUG)
        self.set('status', self._voice.custom(text))
        self.update()
        
    def on_ai_ready(self):
        """Called when AI is ready."""
        self.set('face', faces.AWAKE)
        self.set('status', self._voice.on_normal())
        self.update()
        
    def on_channel_switch(self, channel):
        """Called when channel switches."""
        self.set('channel', str(channel))
        if self._pwnaui and self._pwnaui.connected:
            self._pwnaui.set_channel(str(channel))
        self.update()
        
    def on_wifi(self, aps):
        """Called when WiFi scan results are available."""
        if self._agent:
            total = self._agent.total_aps()
            current = len(aps) if hasattr(aps, '__len__') else 0
            self.set('aps', f'{current} ({total})')
            if self._pwnaui and self._pwnaui.connected:
                self._pwnaui.set_aps(f'{current} ({total})')
        self.update()
        
    def on_epoch(self, epoch_num, epoch_data):
        """Called on each epoch update."""
        if self._agent:
            try:
                start_time = self._agent.start_time()
                elapsed = int(time.time() - start_time)
                days, remainder = divmod(elapsed, 86400)
                hours, remainder = divmod(remainder, 3600)
                minutes, seconds = divmod(remainder, 60)
                uptime_str = f'{days:02d}:{hours:02d}:{minutes:02d}:{seconds:02d}'
                self.set('uptime', uptime_str)
                if self._pwnaui and self._pwnaui.connected:
                    self._pwnaui.set_uptime(uptime_str)
            except Exception:
                pass
        self.update()
        
    def freeze(self):
        """Freeze the display (prevent updates)."""
        self._frozen = True
        
    def unfreeze(self):
        """Unfreeze the display (allow updates)."""
        self._frozen = False
        
    def status(self, text):
        """Set the status text."""
        self.set('status', text)
        if self._pwnaui and self._pwnaui.connected:
            self._pwnaui.set_status(text)
        self.update()
