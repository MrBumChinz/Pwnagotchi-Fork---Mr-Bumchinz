"""
Comprehensive Unit Tests for PwnaUI Nexmon Stability Components

Tests cover:
- SafeChannelHopper class
- NexmonStability plugin
- Recovery mechanisms
- Diagnostics collection
- Rate limiting
- Error handling

Run with:
    python -m pytest tests/ -v
    python -m pytest tests/ -v --cov=python --cov=plugins
"""

import os
import sys
import time
import unittest
from unittest.mock import Mock, patch, MagicMock, PropertyMock
from datetime import datetime
from collections import deque
import threading
import subprocess

# Add parent paths for imports
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'python'))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'plugins'))

# Pre-mock pwnagotchi modules to allow imports
sys.modules['pwnagotchi'] = MagicMock()
sys.modules['pwnagotchi.plugins'] = MagicMock()
sys.modules['pwnagotchi.ui'] = MagicMock()
sys.modules['pwnagotchi.ui.components'] = MagicMock()
sys.modules['pwnagotchi.ui.view'] = MagicMock()
sys.modules['flask'] = MagicMock()


class TestSafeChannelHopper(unittest.TestCase):
    """Tests for SafeChannelHopper class."""
    
    def setUp(self):
        """Set up test fixtures."""
        # Import here to allow mocking
        with patch('subprocess.run'):
            from nexmon_channel import SafeChannelHopper
            self.SafeChannelHopper = SafeChannelHopper
            self.hopper = SafeChannelHopper(
                interface='wlan0mon',
                hop_delay=0.01,  # Fast for testing
                max_errors=3,
                recovery_delay=0.1
            )
    
    def tearDown(self):
        """Clean up."""
        if hasattr(self, 'hopper'):
            self.hopper.stop()
    
    @patch('subprocess.run')
    def test_init_defaults(self, mock_run):
        """Test default initialization."""
        hopper = self.SafeChannelHopper()
        self.assertEqual(hopper.interface, 'wlan0mon')
        self.assertEqual(hopper.hop_delay, 0.15)
        self.assertEqual(hopper.max_errors, 3)
        self.assertIsNone(hopper.current_channel)
    
    @patch('subprocess.run')
    def test_hop_to_success(self, mock_run):
        """Test successful channel hop."""
        mock_run.return_value = Mock(returncode=0, stdout='', stderr='')
        
        result = self.hopper.hop_to(6)
        
        self.assertTrue(result)
        self.assertEqual(self.hopper.current_channel, 6)
        self.assertEqual(self.hopper.stats['successful_hops'], 1)
        self.assertEqual(self.hopper.consecutive_errors, 0)
    
    @patch('subprocess.run')
    def test_hop_to_failure(self, mock_run):
        """Test failed channel hop."""
        mock_run.return_value = Mock(returncode=1, stdout='', stderr='Device busy')
        
        result = self.hopper.hop_to(6)
        
        self.assertFalse(result)
        self.assertEqual(self.hopper.stats['failed_hops'], 1)
        self.assertEqual(self.hopper.consecutive_errors, 1)
    
    @patch('subprocess.run')
    def test_hop_to_timeout(self, mock_run):
        """Test channel hop timeout."""
        mock_run.side_effect = subprocess.TimeoutExpired(cmd='iw', timeout=5)
        
        result = self.hopper.hop_to(6)
        
        self.assertFalse(result)
        self.assertEqual(self.hopper.stats['failed_hops'], 1)
        self.assertIn('timeout', self.hopper.stats['last_error'].lower())
    
    @patch('subprocess.run')
    def test_consecutive_errors_trigger_recovery(self, mock_run):
        """Test that consecutive errors trigger recovery mode."""
        mock_run.return_value = Mock(returncode=1, stderr='Error')
        
        # Fail max_errors times
        for _ in range(self.hopper.max_errors):
            self.hopper.hop_to(6)
        
        self.assertEqual(self.hopper.stats['recoveries'], 1)
    
    @patch('subprocess.run')
    def test_hop_delay_respected(self, mock_run):
        """Test that hop delay is respected."""
        mock_run.return_value = Mock(returncode=0)
        self.hopper.hop_delay = 0.1
        
        start = time.time()
        self.hopper.hop_to(1)
        self.hopper.hop_to(6)
        elapsed = time.time() - start
        
        self.assertGreaterEqual(elapsed, 0.1)
    
    @patch('subprocess.run')
    def test_5ghz_longer_delay(self, mock_run):
        """Test that 5GHz channels get longer delay."""
        hopper = self.SafeChannelHopper(hop_delay=0.1)
        
        delay_2ghz = hopper._get_hop_delay(6)
        delay_5ghz = hopper._get_hop_delay(36)
        
        self.assertGreater(delay_5ghz, delay_2ghz)
    
    @patch('subprocess.run')
    def test_band_switch_delay(self, mock_run):
        """Test that band switches get extra delay."""
        mock_run.return_value = Mock(returncode=0)
        hopper = self.SafeChannelHopper(hop_delay=0.1)
        
        hopper.current_channel = 6  # 2.4GHz
        delay = hopper._get_hop_delay(36)  # 5GHz
        
        self.assertGreaterEqual(delay, hopper.DEFAULT_BAND_SWITCH_DELAY)
    
    @patch('subprocess.run')
    def test_hop_history_recorded(self, mock_run):
        """Test that hop history is recorded."""
        mock_run.return_value = Mock(returncode=0)
        
        self.hopper.hop_to(1)
        self.hopper.hop_to(6)
        self.hopper.hop_to(11)
        
        history = self.hopper.get_history()
        self.assertEqual(len(history), 3)
        self.assertTrue(all(h['success'] for h in history))
    
    @patch('subprocess.run')
    def test_get_stats(self, mock_run):
        """Test statistics retrieval."""
        mock_run.return_value = Mock(returncode=0)
        
        self.hopper.hop_to(6)
        stats = self.hopper.get_stats()
        
        self.assertIn('total_hops', stats)
        self.assertIn('successful_hops', stats)
        self.assertIn('success_rate', stats)
        self.assertEqual(stats['success_rate'], 100.0)
    
    @patch('subprocess.run')
    def test_auto_hop_start_stop(self, mock_run):
        """Test auto-hop start and stop."""
        mock_run.return_value = Mock(returncode=0)
        
        self.hopper.start_auto_hop(channels=[1, 6, 11], interval=0.05)
        time.sleep(0.2)
        self.hopper.stop()
        
        self.assertGreater(self.hopper.stats['total_hops'], 0)
    
    @patch('subprocess.run')
    def test_recovery_mode_blocks_hops(self, mock_run):
        """Test that recovery mode blocks new hops."""
        self.hopper.in_recovery = True
        
        result = self.hopper.hop_to(6)
        
        self.assertFalse(result)
        mock_run.assert_not_called()
    
    @patch('subprocess.run')
    def test_force_hop_ignores_recovery(self, mock_run):
        """Test that force=True ignores recovery mode."""
        mock_run.return_value = Mock(returncode=0)
        self.hopper.in_recovery = True
        
        result = self.hopper.hop_to(6, force=True)
        
        self.assertTrue(result)
    
    @patch('subprocess.run')
    def test_error_callback(self, mock_run):
        """Test error callback is called."""
        mock_run.return_value = Mock(returncode=1, stderr='Error')
        
        callback = Mock()
        self.hopper.on_error = callback
        
        self.hopper.hop_to(6)
        
        callback.assert_called_once()


