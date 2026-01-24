"""
Mock fixtures for unit tests.

Provides mock objects for testing WiFi, subprocess, and system interfaces.
"""

import os
import sys
from unittest.mock import Mock, MagicMock, patch
from typing import Dict, List, Optional, Callable
import subprocess


class MockSubprocess:
    """Mock subprocess module for testing."""
    
    def __init__(self):
        self.commands_run = []
        self.return_values = {}
        self.default_returncode = 0
        self.default_stdout = ""
        self.default_stderr = ""
    
    def set_response(self, command_pattern: str, returncode: int = 0, 
                     stdout: str = "", stderr: str = ""):
        """Set response for a command pattern."""
        self.return_values[command_pattern] = {
            'returncode': returncode,
            'stdout': stdout,
            'stderr': stderr,
        }
    
    def run(self, cmd, **kwargs):
        """Mock subprocess.run."""
        cmd_str = ' '.join(cmd) if isinstance(cmd, list) else cmd
        self.commands_run.append(cmd_str)
        
        # Find matching response
        for pattern, response in self.return_values.items():
            if pattern in cmd_str:
                result = Mock()
                result.returncode = response['returncode']
                result.stdout = response['stdout']
                result.stderr = response['stderr']
                return result
        
        # Default response
        result = Mock()
        result.returncode = self.default_returncode
        result.stdout = self.default_stdout
        result.stderr = self.default_stderr
        return result
    
    def check_output(self, cmd, **kwargs):
        """Mock subprocess.check_output."""
        result = self.run(cmd, **kwargs)
        if result.returncode != 0:
            raise subprocess.CalledProcessError(result.returncode, cmd)
        return result.stdout.encode() if isinstance(result.stdout, str) else result.stdout


class MockWirelessInterface:
    """Mock wireless interface for testing."""
    
    def __init__(self, name: str = "wlan0mon"):
        self.name = name
        self.mode = "managed"
        self.channel = 1
        self.frequency = 2412
        self.is_up = False
        self.driver = "brcmfmac"
        self.mac_address = "aa:bb:cc:dd:ee:ff"
    
    def set_monitor_mode(self) -> bool:
        """Set interface to monitor mode."""
        self.mode = "monitor"
        return True
    
    def set_managed_mode(self) -> bool:
        """Set interface to managed mode."""
        self.mode = "managed"
        return True
    
    def set_channel(self, channel: int) -> bool:
        """Set interface channel."""
        if 1 <= channel <= 14 or channel in [36, 40, 44, 48, 52, 56, 60, 64,
                                              100, 104, 108, 112, 116, 120,
                                              124, 128, 132, 136, 140, 144,
                                              149, 153, 157, 161, 165]:
            self.channel = channel
            return True
        return False
    
    def bring_up(self) -> bool:
        """Bring interface up."""
        self.is_up = True
        return True
    
    def bring_down(self) -> bool:
        """Bring interface down."""
        self.is_up = False
        return True
    
    def get_status(self) -> Dict:
        """Get interface status."""
        return {
            'name': self.name,
            'mode': self.mode,
            'channel': self.channel,
            'is_up': self.is_up,
            'driver': self.driver,
            'mac': self.mac_address,
        }


class MockDmesg:
    """Mock dmesg output generator."""
    
    def __init__(self):
        self.messages = []
        self.timestamp = 0.0
    
    def add_message(self, source: str, message: str, level: str = "info"):
        """Add a dmesg message."""
        self.timestamp += 0.001
        self.messages.append({
            'timestamp': self.timestamp,
            'source': source,
            'message': message,
            'level': level,
        })
    
    def add_brcmfmac_error(self, error_type: str = "scan"):
        """Add a brcmfmac error message."""
        errors = {
            'scan': 'brcmf_cfg80211_scan: scan error (-110)',
            'escan': 'brcmf_run_escan: escan failed (-110)',
            'p2p': 'brcmf_p2p_remain_on_channel: p2p listen error (-52)',
            'firmware': 'brcmf_fw_alloc_request: firmware: failed',
            'timeout': 'brcmfmac: brcmf_msgbuf_query_dcmd: Timeout on response',
        }
        
        message = errors.get(error_type, f'unknown error: {error_type}')
        self.add_message('brcmfmac', message, 'error')
    
    def get_output(self) -> str:
        """Get dmesg output as string."""
        lines = []
        for msg in self.messages:
            lines.append(f"[{msg['timestamp']:10.6f}] {msg['source']}: {msg['message']}")
        return '\n'.join(lines)
    
    def clear(self):
        """Clear all messages."""
        self.messages = []
        self.timestamp = 0.0


class MockPwnagotchiAgent:
    """Mock Pwnagotchi agent for testing plugins."""
    
    def __init__(self):
        self.config = {
            'main': {
                'iface': 'wlan0mon',
            },
            'personality': {
                'channels': [1, 6, 11],
            },
        }
        self.view = MockView()
        self._epoch = MockEpoch()
        self.last_session = MockSession()
    
    def run(self, cmd: str) -> str:
        """Run a command."""
        return ""
    
    def set_channel(self, channel: int):
        """Set channel."""
        self.config['current_channel'] = channel


class MockView:
    """Mock Pwnagotchi view."""
    
    def __init__(self):
        self._state = {}
    
    def set(self, key: str, value):
        """Set view state."""
        self._state[key] = value
    
    def get(self, key: str, default=None):
        """Get view state."""
        return self._state.get(key, default)


