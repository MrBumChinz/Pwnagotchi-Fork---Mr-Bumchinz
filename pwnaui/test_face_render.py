#!/usr/bin/env python3
"""
Test face rendering - convert PNG faces to 1-bit for e-ink display.
Creates preview images to verify conversion works correctly.
"""
import sys
sys.path.insert(0, '/home/pi/pwnaui')

from PIL import Image
import os

from python.themes import get_theme_manager, FaceRenderer

OUTPUT_DIR = '/home/pi/pwnaui/face_previews'
os.makedirs(OUTPUT_DIR, exist_ok=True)

tm = get_theme_manager()

# Create a new renderer with NO invert to see the actual result
class TestRenderer(FaceRenderer):
    def _convert_to_1bit(self, img, invert=False):
        # Override to NOT invert, so we see what actually gets displayed
        return super()._convert_to_1bit(img, invert=False)

renderer = TestRenderer(tm)

# Test themes with different face sizes
test_themes = ['rick-sanchez', 'pwnaflipper', 'white-rabbit', 'pwnachu']
test_states = ['HAPPY', 'SAD', 'BORED', 'SLEEP', 'LOOK_R']

print("Rendering face previews...")
print()

for theme_name in test_themes:
    theme = tm.get_theme(theme_name)
    if not theme or not theme.faces_dir:
        print(f"Skipping {theme_name} - no faces")
        continue
    
    print(f"{theme_name}:")
    
    for state in test_states:
        if not theme.has_face(state):
            print(f"  {state}: N/A")
            continue
        
        # Get original size
        orig_path = theme.get_face_path(state)
        orig = Image.open(orig_path)
        
        # Get converted face
        face = renderer.get_face_pil(state, theme_name)
        
        if face:
            # Save preview
            out_path = os.path.join(OUTPUT_DIR, f"{theme_name}_{state}.png")
            face.save(out_path)
            print(f"  {state}: {orig.size} {orig.mode} -> {face.size} {face.mode}")
        else:
            print(f"  {state}: FAILED")
    
    print()

print(f"Previews saved to {OUTPUT_DIR}")
print("Transfer to PC with: scp -r pi@10.0.0.2:/home/pi/pwnaui/face_previews .")