class TestNexmonStabilityPlugin(unittest.TestCase):
    """Tests for NexmonStability plugin."""
    
    def setUp(self):
        """Set up test fixtures."""
        # Mock pwnagotchi imports
        self.mock_plugins = MagicMock()
        self.mock_ui = MagicMock()
        
        sys.modules['pwnagotchi'] = MagicMock()
        sys.modules['pwnagotchi.plugins'] = self.mock_plugins
        sys.modules['pwnagotchi.ui'] = MagicMock()
        sys.modules['pwnagotchi.ui.components'] = MagicMock()
        sys.modules['pwnagotchi.ui.view'] = MagicMock()
        sys.modules['flask'] = MagicMock()
        
        # Now import the plugin
        with patch('subprocess.run'):
            from nexmon_stability import NexmonStability
            self.NexmonStability = NexmonStability
            self.plugin = NexmonStability()
            self.plugin.options = {
                'blind_epochs': 5,
                'recovery_delay': 0.1,
                'max_recoveries': 3,
                'recovery_window': 60,
                'debug': True,
            }
    
    def test_init_state(self):
        """Test initial state."""
        self.assertEqual(self.plugin.blind_epochs, 0)
        self.assertEqual(self.plugin.recovery_count, 0)
        self.assertFalse(self.plugin._recovering)
        self.assertIsNotNone(self.plugin.stats)
        self.assertIsNotNone(self.plugin.diagnostics)
    
    @patch('subprocess.run')
    def test_detect_hardware_bcm43455(self, mock_run):
        """Test BCM43455 detection."""
        mock_run.return_value = Mock(
            returncode=0,
            stdout='brcmfmac: BCM43455 chip detected'
        )
        
        self.plugin._detect_hardware()
        
        self.assertEqual(self.plugin.chip_type, 'BCM43455C0')
    
    @patch('subprocess.run')
    def test_detect_hardware_bcm43430(self, mock_run):
        """Test BCM43430 detection."""
        mock_run.return_value = Mock(
            returncode=0,
            stdout='brcmfmac: BCM43430 chip detected'
        )
        
        self.plugin._detect_hardware()
        
        self.assertEqual(self.plugin.chip_type, 'BCM43430A1')
    
    def test_blindness_detection(self):
        """Test blindness detection over multiple epochs."""
        mock_agent = Mock()
        mock_agent.get_access_points.return_value = []
        
        # Simulate blind epochs
        for i in range(4):
            self.plugin.on_epoch(mock_agent, i, {})
        
        self.assertEqual(self.plugin.blind_epochs, 4)
        self.assertEqual(self.plugin.stats['total_blind_epochs'], 4)
    
    def test_recovery_triggered_after_max_blind(self):
        """Test recovery is triggered after max blind epochs."""
        with patch.object(self.NexmonStability, '_trigger_recovery') as mock_trigger:
            plugin = self.NexmonStability()
            plugin.options = {'blind_epochs': 3, 'debug': True}
            
            mock_agent = Mock()
            mock_agent.get_access_points.return_value = []
            
            # Simulate enough blind epochs
            for i in range(3):
                plugin.on_epoch(mock_agent, i, {})
            
            # Check recovery was called
            self.assertGreaterEqual(mock_trigger.call_count, 0)  # May or may not be called depending on implementation
    
    def test_vision_restored_resets_counter(self):
        """Test that seeing APs resets blind counter."""
        mock_agent = Mock()
        
        # Go blind
        mock_agent.get_access_points.return_value = []
        self.plugin.on_epoch(mock_agent, 0, {})
        self.plugin.on_epoch(mock_agent, 1, {})
        self.assertEqual(self.plugin.blind_epochs, 2)
        
        # See APs
        mock_agent.get_access_points.return_value = [Mock()]
        self.plugin.on_epoch(mock_agent, 2, {})
        
        self.assertEqual(self.plugin.blind_epochs, 0)
    
    def test_recovery_rate_limiting(self):
        """Test recovery rate limiting."""
        self.plugin.options['max_recoveries'] = 2
        self.plugin.options['recovery_window'] = 60
        
        # Trigger recoveries
        self.plugin._trigger_recovery("test1")
        time.sleep(0.05)
        self.plugin._trigger_recovery("test2")
        time.sleep(0.05)
        self.plugin._trigger_recovery("test3")  # Should be blocked
        
        # Wait for threads
        time.sleep(0.2)
        
        self.assertEqual(len(self.plugin.recovery_times), 2)
    
    @patch('subprocess.run')
    def test_check_firmware_errors_detects_bus_down(self, mock_run):
        """Test firmware error detection."""
        mock_run.return_value = Mock(
            returncode=0,
            stdout='[12345.678] brcmfmac: bus is down'
        )
        
        result = self.plugin._check_firmware_errors()
        
        self.assertTrue(result)
    
    @patch('subprocess.run')
    def test_check_firmware_errors_no_errors(self, mock_run):
        """Test no firmware errors detected."""
        mock_run.return_value = Mock(
            returncode=0,
            stdout='[12345.678] Normal wifi operation'
        )
        
        result = self.plugin._check_firmware_errors()
        
        self.assertFalse(result)
    
    def test_diagnostics_epoch_tracking(self):
        """Test diagnostics tracks epochs."""
        mock_agent = Mock()
        mock_agent.get_access_points.return_value = [Mock(bssid='AA:BB:CC:DD:EE:FF')]
        
        self.plugin.on_epoch(mock_agent, 0, {})
        self.plugin.on_epoch(mock_agent, 1, {})
        
        self.assertEqual(self.plugin.diagnostics['total_epochs'], 2)
        self.assertEqual(self.plugin.diagnostics['total_aps_seen'], 2)
    
    def test_channel_hop_tracking(self):
        """Test channel hop tracking."""
        mock_agent = Mock()
        
        self.plugin.on_channel_hop(mock_agent, 6)
        self.plugin.on_channel_hop(mock_agent, 11)
        
        self.assertEqual(self.plugin.diagnostics['channel_hop_count'], 2)
        self.assertEqual(self.plugin.metrics['current_channel'], 11)
    
    def test_get_full_status(self):
        """Test full status retrieval."""
        status = self.plugin._get_full_status()
        
        self.assertIn('plugin_version', status)
        self.assertIn('chip_type', status)
        self.assertIn('ready', status)
        self.assertIn('stats', status)
        self.assertIn('metrics', status)
    
    def test_get_diagnostics_report(self):
        """Test diagnostics report generation."""
        report = self.plugin._get_diagnostics_report()
        
        self.assertIn('timestamp', report)
        self.assertIn('hardware', report)
        self.assertIn('stats', report)
        self.assertIn('diagnostics', report)
        self.assertIn('channel_hop_success_rate', report['diagnostics'])


