"""
PwnaUI PIL-Free Components - Direct Replacement for pwnagotchi/ui/components.py

Minimal component classes that store state without PIL rendering.
All actual rendering is done by the pwnaui C daemon.
"""

# Color constants (matching e-ink display conventions)
WHITE = 0x00  # Actually white on display (inverted for some displays)
BLACK = 0xFF  # Actually black on display


class Widget:
    """Base widget class - stores position and color"""
    
    def __init__(self, xy=(0, 0), color=BLACK):
        self.xy = xy
        self.color = color
    
    def draw(self, canvas, drawer):
        """No-op: C daemon handles all rendering"""
        pass


class Text(Widget):
    """Text display widget"""
    
    def __init__(self, value='', position=(0, 0), font=None, color=BLACK,
                 wrap=False, max_length=0, png=False):
        super().__init__(position, color)
        self.value = value
        self.position = position
        self.font = font
        self.wrap = wrap
        self.max_length = max_length
        self.png = png


class LabeledValue(Widget):
    """Label + value display widget"""
    
    def __init__(self, color=BLACK, label='', value='', position=(0, 0),
                 label_font=None, text_font=None):
        super().__init__(position, color)
        self.label = label
        self.value = value
        self.position = position
        self.label_font = label_font
        self.text_font = text_font


class Line(Widget):
    """Line drawing widget"""
    
    def __init__(self, xy, color=BLACK, width=1):
        self.xy = xy
        self.color = color
        self.width = width


class Rect(Widget):
    """Rectangle outline widget"""
    
    def __init__(self, xy, color=BLACK):
        self.xy = xy
        self.color = color


class FilledRect(Widget):
    """Filled rectangle widget"""
    
    def __init__(self, xy, color=BLACK):
        self.xy = xy
        self.color = color


class Bitmap(Widget):
    """Bitmap/image widget"""
    
    def __init__(self, path='', xy=(0, 0), color=BLACK):
        super().__init__(xy, color)
        self.path = path
        self.image = None  # No PIL image loading
