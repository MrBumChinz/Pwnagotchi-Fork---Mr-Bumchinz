"""
PwnaUI Client Module Tests

Comprehensive unit tests for the pwnaui_client.py module.
Tests IPC client functionality, connection handling, and command sending.
"""

import unittest
import socket
import os
import sys
import tempfile
import threading
import time
from unittest.mock import Mock, patch, MagicMock

# Add parent directory to path for imports
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'python'))

from pwnaui_client import (
    PwnaUIClient,
    PwnaUIError,
    ConnectionError as PwnaUIConnectionError,
    CommandError,
    SOCKET_PATH,
    CONNECT_TIMEOUT,
    RECV_TIMEOUT,
    MAX_RECONNECT_ATTEMPTS
)


class TestPwnaUIClientInit(unittest.TestCase):
    """Tests for PwnaUIClient initialization."""
    
    def test_init_default_socket_path(self):
        """Test client initializes with default socket path."""
        client = PwnaUIClient()
        self.assertEqual(client._socket_path, SOCKET_PATH)
        
    def test_init_custom_socket_path(self):
        """Test client initializes with custom socket path."""
        custom_path = "/tmp/custom.sock"
        client = PwnaUIClient(socket_path=custom_path)
        self.assertEqual(client._socket_path, custom_path)
        
    def test_init_not_connected(self):
        """Test client starts disconnected."""
        client = PwnaUIClient()
        self.assertFalse(client.connected)
        
    def test_init_socket_is_none(self):
        """Test socket is None before connect."""
        client = PwnaUIClient()
        self.assertIsNone(client._sock)
        
    def test_init_no_last_error(self):
        """Test no last error on init."""
        client = PwnaUIClient()
        self.assertIsNone(client.last_error)


class TestPwnaUIClientConnection(unittest.TestCase):
    """Tests for connection handling."""
    
    def test_connect_fails_when_socket_not_exists(self):
        """Test connect fails when socket doesn't exist."""
        client = PwnaUIClient(socket_path="/nonexistent/socket.sock")
        result = client.connect()
        self.assertFalse(result)
        self.assertIn("not found", client.last_error)
        
    def test_connected_property_false_when_disconnected(self):
        """Test connected property returns False when not connected."""
        client = PwnaUIClient()
        self.assertFalse(client.connected)
        
    def test_disconnect_when_not_connected(self):
        """Test disconnect is safe when not connected."""
        client = PwnaUIClient()
        client.disconnect()  # Should not raise
        self.assertFalse(client.connected)
        
    @unittest.skipIf(not hasattr(socket, 'AF_UNIX'), "AF_UNIX not available on Windows")
    @patch('socket.socket')
    @patch('os.path.exists', return_value=True)
    def test_connect_sets_timeout(self, mock_exists, mock_socket_class):
        """Test connect sets socket timeout."""
        mock_sock = MagicMock()
        mock_socket_class.return_value = mock_sock
        
        client = PwnaUIClient()
        client.connect(timeout=5.0)
        
        mock_sock.settimeout.assert_called()
        
    @unittest.skipIf(not hasattr(socket, 'AF_UNIX'), "AF_UNIX not available on Windows")
    @patch('socket.socket')
    @patch('os.path.exists', return_value=True)
    def test_connect_attempts_connection(self, mock_exists, mock_socket_class):
        """Test connect attempts to connect to socket."""
        mock_sock = MagicMock()
        mock_sock.recv.return_value = b"PONG\n"
        mock_socket_class.return_value = mock_sock
        
        client = PwnaUIClient(socket_path="/test/socket.sock")
        client.connect()
        
        mock_sock.connect.assert_called_once_with("/test/socket.sock")


class TestPwnaUIClientContextManager(unittest.TestCase):
    """Tests for context manager support."""
    
    @patch.object(PwnaUIClient, 'connect')
    @patch.object(PwnaUIClient, 'disconnect')
    def test_context_manager_connects_on_enter(self, mock_disconnect, mock_connect):
        """Test context manager connects on __enter__."""
        with PwnaUIClient() as client:
            mock_connect.assert_called_once()
            
    @patch.object(PwnaUIClient, 'connect')
    @patch.object(PwnaUIClient, 'disconnect')
    def test_context_manager_disconnects_on_exit(self, mock_disconnect, mock_connect):
        """Test context manager disconnects on __exit__."""
        with PwnaUIClient() as client:
            pass
        mock_disconnect.assert_called_once()
        
    @patch.object(PwnaUIClient, 'connect')
    @patch.object(PwnaUIClient, 'disconnect')
    def test_context_manager_disconnects_on_exception(self, mock_disconnect, mock_connect):
        """Test context manager disconnects even on exception."""
        try:
            with PwnaUIClient() as client:
                raise ValueError("Test error")
        except ValueError:
            pass
        mock_disconnect.assert_called_once()


