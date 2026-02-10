"""
PwnaUI Theme Manager - Python wrapper for theme management

Handles:
- Theme discovery and loading
- Voice phrase selection (from voice.py)
- Theme switching via IPC
- Web UI API

Theme Directory Structure:
    /home/pi/pwnaui/themes/
    ├── default/
    │   ├── HAPPY.png
    │   ├── SAD.png
    │   ├── voice.py
    │   └── ...
    ├── rick/
    │   ├── HAPPY.png
    │   ├── SAD.png
    │   ├── voice.py
    │   └── lang/
    │       └── en/voice.po
    └── flipper/
        └── ...
"""

import os
import sys
import json
import random
import importlib.util
import logging
from typing import Dict, List, Optional, Any
from pathlib import Path

log = logging.getLogger("pwnaui.themes")

THEMES_DIR = "/home/pi/pwnaui/themes"

# Face states that match C enum
FACE_STATES = [
    "LOOK_R", "LOOK_L", "LOOK_R_HAPPY", "LOOK_L_HAPPY",
    "SLEEP", "SLEEP2", "AWAKE", "BORED", "INTENSE", "COOL",
    "HAPPY", "EXCITED", "GRATEFUL", "MOTIVATED", "DEMOTIVATED",
    "SMART", "LONELY", "SAD", "ANGRY", "FRIEND", "BROKEN",
    "DEBUG", "UPLOAD", "UPLOAD1", "UPLOAD2"
]

# Default voice messages (used when theme has no voice.py)
DEFAULT_VOICE = {
    "default": [
        "zzz...",
        "(◕‿◕)",
        "...",
    ],
    "on_starting": [
        "Starting up...",
        "Initializing...",
        "Waking up...",
    ],
    "on_ai_ready": [
        "AI is ready!",
        "Neural network initialized.",
        "Brain is online!",
    ],
    "on_normal": [
        "Just vibing...",
        "Scanning...",
        "Looking around...",
    ],
    "on_bored": [
        "I'm bored...",
        "Nothing interesting happening...",
        "Yawn...",
    ],
    "on_happy": [
        "Yay!",
        "I'm happy!",
        "Great!",
    ],
    "on_sad": [
        "I'm sad...",
        "Not feeling great...",
        "Aww...",
    ],
    "on_angry": [
        "Grr!",
        "Not happy!",
        "Frustrated!",
    ],
    "on_excited": [
        "Woohoo!",
        "This is exciting!",
        "Yes!",
    ],
    "on_lonely": [
        "Feeling lonely...",
        "Where is everyone?",
        "All alone...",
    ],
    "on_grateful": [
        "Thank you!",
        "Much appreciated!",
        "Thanks!",
    ],
    "on_motivated": [
        "Let's do this!",
        "Feeling motivated!",
        "Ready to go!",
    ],
    "on_demotivated": [
        "Not great...",
        "Could be better...",
        "Meh...",
    ],
    "on_napping": [
        "Napping for {secs}s...",
        "ZzZz ({secs}s)...",
        "Power nap ({secs}s)...",
    ],
    "on_shutdown": [
        "Goodbye!",
        "Shutting down...",
        "See you later!",
    ],
    "on_awakening": [
        "I'm awake!",
        "Good morning!",
        "Ready!",
    ],
    "on_handshakes": [
        "Got {num} handshake{plural}!",
        "{num} new handshake{plural}!",
        "Captured {num}!",
    ],
    "on_miss": [
        "Missed {who}!",
        "{who} got away!",
        "Aww, lost {who}!",
    ],
    "on_new_peer": [
        "Hello {name}!",
        "Hi {name}!",
        "Welcome {name}!",
    ],
    "on_lost_peer": [
        "Bye {name}!",
        "See you {name}!",
        "{name} left...",
    ],
}


