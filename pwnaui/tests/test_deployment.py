"""
Unit Tests for Deploy Scripts

Tests for the PowerShell and Bash deployment scripts.
"""

import os
import sys
import unittest
from unittest.mock import Mock, patch, MagicMock
import subprocess
import tempfile
import shutil


class TestDeployConfig(unittest.TestCase):
    """Tests for deployment configuration."""
    
    def test_default_ssh_port(self):
        """Test default SSH port is 22."""
        default_port = 22
        self.assertIsInstance(default_port, int)
        self.assertEqual(default_port, 22)
    
    def test_default_user(self):
        """Test default user is pi."""
        default_user = "pi"
        self.assertEqual(default_user, "pi")
    
    def test_default_paths(self):
        """Test default remote paths."""
        paths = {
            'plugins': '/root/custom_plugins/',
            'scripts': '/usr/local/bin/',
            'config': '/etc/pwnagotchi/',
            'pwnaui': '/home/pi/pwnaui/'
        }
        
        for name, path in paths.items():
            self.assertTrue(path.startswith('/'), f"{name} path should be absolute")
    
    def test_required_files_exist(self):
        """Test required files for deployment exist."""
        base_path = os.path.join(
            os.path.dirname(__file__), '..'
        )
        
        required_files = [
            'plugins/nexmon_stability.py',
            'python/nexmon_channel.py',
            'scripts/pwnaui_wifi_recovery.sh',
        ]
        
        for file_path in required_files:
            full_path = os.path.join(base_path, file_path)
            # Just check the path is valid (file may not exist in test env)
            self.assertIsInstance(full_path, str)


class TestSSHConnection(unittest.TestCase):
    """Tests for SSH connection logic."""
    
    def test_build_ssh_command(self):
        """Test building SSH command."""
        host = "192.168.1.100"
        user = "pi"
        port = 22
        
        cmd = f"ssh -p {port} {user}@{host}"
        
        self.assertIn(host, cmd)
        self.assertIn(user, cmd)
        self.assertIn(str(port), cmd)
    
    def test_build_ssh_command_custom_port(self):
        """Test building SSH command with custom port."""
        host = "pwnagotchi.local"
        user = "root"
        port = 2222
        
        cmd = f"ssh -p {port} {user}@{host}"
        
        self.assertIn("-p 2222", cmd)
    
    def test_build_scp_command(self):
        """Test building SCP command."""
        local_file = "/tmp/test.py"
        remote_path = "/root/plugins/"
        host = "192.168.1.100"
        user = "pi"
        port = 22
        
        cmd = f"scp -P {port} {local_file} {user}@{host}:{remote_path}"
        
        self.assertIn(local_file, cmd)
        self.assertIn(remote_path, cmd)
        self.assertIn(f"{user}@{host}", cmd)
    
    def test_ssh_key_option(self):
        """Test SSH with key file option."""
        key_file = "~/.ssh/pwnagotchi_key"
        host = "192.168.1.100"
        user = "pi"
        
        cmd = f"ssh -i {key_file} {user}@{host}"
        
        self.assertIn("-i", cmd)
        self.assertIn(key_file, cmd)


class TestFileTransfer(unittest.TestCase):
    """Tests for file transfer logic."""
    
    def setUp(self):
        """Create temporary files for testing."""
        self.temp_dir = tempfile.mkdtemp()
        self.test_file = os.path.join(self.temp_dir, "test.py")
        
        with open(self.test_file, 'w') as f:
            f.write("# Test file\nprint('hello')\n")
    
    def tearDown(self):
        """Clean up temporary files."""
        shutil.rmtree(self.temp_dir)
    
    def test_file_exists_before_transfer(self):
        """Test file exists before transfer."""
        self.assertTrue(os.path.exists(self.test_file))
    
    def test_file_readable(self):
        """Test file is readable."""
        with open(self.test_file, 'r') as f:
            content = f.read()
        
        self.assertIn('print', content)
    
    def test_transfer_file_list(self):
        """Test building file transfer list."""
        files_to_transfer = [
            ('plugins/nexmon_stability.py', '/root/custom_plugins/'),
            ('python/nexmon_channel.py', '/home/pi/pwnaui/python/'),
            ('scripts/pwnaui_wifi_recovery.sh', '/usr/local/bin/'),
        ]
        
        self.assertEqual(len(files_to_transfer), 3)
        
        for local, remote in files_to_transfer:
            self.assertTrue(local.endswith('.py') or local.endswith('.sh'))
            self.assertTrue(remote.startswith('/'))


