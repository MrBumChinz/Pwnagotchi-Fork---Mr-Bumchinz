"""
PwnaUI Client - Python interface to the PwnaUI C daemon

This module provides a simple API for Pwnagotchi to communicate with
the high-performance C UI renderer via UNIX domain socket.

Usage:
    from pwnaui_client import PwnaUIClient
    
    ui = PwnaUIClient()
    ui.connect()
    ui.set_face("(◕‿‿◕)")
    ui.set_status("Hello World!")
    ui.update()
"""

import socket
import os
import time
import logging
from typing import Optional, Tuple

SOCKET_PATH = "/var/run/pwnaui.sock"
CONNECT_TIMEOUT = 5.0
RECV_TIMEOUT = 1.0
MAX_RECONNECT_ATTEMPTS = 3
RECONNECT_DELAY = 0.5

log = logging.getLogger("pwnaui")


class PwnaUIError(Exception):
    """Base exception for PwnaUI errors"""
    pass


class ConnectionError(PwnaUIError):
    """Connection to daemon failed"""
    pass


class CommandError(PwnaUIError):
    """Command execution failed"""
    pass


class PwnaUIClient:
    """
    Client for communicating with the PwnaUI C daemon.
    
    The daemon handles all UI rendering - this client just sends
    commands over a UNIX socket.
    """
    
    def __init__(self, socket_path: str = SOCKET_PATH):
        """
        Initialize the client.
        
        Args:
            socket_path: Path to the UNIX domain socket
        """
        self._socket_path = socket_path
        self._sock: Optional[socket.socket] = None
        self._connected = False
        self._last_error: Optional[str] = None
        
    def __enter__(self):
        self.connect()
        return self
        
    def __exit__(self, exc_type, exc_val, exc_tb):
        self.disconnect()
        return False
        
    @property
    def connected(self) -> bool:
        """Check if connected to daemon"""
        return self._connected and self._sock is not None
        
    @property
    def last_error(self) -> Optional[str]:
        """Get the last error message"""
        return self._last_error
        
    def connect(self, timeout: float = CONNECT_TIMEOUT) -> bool:
        """
        Connect to the PwnaUI daemon.
        
        Args:
            timeout: Connection timeout in seconds
            
        Returns:
            True if connected, False otherwise
        """
        if self._connected:
            return True
            
        # Check if socket exists
        if not os.path.exists(self._socket_path):
            self._last_error = f"Socket not found: {self._socket_path}"
            log.warning(self._last_error)
            return False
            
        try:
            self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self._sock.settimeout(timeout)
            self._sock.connect(self._socket_path)
            self._sock.settimeout(RECV_TIMEOUT)
            self._connected = True
            self._last_error = None
            log.debug("Connected to PwnaUI daemon")
            
            # Test connection with PING
            if not self._ping():
                self.disconnect()
                return False
                
            return True
            
        except socket.error as e:
            self._last_error = f"Connection failed: {e}"
            log.warning(self._last_error)
            self._connected = False
            if self._sock:
                self._sock.close()
                self._sock = None
            return False
            
    def disconnect(self):
        """Disconnect from the daemon"""
        if self._sock:
            try:
                self._sock.close()
            except:
                pass
            self._sock = None
        self._connected = False
        
    def _send_command(self, cmd: str, expect_response: bool = True) -> Optional[str]:
        """
        Send a command to the daemon.
        
        Args:
            cmd: Command string (without newline)
            expect_response: Whether to wait for a response
            
        Returns:
            Response string or None on error
        """
        if not self._connected:
            if not self.connect():
                return None
                
        # Ensure command ends with newline
        if not cmd.endswith('\n'):
            cmd += '\n'
            
        try:
            self._sock.sendall(cmd.encode('utf-8'))
            
            if expect_response:
                response = self._sock.recv(1024).decode('utf-8').strip()
                if response.startswith("ERR"):
                    self._last_error = response
                    log.warning(f"Command error: {response}")
                    return None
                return response
            return "OK"
            
        except socket.timeout:
            self._last_error = "Timeout waiting for response"
            log.warning(self._last_error)
            return None
            
        except socket.error as e:
            self._last_error = f"Socket error: {e}"
            log.warning(self._last_error)
            self._connected = False
            return None
            
    def _send_with_reconnect(self, cmd: str) -> bool:
        """
        Send command with automatic reconnection on failure.
        
        Args:
            cmd: Command string
            
        Returns:
            True if command succeeded
        """
        for attempt in range(MAX_RECONNECT_ATTEMPTS):
            result = self._send_command(cmd)
            if result is not None:
                return True
                
            # Try to reconnect
            log.debug(f"Reconnection attempt {attempt + 1}/{MAX_RECONNECT_ATTEMPTS}")
            self.disconnect()
            time.sleep(RECONNECT_DELAY)
            
            if not self.connect():
                continue
                
        return False
        
    def _ping(self) -> bool:
        """Test connection with PING command"""
        response = self._send_command("PING")
        return response == "PONG"
        
    # === High-Level API (matches Pwnagotchi's view.py interface) ===
    
    def set_face(self, face: str) -> bool:
        """
        Set the face expression.
        
        Args:
            face: Face string (e.g., "(◕‿‿◕)")
        """
        return self._send_with_reconnect(f"SET_FACE {face}")
        
    def set_status(self, status: str) -> bool:
        """
        Set the status text.
        
        Args:
            status: Status message
        """
        # Escape newlines for protocol
        status = status.replace('\n', '\\n')
        return self._send_with_reconnect(f"SET_STATUS {status}")
        
    def set_channel(self, channel: str) -> bool:
        """Set the channel display"""
        return self._send_with_reconnect(f"SET_CHANNEL {channel}")
        
    def set_aps(self, aps: str) -> bool:
        """Set the APS count"""
        return self._send_with_reconnect(f"SET_APS {aps}")
        
    def set_uptime(self, uptime: str) -> bool:
        """Set the uptime display"""
        return self._send_with_reconnect(f"SET_UPTIME {uptime}")
        
    def set_shakes(self, shakes: str) -> bool:
        """Set the handshakes count"""
        return self._send_with_reconnect(f"SET_SHAKES {shakes}")
        
    def set_mode(self, mode: str) -> bool:
        """Set the mode (AUTO/MANU/AI)"""
        return self._send_with_reconnect(f"SET_MODE {mode}")
        
    def set_name(self, name: str) -> bool:
        """Set the pwnagotchi name"""
        return self._send_with_reconnect(f"SET_NAME {name}")
        
    def set_friend(self, friend: str) -> bool:
        """Set the friend info"""
        return self._send_with_reconnect(f"SET_FRIEND {friend}")
        
    def set_bluetooth(self, status: str) -> bool:
        """Set the Bluetooth tether status (C/-)"""
        return self._send_with_reconnect(f"SET_BLUETOOTH {status}")
        
    def set_memtemp_header(self, header: str) -> bool:
        """Set the memtemp header labels"""
        return self._send_with_reconnect(f"SET_MEMTEMP_HEADER {header}")
        
    def set_memtemp_data(self, data: str) -> bool:
        """Set the memtemp data values"""
        return self._send_with_reconnect(f"SET_MEMTEMP_DATA {data}")
        
    def set_invert(self, invert: bool) -> bool:
        """Set color inversion"""
        return self._send_with_reconnect(f"SET_INVERT {1 if invert else 0}")
        
    def set_layout(self, layout: str) -> bool:
        """Set the display layout"""
        return self._send_with_reconnect(f"SET_LAYOUT {layout}")
    
    # === Theme API ===
    
    def set_theme(self, name: str) -> bool:
        """
        Set the active theme.
        
        Args:
            name: Theme name (e.g., "default", "rick", "flipper")
                  or empty string to disable themes
        """
        if name:
            return self._send_with_reconnect(f"SET_THEME {name}")
        return self._send_with_reconnect("SET_THEME")
    
    def list_themes(self) -> Optional[list]:
        """
        Get list of available themes.
        
        Returns:
            List of theme names, or None on error
        """
        response = self._send_command("LIST_THEMES")
        if response and response.startswith("OK"):
            # Parse: "OK theme1,theme2,theme3"
            themes_str = response[3:].strip()
            if themes_str:
                return themes_str.split(",")
            return []
        return None
    
    def get_theme(self) -> Optional[str]:
        """
        Get the current active theme name.
        
        Returns:
            Theme name or None if no theme active
        """
        response = self._send_command("GET_THEME")
        if response and response.startswith("OK"):
            theme = response[3:].strip()
            return theme if theme else None
        return None
        
    def clear(self) -> bool:
        """Clear the display buffer"""
        return self._send_with_reconnect("CLEAR")
        
    def update(self, full: bool = False) -> bool:
        """
        Flush buffer to display.
        
        Args:
            full: Force full e-ink refresh
        """
        if full:
            return self._send_with_reconnect("FULL_UPDATE")
        return self._send_with_reconnect("UPDATE")
        
    # === Low-Level Drawing API ===
    
    def draw_text(self, x: int, y: int, text: str, font_id: int = 1) -> bool:
        """
        Draw text at a specific position.
        
        Args:
            x, y: Position
            text: Text to draw
            font_id: Font ID (0=small, 1=medium, 2=bold, 3=bold_small, 4=huge)
        """
        return self._send_with_reconnect(f"DRAW_TEXT {x} {y} {font_id} {text}")
        
    def draw_line(self, x1: int, y1: int, x2: int, y2: int) -> bool:
        """Draw a line"""
        return self._send_with_reconnect(f"DRAW_LINE {x1} {y1} {x2} {y2}")
        
    def draw_icon(self, name: str, x: int, y: int) -> bool:
        """
        Draw a named icon.
        
        Args:
            name: Icon name (wifi, battery_full, signal_4, etc.)
            x, y: Position
        """
        return self._send_with_reconnect(f"DRAW_ICON {name} {x} {y}")
        
    def get_state(self) -> Optional[dict]:
        """Get current UI state (for debugging)"""
        response = self._send_command("GET_STATE")
        if not response or not response.startswith("OK"):
            return None
            
        # Parse response: "OK face=X status=Y ..."
        state = {}
        parts = response[3:].strip().split()
        for part in parts:
            if '=' in part:
                key, value = part.split('=', 1)
                state[key] = value
        return state


