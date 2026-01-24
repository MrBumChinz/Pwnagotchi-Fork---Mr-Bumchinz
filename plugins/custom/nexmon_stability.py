"""
Nexmon Stability Plugin for PwnaUI/Pwnagotchi - INTEGRATED VERSION

Combines the best of:
- nexmon_stability.py (our custom diagnostics, metrics, blind detection, web UI)
- fix_services.py (jayofelony's proven error patterns, monstart/monstop, wifi.recon)

Features:
- 7 regex patterns for error detection (from fix_services)
- External adapter auto-disable (from fix_services)
- Patched driver verification (from fix_services)
- on_bcap_sys_log real-time events (from fix_services)
- monstart/monstop recovery (from fix_services)
- wifi.recon off/on (from fix_services)
- Blind epoch detection (from nexmon_stability)
- Full diagnostics web UI (from nexmon_stability)
- Metrics and statistics (from nexmon_stability)
- SafeChannelHopper (from nexmon_stability)

Author: PwnaUI Team
License: MIT
"""

import os
import sys
import re
import time
import json
import hashlib
import logging
import subprocess
import threading
from io import TextIOWrapper
from datetime import datetime
from collections import deque

import pwnagotchi
import pwnagotchi.plugins as plugins
import pwnagotchi.ui.faces as faces
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
    __author__ = 'PwnaUI + jayofelony'
    __version__ = '3.0.0'
    __license__ = 'MIT'
    __description__ = 'Nexmon firmware stability - integrated fix_services + nexmon_stability'
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
        'show_status': True,
        'debug': False,
    }

    def __init__(self):
        self.ready = False
        self.agent = None
        self.options = {}

        # === FROM fix_services.py - Error patterns ===
        self.pattern1 = re.compile(r'ieee80211 phy0: brcmf_cfg80211_add_iface: iface validation failed: err=-95')
        self.pattern2 = re.compile(r'wifi error while hopping to channel')
        self.pattern3 = re.compile(r'Firmware has halted or crashed')
        self.pattern4 = re.compile(r'error 400: could not find interface wlan0mon')
        self.pattern5 = re.compile(r'fatal error: concurrent map iteration and map write')
        self.pattern6 = re.compile(r'panic: runtime error')
        self.pattern7 = re.compile(r'ieee80211 phy0: _brcmf_set_multicast_list: Setting allmulti failed, -110')

        # === FROM fix_services.py - State ===
        self.isReloadingMon = False
        self.LASTTRY = 0
        self.is_disabled = False  # Set by _check_external_adapter()

        # === FROM nexmon_stability.py - State ===
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
            'wifi_recon_flips': 0,
            'last_recovery': None,
            'uptime_since_recovery': 0,
        }

        # Diagnostics
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
        }

        # Metrics
        self.metrics = {
            'current_channel': None,
            'interface_up': False,
            'monitor_mode': False,
            'driver_loaded': False,
        }

    # =========================================================================
    # FROM fix_services.py - External adapter detection
    # =========================================================================
    def _check_external_adapter(self):
        """
        Check if external WiFi adapter is used instead of onboard brcmfmac.
        Returns True if external (plugin should disable), False otherwise.
        """
        try:
            cmd_output = subprocess.check_output("ls /sys/class/net/", shell=True, text=True)
            interfaces = cmd_output.strip().split('\n')

            if 'wlan0' in interfaces:
                try:
                    driver_path = "/sys/class/net/wlan0/device/driver"
                    if os.path.exists(driver_path):
                        driver_link = os.readlink(driver_path)
                        driver_name = os.path.basename(driver_link)

                        logging.info(f"[nexmon_stability] Detected WiFi driver: {driver_name}")

                        if driver_name != "brcmfmac":
                            logging.info(f"[nexmon_stability] External adapter ({driver_name}). Plugin disabled.")
                            return True
                        else:
                            logging.info("[nexmon_stability] Onboard brcmfmac detected. Plugin active.")
                            return False
                    else:
                        # Fallback to lsmod
                        lsmod_output = subprocess.check_output("lsmod | grep brcmfmac", shell=True, text=True)
                        if lsmod_output.strip():
                            logging.info("[nexmon_stability] brcmfmac via lsmod. Plugin active.")
                            return False
                        else:
                            logging.info("[nexmon_stability] brcmfmac not found. Plugin disabled.")
                            return True
                except subprocess.CalledProcessError:
                    logging.info("[nexmon_stability] brcmfmac not found. Plugin disabled.")
                    return True
                except Exception as e:
                    logging.warning(f"[nexmon_stability] Driver check error: {e}. Disabling.")
                    return True
            else:
                logging.warning("[nexmon_stability] wlan0 not found. Plugin disabled.")
                return True
        except Exception as e:
            logging.error(f"[nexmon_stability] Adapter detection error: {e}. Disabling.")
            return True

    # =========================================================================
    # FROM fix_services.py - Patched driver verification
    # =========================================================================
    def _ensure_patched_driver(self):
        """
        Ensures patched brcmfmac driver with 5000ms DCMD_RESP_TIMEOUT is installed.
        Stock driver has 2500ms which causes -110 ETIMEDOUT blindness bug.
        """
        try:
            patched_driver = "/home/pi/nexmon/patches/driver/brcmfmac_6.6.y-nexmon/brcmfmac.ko"
            kernel_version = subprocess.check_output("uname -r", shell=True, text=True).strip()
            installed_driver = f"/lib/modules/{kernel_version}/kernel/drivers/net/wireless/broadcom/brcm80211/brcmfmac/brcmfmac.ko"

            if not os.path.exists(patched_driver):
                logging.debug(f"[nexmon_stability] Patched driver not found at {patched_driver}")
                return

            if not os.path.exists(installed_driver):
                logging.warning(f"[nexmon_stability] Installed driver not found at {installed_driver}")
                return

            def md5sum(filepath):
                with open(filepath, 'rb') as f:
                    return hashlib.md5(f.read()).hexdigest()

            patched_md5 = md5sum(patched_driver)
            installed_md5 = md5sum(installed_driver)

            if patched_md5 == installed_md5:
                logging.info("[nexmon_stability] Patched brcmfmac driver verified (5000ms timeout)")
            else:
                logging.warning("[nexmon_stability] Driver mismatch! Installing patched driver...")
                logging.info(f"[nexmon_stability] Patched: {patched_md5}, Installed: {installed_md5}")

                try:
                    backup_path = installed_driver + ".stock"
                    if not os.path.exists(backup_path):
                        subprocess.check_output(f"sudo cp {installed_driver} {backup_path}", shell=True)
                        logging.info(f"[nexmon_stability] Backed up stock driver to {backup_path}")

                    subprocess.check_output(f"sudo cp {patched_driver} {installed_driver}", shell=True)
                    logging.info(f"[nexmon_stability] Copied patched driver")

                    subprocess.check_output("sudo depmod -a", shell=True)
                    logging.info("[nexmon_stability] Rebuilt module dependencies")

                    logging.info("[nexmon_stability] Reloading brcmfmac with patched version...")
                    subprocess.check_output("sudo modprobe -r brcmfmac brcmutil 2>/dev/null || true", shell=True)
                    time.sleep(1)
                    subprocess.check_output("sudo modprobe brcmfmac", shell=True)
                    logging.info("[nexmon_stability] Patched driver loaded (5000ms timeout)")

                except Exception as e:
                    logging.error(f"[nexmon_stability] Failed to install patched driver: {e}")

        except Exception as e:
            logging.debug(f"[nexmon_stability] Patched driver check error: {e}")

    # =========================================================================
    # PLUGIN LIFECYCLE
    # =========================================================================
    def on_loaded(self):
        """Called when plugin is loaded."""
        # Check for external adapter first
        self.is_disabled = self._check_external_adapter()

        if self.is_disabled:
            logging.info("[nexmon_stability] Plugin loaded but DISABLED (external adapter)")
            return

        logging.info("[nexmon_stability] Plugin loaded")

        # Verify patched driver
        self._ensure_patched_driver()

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
        if self.is_disabled:
            return

        self.agent = agent
        logging.info(f"[nexmon_stability] Ready - Chip: {self.chip_type}, FW: {self.firmware_version}")

        # Verify interface is up (from fix_services)
        try:
            cmd_output = subprocess.check_output("ip link show wlan0mon", shell=True)
            if ",UP," in str(cmd_output):
                logging.debug("[nexmon_stability] wlan0mon is up")
            else:
                logging.warning("[nexmon_stability] wlan0mon not UP, attempting recovery")
                self._tryTurningItOffAndOnAgain(agent)
        except Exception as e:
            logging.error(f"[nexmon_stability] wlan0mon check failed: {e}")
            self._tryTurningItOffAndOnAgain(agent)

    # =========================================================================
    # FROM fix_services.py - Bettercap syslog event handler
    # =========================================================================
    def on_bcap_sys_log(self, agent, event):
        """Real-time bettercap syslog events."""
        if self.is_disabled:
            return

        if re.search('wifi error while hopping to channel', event['data']['Message']):
            logging.debug(f"[nexmon_stability] SYSLOG MATCH: {event['data']['Message']}")
            logging.info("[nexmon_stability] Restarting wifi.recon from syslog event")

            try:
                result = agent.run("wifi.recon off; wifi.recon on")
                if result.get("success"):
                    logging.info("[nexmon_stability] wifi.recon flip: success!")
                    self.stats['wifi_recon_flips'] += 1
                    if hasattr(agent, 'view'):
                        display = agent.view()
                        if display:
                            display.update(force=True, new_data={"status": "Wifi recon flipped!", "face": faces.COOL})
                else:
                    logging.warning(f"[nexmon_stability] wifi.recon flip FAILED: {result}")
                    self._tryTurningItOffAndOnAgain(agent)
            except Exception as e:
                logging.error(f"[nexmon_stability] SYSLOG wifi.recon flip fail: {e}")
                self._tryTurningItOffAndOnAgain(agent)

    # =========================================================================
    # EPOCH HANDLER - Combined from both plugins
    # =========================================================================
    def on_epoch(self, agent, epoch, epoch_data):
        """Called each epoch - check logs AND blind detection."""
        if self.is_disabled:
            return

        self.agent = agent
        self.diagnostics['total_epochs'] += 1

        # === BLIND DETECTION (from nexmon_stability) ===
        try:
            aps = agent.get_access_points() if hasattr(agent, 'get_access_points') else []
            current_aps = len(aps)
            self.diagnostics['total_aps_seen'] += current_aps

            for ap in aps:
                if hasattr(ap, 'bssid'):
                    self.diagnostics['unique_aps'].add(ap.bssid)
        except:
            current_aps = 0

        # Track signal history
        self.diagnostics['signal_history'].append({
            'time': datetime.now().isoformat(),
            'epoch': epoch,
            'aps': current_aps,
            'blind': current_aps == 0
        })

        if current_aps == 0:
            self.blind_epochs += 1
            self.stats['total_blind_epochs'] += 1

            max_blind = self.options.get('blind_epochs', 10)
            if self.blind_epochs >= max_blind:
                logging.warning(f"[nexmon_stability] Blind for {self.blind_epochs} epochs")
                self._tryTurningItOffAndOnAgain(agent)
        else:
            if self.blind_epochs > 0:
                logging.info(f"[nexmon_stability] Vision restored after {self.blind_epochs} blind epochs")
            self.blind_epochs = 0
            self.last_aps = current_aps
            self.last_activity = time.time()

        # === JOURNALCTL LOG CHECKING (from fix_services) ===
        # Don't check if we ran a reset recently
        if time.time() - self.LASTTRY > 180:
            self._check_logs_for_errors(agent)

    def _check_logs_for_errors(self, agent):
        """Check journalctl for known error patterns (from fix_services)."""
        try:
            # Get kernel logs
            last_lines = ''.join(list(TextIOWrapper(subprocess.Popen(
                ['journalctl', '-n10', '-k'], stdout=subprocess.PIPE).stdout))[-10:])

            # Get general logs
            other_last_lines = ''.join(list(TextIOWrapper(subprocess.Popen(
                ['journalctl', '-n10'], stdout=subprocess.PIPE).stdout))[-10:])

            # Get pwnagotchi log
            pwnlog_lines = ''
            try:
                pwnlog_lines = ''.join(list(TextIOWrapper(subprocess.Popen(
                    ['tail', '-n10', '/etc/pwnagotchi/log/pwnagotchi.log'],
                    stdout=subprocess.PIPE).stdout))[-10:])
            except:
                pass

            display = agent.view() if hasattr(agent, 'view') else None

            # Pattern 1: iface validation failed
            if len(self.pattern1.findall(last_lines)) >= 1:
                logging.info("[nexmon_stability] Pattern1: iface validation failed")
                subprocess.check_output("monstop", shell=True)
                subprocess.check_output("monstart", shell=True)
                if display:
                    display.set('status', 'Wifi stuck. Restarting.')
                    display.update(force=True)
                pwnagotchi.restart("AUTO")

            # Pattern 2: wifi error hopping (5+ occurrences)
            elif len(self.pattern2.findall(other_last_lines)) >= 5:
                logging.info("[nexmon_stability] Pattern2: channel hop errors")
                if display:
                    display.set('status', 'Channel hop stuck. Flipping recon.')
                    display.update(force=True)

                try:
                    result = agent.run("wifi.recon off; wifi.recon on")
                    if result.get("success"):
                        logging.info("[nexmon_stability] wifi.recon flip success")
                        self.stats['wifi_recon_flips'] += 1
                        if display:
                            display.update(force=True, new_data={"status": "Wifi recon flipped!", "face": faces.COOL})
                    else:
                        logging.warning(f"[nexmon_stability] wifi.recon flip failed: {result}")
                except Exception as e:
                    logging.error(f"[nexmon_stability] wifi.recon flip error: {e}")

            # Pattern 3: Firmware halted/crashed
            elif len(self.pattern3.findall(other_last_lines)) >= 1:
                logging.info("[nexmon_stability] Pattern3: Firmware crashed")
                if display:
                    display.set('status', 'Firmware crashed. Restarting wlan0mon.')
                    display.update(force=True)
                try:
                    subprocess.check_output("monstart", shell=True)
                except Exception as e:
                    logging.error(f"[nexmon_stability] monstart failed: {e}")

            # Pattern 4: wlan0mon not found (3+ occurrences)
            elif len(self.pattern4.findall(pwnlog_lines)) >= 3:
                logging.info("[nexmon_stability] Pattern4: wlan0mon missing")
                if display:
                    display.set('status', 'wlan0mon down. Restarting.')
                    display.update(force=True)
                try:
                    subprocess.check_output("monstart", shell=True)
                except Exception as e:
                    logging.error(f"[nexmon_stability] monstart failed: {e}")

            # Pattern 5: concurrent map error
            elif len(self.pattern5.findall(pwnlog_lines)) >= 1:
                logging.info("[nexmon_stability] Pattern5: Bettercap map error")
                if display:
                    display.set('status', 'Bettercap crashed. Restarting.')
                    display.update(force=True)
                os.system("systemctl restart bettercap")
                pwnagotchi.restart("AUTO")

            # Pattern 6: runtime panic
            elif len(self.pattern6.findall(pwnlog_lines)) >= 1:
                logging.info("[nexmon_stability] Pattern6: Runtime panic")
                if display:
                    display.set('status', 'Runtime panic. Restarting.')
                    display.update(force=True)
                os.system("systemctl restart bettercap")
                pwnagotchi.restart("AUTO")

            # Pattern 7: allmulti -110 timeout
            elif len(self.pattern7.findall(pwnlog_lines)) >= 1:
                logging.info("[nexmon_stability] Pattern7: allmulti timeout")
                try:
                    result = agent.run("wifi.recon off; wifi.recon on")
                    if result.get("success"):
                        self.stats['wifi_recon_flips'] += 1
                        if display:
                            display.update(force=True, new_data={"status": "Wifi recon flipped!", "face": faces.COOL})
                except Exception as e:
                    logging.error(f"[nexmon_stability] wifi.recon flip error: {e}")

        except Exception as e:
            if self.options.get('debug', False):
                logging.debug(f"[nexmon_stability] Log check error: {e}")

    # =========================================================================
    # FROM fix_services.py - Main recovery procedure (SINGLE ATTEMPT)
    # =========================================================================
    def _tryTurningItOffAndOnAgain(self, agent):
        """
        Main recovery: reload brcmfmac + recreate wlan0mon.
        Uses monstart/monstop (better than iw commands).
        SINGLE ATTEMPT - no 3-retry loop (tested, causes crashes).
        """
        if self.is_disabled:
            return

        # Avoid overlapping restarts
        if self.isReloadingMon and (time.time() - self.LASTTRY) < 180:
            logging.debug("[nexmon_stability] Duplicate recovery attempt ignored")
            return

        self.isReloadingMon = True
        self.LASTTRY = time.time()
        self.stats['total_recoveries'] += 1

        display = agent.view() if hasattr(agent, 'view') else None

        if display:
            display.update(force=True, new_data={
                "status": "I'm blind! Reloading WiFi...",
                "face": faces.BORED
            })

        logging.info("[nexmon_stability] Starting recovery...")

        # Step 1: Stop wifi.recon
        try:
            result = agent.run("wifi.recon off")
            if result.get("success"):
                logging.info("[nexmon_stability] wifi.recon off: success")
                time.sleep(2)
            else:
                logging.warning(f"[nexmon_stability] wifi.recon off failed: {result}")
        except Exception as e:
            logging.error(f"[nexmon_stability] wifi.recon off error: {e}")

        # Step 2: Stop monitor interface
        try:
            subprocess.check_output("monstop", shell=True)
            logging.info("[nexmon_stability] monstop: success")
        except Exception as e:
            logging.debug(f"[nexmon_stability] monstop error (may be ok): {e}")

        # Step 3: Unload brcmfmac
        try:
            subprocess.check_output("sudo modprobe -r brcmfmac", shell=True)
            logging.info("[nexmon_stability] modprobe -r brcmfmac: success")

            if display:
                display.update(force=True, new_data={
                    "status": "Turning WiFi off...",
                    "face": faces.SMART
                })

            # Step 4: Rebuild deps and reload
            try:
                subprocess.check_output("sudo depmod -a", shell=True)
                logging.debug("[nexmon_stability] depmod -a completed")
            except Exception as e:
                logging.warning(f"[nexmon_stability] depmod -a failed: {e}")

            subprocess.check_output("sudo modprobe brcmfmac", shell=True)
            logging.info("[nexmon_stability] modprobe brcmfmac: success")

            # Step 5: Recreate monitor interface
            time.sleep(2)
            subprocess.check_output("monstart", shell=True)
            logging.info("[nexmon_stability] monstart: success")

            # Step 6: Reconnect bettercap
            try:
                result = agent.run("set wifi.interface wlan0mon")
                if result.get("success"):
                    logging.info("[nexmon_stability] set wifi.interface: success")
            except Exception as e:
                logging.debug(f"[nexmon_stability] set wifi.interface error: {e}")

        except Exception as e:
            logging.error(f"[nexmon_stability] Recovery failed: {e}")
            self.stats['failed_recoveries'] += 1
            self.isReloadingMon = False
            # Don't reboot - single attempt policy
            return

        # Step 7: Wait and restart recon
        delay = self.options.get('recovery_delay', 5)
        time.sleep(delay + 3)

        self.isReloadingMon = False

        try:
            result = agent.run("wifi.clear; wifi.recon on")
            if result.get("success"):
                logging.info("[nexmon_stability] Recovery complete! wifi.recon on")
                self.stats['successful_recoveries'] += 1
                self.stats['last_recovery'] = datetime.now().isoformat()
                self.blind_epochs = 0

                if display:
                    display.update(force=True, new_data={
                        "status": "I can see again!",
                        "face": faces.HAPPY
                    })

                # Record in history
                self.diagnostics['recovery_history'].append({
                    'time': datetime.now().isoformat(),
                    'reason': 'blindness_or_pattern',
                    'success': True
                })

                # 2-minute pause before next recovery allowed
                self.LASTTRY = time.time() + 120
            else:
                logging.error("[nexmon_stability] wifi.recon did not start")
                self.stats['failed_recoveries'] += 1
                self.LASTTRY = time.time() - 300  # Allow retry ASAP

        except Exception as e:
            logging.error(f"[nexmon_stability] wifi.recon on error: {e}")
            self.stats['failed_recoveries'] += 1

    # =========================================================================
    # HARDWARE DETECTION
    # =========================================================================
    def _detect_hardware(self):
        """Detect WiFi chip and firmware version."""
        try:
            result = subprocess.run(['dmesg'], capture_output=True, text=True, timeout=5)
            output = result.stdout

            if 'BCM43455' in output or 'bcm43455' in output:
                self.chip_type = 'BCM43455C0'
            elif 'BCM43430' in output or 'bcm43430' in output:
                self.chip_type = 'BCM43430A1'
            elif 'BCM43436' in output or 'bcm43436' in output:
                self.chip_type = 'BCM43436B0'

            fw_result = subprocess.run(
                ['cat', '/sys/module/brcmfmac/version'],
                capture_output=True, text=True, timeout=5
            )
            if fw_result.returncode == 0:
                self.firmware_version = fw_result.stdout.strip()

        except Exception as e:
            logging.debug(f"[nexmon_stability] Hardware detection error: {e}")

    # =========================================================================
    # CHANNEL HOP HANDLER
    # =========================================================================
    def on_channel_hop(self, agent, channel):
        """Called on channel hop - use safe hopper if available."""
        if self.is_disabled:
            return

        self.diagnostics['channel_hop_count'] += 1
        self.metrics['current_channel'] = channel

        if self.channel_hopper and CHANNEL_FIX_AVAILABLE:
            try:
                success = self.channel_hopper.hop_to(channel)
                if not success:
                    self.diagnostics['channel_hop_failures'] += 1
            except Exception as e:
                self.diagnostics['channel_hop_failures'] += 1

    # =========================================================================
    # DIAGNOSTICS COLLECTOR
    # =========================================================================
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
                self._update_interface_status()

                if self.stats['last_recovery']:
                    try:
                        last = datetime.fromisoformat(self.stats['last_recovery'])
                        self.stats['uptime_since_recovery'] = (datetime.now() - last).total_seconds()
                    except:
                        pass

            except Exception as e:
                if self.options.get('debug', False):
                    logging.debug(f"[nexmon_stability] Diagnostics error: {e}")

            self._diag_stop.wait(10)

    def _update_interface_status(self):
        """Update interface status metrics."""
        try:
            result = subprocess.run(['lsmod'], capture_output=True, text=True, timeout=5)
            self.metrics['driver_loaded'] = 'brcmfmac' in result.stdout

            result = subprocess.run(['ip', 'link', 'show', 'wlan0mon'],
                                   capture_output=True, text=True, timeout=5)
            self.metrics['interface_up'] = result.returncode == 0 and 'UP' in result.stdout

            result = subprocess.run(['iw', 'dev', 'wlan0mon', 'info'],
                                   capture_output=True, text=True, timeout=5)
            self.metrics['monitor_mode'] = 'monitor' in result.stdout.lower()
        except:
            pass

    # =========================================================================
    # UI HANDLERS
    # =========================================================================
    def on_ui_setup(self, ui):
        """Add status indicator to UI."""
        if self.is_disabled:
            return

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
        if self.is_disabled or not self.options.get('show_status', True):
            return

        try:
            if self._recovering or self.isReloadingMon:
                status = 'RCV'
            elif self.blind_epochs > 0:
                status = f'B{self.blind_epochs}'
            else:
                status = 'OK'
            ui.set('nexmon_status', status)
        except:
            pass

    # =========================================================================
    # WEB API
    # =========================================================================
    def on_webhook(self, path, request):
        """Handle web API requests."""
        if self.is_disabled:
            return jsonify({'error': 'Plugin disabled (external adapter)'}), 503

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
                if self.agent:
                    self._tryTurningItOffAndOnAgain(self.agent)
                    return jsonify({'status': 'recovery_triggered'})
                return jsonify({'error': 'agent not ready'}), 503
            return jsonify({'error': 'POST required'}), 400
        return jsonify({'error': 'unknown path'}), 404

    def _get_full_status(self):
        """Get complete status for API."""
        return {
            'plugin_version': self.__version__,
            'chip_type': self.chip_type,
            'firmware_version': self.firmware_version,
            'ready': self.ready,
            'disabled': self.is_disabled,
            'recovering': self._recovering or self.isReloadingMon,
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
                'unique_aps_count': len(self.diagnostics['unique_aps']),
                'channel_hop_count': self.diagnostics['channel_hop_count'],
                'channel_hop_failures': self.diagnostics['channel_hop_failures'],
            },
            'signal_history': list(self.diagnostics['signal_history'])[-20:],
            'recovery_history': list(self.diagnostics['recovery_history']),
        }

    def on_unload(self, ui):
        """Called when plugin is unloaded."""
        logging.info(f"[nexmon_stability] Unloading - Stats: {self.stats}")
        self._stop_diagnostics_collector()
        if self.channel_hopper:
            try:
                self.channel_hopper.stop()
            except:
                pass


