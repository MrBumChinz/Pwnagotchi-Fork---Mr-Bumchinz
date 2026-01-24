#!/usr/bin/env python3
"""
PwnaUI Theme Downloader

Downloads face themes from various sources and converts them to PwnaUI format.
Supports themes from: https://github.com/roodriiigooo/PWNAGOTCHI-CUSTOM-FACES-MOD

Usage:
    python3 download_themes.py                    # Download all themes
    python3 download_themes.py --theme flipper    # Download specific theme
    python3 download_themes.py --list             # List available themes
"""

import os
import sys
import json
import shutil
import zipfile
import tempfile
import urllib.request
from pathlib import Path
from PIL import Image
import argparse

THEMES_DIR = "/home/pi/pwnaui/themes"
GITHUB_REPO = "roodriiigooo/PWNAGOTCHI-CUSTOM-FACES-MOD"
GITHUB_API = f"https://api.github.com/repos/{GITHUB_REPO}/contents/custom-themes"
GITHUB_RAW = f"https://raw.githubusercontent.com/{GITHUB_REPO}/main/custom-themes"

# Map of theme directory names to display names
AVAILABLE_THEMES = {
    "pwnaflipper": {
        "name": "Flipper Dolphin",
        "faces_dir": "faces_flipper_dolphin",
        "description": "Flipper Zero inspired dolphin faces"
    },
    "pwnachu": {
        "name": "Pikachu",
        "faces_dir": "faces_pwnachu",
        "description": "Pokemon Pikachu themed faces"
    },
    "rick-sanchez": {
        "name": "Rick Sanchez",
        "faces_dir": "faces_rick",
        "description": "Rick & Morty - Rick Sanchez faces"
    },
    "morty-smith": {
        "name": "Morty Smith",
        "faces_dir": "faces_morty",
        "description": "Rick & Morty - Morty Smith faces"
    },
    "pickle-rick": {
        "name": "Pickle Rick",
        "faces_dir": "faces_pickle",
        "description": "Rick & Morty - Pickle Rick faces"
    },
    "fallout-vault-boy": {
        "name": "Vault Boy",
        "faces_dir": "faces_vault_boy",
        "description": "Fallout Vault Boy themed faces"
    },
    "mikugotchi": {
        "name": "Hatsune Miku",
        "faces_dir": "faces_miku",
        "description": "Hatsune Miku themed faces"
    },
    "hologram": {
        "name": "Holo",
        "faces_dir": "holo_faces",
        "description": "Spice and Wolf - Holo themed faces"
    },
    "rebecca": {
        "name": "Rebecca",
        "faces_dir": "faces_rebecca",
        "description": "Cyberpunk Edgerunners - Rebecca faces"
    },
    "retro-computer": {
        "name": "Retro Computer",
        "faces_dir": "_faces",
        "description": "Retro CRT computer style faces"
    },
    "pwnaflowey": {
        "name": "Flowey",
        "faces_dir": "faces_pwnaflowey",
        "description": "Undertale Flowey themed faces"
    },
    "white-rabbit": {
        "name": "White Rabbit",
        "faces_dir": "faces",
        "description": "Matrix-inspired white rabbit faces"
    },
}

# Face state file names (what we need vs what repos have)
FACE_FILES = [
    "LOOK_R", "LOOK_L", "LOOK_R_HAPPY", "LOOK_L_HAPPY",
    "SLEEP", "SLEEP2", "AWAKE", "BORED", "INTENSE", "COOL",
    "HAPPY", "EXCITED", "GRATEFUL", "MOTIVATED", "DEMOTIVATED",
    "SMART", "LONELY", "SAD", "ANGRY", "FRIEND", "BROKEN",
    "DEBUG", "UPLOAD", "UPLOAD1", "UPLOAD2"
]


def download_file(url: str, dest: str) -> bool:
    """Download a file from URL"""
    try:
        print(f"  Downloading: {os.path.basename(dest)}")
        urllib.request.urlretrieve(url, dest)
        return True
    except Exception as e:
        print(f"  Warning: Failed to download {url}: {e}")
        return False


def convert_png_to_1bit(src_path: str, dest_path: str, threshold: int = 128) -> bool:
    """
    Convert PNG to 1-bit for e-ink display.
    
    Args:
        src_path: Source PNG file
        dest_path: Destination PNG file
        threshold: Grayscale threshold (0-255)
    """
    try:
        img = Image.open(src_path)
        
        # Handle transparency - composite on white background
        if img.mode in ('RGBA', 'LA') or (img.mode == 'P' and 'transparency' in img.info):
            background = Image.new('RGBA', img.size, (255, 255, 255, 255))
            if img.mode == 'P':
                img = img.convert('RGBA')
            background.paste(img, mask=img.split()[-1])  # Use alpha channel as mask
            img = background.convert('RGB')
        else:
            img = img.convert('RGB')
        
        # Convert to grayscale
        img_gray = img.convert('L')
        
        # Apply threshold to get pure black and white
        img_bw = img_gray.point(lambda x: 255 if x > threshold else 0, mode='1')
        
        # Save as 1-bit PNG
        img_bw.save(dest_path)
        return True
        
    except Exception as e:
        print(f"  Warning: Failed to convert {src_path}: {e}")
        return False


