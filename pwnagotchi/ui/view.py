"""
PwnaUI PIL-Free View - Direct Replacement for pwnagotchi/ui/view.py

This completely replaces PIL rendering with native PwnaUI IPC.
No PIL imports, no image buffers, just direct state → C daemon communication.
"""

import threading
import logging
import random
import time
import os
import socket
from threading import Lock

import pwnagotchi
import pwnagotchi.plugins as plugins
import pwnagotchi.ui.faces as faces
import pwnagotchi.ui.fonts as fonts
import pwnagotchi.ui.web as web
import pwnagotchi.utils as utils

from pwnagotchi.ui.state import State
from pwnagotchi.voice import Voice

# Color constants (no PIL needed)
WHITE = 0x00
BLACK = 0xFF

# Global view instance for plugin compatibility
ROOT = None


class PwnaUIClient:
    """IPC client for pwnaui C daemon - robust socket handling"""
    
    SOCKET_PATH = '/var/run/pwnaui.sock'
    
    def __init__(self):
        self._lock = Lock()
    
    def _send_one(self, cmd):
        """Send a single command using a fresh socket each time"""
        try:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.settimeout(0.5)
            sock.connect(self.SOCKET_PATH)
            sock.sendall((cmd + '\n').encode())
            response = sock.recv(256).decode().strip()
            sock.close()
            return response
        except Exception as e:
            logging.debug(f"[pwnaui] IPC error: {e}")
            return None
    
    def send(self, cmd):
        """Thread-safe send"""
        with self._lock:
            return self._send_one(cmd)
    
    def is_available(self):
        return os.path.exists(self.SOCKET_PATH)
    
    def set_face(self, face):
        return self.send(f'SET_FACE {face}')
    
    def set_status(self, status):
        status = str(status).replace('\n', ' ').replace('\r', '')[:100]
        return self.send(f'SET_STATUS {status}')
    
    def set_name(self, name):
        return self.send(f'SET_NAME {name}')
    
    def set_channel(self, ch):
        return self.send(f'SET_CHANNEL {ch}')
    
    def set_aps(self, aps):
        return self.send(f'SET_APS {aps}')
    
    def set_uptime(self, up):
        return self.send(f'SET_UPTIME {up}')
    
    def set_shakes(self, shakes):
        return self.send(f'SET_SHAKES {shakes}')
    
    def set_mode(self, mode):
        return self.send(f'SET_MODE {mode}')
    
    def set_friend(self, name):
        if name:
            return self.send(f'SET_FRIEND {name}')
        return self.send('SET_FRIEND ')
    
    def update(self):
        return self.send('UPDATE')
    
    def full_update(self):
        return self.send('FULL_UPDATE')


# Global client
_pwnaui = PwnaUIClient()


class LabeledValue:
    """Minimal component - stores state only, rendering done by C daemon"""
    def __init__(self, color=BLACK, label='', value='', position=(0, 0),
                 label_font=None, text_font=None):
        self.label = label
        self.value = value
        self.position = position
        self.color = color

    def draw(self, canvas, drawer):
        pass  # No-op, C daemon renders


class Text:
    """Minimal text component"""
    def __init__(self, value='', position=(0, 0), font=None, color=BLACK,
                 wrap=False, max_length=0, png=False):
        self.value = value
        self.position = position
        self.color = color
        self.font = font
        self.wrap = wrap
        self.max_length = max_length
        self.png = png

    def draw(self, canvas, drawer):
        pass  # No-op


class Line:
    """Minimal line component"""
    def __init__(self, xy, color=BLACK, width=1):
        self.xy = xy
        self.color = color
        self.width = width

    def draw(self, canvas, drawer):
        pass  # No-op


class Rect:
    """Minimal rectangle component"""
    def __init__(self, xy, color=BLACK):
        self.xy = xy
        self.color = color

    def draw(self, canvas, drawer):
        pass  # No-op


class FilledRect:
    """Minimal filled rectangle"""
    def __init__(self, xy, color=BLACK):
        self.xy = xy
        self.color = color

    def draw(self, canvas, drawer):
        pass  # No-op


class Bitmap:
    """Minimal bitmap placeholder"""
    def __init__(self, path='', xy=(0, 0), color=BLACK):
        self.path = path
        self.xy = xy
        self.color = color

    def draw(self, canvas, drawer):
        pass  # No-op


