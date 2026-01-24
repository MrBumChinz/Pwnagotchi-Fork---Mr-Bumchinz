"""
Nexmon Stability Plugin for PwnaUI/Pwnagotchi

Addresses the well-known nexmon firmware stability issues:
- SCB NULL pointer crash during packet injection
- "Blindness bug" (-110 timeout errors)
- Channel hop failures and hangs
- Bus down / firmware crash recovery

Based on analysis of:
- https://github.com/seemoo-lab/nexmon/issues/335
- https://github.com/evilsocket/pwnagotchi/issues/267

Installation:
    1. Copy to /etc/pwnagotchi/custom-plugins/nexmon_stability.py
    2. Add to config.toml:
       
       main.custom_plugins = "/etc/pwnagotchi/custom-plugins/"
       main.plugins.nexmon_stability.enabled = true
       main.plugins.nexmon_stability.blind_epochs = 10
       main.plugins.nexmon_stability.recovery_delay = 5

Author: PwnaUI Team
License: MIT
"""

import os
import sys
import time
import json
import logging
import subprocess
import threading
from datetime import datetime
from collections import deque

import pwnagotchi.plugins as plugins
from pwnagotchi.ui.components import LabeledValue
from pwnagotchi.ui.view import BLACK
from flask import Blueprint, jsonify, render_template_string

# Add pwnaui python path for channel hop fix
sys.path.insert(0, '/home/pi/pwnaui/python')

try:
    from nexmon_channel import SafeChannelHopper
    CHANNEL_FIX_AVAILABLE = True
except ImportError:
    SafeChannelHopper = None
    CHANNEL_FIX_AVAILABLE = False


