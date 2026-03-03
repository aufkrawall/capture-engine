#!/usr/bin/env python3
"""
Convert Windows compile_commands.json to WSL-compatible version.
Run this after building to make LSP work in WSL2.
"""

import json
import re
from script_safety import resolve_repo_path, write_text_atomic

COMPILE_COMMANDS = resolve_repo_path("compile_commands.json")


def convert_path(win_path):
    """Convert Windows path to WSL path."""
    if not win_path:
        return win_path

    # Convert C:\... or C:/... (including mixed separators) to /mnt/c/...
    match = re.match(r"^([A-Za-z]):[\\/](.*)$", win_path)
    if match:
        drive = match.group(1).lower()
        rest = match.group(2).replace("\\", "/")
        return f"/mnt/{drive}/{rest}"

    # Convert UNC \\server\share\path to /mnt/unc/server/share/path
    if win_path.startswith("\\\\"):
        unc = win_path.lstrip("\\")
        parts = unc.split("\\")
        if len(parts) >= 2:
            server = parts[0]
            share_and_rest = "/".join(parts[1:])
            return f"/mnt/unc/{server}/{share_and_rest}"

    return win_path


def convert_arguments(arguments):
    """Convert compiler arguments, including split include flags."""
    converted = []
    expects_path = False
    split_path_flags = {"-isystem", "-imsvc", "-iquote", "-isysroot"}

    for arg in arguments:
        if expects_path:
            converted.append(convert_path(arg))
            expects_path = False
            continue

        if arg in split_path_flags:
            converted.append(arg)
            expects_path = True
            continue

        if arg.startswith("-I") and len(arg) > 2:
            converted.append(f"-I{convert_path(arg[2:])}")
            continue

        if arg.startswith("/I") and len(arg) > 2:
            converted.append(f"/I{convert_path(arg[2:])}")
            continue

        if arg.startswith("-isystem") and len(arg) > len("-isystem"):
            converted.append(f"-isystem{convert_path(arg[len('-isystem'):])}")
            continue

        converted.append(convert_path(arg))

    return converted


def convert_compile_commands(input_file, output_file=None):
    """Convert compile_commands.json from Windows to WSL paths."""
    if output_file is None:
        output_file = input_file

    input_path = resolve_repo_path(input_file)
    output_path = resolve_repo_path(output_file)

    with open(input_path, "r", encoding="utf-8") as f:
        entries = json.load(f)

    converted = []
    for entry in entries:
        new_entry = {}
        for key, value in entry.items():
            if key == "directory":
                new_entry[key] = convert_path(value)
            elif key == "file":
                new_entry[key] = convert_path(value)
            elif key == "arguments":
                new_entry[key] = convert_arguments(value)
            elif key == "command":
                new_entry[key] = convert_path(value)
            else:
                new_entry[key] = value
        converted.append(new_entry)

    write_text_atomic(output_path, json.dumps(converted, indent=4), newline='\n')

    print(f"Converted {len(converted)} entries")
    return converted


if __name__ == "__main__":
    convert_compile_commands(COMPILE_COMMANDS)
