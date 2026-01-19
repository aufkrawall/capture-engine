# Zero-Copy Video Capture Plan - DX11/DX12/Vulkan with FFmpeg D3D11 Video Processing

## Executive Summary

This plan outlines the implementation of zero-copy video capture for DirectX 11, DirectX 12, and Vulkan applications using FFmpeg's D3D11VA hardware acceleration. The goal is to eliminate CPU copies and intermediate GPU copies, ensuring textures flow directly from the game to the encoder on the GPU.

**Key Constraints:**
- Zero-copy: No CPU copyback, no intermediate GPU copies where avoidable
- No D3D11On12: Direct D3D11 resource creation on same adapter
- FFmpeg D3D11VA: All paths must output D3D11-compatible textures
- Hardware acceleration: NVENC (NVIDIA) and AMF (AMD) via D3D11VA

---

## Current Architecture Analysis

### Existing Capture Pipeline

```
Game Rendering
    ↓
Hook (DX11/DX12/Vulkan)
    ↓
Shared Texture Export (NT Handles)
    ↓
Shared Memory (Handles + Metadata)
    ↓
Media Engine (OpenSharedResource1)
    ↓
FFmpeg D3D11VA Encode
```

### Current Issues

| API | Current Implementation | Problem | Zero-Copy Compatible? |
|-----|----------------------|---------|----------------------|
| **DX11** | Shared D3D11 textures | Minor: CopyResource in hook | ✅ Yes (already good) |
| **DX12** | D3D12 textures + D3D11 textures | D3D12→D3D12 copy, then D3D12→D3D11 copy | ❌ No (double copy) |
| **Vulkan** | VK_EXTERNAL_MEMORY D3D11_TEXTURE/D3D12_RESOURCE | D3D12_RESOURCE not D3D11-compatible | ⚠️ Partial (depends on export type) |

### Key Code Locations

- **DX11 Hook**: `hook/apis/dx11_hook.cpp` - `DX11Capture::sharedTextures[]`
- **DX12 Hook**: `hook/apis/dx12_hook.cpp` - `DX12Capture::d3d11Textures[]`, `DX12Capture::d3d12Textures[]`
- **Vulkan Layer**: `hook/vulkan_layer/layer_capture.cpp` - `VulkanCaptureState::exportedHandles[]`
- **Video Encoder**: `mediaengine/video_encoder.cpp` - FFmpeg D3D11VA integration
- **Shared Memory**: `common/shared_defs.h` - `SharedMemoryLayout::sharedHandles[]`

---

## Zero-Copy Architecture Design

### Core Principle

All APIs (DX11, DX12, Vulkan) must export **D3D11-compatible shared textures** that FFmpeg's D3D11VA can directly import without conversion.

### Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                         GAME PROCESS                             │
├─────────────────────────────────────────────────────────────────┤
│                                                                   │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐      │
│  │   DX11       │    │   DX12       │    │   Vulkan     │      │
│  │  Backbuffer  │    │  Backbuffer  │    │  Swapchain   │      │
│  └──────┬───────┘    └──────┬───────┘    └──────┬───────┘      │
│         │                   │                   │               │
│         ▼                   ▼                   ▼               │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐      │
│  │  CopyResource│    │  CopyResource│    │  vkCmdBlit   │      │
│  │  (D3D11)     │    │  (D3D12)     │    │  (Vulkan)    │      │
│  └──────┬───────┘    └──────┬───────┘    └──────┬───────┘      │
│         │                   │                   │               │
│         ▼                   ▼                   ▼               │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │         SHARED D3D11 TEXTURES (NT Handles)              │   │
│  │     CreateSharedHandle → IDXGIResource1::CreateSharedHandle│
│  └───────────────────────┬─────────────────────────────────┘   │
│                          │                                       │
└──────────────────────────┼───────────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────────┐
│                    MEDIA ENGINE PROCESS                          │
├─────────────────────────────────────────────────────────────────┤
│                          │                                       │
│                          ▼                                       │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │    OpenSharedResource1 (NT Handle) → ID3D11Texture2D    │   │
│  └───────────────────────────────┬─────────────────────────┘   │
│                                  │                               │
│                                  ▼                               │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │        FFmpeg D3D11VA Video Processor (NV12)            │   │
│  │     ID3D11VideoProcessor → Convert BGRA→NV12            │   │
│  └───────────────────────────────┬─────────────────────────┘   │
│                                  │                               │
│                                  ▼                               │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │            Hardware Encoder (NVENC/AMF)                 │   │
│  │              AV_PIX_FMT_D3D11 → H.264/HEVC              │   │
│  └─────────────────────────────────────────────────────────┘   │
│                                                                   │
└───────────────────────────────────────────────────────────────────┘
```

---

## Implementation Plan by API

### 1. DX11 Capture (Minimal Changes - Already Good)

**Current State**: ✅ Already zero-copy compatible

DX11 already creates shared D3D11 textures with NT handles that work directly with FFmpeg D3D11VA.

**File**: `hook/apis/dx11_hook.cpp`

**Current Flow**:
1. `CopyResource` from backbuffer to shared texture
2. `IDXGIResource1::CreateSharedHandle` exports NT handle
3. Media engine imports via `OpenSharedResource1`

**Required Changes**:
- None (already zero-copy compatible)
- Optional: Consider using keyed mutex instead of fences for better sync

**Key Code**: `dx11_hook.cpp:700-750` (DX11Capture class)

---

### 2. DX12 Capture (Major Refactor Required)

**Current State**: ❌ NOT zero-copy (double copy: D3D12→D3D12, then D3D12→D3D11)

**File**: `hook/apis/dx12_hook.cpp`

**Current Issues**:
1. Creates D3D12 textures for internal copy
2. Creates separate D3D11 textures for media encoder
3. Performs D3D12→D3D12 GPU copy first
4. Then performs D3D12→D3D11 copy (requires D3D11On12 or manual copy)

**New Zero-Copy Architecture**:

Instead of creating D3D12 textures, create **D3D11 textures directly** on the same GPU adapter:

```cpp
// In DX12Capture class
struct DX12Capture {
    // REMOVE: ID3D12Resource* sharedTextures[CAPTURE_TEXTURE_COUNT];
    // KEEP: ID3D11Texture2D* sharedTextures[CAPTURE_TEXTURE_COUNT];

    ID3D11Device* d3d11Device = nullptr;
    ID3D11DeviceContext* d3d11Context = nullptr;
    ID3D11Fence* d3d11Fence = nullptr;
    HANDLE sharedFenceHandle = NULL;

    // DX12 fence for signaling when backbuffer is ready
    ID3D12Fence* dx12Fence = nullptr;
    UINT64 dx12FenceValue = 0;
};
```

**Implementation Steps**:

#### Step 1: Initialize D3D11 Device on Same Adapter
```cpp
// Get DXGI adapter from D3D12 device
IDXGIAdapter* adapter = nullptr;
HRESULT hr = g_Device->GetAdapter(&adapter);

// Create D3D11 device on SAME adapter (critical!)
D3D11CreateDevice(
    adapter, D3D11_DRIVER_TYPE_UNKNOWN, nullptr,
    0, nullptr, 0, D3D11_SDK_VERSION,
    &d3d11Device, nullptr, &d3d11Context
);
```

#### Step 2: Create Shared D3D11 Textures
```cpp
D3D11_TEXTURE2D_DESC texDesc = {};
texDesc.Width = width;
texDesc.Height = height;
texDesc.MipLevels = 1;
texDesc.ArraySize = 1;
texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
texDesc.SampleDesc.Count = 1;
texDesc.Usage = D3D11_USAGE_DEFAULT;
texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
    d3d11Device->CreateTexture2D(&texDesc, nullptr, &sharedTextures[i]);

    IDXGIResource1* resource = nullptr;
    sharedTextures[i]->QueryInterface(__uuidof(IDXGIResource1), (void**)&resource);
    resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &sharedTextureHandles[i]);
    resource->Release();
}
```

#### Step 3: Use ID3D11Fence for GPU Synchronization
```cpp
// Create D3D11 fence (D3D11.3+)
D3D11_FENCE_FLAG flag = D3D11_FENCE_FLAG_NONE;
d3d11Device->CreateFence(0, flag, __uuidof(ID3D11Fence), (void**)&d3d11Fence);