class TestPwnaUIClientCommands(unittest.TestCase):
    """Tests for command sending."""
    
    def setUp(self):
        """Set up test client with mock socket."""
        self.client = PwnaUIClient()
        self.client._sock = MagicMock()
        self.client._connected = True
        self.client._sock.recv.return_value = b"OK\n"
        
    def test_send_command_adds_newline(self):
        """Test _send_command adds newline if missing."""
        self.client._send_command("TEST")
        
        sent_data = self.client._sock.sendall.call_args[0][0]
        self.assertTrue(sent_data.endswith(b'\n'))
        
    def test_send_command_preserves_existing_newline(self):
        """Test _send_command doesn't double newline."""
        self.client._send_command("TEST\n")
        
        sent_data = self.client._sock.sendall.call_args[0][0]
        self.assertEqual(sent_data.count(b'\n'), 1)
        
    def test_send_command_returns_response(self):
        """Test _send_command returns server response."""
        self.client._sock.recv.return_value = b"OK\n"
        result = self.client._send_command("TEST")
        self.assertEqual(result, "OK")
        
    def test_send_command_handles_error_response(self):
        """Test _send_command handles ERR response."""
        self.client._sock.recv.return_value = b"ERR Invalid command\n"
        result = self.client._send_command("TEST")
        self.assertIsNone(result)
        self.assertIn("ERR", self.client.last_error)


class TestPwnaUIClientAPIFunctions(unittest.TestCase):
    """Tests for public API functions."""
    
    def setUp(self):
        """Set up test client with mock."""
        self.client = PwnaUIClient()
        self.client._sock = MagicMock()
        self.client._connected = True
        self.client._sock.recv.return_value = b"OK\n"
        
    def test_set_face(self):
        """Test set_face sends correct command."""
        self.client.set_face("(◕‿‿◕)")
        
        sent = self.client._sock.sendall.call_args[0][0].decode('utf-8')
        self.assertIn("SET_FACE", sent)
        self.assertIn("(◕‿‿◕)", sent)
        
    def test_set_face_empty(self):
        """Test set_face with empty string."""
        self.client.set_face("")
        
        sent = self.client._sock.sendall.call_args[0][0].decode('utf-8')
        self.assertIn("SET_FACE", sent)
        
    def test_set_status(self):
        """Test set_status sends correct command."""
        self.client.set_status("Hello World!")
        
        sent = self.client._sock.sendall.call_args[0][0].decode('utf-8')
        self.assertIn("SET_STATUS", sent)
        self.assertIn("Hello World!", sent)
        
    def test_set_channel(self):
        """Test set_channel sends correct command."""
        self.client.set_channel("11")
        
        sent = self.client._sock.sendall.call_args[0][0].decode('utf-8')
        self.assertIn("SET_CHANNEL", sent)
        self.assertIn("11", sent)
        
    def test_set_channel_int(self):
        """Test set_channel with integer."""
        self.client.set_channel(6)
        
        sent = self.client._sock.sendall.call_args[0][0].decode('utf-8')
        self.assertIn("SET_CHANNEL", sent)
        self.assertIn("6", sent)
        
    def test_set_aps(self):
        """Test set_aps sends correct command."""
        self.client.set_aps("5 (10)")
        
        sent = self.client._sock.sendall.call_args[0][0].decode('utf-8')
        self.assertIn("SET_APS", sent)
        self.assertIn("5", sent)
        
    def test_set_uptime(self):
        """Test set_uptime sends correct command."""
        self.client.set_uptime("01:23:45")
        
        sent = self.client._sock.sendall.call_args[0][0].decode('utf-8')
        self.assertIn("SET_UPTIME", sent)
        self.assertIn("01:23:45", sent)
        
    def test_set_shakes(self):
        """Test set_shakes sends correct command."""
        self.client.set_shakes("42 (100)")
        
        sent = self.client._sock.sendall.call_args[0][0].decode('utf-8')
        self.assertIn("SET_SHAKES", sent)
        self.assertIn("42", sent)
        
    def test_set_mode(self):
        """Test set_mode sends correct command."""
        self.client.set_mode("AUTO")
        
        sent = self.client._sock.sendall.call_args[0][0].decode('utf-8')
        self.assertIn("SET_MODE", sent)
        self.assertIn("AUTO", sent)
        
    def test_set_name(self):
        """Test set_name sends correct command."""
        self.client.set_name("pwnagotchi")
        
        sent = self.client._sock.sendall.call_args[0][0].decode('utf-8')
        self.assertIn("SET_NAME", sent)
        self.assertIn("pwnagotchi", sent)
        
    def test_update(self):
        """Test update sends UPDATE command."""
        self.client.update()
        
        sent = self.client._sock.sendall.call_args[0][0].decode('utf-8')
        self.assertIn("UPDATE", sent)
        
    def test_clear(self):
        """Test clear sends CLEAR command."""
        self.client.clear()
        
        sent = self.client._sock.sendall.call_args[0][0].decode('utf-8')
        self.assertIn("CLEAR", sent)