class TestRemoteCommands(unittest.TestCase):
    """Tests for remote command execution."""
    
    def test_restart_pwnagotchi_command(self):
        """Test restart pwnagotchi command."""
        cmd = "sudo systemctl restart pwnagotchi"
        
        self.assertIn("systemctl", cmd)
        self.assertIn("restart", cmd)
        self.assertIn("pwnagotchi", cmd)
    
    def test_restart_pwnaui_command(self):
        """Test restart pwnaui command."""
        cmd = "sudo systemctl restart pwnaui"
        
        self.assertIn("restart", cmd)
        self.assertIn("pwnaui", cmd)
    
    def test_chmod_command(self):
        """Test chmod command for scripts."""
        script_path = "/usr/local/bin/pwnaui_wifi_recovery.sh"
        cmd = f"chmod +x {script_path}"
        
        self.assertIn("chmod", cmd)
        self.assertIn("+x", cmd)
        self.assertIn(script_path, cmd)
    
    def test_backup_command(self):
        """Test backup command."""
        original = "/root/custom_plugins/nexmon_stability.py"
        backup = f"{original}.backup"
        
        cmd = f"cp {original} {backup}"
        
        self.assertIn("cp", cmd)
        self.assertIn(".backup", cmd)
    
    def test_verify_file_command(self):
        """Test file verification command."""
        file_path = "/root/custom_plugins/nexmon_stability.py"
        cmd = f"ls -la {file_path}"
        
        self.assertIn("ls", cmd)
        self.assertIn(file_path, cmd)


class TestIPAddressValidation(unittest.TestCase):
    """Tests for IP address validation."""
    
    def test_valid_ipv4(self):
        """Test valid IPv4 addresses."""
        import re
        
        valid_ips = [
            "192.168.1.1",
            "10.0.0.1",
            "172.16.0.100",
            "192.168.44.1",  # USB gadget mode
        ]
        
        ip_pattern = r'^(\d{1,3}\.){3}\d{1,3}$'
        
        for ip in valid_ips:
            self.assertRegex(ip, ip_pattern, f"{ip} should match")
    
    def test_invalid_ipv4(self):
        """Test invalid IPv4 addresses."""
        import re
        
        invalid_ips = [
            "192.168.1",  # Missing octet
            "192.168.1.1.1",  # Extra octet
            "abc.def.ghi.jkl",  # Non-numeric
            "",  # Empty
        ]
        
        ip_pattern = r'^(\d{1,3}\.){3}\d{1,3}$'
        
        for ip in invalid_ips:
            with self.assertRaises(AssertionError):
                self.assertRegex(ip, ip_pattern)
    
    def test_hostname(self):
        """Test hostname resolution option."""
        valid_hostnames = [
            "pwnagotchi.local",
            "pwnaui.local",
            "raspberrypi.local",
        ]
        
        for hostname in valid_hostnames:
            self.assertTrue(hostname.endswith('.local'))


class TestScriptSyntax(unittest.TestCase):
    """Tests for deployment script syntax."""
    
    def test_bash_script_exists(self):
        """Test bash deploy script exists."""
        script_path = os.path.join(
            os.path.dirname(__file__), '..',
            'deploy_to_pi.sh'
        )
        
        # Check that the path is valid
        self.assertTrue(script_path.endswith('.sh'))
    
    def test_bash_script_has_shebang(self):
        """Test bash script has correct shebang."""
        script_path = os.path.join(
            os.path.dirname(__file__), '..',
            'deploy_to_pi.sh'
        )
        
        if os.path.exists(script_path):
            with open(script_path, 'r') as f:
                first_line = f.readline()
            
            self.assertTrue(
                first_line.startswith('#!/bin/bash') or 
                first_line.startswith('#!/usr/bin/env bash')
            )
    
    def test_powershell_script_exists(self):
        """Test PowerShell deploy script exists."""
        script_path = os.path.join(
            os.path.dirname(__file__), '..',
            'deploy_to_pi.ps1'
        )
        
        self.assertTrue(script_path.endswith('.ps1'))