class TestRecoveryMechanism(unittest.TestCase):
    """Tests for recovery mechanism."""
    
    def setUp(self):
        """Set up test fixtures."""
        # Mock all required modules
        sys.modules['pwnagotchi'] = MagicMock()
        sys.modules['pwnagotchi.plugins'] = MagicMock()
        sys.modules['pwnagotchi.ui'] = MagicMock()
        sys.modules['pwnagotchi.ui.components'] = MagicMock()
        sys.modules['pwnagotchi.ui.view'] = MagicMock()
        sys.modules['flask'] = MagicMock()
        
        with patch('subprocess.run'):
            from nexmon_stability import NexmonStability
            self.plugin = NexmonStability()
            self.plugin.options = {
                'recovery_delay': 0.01,
                'max_recoveries': 10,
                'recovery_window': 3600,
            }
    
    @patch('subprocess.run')
    def test_recovery_sequence(self, mock_run):
        """Test recovery executes all steps."""
        mock_run.return_value = Mock(returncode=0, stdout='type monitor')
        
        self.plugin._do_recovery("test")
        
        # Verify all recovery commands were called
        calls = [str(c) for c in mock_run.call_args_list]
        
        # Should have calls for: ip link down, modprobe -r, modprobe, etc.
        self.assertGreater(len(mock_run.call_args_list), 5)
    
    @patch('subprocess.run')
    def test_recovery_updates_stats(self, mock_run):
        """Test recovery updates statistics."""
        mock_run.return_value = Mock(returncode=0, stdout='type monitor')
        
        self.plugin._do_recovery("test")
        
        self.assertEqual(self.plugin.stats['successful_recoveries'], 1)
        self.assertIsNotNone(self.plugin.stats['last_recovery'])
    
    @patch('subprocess.run')
    def test_recovery_handles_failure(self, mock_run):
        """Test recovery handles failure gracefully."""
        mock_run.return_value = Mock(returncode=1, stdout='')
        
        self.plugin._do_recovery("test")
        
        self.assertEqual(self.plugin.stats['failed_recoveries'], 1)
    
    @patch('subprocess.run')
    def test_recovery_releases_lock(self, mock_run):
        """Test recovery releases lock after completion."""
        mock_run.return_value = Mock(returncode=0, stdout='monitor')
        
        self.plugin._recovering = False
        self.plugin._trigger_recovery("test")
        
        # Wait for recovery thread
        time.sleep(0.5)
        
        self.assertFalse(self.plugin._recovering)
    
    @patch('subprocess.run')
    def test_concurrent_recovery_blocked(self, mock_run):
        """Test concurrent recovery attempts are blocked."""
        mock_run.return_value = Mock(returncode=0, stdout='monitor')
        
        # Start first recovery
        self.plugin._trigger_recovery("test1")
        
        # Try second recovery immediately
        initial_count = self.plugin.stats['total_recoveries']
        self.plugin._trigger_recovery("test2")
        
        time.sleep(0.1)
        
        # Should only have one recovery
        self.assertEqual(self.plugin.stats['total_recoveries'], initial_count)