class ThemeVoice:
    """
    Voice handler for a theme.
    Loads voice.py from theme directory if available,
    otherwise uses default messages.
    """
    
    def __init__(self, theme_dir: str, lang: str = "en"):
        self.theme_dir = theme_dir
        self.lang = lang
        self.voice_module = None
        self.voice_instance = None
        self._load_voice()
    
    def _load_voice(self):
        """Load voice.py from theme directory"""
        voice_path = os.path.join(self.theme_dir, "voice.py")
        
        if not os.path.exists(voice_path):
            log.debug(f"No voice.py found in {self.theme_dir}")
            return
        
        try:
            # Load module dynamically
            spec = importlib.util.spec_from_file_location("voice", voice_path)
            self.voice_module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(self.voice_module)
            
            # Instantiate Voice class
            if hasattr(self.voice_module, "Voice"):
                self.voice_instance = self.voice_module.Voice(self.lang)
                log.debug(f"Loaded voice.py from {self.theme_dir}")
        except Exception as e:
            log.error(f"Failed to load voice.py: {e}")
            self.voice_module = None
            self.voice_instance = None
    
    def _get_default(self, event: str, **kwargs) -> str:
        """Get default voice message"""
        messages = DEFAULT_VOICE.get(event, DEFAULT_VOICE.get("default", ["..."]))
        msg = random.choice(messages)
        try:
            return msg.format(**kwargs)
        except:
            return msg
    
    def get_message(self, event: str, **kwargs) -> str:
        """
        Get a voice message for an event.
        
        Args:
            event: Event name (e.g., "on_bored", "on_happy")
            **kwargs: Event-specific parameters
            
        Returns:
            Voice message string
        """
        if self.voice_instance:
            # Try to call the method on the Voice instance
            method_name = event if event.startswith("on_") else f"on_{event}"
            if hasattr(self.voice_instance, method_name):
                try:
                    method = getattr(self.voice_instance, method_name)
                    if kwargs:
                        return method(**kwargs)
                    else:
                        return method()
                except Exception as e:
                    log.warning(f"Voice method {method_name} failed: {e}")
            
            # Try 'default' method
            if hasattr(self.voice_instance, "default"):
                try:
                    return self.voice_instance.default()
                except:
                    pass
        
        return self._get_default(event, **kwargs)
    
    def __getattr__(self, name: str):
        """Allow calling voice methods directly"""
        if name.startswith("on_") or name == "default":
            return lambda **kwargs: self.get_message(name, **kwargs)
        raise AttributeError(f"'{type(self).__name__}' has no attribute '{name}'")


class Theme:
    """
    Represents a single theme.
    """
    
    def __init__(self, name: str, path: str):
        self.name = name
        self.path = path
        self.voice: Optional[ThemeVoice] = None
        self.face_images: Dict[str, str] = {}
        self.faces_dir: Optional[str] = None
        self.metadata: Dict[str, Any] = {}
        self._load()
    
    def _find_faces_dir(self) -> Optional[str]:
        """
        Find the directory containing face PNGs.
        Themes use various structures:
        - faces directly in theme root
        - custom-faces/ subdirectory
        - faces_*/ subdirectory (e.g., faces_flipper_dolphin)
        """
        # Check if HAPPY.png exists directly in theme root
        if os.path.exists(os.path.join(self.path, "HAPPY.png")):
            return self.path
        
        # Search for common subdirectory patterns
        for subdir in os.listdir(self.path):
            subpath = os.path.join(self.path, subdir)
            if os.path.isdir(subpath):
                # Check for faces in subdirectory
                if os.path.exists(os.path.join(subpath, "HAPPY.png")):
                    return subpath
                # Also check patterns like custom-faces, faces_*
                if subdir.lower() in ("custom-faces", "faces") or subdir.lower().startswith("faces_"):
                    if os.path.exists(os.path.join(subpath, "HAPPY.png")):
                        return subpath
        
        return None
    
    def _load(self):
        """Load theme data"""
        # Find the faces directory
        self.faces_dir = self._find_faces_dir()
        
        if self.faces_dir:
            # Find face images
            for state in FACE_STATES:
                png_path = os.path.join(self.faces_dir, f"{state}.png")
                if os.path.exists(png_path):
                    self.face_images[state] = png_path
        
        # Load metadata if exists
        meta_path = os.path.join(self.path, "theme.json")
        if os.path.exists(meta_path):
            try:
                with open(meta_path) as f:
                    self.metadata = json.load(f)
            except Exception as e:
                log.warning(f"Failed to load theme.json: {e}")
        
        # Load voice
        self.voice = ThemeVoice(self.path)
        
        log.debug(f"Theme '{self.name}' loaded: {len(self.face_images)} faces from {self.faces_dir}")
    
    def has_face(self, state: str) -> bool:
        """Check if theme has a face image for given state"""
        return state in self.face_images
    
    def get_face_path(self, state: str) -> Optional[str]:
        """Get path to face image"""
        return self.face_images.get(state)
    
    def to_dict(self) -> Dict[str, Any]:
        """Convert to dictionary for JSON serialization"""
        return {
            "name": self.name,
            "path": self.path,
            "faces_dir": self.faces_dir,
            "face_count": len(self.face_images),
            "faces": list(self.face_images.keys()),
            "has_voice": self.voice.voice_instance is not None,
            "metadata": self.metadata,
        }


