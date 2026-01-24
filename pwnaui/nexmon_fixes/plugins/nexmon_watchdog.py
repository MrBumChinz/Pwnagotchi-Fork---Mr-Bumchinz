"""
Nexmon Watchdog Plugin for Pwnagotchi
=====================================

Enhanced watchdog plugin that monitors for both the "blindness bug"
(channel hop failures) and firmware crashes, with proper recovery.

This plugin replaces the default watchdog behavior with more sophisticated
error detection and recovery procedures.

Installation:
1. Copy to /etc/pwnagotchi/custom-plugins/nexmon_watchdog.py
2. Enable in config.toml:
   
   main.custom_plugins = "/etc/pwnagotchi/custom-plugins/"
   main.plugins.nexmon_watchdog.enabled = true
   main.plugins.nexmon_watchdog.blind_epochs = 10
   main.plugins.nexmon_watchdog.crash_recovery = true

Based on:
- https://github.com/seemoo-lab/nexmon/issues/335
- https://github.com/evilsocket/pwnagotchi/issues/267
"""

import logging
import os
import re
import subprocess
import time
from threading import Thread, Lock

import pwnagotchi.plugins as plugins
from pwnagotchi import reboot


class NexmonWatchdog(plugins.Plugin):
    __author__ = 'pwnagotchi-fixes'
    __version__ = '1.0.0'
    __license__ = 'GPL3'
    __description__ = 'Enhanced watchdog for Nexmon-related issues'

    def __init__(self):
        self.ready = False
        self.lock = Lock()
        self.options = {}
        
        # Counters
        self.blind_epochs = 0
        self.channel_errors = 0
        self.crash_count = 0
        self.recovery_attempts = 0
        self.last_check_time = time.time()
        
        # Patterns for error detection
        self.channel_error_pattern = re.compile(
            r'brcmf_cfg80211_nexmon_set_channel.*?Set Channel failed.*?-110'
        )
        self.crash_pattern = re.compile(
            r'Firmware has halted or crashed'
        )
        self.wifi_error_pattern = re.compile(
            r'wifi error while hopping to channel'
        )
        self.no_device_pattern = re.compile(
            r'No such device|error 400: could not find interface'
        )

    def on_loaded(self):
        """Called when the plugin is loaded."""
        logging.info('[nexmon_watchdog] Plugin loaded')
        
    def on_config_changed(self, config):
        """Called when configuration changes."""
        self.options = config.get('main', {}).get('plugins', {}).get('nexmon_watchdog', {})
        
        self.max_blind_epochs = self.options.get('blind_epochs', 10)
        self.crash_recovery = self.options.get('crash_recovery', True)
        self.soft_recovery_first = self.options.get('soft_recovery_first', True)
        self.max_recovery_attempts = self.options.get('max_recovery_attempts', 3)
        self.check_interval = self.options.get('check_interval', 30)
        self.reboot_on_failure = self.options.get('reboot_on_failure', True)
        
        logging.info(f'[nexmon_watchdog] Configuration: max_blind_epochs={self.max_blind_epochs}, '
                     f'crash_recovery={self.crash_recovery}, '
                     f'soft_recovery_first={self.soft_recovery_first}')
        
    def on_ready(self, agent):
        """Called when pwnagotchi is ready."""
        self.ready = True
        logging.info('[nexmon_watchdog] Ready')
        
    def _check_journalctl(self, since='60s'):
        """Check journalctl for recent errors."""
        errors = {
            'channel_errors': 0,
            'crashes': 0,
            'wifi_errors': 0,
            'device_missing': 0
        }
        
        try:
            result = subprocess.run(
                ['journalctl', '-k', '--since', f'{since} ago', '--no-pager'],
                capture_output=True,
                text=True,
                timeout=10
            )
            output = result.stdout + result.stderr
            
            errors['channel_errors'] = len(self.channel_error_pattern.findall(output))
            errors['crashes'] = len(self.crash_pattern.findall(output))
            errors['wifi_errors'] = len(self.wifi_error_pattern.findall(output))
            errors['device_missing'] = len(self.no_device_pattern.findall(output))
            
        except Exception as e:
            logging.error(f'[nexmon_watchdog] Error checking journalctl: {e}')
            
        return errors
    
    def _check_dmesg(self):
        """Check dmesg for errors."""
        errors = {
            'channel_errors': 0,
            'crashes': 0
        }
        
        try:
            result = subprocess.run(
                ['dmesg'],
                capture_output=True,
                text=True,
                timeout=10
            )
            output = result.stdout
            
            errors['channel_errors'] = len(self.channel_error_pattern.findall(output))
            errors['crashes'] = len(self.crash_pattern.findall(output))
            
        except Exception as e:
            logging.error(f'[nexmon_watchdog] Error checking dmesg: {e}')
            
        return errors
    
    def _check_interface(self, interface='wlan0mon'):
        """Check if the monitor interface exists."""
        try:
            result = subprocess.run(
                ['ip', 'link', 'show', interface],
                capture_output=True,
                timeout=5
            )
            return result.returncode == 0
        except:
            return False
    
    def _reload_driver(self):
        """Attempt to reload the brcmfmac driver."""
        logging.info('[nexmon_watchdog] Attempting driver reload')
        
        try:
            # Remove driver
            subprocess.run(['rmmod', 'brcmfmac'], timeout=10, check=False)
            time.sleep(2)
            
            # Reload driver
            subprocess.run(['modprobe', 'brcmutil'], timeout=10, check=False)
            subprocess.run(['modprobe', 'brcmfmac'], timeout=10, check=False)
            time.sleep(3)
            
            # Check if interface came back
            return self._check_interface('wlan0')
            
        except Exception as e:
            logging.error(f'[nexmon_watchdog] Driver reload failed: {e}')
            return False
    
    def _setup_monitor_mode(self, interface='wlan0mon', base_interface='wlan0'):
        """Setup monitor mode with proper delays."""
        logging.info(f'[nexmon_watchdog] Setting up monitor mode on {interface}')
        
        try:
            # Unblock RF
            subprocess.run(['rfkill', 'unblock', 'all'], timeout=5, check=False)
            time.sleep(1)
            
            # Bring up base interface
            subprocess.run(['ip', 'link', 'set', base_interface, 'up'], timeout=5, check=False)
            time.sleep(3)  # Critical delay
            
            # Disable power save
            subprocess.run(['iw', 'dev', base_interface, 'set', 'power_save', 'off'], 
                           timeout=5, check=False)
            time.sleep(1)
            
            # Get phy name
            result = subprocess.run(
                ['iw', 'dev', base_interface, 'info'],
                capture_output=True,
                text=True,
                timeout=5
            )
            phy = None
            for line in result.stdout.split('\n'):
                if 'wiphy' in line:
                    phy = f"phy{line.split()[-1]}"
                    break
            
            if not phy:
                logging.error('[nexmon_watchdog] Could not determine phy')
                return False
            
            # Create monitor interface
            subprocess.run(['iw', 'phy', phy, 'interface', 'add', interface, 'type', 'monitor'],
                           timeout=5, check=False)
            time.sleep(2)  # Critical delay
            
            # Unblock again
            subprocess.run(['rfkill', 'unblock', 'all'], timeout=5, check=False)
            
            # Bring down base interface
            subprocess.run(['ip', 'link', 'set', base_interface, 'down'], timeout=5, check=False)
            time.sleep(1)
            
            # Bring up monitor interface
            subprocess.run(['ip', 'link', 'set', interface, 'up'], timeout=5, check=False)
            time.sleep(1)
            
            # Disable power save on monitor
            subprocess.run(['iw', 'dev', interface, 'set', 'power_save', 'off'],
                           timeout=5, check=False)
            
            return self._check_interface(interface)
            
        except Exception as e:
            logging.error(f'[nexmon_watchdog] Monitor setup failed: {e}')
            return False
    
    def _perform_recovery(self):
        """Perform full recovery procedure."""
        with self.lock:
            self.recovery_attempts += 1
            
            if self.recovery_attempts > self.max_recovery_attempts:
                logging.error('[nexmon_watchdog] Max recovery attempts reached')
                if self.reboot_on_failure:
                    logging.info('[nexmon_watchdog] Initiating reboot')
                    reboot()
                return False
            
            logging.info(f'[nexmon_watchdog] Recovery attempt {self.recovery_attempts}/{self.max_recovery_attempts}')
            
            # Try soft recovery first
            if self.soft_recovery_first:
                if self._reload_driver():
                    time.sleep(2)
                    if self._setup_monitor_mode():
                        logging.info('[nexmon_watchdog] Soft recovery successful')
                        self.recovery_attempts = 0
                        return True
            
            # If soft recovery failed, just reload driver and let pwnagotchi handle it
            logging.info('[nexmon_watchdog] Soft recovery failed, attempting harder reset')
            self._reload_driver()
            time.sleep(5)
            
            # Final check
            if self._check_interface('wlan0'):
                logging.info('[nexmon_watchdog] Recovery completed')
                self.recovery_attempts = 0
                return True
            
            return False
    
    def on_epoch(self, agent, epoch, epoch_data):
        """Called at the end of each epoch."""
        if not self.ready:
            return
            
        current_time = time.time()
        
        # Don't check too frequently
        if current_time - self.last_check_time < self.check_interval:
            return
            
        self.last_check_time = current_time
        
        # Check for errors
        recent_errors = self._check_journalctl('60s')
        
        # Check for firmware crash (highest priority)
        if recent_errors['crashes'] > 0:
            logging.warning(f'[nexmon_watchdog] Firmware crash detected!')
            self.crash_count += 1
            if self.crash_recovery:
                self._perform_recovery()
            return
        
        # Check for blindness (channel hop failures)
        if recent_errors['channel_errors'] > 0 or recent_errors['wifi_errors'] > 0:
            self.blind_epochs += 1
            logging.warning(f'[nexmon_watchdog] Blind epoch {self.blind_epochs}/{self.max_blind_epochs}')
            
            if self.blind_epochs >= self.max_blind_epochs:
                logging.error('[nexmon_watchdog] Too many blind epochs, initiating recovery')
                self.blind_epochs = 0
                self._perform_recovery()
        else:
            # Reset counter if no errors
            if self.blind_epochs > 0:
                logging.info('[nexmon_watchdog] Blindness cleared')
            self.blind_epochs = 0
        
        # Check if interface disappeared
        if recent_errors['device_missing'] > 0:
            logging.warning('[nexmon_watchdog] Interface may be missing')
            if not self._check_interface('wlan0mon'):
                logging.error('[nexmon_watchdog] Monitor interface missing!')
                self._perform_recovery()
    
    def on_ui_update(self, ui):
        """Update UI with watchdog status (optional)."""
        if self.blind_epochs > 0:
            ui.set('status', f'Blind:{self.blind_epochs}/{self.max_blind_epochs}')
    
    def on_unload(self, ui):
        """Called when plugin is unloaded."""
        logging.info('[nexmon_watchdog] Unloaded')
