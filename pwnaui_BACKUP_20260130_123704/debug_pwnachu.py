#!/usr/bin/env python3
"""Debug pwnachu face rendering"""
import sys
sys.path.insert(0, '/home/pi/pwnaui')

from PIL import Image
import os

# Load pwnachu HAPPY
path = '/home/pi/pwnaui/themes/pwnachu/faces_pwnachu/HAPPY.png'
img = Image.open(path)
print(f"Original: {img.size} mode={img.mode}")
print(f"Extrema: {img.getextrema()}")

# Convert to RGBA if LA
if img.mode == 'LA':
    print("Converting from LA to RGBA...")
    img = img.convert('RGBA')
    print(f"After convert: {img.size} mode={img.mode}")
    print(f"Extrema: {img.getextrema()}")

# Make white background
bg = Image.new('RGBA', img.size, (255, 255, 255, 255))
img = Image.alpha_composite(bg, img)
print(f"After composite: {img.size} mode={img.mode}")

# Convert to grayscale
gray = img.convert('L')
print(f"Gray: {gray.size} mode={gray.mode}")
print(f"Gray extrema: {gray.getextrema()}")

# Threshold
bw = gray.point(lambda x: 255 if x > 128 else 0, '1')
print(f"BW: {bw.size} mode={bw.mode}")

# Save
bw.save('/home/pi/pwnaui/pwnachu_debug.png')
print("Saved /home/pi/pwnaui/pwnachu_debug.png")