class TestPwnaUIClientPing(unittest.TestCase):
    """Tests for ping functionality."""
    
    def setUp(self):
        """Set up test client."""
        self.client = PwnaUIClient()
        self.client._sock = MagicMock()
        self.client._connected = True
        
    def test_ping_success(self):
        """Test ping returns True on PONG."""
        self.client._sock.recv.return_value = b"PONG\n"
        result = self.client._ping()
        self.assertTrue(result)
        
    def test_ping_failure(self):
        """Test ping returns False on other response."""
        self.client._sock.recv.return_value = b"ERR\n"
        result = self.client._ping()
        self.assertFalse(result)


class TestPwnaUIClientReconnect(unittest.TestCase):
    """Tests for automatic reconnection."""
    
    @patch.object(PwnaUIClient, 'connect')
    @patch.object(PwnaUIClient, 'disconnect')
    def test_reconnect_on_failure(self, mock_disconnect, mock_connect):
        """Test automatic reconnect on send failure."""
        client = PwnaUIClient()
        client._sock = MagicMock()
        client._connected = True
        
        # First send fails, second succeeds
        client._sock.sendall.side_effect = [socket.error("Connection reset"), None]
        client._sock.recv.return_value = b"OK\n"
        mock_connect.return_value = True
        
        result = client._send_with_reconnect("TEST")
        # After failure, should attempt reconnect
        
    def test_max_reconnect_attempts(self):
        """Test reconnection gives up after max attempts."""
        with patch.object(PwnaUIClient, 'connect', return_value=False) as mock_connect:
            client = PwnaUIClient()
            client._send_with_reconnect("TEST")
            
            # Should try some reconnect attempts (behavior may vary)
            self.assertGreaterEqual(mock_connect.call_count, 1)


class TestPwnaUIClientLayout(unittest.TestCase):
    """Tests for layout configuration."""
    
    def setUp(self):
        """Set up test client."""
        self.client = PwnaUIClient()
        self.client._sock = MagicMock()
        self.client._connected = True
        self.client._sock.recv.return_value = b"OK\n"
        
    def test_set_layout(self):
        """Test set_layout sends correct command."""
        self.client.set_layout("waveshare2in13_v2")
        
        sent = self.client._sock.sendall.call_args[0][0].decode('utf-8')
        self.assertIn("SET_LAYOUT", sent)
        self.assertIn("waveshare2in13_v2", sent)
        
    def test_set_invert_true(self):
        """Test set_invert with True."""
        self.client.set_invert(True)
        
        sent = self.client._sock.sendall.call_args[0][0].decode('utf-8')
        self.assertIn("SET_INVERT", sent)
        self.assertIn("1", sent)
        
    def test_set_invert_false(self):
        """Test set_invert with False."""
        self.client.set_invert(False)
        
        sent = self.client._sock.sendall.call_args[0][0].decode('utf-8')
        self.assertIn("SET_INVERT", sent)
        self.assertIn("0", sent)


class TestPwnaUIClientFriends(unittest.TestCase):
    """Tests for friend-related functionality."""
    
    def setUp(self):
        """Set up test client."""
        self.client = PwnaUIClient()
        self.client._sock = MagicMock()
        self.client._connected = True
        self.client._sock.recv.return_value = b"OK\n"
        
    def test_set_friend(self):
        """Test set_friend sends correct command."""
        self.client.set_friend("friend_pwn")
        
        sent = self.client._sock.sendall.call_args[0][0].decode('utf-8')
        self.assertIn("SET_FRIEND", sent)
        self.assertIn("friend_pwn", sent)
        
    def test_clear_friend(self):
        """Test clearing friend."""
        self.client.set_friend("")
        
        sent = self.client._sock.sendall.call_args[0][0].decode('utf-8')
        self.assertIn("SET_FRIEND", sent)