// Export fence handle
d3d11Fence->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &sharedFenceHandle);
```

#### Step 4: DX12 → D3D11 Copy via Interop Queue

**Option A: Use D3D11 device from D3D12 (Recommended)**
- Create D3D11 device on same adapter
- Use D3D11ON12 ONLY for the copy queue (not for textures)
- Copy from DX12 resource to D3D11 shared texture

```cpp
// During Present/DetourExecuteCommandLists
// 1. Copy DX12 backbuffer to a staging D3D12 resource
// 2. Use D3D11ON12 to copy from D3D12 to D3D11 shared texture
// 3. Signal D3D11 fence when complete

// NOTE: This is NOT using D3D11On12 for the textures themselves,
// only for the copy operation. The shared textures remain pure D3D11.
```

**Option B: Use DXGI Keyed Mutex (Legacy)**
- Use `IDXGIKeyedMutex` for synchronization
- Avoids need for D3D11ON12 entirely
- Performance may be lower

**Option C: Direct Write to Shared Texture (Most Efficient)**
- DX12 writes directly to D3D11 shared texture (requires interop)
- Most complex but truly zero-copy

**Recommended**: Start with Option A, optimize to Option C later.

#### Step 5: Remove Intermediate D3D12 Textures
- Remove `ID3D12Resource* sharedTextures[CAPTURE_TEXTURE_COUNT]`
- Keep only `ID3D11Texture2D* sharedTextures[CAPTURE_TEXTURE_COUNT]`

**Key Files to Modify**:
- `hook/apis/dx12_hook.cpp:308-350` (DX12Capture class)
- `hook/apis/dx12_hook.cpp:770-850` (InitCapture)
- `hook/apis/dx12_hook.cpp:1500-1600` (Frame copy logic)

---

### 3. Vulkan Layer (Minor Changes)

**Current State**: ⚠️ Partially zero-copy (depends on external memory type)

**File**: `hook/vulkan_layer/layer_capture.cpp`

**Current Behavior**:
- Tries external memory types in priority order:
  1. `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT`
  2. `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT`
  3. `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT`

**Required Change**:
Force `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT` as the primary choice for FFmpeg D3D11VA compatibility.

**Implementation**:

```cpp
// In layer_capture.cpp:203-298 (InitCapture)
// Change priority order

static const ExternalMemoryTypeInfo g_ExternalMemoryTypes[] = {
    // MUST be D3D11_TEXTURE for FFmpeg D3D11VA compatibility
    { VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, "D3D11_TEXTURE (primary)" },
    { VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT, "D3D11_TEXTURE_KMT (legacy)" },
    // REMOVE D3D12_RESOURCE - not compatible with D3D11VA
    // { VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, "D3D12_RESOURCE" }
};
```

**Key Code**: `layer_capture.cpp:64-68` (ExternalMemoryTypeInfo array)

---

## FFmpeg D3D11VA Integration

**File**: `mediaengine/video_encoder.cpp`

### Current State
✅ Already uses D3D11VA (`AV_PIX_FMT_D3D11`) with hardware acceleration

### Current Flow
1. Open shared handle via `OpenSharedResource1`
2. Import into D3D11 device
3. Convert BGRA → NV12 using `ID3D11VideoProcessor`
4. Wrap in `AVFrame` with `AV_PIX_FMT_D3D11`
5. Send to encoder (NVENC/AMF)

### Required Changes
**None for basic functionality** - already compatible with all proposed changes.

### Optional Enhancements

#### 1. Better Synchronization
```cpp
// Use ID3D11Fence instead of IDXGIKeyedMutex
ID3D11Fence* sharedFence = nullptr;
UINT64 fenceValue = 0;

