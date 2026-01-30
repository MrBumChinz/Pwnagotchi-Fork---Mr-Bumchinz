# PwnaUI PNG-only face identifiers
# These are just identifiers - pwnaui daemon renders actual PNG faces
# The C daemon (pwnaui) handles theme → image resolution
LOOK_R = 'LOOK_R'
LOOK_L = 'LOOK_L'
LOOK_R_HAPPY = 'LOOK_R_HAPPY'
LOOK_L_HAPPY = 'LOOK_L_HAPPY'
SLEEP = 'SLEEP'
SLEEP2 = 'SLEEP2'
AWAKE = 'AWAKE'
BORED = 'BORED'
INTENSE = 'INTENSE'
COOL = 'COOL'
HAPPY = 'HAPPY'
GRATEFUL = 'GRATEFUL'
EXCITED = 'EXCITED'
MOTIVATED = 'MOTIVATED'
DEMOTIVATED = 'DEMOTIVATED'
SMART = 'SMART'
LONELY = 'LONELY'
SAD = 'SAD'
ANGRY = 'ANGRY'
FRIEND = 'FRIEND'
BROKEN = 'BROKEN'
DEBUG = 'DEBUG'
UPLOAD = 'UPLOAD'
UPLOAD1 = 'UPLOAD1'
UPLOAD2 = 'UPLOAD2'
PNG = True  # Always PNG mode
POSITION_X = 0
POSITION_Y = 40

import logging

# Allowed config keys - everything else is ignored (legacy face paths)
_ALLOWED_CONFIG_KEYS = {'position_x', 'position_y', 'png', 'path', 'theme'}

def load_from_config(config):
    """
    Load face configuration from config.toml [ui.faces] section.
    
    OPTION D FIX: Only reads position_x, position_y, and theme.
    All other entries (legacy face paths like happy=/path/to/file.png) 
    are IGNORED. The C daemon (pwnaui) owns theme→image resolution.
    
    This fixes the bug where config.toml face paths overwrote state
    identifiers, breaking theme switching.
    """
    global PNG, POSITION_X, POSITION_Y
    logging.info('[faces] load_from_config called with %d items' % len(config))
    PNG = True  # Always PNG mode - no ASCII

    for face_name, face_value in config.items():
        if face_name in _ALLOWED_CONFIG_KEYS:
            if face_name == 'position_x':
                POSITION_X = face_value
                logging.debug('[faces] Set POSITION_X = %d' % face_value)
            elif face_name == 'position_y':
                POSITION_Y = face_value
                logging.debug('[faces] Set POSITION_Y = %d' % face_value)
            elif face_name == 'theme':
                logging.info('[faces] Theme specified in config: %s (pwnaui handles this)' % face_value)
            # png and path are silently ignored (legacy)
        else:
            # Legacy face path entry - IGNORE it
            logging.warning('[faces] Ignoring legacy config entry: %s (pwnaui owns themes)' % face_name)

    logging.info('[faces] Configuration loaded - PNG=%s, pos=(%d,%d)' % (PNG, POSITION_X, POSITION_Y))