# === Convenience functions for quick usage ===

_default_client: Optional[PwnaUIClient] = None


def get_client() -> PwnaUIClient:
    """Get or create the default client instance"""
    global _default_client
    if _default_client is None:
        _default_client = PwnaUIClient()
    if not _default_client.connected:
        _default_client.connect()
    return _default_client


def set_face(face: str) -> bool:
    return get_client().set_face(face)
    
def set_status(status: str) -> bool:
    return get_client().set_status(status)
    
def set_channel(channel: str) -> bool:
    return get_client().set_channel(channel)
    
def set_aps(aps: str) -> bool:
    return get_client().set_aps(aps)
    
def set_uptime(uptime: str) -> bool:
    return get_client().set_uptime(uptime)
    
def set_shakes(shakes: str) -> bool:
    return get_client().set_shakes(shakes)
    
def set_mode(mode: str) -> bool:
    return get_client().set_mode(mode)
    
def set_name(name: str) -> bool:
    return get_client().set_name(name)
    
def clear() -> bool:
    return get_client().clear()
    
def update(full: bool = False) -> bool:
    return get_client().update(full)


# === Test Code ===

if __name__ == "__main__":
    logging.basicConfig(level=logging.DEBUG)
    
    print("PwnaUI Client Test")
    print("==================")
    
    client = PwnaUIClient()
    
    if not client.connect():
        print(f"Failed to connect: {client.last_error}")
        print("Make sure pwnaui daemon is running:")
        print("  sudo systemctl start pwnaui")
        exit(1)
        
    print("Connected to daemon")
    
    # Test basic commands
    client.set_name("pwnagotchi>")
    client.set_face("(◕‿‿◕)")
    client.set_status("Testing PwnaUI!")
    client.set_channel("06")
    client.set_aps("3 (42)")
    client.set_uptime("01:23:45")
    client.set_shakes("5 (128)")
    client.set_mode("AUTO")
    
    # Update display
    client.update()
    
    print("Test commands sent successfully")
    print(f"State: {client.get_state()}")
    
    client.disconnect()
