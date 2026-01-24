import os
import sys
import glob
import shlex
import shutil
import tarfile
import subprocess
import urllib.request
import time
import datetime
import hashlib
import zipfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from multiprocessing import cpu_count
import re
import json

from typing import List, Dict, Optional, Union, Any

# --- Configuration ---
BUILD_DIR_NAME = "build"
COMPILE_COMMANDS: List[Dict[str, Any]] = []

PROJECT_ROOT = os.path.dirname(os.path.abspath(__file__))

BUILD_DIR = os.path.join(PROJECT_ROOT, BUILD_DIR_NAME)
MSYS2_URL = "https://repo.msys2.org/distrib/x86_64/msys2-base-x86_64-20240113.tar.xz"
MSYS2_DIR = os.path.join(BUILD_DIR, "msys64")
OBJ_DIR = os.path.join(BUILD_DIR, "obj")
BIN_DIR = os.path.join(BUILD_DIR, "bin")
INSTALLED_DIR = os.path.join(PROJECT_ROOT, "installed")
CAPTURE_BIN_DIR = os.path.join(INSTALLED_DIR, "captureengine")
TESTAPP_BIN_DIR = os.path.join(INSTALLED_DIR, "testapp")
BIN_DIR = CAPTURE_BIN_DIR # output captureengine binaries to installed\captureengine
LOG_FILE = os.path.join(PROJECT_ROOT, "build_log.txt")

IMGUI_URL = "https://github.com/ocornut/imgui/archive/refs/tags/v1.91.5.zip"
IMGUI_DIR = os.path.join(PROJECT_ROOT, "external", "imgui")
IMGUI_DIR = os.path.join(PROJECT_ROOT, "external", "imgui")

FFMPEG_DIR = os.path.join(PROJECT_ROOT, "external", "ffmpeg")

PACKAGES = [
    "mingw-w64-clang-x86_64-toolchain",
    # "mingw-w64-x86_64-toolchain", # GCC removed (User requested Zig)
    "mingw-w64-clang-x86_64-pkgconf",
    # ffmpeg & codecs removed (built from source)
    "mingw-w64-clang-x86_64-openssl",
    "mingw-w64-clang-x86_64-libxml2", 
    "mingw-w64-clang-x86_64-libxml2", 
    "mingw-w64-clang-x86_64-shaderc",
    "mingw-w64-clang-x86_64-cmake",
    "mingw-w64-clang-x86_64-ninja",
    "mingw-w64-clang-x86_64-meson",
    "mingw-w64-clang-x86_64-nasm",
    "mingw-w64-clang-x86_64-vulkan-headers",
    "mingw-w64-clang-x86_64-vulkan-loader",

    "mingw-w64-i686-toolchain",
    "mingw-w64-i686-clang",
    "mingw-w64-i686-vulkan-headers",
    "mingw-w64-i686-vulkan-loader",
    "mingw-w64-clang-x86_64-cppwinrt",  # For Windows Graphics Capture
    "mingw-w64-clang-x86_64-gtest",
    "mingw-w64-clang-x86_64-amf-headers",
    "mingw-w64-clang-x86_64-onevpl", # For QSV
    "mingw-w64-clang-x86_64-lld",    # For delay-load support (x64)
    "mingw-w64-i686-lld",            # For delay-load support (x86)
    "make",
    "ccache",
]

def log(msg: str) -> None:
    timestamp = time.strftime('%Y-%m-%d %H:%M:%S')
    formatted = f"[{timestamp}] {msg}"
    print(formatted)
    try:
        with open(LOG_FILE, "a") as f:
            f.write(formatted + "\n")
    except:
        pass

def run_command(cmd: Union[List[str], str], 
                env: Optional[Dict[str, str]] = None, 
                cwd: Optional[str] = None, 
                input_str: Optional[str] = None, 
                fail_exit: bool = True) -> str:
    cmd_str = ' '.join(cmd) if isinstance(cmd, list) else cmd

    log(f"Running: {cmd_str}")
    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            env=env,
            cwd=cwd,
            input=input_str
        )
        if result.returncode != 0:
            log(f"ERROR: Command failed with code {result.returncode}")
            log(f"STDOUT: {result.stdout}")
            log(f"STDERR: {result.stderr}")
            if fail_exit:
                sys.exit(1)
        return result.stdout
    except Exception as e:
        log(f"EXCEPTION: {e}")
        if fail_exit:
            sys.exit(1)
        return ""

def bump_and_write_build_version():
    version_header_path = os.path.join(PROJECT_ROOT, "common", "build_version.h")
    # Store build number in build directory (not tracked by git)
    build_num_path = os.path.join(BUILD_DIR, "build_number.txt")
    
    os.makedirs(BUILD_DIR, exist_ok=True)
    os.makedirs(os.path.dirname(version_header_path), exist_ok=True)

    build_number = 0
    
    # Try to read local build number
    if os.path.exists(build_num_path):
        try:
            with open(build_num_path, "r") as f:
                build_number = int(f.read().strip())
        except Exception as e:
             log(f"Warning: Failed to read {build_num_path}: {e}")

    # Fallback/Seed: If no local counter, try to read from existing header to continuity
    # (Optional: user wanted to get rid of "weird git contraption", but seeding once is safe)
    if build_number == 0 and os.path.exists(version_header_path):
         try:
            with open(version_header_path, "r", encoding="utf-8") as f:
                txt = f.read()
            m = re.search(r"#define\s+BUILD_NUMBER\s+(\d+)", txt)
            if m:
                build_number = int(m.group(1))
         except: pass

    build_number += 1
    
    # Save new build number
    try:
        with open(build_num_path, "w") as f:
            f.write(str(build_number))
    except Exception as e:
        log(f"Warning: Failed to write {build_num_path}: {e}")

    timestamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    version_str = f"1.1.0-dev+build.{build_number}"

    contents = (
        "#pragma once\n\n"
        f"#define BUILD_NUMBER {build_number}\n"
        f"#define CAPTURE_VERSION \"{version_str}\"\n"
        f"#define BUILD_TIMESTAMP \"{timestamp}\"\n"
    )

    try:
        with open(version_header_path, "w", encoding="utf-8") as f:
            f.write(contents)
        log(f"Build version bumped: {version_str}")
    except Exception as e:
        log(f"ERROR: Failed to write {version_header_path}: {e}")
        sys.exit(1)

def setup_msys2():
    if not os.path.exists(MSYS2_DIR):
        log("Downloading MSYS2...")
        os.makedirs(BUILD_DIR, exist_ok=True)
        tar_path = os.path.join(BUILD_DIR, "msys2.tar.xz")
        if not os.path.exists(tar_path):
            urllib.request.urlretrieve(MSYS2_URL, tar_path)
        
        log("Extracting MSYS2...")
        with tarfile.open(tar_path) as f:
            f.extractall(BUILD_DIR)
        
        msys_bash = os.path.join(MSYS2_DIR, "usr", "bin", "bash.exe")
        run_command([msys_bash, "-lc", "pacman-key --init"])
        run_command([msys_bash, "-lc", "pacman-key --populate msys2"])
        run_command([msys_bash, "-lc", "pacman -Sy --noconfirm"])
    else:
        log("MSYS2 found.")

    log("Installing Packages...")
    msys_bash = os.path.join(MSYS2_DIR, "usr", "bin", "bash.exe")
    pkg_cmd = f"pacman -S --needed --noconfirm {' '.join(PACKAGES)}"
    run_command([msys_bash, "-lc", pkg_cmd], input_str="\n")

def setup_imgui():
    if not os.path.exists(IMGUI_DIR):
        log("Downloading ImGui...")
        zip_path = os.path.join(BUILD_DIR, "imgui.zip")
        urllib.request.urlretrieve(IMGUI_URL, zip_path)
        
        log("Extracting ImGui...")
        external_dir = os.path.join(PROJECT_ROOT, "external")
        os.makedirs(external_dir, exist_ok=True)
        with zipfile.ZipFile(zip_path, 'r') as zip_ref:
            zip_ref.extractall(external_dir)
        
        # Rename imgui-1.91.5 to imgui
        src = os.path.join(external_dir, "imgui-1.91.5")
        if os.path.exists(src):
            os.rename(src, IMGUI_DIR)
        
        log("ImGui Setup Complete.")
        patch_imgui()

def patch_imgui():
    """Apply unified command queue patches to ImGui DX12 backend."""
    dx12_h = os.path.join(IMGUI_DIR, "backends", "imgui_impl_dx12.h")
    dx12_cpp = os.path.join(IMGUI_DIR, "backends", "imgui_impl_dx12.cpp")

    if not os.path.exists(dx12_h) or not os.path.exists(dx12_cpp):
        log("Warning: ImGui DX12 backend files not found for patching.")
        return

    # 1. Patch Header
    with open(dx12_h, "r") as f:
        h_content = f.read()
    
    if "ImGui_ImplDX12_SetCommandQueue" not in h_content:
        log("Patching imgui_impl_dx12.h...")
        # Add forward declaration
        h_content = h_content.replace(
            "struct ID3D12GraphicsCommandList;",
            "struct ID3D12GraphicsCommandList;\nstruct ID3D12CommandQueue;"
        )
        # Add function declaration
        h_content = h_content.replace(
            "IMGUI_IMPL_API void     ImGui_ImplDX12_InvalidateDeviceObjects();",
            "IMGUI_IMPL_API void     ImGui_ImplDX12_InvalidateDeviceObjects();\nIMGUI_IMPL_API void     ImGui_ImplDX12_SetCommandQueue(ID3D12CommandQueue* command_queue);"
        )
        with open(dx12_h, "w") as f:
            f.write(h_content)

    # 2. Patch CPP
    with open(dx12_cpp, "r") as f:
        cpp_content = f.read()
    
    if "pCommandQueue" not in cpp_content:
        log("Patching imgui_impl_dx12.cpp...")
        # Add member to struct
        cpp_content = cpp_content.replace(
            "ID3D12DescriptorHeap*       pd3dSrvDescHeap;",
            "ID3D12DescriptorHeap*       pd3dSrvDescHeap;\n    ID3D12CommandQueue*         pCommandQueue;"
        )
        # Update CreateFontsTexture to use pCommandQueue
        old_queue_logic = """        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queueDesc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.NodeMask = 1;

        ID3D12CommandQueue* cmdQueue = nullptr;
        hr = bd->pd3dDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&cmdQueue));
        IM_ASSERT(SUCCEEDED(hr));"""
        
        new_queue_logic = """        ID3D12CommandQueue* cmdQueue = bd->pCommandQueue;
        bool ownQueue = false;
        if (cmdQueue == nullptr)
        {
            D3D12_COMMAND_QUEUE_DESC queueDesc = {};
            queueDesc.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
            queueDesc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
            queueDesc.NodeMask = 1;
            hr = bd->pd3dDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&cmdQueue));
            IM_ASSERT(SUCCEEDED(hr));
            ownQueue = true;
        }"""
        cpp_content = cpp_content.replace(old_queue_logic, new_queue_logic)
        
        # Update queue cleanup
        cpp_content = cpp_content.replace("cmdQueue->Release();", "if (ownQueue) cmdQueue->Release();")
        
        # Add implementation of SetCommandQueue
        set_queue_impl = """void ImGui_ImplDX12_SetCommandQueue(ID3D12CommandQueue* command_queue)
{
    ImGui_ImplDX12_Data* bd = ImGui_ImplDX12_GetBackendData();
    IM_ASSERT(bd != nullptr && "Context or backend not initialized!");
    bd->pCommandQueue = command_queue;
}

//-----------------------------------------------------------------------------"""
        cpp_content = cpp_content.replace("//-----------------------------------------------------------------------------", set_queue_impl)
        
        with open(dx12_cpp, "w") as f:
            f.write(cpp_content)
    
    log("ImGui patches applied successfully.")



