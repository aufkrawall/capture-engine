#!/usr/bin/env python3
"""Convert SPIR-V binary to C array."""
import struct
import sys


def spv_to_c_array(spv_path, var_name):
    with open(spv_path, 'rb') as f:
        data = f.read()

    # Convert to uint32 array
    count = len(data) // 4
    words = struct.unpack(f'<{count}I', data)

    lines = [f'static const uint32_t {var_name}[] = {{']
    for i in range(0, len(words), 4):
        chunk = words[i:i+4]
        hex_vals = ', '.join(f'0x{w:08x}' for w in chunk)
        if i + 4 < len(words):
            lines.append(f'    {hex_vals},')
        else:
            lines.append(f'    {hex_vals}')
    lines.append('};')
    return '\n'.join(lines)


if __name__ == '__main__':
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <spv_file> <var_name>")
        sys.exit(1)

    print(spv_to_c_array(sys.argv[1], sys.argv[2]))