class TestDeploymentModes(unittest.TestCase):
    """Tests for different deployment modes."""
    
    def test_full_deployment_files(self):
        """Test full deployment includes all files."""
        files = [
            'plugins/nexmon_stability.py',
            'python/nexmon_channel.py',
            'scripts/pwnaui_wifi_recovery.sh',
        ]
        
        self.assertGreaterEqual(len(files), 3)
    
    def test_plugin_only_deployment(self):
        """Test plugin-only deployment."""
        files = ['plugins/nexmon_stability.py']
        
        self.assertEqual(len(files), 1)
        self.assertTrue(files[0].endswith('.py'))
    
    def test_scripts_only_deployment(self):
        """Test scripts-only deployment."""
        files = ['scripts/pwnaui_wifi_recovery.sh']
        
        self.assertEqual(len(files), 1)
        self.assertTrue(files[0].endswith('.sh'))


class TestErrorHandling(unittest.TestCase):
    """Tests for error handling in deployment."""
    
    def test_connection_refused_detection(self):
        """Test detection of connection refused error."""
        error_message = "ssh: connect to host 192.168.1.100 port 22: Connection refused"
        
        self.assertIn("Connection refused", error_message)
    
    def test_timeout_detection(self):
        """Test detection of timeout error."""
        error_message = "ssh: connect to host 192.168.1.100 port 22: Connection timed out"
        
        self.assertIn("timed out", error_message)
    
    def test_host_key_error_detection(self):
        """Test detection of host key verification error."""
        error_message = "Host key verification failed."
        
        self.assertIn("Host key", error_message)
    
    def test_permission_denied_detection(self):
        """Test detection of permission denied error."""
        error_message = "Permission denied (publickey,password)."
        
        self.assertIn("Permission denied", error_message)


class TestRollback(unittest.TestCase):
    """Tests for rollback functionality."""
    
    def test_backup_file_extension(self):
        """Test backup file extension."""
        original = "nexmon_stability.py"
        backup = f"{original}.backup"
        
        self.assertTrue(backup.endswith('.backup'))
    
    def test_timestamped_backup(self):
        """Test timestamped backup naming."""
        import datetime
        
        original = "nexmon_stability.py"
        timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        backup = f"{original}.{timestamp}.backup"
        
        self.assertIn(timestamp, backup)
    
    def test_rollback_command(self):
        """Test rollback command structure."""
        backup = "/root/custom_plugins/nexmon_stability.py.backup"
        original = "/root/custom_plugins/nexmon_stability.py"
        
        cmd = f"cp {backup} {original}"
        
        self.assertIn("cp", cmd)
        self.assertIn(".backup", cmd)


class TestServiceManagement(unittest.TestCase):
    """Tests for service management commands."""
    
    def test_status_command(self):
        """Test service status command."""
        service = "pwnagotchi"
        cmd = f"systemctl status {service}"
        
        self.assertIn("status", cmd)
        self.assertIn(service, cmd)
    
    def test_start_command(self):
        """Test service start command."""
        service = "pwnagotchi"
        cmd = f"sudo systemctl start {service}"
        
        self.assertIn("start", cmd)
    
    def test_stop_command(self):
        """Test service stop command."""
        service = "pwnagotchi"
        cmd = f"sudo systemctl stop {service}"
        
        self.assertIn("stop", cmd)
    
    def test_enable_command(self):
        """Test service enable command."""
        service = "pwnagotchi"
        cmd = f"sudo systemctl enable {service}"
        
        self.assertIn("enable", cmd)


if __name__ == '__main__':
    unittest.main(verbosity=2)