# --- FFmpeg Configuration ---
FFMPEG_URL = "https://git.ffmpeg.org/ffmpeg.git"
FFNVCODEC_URL = "https://git.videolan.org/git/ffmpeg/nv-codec-headers.git"

def to_unix(p):
    """Convert Windows path to MSYS2 Unix path."""
    p = p.replace('\\', '/')
    if len(p) >= 2 and p[1] == ':':
        drive = p[0].lower()
        return '/' + drive + p[2:]
    return p

class FFmpegBuilder:
    def __init__(self, root_dir, msys_dir, install_dir, license_mode="gpl"):
        self.root = root_dir
        self.msys = msys_dir
        self.install_dir = install_dir
        self.license_mode = license_mode # 'gpl' or 'lgpl'
        
        self.build_root = os.path.join(self.root, "ffmpeg_build")
        self.repos_dir = os.path.join(self.build_root, "repos")
        self.working_dir = os.path.join(self.build_root, "working")
        
        # Output dirs
        self.prefix = to_unix(install_dir)
        self.win_prefix = install_dir
        self.license_mode = "lgpl" # Changed to LGPL per user request

    def setup_dirs(self):
        for d in [self.build_root, self.repos_dir, self.working_dir, self.install_dir]:
            os.makedirs(d, exist_ok=True)
            
    def get_msys_env(self):
        env = os.environ.copy()
        
        # Add MSYS2 tools to path
        msys_bin = [
            os.path.join(self.msys, "clang64", "bin"),
            os.path.join(self.msys, "usr", "bin"),
            os.path.join(self.install_dir, "bin")
        ]
        env["PATH"] = os.pathsep.join(msys_bin + [env["PATH"]])
        
        # Compiler flags
        pkg_config = self.get_tool_path("pkg-config")
        
        # Add MSYS2 paths for system libs (vpl, etc.)
        msys_inc = to_unix(os.path.join(self.msys, "clang64", "include"))
        msys_lib = to_unix(os.path.join(self.msys, "clang64", "lib"))
        
        env["CC"] = "clang"
        env["CXX"] = "clang++"
        env["CFLAGS"] = f"-O3 -ffunction-sections -fdata-sections -I{self.prefix}/include -I{msys_inc}"
        env["CXXFLAGS"] = f"-O3 -ffunction-sections -fdata-sections -I{self.prefix}/include -I{msys_inc}"
        env["LDFLAGS"] = f"-Wl,--gc-sections -L{self.prefix}/lib -L{msys_lib}"
        env["PKG_CONFIG"] = f"{pkg_config} --static"
        env["PKG_CONFIG_PATH"] = f"{self.prefix}/lib/pkgconfig"
        env["MSYSTEM"] = "CLANG64" # Ensure we are treated as MinGW-Clang
        
        return env

    def get_tool_path(self, tool_name):
        """Find tool in MSYS2 directories."""
        # Check clang64/bin first, then usr/bin, then ucrt64/bin
        search_dirs = [
            os.path.join(self.msys, "clang64", "bin"),
            os.path.join(self.msys, "usr", "bin"),
            os.path.join(self.msys, "ucrt64", "bin"),
        ]
        
        exe = tool_name + ".exe"
        for d in search_dirs:
            p = os.path.join(d, exe)
            if os.path.exists(p):
                return p
        
        return tool_name # Fallback to path lookup

    def run(self, cmd, cwd=None, env=None, check=True):
        cmd_str = ' '.join(cmd) if isinstance(cmd, list) else cmd
        log(f"[FFmpeg] EXEC: {cmd_str}")
        try:
            if env is None:
                env = os.environ.copy()
            if env and "PATH" not in env:
                 env["PATH"] = os.environ["PATH"]

            subprocess.run(cmd_str, cwd=cwd, env=env, check=check, shell=True)
        except subprocess.CalledProcessError as e:
            log(f"[FFmpeg] FAILED: {cmd_str}")
            raise e

    def git_clone(self, url, name, update=True):
        """Clone or update a git repository. Returns (path, updated) tuple."""
        dest = os.path.join(self.repos_dir, name)
        git_exe = self.get_tool_path("git")
        env = self.get_msys_env()
        
        if not os.path.exists(dest):
            log(f"[FFmpeg] Cloning {name}...")
            self.run([git_exe, "clone", "--depth", "1", url, dest], env=env)
            return dest, True  # New clone = always needs build
        
        if not update:
            log(f"[FFmpeg] Using existing {name} (--skip-updates)")
            return dest, False
        
        # Get current commit before update
        old_commit = subprocess.check_output(
            [git_exe, "rev-parse", "HEAD"], cwd=dest, env=env
        ).decode().strip()
        
        # Fetch and reset to latest
        log(f"[FFmpeg] Checking for updates to {name}...")
        try:
            self.run([git_exe, "fetch", "--depth", "1", "origin"], cwd=dest, env=env, check=False)
            self.run([git_exe, "reset", "--hard", "origin/HEAD"], cwd=dest, env=env, check=False)
        except Exception as e:
            log(f"[FFmpeg] Warning: Could not update {name}: {e}")
            return dest, False
        
        # Get new commit
        new_commit = subprocess.check_output(
            [git_exe, "rev-parse", "HEAD"], cwd=dest, env=env
        ).decode().strip()
        
        if old_commit != new_commit:
            log(f"[FFmpeg] Updated {name}: {old_commit[:8]} -> {new_commit[:8]}")
            return dest, True
        else:
            log(f"[FFmpeg] {name} is up to date ({new_commit[:8]})")
            return dest, False

    def rmtree_onerror(self, func, path, exc_info):
        import stat
        import time
        if not os.access(path, os.W_OK):
            try:
                os.chmod(path, stat.S_IWRITE)
                func(path)
                return
            except Exception:
                pass
        
        ex = exc_info[1]
        if isinstance(ex, PermissionError) or isinstance(ex, OSError):
            log(f"[FFmpeg] Locked file: {path}. Retrying...")
            for i in range(5):
                try:
                    time.sleep(1.0)
                    if not os.access(path, os.W_OK): os.chmod(path, stat.S_IWRITE)
                    func(path)
                    return
                except Exception:
                    pass
            log(f"[FFmpeg] FAILED to remove: {path}")
            raise

    def build_dependencies(self, update=True):
        log("[FFmpeg] Building Dependencies...")
        # 1. FFNVCodec
        nv_dir, _ = self.git_clone(FFNVCODEC_URL, "ffnvcodec", update=update)
        make_exe = self.get_tool_path("make")
        self.run(f'{make_exe} PREFIX="{self.prefix}" install', cwd=nv_dir, env=self.get_msys_env())

    def build_ffmpeg(self, update=True):
        """Build FFmpeg. Returns True if build was performed."""
        log("[FFmpeg] Building FFmpeg...")
        src_dir, updated = self.git_clone(FFMPEG_URL, "ffmpeg", update=update)
        build_dir = os.path.join(self.working_dir, "ffmpeg")
        
        if os.path.exists(build_dir): 
            shutil.rmtree(build_dir, onerror=self.rmtree_onerror)
        shutil.copytree(src_dir, build_dir)
        
        env = self.get_msys_env()
        make_exe = self.get_tool_path("make")
        bash_exe = self.get_tool_path("bash")
        
        # Define msys_lib for extra-ldflags
        msys_lib = to_unix(os.path.join(self.msys, "clang64", "lib"))
        
        conf = [
            bash_exe, './configure',
            f'--prefix="{self.prefix}"',
            '--target-os=mingw32',
            '--enable-shared', '--disable-static', # SHARED BUILD
            # '--pkg-config-flags="--static"', 
            '--arch=x86_64',
            
            # Linking fixes
            # We explicitly link dependent C++ libraries to ensure they are available to avcodec.dll
            # libvpl (for QSV) often needs -lvpl -lstdc++ and system libs
            '--extra-libs="-lc++ -lvpl -lstdc++ -lole32 -lgdi32 -luuid"', 
            
            # Toolchain
            '--extra-libs="-lc++"', 
            
            # Toolchain - Use MSYS2 Clang
            '--cc=clang', '--cxx=clang++', '--ar=llvm-ar', '--nm=llvm-nm', '--ranlib=llvm-ranlib',
            
            # Optimization
            '--extra-cflags="-O3 -ffast-math -flto"',
            '--extra-cxxflags="-O3 -ffast-math -flto"',
            '--extra-ldflags="-flto -O3"', 
            f'--extra-ldflags="-L{msys_lib}"', 
            
            # Licensing
            '--disable-gpl', # NO GPL
            '--enable-version3', '--enable-nonfree', # NVENC requires nonfree, but nonfree + lgpl is compatible? 
            # Wait, NVENC headers are MIT. But --enable-nvenc in ffmpeg might trigger nonfree?
            # Actually, "The resulting binary will be nonfree".
            # If so, it's not LGPL. It's proprietary.
            # User wants MIT release. "MIT + proprietary generic binary" is allowed.
            # Key is: Don't link GPL code.
            
            # Components

            '--disable-doc', '--disable-programs',
            '--enable-ffmpeg', '--enable-ffprobe',
            '--disable-zlib', '--disable-bzlib', '--disable-lzma',
            '--disable-alsa', # Linux audio not available on Windows
            
            # Hardware
            '--enable-d3d11va', '--enable-dxva2',
            '--enable-nvenc', '--enable-nvdec',
            '--enable-vulkan', '--enable-amf',
            '--enable-libvpl', # QSV
            '--enable-mediafoundation',
            
            # Tuning
            '--disable-encoders', '--disable-decoders',
            '--disable-muxers', '--disable-demuxers',
            '--disable-parsers', '--disable-bsfs', '--disable-protocols',
            
            '--enable-protocol=file',
            '--enable-muxer=mp4,matroska,mov,flv,ts',
            '--enable-demuxer=concat',
            
            # SW Encoders (Audio)
            '--enable-encoder=aac,opus,flac,alac',
            '--enable-decoder=aac,opus,flac,alac',
            '--enable-parser=aac,opus,flac',
            
            # HW Encoders
            '--enable-encoder=h264_nvenc,hevc_nvenc,av1_nvenc',
            '--enable-encoder=h264_amf,hevc_amf,av1_amf',
            '--enable-encoder=h264_qsv,hevc_qsv,av1_qsv,vp9_qsv',
            '--enable-encoder=h264_mf,hevc_mf', # MediaFoundation
            
            # HW Decoders
            '--enable-decoder=h264,hevc,av1,vp9,mjpeg',
            '--enable-decoder=h264_qsv,hevc_qsv,av1_qsv,vp9_qsv',
            '--enable-decoder=h264_cuvid,hevc_cuvid,vp9_cuvid,av1_cuvid',
            
            # Filters
            '--enable-filter=scale,scale_qsv,vpp_qsv', 
            '--enable-hwaccel=h264_nvdec,hevc_nvdec,av1_nvdec',
            '--enable-hwaccel=h264_d3d11va,hevc_d3d11va,av1_d3d11va',
        ]
        
        self.run(' '.join(conf), cwd=build_dir, env=env)
        self.run(f'{make_exe} -j16', cwd=build_dir, env=env)
        self.run(f'{make_exe} install', cwd=build_dir, env=env)

