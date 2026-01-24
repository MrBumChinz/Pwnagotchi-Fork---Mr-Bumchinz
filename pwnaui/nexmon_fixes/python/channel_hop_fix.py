"""
Nexmon Channel Hop Fix for Pwnagotchi
=====================================

This module provides a fixed channel hopping implementation that:
1. Adds delays between channel changes
2. Validates channel changes succeeded
3. Recovers from failed channel changes
4. Provides retry logic

Usage:
    Copy to /etc/pwnagotchi/custom-plugins/
    Import and use in your custom plugins or patches

Based on research from:
- https://github.com/seemoo-lab/nexmon/issues/335
- https://github.com/evilsocket/pwnagotchi/issues/267
"""

import logging
import subprocess
import time
import random
from typing import List, Optional, Tuple

class ChannelHopper:
    """
    Safe channel hopping implementation for Nexmon-patched WiFi.
    """
    
    # Default channel lists
    CHANNELS_2G = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]
    CHANNELS_5G = [36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 149, 153, 157, 161, 165]
    
    def __init__(self, 
                 interface: str = 'wlan0mon',
                 channels: Optional[List[int]] = None,
                 min_dwell_time: float = 0.3,
                 max_retries: int = 3,
                 retry_delay: float = 0.5):
        """
        Initialize the channel hopper.
        
        Args:
            interface: Monitor mode interface name
            channels: List of channels to hop through (None = all 2.4GHz)
            min_dwell_time: Minimum time to stay on each channel (seconds)
            max_retries: Maximum retries for failed channel changes
            retry_delay: Delay between retries (seconds)
        """
        self.interface = interface
        self.channels = channels or self.CHANNELS_2G
        self.min_dwell_time = min_dwell_time
        self.max_retries = max_retries
        self.retry_delay = retry_delay
        
        self.current_channel = None
        self.last_change_time = 0
        self.error_count = 0
        self.success_count = 0
        
    def _run_command(self, cmd: List[str], timeout: int = 5) -> Tuple[bool, str]:
        """Run a shell command and return success status and output."""
        try:
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=timeout
            )
            return result.returncode == 0, result.stdout + result.stderr
        except subprocess.TimeoutExpired:
            return False, "Command timed out"
        except Exception as e:
            return False, str(e)
    
    def get_current_channel(self) -> Optional[int]:
        """Get the current channel of the interface."""
        success, output = self._run_command(['iw', 'dev', self.interface, 'info'])
        if success:
            for line in output.split('\n'):
                if 'channel' in line:
                    try:
                        # Parse "channel X (FREQ MHz)"
                        parts = line.strip().split()
                        for i, part in enumerate(parts):
                            if part == 'channel' and i + 1 < len(parts):
                                return int(parts[i + 1])
                    except (ValueError, IndexError):
                        pass
        return None
    
    def set_channel(self, channel: int) -> bool:
        """
        Set the interface to a specific channel with retry logic.
        
        Args:
            channel: Channel number to set
            
        Returns:
            True if successful, False otherwise
        """
        # Respect minimum dwell time
        time_since_last = time.time() - self.last_change_time
        if time_since_last < self.min_dwell_time:
            time.sleep(self.min_dwell_time - time_since_last)
        
        for attempt in range(self.max_retries):
            success, output = self._run_command([
                'iw', 'dev', self.interface, 'set', 'channel', str(channel)
            ])
            
            if success:
                # Verify the channel was actually set
                time.sleep(0.05)  # Small delay before verification
                actual_channel = self.get_current_channel()
                
                if actual_channel == channel:
                    self.current_channel = channel
                    self.last_change_time = time.time()
                    self.success_count += 1
                    return True
                else:
                    logging.warning(f'Channel mismatch: requested {channel}, got {actual_channel}')
            
            # Check for the dreaded -110 error
            if '-110' in output or 'Set Channel failed' in output:
                logging.warning(f'Channel hop failure (-110) on attempt {attempt + 1}')
                self.error_count += 1
                
                # Longer delay on -110 errors
                time.sleep(self.retry_delay * 2)
            else:
                time.sleep(self.retry_delay)
        
        logging.error(f'Failed to set channel {channel} after {self.max_retries} attempts')
        return False
    
    def hop_to_next(self) -> bool:
        """Hop to the next channel in the sequence."""
        if not self.channels:
            return False
            
        if self.current_channel is None:
            next_channel = self.channels[0]
        else:
            try:
                current_idx = self.channels.index(self.current_channel)
                next_idx = (current_idx + 1) % len(self.channels)
                next_channel = self.channels[next_idx]
            except ValueError:
                next_channel = self.channels[0]
        
        return self.set_channel(next_channel)
    
    def hop_random(self) -> bool:
        """Hop to a random channel."""
        if not self.channels:
            return False
            
        # Avoid the current channel if possible
        available = [c for c in self.channels if c != self.current_channel]
        if not available:
            available = self.channels
            
        return self.set_channel(random.choice(available))
    
    def get_stats(self) -> dict:
        """Get hopping statistics."""
        return {
            'current_channel': self.current_channel,
            'success_count': self.success_count,
            'error_count': self.error_count,
            'error_rate': self.error_count / max(1, self.success_count + self.error_count),
        }
    
    def is_healthy(self, max_error_rate: float = 0.5) -> bool:
        """Check if the hopper is functioning healthily."""
        stats = self.get_stats()
        return stats['error_rate'] < max_error_rate


def safe_channel_hop(interface: str = 'wlan0mon', 
                     channel: int = 1,
                     retries: int = 3) -> bool:
    """
    Standalone function for safe channel hopping.
    
    This is a simple wrapper that can be used as a drop-in replacement
    for direct iw commands.
    
    Args:
        interface: Monitor mode interface
        channel: Target channel
        retries: Number of retry attempts
        
    Returns:
        True if successful, False otherwise
    """
    hopper = ChannelHopper(
        interface=interface,
        max_retries=retries,
        min_dwell_time=0.1
    )
    return hopper.set_channel(channel)


# Example usage
if __name__ == '__main__':
    import sys
    
    logging.basicConfig(level=logging.INFO)
    
    interface = sys.argv[1] if len(sys.argv) > 1 else 'wlan0mon'
    hopper = ChannelHopper(interface=interface, channels=[1, 6, 11])
    
    print(f"Testing channel hopping on {interface}")
    print(f"Channels: {hopper.channels}")
    
    for i in range(10):
        if hopper.hop_to_next():
            print(f"Hopped to channel {hopper.current_channel}")
        else:
            print(f"Failed to hop!")
        
        time.sleep(1)
    
    print(f"\nStats: {hopper.get_stats()}")
