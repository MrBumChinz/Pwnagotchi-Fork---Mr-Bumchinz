#!/usr/bin/env python3
"""Test theme discovery"""
import sys
sys.path.insert(0, '/home/pi/pwnaui')

from python.themes import ThemeManager

tm = ThemeManager()
print(f'Found {len(tm.themes)} themes:\n')
for name, theme in sorted(tm.themes.items()):
    d = theme.to_dict()
    print(f'  {name}:')
    print(f'    Faces: {d["face_count"]}')
    print(f'    Dir: {d["faces_dir"]}')
    print(f'    Voice: {d["has_voice"]}')
    print()