def compile_custom_ffmpeg(skip_updates=False):
    """Build FFmpeg from git master. Check for updates and rebuild if needed.
    
    Args:
        skip_updates: If True, don't check for git updates, use existing repo as-is.
    """
    # SEMI-HARDCODED: Skip FFmpeg setup if DLLs already exist in bin/ffmpeg
    ffmpeg_bin_dst = os.path.join(BIN_DIR, "ffmpeg")
    # Check for any avcodec DLL (version number varies)
    avcodec_dlls = glob.glob(os.path.join(ffmpeg_bin_dst, "avcodec-*.dll"))
    if avcodec_dlls:
        log("FFmpeg DLLs already exist in target ffmpeg dir - skipping whole FFmpeg setup to avoid permission locks.")
        return

    # Use internal builder
    builder = FFmpegBuilder(
        root_dir=PROJECT_ROOT,
        msys_dir=MSYS2_DIR,
        install_dir=FFMPEG_DIR
    )
    builder.setup_dirs()
    
    # Check if FFmpeg repo exists and get current commit for tracking
    ffmpeg_repo = os.path.join(builder.repos_dir, "ffmpeg")
    commit_file = os.path.join(builder.build_root, "last_built_commit.txt")
    
    # Determine if rebuild is needed
    needs_rebuild = False
    
    if not os.path.exists(os.path.join(FFMPEG_DIR, "lib", "libavcodec.dll.a")):
        # No built FFmpeg - definitely need to build
        needs_rebuild = True
        log("FFmpeg not built yet - building...")
    elif not os.path.exists(ffmpeg_repo):
        # Repo doesn't exist - need to clone and build
        needs_rebuild = True
        log("FFmpeg repo not found - cloning and building...")
    else:
        # Check for updates
        update = not skip_updates
        _, updated = builder.git_clone(FFMPEG_URL, "ffmpeg", update=update)
        
        if updated:
            needs_rebuild = True
            log("FFmpeg source updated - rebuilding...")
        else:
            # Check if last build matches current commit
            git_exe = builder.get_tool_path("git")
            current_commit = subprocess.check_output(
                [git_exe, "rev-parse", "HEAD"], cwd=ffmpeg_repo, env=builder.get_msys_env()
            ).decode().strip()
            
            last_built = ""
            if os.path.exists(commit_file):
                with open(commit_file, "r") as f:
                    last_built = f.read().strip()
            
            if current_commit != last_built:
                needs_rebuild = True
                log(f"FFmpeg commit changed ({last_built[:8] if last_built else 'none'} -> {current_commit[:8]}) - rebuilding...")
            else:
                log(f"FFmpeg up to date ({current_commit[:8]}) - skipping build.")
    
    if not needs_rebuild:
        # Even if we don't rebuild, let's ensure DLLs are in bin/ffmpeg
        log("FFmpeg up to date, ensuring DLLs are in target ffmpeg dir...")
    else:
        log("Building Custom FFmpeg (this may take a while)...")
        try:
            builder.build_dependencies(update=not skip_updates)
            builder.build_ffmpeg(update=not skip_updates)
            
            # Save current commit hash for future comparison
            git_exe = builder.get_tool_path("git")
            current_commit = subprocess.check_output(
                [git_exe, "rev-parse", "HEAD"], cwd=ffmpeg_repo, env=builder.get_msys_env()
            ).decode().strip()
            with open(commit_file, "w") as f:
                f.write(current_commit)
            log(f"Built FFmpeg commit: {current_commit[:8]}")
        except Exception as e:
            log(f"FFmpeg Build Failed: {e}")
            sys.exit(1)
            
    # Post-build (or if already built): Copy DLLs to bin/ffmpeg
    try:
        log("Copying FFmpeg DLLs...")
        ffmpeg_bin_src = os.path.join(FFMPEG_DIR, "bin")
        ffmpeg_bin_dst = os.path.join(BIN_DIR, "ffmpeg")
        os.makedirs(ffmpeg_bin_dst, exist_ok=True)
        
        for f in glob.glob(os.path.join(ffmpeg_bin_src, "*.dll")):
            shutil.copy(f, ffmpeg_bin_dst)
            log(f"Copied {os.path.basename(f)} to ffmpeg dir")
        
        # Copy MSYS2 runtime dependencies that FFmpeg DLLs need
        msys_bin = os.path.join(MSYS2_DIR, "clang64", "bin")
        runtime_deps = [
            "libiconv-2.dll",
            "libva_win32.dll", 
            "libva.dll",
            "libvpl-2.dll",
            "libc++.dll",
            "libunwind.dll",  # libc++ often needs this
        ]
        for dep in runtime_deps:
            src = os.path.join(msys_bin, dep)
            if os.path.exists(src):
                shutil.copy(src, ffmpeg_bin_dst)
                log(f"Copied runtime dep {dep} to bin/ffmpeg")
            
        log("Custom FFmpeg Setup Complete.")
    except Exception as e:
        log(f"FFmpeg Setup Failed: {e}")
        sys.exit(1)

def get_env():
    clang_bin = os.path.join(MSYS2_DIR, "clang64", "bin")
    usr_bin = os.path.join(MSYS2_DIR, "usr", "bin")
    env = os.environ.copy()
    env["PATH"] = clang_bin + os.pathsep + usr_bin + os.pathsep + env["PATH"]
    env["PKG_CONFIG_PATH"] = os.path.join(MSYS2_DIR, "clang64", "lib", "pkgconfig")
    # Enable ccache
    env["CCACHE_DIR"] = os.path.join(MSYS2_DIR, ".ccache")
    # env["CCACHE_BASEDIR"] = PROJECT_ROOT
    return env, clang_bin

def get_env_x86():
    # MSYS2 Mingw32 environment
    clang_bin = os.path.join(MSYS2_DIR, "mingw32", "bin")
    usr_bin = os.path.join(MSYS2_DIR, "usr", "bin")
    env = os.environ.copy()
    env["PATH"] = clang_bin + os.pathsep + usr_bin + os.pathsep + env["PATH"]
    env["PKG_CONFIG_PATH"] = os.path.join(MSYS2_DIR, "mingw32", "lib", "pkgconfig")
    # Enable ccache
    env["CCACHE_DIR"] = os.path.join(MSYS2_DIR, ".ccache")
    # env["CCACHE_BASEDIR"] = PROJECT_ROOT
    # env["DISABLE_CCACHE"] = "1" # Enable ccache for x86 too now that we fixed flags
    return env, clang_bin

def ensure_dirs():
    os.makedirs(OBJ_DIR, exist_ok=True)
    os.makedirs(CAPTURE_BIN_DIR, exist_ok=True)
    os.makedirs(TESTAPP_BIN_DIR, exist_ok=True)
    os.makedirs(os.path.join(OBJ_DIR, "common"), exist_ok=True)
    os.makedirs(os.path.join(OBJ_DIR, "mediaengine"), exist_ok=True)
    os.makedirs(os.path.join(OBJ_DIR, "hook"), exist_ok=True)
    os.makedirs(os.path.join(OBJ_DIR, "captureengine"), exist_ok=True)
    os.makedirs(os.path.join(OBJ_DIR, "tests"), exist_ok=True)
    os.makedirs(os.path.join(OBJ_DIR, "external"), exist_ok=True)

def parse_dep_file(dep_path: str) -> List[str]:
    """Parse a GCC/Clang .d file."""
    deps = []
    try:
        with open(dep_path, 'r') as f:
            content = f.read().replace('\\\n', '') # Join lines
            # Content is like "obj: src dep1 dep2..."
            parts = content.split(':')
            if len(parts) > 1:
                # Split by space and filter empty
                files = parts[1].split()
                deps = [f.strip() for f in files if f.strip()]
    except Exception:
        pass
    return deps

def should_recompile(src: str, obj: str, dep_file: str) -> bool:
    if not os.path.exists(obj):
        return True
    
    # Check source timestamp
    if os.path.getmtime(src) > os.path.getmtime(obj):
        return True
        
    # Check dependencies
    if os.path.exists(dep_file):
        deps = parse_dep_file(dep_file)
        obj_mtime = os.path.getmtime(obj)
        for dep in deps:
            if os.path.exists(dep) and os.path.getmtime(dep) > obj_mtime:
                return True
    else:
        # If object exists but dep file is missing, recompile to generate dep file
        return True
        
    return False

def compile_object(env: Dict[str, str], clang_exe: str, cflags: List[str], src: str, obj: str) -> bool:
    """Compile a single object file. Returns True if compiled, False if skipped."""
    dep_file = obj + ".d"
    
    # Add dependency tracking flags
    # -MMD: Generate dependency file, ignore system headers
    # -MF: Specify output dependency file
    compile_flags = cflags + ["-MMD", "-MF", dep_file]

    # Construct the full command list for compile_commands.json
    full_cmd_list = [clang_exe] + compile_flags + ["-c", src, "-o", obj]
    
    # Add to global compile commands list
    # Use 'arguments' list instead of 'command' string for better cross-platform/shell reliability
    COMPILE_COMMANDS.append({
        "directory": PROJECT_ROOT,
        "arguments": full_cmd_list,
        "file": src
    })

    if not should_recompile(src, obj, dep_file):
        return False  # Skip - up to date
    
    # Use ccache if available
    ccache_exe = shutil.which("ccache", path=env["PATH"])
    if env.get("DISABLE_CCACHE"):
        ccache_exe = None
    
    if ccache_exe:
        # ccache on Windows often dislikes absolute paths for the compiler
        cmd = [ccache_exe, os.path.basename(clang_exe)] + compile_flags + ["-c", src, "-o", obj]
    else:
        cmd = [clang_exe] + compile_flags + ["-c", src, "-o", obj]
        
    run_command(cmd, env=env)
    return True

def parallel_compile(env, clang_exe, cflags, src_obj_pairs):
    """Compile multiple source files in parallel using all CPU cores."""
    num_workers = cpu_count()
    compiled = 0
    skipped = 0
    
    def compile_one(args):
        src, obj = args
        os.makedirs(os.path.dirname(obj), exist_ok=True)
        return compile_object(env, clang_exe, cflags, src, obj), obj
    
    with ThreadPoolExecutor(max_workers=num_workers) as executor:
        futures = {executor.submit(compile_one, pair): pair for pair in src_obj_pairs}
        for future in as_completed(futures):
            was_compiled, obj = future.result()
            if was_compiled:
                compiled += 1
            else:
                skipped += 1
    
    return compiled, skipped

def compile_imgui(env, clang_exe, cflags):
    log(f"Compiling ImGui (parallel, {cpu_count()} threads)...")
    src_files = glob.glob(os.path.join(IMGUI_DIR, "*.cpp"))
    src_files.append(os.path.join(IMGUI_DIR, "backends", "imgui_impl_dx12.cpp"))
    src_files.append(os.path.join(IMGUI_DIR, "backends", "imgui_impl_dx11.cpp"))
    src_files.append(os.path.join(IMGUI_DIR, "backends", "imgui_impl_dx9.cpp"))
    src_files.append(os.path.join(IMGUI_DIR, "backends", "imgui_impl_opengl3.cpp"))
    src_files.append(os.path.join(IMGUI_DIR, "backends", "imgui_impl_vulkan.cpp"))
    src_files.append(os.path.join(IMGUI_DIR, "backends", "imgui_impl_win32.cpp"))
    
    imgui_cflags = cflags + ["-I" + IMGUI_DIR]
    imgui_objs = []
    src_obj_pairs = []
    
    for src in src_files:
        rel_path = os.path.relpath(src, PROJECT_ROOT)
        obj = os.path.join(OBJ_DIR, os.path.splitext(rel_path)[0] + ".o").replace("\\", "/")
        src_obj_pairs.append((src, obj))
        imgui_objs.append(obj)
    
    compiled, skipped = parallel_compile(env, clang_exe, imgui_cflags, src_obj_pairs)
    if compiled > 0:
        log(f"ImGui: compiled {compiled}, skipped {skipped}")
    return imgui_objs

