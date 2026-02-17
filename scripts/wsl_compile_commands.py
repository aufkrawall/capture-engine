#!/usr/bin/env python3
"""
Convert Windows compile_commands.json to WSL-compatible version.
Run this after building to make LSP work in WSL2.
"""

import json
import os
import re

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COMPILE_COMMANDS = os.path.join(PROJECT_ROOT, "compile_commands.json")

def convert_path(win_path):
    """Convert Windows path to WSL path."""
    if not win_path:
        return win_path
    
    # Convert C:\... to /mnt/c/...
    match = re.match(r'^([A-Z]):\\(.*)$', win_path)
    if match:
        drive = match.group(1).lower()
        rest = match.group(2).replace('\\', '/')
        return f"/mnt/{drive}/{rest}"
    
    # Convert C:/... to /mnt/c/...
    match = re.match(r'^([A-Z]):/(.*)$', win_path)
    if match:
        drive = match.group(1).lower()
        rest = match.group(2)
        return f"/mnt/{drive}/{rest}"
    
    return win_path

def convert_argument(arg):
    """Convert a compiler argument, handling -I prefixes."""
    # Handle -I followed by Windows path
    if arg.startswith('-I'):
        path_part = arg[2:]
        converted = convert_path(path_part)
        return f"-I{converted}"
    return convert_path(arg)

def convert_compile_commands(input_file, output_file=None):
    """Convert compile_commands.json from Windows to WSL paths."""
    if output_file is None:
        output_file = input_file
    
    with open(input_file, 'r', encoding='utf-8') as f:
        entries = json.load(f)
    
    converted = []
    for entry in entries:
        new_entry = {}
        for key, value in entry.items():
            if key == 'directory':
                new_entry[key] = convert_path(value)
            elif key == 'file':
                new_entry[key] = convert_path(value)
            elif key == 'arguments':
                new_entry[key] = [convert_argument(arg) for arg in value]
            elif key == 'command':
                new_entry[key] = convert_path(value)
            else:
                new_entry[key] = value
        converted.append(new_entry)
    
    with open(output_file, 'w', encoding='utf-8') as f:
        json.dump(converted, f, indent=4)
    
    print(f"Converted {len(converted)} entries")
    return converted

if __name__ == '__main__':
    convert_compile_commands(COMPILE_COMMANDS)