class ThemeManager:
    """
    Manages available themes and the active theme.
    """
    
    def __init__(self, themes_dir: str = THEMES_DIR, pwnaui_client=None):
        self.themes_dir = themes_dir
        self.themes: Dict[str, Theme] = {}
        self.active_theme: Optional[Theme] = None
        self.pwnaui_client = pwnaui_client  # For sending SET_THEME commands
        
        # Ensure themes directory exists
        os.makedirs(self.themes_dir, exist_ok=True)
        
        self._discover_themes()
    
    def _discover_themes(self):
        """Scan themes directory for available themes"""
        self.themes = {}
        
        if not os.path.isdir(self.themes_dir):
            log.warning(f"Themes directory not found: {self.themes_dir}")
            return
        
        for name in os.listdir(self.themes_dir):
            theme_path = os.path.join(self.themes_dir, name)
            if os.path.isdir(theme_path) and not name.startswith('.'):
                try:
                    theme = Theme(name, theme_path)
                    self.themes[name] = theme
                except Exception as e:
                    log.error(f"Failed to load theme '{name}': {e}")
        
        log.debug(f"Discovered {len(self.themes)} themes")
    
    def refresh(self):
        """Refresh theme list"""
        self._discover_themes()
    
    def list_themes(self) -> List[str]:
        """Get list of available theme names"""
        return list(self.themes.keys())
    
    def get_theme(self, name: str) -> Optional[Theme]:
        """Get theme by name"""
        return self.themes.get(name)
    
    def set_active_theme(self, name: str) -> bool:
        """
        Set the active theme.
        
        Args:
            name: Theme name (or None to disable themes)
            
        Returns:
            True if successful
        """
        if name is None:
            self.active_theme = None
            if self.pwnaui_client:
                try:
                    self.pwnaui_client._send_command("SET_THEME")
                except:
                    pass
            return True
        
        theme = self.themes.get(name)
        if not theme:
            log.error(f"Theme not found: {name}")
            return False
        
        self.active_theme = theme
        
        # Notify C daemon
        if self.pwnaui_client:
            try:
                self.pwnaui_client._send_command(f"SET_THEME {name}")
            except Exception as e:
                log.warning(f"Failed to notify daemon of theme change: {e}")
        
        log.debug(f"Active theme set to: {name}")
        return True
    
    def get_active_theme(self) -> Optional[Theme]:
        """Get the currently active theme"""
        return self.active_theme
    
    def get_voice_message(self, event: str, **kwargs) -> str:
        """
        Get voice message from active theme.
        Falls back to default if no theme active.
        """
        if self.active_theme and self.active_theme.voice:
            return self.active_theme.voice.get_message(event, **kwargs)
        
        # Use default voice
        return ThemeVoice("").get_message(event, **kwargs)
    
    def to_dict(self) -> Dict[str, Any]:
        """Convert to dictionary for JSON serialization"""
        return {
            "themes_dir": self.themes_dir,
            "available_themes": [t.to_dict() for t in self.themes.values()],
            "active_theme": self.active_theme.name if self.active_theme else None,
        }


# Global instance
_theme_manager: Optional[ThemeManager] = None


def get_theme_manager(pwnaui_client=None) -> ThemeManager:
    """Get or create the global ThemeManager instance"""
    global _theme_manager
    if _theme_manager is None:
        _theme_manager = ThemeManager(pwnaui_client=pwnaui_client)
    return _theme_manager


def set_theme(name: str) -> bool:
    """Convenience function to set active theme"""
    return get_theme_manager().set_active_theme(name)