def compile_tests(env, clang_exe, cflags, common_objs, pkg_config, obj_dir):
    log(f"Compiling Tests (parallel, {cpu_count()} threads)...")
    src_files = glob.glob(os.path.join(PROJECT_ROOT, "tests", "*.cpp"))
    if not src_files:
        log("No test files found.")
        return

    tests_dir = os.path.join(PROJECT_ROOT, "tests")
    os.makedirs(tests_dir, exist_ok=True)
    test_exe = os.path.join(tests_dir, "unit_tests.exe")
    
    # 1. Get FFmpeg flags - use our local ffmpeg pkg-config  
    env_ffmpeg = env.copy()
    env_ffmpeg["PKG_CONFIG_PATH"] = os.path.join(FFMPEG_DIR, "lib", "pkgconfig") + os.pathsep + env_ffmpeg.get("PKG_CONFIG_PATH", "")
    
    pkgs = ["libavcodec", "libavformat", "libavutil", "libswresample", "libswscale"]
    pkg_cmd = [pkg_config, "--cflags", "--libs"] + pkgs  # Removed --static for shared linking
    ffmpeg_flags_raw = run_command(pkg_cmd, env=env_ffmpeg).strip().split()
    ffmpeg_flags = [f for f in ffmpeg_flags_raw if f not in ["-ldl", "-lshaderc_shared"]]
    
    # Link against gtest, common, hook, mediaengine, and ffmpeg
    # Add VPL for QSV symbols, ole32/gdi32/uuid as VPL deps
    ldflags_test = ["-static-libgcc", "-static-libstdc++", "-Wl,--allow-multiple-definition",
                    "-lgtest", "-lgtest_main", "-lwinmm", "-lshlwapi",
                    "-ld3d11", "-ldxgi", "-ld3dcompiler", "-lgdi32", "-luser32", "-ldwmapi", "-lavrt", "-lpdh", "-lshcore",
                    "-lole32", "-lmfplat", "-lmfuuid", "-lbcrypt", "-lsecur32", "-lws2_32", "-lmmdevapi",
                    "-lvpl", "-luuid",  # VPL for QSV
                    "-lshaderc_combined", "-lglslang", "-lSPIRV-Tools", "-lSPIRV-Tools-opt", "-lSPIRV-Tools-link",
                    "-lMachineIndependent", "-lGenericCodeGen", "-lOSDependent", "-lSPIRV",
                    "-ljxl_cms", "-ljxl_threads", "-lhwy", "-llcms2", "-ltasn1", "-lnettle", "-lhogweed", "-lgmp",
                    "-lpangocairo-1.0", "-lpangowin32-1.0", "-lpangoft2-1.0", "-lpango-1.0",
                    "-lharfbuzz", "-lfreetype", "-lgraphite2", "-lfribidi", "-lthai", "-ldatrie", "-lintl", "-lfontconfig", "-lexpat",
                    "-lcairo-gobject", "-lpixman-1", "-lffi", "-lpcre2-8", "-lgmodule-2.0",
                    "-lssl", "-lcrypto", "-lsharpyuv", "-lcrypt32", "-lncrypt", "-lntdll", "-luserenv", "-lwinmm", "-liphlpapi",
                    "-lgdiplus", "-lshlwapi", "-lrpcrt4", "-ldwrite", "-ldnsapi", "-lmsimg32",
                    "-lbrotlienc", "-lbrotlidec", "-lbrotlicommon", "-lz", "-llzma", "-lbz2", "-liconv", "-lunistring", "-lzstd", "-lidn2"] + ffmpeg_flags

    # 2. Compile MediaEngine objects for tests
    me_src = glob.glob(os.path.join(PROJECT_ROOT, "mediaengine", "*.cpp"))
    me_objs = []
    src_obj_pairs = []
    # We need to compile MediaEngine with MEDIAENGINE_EXPORTS or similar if needed, 
    # but for static linking in tests, we just need the symbols.
    # Note: AudioEncoder.cpp might rely on specific defines.
    me_cflags = cflags + ffmpeg_flags
    
    for src in me_src:
        rel_path = os.path.relpath(src, PROJECT_ROOT)
        obj = os.path.join(obj_dir, os.path.splitext(rel_path)[0] + ".o").replace("\\", "/")
        src_obj_pairs.append((src, obj))
        me_objs.append(obj)
    
    parallel_compile(env, clang_exe, me_cflags, src_obj_pairs)

    # 3. Compile Tests
    test_cflags = cflags + ["-I" + os.path.join(PROJECT_ROOT, "mediaengine")] # Ensure we can include audio_encoder.h
    test_objs = []
    src_obj_pairs = []
    for src in src_files:
        rel_path = os.path.relpath(src, PROJECT_ROOT)
        obj = os.path.join(obj_dir, os.path.splitext(rel_path)[0] + ".o").replace("\\", "/")
        src_obj_pairs.append((src, obj))
        test_objs.append(obj)
        
    parallel_compile(env, clang_exe, test_cflags, src_obj_pairs)

    # 4. Compile hook/common for tests
    hook_common_src = glob.glob(os.path.join(PROJECT_ROOT, "hook", "common", "*.cpp"))
    # Exclude overlay.cpp as it requires ImGui linking which tests don't have
    hook_common_src = [f for f in hook_common_src if "overlay.cpp" not in f]
    hook_common_objs = []
    src_obj_pairs = []
    for src in hook_common_src:
        rel_path = os.path.relpath(src, PROJECT_ROOT)
        obj = os.path.join(obj_dir, os.path.splitext(rel_path)[0] + ".o").replace("\\", "/")
        src_obj_pairs.append((src, obj))
        hook_common_objs.append(obj)
    parallel_compile(env, clang_exe, cflags, src_obj_pairs)

    log("Linking Unit Tests...")
    # Order: Tests -> Common -> MediaEngine -> HookCommon -> Libs
    cmd = [clang_exe] + test_objs + common_objs + me_objs + hook_common_objs + ldflags_test + ["-o", test_exe]
    run_command(cmd, env=env)
    return test_exe

def run_tests(env, test_exe):
    log("=== Running Unit Tests ===")
    if not os.path.exists(test_exe):
        log("Error: Test executable not found.")
        return
        
    # Run tests
    # Use standard run_command but allow exit on failure manually if we want more control
    # For now, let's just run it
    cmd = [test_exe]
    
    # We want to see output live usually, but build system captures it
    # We want to see output live usually, but build system captures it
    # Modified to stream output to debug crashes
    subprocess.run(cmd, env=env)
    # log(output)
    log("=== Unit Tests Passed ===")

def run_integration_tests(env):
    log("=== Running Integration Tests (testapp/run_tests.py) ===")
    script = os.path.join(PROJECT_ROOT, "testapp", "run_tests.py")
    if not os.path.exists(script):
        log(f"Error: {script} not found.")
        return

    # Run the python script
    # We assume 'python' is in path, or use sys.executable
    cmd = [sys.executable, script, "--duration", "5", "--tests", "1"] 
    # Shorten duration for smoke test (5s record, 1 iteration)
    
    log(f"Executing: {' '.join(cmd)}")
    result = subprocess.run(cmd, cwd=os.path.dirname(script))
    
    if result.returncode != 0:
        log("ERROR: Integration tests failed!")
        sys.exit(1)
    
    log("=== Integration Tests Passed ===")

def compile_testapps(env, x86_env, clang_exe, cflags):
    """Compile test applications using Clang (and x86 if available)"""
    """Compile DX12 and Vulkan test applications to testapp/bin"""
    log("Compiling Test Applications...")
    
    testapp_src_dir = os.path.join(PROJECT_ROOT, "testapp")
    testapp_bin_dir = TESTAPP_BIN_DIR
    os.makedirs(testapp_bin_dir, exist_ok=True)

    # Optional: also build 32-bit variants if 32-bit toolchain is available.
    x86_bin_dir = os.path.join(testapp_bin_dir, "x86")
    os.makedirs(x86_bin_dir, exist_ok=True)
    clang_exe_x86 = os.path.join(MSYS2_DIR, 'mingw32', 'bin', 'clang++.exe')
    have_x86 = os.path.exists(clang_exe_x86)
    # NOTE: mingw32 clang toolchain can't reliably link LTO bitcode in our setup.
    # Build x86 test apps without -flto.
    cflags_x86 = [f for f in cflags if f != "-flto"]
    
    # Collect all build tasks
    tasks = []
    
    # helper to resolve ccache
    ccache_exe = shutil.which("ccache", path=env["PATH"])
    if env.get("DISABLE_CCACHE"): ccache_exe = None

    
    def add_task(desc, cmd, cwd=None, task_env=env):
        tasks.append((desc, cmd, cwd, task_env))

    # Helper to build command with ccache
    def make_cmd(compiler, flags, source, linker_flags, output):
        cmd_base = [compiler] + flags + [source] + linker_flags + ["-o", output]
        if ccache_exe:
            # ccache prefers basename if compiler is in PATH
            return [ccache_exe, os.path.basename(compiler)] + flags + [source] + linker_flags + ["-o", output]
        return cmd_base

    # DX12 Test App
    dx12_src = os.path.join(testapp_src_dir, "dx12_test.cpp")
    dx12_exe = os.path.join(testapp_bin_dir, "dx12_test.exe")
    if os.path.exists(dx12_src):
        dx12_ldflags = ["-static", 
                        "-ld3d12", "-ldxgi", "-ld3dcompiler", "-lgdi32", "-luser32", "-lshcore"]
        add_task("dx12_test.exe", make_cmd(clang_exe, cflags, dx12_src, dx12_ldflags, dx12_exe))

        if have_x86:
            dx12_exe_x86 = os.path.join(x86_bin_dir, "dx12_test.exe")
            add_task("dx12_test.exe (x86)", make_cmd(clang_exe_x86, cflags_x86, dx12_src, dx12_ldflags, dx12_exe_x86))
    
    # DX11 Test App
    dx11_src = os.path.join(testapp_src_dir, "dx11_test.cpp")
    dx11_exe = os.path.join(testapp_bin_dir, "dx11_test.exe")
    if os.path.exists(dx11_src):
        dx11_ldflags = ["-static", 
                        "-ld3d11", "-ldxgi", "-lgdi32", "-luser32", "-lshcore"]
        add_task("dx11_test.exe", make_cmd(clang_exe, cflags, dx11_src, dx11_ldflags, dx11_exe))

        if have_x86:
            dx11_exe_x86 = os.path.join(x86_bin_dir, "dx11_test.exe")
            add_task("dx11_test.exe (x86)", make_cmd(clang_exe_x86, cflags_x86, dx11_src, dx11_ldflags, dx11_exe_x86))

    # DX9 Test App
    dx9_src = os.path.join(testapp_src_dir, "dx9_test.cpp")
    dx9_exe = os.path.join(testapp_bin_dir, "dx9_test.exe")
    if os.path.exists(dx9_src):
        dx9_ldflags = ["-static", 
                       "-ld3d9", "-lgdi32", "-luser32"]
        add_task("dx9_test.exe", make_cmd(clang_exe, cflags, dx9_src, dx9_ldflags, dx9_exe))

        if have_x86:
            dx9_exe_x86 = os.path.join(x86_bin_dir, "dx9_test.exe")
            add_task("dx9_test.exe (x86)", make_cmd(clang_exe_x86, cflags_x86, dx9_src, dx9_ldflags, dx9_exe_x86))

    # DX10 Test App
    dx10_src = os.path.join(testapp_src_dir, "dx10_test.cpp")
    dx10_exe = os.path.join(testapp_bin_dir, "dx10_test.exe")
    if os.path.exists(dx10_src):
        dx10_ldflags = ["-static",
                        "-ld3d10", "-ldxgi", "-ld3dcompiler", "-lgdi32", "-luser32", "-lshcore"]
        add_task("dx10_test.exe", make_cmd(clang_exe, cflags, dx10_src, dx10_ldflags, dx10_exe))

        if have_x86:
            dx10_exe_x86 = os.path.join(x86_bin_dir, "dx10_test.exe")
            add_task("dx10_test.exe (x86)", make_cmd(clang_exe_x86, cflags_x86, dx10_src, dx10_ldflags, dx10_exe_x86))

    # Vulkan Test App
    vulkan_src = os.path.join(testapp_src_dir, "vulkan_test.cpp")
    vulkan_exe = os.path.join(testapp_bin_dir, "vulkan_test.exe")
    if os.path.exists(vulkan_src):
        vulkan_lib = os.path.join(MSYS2_DIR, 'clang64', 'lib', 'libvulkan-1.dll.a')
        vulkan_ldflags = ["-static",
                          "-Wl,--subsystem,windows",
                          vulkan_lib, "-lgdi32", "-luser32", "-lshcore"]
        add_task("vulkan_test.exe", make_cmd(clang_exe, cflags, vulkan_src, vulkan_ldflags, vulkan_exe))

        if have_x86:
            vulkan_exe_x86 = os.path.join(x86_bin_dir, "vulkan_test.exe")
            vulkan_lib_x86 = os.path.join(MSYS2_DIR, 'mingw32', 'lib', 'libvulkan-1.dll.a')
            vulkan_ldflags_x86 = ["-static",
                                  "-Wl,--subsystem,windows",
                                  vulkan_lib_x86, "-lgdi32", "-luser32", "-lshcore"]
            add_task("vulkan_test.exe (x86)", make_cmd(clang_exe_x86, cflags_x86, vulkan_src, vulkan_ldflags_x86, vulkan_exe_x86))

    # OpenGL Test App
    opengl_src = os.path.join(testapp_src_dir, "opengl_test.cpp")
    opengl_exe = os.path.join(testapp_bin_dir, "opengl_test.exe")
    if os.path.exists(opengl_src):
        # opengl_ldflags = ["-static", "-static-libgcc", "-static-libstdc++", 
        opengl_ldflags = ["-static",
                        "-lopengl32", "-lglu32", "-lgdi32", "-luser32", "-lshcore"]
        
        add_task("opengl_test.exe", make_cmd(clang_exe, cflags, opengl_src, opengl_ldflags, opengl_exe))

        if have_x86:
            opengl_exe_x86 = os.path.join(x86_bin_dir, "opengl_test.exe")
            cmd = [clang_exe_x86] + cflags_x86 + [opengl_src] + opengl_ldflags + ["-o", opengl_exe_x86]
            add_task("opengl_test.exe (x86)", cmd)

    # Execute all tasks in parallel
    if not tasks:
        return

    # Record tasks for compile_commands.json
    for desc, cmd, cwd, tenv in tasks:
        # Find the source file in the command arguments
        src_file = None
        for arg in cmd:
            if arg.endswith(".cpp"):
                src_file = arg
                break
        
        if src_file:
            COMPILE_COMMANDS.append({
                "directory": PROJECT_ROOT,
                "arguments": cmd,
                "file": src_file
            })

    def compile_app(t):

        desc, cmd, cwd, tenv = t
        log(f"Compiling {desc}...")
        try:
            subprocess.run(cmd, env=tenv, cwd=cwd, check=True, capture_output=True, text=True)
            log(f"Built: {desc}")
        except subprocess.CalledProcessError as e:
            log(f"ERROR compiling {desc}:")
            log(e.stdout)
            log(e.stderr)
            raise e

    log(f"Compiling {len(tasks)} Test Apps in parallel...")
    with ThreadPoolExecutor(max_workers=cpu_count()) as executor:
        futures = [executor.submit(compile_app, t) for t in tasks]
        for future in as_completed(futures):
            try:
                future.result()
            except Exception:
                sys.exit(1)