class NexmonStability(plugins.Plugin):
    __author__ = 'PwnaUI'
    __version__ = '2.0.0'
    __license__ = 'MIT'
    __description__ = 'Nexmon firmware stability fixes and recovery'
    __name__ = 'nexmon_stability'
    __help__ = 'Monitors WiFi firmware health and recovers from crashes'
    __dependencies__ = {}
    __defaults__ = {
        'enabled': False,
        'blind_epochs': 10,
        'recovery_delay': 5,
        'max_recoveries': 5,
        'recovery_window': 3600,
        'channel_hop_delay': 0.15,
        'injection_rate_limit': 50,
        'show_status': True,
        'debug': False,
    }

    def __init__(self):
        self.ready = False
        self.agent = None
        self.options = {}
        
        # State tracking
        self.blind_epochs = 0
        self.last_aps = 0
        self.last_activity = time.time()
        self.recovery_count = 0
        self.recovery_times = []
        self.firmware_version = "unknown"
        self.chip_type = "unknown"
        
        # Recovery lock
        self._recovery_lock = threading.Lock()
        self._recovering = False
        
        # Channel hopper
        self.channel_hopper = None
        
        # Statistics
        self.stats = {
            'total_recoveries': 0,
            'successful_recoveries': 0,
            'failed_recoveries': 0,
            'total_blind_epochs': 0,
            'last_recovery': None,
            'uptime_since_recovery': 0,
        }
        
        # Enhanced diagnostics
        self.diagnostics = {
            'start_time': datetime.now().isoformat(),
            'total_epochs': 0,
            'total_aps_seen': 0,
            'unique_aps': set(),
            'channel_hop_count': 0,
            'channel_hop_failures': 0,
            'dmesg_errors': [],
            'signal_history': deque(maxlen=100),
            'recovery_history': deque(maxlen=50),
            'firmware_reloads': 0,
        }
        
        # Real-time metrics
        self.metrics = {
            'current_channel': None,
            'last_rssi': None,
            'packets_injected': 0,
            'injection_failures': 0,
            'interface_up': False,
            'monitor_mode': False,
            'driver_loaded': False,
        }

    def on_loaded(self):
        """Called when plugin is loaded."""
        logging.info("[nexmon_stability] Plugin loaded")
        
        # Detect chip and firmware
        self._detect_hardware()
        
        # Initialize channel hopper if available
        if CHANNEL_FIX_AVAILABLE:
            try:
                hop_delay = self.options.get('channel_hop_delay', 0.15)
                self.channel_hopper = SafeChannelHopper(
                    interface='wlan0mon',
                    hop_delay=hop_delay
                )
                logging.info("[nexmon_stability] Safe channel hopper initialized")
            except Exception as e:
                logging.warning(f"[nexmon_stability] Channel hopper init failed: {e}")
        
        # Start diagnostics collector
        self._start_diagnostics_collector()
        
        self.ready = True

    def on_ready(self, agent):
        """Called when pwnagotchi is ready."""
        self.agent = agent
        logging.info(f"[nexmon_stability] Ready - Chip: {self.chip_type}, FW: {self.firmware_version}")

    def on_webhook(self, path, request):
        """Handle web API requests for diagnostics."""
        if path == '/' or path == '':
            return render_template_string(DIAGNOSTICS_HTML, plugin=self)
        elif path == '/api/status':
            return jsonify(self._get_full_status())
        elif path == '/api/stats':
            return jsonify(self.stats)
        elif path == '/api/diagnostics':
            return jsonify(self._get_diagnostics_report())
        elif path == '/api/recovery':
            if request.method == 'POST':
                self._trigger_recovery("manual_api")
                return jsonify({'status': 'recovery_triggered'})
            return jsonify({'error': 'POST required'}), 400
        elif path == '/api/dmesg':
            return jsonify(self._get_dmesg_errors())
        elif path == '/api/interface':
            return jsonify(self._get_interface_info())
        return jsonify({'error': 'unknown path'}), 404

    def on_ui_setup(self, ui):
        """Add status indicator to UI."""
        if self.options.get('show_status', True):
            try:
                pos = (ui.width() - 60, 0)
                ui.add_element(
                    'nexmon_status',
                    LabeledValue(
                        color=BLACK,
                        label='NX:',
                        value='OK',
                        position=pos,
                        label_font=None,
                        text_font=None
                    )
                )
            except Exception as e:
                logging.debug(f"[nexmon_stability] UI setup error: {e}")

    def on_ui_update(self, ui):
        """Update status indicator."""
        if not self.options.get('show_status', True):
            return
            
        try:
            if self._recovering:
                status = 'RCV'
            elif self.blind_epochs > 0:
                status = f'B{self.blind_epochs}'
            else:
                status = 'OK'
            
            ui.set('nexmon_status', status)
        except:
            pass

    def on_epoch(self, agent, epoch, epoch_data):
        """Called each epoch - monitor for blindness."""
        self.agent = agent
        self.diagnostics['total_epochs'] += 1
        
        # Get current AP count
        try:
            aps = agent.get_access_points() if hasattr(agent, 'get_access_points') else []
            current_aps = len(aps)
            self.diagnostics['total_aps_seen'] += current_aps
            
            # Track unique APs
            for ap in aps:
                if hasattr(ap, 'bssid'):
                    self.diagnostics['unique_aps'].add(ap.bssid)
        except:
            current_aps = 0
        
        # Record signal history
        self.diagnostics['signal_history'].append({
            'time': datetime.now().isoformat(),
            'epoch': epoch,
            'aps': current_aps,
            'blind': current_aps == 0
        })
        
        # Detect blindness (seeing 0 APs for multiple epochs)
        if current_aps == 0:
            self.blind_epochs += 1
            self.stats['total_blind_epochs'] += 1
            
            if self.options.get('debug', False):
                logging.debug(f"[nexmon_stability] Blind epoch {self.blind_epochs}")
            
            # Check if we've been blind too long
            max_blind = self.options.get('blind_epochs', 10)
            if self.blind_epochs >= max_blind:
                logging.warning(f"[nexmon_stability] Blind for {self.blind_epochs} epochs, triggering recovery")
                self._trigger_recovery("blindness")
        else:
            # Reset blind counter
            if self.blind_epochs > 0:
                logging.info(f"[nexmon_stability] Vision restored after {self.blind_epochs} blind epochs")
            self.blind_epochs = 0
            self.last_aps = current_aps
            self.last_activity = time.time()

    def on_channel_hop(self, agent, channel):
        """Called on channel hop - use safe hopper if available."""
        self.diagnostics['channel_hop_count'] += 1
        self.metrics['current_channel'] = channel
        
        if self.channel_hopper and CHANNEL_FIX_AVAILABLE:
            try:
                # Let our safe hopper handle it
                success = self.channel_hopper.hop_to(channel)
                if not success:
                    self.diagnostics['channel_hop_failures'] += 1
                    logging.warning(f"[nexmon_stability] Channel hop to {channel} failed")
            except Exception as e:
                self.diagnostics['channel_hop_failures'] += 1
                if self.options.get('debug', False):
                    logging.debug(f"[nexmon_stability] Channel hop error: {e}")

    def on_wifi_update(self, agent, access_points):
        """Called when WiFi data updates - check health."""
        self.last_activity = time.time()
        
        # Check for firmware errors in dmesg
        if self._check_firmware_errors():
            logging.warning("[nexmon_stability] Firmware errors detected")
            self._trigger_recovery("firmware_error")

    def _detect_hardware(self):
        """Detect WiFi chip and firmware version."""
        try:
            result = subprocess.run(
                ['dmesg'], capture_output=True, text=True, timeout=5
            )
            output = result.stdout
            
            # Detect chip
            if 'BCM43455' in output or 'bcm43455' in output:
                self.chip_type = 'BCM43455C0'
            elif 'BCM43430' in output or 'bcm43430' in output:
                self.chip_type = 'BCM43430A1'
            elif 'BCM43436' in output or 'bcm43436' in output:
                self.chip_type = 'BCM43436B0'
            
            # Try to get firmware version
            fw_result = subprocess.run(
                ['cat', '/sys/module/brcmfmac/version'],
                capture_output=True, text=True, timeout=5
            )
            if fw_result.returncode == 0:
                self.firmware_version = fw_result.stdout.strip()
                
        except Exception as e:
            logging.debug(f"[nexmon_stability] Hardware detection error: {e}")

    def _check_firmware_errors(self):
        """Check dmesg for firmware errors."""
        try:
            result = subprocess.run(
                ['dmesg'], capture_output=True, text=True, timeout=5
            )
            
            # Get last 20 lines
            lines = result.stdout.strip().split('\n')[-20:]
            
            error_patterns = [
                'bus is down',
                'firmware fail',
                'card removed',
                'brcmf_sdio_firmware_callback: brcmf_attach failed',
                'brcmfmac: probe of',
                'sdio_cmd52_error',
            ]
            
            for line in lines:
                for pattern in error_patterns:
                    if pattern in line.lower():
                        return True
            
            return False
            
        except:
            return False

    def _trigger_recovery(self, reason):
        """Trigger WiFi recovery procedure."""
        with self._recovery_lock:
            if self._recovering:
                logging.debug("[nexmon_stability] Recovery already in progress")
                return
            
            # Check recovery rate limiting
            current_time = time.time()
            window = self.options.get('recovery_window', 3600)
            max_recoveries = self.options.get('max_recoveries', 5)
            
            # Clean old recovery times
            self.recovery_times = [t for t in self.recovery_times if current_time - t < window]
            
            if len(self.recovery_times) >= max_recoveries:
                logging.error(f"[nexmon_stability] Too many recoveries ({len(self.recovery_times)}) in {window}s window")
                return
            
            self._recovering = True
            self.recovery_times.append(current_time)
            self.stats['total_recoveries'] += 1
        
        # Run recovery in background
        thread = threading.Thread(target=self._do_recovery, args=(reason,))
        thread.daemon = True
        thread.start()

    def _do_recovery(self, reason):
        """Perform the actual recovery."""
        try:
            logging.info(f"[nexmon_stability] Starting recovery (reason: {reason})")
            
            delay = self.options.get('recovery_delay', 5)
            
            # Step 1: Bring interface down
            logging.info("[nexmon_stability] Step 1: Bringing interface down")
            subprocess.run(['ip', 'link', 'set', 'wlan0mon', 'down'], 
                          capture_output=True, timeout=10)
            time.sleep(delay)
            
            # Step 2: Remove driver module
            logging.info("[nexmon_stability] Step 2: Removing brcmfmac module")
            subprocess.run(['modprobe', '-r', 'brcmfmac'], 
                          capture_output=True, timeout=30)
            time.sleep(delay)
            
            # Step 3: Reload driver module
            logging.info("[nexmon_stability] Step 3: Reloading brcmfmac module")
            subprocess.run(['modprobe', 'brcmfmac'], 
                          capture_output=True, timeout=30)
            time.sleep(delay * 2)  # Give firmware time to load
            
            # Step 4: Wait for interface to appear
            logging.info("[nexmon_stability] Step 4: Waiting for interface")
            for i in range(10):
                result = subprocess.run(['ip', 'link', 'show', 'wlan0'],
                                       capture_output=True, timeout=5)
                if result.returncode == 0:
                    break
                time.sleep(1)
            
            # Step 5: Recreate monitor interface
            logging.info("[nexmon_stability] Step 5: Creating monitor interface")
            
            # Delete old mon interface if exists
            subprocess.run(['iw', 'dev', 'wlan0mon', 'del'], 
                          capture_output=True, timeout=10)
            time.sleep(1)
            
            # Create new monitor interface
            subprocess.run(['iw', 'phy', 'phy0', 'interface', 'add', 'wlan0mon', 
                          'type', 'monitor'], capture_output=True, timeout=10)
            time.sleep(1)
            
            # Bring it up
            subprocess.run(['ip', 'link', 'set', 'wlan0mon', 'up'],
                          capture_output=True, timeout=10)
            time.sleep(delay)
            
            # Step 6: Verify recovery
            result = subprocess.run(['iw', 'dev', 'wlan0mon', 'info'],
                                   capture_output=True, text=True, timeout=10)
            
            if result.returncode == 0 and 'monitor' in result.stdout.lower():
                logging.info("[nexmon_stability] Recovery successful!")
                self.stats['successful_recoveries'] += 1
                self.stats['last_recovery'] = datetime.now().isoformat()
                self.stats['uptime_since_recovery'] = 0
                
                # Reset blind counter
                self.blind_epochs = 0
                
                # Restart bettercap if available
                self._restart_bettercap()
            else:
                logging.error("[nexmon_stability] Recovery may have failed")
                self.stats['failed_recoveries'] += 1
                
        except Exception as e:
            logging.error(f"[nexmon_stability] Recovery error: {e}")
            self.stats['failed_recoveries'] += 1
        finally:
            with self._recovery_lock:
                self._recovering = False

    def _restart_bettercap(self):
        """Restart bettercap to reconnect to interface."""
        try:
            # Try systemctl restart
            result = subprocess.run(
                ['systemctl', 'restart', 'bettercap'],
                capture_output=True, timeout=30
            )
            if result.returncode == 0:
                logging.info("[nexmon_stability] Bettercap restarted")
            else:
                logging.warning("[nexmon_stability] Could not restart bettercap via systemctl")
        except Exception as e:
            logging.debug(f"[nexmon_stability] Bettercap restart error: {e}")

    def on_unload(self, ui):
        """Called when plugin is unloaded."""
        logging.info(f"[nexmon_stability] Unloading - Stats: {self.stats}")
        
        # Stop diagnostics collector
        self._stop_diagnostics_collector()
        
        # Clean up channel hopper
        if self.channel_hopper:
            try:
                self.channel_hopper.stop()
            except:
                pass

    # ========== DIAGNOSTICS METHODS ==========
    
    def _start_diagnostics_collector(self):
        """Start background diagnostics collection."""
        self._diag_stop = threading.Event()
        self._diag_thread = threading.Thread(target=self._diagnostics_loop, daemon=True)
        self._diag_thread.start()
    
    def _stop_diagnostics_collector(self):
        """Stop diagnostics collection."""
        if hasattr(self, '_diag_stop'):
            self._diag_stop.set()
            if hasattr(self, '_diag_thread'):
                self._diag_thread.join(timeout=2)
    
    def _diagnostics_loop(self):
        """Background loop for collecting diagnostics."""
        while not self._diag_stop.is_set():
            try:
                # Update interface status
                self._update_interface_status()
                
                # Check for new dmesg errors
                self._collect_dmesg_errors()
                
                # Update uptime
                if self.stats['last_recovery']:
                    try:
                        last = datetime.fromisoformat(self.stats['last_recovery'])
                        self.stats['uptime_since_recovery'] = (datetime.now() - last).total_seconds()
                    except:
                        pass
                
            except Exception as e:
                if self.options.get('debug', False):
                    logging.debug(f"[nexmon_stability] Diagnostics error: {e}")
            
            self._diag_stop.wait(10)  # Collect every 10 seconds
    
    def _update_interface_status(self):
        """Update interface status metrics."""
        try:
            # Check if driver loaded
            result = subprocess.run(['lsmod'], capture_output=True, text=True, timeout=5)
            self.metrics['driver_loaded'] = 'brcmfmac' in result.stdout
            
            # Check interface up
            result = subprocess.run(['ip', 'link', 'show', 'wlan0mon'], 
                                   capture_output=True, text=True, timeout=5)
            self.metrics['interface_up'] = result.returncode == 0 and 'UP' in result.stdout
            
            # Check monitor mode
            result = subprocess.run(['iw', 'dev', 'wlan0mon', 'info'],
                                   capture_output=True, text=True, timeout=5)
            self.metrics['monitor_mode'] = 'monitor' in result.stdout.lower()
            
            # Get current channel
            if result.returncode == 0:
                for line in result.stdout.split('\n'):
                    if 'channel' in line.lower():
                        parts = line.strip().split()
                        for i, p in enumerate(parts):
                            if p.lower() == 'channel' and i + 1 < len(parts):
                                try:
                                    self.metrics['current_channel'] = int(parts[i + 1])
                                except:
                                    pass
                                break
        except:
            pass
    
    def _collect_dmesg_errors(self):
        """Collect recent dmesg errors."""
        try:
            result = subprocess.run(['dmesg', '-T'], capture_output=True, text=True, timeout=5)
            lines = result.stdout.strip().split('\n')[-100:]
            
            error_patterns = [
                'bus is down', 'firmware fail', 'timeout', 'card removed',
                'brcmf_sdio', 'sdio_cmd52_error', '-110', 'probe failed'
            ]
            
            errors = []
            for line in lines:
                line_lower = line.lower()
                if any(p in line_lower for p in error_patterns):
                    if 'brcm' in line_lower or 'wlan' in line_lower or 'sdio' in line_lower:
                        errors.append(line.strip())
            
            # Keep last 50 unique errors
            for err in errors:
                if err not in self.diagnostics['dmesg_errors']:
                    self.diagnostics['dmesg_errors'].append(err)
            self.diagnostics['dmesg_errors'] = self.diagnostics['dmesg_errors'][-50:]
            
        except:
            pass
    
    def _get_full_status(self):
        """Get complete status for API."""
        return {
            'plugin_version': self.__version__,
            'chip_type': self.chip_type,
            'firmware_version': self.firmware_version,
            'ready': self.ready,
            'recovering': self._recovering,
            'blind_epochs': self.blind_epochs,
            'stats': self.stats,
            'metrics': self.metrics,
            'channel_hopper_available': CHANNEL_FIX_AVAILABLE,
        }
    
    def _get_diagnostics_report(self):
        """Generate full diagnostics report."""
        return {
            'timestamp': datetime.now().isoformat(),
            'hardware': {
                'chip': self.chip_type,
                'firmware': self.firmware_version,
            },
            'stats': self.stats,
            'metrics': self.metrics,
            'diagnostics': {
                'start_time': self.diagnostics['start_time'],
                'total_epochs': self.diagnostics['total_epochs'],
                'total_aps_seen': self.diagnostics['total_aps_seen'],
                'unique_aps_count': len(self.diagnostics['unique_aps']),
                'channel_hop_count': self.diagnostics['channel_hop_count'],
                'channel_hop_failures': self.diagnostics['channel_hop_failures'],
                'channel_hop_success_rate': (
                    (self.diagnostics['channel_hop_count'] - self.diagnostics['channel_hop_failures']) 
                    / max(1, self.diagnostics['channel_hop_count']) * 100
                ),
                'dmesg_error_count': len(self.diagnostics['dmesg_errors']),
            },
            'recent_errors': self.diagnostics['dmesg_errors'][-10:],
            'signal_history': list(self.diagnostics['signal_history'])[-20:],
            'recovery_history': list(self.diagnostics['recovery_history']),
        }
    
    def _get_dmesg_errors(self):
        """Get collected dmesg errors."""
        return {
            'errors': self.diagnostics['dmesg_errors'],
            'count': len(self.diagnostics['dmesg_errors']),
        }
    
    def _get_interface_info(self):
        """Get detailed interface information."""
        info = {
            'wlan0mon': {},
            'wlan0': {},
            'driver': {},
        }
        
        try:
            # wlan0mon
            result = subprocess.run(['iw', 'dev', 'wlan0mon', 'info'],
                                   capture_output=True, text=True, timeout=5)
            if result.returncode == 0:
                info['wlan0mon']['raw'] = result.stdout
                info['wlan0mon']['exists'] = True
            else:
                info['wlan0mon']['exists'] = False
            
            # wlan0
            result = subprocess.run(['iw', 'dev', 'wlan0', 'info'],
                                   capture_output=True, text=True, timeout=5)
            if result.returncode == 0:
                info['wlan0']['raw'] = result.stdout
                info['wlan0']['exists'] = True
            else:
                info['wlan0']['exists'] = False
            
            # Driver info
            result = subprocess.run(['modinfo', 'brcmfmac'],
                                   capture_output=True, text=True, timeout=5)
            if result.returncode == 0:
                for line in result.stdout.split('\n'):
                    if ':' in line:
                        key, _, val = line.partition(':')
                        info['driver'][key.strip()] = val.strip()
                        
        except Exception as e:
            info['error'] = str(e)
        
        return info