class View:
    """
    PIL-Free View implementation.
    
    All rendering is delegated to the pwnaui C daemon via IPC.
    This maintains API compatibility with plugins while eliminating
    PIL's ~55MB memory footprint.
    """
    
    def __init__(self, config, impl, state=None):
        global BLACK, WHITE
        
        self.invert = 0
        self._black = 0xFF
        self._white = 0x00
        
        if config['ui'].get('invert', False):
            logging.debug("INVERT BLACK/WHITES")
            self.invert = 1
            BLACK = 0x00
            WHITE = 0xFF
            self._black = 0x00
            self._white = 0xFF
            _pwnaui.send('SET_INVERT 1')
        
        # Setup faces
        faces.load_from_config(config['ui']['faces'])
        
        self._agent = None
        self._render_cbs = []
        self._config = config
        self._canvas = None  # No PIL canvas needed
        self._frozen = False
        self._start_time = time.time()
        self._lock = Lock()
        self._voice = Voice(lang=config['main']['lang'])
        self._implementation = impl
        self._layout = impl.layout()
        self._width = self._layout['width']
        self._height = self._layout['height']
        
        # Initialize state with minimal components
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
        
        # Send initial layout to C daemon
        _pwnaui.send(f'SET_LAYOUT {self._layout["width"]}x{self._layout["height"]}')
        
        logging.info("[view] PIL-Free View initialized with PwnaUI backend")

        # Set global ROOT and notify plugins
        global ROOT
        ROOT = self
        plugins.on('ui_setup', self)
        
        # Start refresh handler thread for 1-second uptime ticks
        if config['ui'].get('fps', 0) > 0:
            threading.Thread(target=self._refresh_handler, args=(),
                           name="PwnaUI Handler", daemon=True).start()
            logging.info(f"[view] Started refresh handler at {config['ui']['fps']} FPS")
    
    def _refresh_handler(self):
        """Background thread that updates uptime every second"""
        delay = 1.0 / self._config['ui']['fps']
        while True:
            try:
                # Calculate live uptime
                elapsed = int(time.time() - self._start_time)
                days, remainder = divmod(elapsed, 86400)
                hours, remainder = divmod(remainder, 3600)
                mins, secs = divmod(remainder, 60)
                uptime = '%02d:%02d:%02d:%02d' % (days, hours, mins, secs)
                
                # Send directly to daemon (bypass state change tracking)
                _pwnaui.set_uptime(uptime)
                _pwnaui.update()
            except Exception as e:
                logging.debug(f"[view] Refresh error: {e}")
            time.sleep(delay)
    
    def on_agent(self, agent):
        self._agent = agent
    
    def config(self):
        return self._config
    
    def layout(self):
        return self._layout
    
    def on_state_change(self, key, value):
        """Forward state changes to C daemon"""
        if key == 'face':
            _pwnaui.set_face(value)
        elif key == 'status':
            _pwnaui.set_status(value)
        elif key == 'name':
            _pwnaui.set_name(value)
        elif key == 'channel':
            _pwnaui.set_channel(value)
        elif key == 'aps':
            _pwnaui.set_aps(value)
        elif key == 'uptime':
            _pwnaui.set_uptime(value)
        elif key == 'shakes':
            _pwnaui.set_shakes(value)
        elif key == 'mode':
            _pwnaui.set_mode(value)
        elif key == 'friend_name':
            _pwnaui.set_friend(value)
    
    def set(self, key, value):
        """Set UI state and forward to C daemon"""
        self._state.set(key, value)
        self.on_state_change(key, value)
    
    def get(self, key):
        return self._state.get(key)
    
    def on_render(self, cb):
        if cb not in self._render_cbs:
            self._render_cbs.append(cb)
    
    def width(self):
        return self._width
    
    def height(self):
        return self._height
    
    def set_agent(self, agent):
        self._agent = agent
    
    def is_frozen(self):
        return self._frozen
    
    def freeze(self):
        self._frozen = True
    
    def unfreeze(self):
        self._frozen = False
    
    def add_element(self, key, elem):
        self._state.set(key, elem)
    
    def remove_element(self, key):
        self._state.items.pop(key, None)
    
    def _refresh_status(self):
        """Refresh status text"""
        if self._agent:
            if self._agent.in_manual_mode():
                self.set('face', faces.COOL)
                self.set('status', self._voice.on_normal())
            else:
                self.set('status', self._voice.default())
    
    def update(self, force=False, new_data={}):
        """Update display - delegates to C daemon"""
        if self._frozen and not force:
            return
        
        with self._lock:
            # Call any registered render callbacks
            for cb in self._render_cbs:
                try:
                    cb(None)  # No canvas
                except Exception as e:
                    logging.error(f"Render callback error: {e}")
            
            # Trigger C daemon update
            if force:
                _pwnaui.full_update()
            else:
                _pwnaui.update()
            
            # Update web UI (still uses a simple approach)
            try:
                web.update_frame(None)
            except:
                pass
    
    # Face helper methods
    def on_starting(self):
        self.set('status', self._voice.on_starting())
        self.set('face', faces.COOL)
        self.update()
    
    def on_ai_ready(self):
        self.set('status', self._voice.on_ai_ready())
        self.set('face', faces.HAPPY)
        self.update()
    
    def on_manual_mode(self, last_session):
        self.set('status', self._voice.on_last_session_data(last_session))
        self.set('face', faces.COOL)
        self.update()
    
    def on_normal(self):
        self.set('status', self._voice.on_normal())
        self.set('face', faces.LOOK_R)
        self.update()
    
    def on_free_channel(self, channel):
        self.set('status', self._voice.on_free_channel(channel))
        self.set('face', faces.SMART)
        self.update()
    
    def on_reading_logs(self, lines_so_far=0):
        self.set('status', self._voice.on_reading_logs(lines_so_far))
        self.set('face', faces.SMART)
        self.update()
    
    def on_bored(self):
        self.set('status', self._voice.on_bored())
        self.set('face', faces.BORED)
        self.update()
    
    def on_sad(self):
        self.set('status', self._voice.on_sad())
        self.set('face', faces.SAD)
        self.update()
    
    def on_angry(self):
        self.set('status', self._voice.on_angry())
        self.set('face', faces.ANGRY)
        self.update()
    
    def on_motivated(self, reward):
        self.set('status', self._voice.on_motivated(reward))
        self.set('face', faces.MOTIVATED)
        self.update()
    
    def on_demotivated(self, reward):
        self.set('status', self._voice.on_demotivated(reward))
        self.set('face', faces.DEMOTIVATED)
        self.update()
    
    def on_excited(self):
        self.set('status', self._voice.on_excited())
        self.set('face', faces.EXCITED)
        self.update()
    
    def on_new_peer(self, peer):
        self.set('status', self._voice.on_new_peer(peer))
        self.set('face', faces.FRIEND)
        self.update()
    
    def on_lost_peer(self, peer):
        self.set('status', self._voice.on_lost_peer(peer))
        self.set('face', faces.LONELY)
        self.update()
    
    def on_miss(self, who):
        self.set('status', self._voice.on_miss(who))
        self.set('face', faces.SAD)
        self.update()
    
    def on_grateful(self):
        self.set('status', self._voice.on_grateful())
        self.set('face', faces.GRATEFUL)
        self.update()
    
    def on_lonely(self):
        self.set('status', self._voice.on_lonely())
        self.set('face', faces.LONELY)
        self.update()
    
    def on_handshakes(self, new_shakes):
        self.set('status', self._voice.on_handshakes(new_shakes))
        self.set('face', faces.HAPPY)
        self.update()
    
    def on_unread_messages(self, count, total):
        self.set('status', self._voice.on_unread_messages(count, total))
        self.set('face', faces.LOOK_R)
        self.update()
    
    def on_uploading(self, to):
        self.set('status', self._voice.on_uploading(to))
        self.set('face', faces.UPLOAD)
        self.update()
    
    def on_rebooting(self):
        self.set('status', self._voice.on_rebooting())
        self.set('face', faces.BROKEN)
        self.update()
    
    def on_custom(self, text, face=None):
        self.set('status', text)
        if face:
            self.set('face', face)
        self.update()
    
    def on_assoc(self, ap):
        self.set('status', self._voice.on_assoc(ap))
        self.set('face', faces.INTENSE)
        self.update()
    
    def on_deauth(self, sta):
        self.set('status', self._voice.on_deauth(sta))
        self.set('face', faces.COOL)
        self.update()
    
    def on_channel_hop(self, channel):
        self.set('channel', channel)
        self.update()
    
    def on_epoch(self, epoch, epoch_data):
        self.set('status', self._voice.on_epoch(epoch, epoch_data))
        self.update()
    
    def on_wait(self, remaining):
        self.set('status', self._voice.on_wait(remaining))
        self.set('face', faces.SLEEP)
        self.update()
    
    def on_sleep(self, secs):
        self.set('status', self._voice.on_sleep(secs))
        self.set('face', faces.SLEEP)
        self.update()
    
    def on_wake(self):
        self.set('status', self._voice.on_wake())
        self.set('face', faces.AWAKE)
        self.update()
    
    def on_keys_generation(self):
        self.set('status', self._voice.on_keys_generation())
        self.set('face', faces.SLEEP)
        self.update()
