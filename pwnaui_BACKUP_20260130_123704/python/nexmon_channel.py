"""
Nexmon Safe Channel Hopping Module for PwnaUI

Provides rate-limited, delay-safe channel hopping to prevent the
well-known nexmon -110 timeout errors and firmware crashes.

Based on analysis from:
- https://github.com/seemoo-lab/nexmon/issues/335
- https://github.com/evilsocket/pwnagotchi/issues/267

Usage:
    from nexmon_channel import SafeChannelHopper
    
    hopper = SafeChannelHopper('wlan0mon')
    hopper.hop_to(6)
    hopper.start_auto_hop(channels=[1, 6, 11], interval=0.5)
    hopper.stop()

Author: PwnaUI Team
License: MIT
"""

import os
import time
import logging
import subprocess
import threading
from typing import List, Optional, Callable
from collections import deque
from datetime import datetime


class SafeChannelHopper:
    """
    Rate-limited channel hopper with safety delays to prevent
    nexmon firmware crashes.
    """
    
    # Channel definitions
    CHANNELS_2GHZ = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13]
    CHANNELS_5GHZ = [36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 
                    112, 116, 120, 124, 128, 132, 136, 140, 149, 153, 157, 161, 165]
    
    # Default timing (conservative values to prevent crashes)
    DEFAULT_HOP_DELAY = 0.15        # 150ms minimum between hops
    DEFAULT_5GHZ_DELAY = 0.25       # 250ms for 5GHz (DFS channels need more time)
    DEFAULT_BAND_SWITCH_DELAY = 0.3 # 300ms when switching bands
    DEFAULT_RECOVERY_DELAY = 2.0    # 2s after error before retry
    
    def __init__(
        self,
        interface: str = 'wlan0mon',
        hop_delay: float = DEFAULT_HOP_DELAY,
        max_errors: int = 3,
        recovery_delay: float = DEFAULT_RECOVERY_DELAY,
        on_error: Optional[Callable] = None
    ):
        """
        Initialize safe channel hopper.
        
        Args:
            interface: Monitor mode interface name
            hop_delay: Minimum delay between channel hops (seconds)
            max_errors: Max consecutive errors before recovery pause
            recovery_delay: Delay after errors (seconds)
            on_error: Callback function on error (receives channel, error)
        """
        self.interface = interface
        self.hop_delay = hop_delay
        self.max_errors = max_errors
        self.recovery_delay = recovery_delay
        self.on_error = on_error
        
        # State
        self.current_channel = None
        self.last_hop_time = 0
        self.consecutive_errors = 0
        self.in_recovery = False
        
        # Auto-hop state
        self._auto_hop_thread = None
        self._stop_event = threading.Event()
        self._hop_lock = threading.Lock()
        
        # Statistics
        self.stats = {
            'total_hops': 0,
            'successful_hops': 0,
            'failed_hops': 0,
            'recoveries': 0,
            'last_channel': None,
            'last_error': None,
        }
        
        # Recent hop history (for debugging)
        self._hop_history = deque(maxlen=100)
        
        # Detect current channel
        self._detect_current_channel()

    def _detect_current_channel(self):
        """Detect current channel from interface."""
        try:
            result = subprocess.run(
                ['iw', 'dev', self.interface, 'info'],
                capture_output=True, text=True, timeout=5
            )
            if result.returncode == 0:
                for line in result.stdout.split('\n'):
                    if 'channel' in line.lower():
                        # Parse "channel 6 (2437 MHz)"
                        parts = line.strip().split()
                        for i, part in enumerate(parts):
                            if part.lower() == 'channel' and i + 1 < len(parts):
                                self.current_channel = int(parts[i + 1])
                                break
        except:
            pass

    def _get_hop_delay(self, target_channel: int) -> float:
        """
        Calculate appropriate delay for channel hop.
        
        Returns longer delays for:
        - 5GHz channels (DFS scan requirements)
        - Band switches (2.4 <-> 5GHz)
        """
        delay = self.hop_delay
        
        # 5GHz channels need more time
        if target_channel in self.CHANNELS_5GHZ:
            delay = max(delay, self.DEFAULT_5GHZ_DELAY)
        
        # Band switch needs even more time
        if self.current_channel:
            current_is_5ghz = self.current_channel in self.CHANNELS_5GHZ
            target_is_5ghz = target_channel in self.CHANNELS_5GHZ
            if current_is_5ghz != target_is_5ghz:
                delay = max(delay, self.DEFAULT_BAND_SWITCH_DELAY)
        
        return delay

    def _wait_for_hop(self, target_channel: int):
        """Wait appropriate time before hop."""
        required_delay = self._get_hop_delay(target_channel)
        elapsed = time.time() - self.last_hop_time
        
        if elapsed < required_delay:
            wait_time = required_delay - elapsed
            time.sleep(wait_time)

    def hop_to(self, channel: int, force: bool = False) -> bool:
        """
        Safely hop to specified channel.
        
        Args:
            channel: Target channel number
            force: Skip safety delays (not recommended)
            
        Returns:
            True if successful, False otherwise
        """
        with self._hop_lock:
            # Check if in recovery mode
            if self.in_recovery and not force:
                logging.debug(f"[nexmon_channel] In recovery mode, skipping hop to {channel}")
                return False
            
            # Wait for appropriate delay
            if not force:
                self._wait_for_hop(channel)
            
            self.stats['total_hops'] += 1
            
            try:
                # Execute channel change
                result = subprocess.run(
                    ['iw', 'dev', self.interface, 'set', 'channel', str(channel)],
                    capture_output=True, text=True, timeout=5
                )
                
                self.last_hop_time = time.time()
                
                if result.returncode == 0:
                    # Success
                    self.current_channel = channel
                    self.consecutive_errors = 0
                    self.stats['successful_hops'] += 1
                    self.stats['last_channel'] = channel
                    
                    self._hop_history.append({
                        'time': datetime.now().isoformat(),
                        'channel': channel,
                        'success': True
                    })
                    
                    return True
                else:
                    # Failed
                    error_msg = result.stderr.strip()
                    return self._handle_hop_error(channel, error_msg)
                    
            except subprocess.TimeoutExpired:
                return self._handle_hop_error(channel, "timeout")
            except Exception as e:
                return self._handle_hop_error(channel, str(e))

    def _handle_hop_error(self, channel: int, error: str) -> bool:
        """Handle channel hop error."""
        self.consecutive_errors += 1
        self.stats['failed_hops'] += 1
        self.stats['last_error'] = f"Ch {channel}: {error}"
        
        self._hop_history.append({
            'time': datetime.now().isoformat(),
            'channel': channel,
            'success': False,
            'error': error
        })
        
        logging.warning(f"[nexmon_channel] Channel hop to {channel} failed: {error}")
        
        # Call error callback if set
        if self.on_error:
            try:
                self.on_error(channel, error)
            except:
                pass
        
        # Check if we need recovery pause
        if self.consecutive_errors >= self.max_errors:
            logging.warning(f"[nexmon_channel] {self.consecutive_errors} consecutive errors, entering recovery")
            self._enter_recovery()
        
        return False

    def _enter_recovery(self):
        """Enter recovery mode after too many errors."""
        self.in_recovery = True
        self.stats['recoveries'] += 1
        
        # Wait recovery delay
        time.sleep(self.recovery_delay)
        
        # Reset state
        self.consecutive_errors = 0
        self.in_recovery = False
        
        logging.info("[nexmon_channel] Recovery complete")

    def start_auto_hop(
        self,
        channels: Optional[List[int]] = None,
        interval: float = 0.5,
        include_5ghz: bool = False
    ):
        """
        Start automatic channel hopping.
        
        Args:
            channels: List of channels to hop through (default: 2.4GHz)
            interval: Time to spend on each channel (seconds)
            include_5ghz: Include 5GHz channels in default list
        """
        if self._auto_hop_thread and self._auto_hop_thread.is_alive():
            logging.warning("[nexmon_channel] Auto-hop already running")
            return
        
        # Default channels
        if channels is None:
            channels = self.CHANNELS_2GHZ.copy()
            if include_5ghz:
                channels.extend(self.CHANNELS_5GHZ)
        
        self._stop_event.clear()
        self._auto_hop_thread = threading.Thread(
            target=self._auto_hop_loop,
            args=(channels, interval),
            daemon=True
        )
        self._auto_hop_thread.start()
        logging.info(f"[nexmon_channel] Started auto-hop: {len(channels)} channels, {interval}s interval")

    def _auto_hop_loop(self, channels: List[int], interval: float):
        """Auto-hop thread loop."""
        idx = 0
        while not self._stop_event.is_set():
            channel = channels[idx % len(channels)]
            
            if self.hop_to(channel):
                # Successful hop - wait interval
                self._stop_event.wait(interval)
            else:
                # Failed - small delay before retry
                self._stop_event.wait(0.1)
            
            idx += 1

    def stop(self):
        """Stop auto-hopping."""
        if self._auto_hop_thread:
            self._stop_event.set()
            self._auto_hop_thread.join(timeout=2)
            self._auto_hop_thread = None
            logging.info("[nexmon_channel] Auto-hop stopped")

    def get_stats(self) -> dict:
        """Get hopping statistics."""
        return {
            **self.stats,
            'current_channel': self.current_channel,
            'consecutive_errors': self.consecutive_errors,
            'in_recovery': self.in_recovery,
            'success_rate': (
                self.stats['successful_hops'] / self.stats['total_hops'] * 100
                if self.stats['total_hops'] > 0 else 100
            )
        }

    def get_history(self, count: int = 20) -> list:
        """Get recent hop history."""
        return list(self._hop_history)[-count:]