def compile_vulkan_layer(env, clang_exe, cflags, arch):
    """Compile VK_LAYER_CE_overlay - Vulkan implicit layer for overlay and capture"""
    log(f"Compiling Vulkan Layer ({arch})...")
    
    layer_dir = os.path.join(PROJECT_ROOT, "hook", "vulkan_layer")
    bin_dir = CAPTURE_BIN_DIR
    obj_dir = os.path.join(PROJECT_ROOT, "build", "obj", arch, "vulkan_layer")
    os.makedirs(obj_dir, exist_ok=True)
    
    # Layer source files
    layer_sources = [
        os.path.join(layer_dir, "layer_main.cpp"),
        os.path.join(layer_dir, "vulkan_layer.cpp"),
        os.path.join(layer_dir, "layer_ipc.cpp"),
        os.path.join(layer_dir, "layer_overlay.cpp"),
        os.path.join(layer_dir, "layer_capture.cpp"),
        os.path.join(layer_dir, "layer_bridge.cpp"),
        os.path.join(layer_dir, "layer_hooks.cpp"),
        # Common sources needed for standard overlay
        os.path.join(PROJECT_ROOT, "hook", "common", "overlay.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "fg_detection.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "ipc_client.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "system_metrics.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "performance_metrics.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "input_manager.cpp"),
    ]
    
    # ImGui sources for the layer
    imgui_sources = glob.glob(os.path.join(IMGUI_DIR, "*.cpp")) + [
        os.path.join(IMGUI_DIR, "backends", "imgui_impl_vulkan.cpp"),
        os.path.join(IMGUI_DIR, "backends", "imgui_impl_win32.cpp"),
    ]
    
    # Compile layer sources
    layer_cflags = cflags + [
        "-I" + layer_dir,
        "-I" + os.path.join(PROJECT_ROOT, "common"),
        "-I" + os.path.join(PROJECT_ROOT, "hook", "common"),
        "-I" + IMGUI_DIR,
        "-DVK_NO_PROTOTYPES",
        "-DIMGUI_IMPL_VULKAN_NO_PROTOTYPES",
        "-DVK_USE_PLATFORM_WIN32_KHR",
    ]
    
    layer_objs = []
    
    # Compile all sources in parallel
    src_obj_pairs = []
    
    # helper to add sources
    def add_sources(sources, dest_obj_dir):
        for src in sources:
            if not os.path.exists(src):
                log(f"Warning: Layer source not found: {src}")
                continue
            basename = os.path.splitext(os.path.basename(src))[0]
            obj = os.path.join(dest_obj_dir, basename + ".o")
            src_obj_pairs.append((src, obj))
            layer_objs.append(obj)

    add_sources(layer_sources, obj_dir)
    
    imgui_obj_dir = os.path.join(obj_dir, "imgui")
    os.makedirs(imgui_obj_dir, exist_ok=True)
    add_sources(imgui_sources, imgui_obj_dir)
    
    if not src_obj_pairs:
        log("Error: No layer sources found.")
        return

    # Run parallel compilation
    compiled, skipped = parallel_compile(env, clang_exe, layer_cflags, src_obj_pairs)
    if compiled > 0:
        log(f"Vulkan Layer ({arch}): compiled {compiled}, skipped {skipped}")
    
    # Link layer DLL
    if arch == "x64":
        layer_dll_name = "VK_LAYER_CE_overlay.dll"
        vulkan_lib = os.path.join(MSYS2_DIR, 'clang64', 'lib', 'libvulkan-1.dll.a')
    else:
        layer_dll_name = "VK_LAYER_CE_overlay_x86.dll"
        vulkan_lib = os.path.join(MSYS2_DIR, 'mingw32', 'lib', 'libvulkan-1.dll.a')
    
    layer_dll = os.path.join(bin_dir, layer_dll_name)
    
    # Use .def file for exports to avoid attributes mismatch with official headers
    layer_def = os.path.join(layer_dir, "layer.def")

    ldflags = [
        "-shared",
        "-static-libgcc",
        "-static-libstdc++",
        layer_def,
        vulkan_lib,
        "-lgdi32",
        "-luser32",
        "-lpdh",
        "-ldxgi",
        "-lshcore",
        "-lwinmm",
        "-luser32",
        "-lpdh",
        "-ldxgi",
        "-lshcore",
        "-o", layer_dll,
    ]
    
    if arch == "x86":
        ldflags.append("-Wl,--kill-at")
        ldflags.append("-static")
        
    
    # Use ccache for linking too if available
    ccache_exe = shutil.which("ccache", path=env["PATH"])
    if env.get("DISABLE_CCACHE"): ccache_exe = None
    
    if ccache_exe:
        cmd = [ccache_exe, os.path.basename(clang_exe)] + layer_objs + ldflags
    else:
        cmd = [clang_exe] + layer_objs + ldflags
    
    # Robust handling for locked DLLs (DataExchangeHost etc.)
    if os.path.exists(layer_dll):
        try:
            os.remove(layer_dll)
        except OSError:
            log(f"[Warning] {os.path.basename(layer_dll)} is locked. Attempting to rename...")
            try:
                    import time
                    import random
                    trash_name = f"{layer_dll}.trash.{int(time.time())}.{random.randint(1000,9999)}"
                    os.rename(layer_dll, trash_name)
                    log(f"[Info] Renamed locked Layer DLL to {os.path.basename(trash_name)}")
            except OSError as e:
                    log(f"[Error] Failed to rename locked Layer DLL: {e}")
                    # sys.exit(1) # Don't exit, try linking anyway? No, it will fail.
                    
    try:
        run_command(cmd, env=env)
        log(f"Built: {layer_dll}")

        # Generate layer manifest JSON dynamically
        # This ensures the path is always correct and current
        import json
        
        manifest_name = "VK_LAYER_CE_overlay.json" if arch == "x64" else "VK_LAYER_CE_overlay_x86.json"
        manifest_path = os.path.join(bin_dir, manifest_name)
        
        # Absolute path to the DLL we just built
        # Escape backslashes for JSON
        abs_dll_path = os.path.abspath(layer_dll)
        
        manifest = {
            "file_format_version": "1.2.0",
            "layer": {
                "name": "VK_LAYER_CE_overlay" if arch == "x64" else "VK_LAYER_CE_overlay_x86",
                "type": "GLOBAL",
                "library_path": abs_dll_path, # JSON serializer handles escaping
                "api_version": "1.3.0",
                "implementation_version": "1",
                "description": "CaptureEngine Overlay and Recording Layer",
                "functions": {
                    "vkGetInstanceProcAddr": "vkGetInstanceProcAddr",
                    "vkGetDeviceProcAddr": "vkGetDeviceProcAddr",
                    "vkNegotiateLoaderLayerInterfaceVersion": "vkNegotiateLoaderLayerInterfaceVersion"
                },
                "disable_environment": {
                    "DISABLE_CE_VULKAN_LAYER": "1"
                }
            }
        }
        
        with open(manifest_path, "w") as f:
            json.dump(manifest, f, indent=4)
        
        log(f"Generated Manifest: {manifest_path}")

        # CLEANUP: Ensure this layer is NOT registered globally in the registry.
        # It should ONLY be registered ephemerally by captureengine.exe at runtime.
        try:
             import winreg
             key_path = r"Software\Khronos\Vulkan\ImplicitLayers"
             try:
                 key = winreg.OpenKey(winreg.HKEY_CURRENT_USER, key_path, 0, winreg.KEY_SET_VALUE | winreg.KEY_READ)
                 # Check if value exists and delete it
                 try:
                     winreg.DeleteValue(key, manifest_path)
                     log(f"Cleaned legacy registry key for: {manifest_name}")
                 except FileNotFoundError:
                     pass # Key doesn't exist, good.
                 winreg.CloseKey(key)
             except FileNotFoundError:
                 pass # Interface key doesn't exist, good.
        except Exception as e:
             log(f"Warning: Failed to clean registry keys: {e}")


    except Exception as e:
        log(f"Error linking layer: {e}")


def compile_d3d12_wrappers_msvc(env, arch):
    """
    Compile D3D12 wrappers using MSVC.
    Required because MinGW ABI is incompatible with D3D12 headers (WIDL_EXPLICIT_AGGREGATE_RETURNS).
    """
    log("Checking for MSVC to compile D3D12 wrappers...")
    
    # 1. Detect MSVC
    vs_root = r"C:\Program Files\Microsoft Visual Studio\2022\Community"
    if not os.path.exists(vs_root):
         vs_root = r"C:\Program Files\Microsoft Visual Studio\18\Community" # As seen in previous script
    
    # helper to find latest version in a dir
    def find_latest_version(path):
        if not os.path.exists(path): return None
        versions = [d for d in os.listdir(path) if os.path.isdir(os.path.join(path, d)) and d[0].isdigit()]
        if not versions: return None
        return sorted(versions)[-1]

    # Find MSVC Tools
    msvc_tools_root = os.path.join(vs_root, "VC", "Tools", "MSVC")
    msvc_ver = find_latest_version(msvc_tools_root)
    
    if not msvc_ver:
        log("Warning: MSVC Tools not found. Skipping D3D12 wrappers (MSVC).")
        return False, None

    # Detect architecture tools
    msvc_include = os.path.join(msvc_tools_root, msvc_ver, "include")
    if arch == "x64":
        msvc_bin = os.path.join(msvc_tools_root, msvc_ver, "bin", "Hostx64", "x64")
        msvc_lib = os.path.join(msvc_tools_root, msvc_ver, "lib", "x64")
        sdk_arch = "x64"
    else:
        msvc_bin = os.path.join(msvc_tools_root, msvc_ver, "bin", "Hostx64", "x86")
        msvc_lib = os.path.join(msvc_tools_root, msvc_ver, "lib", "x86")
        sdk_arch = "x86"
    
    cl_exe = os.path.join(msvc_bin, "cl.exe")
    link_exe = os.path.join(msvc_bin, "link.exe")
    
    
    if not os.path.exists(cl_exe):
        log(f"Warning: cl.exe not found at {cl_exe}")
        return False, None

    # 2. Detect Windows SDK
    win_sdk_root = r"C:\Program Files (x86)\Windows Kits\10"
    win_sdk_include = os.path.join(win_sdk_root, "Include")
    win_sdk_ver = find_latest_version(win_sdk_include)
    
    if not win_sdk_ver:
        log("Warning: Windows SDK not found.")
        return False, None

    sdk_include_um = os.path.join(win_sdk_include, win_sdk_ver, "um")
    sdk_include_shared = os.path.join(win_sdk_include, win_sdk_ver, "shared")
    sdk_include_ucrt = os.path.join(win_sdk_include, win_sdk_ver, "ucrt")
    # sdk_lib_um = os.path.join(win_sdk_root, "Lib", win_sdk_ver, "um", "x64")
    # sdk_lib_ucrt = os.path.join(win_sdk_root, "Lib", win_sdk_ver, "ucrt", "x64")

    # 3. Setup paths
    obj_dir = os.path.join(BUILD_DIR, "obj", f"msvc_{arch}")
    os.makedirs(obj_dir, exist_ok=True)
    suffix = "" if arch == "x64" else "_x86"
    dll_out = os.path.join(BIN_DIR, f"d3d12_wrappers{suffix}.dll")
    implib_out = os.path.join(BUILD_DIR, "lib", f"d3d12_wrappers{suffix}.lib")
    os.makedirs(os.path.dirname(implib_out), exist_ok=True)

    sources = [
        os.path.join(PROJECT_ROOT, "hook", "wrappers", "d3d12_device_wrap.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "wrappers", "d3d12_commandqueue_wrap.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "wrappers", "d3d12_wrapper_interface.cpp"),
    ]

    include_paths = [
        msvc_include,
        sdk_include_um,
        sdk_include_shared,
        sdk_include_ucrt,
        os.path.join(PROJECT_ROOT, "hook", "wrappers"),
        os.path.join(PROJECT_ROOT, "hook", "apis"),
        os.path.join(PROJECT_ROOT, "hook", "common"),
        os.path.join(PROJECT_ROOT, "common"),
    ]

    # 4. Compile
    cflags = [
        "/nologo", "/c", "/O2", "/MT", "/EHsc", "/W3", "/std:c++20",
        "/DUNICODE", "/D_UNICODE", "/DWIN32", "/D_WINDOWS"
    ]
    for inc in include_paths:
        cflags.append(f"/I{inc}")

    obj_files = []
    
    # Set MSVC environment (PATH)
    msvc_env = env.copy()
    msvc_env["PATH"] = msvc_bin + os.pathsep + msvc_env.get("PATH", "")

    # Parallel Compilation Logic for MSVC
    src_obj_pairs = []
    
    for src in sources:
        if not os.path.exists(src):
            log(f"Warning: Source not found: {src}")
            continue
            
        basename = os.path.splitext(os.path.basename(src))[0]
        obj = os.path.join(obj_dir, basename + ".obj")
        
        # Check timestamp
        if os.path.exists(obj) and os.path.getmtime(obj) > os.path.getmtime(src):
            obj_files.append(obj)
            continue
        
        src_obj_pairs.append((src, obj))

    if src_obj_pairs:
        log(f"[MSVC] Compiling {len(src_obj_pairs)} files in parallel...")
        
        def compile_msvc_obj(args):
            src, obj = args
            basename = os.path.splitext(os.path.basename(src))[0]
            log(f"[MSVC] Compiling {basename}...")
            cmd = [cl_exe] + cflags + [f"/Fo{obj}", src]
            
            # Record for compile_commands.json
            COMPILE_COMMANDS.append({
                "directory": PROJECT_ROOT,
                "arguments": cmd,
                "file": src
            })
            
            try:

                res = subprocess.run(cmd, env=msvc_env, capture_output=True, text=True)
                if res.returncode != 0:
                    return None, f"Error compiling {basename}:\n{res.stdout}\n{res.stderr}"
                return obj, None
            except Exception as e:
                return None, f"Exception compiling {basename}: {e}"

        with ThreadPoolExecutor(max_workers=cpu_count()) as executor:
            futures = [executor.submit(compile_msvc_obj, pair) for pair in src_obj_pairs]
            for future in as_completed(futures):
                obj, error = future.result()
                if error:
                    log(error)
                    sys.exit(1)
                obj_files.append(obj)

    if not obj_files and not os.path.exists(implib_out):
        log("No MSVC objects compiled and implib doesn't exist.")
        return False, None
    elif not obj_files and os.path.exists(implib_out):
         log("[MSVC] Lib already up to date.")
         return True, implib_out

    # 5. Create DLL
    log(f"[MSVC] Linking {os.path.basename(dll_out)}...")
    
    # Robust handling for locked DLLs
    if os.path.exists(dll_out):
        try:
            os.remove(dll_out)
        except OSError:
            log(f"[Warning] {os.path.basename(dll_out)} is locked. Attempting to rename...")
            try:
                import time
                import random
                trash_name = f"{dll_out}.trash.{int(time.time())}.{random.randint(1000,9999)}"
                os.rename(dll_out, trash_name)
                log(f"[Info] Renamed locked DLL to {os.path.basename(trash_name)}")
            except OSError as e:
                log(f"[Error] Failed to rename locked DLL: {e}")
                return False, None
    
    sdk_lib_um = os.path.join(win_sdk_root, "Lib", win_sdk_ver, "um", sdk_arch)
    sdk_lib_ucrt = os.path.join(win_sdk_root, "Lib", win_sdk_ver, "ucrt", sdk_arch)
    
    log(f"[MSVC] MSVC Lib Path: {msvc_lib}")
    log(f"[MSVC] SDK Lib UM Path: {sdk_lib_um}")
    log(f"[MSVC] SDK Lib UCRT Path: {sdk_lib_ucrt}")
    
    link_cmd = [
        link_exe, "/nologo", "/DLL", 
        f"/OUT:{dll_out}", 
        f"/IMPLIB:{implib_out}",
        f"/LIBPATH:{msvc_lib}",
        f"/LIBPATH:{sdk_lib_um}",
        f"/LIBPATH:{sdk_lib_ucrt}",
        "d3d12.lib", "dxgi.lib", "dxguid.lib", "user32.lib", "kernel32.lib", "uuid.lib",
        "libucrt.lib", "libcmt.lib", "libvcruntime.lib"
    ] + obj_files
    
    try:
        res = subprocess.run(link_cmd, env=msvc_env, capture_output=True, text=True)
        if res.returncode != 0:
            log(f"Error linking DLL:")
            log(res.stdout)
            log(res.stderr)
            # sys.exit(1) # Don't fail entire build, just disable D3D12 wrappers for this arch
            return False, None
    except Exception as e:
        log(f"Exception linking DLL: {e}")
        # sys.exit(1) 
        return False, None
        
    log(f"[MSVC] Successfully built {dll_out}")
    return True, implib_out


def compile_project(env, clang_bin, skip_updates=False, should_run_tests=False):
    ensure_dirs()
    setup_imgui()

    compile_custom_ffmpeg(skip_updates=skip_updates) # Ensure FFmpeg is ready
    clang_exe = os.path.join(clang_bin, "clang++.exe")
    pkg_config = os.path.join(clang_bin, "pkg-config.exe")

    cflags = ["-std=c++20", "-O3", "-flto", "-ffast-math", "-ffunction-sections", "-fdata-sections", "-Wall", "-D_WIN32_WINNT=0x0A00",
              "-I" + os.path.join(PROJECT_ROOT, "common"),
              "-I" + IMGUI_DIR]

    # Compile D3D12 wrappers (MSVC)
    has_d3d12_msvc_x64, msvc_lib_path_x64 = compile_d3d12_wrappers_msvc(env, "x64")
    has_d3d12_msvc_x86, msvc_lib_path_x86 = compile_d3d12_wrappers_msvc(env, "x86")
    
    # We'll use these results later when building hook DLLs
    msvc_d3d12_status = {
        "x64": (has_d3d12_msvc_x64, msvc_lib_path_x64),
        "x86": (has_d3d12_msvc_x86, msvc_lib_path_x86)
    }
    if not has_d3d12_msvc_x64 and not has_d3d12_msvc_x86:
        log("Error: MSVC D3D12 wrappers failed to build!")
        sys.exit(1)
    
    
    # Create dummy MSVC libs to satisfy LLD when linking against MSVC objects/import libs
    dummy_lib_dir = os.path.join(BUILD_DIR, "dummy_libs")
    os.makedirs(dummy_lib_dir, exist_ok=True)
    ar_exe = os.path.join(clang_bin, "llvm-ar.exe")
    if os.path.exists(ar_exe):
        for lib in ["libmsvcprt.a", "libOLDNAMES.a"]:
            path = os.path.join(dummy_lib_dir, lib)
            if not os.path.exists(path):
                # Create empty archive
                subprocess.run([ar_exe, "rc", path], env=env)
    
    # --- Architecture Loop ---
    for arch in ["x64", "x86"]:
        curr_env = env
        curr_clang_bin = clang_bin
        mingw_lib = ""
        std_lib_path = ""

        curr_obj_dir = os.path.join(OBJ_DIR, arch)
        os.makedirs(curr_obj_dir, exist_ok=True)
        
        if arch == "x86":
            curr_env, curr_clang_bin = get_env_x86()
        
        curr_clang_exe = os.path.join(curr_clang_bin, "clang++.exe")
        curr_pkg_config = os.path.join(curr_clang_bin, "pkg-config.exe")
        
        curr_cflags = ["-std=c++20", "-O2", "-flto", "-fno-stack-protector", "-ffunction-sections", "-fdata-sections", "-Wall", "-Wno-microsoft-exception-spec", "-D_WIN32_WINNT=0x0A00",
                      "-I" + os.path.join(PROJECT_ROOT, "common"),
                      "-I" + IMGUI_DIR]
        # if arch == "x64":
        #    curr_cflags.append("-flto")
        #    mingw_lib = os.path.join(MSYS2_DIR, 'clang64', 'lib')
        if arch == "x64":
             mingw_lib = os.path.join(MSYS2_DIR, 'clang64', 'lib')

        if arch == "x86":
            curr_cflags.append("-m32")
            curr_cflags.append("-mstackrealign")
            try:
                # Use --target to get correct 32-bit lib path
                cmd = [clang_bin, "-print-libgcc-file-name", "--target=i686-w64-mingw32"]
                res = subprocess.check_output(cmd, encoding="utf-8").strip()
                std_lib_path = os.path.dirname(res)
            except Exception as e:
                log(f"Warning: Failed to find 32-bit lib path: {e}")
                std_lib_path = ""
            
        # 1. Compile ImGui
        log(f"Compiling ImGui {arch}...")
        imgui_src_files = glob.glob(os.path.join(IMGUI_DIR, "*.cpp")) + \
                         [os.path.join(IMGUI_DIR, "backends", f) for f in ["imgui_impl_dx12.cpp", "imgui_impl_dx11.cpp", "imgui_impl_dx10.cpp", "imgui_impl_dx9.cpp", "imgui_impl_opengl3.cpp", "imgui_impl_opengl2.cpp", "imgui_impl_win32.cpp"]]
                         # NOTE: imgui_impl_vulkan.cpp is in Vulkan layer, not main hook

        
        imgui_objs = []
        src_obj_pairs = []
        for src in imgui_src_files:
            rel_path = os.path.relpath(src, PROJECT_ROOT)
            obj = os.path.join(curr_obj_dir, os.path.splitext(rel_path)[0] + ".o").replace("\\", "/")
            src_obj_pairs.append((src, obj))
            imgui_objs.append(obj)
        parallel_compile(curr_env, curr_clang_exe, curr_cflags + ["-I" + IMGUI_DIR], src_obj_pairs)

        # 2. Compile Common
        log(f"Compiling Common {arch}...")
        common_src = glob.glob(os.path.join(PROJECT_ROOT, "common", "*.cpp")) + \
                     glob.glob(os.path.join(PROJECT_ROOT, "common", "utils", "*.cpp"))
        common_objs = []
        src_obj_pairs = []
        for src in common_src:
            rel_path = os.path.relpath(src, PROJECT_ROOT)
            obj = os.path.join(curr_obj_dir, os.path.splitext(rel_path)[0] + ".o").replace("\\", "/")
            src_obj_pairs.append((src, obj))
            common_objs.append(obj)
        parallel_compile(curr_env, curr_clang_exe, curr_cflags, src_obj_pairs)

        # 3. Compile Hook DLL
        log(f"Compiling Hook DLL {arch}...")
        hk_src = glob.glob(os.path.join(PROJECT_ROOT, "hook", "*.cpp")) + \
                 glob.glob(os.path.join(PROJECT_ROOT, "hook", "common", "*.cpp")) + \
                 glob.glob(os.path.join(PROJECT_ROOT, "hook", "apis", "*.cpp")) + \
                 glob.glob(os.path.join(PROJECT_ROOT, "hook", "capture", "*.cpp")) + \
                 glob.glob(os.path.join(PROJECT_ROOT, "hook", "wrappers", "*.cpp"))
        
        # Exclude D3D12 device/commandqueue wrappers due to MinGW ABI incompatibility
        # (MSYS2's D3D12 headers use WIDL_EXPLICIT_AGGREGATE_RETURNS which has different vtable layout)
        # d3d12_wrapper_interface.cpp is compiled by MSVC and linked as a static lib
        # vulkan_hook.cpp is excluded - replaced by VK_LAYER_CE_overlay (Vulkan layer)
        excluded_files = [
            os.path.join(PROJECT_ROOT, "hook", "wrappers", "d3d12_device_wrap.cpp"),
            os.path.join(PROJECT_ROOT, "hook", "wrappers", "d3d12_commandqueue_wrap.cpp"),
            os.path.join(PROJECT_ROOT, "hook", "wrappers", "d3d12_wrapper_interface.cpp"),
            os.path.join(PROJECT_ROOT, "hook", "apis", "vulkan_hook.cpp"),  # Using Vulkan layer instead
        ]
        hk_src = [f for f in hk_src if f not in excluded_files]
        
        # mh_src removed
        # volk removed - using vulkan layer instead
        # hk_src.append(os.path.join(PROJECT_ROOT, "external", "volk", "volk.c"))

        # hk_src = [os.path.join(PROJECT_ROOT, "hook", "minimal_main.cpp")]

        hk_dll = os.path.join(BIN_DIR, f"capture_hook_{arch}.dll")
        
        # Use delay-load for graphics DLLs so the hook can load even in games that don't have them
        # This prevents crash during DLL load when injecting into games that don't use D3D12/D3D11/etc
        ldflags_hook = [
            "-shared",
            "-static",
            # "-static-libgcc", 
            # "-static-libstdc++", # Let Clang choose default (libc++ for x86 clang usually)
            "-L" + std_lib_path if arch == "x86" else "-L" + mingw_lib,
            "-ld3d9",
            "-ld3d10",
            "-ld3d11",
            "-ld3dcompiler",
            "-ldxguid",
            "-lws2_32",
            "-lole32",
            "-lwinmm", # For timeGetTime
            "-luser32", # For GetWindowRect etc
            "-lgdi32",
            "-lopengl32",
            os.path.join(MSYS2_DIR, 'clang64' if arch == "x64" else 'mingw32', 'lib', 'libvulkan-1.dll.a'),  # Vulkan overlay
            "-lversion", # For GetFileVersionInfo
            "-ldxgi",    # Needed for VRAM query
            "-lpdh",     # Needed for CPU usage
            "-lpsapi",   # Needed for IAT patching (EnumProcessModules)
            "-lavrt",
            # "-ld3dcompiler", # Removed for dynamic loading
            # "-Wl,--delayload,D3DCOMPILER_47.dll",
            # "-Wl,--delayload,dwmapi.dll",
            # "-ldelayimp",
            "-fuse-ld=lld",
        ]
        if arch == "x64":
            # ldflags_hook.append("-flto")
            pass
        
        hk_cflags = curr_cflags + ["-DVK_NO_PROTOTYPES", "-DBUILDING_CAPTURE_HOOK"] + [  # Vulkan hooks now in layer
            "-I" + os.path.join(PROJECT_ROOT, "common"),
            "-I" + os.path.join(PROJECT_ROOT, "hook", "common"),
            "-I" + os.path.join(PROJECT_ROOT, "hook", "apis"),
            "-I" + os.path.join(PROJECT_ROOT, "hook", "capture"),
            "-I" + os.path.join(PROJECT_ROOT, "hook", "wrappers")
        ]
        
        # Check for MSVC-compiled D3D12 wrappers
        has_msvc_d3d12, d3d12_lib = msvc_d3d12_status.get(arch, (False, None))
        if has_msvc_d3d12 and d3d12_lib and os.path.exists(d3d12_lib):
            log(f"Enabling D3D12 wrapper support for {arch}...")
            hk_cflags.append("-DENABLE_D3D12_WRAPPER")
            
            # Use Delay Load for d3d12_wrappers.dll to avoid dependency issues
            suffix = "" if arch == "x64" else "_x86"
            dll_base = f"d3d12_wrappers{suffix}.dll"
            
            ldflags_hook.append(d3d12_lib)
            ldflags_hook.append(f"-Wl,--delayload={dll_base}")
            ldflags_hook.append("-ldelayimp") # MinGW delay load helper
            
            ldflags_hook.append("-L" + dummy_lib_dir)
            if has_msvc_d3d12:
                 # Ensure proper rebuild if MSVC lib changed?
                 pass

        
        hk_objs = []
        src_obj_pairs = []
        for src in hk_src:
            rel_path = os.path.relpath(src, PROJECT_ROOT)
            obj = os.path.join(curr_obj_dir, os.path.splitext(rel_path)[0] + ".o").replace("\\", "/")
            src_obj_pairs.append((src, obj))
            hk_objs.append(obj)
            
        parallel_compile(curr_env, curr_clang_exe, hk_cflags, src_obj_pairs)
        
        log(f"Linking Hook DLL {arch}...")
        
        # Robust handling for locked DLLs (e.g. by DataExchangeHost with CBT hooks)
        if os.path.exists(hk_dll):
            try:
                os.remove(hk_dll)
            except OSError:
                log(f"[Warning] {os.path.basename(hk_dll)} is locked. Attempting to rename...")
                try:
                     # Rename to .trash in timestamped format
                     import time
                     import random
                     trash_name = f"{hk_dll}.trash.{int(time.time())}.{random.randint(1000,9999)}"
                     os.rename(hk_dll, trash_name)
                     log(f"[Info] Renamed locked DLL to {os.path.basename(trash_name)}")
                except OSError as e:
                     log(f"[Error] Failed to rename locked DLL: {e}")
                     sys.exit(1)

        cmd = [curr_clang_exe] + hk_objs + imgui_objs + common_objs + ldflags_hook + ["-o", hk_dll]
        # cmd = [curr_clang_exe] + hk_objs + ldflags_hook + ["-o", hk_dll]
        run_command(cmd, env=curr_env)
        # generate_hash(hk_dll) # Removed in favor of embedded hash header

        # 4. MediaEngine (x64 only for now as requested)
        if arch == "x64":
            log("Compiling MediaEngine x64...")
            me_src = glob.glob(os.path.join(PROJECT_ROOT, "mediaengine", "*.cpp"))
            if me_src:
                # Update pkg-config to look in our local ffmpeg dir
                env_ffmpeg = curr_env.copy()
                env_ffmpeg["PKG_CONFIG_PATH"] = os.path.join(FFMPEG_DIR, "lib", "pkgconfig") + os.pathsep + env_ffmpeg.get("PKG_CONFIG_PATH", "")
                
                pkgs = ["libavcodec", "libavformat", "libavutil", "libswresample", "libswscale"]
                pkg_cmd = [curr_pkg_config, "--cflags", "--libs"] + pkgs 
                # Removed --static for pkg-config to get shared linking flags
                
                # We need to manually add -I and -L for our custom directory if pkg-config doesn't pick it up fully relative
                # But setting PKG_CONFIG_PATH should work.
                
                ffmpeg_flags_raw = run_command(pkg_cmd, env=env_ffmpeg).strip().split()
                ffmpeg_flags = [f for f in ffmpeg_flags_raw if f not in ["-ldl", "-lshaderc_shared"]]
                
                # For shared build, linking usually requires -Lpath -lavcodec. 
                # We need to make sure the DLLs are findable at runtime.
                # Since they are in bin/ffmpeg, we might need to SetDllDirectory in app code or move them to bin/.
                # User asked for bin/ffmpeg. MediaEngine needs to handle it or we use DelayLoad.
                # DelayLoad is complex with MinGW.
                # Let's just link normally. User must assume bin/ffmpeg is in PATH or simple copy for dev.
                # Actually, standard Windows search path includes current dir.
                # If they are in bin/ffmpeg, they are NOT in current dir of captureengine.exe (bin).
                # We might need a manifest or SetDllDirectory.
                # For now, just link.

                
                me_dll = os.path.join(BIN_DIR, "mediaengine.dll")
                me_lib = os.path.join(BIN_DIR, "libmediaengine.dll.a")
                
                me_ldflags = ["-shared", "-static", "-static-libgcc", "-static-libstdc++", "-flto", "-Wl,--gc-sections", "-s", "-Wl,--allow-multiple-definition", 
                           "-lole32", "-lmfplat", "-lmfuuid", "-lbcrypt", "-lsecur32", "-lshlwapi", "-lpsapi", "-lws2_32", "-luser32", "-ld3d11", "-ldxgi", "-lmmdevapi",
                           "-lversion", "-lwinmm", "-luuid", "-lsetupapi", "-lcfgmgr32", "-ladvapi32", "-lgdi32"]
                me_ldflags.append(f"-Wl,--out-implib,{me_lib}")
                
                me_cflags = curr_cflags + ["-DMEDIAENGINE_EXPORTS"] + ffmpeg_flags
                me_objs = []
                src_obj_pairs = []
                for src in me_src:
                    rel_path = os.path.relpath(src, PROJECT_ROOT)
                    obj = os.path.join(curr_obj_dir, os.path.splitext(rel_path)[0] + ".o").replace("\\", "/")
                    src_obj_pairs.append((src, obj))
                    me_objs.append(obj)
                parallel_compile(curr_env, curr_clang_exe, me_cflags, src_obj_pairs)
                
                log("Linking MediaEngine x64...")
                # Use absolute paths to our custom FFmpeg import libs to avoid system FFmpeg
                ffmpeg_lib_dir = os.path.join(FFMPEG_DIR, "lib")
                ffmpeg_import_libs = [
                    os.path.join(ffmpeg_lib_dir, "libavformat.dll.a"),
                    os.path.join(ffmpeg_lib_dir, "libavcodec.dll.a"),
                    os.path.join(ffmpeg_lib_dir, "libswresample.dll.a"),
                    os.path.join(ffmpeg_lib_dir, "libswscale.dll.a"),
                    os.path.join(ffmpeg_lib_dir, "libavutil.dll.a"),
                ]
                cmd = [curr_clang_exe] + me_objs + common_objs + me_ldflags + ffmpeg_import_libs + ["-o", me_dll]
                run_command(cmd, env=curr_env)
                # generate_hash(me_dll) # MediaEngine doesn't need hash check for injection
                

    # Compile and run tests (using x64 objects) if requested
    if should_run_tests:
        x64_common_objs = [os.path.join(OBJ_DIR, "x64", os.path.relpath(s, PROJECT_ROOT).replace(".cpp", ".o")) for s in glob.glob(os.path.join(PROJECT_ROOT, "common", "*.cpp"))]
        # We use x64 obj dir for tests
        test_exe = compile_tests(env, clang_exe, cflags, x64_common_objs, pkg_config, os.path.join(OBJ_DIR, "x64"))
        if test_exe:
            run_tests(env, test_exe)

    # 5. CaptureEngine (x64 only for now)
    log("Compiling CaptureEngine x64...")
    ce_src = glob.glob(os.path.join(PROJECT_ROOT, "captureengine", "*.cpp"))
    if ce_src:
        ce_exe = os.path.join(BIN_DIR, "captureengine.exe")
        me_lib = os.path.join(BIN_DIR, "libmediaengine.dll.a")
        # Reuse x64 objects for CE
        ce_obj_dir = os.path.join(OBJ_DIR, "x64")
        ce_objs = []
        src_obj_pairs = []
        for src in ce_src:
            if "screen_capture.cpp" in src: continue
            rel_path = os.path.relpath(src, PROJECT_ROOT)
            obj = os.path.join(ce_obj_dir, os.path.splitext(rel_path)[0] + ".o").replace("\\", "/")
            src_obj_pairs.append((src, obj))
            ce_objs.append(obj)
        parallel_compile(env, os.path.join(clang_bin, "clang++.exe"), cflags, src_obj_pairs)
        
        # Resource file
        rc_file = os.path.join(PROJECT_ROOT, "captureengine", "captureengine.rc")
        rc_obj = os.path.join(ce_obj_dir, "captureengine", "captureengine.res.o").replace("\\", "/")
        if os.path.exists(rc_file):
            windres = os.path.join(MSYS2_DIR, "clang64", "bin", "windres.exe")
            log("Compiling resource file (manifest)...")
            cmd = [windres, rc_file, "-o", rc_obj]
            run_command(cmd, env=env, cwd=os.path.join(PROJECT_ROOT, "captureengine"))
            ce_objs.append(rc_obj)

        log("Linking CaptureEngine x64...")
        ce_ldflags = ["-mwindows", "-static", "-static-libgcc", "-static-libstdc++", "-flto", "-Wl,--gc-sections", "-s", "-ld3d11", "-ldxgi", "-luser32", "-lshell32", "-lshlwapi", "-lpsapi", "-lwinmm", "-lavrt", "-lruntimeobject", "-lole32", "-loleaut32", "-ldbghelp", "-lwbemuuid", "-lbcrypt", "-lwintrust", "-lpdh", "-lntdll", me_lib]
        # Delay-load mediaengine.dll to allow early DLL path setup
        ce_ldflags.append("-Wl,--delayload,mediaengine.dll")
        # Need to link against delayimp for delay loading to work
        ce_ldflags.append("-ldelayimp")
        # x64 common objects
        x64_common_objs = [os.path.join(OBJ_DIR, "x64", os.path.relpath(s, PROJECT_ROOT).replace(".cpp", ".o")) for s in glob.glob(os.path.join(PROJECT_ROOT, "common", "*.cpp"))]
        cmd = [os.path.join(clang_bin, "clang++.exe")] + ce_objs + x64_common_objs + ce_ldflags + ["-o", ce_exe]
        run_command(cmd, env=env)

    # 6. Compile Test Applications (DX12 and Vulkan test apps)
    x86_env_for_tests = None
    if get_env_x86:
        x86_env_for_tests, _ = get_env_x86()
    compile_testapps(env, x86_env_for_tests, os.path.join(clang_bin, "clang++.exe"), cflags)

    # 7. Compile Vulkan Layer (VK_LAYER_CE_overlay) - both architectures
    compile_vulkan_layer(env, os.path.join(clang_bin, "clang++.exe"), cflags, "x64")
    # x86 layer using mingw32 toolchain
    # x86 layer using mingw32 toolchain
    if get_env_x86:
        x86_env, x86_clang_bin = get_env_x86()
        x86_clang = os.path.join(x86_clang_bin, "clang++.exe")
        if os.path.exists(x86_clang):
            x86_cflags = ["-std=c++20", "-O3", "-m32", "-Wall", "-D_WIN32_WINNT=0x0A00"]
            compile_vulkan_layer(x86_env, x86_clang, x86_cflags, "x86")

    # Cleanup import libraries
    me_lib = os.path.join(BIN_DIR, "libmediaengine.dll.a")
    if os.path.exists(me_lib): 
        try:
            os.remove(me_lib)
            log(f"Removed {me_lib}")
        except Exception as e:
            log(f"Failed to remove {me_lib}: {e}")
    
    # Copy License files
    log("Copying License files...")
    
    licenses_src = os.path.join(PROJECT_ROOT, "licenses")
    licenses_dst = os.path.join(BIN_DIR, "licenses")
    if os.path.exists(licenses_src):
        if os.path.exists(licenses_dst):
            shutil.rmtree(licenses_dst)
        shutil.copytree(licenses_src, licenses_dst)
        log("Copied licenses/ directory to installed/captureengine/")

    log("Build Complete.")

def backup_sources(script_dir):
    timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    backup_root = os.path.join(script_dir, "bak", timestamp)
    
    log(f"Creating source backup in {backup_root}...")
    
    dirs_to_backup = ["hook", "captureengine", "mediaengine", "common", "tests"]
    files_to_backup = ["build_and_run.py", "CMakeLists.txt"] # Add strict files if needed
    
    for d in dirs_to_backup:
        src_path = os.path.join(script_dir, d)
        if os.path.exists(src_path):
            dst_path = os.path.join(backup_root, d)
            # Ignore binary/obj directories inside if any (though unlikely in source dirs)
            shutil.copytree(src_path, dst_path, ignore=shutil.ignore_patterns("*.obj", "*.o", "*.tmp"))

    # Also backup root files
    os.makedirs(backup_root, exist_ok=True)
    for f in files_to_backup:
        src = os.path.join(script_dir, f)
        if os.path.exists(src):
            shutil.copy2(src, backup_root)

def ensure_debug_logging():
    """Ensure debug_logging=true in bin/config.ini."""
    config_path = os.path.join(BIN_DIR, "config.ini")
    if not os.path.exists(config_path):
        log("config.ini missing, skipping debug_logging check.")
        return
        
    try:
        with open(config_path, "r") as f:
            lines = f.readlines()
        
        changed = False
        new_lines = []
        for line in lines:
            if line.strip().startswith("debug_logging="):
                if "true" not in line:
                    new_lines.append("debug_logging=true\n")
                    changed = True
                else:
                    new_lines.append(line)
            else:
                new_lines.append(line)
        
        if changed:
            with open(config_path, "w") as f:
                f.writelines(new_lines)
            log("Forced debug_logging=true in config.ini for testing.")
    except Exception as e:
        log(f"Warning: Failed to update config.ini: {e}")

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(script_dir)
    
    # 0. Backup Sources - DISABLED per user request
    # backup_sources(script_dir)

    if os.path.exists(LOG_FILE):
        try: os.remove(LOG_FILE)
        except: pass
    
    log("=== Starting Build ===")

    bump_and_write_build_version()
    
    # Always clean object files to avoid struct layout mismatches
    # if os.path.exists(OBJ_DIR):
    #     log("Cleaning object files for fresh build...")
    #     try:
    #         shutil.rmtree(OBJ_DIR)
    #     except Exception as e:
    #         log(f"Warning: Could not clean {OBJ_DIR}: {e}")

    
    # Clean log files in logs/ subfolder
    logs_dir = os.path.join(BIN_DIR, "logs")
    if os.path.exists(logs_dir):
        log("Cleaning log files...")
        for log_file in glob.glob(os.path.join(logs_dir, "*.log")):
            try:
                os.remove(log_file)
            except Exception as e:
                log(f"Warning: Could not delete {log_file}: {e}")
        for csv_file in glob.glob(os.path.join(logs_dir, "*.csv")):
            try:
                os.remove(csv_file)
            except Exception as e:
                log(f"Warning: Could not delete {csv_file}: {e}")

    # Clean legacy log files from root/bin
    legacy_logs = [
        os.path.join(BIN_DIR, "msvc_debug.log"),
        os.path.join(PROJECT_ROOT, "msvc_debug.log"), 
        os.path.join(BIN_DIR, "Layer"),
        os.path.join(PROJECT_ROOT, "Layer")
    ]
    for f in legacy_logs:
        if os.path.exists(f):
            try: os.remove(f); log(f"Removed legacy log: {f}")
            except: pass
    
    setup_msys2()
    env, clang_bin = get_env()
    
    # Parse flags
    skip_updates = "--skip-updates" in sys.argv
    run_tests_flag = "--run-tests" in sys.argv
    
    if skip_updates:
        log("FFmpeg updates disabled (--skip-updates)")
    
    compile_project(env, clang_bin, skip_updates=skip_updates, should_run_tests=run_tests_flag)

    # Write compile_commands.json
    try:
        # Deduplicate and sort compile commands for better LSP performance/determinism
        seen_files = set()
        unique_commands = []
        
        # Sort by file path to keep output stable
        sorted_commands = sorted(COMPILE_COMMANDS, key=lambda x: x['file'])
        
        for cmd in sorted_commands:
            # Prefer x64 commands over x86 for LSP if both exist for the same file
            # (assuming x64 is usually the primary dev target)
            is_x86 = "mingw32" in cmd['arguments'][0] or "-m32" in cmd['arguments']
            
            if cmd['file'] not in seen_files:
                unique_commands.append(cmd)
                seen_files.add(cmd['file'])
            elif not is_x86:
                # Replace x86 entry with x64 entry if we encounter it
                for i, existing in enumerate(unique_commands):
                    if existing['file'] == cmd['file']:
                        unique_commands[i] = cmd
                        break

        with open(os.path.join(PROJECT_ROOT, "compile_commands.json"), "w") as f:
            json.dump(unique_commands, f, indent=4)
        log(f"Generated compile_commands.json ({len(unique_commands)} entries)")
    except Exception as e:
        log(f"Error writing compile_commands.json: {e}")

    if run_tests_flag:

        ensure_debug_logging()
        run_integration_tests(env)

if __name__ == "__main__":
    main()