# Web UI Template for Diagnostics
DIAGNOSTICS_HTML = '''
<!DOCTYPE html>
<html>
<head>
    <title>Nexmon Stability - Diagnostics</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        * { box-sizing: border-box; }
        body { 
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, monospace;
            background: #1a1a2e; color: #eee; margin: 0; padding: 20px;
        }
        .container { max-width: 900px; margin: 0 auto; }
        h1 { color: #00ff88; margin-bottom: 5px; }
        .subtitle { color: #888; margin-bottom: 20px; }
        .card {
            background: #16213e; border-radius: 8px; padding: 15px;
            margin-bottom: 15px; border: 1px solid #0f3460;
        }
        .card h2 { margin-top: 0; color: #00d4ff; font-size: 14px; text-transform: uppercase; }
        .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 15px; }
        .metric { text-align: center; padding: 10px; background: #0f3460; border-radius: 5px; }
        .metric .value { font-size: 28px; font-weight: bold; color: #00ff88; }
        .metric .label { font-size: 12px; color: #888; }
        .status-ok { color: #00ff88; }
        .status-warn { color: #ffaa00; }
        .status-error { color: #ff4444; }
        table { width: 100%; border-collapse: collapse; }
        th, td { padding: 8px; text-align: left; border-bottom: 1px solid #0f3460; }
        th { color: #00d4ff; }
        .btn {
            background: #0f3460; color: #fff; border: none; padding: 10px 20px;
            border-radius: 5px; cursor: pointer; margin-right: 10px;
        }
        .btn:hover { background: #00d4ff; }
        .btn-danger { background: #cc3300; }
        .btn-danger:hover { background: #ff4444; }
        pre { background: #0a0a15; padding: 10px; border-radius: 5px; overflow-x: auto; font-size: 11px; }
        .log-line { margin: 2px 0; }
        .refresh { float: right; font-size: 12px; color: #666; }
    </style>
</head>
<body>
    <div class="container">
        <h1>🛡️ Nexmon Stability</h1>
        <p class="subtitle">Real-time WiFi firmware diagnostics</p>
        
        <div class="card">
            <h2>Status</h2>
            <div class="grid">
                <div class="metric">
                    <div class="value" id="status">--</div>
                    <div class="label">Status</div>
                </div>
                <div class="metric">
                    <div class="value" id="chip">--</div>
                    <div class="label">WiFi Chip</div>
                </div>
                <div class="metric">
                    <div class="value" id="channel">--</div>
                    <div class="label">Channel</div>
                </div>
                <div class="metric">
                    <div class="value" id="blind">--</div>
                    <div class="label">Blind Epochs</div>
                </div>
            </div>
        </div>
        
        <div class="card">
            <h2>Statistics</h2>
            <div class="grid">
                <div class="metric">
                    <div class="value" id="recoveries">--</div>
                    <div class="label">Total Recoveries</div>
                </div>
                <div class="metric">
                    <div class="value" id="success_rate">--</div>
                    <div class="label">Success Rate</div>
                </div>
                <div class="metric">
                    <div class="value" id="epochs">--</div>
                    <div class="label">Total Epochs</div>
                </div>
                <div class="metric">
                    <div class="value" id="uptime">--</div>
                    <div class="label">Uptime (min)</div>
                </div>
            </div>
        </div>
        
        <div class="card">
            <h2>Interface Status</h2>
            <table>
                <tr><th>Property</th><th>Value</th></tr>
                <tr><td>Driver Loaded</td><td id="driver_loaded">--</td></tr>
                <tr><td>Interface Up</td><td id="interface_up">--</td></tr>
                <tr><td>Monitor Mode</td><td id="monitor_mode">--</td></tr>
                <tr><td>Channel Hop Success</td><td id="hop_success">--</td></tr>
            </table>
        </div>
        
        <div class="card">
            <h2>Recent Errors <span class="refresh" id="error_count">0 errors</span></h2>
            <pre id="errors">No errors collected</pre>
        </div>
        
        <div class="card">
            <h2>Actions</h2>
            <button class="btn" onclick="refreshData()">🔄 Refresh</button>
            <button class="btn btn-danger" onclick="triggerRecovery()">⚠️ Force Recovery</button>
            <button class="btn" onclick="downloadReport()">📥 Download Report</button>
        </div>
    </div>
    
    <script>
        function updateUI(data) {
            // Status
            const status = data.recovering ? 'RECOVERING' : (data.blind_epochs > 0 ? 'BLIND' : 'OK');
            const statusEl = document.getElementById('status');
            statusEl.textContent = status;
            statusEl.className = 'value ' + (status === 'OK' ? 'status-ok' : (status === 'BLIND' ? 'status-warn' : 'status-error'));
            
            document.getElementById('chip').textContent = data.chip_type || '--';
            document.getElementById('channel').textContent = data.metrics?.current_channel || '--';
            document.getElementById('blind').textContent = data.blind_epochs || '0';
            
            // Stats
            document.getElementById('recoveries').textContent = data.stats?.total_recoveries || '0';
            const sr = data.stats?.total_recoveries > 0 
                ? Math.round(data.stats.successful_recoveries / data.stats.total_recoveries * 100) + '%'
                : '100%';
            document.getElementById('success_rate').textContent = sr;
            document.getElementById('uptime').textContent = Math.round((data.stats?.uptime_since_recovery || 0) / 60);
            
            // Interface
            document.getElementById('driver_loaded').innerHTML = data.metrics?.driver_loaded 
                ? '<span class="status-ok">✓ Loaded</span>' : '<span class="status-error">✗ Not Loaded</span>';
            document.getElementById('interface_up').innerHTML = data.metrics?.interface_up
                ? '<span class="status-ok">✓ Up</span>' : '<span class="status-error">✗ Down</span>';
            document.getElementById('monitor_mode').innerHTML = data.metrics?.monitor_mode
                ? '<span class="status-ok">✓ Active</span>' : '<span class="status-error">✗ Inactive</span>';
        }
        
        function updateDiagnostics(data) {
            document.getElementById('epochs').textContent = data.diagnostics?.total_epochs || '0';
            document.getElementById('hop_success').textContent = 
                (data.diagnostics?.channel_hop_success_rate?.toFixed(1) || '100') + '%';
            
            // Errors
            const errors = data.recent_errors || [];
            document.getElementById('error_count').textContent = errors.length + ' errors';
            document.getElementById('errors').textContent = errors.length > 0 
                ? errors.join('\\n') : 'No errors collected';
        }
        
        async function refreshData() {
            try {
                const [status, diag] = await Promise.all([
                    fetch('api/status').then(r => r.json()),
                    fetch('api/diagnostics').then(r => r.json())
                ]);
                updateUI(status);
                updateDiagnostics(diag);
            } catch (e) {
                console.error('Refresh failed:', e);
            }
        }
        
        async function triggerRecovery() {
            if (!confirm('Trigger WiFi recovery? This will restart the WiFi interface.')) return;
            try {
                await fetch('api/recovery', { method: 'POST' });
                alert('Recovery triggered');
                setTimeout(refreshData, 5000);
            } catch (e) {
                alert('Failed to trigger recovery');
            }
        }
        
        async function downloadReport() {
            try {
                const data = await fetch('api/diagnostics').then(r => r.json());
                const blob = new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' });
                const url = URL.createObjectURL(blob);
                const a = document.createElement('a');
                a.href = url;
                a.download = 'nexmon_diagnostics_' + new Date().toISOString().split('T')[0] + '.json';
                a.click();
            } catch (e) {
                alert('Failed to download report');
            }
        }
        
        // Auto-refresh every 5 seconds
        refreshData();
        setInterval(refreshData, 5000);
    </script>
</body>
</html>
'''