class MockEpoch:
    """Mock Pwnagotchi epoch."""
    
    def __init__(self):
        self.data = {
            'duration': 0,
            'deauths': 0,
            'assocs': 0,
            'handshakes': 0,
        }


class MockSession:
    """Mock Pwnagotchi session."""
    
    def __init__(self):
        self.is_new = True


class MockFlaskApp:
    """Mock Flask app for testing web endpoints."""
    
    def __init__(self):
        self.routes = {}
        self.test_client = MockTestClient(self)
    
    def route(self, path: str, methods: List[str] = None):
        """Decorator to register a route."""
        methods = methods or ['GET']
        
        def decorator(func):
            self.routes[path] = {
                'handler': func,
                'methods': methods,
            }
            return func
        
        return decorator
    
    def add_url_rule(self, path: str, endpoint: str, view_func: Callable,
                     methods: List[str] = None):
        """Add a URL rule."""
        methods = methods or ['GET']
        self.routes[path] = {
            'handler': view_func,
            'methods': methods,
            'endpoint': endpoint,
        }


class MockTestClient:
    """Mock test client for Flask app."""
    
    def __init__(self, app: MockFlaskApp):
        self.app = app
    
    def get(self, path: str):
        """Make GET request."""
        if path in self.app.routes:
            route = self.app.routes[path]
            if 'GET' in route['methods']:
                return MockResponse(route['handler']())
        return MockResponse(None, status_code=404)
    
    def post(self, path: str, json=None, data=None):
        """Make POST request."""
        if path in self.app.routes:
            route = self.app.routes[path]
            if 'POST' in route['methods']:
                return MockResponse(route['handler']())
        return MockResponse(None, status_code=404)


class MockResponse:
    """Mock HTTP response."""
    
    def __init__(self, data, status_code: int = 200):
        self._data = data
        self.status_code = status_code
    
    def get_json(self):
        """Get JSON data."""
        return self._data
    
    @property
    def data(self):
        """Get raw data."""
        return self._data


class MockFileSystem:
    """Mock file system for testing."""
    
    def __init__(self):
        self.files = {}
        self.directories = set()
    
    def create_file(self, path: str, content: bytes = b''):
        """Create a file."""
        self.files[path] = content
        # Create parent directories
        parts = path.split('/')
        for i in range(1, len(parts)):
            self.directories.add('/'.join(parts[:i]))
    
    def read_file(self, path: str) -> bytes:
        """Read a file."""
        if path in self.files:
            return self.files[path]
        raise FileNotFoundError(path)
    
    def write_file(self, path: str, content: bytes):
        """Write to a file."""
        self.files[path] = content
    
    def exists(self, path: str) -> bool:
        """Check if path exists."""
        return path in self.files or path in self.directories
    
    def is_file(self, path: str) -> bool:
        """Check if path is a file."""
        return path in self.files
    
    def is_dir(self, path: str) -> bool:
        """Check if path is a directory."""
        return path in self.directories


class MockSystemInfo:
    """Mock system information provider."""
    
    def __init__(self):
        self.chip = 'bcm43455c0'
        self.fw_version = '7_45_206'
        self.kernel = '5.10.0-20-arm64'
        self.memory = {
            'total': 1024 * 1024 * 1024,  # 1GB
            'available': 512 * 1024 * 1024,  # 512MB
        }
        self.cpu_temp = 45.0
    
    def get_chip_info(self) -> Dict:
        """Get WiFi chip info."""
        return {
            'chip': self.chip,
            'firmware_version': self.fw_version,
        }
    
    def get_system_info(self) -> Dict:
        """Get system info."""
        return {
            'kernel': self.kernel,
            'memory': self.memory,
            'cpu_temp': self.cpu_temp,
        }


# Factory functions for creating configured mocks

def create_healthy_interface_mock() -> MockWirelessInterface:
    """Create a mock interface in healthy state."""
    iface = MockWirelessInterface('wlan0mon')
    iface.mode = 'monitor'
    iface.is_up = True
    iface.channel = 6
    return iface


def create_failed_interface_mock() -> MockWirelessInterface:
    """Create a mock interface in failed state."""
    iface = MockWirelessInterface('wlan0mon')
    iface.mode = 'managed'
    iface.is_up = False
    return iface


def create_subprocess_mock_with_errors() -> MockSubprocess:
    """Create subprocess mock that returns errors."""
    mock = MockSubprocess()
    mock.set_response('iw dev', returncode=1, stderr='Device or resource busy')
    mock.set_response('ip link set', returncode=1, stderr='Operation not permitted')
    return mock


def create_subprocess_mock_healthy() -> MockSubprocess:
    """Create subprocess mock for healthy system."""
    mock = MockSubprocess()
    mock.set_response('iw dev', returncode=0, 
                      stdout='Interface wlan0mon\n\ttype monitor\n\tchannel 6')
    mock.set_response('ip link show', returncode=0, 
                      stdout='wlan0mon: <BROADCAST,MULTICAST,UP> state UP')
    return mock


def create_dmesg_mock_with_errors() -> MockDmesg:
    """Create dmesg mock with error messages."""
    mock = MockDmesg()
    mock.add_message('kernel', 'Linux version 5.10.0')
    mock.add_brcmfmac_error('scan')
    mock.add_brcmfmac_error('escan')
    mock.add_message('brcmfmac', 'wlan0mon: associated')
    mock.add_brcmfmac_error('timeout')
    return mock
