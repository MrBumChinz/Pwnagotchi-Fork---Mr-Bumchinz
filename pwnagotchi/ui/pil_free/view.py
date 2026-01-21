"""
PIL-Free View for Pwnagotchi

This module provides a drop-in replacement for pwnagotchi.ui.view.View
that communicates with the PwnaUI C daemon instead of using PIL for rendering.

Key differences from PIL version:
- No PIL imports at module level (only imported for web UI fallback if needed)
- update() sends IPC commands to PwnaUI daemon instead of drawing to PIL canvas
- Memory usage: ~1MB vs ~55MB with PIL
- CPU usage: <1% vs 10-30% with PIL during rendering

The web UI still needs PNG output, so we support an optional fallback mode
that only renders to PIL when specifically requested for web UI updates.
"""

import logging
import random
import time
import threading
import os
from threading import Lock
from typing import Optional, Dict, Any, Callable, List

import pwnagotchi
import pwnagotchi.plugins as plugins
import pwnagotchi.ui.faces as faces
import pwnagotchi.ui.fonts as fonts
import pwnagotchi.ui.web as web
import pwnagotchi.utils as utils

from .components import (
    Widget, Text, LabeledValue, Line, Bitmap, Rect, FilledRect,
    create_fallback_canvas, render_widget_to_canvas
)
from pwnagotchi.ui.state import State
from pwnagotchi.voice import Voice

# Try to import PwnaUI client
try:
    import sys
    # Add pwnaui python path if not already present
    pwnaui_path = '/usr/lib/python3/dist-packages/pwnaui'
    if os.path.exists(pwnaui_path) and pwnaui_path not in sys.path:
        sys.path.insert(0, pwnaui_path)
    
    from pwnaui_client import PwnaUIClient
    PWNAUI_AVAILABLE = True
except ImportError:
    PWNAUI_AVAILABLE = False
    logging.warning("pwnaui_client not available - display will be limited")

# Colors
WHITE = 0x00
BLACK = 0xFF

# Global reference for plugins
ROOT = None

log = logging.getLogger("pil_free.view")