def download_theme(theme_id: str, themes_dir: str = THEMES_DIR) -> bool:
    """
    Download and convert a theme.
    
    Args:
        theme_id: Theme ID (e.g., "pwnaflipper")
        themes_dir: Destination themes directory
    """
    if theme_id not in AVAILABLE_THEMES:
        print(f"Unknown theme: {theme_id}")
        print(f"Available: {', '.join(AVAILABLE_THEMES.keys())}")
        return False
    
    theme_info = AVAILABLE_THEMES[theme_id]
    theme_name = theme_info["name"]
    faces_subdir = theme_info.get("faces_dir", "faces")
    
    print(f"\nDownloading theme: {theme_name} ({theme_id})")
    
    # Create theme directory
    dest_dir = os.path.join(themes_dir, theme_id)
    os.makedirs(dest_dir, exist_ok=True)
    
    # Create temp directory for raw downloads
    with tempfile.TemporaryDirectory() as tmp_dir:
        # Download face images
        base_url = f"{GITHUB_RAW}/{theme_id}/{faces_subdir}"
        downloaded = 0
        
        for face in FACE_FILES:
            # Try different file name patterns
            patterns = [
                f"{face}.png",
                f"{face.lower()}.png",
                f"{face.replace('_', '-')}.png",
                f"{face.lower().replace('_', '-')}.png",
            ]
            
            success = False
            for pattern in patterns:
                url = f"{base_url}/{pattern}"
                tmp_path = os.path.join(tmp_dir, f"{face}.png")
                
                try:
                    urllib.request.urlretrieve(url, tmp_path)
                    success = True
                    break
                except:
                    continue
            
            if success:
                # Convert to 1-bit
                dest_path = os.path.join(dest_dir, f"{face}.png")
                if convert_png_to_1bit(tmp_path, dest_path):
                    downloaded += 1
        
        # Try to download voice.py
        voice_url = f"{GITHUB_RAW}/{theme_id}/voice.py"
        voice_dest = os.path.join(dest_dir, "voice.py")
        try:
            urllib.request.urlretrieve(voice_url, voice_dest)
            print(f"  Downloaded voice.py")
        except:
            print(f"  No voice.py found")
        
        # Create theme metadata
        meta = {
            "name": theme_name,
            "id": theme_id,
            "description": theme_info.get("description", ""),
            "source": f"https://github.com/{GITHUB_REPO}/tree/main/custom-themes/{theme_id}",
            "face_count": downloaded,
        }
        
        meta_path = os.path.join(dest_dir, "theme.json")
        with open(meta_path, 'w') as f:
            json.dump(meta, f, indent=2)
    
    print(f"  Installed {downloaded}/{len(FACE_FILES)} faces to {dest_dir}")
    return downloaded > 0


def list_themes():
    """List available themes for download"""
    print("\nAvailable themes:")
    print("-" * 60)
    for theme_id, info in AVAILABLE_THEMES.items():
        print(f"  {theme_id:20} - {info['name']}")
        if info.get('description'):
            print(f"  {' ' * 20}   {info['description']}")
    print()


def download_all_themes(themes_dir: str = THEMES_DIR):
    """Download all available themes"""
    os.makedirs(themes_dir, exist_ok=True)
    
    success = 0
    for theme_id in AVAILABLE_THEMES:
        if download_theme(theme_id, themes_dir):
            success += 1
    
    print(f"\nDownloaded {success}/{len(AVAILABLE_THEMES)} themes")


def create_default_theme(themes_dir: str = THEMES_DIR):
    """Create a default theme with simple text-based faces"""
    print("\nCreating default theme...")
    
    dest_dir = os.path.join(themes_dir, "default")
    os.makedirs(dest_dir, exist_ok=True)
    
    # Create simple face images programmatically
    # These are just placeholders - you'd want actual face art
    
    face_texts = {
        "HAPPY": "(◕‿◕)",
        "SAD": "(╥_╥)",
        "BORED": "(-_-)",
        "ANGRY": "(>_<)",
        "COOL": "(⌐■_■)",
        "SLEEP": "(-_-) zzZ",
        "AWAKE": "(◕◡◕)",
        "EXCITED": "(ᵔ◡ᵔ)",
        # ... more faces
    }
    
    # Create theme metadata
    meta = {
        "name": "Default",
        "id": "default",
        "description": "Default PwnaUI theme",
        "face_count": 0,  # Will be updated
    }
    
    meta_path = os.path.join(dest_dir, "theme.json")
    with open(meta_path, 'w') as f:
        json.dump(meta, f, indent=2)
    
    print(f"  Created default theme at {dest_dir}")


def main():
    parser = argparse.ArgumentParser(description="Download PwnaUI face themes")
    parser.add_argument("--list", action="store_true", help="List available themes")
    parser.add_argument("--theme", "-t", type=str, help="Download specific theme")
    parser.add_argument("--all", "-a", action="store_true", help="Download all themes")
    parser.add_argument("--dir", "-d", type=str, default=THEMES_DIR, help="Themes directory")
    parser.add_argument("--default", action="store_true", help="Create default theme")
    
    args = parser.parse_args()
    
    if args.list:
        list_themes()
    elif args.theme:
        download_theme(args.theme, args.dir)
    elif args.all:
        download_all_themes(args.dir)
    elif args.default:
        create_default_theme(args.dir)
    else:
        # Default: show help
        parser.print_help()
        print("\nExamples:")
        print("  python3 download_themes.py --list")
        print("  python3 download_themes.py --theme pwnaflipper")
        print("  python3 download_themes.py --all")


if __name__ == "__main__":
    main()