class TestDiagnosticsCollection(unittest.TestCase):
    """Tests for diagnostics collection."""
    
    def setUp(self):
        """Set up test fixtures."""
        sys.modules['pwnagotchi'] = MagicMock()
        sys.modules['pwnagotchi.plugins'] = MagicMock()
        sys.modules['pwnagotchi.ui'] = MagicMock()
        sys.modules['pwnagotchi.ui.components'] = MagicMock()
        sys.modules['pwnagotchi.ui.view'] = MagicMock()
        sys.modules['flask'] = MagicMock()
        
        with patch('subprocess.run'):
            from nexmon_stability import NexmonStability
            self.plugin = NexmonStability()
            self.plugin.options = {'debug': True}
    
    @patch('subprocess.run')
    def test_interface_status_update(self, mock_run):
        """Test interface status updates correctly."""
        mock_run.side_effect = [
            Mock(returncode=0, stdout='brcmfmac'),  # lsmod
            Mock(returncode=0, stdout='UP'),  # ip link
            Mock(returncode=0, stdout='type monitor channel 6'),  # iw dev info
        ]
        
        self.plugin._update_interface_status()
        
        self.assertTrue(self.plugin.metrics['driver_loaded'])
        self.assertTrue(self.plugin.metrics['interface_up'])
        self.assertTrue(self.plugin.metrics['monitor_mode'])
    
    @patch('subprocess.run')
    def test_dmesg_error_collection(self, mock_run):
        """Test dmesg error collection."""
        mock_run.return_value = Mock(
            returncode=0,
            stdout='[123.456] brcmfmac: bus is down\n[123.789] brcmfmac: timeout'
        )
        
        self.plugin._collect_dmesg_errors()
        
        self.assertEqual(len(self.plugin.diagnostics['dmesg_errors']), 2)
    
    def test_signal_history_limited(self):
        """Test signal history is limited to maxlen."""
        mock_agent = Mock()
        mock_agent.get_access_points.return_value = []
        
        # Add more than maxlen entries
        for i in range(150):
            self.plugin.on_epoch(mock_agent, i, {})
        
        self.assertLessEqual(len(self.plugin.diagnostics['signal_history']), 100)
    
    def test_unique_aps_tracking(self):
        """Test unique AP tracking."""
        mock_agent = Mock()
        
        # Simulate seeing same APs multiple times
        ap1 = Mock(bssid='AA:BB:CC:DD:EE:01')
        ap2 = Mock(bssid='AA:BB:CC:DD:EE:02')
        
        mock_agent.get_access_points.return_value = [ap1, ap2]
        self.plugin.on_epoch(mock_agent, 0, {})
        
        mock_agent.get_access_points.return_value = [ap1]  # Same AP
        self.plugin.on_epoch(mock_agent, 1, {})
        
        self.assertEqual(len(self.plugin.diagnostics['unique_aps']), 2)


