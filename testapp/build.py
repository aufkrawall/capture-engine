#!/usr/bin/env python3
"""
Build script for DX12 and Vulkan test apps
Output: testapp/bin/dx12_test.exe, testapp/bin/vulkan_test.exe

Usage:
    python build.py          # Build both test apps
    python build.py dx12     # Build only DX12 test app
    python build.py vulkan   # Build only Vulkan test app
"""

import os
import sys
import subprocess
from pathlib import Path

# Get absolute paths
SCRIPT_DIR = Path(__file__).parent.absolute()
PROJECT_ROOT = SCRIPT_DIR.parent
BIN_DIR = SCRIPT_DIR / "bin"
MSYS_PATH = PROJECT_ROOT / "build" / "msys64" / "clang64" / "bin"

# Ensure output directory exists
BIN_DIR.mkdir(parents=True, exist_ok=True)

def find_compiler():
    """Find clang++ compiler"""
    # Try MSYS2 path first
    clang_msys = MSYS_PATH / "clang++.exe"
    if clang_msys.exists():
        return str(clang_msys)
    
    # Fall back to system PATH
    try:
        result = subprocess.run(["clang++", "--version"], capture_output=True)
        if result.returncode == 0:
            return "clang++"
    except FileNotFoundError:
        pass
    
    # Try cl.exe (MSVC)
    try:
        result = subprocess.run(["cl"], capture_output=True)
        return "cl"
    except FileNotFoundError:
        pass
    
    print("ERROR: No C++ compiler found (clang++ or cl)")
    sys.exit(1)

def find_vulkan_sdk():
    """Find Vulkan SDK include path"""
    vulkan_sdk = os.environ.get("VULKAN_SDK")
    if vulkan_sdk:
        return Path(vulkan_sdk) / "Include"
    
    # Try common paths
    common_paths = [
        Path("C:/VulkanSDK"),
        Path(os.environ.get("LOCALAPPDATA", "")) / "VulkanSDK",
    ]
    
    for base in common_paths:
        if base.exists():
            # Find latest version
            versions = sorted([d for d in base.iterdir() if d.is_dir()], reverse=True)
            if versions:
                return versions[0] / "Include"
    
    return None

def build_dx12(compiler):
    """Build DX12 test app"""
    print("[Building DX12 test app...]")
    
    src = SCRIPT_DIR / "dx12_test.cpp"
    out = BIN_DIR / "dx12_test.exe"
    
    if compiler == "cl":
        cmd = [
            "cl", "/EHsc", "/O2", "/std:c++17",
            str(src),
            "/Fe:" + str(out),
            "/link", "d3d12.lib", "dxgi.lib", "d3dcompiler.lib", "shcore.lib"
        ]
    else:
        cmd = [
            compiler, "-std=c++17", "-O2",
            "-o", str(out),
            str(src),
            "-ld3d12", "-ldxgi", "-ld3dcompiler", "-lshcore",
            "-luser32", "-lgdi32", "-lshell32",
            "-static", "-static-libstdc++", "-static-libgcc"
        ]
    
    print(f"  Command: {' '.join(cmd[:5])}...")
    result = subprocess.run(cmd, capture_output=True, text=True)
    
    if result.returncode != 0:
        print(f"  ERROR: {result.stderr}")
        return False
    
    print(f"  Output: {out}")
    return True

def build_vulkan(compiler):
    """Build Vulkan test app"""
    print("[Building Vulkan test app...]")
    
    vulkan_include = find_vulkan_sdk()
    if not vulkan_include:
        print("  WARNING: Vulkan SDK not found, trying anyway...")
        vulkan_include = None
    else:
        print(f"  Vulkan SDK: {vulkan_include.parent}")
    
    src = SCRIPT_DIR / "vulkan_test.cpp"
    out = BIN_DIR / "vulkan_test.exe"
    
    if compiler == "cl":
        cmd = [
            "cl", "/EHsc", "/O2", "/std:c++17",
            f"/I{vulkan_include}" if vulkan_include else "",
            str(src),
            "/Fe:" + str(out),
            "/link", "user32.lib", "gdi32.lib", "shcore.lib"
        ]
        cmd = [c for c in cmd if c]  # Remove empty strings
    else:
        cmd = [
            compiler, "-std=c++17", "-O2",
        ]
        if vulkan_include:
            cmd.extend(["-I", str(vulkan_include)])
        cmd.extend([
            "-o", str(out),
            str(src),
            "-luser32", "-lgdi32", "-lshcore",
            "-static", "-static-libstdc++", "-static-libgcc"
        ])
    
    print(f"  Command: {' '.join(cmd[:5])}...")
    result = subprocess.run(cmd, capture_output=True, text=True)
    
    if result.returncode != 0:
        print(f"  ERROR: {result.stderr}")
        return False
    
    print(f"  Output: {out}")
    return True

def main():
    print("=" * 50)
    print("Test App Build Script")
    print("=" * 50)
    
    # Parse args
    build_targets = sys.argv[1:] if len(sys.argv) > 1 else ["dx12", "vulkan"]
    
    # Find compiler
    compiler = find_compiler()
    print(f"Compiler: {compiler}")
    print()
    
    success = True
    
    if "dx12" in build_targets:
        if not build_dx12(compiler):
            success = False
    
    if "vulkan" in build_targets:
        if not build_vulkan(compiler):
            success = False
    
    print()
    if success:
        print("Build complete!")
        print()
        print("Usage:")
        print(f"  {BIN_DIR / 'dx12_test.exe'} [width] [height] [gpu_load]")
        print(f"  {BIN_DIR / 'vulkan_test.exe'} [width] [height] [gpu_load]")
        print()
        print("Examples:")
        print("  dx12_test.exe 3840 2160 15   # 4K with moderate GPU load")
        print("  dx12_test.exe 1920 1080 5    # 1080p with light GPU load")
    else:
        print("Build failed!")
        sys.exit(1)

if __name__ == "__main__":
    main()
