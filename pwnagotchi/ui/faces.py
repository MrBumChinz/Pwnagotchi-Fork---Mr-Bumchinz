# PwnaUI PNG-only face identifiers
# These are just identifiers - pwnaui daemon renders actual PNG faces
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

def load_from_config(config):
    global PNG, POSITION_X, POSITION_Y
    logging.info('[faces] load_from_config called with %d items' % len(config))
    PNG = True  # Always PNG mode - no ASCII

    for face_name, face_value in config.items():
        if face_name in ('position_x', 'position_y', 'png', 'path', 'theme'):
            if face_name == 'position_x':
                POSITION_X = face_value
            elif face_name == 'position_y':
                POSITION_Y = face_value
            continue

        upper_name = face_name.upper()
        logging.info('[faces] Setting %s = %s' % (upper_name, str(face_value)[:50]))
        globals()[upper_name] = face_value

    logging.info('[faces] Done - PNG=%s' % PNG)
