#!/usr/bin/env python3
"""
OpenDash Font Converter
Automatically converts TrueType fonts to LVGL C format using lv_font_conv.

This script:
1. Reads font_config.json
2. Checks if lv_font_conv is available
3. Converts any missing or outdated fonts
4. Generates C source files for LVGL

Usage:
    ./convert_fonts.py              # Convert all fonts in config
    ./convert_fonts.py --force      # Force re-conversion of all fonts
    ./convert_fonts.py --check      # Check setup without converting
"""

import os
import sys
import json
import subprocess
import hashlib
from pathlib import Path
from typing import Dict, List, Optional

# Color codes for terminal output
class Colors:
    HEADER = '\033[95m'
    OKBLUE = '\033[94m'
    OKCYAN = '\033[96m'
    OKGREEN = '\033[92m'
    WARNING = '\033[93m'
    FAIL = '\033[91m'
    ENDC = '\033[0m'
    BOLD = '\033[1m'

def print_info(msg: str):
    """Print info message"""
    print(f"{Colors.OKBLUE}[INFO]{Colors.ENDC} {msg}")

def print_success(msg: str):
    """Print success message"""
    print(f"{Colors.OKGREEN}[SUCCESS]{Colors.ENDC} {msg}")

def print_warning(msg: str):
    """Print warning message"""
    print(f"{Colors.WARNING}[WARNING]{Colors.ENDC} {msg}")

def print_error(msg: str):
    """Print error message"""
    print(f"{Colors.FAIL}[ERROR]{Colors.ENDC} {msg}")

def check_node_installed() -> bool:
    """Check if Node.js is installed"""
    try:
        result = subprocess.run(['node', '--version'], 
                              capture_output=True, text=True, check=True)
        print_success(f"Node.js is installed: {result.stdout.strip()}")
        return True
    except (subprocess.CalledProcessError, FileNotFoundError):
        print_error("Node.js is not installed")
        print_info("Install Node.js: sudo apt-get install nodejs npm")
        return False

def check_lv_font_conv_installed(fonts_dir: Path) -> bool:
    """Check if lv_font_conv is installed (locally or globally)"""
    # Check local installation first
    local_bin = fonts_dir / 'node_modules' / '.bin' / 'lv_font_conv'
    if local_bin.exists():
        try:
            result = subprocess.run([str(local_bin), '--help'], 
                                  capture_output=True, text=True)
            if result.returncode == 0:
                print_success("lv_font_conv is installed locally")
                return True
        except (subprocess.CalledProcessError, FileNotFoundError, OSError) as e:
            print_warning(f"Local lv_font_conv found but failed to run: {e}")
            pass
    
    # Check global installation
    try:
        result = subprocess.run(['lv_font_conv', '--help'], 
                              capture_output=True, text=True)
        if result.returncode == 0:
            print_success("lv_font_conv is installed globally")
            return True
        else:
            print_warning("lv_font_conv found but may not be working correctly")
            return False
    except FileNotFoundError:
        print_warning("lv_font_conv is not installed")
        return False

def install_lv_font_conv(fonts_dir: Path) -> bool:
    """Install lv_font_conv via npm (locally to avoid permission issues)"""
    print_info("Installing lv_font_conv locally via npm...")
    try:
        # Install locally in the fonts directory
        result = subprocess.run(['npm', 'install', 'lv_font_conv'], 
                      cwd=str(fonts_dir),
                      check=True,
                      capture_output=True,
                      text=True)
        print_success("lv_font_conv installed successfully")
        return True
    except subprocess.CalledProcessError as e:
        print_error(f"Failed to install lv_font_conv locally: {e}")
        if e.stderr:
            print_error(f"npm error: {e.stderr}")
        print_info("You can try manually:")
        print_info(f"  cd {fonts_dir}")
        print_info("  npm install lv_font_conv")
        print_info("Or install globally (requires sudo):")
        print_info("  sudo npm install -g lv_font_conv")
        return False

def load_font_config(config_path: Path) -> Optional[Dict]:
    """Load font configuration from JSON file"""
    try:
        with open(config_path, 'r') as f:
            config = json.load(f)
        print_success(f"Loaded font configuration with {len(config.get('fonts', []))} font(s)")
        return config
    except FileNotFoundError:
        print_error(f"Configuration file not found: {config_path}")
        return None
    except json.JSONDecodeError as e:
        print_error(f"Invalid JSON in configuration file: {e}")
        return None

def get_font_hash(font_path: Path) -> str:
    """Calculate MD5 hash of font file for change detection"""
    hasher = hashlib.md5()
    with open(font_path, 'rb') as f:
        hasher.update(f.read())
    return hasher.hexdigest()

def should_convert_font(ttf_path: Path, output_path: Path, force: bool = False) -> bool:
    """Determine if font needs conversion"""
    if force:
        return True
    
    if not output_path.exists():
        return True
    
    # Check if source is newer than output
    if ttf_path.stat().st_mtime > output_path.stat().st_mtime:
        return True
    
    return False

