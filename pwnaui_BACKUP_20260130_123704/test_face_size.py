#!/usr/bin/env python3
"""Test face image properties"""
from PIL import Image
import sys

paths = [
    '/home/pi/pwnaui/themes/rick-sanchez/custom-faces/HAPPY.png',
    '/home/pi/pwnaui/themes/pwnaflipper/faces_flipper_dolphin/HAPPY.png',
    '/home/pi/pwnaui/themes/white-rabbit/_faces/HAPPY.png',
]

for p in paths:
    try:
        img = Image.open(p)
        print(f'{p.split("/")[5]}:')
        print(f'  Size: {img.size}')
        print(f'  Mode: {img.mode}')
        print()
    except Exception as e:
        print(f'{p}: {e}')
