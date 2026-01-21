"""
PIL-Free UI Components for Pwnagotchi

This package provides drop-in replacements for Pwnagotchi's PIL-based
UI components. Instead of rendering with PIL, these components store
their state and send commands to the PwnaUI C daemon for rendering.

This eliminates the ~55MB memory footprint of PIL and reduces CPU usage
by 10-30x compared to the Python/PIL rendering path.

Installation:
1. Copy this directory to /usr/lib/python3/dist-packages/pwnagotchi/ui/pil_free/
2. Apply the patch to view.py and display.py (see patch_pil_free.py)
3. Restart pwnagotchi

Or use the automated deployment:
    python3 deploy_phase2.py --host <pi-hostname>
"""

from .components import (
    Widget,
    Bitmap,
    Line,
    Rect,
    FilledRect,
    Text,
    LabeledValue,
    WHITE,
    BLACK,
)

# View requires pwnagotchi - only import on Pi
# Use: from pil_free.view import View
def __getattr__(name):
    """Lazy import for View to avoid requiring pwnagotchi on import."""
    if name == 'View':
        from .view import View
        return View
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")

__version__ = '2.0.0'
__all__ = [
    'Widget', 'Bitmap', 'Line', 'Rect', 'FilledRect', 'Text', 'LabeledValue',
    'View', 'WHITE', 'BLACK',
]