def get_lv_font_conv_command(fonts_dir: Path) -> Optional[str]:
    """Get the lv_font_conv command (local or global)"""
    # Try local installation first
    local_bin = fonts_dir / 'node_modules' / '.bin' / 'lv_font_conv'
    if local_bin.exists():
        return str(local_bin)
    
    # Try global installation
    try:
        result = subprocess.run(['which', 'lv_font_conv'], 
                              capture_output=True, text=True)
        if result.returncode == 0 and result.stdout.strip():
            return 'lv_font_conv'
    except Exception:
        pass
    
    return None

def convert_font(font_config: Dict, ttf_dir: Path, output_dir: Path, 
                fonts_dir: Path, force: bool = False) -> int:
    """Convert a single font to all specified sizes"""
    name = font_config.get('name')
    source = font_config.get('source')
    
    # Skip built-in fonts
    if source == 'built-in':
        print_info(f"Skipping built-in font: {name}")
        return 0
    
    sizes = font_config.get('sizes', [])
    bpp = font_config.get('bpp', 4)
    char_range = font_config.get('range', '0x20-0x7F')
    
    # Check if source font exists
    ttf_path = ttf_dir / source
    if not ttf_path.exists():
        print_error(f"Font file not found: {ttf_path}")
        return 1
    
    print_info(f"Converting font: {name} from {source}")
    
    converted_count = 0
    for size in sizes:
        output_name = f"{name}_{size}"
        output_path = output_dir / f"{output_name}.c"
        
        # Check if conversion is needed
        if not should_convert_font(ttf_path, output_path, force):
            print_info(f"  {output_name}: Up to date, skipping")
            continue
        
        print_info(f"  Converting {output_name} (size={size}, bpp={bpp})...")
        
        # Get lv_font_conv command
        lv_font_conv_cmd = get_lv_font_conv_command(fonts_dir)
        if not lv_font_conv_cmd:
            print_error("lv_font_conv command not found")
            return 1
        
        # Build lv_font_conv command
        cmd = [
            lv_font_conv_cmd,
            '--font', str(ttf_path),
            '--size', str(size),
            '--bpp', str(bpp),
            '--format', 'lvgl',
            '--range', char_range,
            '--output', str(output_path)
        ]
        
        # Run conversion
        try:
            result = subprocess.run(cmd, capture_output=True, text=True, check=True)
            print_success(f"    Generated: {output_path.name}")
            converted_count += 1
        except subprocess.CalledProcessError as e:
            print_error(f"    Failed to convert {output_name}")
            print_error(f"    Error: {e.stderr}")
            return 1
    
    if converted_count == 0:
        print_info(f"  All sizes for {name} are up to date")
    else:
        print_success(f"  Converted {converted_count} size(s) for {name}")
    
    return 0

def main():
    """Main entry point"""
    # Parse command line arguments
    force = '--force' in sys.argv
    check_only = '--check' in sys.argv
    
    print(f"{Colors.BOLD}OpenDash Font Converter{Colors.ENDC}")
    print("=" * 60)
    
    # Get paths
    script_dir = Path(__file__).parent
    ttf_dir = script_dir / 'ttf'
    output_dir = script_dir / 'generated'
    config_path = script_dir / 'font_config.json'
    
    # Check prerequisites
    if not check_node_installed():
        return 1
    
    if not check_lv_font_conv_installed(script_dir):
        if check_only:
            print_info("Skipping installation in check-only mode")
            return 1
        
        print_info("Attempting to install lv_font_conv...")
        if not install_lv_font_conv(script_dir):
            return 1
    
    if check_only:
        print_success("Setup check complete - ready for font conversion")
        return 0
    
    # Load configuration
    config = load_font_config(config_path)
    if config is None:
        return 1
    
    # Ensure output directory exists
    output_dir.mkdir(parents=True, exist_ok=True)
    
    # Convert fonts
    fonts = config.get('fonts', [])
    if not fonts:
        print_warning("No fonts configured in font_config.json")
        return 0
    
    print_info(f"Converting {len(fonts)} font(s)...")
    print("=" * 60)
    
    total_errors = 0
    for font_config in fonts:
        result = convert_font(font_config, ttf_dir, output_dir, script_dir, force)
        total_errors += result
    
    print("=" * 60)
    if total_errors == 0:
        print_success("Font conversion completed successfully!")
        print_info(f"Generated fonts are in: {output_dir}")
        print_info("To use a font in your code:")
        print(f"  {Colors.OKCYAN}LV_FONT_DECLARE(font_name_size);{Colors.ENDC}")
        print(f"  {Colors.OKCYAN}lv_obj_set_style_text_font(obj, &font_name_size, 0);{Colors.ENDC}")
    else:
        print_error(f"Font conversion failed with {total_errors} error(s)")
        return 1
    
    return 0

if __name__ == '__main__':
    sys.exit(main())
