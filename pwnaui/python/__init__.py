"""
PwnaUI - High-Performance UI Renderer for Pwnagotchi

This package provides C-accelerated UI rendering for Pwnagotchi,
delivering 10-30x faster display updates compared to Python/PIL.

Installation:
1. Ensure pwnaui daemon is running: sudo systemctl start pwnaui
2. Import and patch early in pwnagotchi startup:
   
   import pwnaui_patch
   if pwnaui_patch.is_available():
       pwnaui_patch.patch_view()
"""

from .pwnaui_patch import patch_view, is_available, get_client, PwnaUIClient

__version__ = '1.0.0'
__all__ = ['patch_view', 'is_available', 'get_client', 'PwnaUIClient']