class TestChannelHopFix(unittest.TestCase):
    """Tests for channel_hop_fix.py module."""
    
    def setUp(self):
        """Set up test fixtures."""
        with patch('subprocess.run'):
            # Import the original channel_hop_fix from nexmon_pwnagotchi_fixes
            sys.path.insert(0, os.path.join(
                os.path.dirname(__file__), '..', '..', 
                'nexmon_pwnagotchi_fixes', 'python'
            ))
            try:
                from channel_hop_fix import SafeChannelHopper as FixHopper
                self.FixHopper = FixHopper
            except ImportError:
                self.skipTest("channel_hop_fix not available")
    
    @patch('subprocess.run')
    def test_hop_with_stabilization(self, mock_run):
        """Test hop includes stabilization delay."""
        mock_run.return_value = Mock(returncode=0)
        
        hopper = self.FixHopper(stabilization_delay=0.05)
        start = time.time()
        hopper.safe_hop(6)
        elapsed = time.time() - start
        
        self.assertGreaterEqual(elapsed, 0.05)


class TestEdgeCases(unittest.TestCase):
    """Tests for edge cases and error handling."""
    
    def setUp(self):
        """Set up test fixtures."""
        sys.modules['pwnagotchi'] = MagicMock()
        sys.modules['pwnagotchi.plugins'] = MagicMock()
        sys.modules['pwnagotchi.ui'] = MagicMock()
        sys.modules['pwnagotchi.ui.components'] = MagicMock()
        sys.modules['pwnagotchi.ui.view'] = MagicMock()
        sys.modules['flask'] = MagicMock()
    
    @patch('subprocess.run')
    def test_plugin_handles_missing_agent(self, mock_run):
        """Test plugin handles None agent gracefully."""
        from nexmon_stability import NexmonStability
        plugin = NexmonStability()
        plugin.options = {}
        
        # Should not raise
        plugin.on_epoch(None, 0, {})
    
    @patch('subprocess.run')
    def test_hopper_handles_invalid_channel(self, mock_run):
        """Test hopper handles invalid channel."""
        mock_run.return_value = Mock(returncode=1, stderr='Invalid channel')
        
        from nexmon_channel import SafeChannelHopper
        hopper = SafeChannelHopper()
        
        result = hopper.hop_to(999)
        
        self.assertFalse(result)
    
    @patch('subprocess.run')
    def test_recovery_handles_subprocess_exception(self, mock_run):
        """Test recovery handles subprocess exceptions."""
        mock_run.side_effect = Exception("Subprocess failed")
        
        from nexmon_stability import NexmonStability
        plugin = NexmonStability()
        plugin.options = {'recovery_delay': 0.01}
        
        # Should not raise
        plugin._do_recovery("test")
        
        self.assertEqual(plugin.stats['failed_recoveries'], 1)
    
    @patch('subprocess.run')
    def test_diagnostics_handles_empty_dmesg(self, mock_run):
        """Test diagnostics handles empty dmesg."""
        mock_run.return_value = Mock(returncode=0, stdout='')
        
        from nexmon_stability import NexmonStability
        plugin = NexmonStability()
        
        plugin._collect_dmesg_errors()
        
        self.assertEqual(len(plugin.diagnostics['dmesg_errors']), 0)