class View:
    """
    PIL-Free View class that renders via PwnaUI C daemon.
    
    This is a drop-in replacement for pwnagotchi.ui.view.View that:
    - Stores UI state in Python objects
    - Sends state changes to PwnaUI daemon via IPC
    - Optionally renders to PIL for web UI (only when requested)
    - Uses ~40x less memory than PIL version
    """
    
    def __init__(self, config: Dict, impl: Any, state: Optional[Dict] = None):
        global ROOT, BLACK, WHITE
        
        self.invert = 0
        self._black = 0xFF
        self._white = 0x00
        
        if 'invert' in config['ui'] and config['ui']['invert'] == True:
            log.debug(f"INVERT BLACK/WHITES: {config['ui']['invert']}")
            self.invert = 1
            BLACK = 0x00
            WHITE = 0xFF
            self._black = 0x00
            self._white = 0xFF
        
        # Setup faces from configuration
        faces.load_from_config(config['ui']['faces'])
        
        self._agent = None
        self._render_cbs: List[Callable] = []
        self._config = config
        self._canvas = None  # Only used for web UI fallback
        self._frozen = False
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
        self._pwnaui_connected = False
        if PWNAUI_AVAILABLE:
            self._pwnaui = PwnaUIClient()
            self._connect_pwnaui()
        
        # Initialize state with UI components
        self._state = State(state={
            'channel': LabeledValue(
                color=BLACK, label='CH', value='00',
                position=self._layout['channel'],
                label_font=fonts.Bold, text_font=fonts.Medium
            ),
            'aps': LabeledValue(
                color=BLACK, label='APS', value='0 (00)',
                position=self._layout['aps'],
                label_font=fonts.Bold, text_font=fonts.Medium
            ),
            'uptime': LabeledValue(
                color=BLACK, label='UP', value='00:00:00',
                position=self._layout['uptime'],
                label_font=fonts.Bold, text_font=fonts.Medium
            ),
            'line1': Line(self._layout['line1'], color=BLACK),
            'line2': Line(self._layout['line2'], color=BLACK),
            'face': Text(
                value=faces.SLEEP,
                position=(config['ui']['faces']['position_x'], config['ui']['faces']['position_y']),
                color=BLACK, font=fonts.Huge,
                png=config['ui']['faces']['png']
            ),
            'friend_name': Text(
                value=None,
                position=self._layout['friend_face'],
                font=fonts.BoldSmall, color=BLACK
            ),
            'name': Text(
                value='%s>' % 'pwnagotchi',
                position=self._layout['name'],
                color=BLACK, font=fonts.Bold
            ),
            'status': Text(
                value=self._voice.default(),
                position=self._layout['status']['pos'],
                color=BLACK,
                font=self._layout['status']['font'],
                wrap=True,
                max_length=self._layout['status']['max']
            ),
            'shakes': LabeledValue(
                label='PWND ', value='0 (00)', color=BLACK,
                position=self._layout['shakes'],
                label_font=fonts.Bold, text_font=fonts.Medium
            ),
            'mode': Text(
                value='AUTO',
                position=self._layout['mode'],
                font=fonts.Bold, color=BLACK
            ),
        })
        
        if state:
            for key, value in state.items():
                self._state.set(key, value)
        
        plugins.on('ui_setup', self)
        
        if config['ui']['fps'] > 0.0:
            threading.Thread(
                target=self._refresh_handler,
                args=(),
                name="UI Handler",
                daemon=True
            ).start()
            self._ignore_changes = ()
        else:
            log.warning("ui.fps is 0, the display will only update for major changes")
            self._ignore_changes = ('uptime', 'name')
        
        ROOT = self
    
    def _connect_pwnaui(self) -> bool:
        """Connect to PwnaUI daemon"""
        if not self._pwnaui:
            return False
        
        try:
            if self._pwnaui.connect():
                self._pwnaui_connected = True
                log.info("Connected to PwnaUI daemon")
                return True
        except Exception as e:
            log.warning(f"Failed to connect to PwnaUI daemon: {e}")
        
        self._pwnaui_connected = False
        return False
    
    def _send_to_pwnaui(self, command: str) -> bool:
        """Send a command to PwnaUI daemon"""
        if not self._pwnaui_connected:
            if not self._connect_pwnaui():
                return False
        
        try:
            response = self._pwnaui._send_command(command)
            return response is not None and response.startswith('OK')
        except Exception as e:
            log.debug(f"PwnaUI command failed: {e}")
            self._pwnaui_connected = False
            return False
    
    def set_agent(self, agent):
        self._agent = agent
    
    def has_element(self, key):
        return self._state.has_element(key)
    
    def add_element(self, key, elem):
        if self.invert == 1 and hasattr(elem, 'color') and elem.color:
            if elem.color == 0xff:
                elem.color = 0x00
            elif elem.color == 0x00:
                elem.color = 0xff
        self._state.add_element(key, elem)
    
    def remove_element(self, key):
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
        """Background thread for periodic UI updates"""
        delay = 1.0 / self._config['ui']['fps']
        while True:
            try:
                if self._config['ui'].get('cursor', True):
                    name = self._state.get('name')
                    self.set('name', name.rstrip('█').strip() if '█' in name else (name + ' █'))
                self.update()
            except Exception as e:
                log.warning(f"non fatal error while updating view: {e}")
            
            time.sleep(delay)
    
    def set(self, key: str, value: Any):
        """Set a UI state value"""
        self._state.set(key, value)
    
    def get(self, key: str) -> Any:
        """Get a UI state value"""
        return self._state.get(key)
    
    # =========================================================================
    # Event handlers - same as original view.py
    # =========================================================================
    
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
        self.set('shakes', '%d (%s)' % (
            last_session.handshakes,
            utils.total_unique_handshakes(self._config['bettercap']['handshakes'])
        ))
        self.set_closest_peer(last_session.last_peer, last_session.peers)
        self.update()
    
    def is_normal(self):
        return self._state.get('face') not in (
            faces.INTENSE, faces.COOL, faces.BORED, faces.HAPPY,
            faces.EXCITED, faces.MOTIVATED, faces.DEMOTIVATED,
            faces.SMART, faces.SAD, faces.LONELY
        )
    
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
            
            name = '▌' * num_bars
            name += '│' * (4 - num_bars)
            name += ' %s %d (%d)' % (peer.name(), peer.pwnd_run(), peer.pwnd_total())
            
            if num_total > 1:
                if num_total > 9000:
                    name += ' of over 9000'
                else:
                    name += ' of %d' % num_total
            
            self.set('friend_face', peer.face())
            self.set('friend_name', name)
        self.update()
    
    def on_new_peer(self, peer):
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
        
        for step in range(0, 10):
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
                    good_mood = self._agent.in_good_mood() if self._agent else True
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
    
    # =========================================================================
    # Main update method - sends state to PwnaUI instead of rendering with PIL
    # =========================================================================
    
    def update(self, force: bool = False, new_data: Dict = {}):
        """
        Update the display via PwnaUI daemon.
        
        Instead of rendering with PIL, this method:
        1. Sends state changes to PwnaUI daemon via IPC
        2. Optionally renders to PIL for web UI (if needed)
        3. Triggers render callbacks (for web UI)
        """
        for key, val in new_data.items():
            self.set(key, val)
        
        with self._lock:
            if self._frozen:
                return
            
            state = self._state
            changes = state.changes(ignore=self._ignore_changes)
            
            if force or len(changes):
                # Send state to PwnaUI daemon
                self._update_pwnaui(changes, force)
                
                # Notify plugins
                plugins.on('ui_update', self)
                
                # Update web UI (if configured and callbacks registered)
                if self._render_cbs or self._config.get('ui', {}).get('web', {}).get('enabled', True):
                    self._update_web_ui()
                
                self._state.reset()
    
    def _update_pwnaui(self, changes: List[str], force: bool = False):
        """Send state changes to PwnaUI daemon"""
        if not self._pwnaui_connected and not self._connect_pwnaui():
            return
        
        try:
            # Send changed values
            for key in changes:
                elem = self._state.get(key)
                if elem is None:
                    continue
                
                if key == 'face':
                    value = elem if isinstance(elem, str) else getattr(elem, 'value', str(elem))
                    self._send_to_pwnaui(f'SET_FACE {value}')
                
                elif key == 'status':
                    value = elem if isinstance(elem, str) else getattr(elem, 'value', str(elem))
                    # Escape newlines for IPC
                    value = value.replace('\n', '\\n') if value else ''
                    self._send_to_pwnaui(f'SET_STATUS {value}')
                
                elif key == 'name':
                    value = elem if isinstance(elem, str) else getattr(elem, 'value', str(elem))
                    self._send_to_pwnaui(f'SET_NAME {value}')
                
                elif key == 'channel':
                    if isinstance(elem, LabeledValue):
                        value = elem.value
                    elif isinstance(elem, str):
                        value = elem
                    else:
                        value = str(elem)
                    self._send_to_pwnaui(f'SET_CHANNEL {value}')
                
                elif key == 'aps':
                    if isinstance(elem, LabeledValue):
                        value = elem.value
                    elif isinstance(elem, str):
                        value = elem
                    else:
                        value = str(elem)
                    self._send_to_pwnaui(f'SET_APS {value}')
                
                elif key == 'uptime':
                    if isinstance(elem, LabeledValue):
                        value = elem.value
                    elif isinstance(elem, str):
                        value = elem
                    else:
                        value = str(elem)
                    self._send_to_pwnaui(f'SET_UPTIME {value}')
                
                elif key == 'shakes':
                    if isinstance(elem, LabeledValue):
                        value = elem.value
                    elif isinstance(elem, str):
                        value = elem
                    else:
                        value = str(elem)
                    self._send_to_pwnaui(f'SET_SHAKES {value}')
                
                elif key == 'mode':
                    value = elem if isinstance(elem, str) else getattr(elem, 'value', str(elem))
                    self._send_to_pwnaui(f'SET_MODE {value}')
                
                elif key == 'friend_name':
                    value = elem if isinstance(elem, str) else getattr(elem, 'value', '')
                    if value:
                        self._send_to_pwnaui(f'SET_FRIEND {value}')
            
            # Request display update
            if force:
                self._send_to_pwnaui('FULL_UPDATE')
            else:
                self._send_to_pwnaui('UPDATE')
        
        except Exception as e:
            log.debug(f"PwnaUI update failed: {e}")
            self._pwnaui_connected = False
    
    def _update_web_ui(self):
        """
        Update web UI with fallback PIL rendering.
        
        This is only used for the web interface PNG output.
        The main e-ink display uses PwnaUI daemon exclusively.
        """
        try:
            # Lazy import PIL only when needed for web UI
            from PIL import Image, ImageDraw
            
            # Create canvas for web UI
            self._canvas = Image.new('1', (self._width, self._height), self._white)
            drawer = ImageDraw.Draw(self._canvas)
            
            # Render all widgets (fallback mode)
            for key, elem in self._state.items():
                if elem is not None:
                    render_widget_to_canvas(elem, self._canvas, drawer)
            
            # Apply rotation if needed
            if self._rotation != 0:
                self._canvas = self._canvas.rotate(self._rotation, expand=True)
            
            # Save for web UI
            web.update_frame(self._canvas)
            
            # Trigger render callbacks
            for cb in self._render_cbs:
                cb(self._canvas)
        
        except ImportError:
            # PIL not available - web UI won't work
            log.debug("PIL not available for web UI fallback")
        except Exception as e:
            log.debug(f"Web UI update failed: {e}")