# Simplified Web UI (keeping it shorter for this file)
DIAGNOSTICS_HTML = '''
<!DOCTYPE html>
<html>
<head>
    <title>Nexmon Stability v3</title>
    <style>
        body { font-family: monospace; background: #1a1a2e; color: #eee; padding: 20px; }
        h1 { color: #00ff88; }
        .card { background: #16213e; padding: 15px; margin: 10px 0; border-radius: 8px; }
        .ok { color: #00ff88; } .warn { color: #ffaa00; } .err { color: #ff4444; }
        table { width: 100%; } th, td { padding: 8px; text-align: left; }
        .btn { background: #0f3460; color: #fff; border: none; padding: 10px 20px; cursor: pointer; margin: 5px; }
        .btn:hover { background: #00d4ff; }
    </style>
</head>
<body>
    <h1>🛡️ Nexmon Stability v3.0 (Integrated)</h1>
    <div class="card">
        <h3>Status</h3>
        <div id="status">Loading...</div>
    </div>
    <div class="card">
        <h3>Statistics</h3>
        <table>
            <tr><td>Total Recoveries</td><td id="recoveries">-</td></tr>
            <tr><td>Successful</td><td id="successful">-</td></tr>
            <tr><td>Failed</td><td id="failed">-</td></tr>
            <tr><td>WiFi Recon Flips</td><td id="flips">-</td></tr>
            <tr><td>Blind Epochs</td><td id="blind">-</td></tr>
        </table>
    </div>
    <div class="card">
        <button class="btn" onclick="refresh()">🔄 Refresh</button>
        <button class="btn" onclick="recovery()" style="background:#cc3300">⚠️ Force Recovery</button>
    </div>
    <script>
        async function refresh() {
            const r = await fetch('api/status').then(r=>r.json());
            document.getElementById('status').innerHTML = 
                `<span class="${r.recovering?'warn':r.blind_epochs>0?'warn':'ok'}">
                ${r.recovering?'RECOVERING':r.blind_epochs>0?'BLIND ('+r.blind_epochs+')':'OK'}</span>
                - Chip: ${r.chip_type} - Driver: ${r.metrics.driver_loaded?'✓':'✗'}`;
            document.getElementById('recoveries').textContent = r.stats.total_recoveries;
            document.getElementById('successful').textContent = r.stats.successful_recoveries;
            document.getElementById('failed').textContent = r.stats.failed_recoveries;
            document.getElementById('flips').textContent = r.stats.wifi_recon_flips;
            document.getElementById('blind').textContent = r.stats.total_blind_epochs;
        }
        async function recovery() {
            if(!confirm('Trigger recovery?')) return;
            await fetch('api/recovery',{method:'POST'});
            alert('Recovery triggered');
            setTimeout(refresh, 5000);
        }
        refresh(); setInterval(refresh, 5000);
    </script>
</body>
</html>
'''