// Wait for capture to complete
d3d11Fence->SetEventOnCompletion(fenceValue, waitEvent);
WaitForSingleObject(waitEvent, INFINITE);
```

#### 2. Pool Reuse
Reuse NV12 textures instead of allocating per-frame:
```cpp
// Create pool of NV12 textures
ID3D11Texture2D* nv12TexturePool[8];
int nv12TexturePoolIndex = 0;

// Reuse textures in round-robin
ID3D11Texture2D* nv12Tex = nv12TexturePool[nv12TexturePoolIndex++ % 8];
```

#### 3. Async Encoding Pipeline
- Separate capture thread from encode thread
- Use multiple fences for overlapping operations

---

## Synchronization Architecture

### Fence-Based Synchronization

```
┌─────────────────────────────────────────────────────────────────┐
│                       SHARED FENCE                              │
│                   (ID3D11Fence / HANDLE)                        │
└─────────────────────────────────────────────────────────────────┘
         ▲                                    │
         │ Signal after copy                 │ Wait before encode
         │                                    │
    ┌────┴────┐                          ┌───┴────┐
    │  Hook   │                          │ Encoder│
    │ Thread  │                          │ Thread │
    └─────────┘                          └────────┘
```

### Implementation

#### In Hook (DX11/DX12/Vulkan):
```cpp
// After copying to shared texture
UINT64 signalValue = currentFenceValue++;
d3d11Fence->Signal(signalValue);

// Write to shared memory
sharedMem->frameSlot[ringIndex].fenceValue = signalValue;
```

#### In Encoder:
```cpp
// Before encoding
ID3D11Fence* fence = GetSharedFence();
UINT64 waitValue = sharedMem->frameSlot[ringIndex].fenceValue;
HANDLE waitEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
fence->SetEventOnCompletion(waitValue, waitEvent);
WaitForSingleObject(waitEvent, INFINITE);
```

---

## Memory Layout

### Shared Memory Structure

```cpp
struct SharedMemoryLayout {
    // Texture handles (NT handles)
    uint64_t sharedHandles[8];  // HANDLE to ID3D11Texture2D

    // Synchronization
    uint64_t fenceShareHandle;  // HANDLE to ID3D11Fence

    // Frame metadata
    uint32_t width, height;
    uint32_t format;  // DXGI_FORMAT

    // Frame ring buffer
    FrameSlot frames[16];
};

struct FrameSlot {
    uint64_t fenceValue;  // Value to wait on
    int64_t timestamp;    // QPC ticks
    uint32_t frameIndex;
    int32_t textureIndex; // Which shared texture (0-7)
    uint32_t sourcePid;
    std::atomic<uint32_t> valid{0};
};
```

---

## Testing Strategy

### 1. Unit Tests per API
- **DX11**: Verify shared texture opens correctly in encoder
- **DX12**: Verify D3D11 textures created on same adapter
- **Vulkan**: Verify D3D11_TEXTURE export works

### 2. Integration Tests
- Test full pipeline: Hook → Shared Memory → Encoder → Output File
- Verify no CPU memory spikes during capture
- Verify GPU performance (no unexpected stalls)

### 3. Compatibility Tests
- NVIDIA GPUs (NVENC)
- AMD GPUs (AMF)
- Intel iGPU (QuickSync)
- Multi-GPU systems

### 4. Performance Benchmarks
- Measure copy times (DXGICopy vs zero-copy)
- Measure encode latency
- Measure frame pacing

---

## Performance Expectations

### Before (Current DX12 Path)
```
Present
  ↓ (~0.5ms) D3D12→D3D12 copy
  ↓ (~1.0ms) D3D12→D3D11 copy (D3D11On12)
  ↓ (~2.0ms) BGRA→NV12 conversion
  ↓ (~3.0ms) Encode
= ~6.5ms total latency
```

### After (Zero-Copy DX12 Path)
```
Present
  ↓ (~0.5ms) DX12→D3D11 copy (optimized)
  ↓ (~2.0ms) BGRA→NV12 conversion
  ↓ (~3.0ms) Encode