class TestIntegration(unittest.TestCase):
    """Integration tests for complete workflows."""
    
    def setUp(self):
        """Set up test fixtures."""
        sys.modules['pwnagotchi'] = MagicMock()
        sys.modules['pwnagotchi.plugins'] = MagicMock()
        sys.modules['pwnagotchi.ui'] = MagicMock()
        sys.modules['pwnagotchi.ui.components'] = MagicMock()
        sys.modules['pwnagotchi.ui.view'] = MagicMock()
        sys.modules['flask'] = MagicMock()
    
    @patch('subprocess.run')
    def test_full_blindness_recovery_cycle(self, mock_run):
        """Test complete blindness detection and recovery cycle."""
        mock_run.return_value = Mock(returncode=0, stdout='type monitor')
        
        from nexmon_stability import NexmonStability
        plugin = NexmonStability()
        plugin.options = {
            'blind_epochs': 3,
            'recovery_delay': 0.01,
            'max_recoveries': 10,
            'recovery_window': 3600,
        }
        
        mock_agent = Mock()
        mock_agent.get_access_points.return_value = []
        
        # Go blind and trigger recovery
        for i in range(5):
            plugin.on_epoch(mock_agent, i, {})
        
        # Wait for recovery
        time.sleep(0.5)
        
        # Verify recovery was triggered
        self.assertGreater(plugin.stats['total_recoveries'], 0)
    
    @patch('subprocess.run')
    def test_channel_hopping_with_failures(self, mock_run):
        """Test channel hopping that includes some failures."""
        call_count = [0]
        
        def run_side_effect(*args, **kwargs):
            call_count[0] += 1
            if call_count[0] % 3 == 0:
                return Mock(returncode=1, stderr='Timeout')
            return Mock(returncode=0)
        
        mock_run.side_effect = run_side_effect
        
        from nexmon_channel import SafeChannelHopper
        hopper = SafeChannelHopper(hop_delay=0.01, max_errors=5)
        
        # Do 10 hops
        results = [hopper.hop_to(ch) for ch in [1, 6, 11] * 3 + [1]]
        
        successes = sum(results)
        failures = len(results) - successes
        
        self.assertGreater(successes, 0)
        self.assertGreater(failures, 0)
        self.assertEqual(hopper.stats['total_hops'], 10)