class FaceRenderer:
    """
    Renders theme faces for the e-ink display.
    Handles conversion from RGBA PNG to 1-bit bitmap.
    """
    
    # Display dimensions (Waveshare 2.13" V4)
    DISPLAY_WIDTH = 250
    DISPLAY_HEIGHT = 122
    
    # Face rendering area (centered, with margins for UI elements)
    FACE_MAX_WIDTH = 192   # Max width for face
    FACE_MAX_HEIGHT = 96  # Max height for face
    FACE_X = 29           # X position (centered in display)
    FACE_Y = 21           # Y position (below status bar)
    
    def __init__(self, theme_manager: Optional[ThemeManager] = None):
        self.theme_manager = theme_manager or get_theme_manager()
        self._face_cache: Dict[str, bytes] = {}  # Cache converted bitmaps
    
    def _convert_to_1bit(self, img: 'Image.Image', invert: bool = True) -> 'Image.Image':
        """
        Convert image to 1-bit for e-ink display.
        
        Args:
            img: PIL Image (any mode)
            invert: If True, black background becomes white (for e-ink)
            
        Returns:
            1-bit PIL Image
        """
        from PIL import Image
        
        # Handle images with transparency (RGBA, LA, PA, etc.)
        if img.mode in ('RGBA', 'LA', 'PA'):
            # Convert to RGBA for consistent handling
            if img.mode != 'RGBA':
                img = img.convert('RGBA')
            # Create white background
            bg = Image.new('RGBA', img.size, (255, 255, 255, 255))
            # Composite
            img = Image.alpha_composite(bg, img)
        
        # Convert to grayscale
        gray = img.convert('L')
        
        # Apply threshold for 1-bit conversion
        # Use Floyd-Steinberg dithering for better quality
        bw = gray.point(lambda x: 255 if x > 128 else 0, '1')
        
        if invert:
            # Invert so black drawings appear on white e-ink background
            bw = bw.point(lambda x: 255 - x)
        
        return bw
    
    def _scale_to_fit(self, img: 'Image.Image', max_width: int, max_height: int) -> 'Image.Image':
        """
        Scale image to fit within bounds while maintaining aspect ratio.
        """
        from PIL import Image
        
        w, h = img.size
        
        # Calculate scale factor
        scale = min(max_width / w, max_height / h)
        
        if scale >= 1:
            # Image already fits, no scaling needed
            return img
        
        # Calculate new size
        new_w = int(w * scale)
        new_h = int(h * scale)
        
        # Use LANCZOS for high quality downscale
        return img.resize((new_w, new_h), Image.Resampling.LANCZOS)
    
    def get_face_bitmap(self, state: str, theme_name: Optional[str] = None) -> Optional[bytes]:
        """
        Get face bitmap for given state, ready for e-ink display.
        
        Args:
            state: Face state (e.g., "HAPPY", "SAD")
            theme_name: Theme name (uses active theme if None)
            
        Returns:
            Raw bitmap bytes (1-bit, packed) or None if not available
        """
        from PIL import Image
        
        # Get theme
        theme = None
        if theme_name:
            theme = self.theme_manager.get_theme(theme_name)
        else:
            theme = self.theme_manager.get_active_theme()
        
        if not theme:
            return None
        
        # Check cache
        cache_key = f"{theme.name}:{state}"
        if cache_key in self._face_cache:
            return self._face_cache[cache_key]
        
        # Get face image path
        face_path = theme.get_face_path(state)
        if not face_path:
            return None
        
        try:
            # Load and process image
            img = Image.open(face_path)
            
            # Scale to fit
            img = self._scale_to_fit(img, self.FACE_MAX_WIDTH, self.FACE_MAX_HEIGHT)
            
            # Convert to 1-bit
            bw = self._convert_to_1bit(img)
            
            # Convert to raw bytes
            # For 1-bit images, tobytes() packs 8 pixels per byte
            bitmap = bw.tobytes()
            
            # Cache it
            self._face_cache[cache_key] = bitmap
            
            return bitmap
            
        except Exception as e:
            log.error(f"Failed to process face {state}: {e}")
            return None
    
    def get_face_pil(self, state: str, theme_name: Optional[str] = None) -> Optional['Image.Image']:
        """
        Get face as PIL Image (1-bit) for given state.
        
        Args:
            state: Face state (e.g., "HAPPY", "SAD")
            theme_name: Theme name (uses active theme if None)
            
        Returns:
            1-bit PIL Image or None
        """
        from PIL import Image
        
        # Get theme
        theme = None
        if theme_name:
            theme = self.theme_manager.get_theme(theme_name)
        else:
            theme = self.theme_manager.get_active_theme()
        
        if not theme:
            return None
        
        # Get face image path
        face_path = theme.get_face_path(state)
        if not face_path:
            return None
        
        try:
            # Load and process image
            img = Image.open(face_path)
            
            # Scale to fit
            img = self._scale_to_fit(img, self.FACE_MAX_WIDTH, self.FACE_MAX_HEIGHT)
            
            # Convert to 1-bit
            return self._convert_to_1bit(img)
            
        except Exception as e:
            log.error(f"Failed to process face {state}: {e}")
            return None
    
    def clear_cache(self):
        """Clear the face bitmap cache"""
        self._face_cache.clear()


# Global face renderer
_face_renderer: Optional[FaceRenderer] = None


def get_face_renderer() -> FaceRenderer:
    """Get or create the global FaceRenderer instance"""
    global _face_renderer
    if _face_renderer is None:
        _face_renderer = FaceRenderer()
    return _face_renderer



def get_voice(event: str, **kwargs) -> str:
    """Convenience function to get voice message"""
    return get_theme_manager().get_voice_message(event, **kwargs)


def list_themes() -> List[str]:
    """Convenience function to list available themes"""
    return get_theme_manager().list_themes()
