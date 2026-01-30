"""
PwnaUI View Wrapper

This module patches the Pwnagotchi View class to send UI state updates
directly to the pwnaui C daemon instead of rendering with PIL.

Install to: /usr/lib/python3/dist-packages/pwnagotchi/ui/pwnaui_view.py
"""

import logging
import socket
import os


class PwnaUIClient:
    """Client for communicating with pwnaui daemon"""
    
    SOCKET_PATH = '/var/run/pwnaui.sock'
    
    def __init__(self):
        self._socket = None
        self._connected = False
        
    def connect(self):
        """Connect to pwnaui daemon"""
        if self._connected:
            return True
            
        try:
            self._socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self._socket.settimeout(0.5)
            self._socket.connect(self.SOCKET_PATH)
            self._connected = True
            return True
        except Exception as e:
            logging.debug(f"[pwnaui] Connect failed: {e}")
            self._connected = False
            return False
            
    def send(self, cmd):
        """Send command to daemon"""
        if not self._connected and not self.connect():
            return None
            
        try:
            self._socket.sendall((cmd + '\n').encode())
            return self._socket.recv(256).decode().strip()
        except:
            self._connected = False
            return None
            
    def set_face(self, face):
        return self.send(f'SET_FACE {face}')
        
    def set_status(self, status):
        # Truncate and clean status
        status = str(status).replace('\n', ' ')[:100]
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
        
    def set_friend(self, friend):
        if friend:
            return self.send(f'SET_FRIEND {friend}')
        else:
            return self.send('SET_FRIEND ')
            
    def update(self):
        return self.send('UPDATE')
        
    def full_update(self):
        return self.send('FULL_UPDATE')


# Global client instance
_client = None


def get_client():
    """Get or create the global pwnaui client"""
    global _client
    if _client is None:
        _client = PwnaUIClient()
    return _client


def patch_view():
    """
    Patch the View class to send updates to pwnaui daemon.
    Call this early in pwnagotchi startup.
    """
    from pwnagotchi.ui.view import View
    
    # Store original methods
    _original_set = View.set
    _original_update = View.update
    
    def patched_set(self, key, value):
        """Intercept state changes and forward to pwnaui"""
        # Call original
        _original_set(self, key, value)
        
        # Forward to pwnaui daemon
        client = get_client()
        
        if key == 'face':
            client.set_face(value)
        elif key == 'status':
            client.set_status(value)
        elif key == 'name':
            client.set_name(value)
        elif key == 'channel':
            client.set_channel(value)
        elif key == 'aps':
            client.set_aps(value)
        elif key == 'uptime':
            client.set_uptime(value)
        elif key == 'shakes':
            client.set_shakes(value)
        elif key == 'mode':
            client.set_mode(value)
        elif key == 'friend_name':
            client.set_friend(value)
            
    def patched_update(self, force=False, new_data={}):
        """Intercept update calls and forward to pwnaui"""
        # Call original (still does PIL rendering for web UI)
        _original_update(self, force, new_data)
        
        # Trigger pwnaui update
        client = get_client()
        if force:
            client.full_update()
        else:
            client.update()
    
    # Apply patches
    View.set = patched_set
    View.update = patched_update
    
    logging.info("[pwnaui] View class patched for pwnaui acceleration")


def is_available():
    """Check if pwnaui daemon is running"""
    return os.path.exists(PwnaUIClient.SOCKET_PATH)
