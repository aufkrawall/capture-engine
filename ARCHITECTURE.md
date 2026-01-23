# System Architecture

## High-Level Overview

The **Capture Project** is a high-performance game capture solution consisting of two main processes communicating via Shared Memory IPC.

1.  **Hook (In-Process):** A DLL injected into the target game process (DirectX 9/10/11/12, Vulkan, OpenGL). It intercepts frame presentation calls, copies the frame to a shared texture/memory, and handles overlay rendering.
2.  **CaptureEngine (Out-of-Process):** A separate executable that acts as the "Server". It consumes frames from shared memory, encodes them (using FFmpeg/MediaEngine), and manages the recording logic.

## Data Flow

```mermaid
graph TD
    Game[Game Process] -->|Present()| Hook[Hook DLL]
    Hook -->|Copy Frame| SharedMem[Shared Memory Ring Buffer]
    SharedMem -->|Read Frame| CE[CaptureEngine.exe]
    CE -->|Encode| ME[MediaEngine (FFmpeg)]
    ME -->|Write| Disk[MKV File]
    CE -->|Overlay Data| SharedMem
    SharedMem -->|Render Overlay| Hook
```

## Core Components

### 1. Hook (`hook/`)
*   **Purpose:** Injected DLL that hooks graphics APIs.
*   **Key Logic:**
    *   **APIs:** `hook/apis/` contains specific hooks for DX12, DX11, Vulkan, etc.
    *   **IPC Client:** `hook/common/ipc_client.cpp` communicates with the Engine.
    *   **Overlay:** `hook/common/overlay.cpp` renders the UI (ImGui) inside the game.
    *   **Wrappers:** `hook/wrappers/` contains "Trampoline" hooks to intercept calls safely.

### 2. CaptureEngine (`captureengine/`)
*   **Purpose:** The main application process.
*   **Key Logic:**
    *   **IPC Server:** `captureengine/ipc.cpp` creates the shared memory mapping.
    *   **Service Loop:** `captureengine/main.cpp` runs the main loop, polling for frames.
    *   **Injection:** `captureengine/injection.cpp` handles injecting the Hook DLL into target processes.

### 3. MediaEngine (`mediaengine/`)
*   **Purpose:** Video/Audio encoding abstraction layer.
*   **Key Logic:**
    *   Wraps FFmpeg (avcodec, avformat).
    *   Handles color conversion and hardware acceleration where available.

### 4. Common (`common/`)
*   **Purpose:** Code shared between Hook and Engine.
*   **Key Logic:**
    *   **Shared Structs:** `capture_base.h` and `shared_defs.h` define the memory layout of the IPC buffer. **Critical:** These must match exactly in both processes.
    *   **Logging:** `common/logging.h` provides cross-process logging.

## IPC Mechanism (Critical)
*   **Type:** Shared Memory (Memory Mapped File) + Named Pipes (for signaling).
*   **Structure:** A lock-free Ring Buffer defined in `common/shared_defs.h`.
*   **Synchronization:** Uses `std::atomic` head/tail indices.
*   **Constraint:** The Hook must be extremely fast to avoid stalling the game's render thread. Complex processing is offloaded to CaptureEngine.

## Directory Map

*   `hook/` - The injected DLL source.
*   `captureengine/` - The main executable source.
*   `mediaengine/` - Encoding logic.
*   `common/` - Shared types/utils (IPC definitions).
*   `testapp/` - Dummy games for testing integration.
*   `tests/` - Unit tests (GTest).
*   `build.py` - The build system.