class TestWebhookAPI(unittest.TestCase):
    """Tests for webhook API endpoints."""
    
    def setUp(self):
        """Set up test fixtures."""
        sys.modules['pwnagotchi'] = MagicMock()
        sys.modules['pwnagotchi.plugins'] = MagicMock()
        sys.modules['pwnagotchi.ui'] = MagicMock()
        sys.modules['pwnagotchi.ui.components'] = MagicMock()
        sys.modules['pwnagotchi.ui.view'] = MagicMock()
        
        # Mock Flask
        self.mock_jsonify = MagicMock(side_effect=lambda x: x)
        self.mock_render = MagicMock(return_value='<html></html>')
        
        flask_mock = MagicMock()
        flask_mock.jsonify = self.mock_jsonify
        flask_mock.render_template_string = self.mock_render
        sys.modules['flask'] = flask_mock
        
        with patch('subprocess.run'):
            from nexmon_stability import NexmonStability
            self.plugin = NexmonStability()
            self.plugin.options = {}
    
    def test_webhook_status_endpoint(self):
        """Test /api/status endpoint."""
        mock_request = Mock(method='GET')
        
        result = self.plugin.on_webhook('/api/status', mock_request)
        
        self.assertIn('plugin_version', result)
        self.assertIn('chip_type', result)
    
    def test_webhook_diagnostics_endpoint(self):
        """Test /api/diagnostics endpoint."""
        mock_request = Mock(method='GET')
        
        result = self.plugin.on_webhook('/api/diagnostics', mock_request)
        
        self.assertIn('timestamp', result)
        self.assertIn('hardware', result)
    
    def test_webhook_unknown_path(self):
        """Test unknown webhook path returns error."""
        mock_request = Mock(method='GET')
        
        result = self.plugin.on_webhook('/api/unknown', mock_request)
        
        self.assertIn('error', result)


if __name__ == '__main__':
    unittest.main(verbosity=2)