= ~5.5ms total latency
```

**Expected improvement**: 1-2ms latency reduction, no CPU memory usage

---

## Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|-----------|
| D3D11ON12 requirement | High | High | Create D3D11 on same adapter, avoid D3D11ON12 |
| Multi-GPU issues | Medium | High | Ensure devices on same adapter via LUID |
| Fence compatibility | Low | Medium | Fallback to keyed mutex |
| Performance regression | Low | High | Benchmark before/after |
| Vulkan driver issues | Medium | Medium | Test on multiple vendors |

---

## Migration Path

### Phase 1: Vulkan Layer (1-2 days)
1. Change external memory type priority
2. Test with existing encoder
3. Verify no regression

### Phase 2: DX11 Optimization (1 day)
1. Audit current implementation
2. Optional: Add fence-based sync

### Phase 3: DX12 Refactor (3-5 days)
1. Create D3D11 device on same adapter
2. Replace D3D12 textures with D3D11 textures
3. Implement optimized copy path
4. Test extensively

### Phase 4: Testing & Validation (2-3 days)
1. Performance benchmarking
2. Multi-API testing
3. Multi-GPU testing

**Total Estimated Time**: 7-11 days

---

## API-Specific Implementation Details

### DX11: Key Changes
**File**: `hook/apis/dx11_hook.cpp`

```cpp
// Change line ~700-750 (DX11Capture class)
class DX11Capture : public HookCaptureBase {
public:
    ID3D11Texture2D *sharedTextures[CAPTURE_TEXTURE_COUNT]{};
    ID3D11Fence* sharedFence = nullptr;  // NEW: Use fence instead of queries

    // REMOVE: ID3D11Query *copyQueries[CAPTURE_TEXTURE_COUNT];
    // REMOVE: IDXGIKeyedMutex *keyedMutexes[CAPTURE_TEXTURE_COUNT]{};
```

### DX12: Key Changes
**File**: `hook/apis/dx12_hook.cpp`

```cpp
// Change line ~308-350 (DX12Capture class)
class DX12Capture : public HookCaptureBase {
public:
    // REMOVE: ID3D12Resource* d3d12Textures[CAPTURE_TEXTURE_COUNT];

    // D3D11 textures for FFmpeg D3D11VA compatibility
    ID3D11Texture2D* sharedTextures[CAPTURE_TEXTURE_COUNT]{};
    ID3D11Device* d3d11Device = nullptr;
    ID3D11DeviceContext* d3d11Context = nullptr;
    ID3D11Fence* d3d11Fence = nullptr;

    // DX12 fence for sync
    ID3D12Fence* dx12Fence = nullptr;
    UINT64 dx12FenceValue = 0;
```

### Vulkan: Key Changes
**File**: `hook/vulkan_layer/layer_capture.cpp`

```cpp
// Change line ~64-68
static const ExternalMemoryTypeInfo g_ExternalMemoryTypes[] = {
    // Force D3D11_TEXTURE for FFmpeg D3D11VA compatibility
    { VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, "D3D11_TEXTURE (primary)" },
    { VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT, "D3D11_TEXTURE_KMT (fallback)" }
};
```

---

## Summary

This plan achieves zero-copy video capture by ensuring all APIs (DX11, DX12, Vulkan) export **D3D11-compatible shared textures** that FFmpeg's D3D11VA can directly import. The key insight is that while DX12 and Vulkan are more modern, the media encoder's D3D11 video processor requires D3D11 resources.

**Key Design Decisions**:
1. **No D3D11On12 textures**: Create D3D11 device on same adapter instead
2. **D3D11 textures for all APIs**: Universal compatibility with FFmpeg D3D11VA
3. **Fence-based synchronization**: Modern, efficient GPU sync
4. **Vulkan D3D11_TEXTURE export**: Ensure D3D11 compatibility

**Expected Outcome**:
- Zero CPU memory usage for capture
- Minimal GPU copies (one optimized copy per frame)
- Universal API support (DX11/DX12/Vulkan)
- FFmpeg hardware acceleration (NVENC/AMF/QuickSync)
