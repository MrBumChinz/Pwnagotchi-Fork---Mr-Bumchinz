import os
import logging
from threading import Lock

frame_path = '/var/tmp/pwnagotchi/pwnagotchi.png'
frame_format = 'PNG'
frame_ctype = 'image/png'
frame_lock = Lock()

# Cache for PwnaUI state rendering
_last_state = {}
_pwnaui_mode = False


def update_frame(img):
    """
    Update the web UI frame.
    
    In PIL mode: img is a PIL Image object
    In PwnaUI mode: img is None, we generate from IPC state
    """
    global frame_lock, frame_path, frame_format, _pwnaui_mode
    
    if not os.path.exists(os.path.dirname(frame_path)):
        os.makedirs(os.path.dirname(frame_path))
    
    with frame_lock:
        if img is not None:
            # Traditional PIL mode
            img.save(frame_path, format=frame_format)
        else:
            # PwnaUI mode - generate a simple status image
            _pwnaui_mode = True
            _generate_pwnaui_frame()


def _generate_pwnaui_frame():
    """Generate a PNG frame for web UI when using PwnaUI"""
    global frame_path, _last_state
    
    try:
        # Try to get state from PwnaUI IPC
        from pwnagotchi.ui.hw import pwnaui as _pwnaui
        state = _pwnaui.get_state() if hasattr(_pwnaui, 'get_state') else {}
        _last_state = state
    except:
        state = _last_state
    
    try:
        from PIL import Image, ImageDraw, ImageFont
        
        # Create e-paper sized image (250x122 for waveshare 2.13 v4)
        width, height = 250, 122
        img = Image.new('1', (width, height), 1)  # 1-bit, white background
        draw = ImageDraw.Draw(img)
        
        # Try to load a font
        try:
            font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 12)
            font_small = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 10)
        except:
            font = ImageFont.load_default()
            font_small = font
        
        # Get state values
        face = state.get('face', '(•_•)')
        name = state.get('name', 'pwnagotchi')
        status = state.get('status', 'PwnaUI Mode')
        uptime = state.get('uptime', '00:00:00:00')
        mode = state.get('mode', 'MANU')
        channel = state.get('channel', '*')
        aps = state.get('aps', '0')
        
        # Draw face (centered, large)
        face_bbox = draw.textbbox((0, 0), face, font=font)
        face_width = face_bbox[2] - face_bbox[0]
        draw.text(((width - face_width) // 2, 35), face, font=font, fill=0)
        
        # Draw name at top
        draw.text((5, 5), name, font=font, fill=0)
        
        # Draw status below face
        draw.text((5, 70), status[:35], font=font_small, fill=0)
        
        # Draw stats at bottom
        stats = f"CH:{channel} APs:{aps} Up:{uptime}"
        draw.text((5, 100), stats, font=font_small, fill=0)
        
        # Draw mode indicator
        draw.text((width - 40, 5), mode, font=font_small, fill=0)
        
        # Save
        img.save(frame_path, format='PNG')
        
    except ImportError:
        # PIL not available - create minimal placeholder
        _create_placeholder_frame()
    except Exception as e:
        logging.error(f"Error generating pwnaui frame: {e}")
        _create_placeholder_frame()


def _create_placeholder_frame():
    """Create a minimal placeholder PNG when PIL is not available"""
    global frame_path
    
    # Minimal valid 1x1 white PNG
    png_data = bytes([
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,  # PNG signature
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,  # IHDR chunk
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,  # 1x1
        0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53,
        0xDE, 0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41,  # IDAT chunk
        0x54, 0x08, 0xD7, 0x63, 0xF8, 0xFF, 0xFF, 0xFF,
        0x00, 0x05, 0xFE, 0x02, 0xFE, 0xDC, 0xCC, 0x59,
        0xE7, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E,  # IEND chunk
        0x44, 0xAE, 0x42, 0x60, 0x82
    ])
    
    try:
        with open(frame_path, 'wb') as f:
            f.write(png_data)
    except Exception as e:
        logging.error(f"Error creating placeholder: {e}")


def set_state(key, value):
    """Update state cache for web frame generation"""
    global _last_state
    _last_state[key] = value