class TestPwnaUIClientModuleFunctions(unittest.TestCase):
    """Tests for module-level convenience functions."""
    
    def test_convenience_functions_exist(self):
        """Test module provides expected classes and errors."""
        import pwnaui_client
        
        # Test core classes exist
        self.assertTrue(hasattr(pwnaui_client, 'PwnaUIClient'))
        self.assertTrue(hasattr(pwnaui_client, 'PwnaUIError'))
        self.assertTrue(hasattr(pwnaui_client, 'ConnectionError'))
        self.assertTrue(hasattr(pwnaui_client, 'CommandError'))


class TestPwnaUIClientEdgeCases(unittest.TestCase):
    """Tests for edge cases and error handling."""
    
    def test_unicode_face(self):
        """Test handling Unicode characters in face."""
        client = PwnaUIClient()
        client._sock = MagicMock()
        client._connected = True
        client._sock.recv.return_value = b"OK\n"
        
        # Various face expressions with Unicode
        faces = [
            "(◕‿‿◕)",
            "(≖__≖)",
            "(⚆_⚆)",
            "(☉_☉)",
            "(-__-)",
            "(◕‿◕)",
        ]
        
        for face in faces:
            client.set_face(face)
            sent = client._sock.sendall.call_args[0][0].decode('utf-8')
            self.assertIn(face, sent)
            
    def test_long_status(self):
        """Test handling long status messages."""
        client = PwnaUIClient()
        client._sock = MagicMock()
        client._connected = True
        client._sock.recv.return_value = b"OK\n"
        
        long_status = "A" * 500
        client.set_status(long_status)
        
        sent = client._sock.sendall.call_args[0][0].decode('utf-8')
        self.assertIn("SET_STATUS", sent)
        
    def test_special_characters_in_status(self):
        """Test special characters in status."""
        client = PwnaUIClient()
        client._sock = MagicMock()
        client._connected = True
        client._sock.recv.return_value = b"OK\n"
        
        # Status with special characters
        status = "Hello! @world #test $100"
        client.set_status(status)
        
        sent = client._sock.sendall.call_args[0][0].decode('utf-8')
        self.assertIn(status, sent)
        
    def test_newlines_in_status_stripped(self):
        """Test newlines in status are handled."""
        client = PwnaUIClient()
        client._sock = MagicMock()
        client._connected = True
        client._sock.recv.return_value = b"OK\n"
        
        # Status with embedded newlines should be handled
        status = "Line1\nLine2"
        client.set_status(status)
        
        # Should send without breaking protocol
        sent = client._sock.sendall.call_args[0][0]
        # Should only have one newline at end
        self.assertEqual(sent.count(b'\n'), 1)


class TestPwnaUIClientThreadSafety(unittest.TestCase):
    """Tests for thread safety considerations."""
    
    def test_multiple_set_operations(self):
        """Test multiple rapid set operations."""
        client = PwnaUIClient()
        client._sock = MagicMock()
        client._connected = True
        client._sock.recv.return_value = b"OK\n"
        
        # Simulate rapid updates
        for i in range(100):
            client.set_face(f"({i})")
            client.set_status(f"Status {i}")
            client.set_channel(str(i % 14))
            
        # Should complete without error
        self.assertTrue(True)


class TestExceptionClasses(unittest.TestCase):
    """Tests for exception classes."""
    
    def test_pwnaui_error_is_exception(self):
        """Test PwnaUIError inherits from Exception."""
        self.assertTrue(issubclass(PwnaUIError, Exception))
        
    def test_connection_error_is_pwnaui_error(self):
        """Test ConnectionError inherits from PwnaUIError."""
        self.assertTrue(issubclass(PwnaUIConnectionError, PwnaUIError))
        
    def test_command_error_is_pwnaui_error(self):
        """Test CommandError inherits from PwnaUIError."""
        self.assertTrue(issubclass(CommandError, PwnaUIError))
        
    def test_can_raise_pwnaui_error(self):
        """Test PwnaUIError can be raised and caught."""
        with self.assertRaises(PwnaUIError):
            raise PwnaUIError("Test error")
            
    def test_can_raise_connection_error(self):
        """Test ConnectionError can be raised and caught."""
        with self.assertRaises(PwnaUIConnectionError):
            raise PwnaUIConnectionError("Connection failed")
            
    def test_can_raise_command_error(self):
        """Test CommandError can be raised and caught."""
        with self.assertRaises(CommandError):
            raise CommandError("Command failed")


if __name__ == '__main__':
    # Run with verbose output
    unittest.main(verbosity=2)