class PwnagotchiChannelHook:
    """
    Hook for integrating SafeChannelHopper with Pwnagotchi's
    bettercap wifi.recon channel hopping.
    """
    
    def __init__(self, hopper: SafeChannelHopper):
        self.hopper = hopper
        self._original_recon = None
    
    def install(self, agent):
        """
        Install hook into pwnagotchi agent.
        
        This replaces the default channel hop behavior with
        our safe hopper.
        """
        try:
            # Hook into the agent's channel hop callback
            if hasattr(agent, '_on_channel_hop'):
                self._original_recon = agent._on_channel_hop
                agent._on_channel_hop = self._hooked_channel_hop
                logging.info("[nexmon_channel] Installed channel hop hook")
        except Exception as e:
            logging.error(f"[nexmon_channel] Failed to install hook: {e}")
    
    def _hooked_channel_hop(self, agent, channel):
        """Hooked channel hop that uses safe hopper."""
        # Use our safe hopper
        self.hopper.hop_to(channel)
        
        # Call original if exists
        if self._original_recon:
            try:
                self._original_recon(agent, channel)
            except:
                pass


# Convenience function for quick setup
def create_safe_hopper(interface: str = 'wlan0mon', **kwargs) -> SafeChannelHopper:
    """Create and return a configured SafeChannelHopper instance."""
    return SafeChannelHopper(interface=interface, **kwargs)


# Module-level default instance
_default_hopper = None

def get_default_hopper() -> SafeChannelHopper:
    """Get or create default hopper instance."""
    global _default_hopper
    if _default_hopper is None:
        _default_hopper = SafeChannelHopper()
    return _default_hopper
