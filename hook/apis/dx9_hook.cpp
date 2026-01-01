#include "dx9_hook.h"
#include "lod_helper.h"
#include "../../common/frame_timing.h"
#include "../common/capture_base.h"
#include "../common/fps_limiter.h"
#include "../common/overlay.h"
#include "hook_common.h"
#include "performance_metrics.h"
#include <MinHook.h>
#include <backends/imgui_impl_dx9.h>
#include <backends/imgui_impl_win32.h>
#include <cstdint>
#include <cstdio>
#include <d3d9.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <imgui.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <thread>

// Function pointer typedefs for hooked functions
typedef HRESULT(STDMETHODCALLTYPE *Present_t)(IDirect3DDevice9*, CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*);
typedef HRESULT(STDMETHODCALLTYPE *PresentEx_t)(IDirect3DDevice9Ex*, CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*, DWORD);
typedef HRESULT(STDMETHODCALLTYPE *PresentSwap_t)(IDirect3DSwapChain9*, CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*, DWORD);
typedef HRESULT(STDMETHODCALLTYPE *Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE *ResetEx_t)(IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);
typedef HRESULT(WINAPI *Direct3DCreate9Ex_t)(UINT, IDirect3D9Ex**);
typedef HRESULT(STDMETHODCALLTYPE *SetSamplerState_t)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);

// Original function pointers
static Present_t oPresent = nullptr;
static PresentEx_t oPresentEx = nullptr;
static PresentSwap_t oPresentSwap = nullptr;
static Reset_t oReset = nullptr;
static ResetEx_t oResetEx = nullptr;
static SetSamplerState_t oSetSamplerState = nullptr;

// Globals
static PerformanceMetrics g_PerfMetrics;
static bool g_ImGuiInitialized = false;
static HWND g_CachedHwnd = NULL;
static bool g_HooksInitialized = false;
static bool g_ResetHooksInstalled = false;
static std::mutex g_PresentMutex;
static thread_local int g_PresentRecurse = 0;  // Prevent recursive Present calls on same thread
static HRESULT STDMETHODCALLTYPE DetourPresent(IDirect3DDevice9* device, const RECT* pSourceRect, const RECT* pDestRect, HWND hDestWindowOverride, const RGNDATA* pDirtyRegion);

// D3D9 format to DXGI format conversion
static DXGI_FORMAT D3D9ToDXGIFormat(D3DFORMAT format) {
    switch (format) {
        case D3DFMT_A8R8G8B8:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case D3DFMT_X8R8G8B8:
            return DXGI_FORMAT_B8G8R8X8_UNORM;
        case D3DFMT_A2B10G10R10:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

static D3DMULTISAMPLE_TYPE ParseD3D9MSAA(const char* msaa) {
    if (strcmp(msaa, "2x") == 0) return D3DMULTISAMPLE_2_SAMPLES;
    if (strcmp(msaa, "4x") == 0) return D3DMULTISAMPLE_4_SAMPLES;
    if (strcmp(msaa, "8x") == 0) return D3DMULTISAMPLE_8_SAMPLES;
    return D3DMULTISAMPLE_NONE;
}

static void ApplyMSAAOverride(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE deviceType, D3DPRESENT_PARAMETERS* pp) {
    if (!pp) return;
    
    const auto& gfx = GetActiveGraphicsConfig();
    const char* msaa = gfx.msaaSamples.c_str();
    if (msaa[0] == 'd') return; // default
    
    D3DMULTISAMPLE_TYPE msType = ParseD3D9MSAA(msaa);
    if (msType != D3DMULTISAMPLE_NONE) {
        DWORD quality;
        if (SUCCEEDED(d3d->CheckDeviceMultiSampleType(adapter, deviceType, pp->BackBufferFormat, pp->Windowed, msType, &quality))) {
            pp->MultiSampleType = msType;
            pp->MultiSampleQuality = 0; 
            pp->SwapEffect = D3DSWAPEFFECT_DISCARD; // Must be DISCARD
            HookLog("DX9: Forcing MSAA %d samples", (int)msType);
        } else {
            HookLog("DX9: MSAA %d samples NOT SUPPORTED for this format/device", (int)msType);
        }
    } else if (strcmp(msaa, "off") == 0) {
        pp->MultiSampleType = D3DMULTISAMPLE_NONE;
        pp->MultiSampleQuality = 0;
        HookLog("DX9: Forcing MSAA OFF");
    }
}


// Proactive apply in Present
static void ApplySGSSAAProactive(IDirect3DDevice9* device) {
     if (!g_IPC || !g_IPC->GetSharedMem() || !g_IPC->GetSharedMem()->graphicsConfig.sgssaa) return;
     
     float bias = 0.0f;
     const auto& gfx = GetActiveGraphicsConfig();
     if (GetSGSSAABias(gfx.sgssaa, gfx.msaaSamples.c_str(), bias)) {
         DWORD dwBias = *((DWORD*)&bias);
         for (DWORD i = 0; i < 8; i++) { 
             oSetSamplerState(device, i, D3DSAMP_MIPMAPLODBIAS, dwBias);
         }
     }
}

// ============================================================================
// D3D9 Runtime Patching (OBS-style) for Zero-Copy on Legacy Devices
// ============================================================================

// Patch data - These are the bytes to write to force shared texture creation
static const BYTE g_ForceJump[] = {0xEB};      // Unconditional short jump
static const BYTE g_IgnoreJump[] = {0x90, 0x90}; // Two NOPs

#define MAX_D3D9_PATCH_SIZE 2
#define D3D9_CMP_SIZE 12

// Number of known D3D9 versions (x86)
#define NUM_D3D9_VERSIONS 20

// Patch offsets for x86 d3d9.dll (32-bit) - expanded for Windows 10/11
static const uintptr_t g_D3D9PatchOffset[NUM_D3D9_VERSIONS] = {
    0x79AA6,  //win7   - 6.1.7601.16562
    0x79C9E,  //win7   - 6.1.7600.16385
    0x79D96,  //win7   - 6.1.7601.17514
    0x7F9BD,  //win8.1 - 6.3.9431.00000
    0x8A3F4,  //win8.1 - 6.3.9600.16404
    0x8B15F,  //win10  - 10.0.10240.16384
    0x8B19F,  //win10  - 10.0.10162.0
    0x8B83F,  //win10  - 10.0.10240.16412
    0x8E9F7,  //win8.1 - 6.3.9600.17095
    0x8F00F,  //win8.1 - 6.3.9600.17085
    0x8FBB1,  //win8.1 - 6.3.9600.16384
    0x90264,  //win8.1 - 6.3.9600.17415
    0x90C3A,  //win10  - 10.0.10586.494
    0x90C57,  //win10  - 10.0.10586.0
    0x96673,  //win10  - 10.0.14393.0
    0x166A08, //win8   - 6.2.9200.16384
    // Windows 10 1803+ / Windows 11 - match at known pattern, use delta to target JE
    0x7A000,  //win11  - 10.0.26200+ (Windows 11 25H2) - pattern here, JE at +6
    0x7A004,  //win11  - alternate (+4)
    0x79FFC,  //win11  - alternate (-4)
    0x7A002,  //win11  - alternate (+2)
};

// Byte patterns to match for each version
static const uint8_t g_D3D9PatchCmp[NUM_D3D9_VERSIONS][D3D9_CMP_SIZE] = {
    {0x8b, 0x89, 0xe8, 0x29, 0x00, 0x00, 0x39, 0xb9, 0x80, 0x4b, 0x00, 0x00},
    {0x8b, 0x89, 0xe8, 0x29, 0x00, 0x00, 0x39, 0xb9, 0x80, 0x4b, 0x00, 0x00},
    {0x8b, 0x89, 0xe8, 0x29, 0x00, 0x00, 0x39, 0xb9, 0x80, 0x4b, 0x00, 0x00},
    {0x8b, 0x80, 0xe8, 0x29, 0x00, 0x00, 0x39, 0xb0, 0x40, 0x4c, 0x00, 0x00},
    {0x80, 0xe8, 0x29, 0x00, 0x00, 0x83, 0xb8, 0x40, 0x4c, 0x00, 0x00, 0x00},
    {0x81, 0xe8, 0x29, 0x00, 0x00, 0x83, 0xb8, 0xa0, 0x4c, 0x00, 0x00, 0x00},
    {0x81, 0xe8, 0x29, 0x00, 0x00, 0x83, 0xb8, 0xa0, 0x4c, 0x00, 0x00, 0x00},
    {0x81, 0xe8, 0x29, 0x00, 0x00, 0x83, 0xb8, 0xa0, 0x4c, 0x00, 0x00, 0x00},
    {0x80, 0xe8, 0x29, 0x00, 0x00, 0x83, 0xb8, 0x40, 0x4c, 0x00, 0x00, 0x00},
    {0x80, 0xe8, 0x29, 0x00, 0x00, 0x83, 0xb8, 0x40, 0x4c, 0x00, 0x00, 0x00},
    {0x80, 0xe8, 0x29, 0x00, 0x00, 0x83, 0xb8, 0x40, 0x4c, 0x00, 0x00, 0x00},
    {0x87, 0xe8, 0x29, 0x00, 0x00, 0x83, 0xb8, 0x40, 0x4c, 0x00, 0x00, 0x00},
    {0x81, 0x18, 0x2a, 0x00, 0x00, 0x83, 0xb8, 0xa0, 0x4c, 0x00, 0x00, 0x00},
    {0x81, 0x18, 0x2a, 0x00, 0x00, 0x83, 0xb8, 0xa0, 0x4c, 0x00, 0x00, 0x00},
    {0x81, 0x18, 0x2a, 0x00, 0x00, 0x83, 0xb8, 0xa8, 0x4c, 0x00, 0x00, 0x00},
    {0x8b, 0x80, 0xe8, 0x29, 0x00, 0x00, 0x39, 0x90, 0xb0, 0x4b, 0x00, 0x00},
    // Windows 10 1803+ / Windows 11 - discovered from user's d3d9.dll (Win11 25H2)
    // 0x79FFA: ?? ?? ?? ?? ?? ?? 70 02 00 00 84 C0 (ends at 0x7A006 where JE is)
    // Need to match bytes from 0x79FFA-0x7A005 so patch lands on JE at 0x7A006
    // Using 0x7A000 - 6 bytes = can't see those bytes, but we know 70 02 00 00 84 C0 74 13
    // Actually patch at the CHECK OFFSET itself - the CMP, not the JE
    // Let's try matching from 0x7A000 and apply patch with offset adjustment
    {0x70, 0x02, 0x00, 0x00, 0x84, 0xc0, 0x74, 0x13, 0x8b, 0x87, 0x3c, 0x2b}, // 0x7A000 bytes
    {0x84, 0xc0, 0x74, 0x13, 0x8b, 0x87, 0x3c, 0x2b, 0x00, 0x00, 0x83, 0xb8}, // 0x7A004 guess
    {0x00, 0x00, 0x84, 0xc0, 0x74, 0x13, 0x8b, 0x87, 0x3c, 0x2b, 0x00, 0x00}, // 0x79FFE guess
    {0x02, 0x00, 0x00, 0x84, 0xc0, 0x74, 0x13, 0x8b, 0x87, 0x3c, 0x2b, 0x00}, // 0x79FFF guess
};

// Patch sizes for each version (1 = force_jump, 2 = ignore_jump)
static const size_t g_D3D9PatchSize[NUM_D3D9_VERSIONS] = {
    1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 1, 2, 2, 2, 2
};

// Patch offset delta - how many bytes to adjust the patch address from offset+CMP_SIZE
// For Win11 patterns: JE is at offset+6, not offset+12, so delta = +6 - 12 = -6
static const int g_D3D9PatchDelta[NUM_D3D9_VERSIONS] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    -6, -6, -6, -6  // Win11 patterns: JE is 6 bytes into the pattern
};

// Global patch state
static HMODULE g_D3D9Module = nullptr;
static int g_D3D9PatchIndex = -1;

// Safe memcmp - check memory is readable before comparing (no SEH for MinGW)
static int SafeMemCmp(const void *p1, const void *p2, size_t size) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p1, &mbi, sizeof(mbi)) == 0)
        return -1;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return -1;
    if ((mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) == 0)
        return -1;
    return memcmp(p1, p2, size);
}

// Diagnostic: Scan d3d9.dll for potential patch locations
static void ScanD3D9ForPatchCandidates(HMODULE d3d9) {
    uint8_t *base = (uint8_t *)d3d9;
    
    // Get module size from PE header
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        EarlyLog("DX9: Invalid DOS header");
        return;
    }
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        EarlyLog("DX9: Invalid NT header");
        return;
    }
    
    DWORD moduleSize = nt->OptionalHeader.SizeOfImage;
    EarlyLog("DX9: d3d9.dll module size: 0x%X (%d KB)", moduleSize, moduleSize / 1024);
    
    // Scan at various offsets within the module
    // Focus on the typical range where the D3D9Ex check is located
    EarlyLog("DX9: Scanning for patch candidates...");
    
    // Scan from 0x50000 to min(moduleSize, 0x200000) in 0x10000 increments
    for (uintptr_t offset = 0x50000; offset < moduleSize && offset < 0x200000; offset += 0x10000) {
        uint8_t *addr = base + offset;
        
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
            continue;
        if ((mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_READONLY | PAGE_READWRITE)) == 0)
            continue;
        
        EarlyLog("DX9: 0x%05X: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
            (unsigned)offset,
            addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], addr[6], addr[7],
            addr[8], addr[9], addr[10], addr[11], addr[12], addr[13], addr[14], addr[15]);
    }
    
    // Also scan specifically around known Windows 10/11 offsets with finer granularity
    const uintptr_t knownRanges[] = { 0x70000, 0x75000, 0x78000, 0x7A000, 0x7C000, 0x7E000, 0x80000 };
    for (size_t i = 0; i < sizeof(knownRanges)/sizeof(knownRanges[0]); i++) {
        uintptr_t offset = knownRanges[i];
        if (offset >= moduleSize) continue;
        
        uint8_t *addr = base + offset;
        EarlyLog("DX9: 0x%05X: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
            (unsigned)offset,
            addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], addr[6], addr[7],
            addr[8], addr[9], addr[10], addr[11], addr[12], addr[13], addr[14], addr[15]);
    }
}

// Find the patch index for the current d3d9.dll version
static int FindD3D9Patch(HMODULE d3d9) {
    uint8_t *addr = (uint8_t *)d3d9;
    
    // First try known patterns
    for (int i = 0; i < NUM_D3D9_VERSIONS; i++) {
        // Skip placeholder patterns
        bool isPlaceholder = true;
        for (int j = 0; j < D3D9_CMP_SIZE; j++) {
            if (g_D3D9PatchCmp[i][j] != 0) {
                isPlaceholder = false;
                break;
            }
        }
        if (isPlaceholder)
            continue;
            
        int ret = SafeMemCmp(addr + g_D3D9PatchOffset[i], g_D3D9PatchCmp[i], D3D9_CMP_SIZE);
        if (ret == 0) {
            EarlyLog("DX9: Found D3D9 patch version %d at offset 0x%X", i, (unsigned)g_D3D9PatchOffset[i]);
            return i;
        }
    }
    
    // If no match, run diagnostic scan
    ScanD3D9ForPatchCandidates(d3d9);
    
    return -1;
}

// Get the address to patch
static uint8_t* GetD3D9PatchAddr(HMODULE d3d9, int patchIndex) {
    if (patchIndex < 0 || patchIndex >= NUM_D3D9_VERSIONS)
        return nullptr;
    uint8_t *addr = (uint8_t *)d3d9;
    // Apply delta for Win11 patterns where JE is not at offset+CMP_SIZE
    return addr + g_D3D9PatchOffset[patchIndex] + D3D9_CMP_SIZE + g_D3D9PatchDelta[patchIndex];
}



// DX9 Capture class with D3D11 interop
class DX9Capture : public HookCaptureBase {
public:
    // Capture State
    bool firstFrame = true;
    bool initializationFailed = false; // Prevent endless retries if HW really fails
    
    DX9Capture() {
        CaptureBase::initialized = false;
        initializationFailed = false;
        firstFrame = true;
    }
    
    // D3D9 resources
    IDirect3DDevice9 *d3d9Device = nullptr;
    IDirect3DDevice9Ex *d3d9DeviceEx = nullptr; // Interface to Ex device if avail
    IDirect3DTexture9 *sharedTexture9 = nullptr; // The shared texture resource
    IDirect3DSurface9 *copySurface = nullptr;  // Surface level 0 of sharedTexture9
    
    HANDLE sharedHandle9 = NULL;               // Handle for D3D11 interop
    D3DFORMAT d3d9Format = D3DFMT_UNKNOWN;
    HRESULT hr = S_OK;
    
    // D3D11 resources for sharing
    ID3D11Device *d3d11Device = nullptr;
    ID3D11DeviceContext *d3d11Context = nullptr;
    ID3D11Texture2D *d3d11SharedTexture = nullptr; // The texture opened in D3D11
    IDirect3DTexture9 *overlayTexture9 = nullptr;
    
    ID3D11Texture2D *sharedTextures[CAPTURE_TEXTURE_COUNT]{};
    HANDLE sharedTextureHandles[CAPTURE_TEXTURE_COUNT] = { NULL };
    HANDLE sharedFenceHandle = NULL;
    
    // D3D11.3 Fence support
    ID3D11Fence *fence = nullptr;
    ID3D11DeviceContext4 *context4 = nullptr;
    bool useFences = false;
    UINT64 fenceValue = 0;
    
    // Shmem fallback for legacy D3D9 (when patching fails)
    bool useShmem = false;
    IDirect3DSurface9 *shmemSurfaces[CAPTURE_TEXTURE_COUNT] = { nullptr };
    IDirect3DQuery9 *shmemQueries[CAPTURE_TEXTURE_COUNT] = { nullptr };
    bool shmemTextureReady[CAPTURE_TEXTURE_COUNT] = { false };
    uint32_t shmemPitch = 0;
    int shmemCurTex = 0;
    int shmemCopyWait = 0;
    
    // CPU Prerender Limit
    struct QuerySlot {
        IDirect3DQuery9* query = nullptr;
    };
    std::vector<QuerySlot> prerenderQueries;
    uint32_t prerenderIdx = 0;
    
    void Cleanup() override {
        CleanupDX9();
    }
    
    void CleanupDX9(bool permanentFailure = false) {
        // Release texture handles
        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
            if (sharedTextureHandles[i]) {
                CloseHandle(sharedTextureHandles[i]);
                sharedTextureHandles[i] = NULL;
            }
            if (sharedTextures[i]) {
                sharedTextures[i]->Release();
                sharedTextures[i] = nullptr;
            }
        }
        
        if (fence) { fence->Release(); fence = nullptr; }
        if (context4) { context4->Release(); context4 = nullptr; }
        if (sharedFenceHandle) {
            CloseHandle(sharedFenceHandle);
            sharedFenceHandle = NULL;
        }
        
        if (copySurface) { copySurface->Release(); copySurface = nullptr; }
        if (sharedTexture9) { sharedTexture9->Release(); sharedTexture9 = nullptr; }
        sharedHandle9 = NULL;
        
        if (d3d11SharedTexture) { d3d11SharedTexture->Release(); d3d11SharedTexture = nullptr; }
        if (d3d11Context) { d3d11Context->Release(); d3d11Context = nullptr; }
        if (d3d11Device) { d3d11Device->Release(); d3d11Device = nullptr; }
        if (d3d9DeviceEx) { d3d9DeviceEx->Release(); d3d9DeviceEx = nullptr; }
        if (overlayTexture9) { overlayTexture9->Release(); overlayTexture9 = nullptr; }
        
        if (d3d9Device) {
            d3d9Device->Release();
            d3d9Device = nullptr;
        }
        
        d3d9Format = D3DFMT_UNKNOWN;
        initialized = false;
        useFences = false;
        fenceValue = 0;
        firstFrame = true;
        
        // Cleanup shmem resources
        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
            if (shmemSurfaces[i]) { shmemSurfaces[i]->Release(); shmemSurfaces[i] = nullptr; }
            if (shmemQueries[i]) { shmemQueries[i]->Release(); shmemQueries[i] = nullptr; }
            shmemTextureReady[i] = false;
        }
        
        for (auto& q : prerenderQueries) {
            if (q.query) q.query->Release();
        }
        prerenderQueries.clear();
        prerenderIdx = 0;
        
        useShmem = false;
        shmemPitch = 0;
        shmemCurTex = 0;
        shmemCopyWait = 0;
        
        if (permanentFailure) {
            initializationFailed = true;
        } else {
            initializationFailed = false; // Allow retry if it wasn't a permanent fail
        }
    }
    
    void CreateSharedResources(uint32_t w, uint32_t h, uint32_t fmt) override {
        // Implemented in Init
    }
    
    bool CreateD3D11Device() {
        // Find the adapter matching the D3D9 device
        HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
        if (!hDXGI) hDXGI = LoadLibraryA("dxgi.dll");
        if (!hDXGI) {
            EarlyLog("DX9: DXGI DLL not found");
            return false;
        }

        typedef HRESULT (WINAPI *PFN_CREATE_DXGI_FACTORY1)(REFIID, void**);
        PFN_CREATE_DXGI_FACTORY1 pCreateDXGIFactory1 = (PFN_CREATE_DXGI_FACTORY1)GetProcAddress(hDXGI, "CreateDXGIFactory1");
        if (!pCreateDXGIFactory1) {
             EarlyLog("DX9: CreateDXGIFactory1 not found");
             return false;
        }

        IDXGIFactory1 *factory = nullptr;
        HRESULT hr = pCreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) {
            EarlyLog("DX9: Failed to create DXGI factory");
            return false;
        }
        
        // Get the adapter for this D3D9 device
        IDirect3D9 *d3d9 = nullptr;
        d3d9Device->GetDirect3D(&d3d9);
        
        // Get adapter identifier
        D3DADAPTER_IDENTIFIER9 adapterIdent;
        d3d9->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &adapterIdent);
        d3d9->Release();
        
        // Find matching DXGI adapter
        IDXGIAdapter1 *adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            
            // Store LUID
            luidLow = desc.AdapterLuid.LowPart;
            luidHigh = desc.AdapterLuid.HighPart;
            break; // Use first adapter for now
        }
        factory->Release();
        
        if (!adapter) {
            EarlyLog("DX9: No DXGI adapter found");
            return false;
        }
        
        // Create D3D11 device
        HMODULE hD3D11 = GetModuleHandleA("d3d11.dll");
        if (!hD3D11) hD3D11 = LoadLibraryA("d3d11.dll");
        if (!hD3D11) {
            EarlyLog("DX9: D3D11 DLL not found");
            adapter->Release();
            return false;
        }

        typedef HRESULT (WINAPI *PFN_D3D11_CREATE_DEVICE)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
        PFN_D3D11_CREATE_DEVICE pD3D11CreateDevice = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(hD3D11, "D3D11CreateDevice");
        if (!pD3D11CreateDevice) {
             EarlyLog("DX9: D3D11CreateDevice not found");
             adapter->Release();
             return false;
        }

        D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
        D3D_FEATURE_LEVEL featureLevel;
        
        hr = pD3D11CreateDevice(
            adapter,
            D3D_DRIVER_TYPE_UNKNOWN,
            NULL,
            0,
            featureLevels,
            3,
            D3D11_SDK_VERSION,
            &d3d11Device,
            &featureLevel,
            &d3d11Context
        );
        adapter->Release();
        
        if (FAILED(hr)) {
            EarlyLog("DX9: Failed to create D3D11 device (hr=0x%08x)", hr);
            return false;
        }
        
        EarlyLog("DX9: Created D3D11 device (feature level %d)", featureLevel);
        return true;
    }
    
    void Init(IDirect3DDevice9 *device) {
        EarlyLog("DX9: DX9Capture::Init() entering. initialized=%d, failed=%d", initialized, initializationFailed);
        if (initialized || initializationFailed)
            return;
            
        EarlyLog("DX9: Init Step 1: AddRef device");
        device->AddRef();
        d3d9Device = device;
        
        EarlyLog("DX9: Init Step 2: GetRenderTarget");
        IDirect3DSurface9 *backBuffer = nullptr;
        if (FAILED(device->GetRenderTarget(0, &backBuffer))) {
            EarlyLog("DX9: Failed to get render target");
            CleanupDX9(true);
            return;
        }
        
        EarlyLog("DX9: Init Step 3: GetDesc");
        D3DSURFACE_DESC desc;
        backBuffer->GetDesc(&desc);
        backBuffer->Release();
        
        width = desc.Width;
        height = desc.Height;
        d3d9Format = desc.Format;
        format = D3D9ToDXGIFormat(desc.Format);
        EarlyLog("DX9: Init Step 4: Format check. w=%d, h=%d, fmt=%d", width, height, d3d9Format);
        
        if (format == DXGI_FORMAT_UNKNOWN) {
            EarlyLog("DX9: Unsupported format %d", desc.Format);
            CleanupDX9(true);
            return;
        }
        
        EarlyLog("DX9: Init Step 5: CreateD3D11Device");
        if (!CreateD3D11Device()) {
            EarlyLog("DX9: CreateD3D11Device failed");
            CleanupDX9(true);
            return;
        }
        
        EarlyLog("DX9: Init Step 6: Check D3D9Ex support");
        bool isD3D9Ex = false;
        if (SUCCEEDED(device->QueryInterface(__uuidof(IDirect3DDevice9Ex), (void**)&d3d9DeviceEx))) {
            EarlyLog("DX9: Device supports D3D9Ex natively");
            isD3D9Ex = true;
        } else {
            EarlyLog("DX9: Device is legacy D3D9, will attempt runtime patching");
            d3d9DeviceEx = nullptr;
        }
        
        EarlyLog("DX9: Init Step 7: Create DX9 Shared Texture");
        sharedHandle9 = NULL;
        
        // If legacy D3D9, we need to patch the runtime to force shared handle creation
        if (!isD3D9Ex) {
            g_D3D9Module = GetModuleHandleA("d3d9.dll");
            if (g_D3D9Module) {
                g_D3D9PatchIndex = FindD3D9Patch(g_D3D9Module);
                if (g_D3D9PatchIndex >= 0) {
                    EarlyLog("DX9: Applying runtime patch (version %d)...", g_D3D9PatchIndex);
                    
                    uint8_t *patchAddr = GetD3D9PatchAddr(g_D3D9Module, g_D3D9PatchIndex);
                    size_t patchSize = g_D3D9PatchSize[g_D3D9PatchIndex];
                    uint8_t savedData[MAX_D3D9_PATCH_SIZE];
                    DWORD oldProtect;
                    
                    // Apply patch
                    VirtualProtect(patchAddr, patchSize, PAGE_EXECUTE_READWRITE, &oldProtect);
                    memcpy(savedData, patchAddr, patchSize);
                    if (patchSize == 1) {
                        memcpy(patchAddr, g_ForceJump, 1);
                    } else {
                        memcpy(patchAddr, g_IgnoreJump, 2);
                    }
                    
                    // Create texture with patch applied
                    hr = device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, d3d9Format, D3DPOOL_DEFAULT, &sharedTexture9, &sharedHandle9);
                    
                    // Restore original bytes
                    memcpy(patchAddr, savedData, patchSize);
                    VirtualProtect(patchAddr, patchSize, oldProtect, &oldProtect);
                    
                    EarlyLog("DX9: Patch restored. CreateTexture hr=0x%08x, handle=%p", hr, sharedHandle9);
                } else {
                    EarlyLog("DX9: No matching D3D9 patch found for this Windows version");
                    // Try anyway without patching (will likely fail)
                    hr = device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, d3d9Format, D3DPOOL_DEFAULT, &sharedTexture9, &sharedHandle9);
                }
            } else {
                EarlyLog("DX9: d3d9.dll not found");
                hr = E_FAIL;
            }
        } else {
            // D3D9Ex device - shared handles work natively
            hr = device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, d3d9Format, D3DPOOL_DEFAULT, &sharedTexture9, &sharedHandle9);
        }
        
        if (FAILED(hr) || !sharedTexture9 || !sharedHandle9) {
            EarlyLog("DX9: Shared texture failed (hr=0x%08x), falling back to shmem capture", hr);
            
            // Fallback to shmem capture
            if (sharedTexture9) { sharedTexture9->Release(); sharedTexture9 = nullptr; }
            sharedHandle9 = NULL;
            
            // Create offscreen surfaces in system memory
            bool shmemOk = true;
            for (int i = 0; i < CAPTURE_TEXTURE_COUNT && shmemOk; i++) {
                hr = device->CreateOffscreenPlainSurface(width, height, d3d9Format, D3DPOOL_SYSTEMMEM, &shmemSurfaces[i], nullptr);
                if (FAILED(hr)) {
                    EarlyLog("DX9: Failed to create shmem surface %d (hr=0x%08x)", i, hr);
                    shmemOk = false;
                } else {
                    // Get pitch from first surface
                    if (i == 0) {
                        D3DLOCKED_RECT rect;
                        if (SUCCEEDED(shmemSurfaces[i]->LockRect(&rect, nullptr, D3DLOCK_READONLY))) {
                            shmemPitch = rect.Pitch;
                            shmemSurfaces[i]->UnlockRect();
                        }
                    }
                    // Create event query for sync
                    hr = device->CreateQuery(D3DQUERYTYPE_EVENT, &shmemQueries[i]);
                    if (FAILED(hr)) {
                        EarlyLog("DX9: Failed to create shmem query %d (hr=0x%08x)", i, hr);
                        shmemOk = false;
                    }
                }
            }
            
            if (!shmemOk) {
                CleanupDX9(true);
                return;
            }
            
            useShmem = true;
            EarlyLog("DX9: Shmem capture initialized (pitch=%d)", shmemPitch);
            
            // Skip D3D11 interop steps for shmem path - go directly to success
            if (g_IPC) {
                // For shmem, we don't use shared textures, but we still need to signal frames
                // We'll directly copy to IPC shared memory in CaptureFrame
                if (g_IPC->GetSharedMem()) {
                    g_IPC->GetSharedMem()->width = width;
                    g_IPC->GetSharedMem()->height = height;
                    g_IPC->GetSharedMem()->format = 87; // DXGI_FORMAT_B8G8R8A8_UNORM
                }
                format = D3D9ToDXGIFormat(d3d9Format);
            }
            CaptureBase::initialized = true;
            HookLog("DX9 Capture Initialized (SHMEM): %dx%d", width, height);
            return;  // Done with shmem path
        }


        
        EarlyLog("DX9: Init Step 8: GetSurfaceLevel");
        hr = sharedTexture9->GetSurfaceLevel(0, &copySurface);
        if (FAILED(hr)) {
             EarlyLog("DX9: GetSurfaceLevel failed");
             CleanupDX9(true); 
             return;
        }
        
        EarlyLog("DX9: Init Step 9: OpenSharedResource in D3D11");
        if (d3d11Device) {
             hr = d3d11Device->OpenSharedResource(sharedHandle9, __uuidof(ID3D11Texture2D), (void**)&d3d11SharedTexture);
             if (FAILED(hr)) {
                  EarlyLog("DX9: Failed to open shared resource in D3D11 (hr=0x%08x)", hr);
                  CleanupDX9(true); 
                  return;
             }
        }
        
        EarlyLog("DX9: Init Step 10: Create Ring Buffer Shared Textures");
        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = (DXGI_FORMAT)format;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        
        // Try to enable fences
        ID3D11Device5 *device5 = nullptr;
        if (SUCCEEDED(d3d11Device->QueryInterface(IID_PPV_ARGS(&device5)))) {
            if (SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)))) {
                if (SUCCEEDED(d3d11Context->QueryInterface(IID_PPV_ARGS(&context4)))) {
                    IDXGIResource *res = nullptr;
                    if (SUCCEEDED(fence->QueryInterface(IID_PPV_ARGS(&res)))) {
                        res->GetSharedHandle(&sharedFenceHandle);
                        res->Release();
                        useFences = true;
                        EarlyLog("DX9: ID3D11Fence support enabled");
                    }
                }
            }
            device5->Release();
        }
        
        if (!useFences) {
            EarlyLog("DX9: Fence not available, using synchronous copy");
        }
        
        bool success = true;
        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
            hr = d3d11Device->CreateTexture2D(&texDesc, NULL, &sharedTextures[i]);
            if (SUCCEEDED(hr)) {
                IDXGIResource *pResource = nullptr;
                sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&pResource));
                pResource->GetSharedHandle(&sharedTextureHandles[i]);
                pResource->Release();
            } else {
                success = false;
                EarlyLog("DX9: Failed to create texture %d (hr=0x%08x)", i, hr);
            }
        }
        
        if (success) {
            if (g_IPC) {
                PublishToSharedMemory(g_IPC);
            }
            CaptureBase::initialized = true;
            HookLog("DX9 Capture Initialized: %dx%d (LUID: %08x)", width, height, luidLow);
        } else {
            CleanupDX9();
        }
    }
    
    void CaptureFrame(IDirect3DDevice9 *device, IDirect3DSurface9 *backBuffer) {
        if (!initialized || !backBuffer)
            return;
        
        // Get timestamp
        static int64_t qpcFreq = 0;
        if (qpcFreq == 0) {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            qpcFreq = f.QuadPart;
        }
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;
        
        if (useShmem) {
            // Shmem capture path - Copy to system memory surface then to shared buffer
            // Use current surface index
            int idx = shmemCurTex;
            
            // 1. Copy from GPU Backbuffer to System Memory Surface
            HRESULT hr = device->GetRenderTargetData(backBuffer, shmemSurfaces[idx]);
            if (SUCCEEDED(hr)) {
                // 2. Lock to access pixels
                D3DLOCKED_RECT rect;
                hr = shmemSurfaces[idx]->LockRect(&rect, NULL, D3DLOCK_READONLY);
                if (SUCCEEDED(hr)) {
                    // 3. Copy to Shared Buffer
                    // Use slot 0 or 1 based on idx
                    int slot = idx % 2; // Assuming CAPTURE_TEXTURE_COUNT >= 2
                    if (g_IPC && g_IPC->GetSharedMem()) {
                        auto& shmem = g_IPC->GetSharedMem()->shmem;
                        
                        // Copy parameters
                        uint32_t copyW = width;
                        uint32_t copyH = height;
                        // Avoid buffer overflow
                        if (copyW > SharedMemoryLayout::ShmemBuffer::MAX_WIDTH) copyW = SharedMemoryLayout::ShmemBuffer::MAX_WIDTH;
                        if (copyH > SharedMemoryLayout::ShmemBuffer::MAX_HEIGHT) copyH = SharedMemoryLayout::ShmemBuffer::MAX_HEIGHT;

                        uint8_t* dst = shmem.data[slot];
                        uint8_t* src = (uint8_t*)rect.pBits;
                        uint32_t dstPitch = copyW * 4; // Tight packing
                        
                        // Copy row by row
                        for (uint32_t y = 0; y < copyH; y++) {
                            memcpy(dst + (y * dstPitch), src + (y * rect.Pitch), dstPitch);
                        }
                        
                        // Update metadata
                        shmem.validWidth = copyW;
                        shmem.validHeight = copyH;
                        shmem.pitch = dstPitch;
                        shmem.writeSlot.store(slot);
                        shmem.slotReady[slot].store(true);
                        
                        // 4. Signal Encoder (Index 100+ to indicate shmem slot)
                        // Using 100 + slot as textureIndex
                        SignalFrameReady(g_IPC, 100 + slot, us / 1000, 0); 
                    }
                    shmemSurfaces[idx]->UnlockRect();
                }
            }
            
            // Cycle surface for next frame (double/triple buffering)
            shmemCurTex = (shmemCurTex + 1) % CAPTURE_TEXTURE_COUNT;
        } else {
            // Zero-copy path (original)
            int idx = writeIndex;
            
            HRESULT hr = device->StretchRect(backBuffer, NULL, copySurface, NULL, D3DTEXF_NONE);
            if (FAILED(hr)) {
                return;
            }
            
            if (d3d11Context && d3d11SharedTexture && sharedTextures[idx]) {
                d3d11Context->CopySubresourceRegion(
                    sharedTextures[idx], 0, 0, 0, 0,
                    d3d11SharedTexture, 0, NULL
                );
                
                if (useFences && fence && context4) {
                        SignalFrameReady(g_IPC, idx, us / 1000, fenceValue);
                } else {
                    SignalFrameReady(g_IPC, idx, us / 1000, 0);
                }
                
                AdvanceWriteIndex();
            }
        }
    }

    void WaitPrerender(IDirect3DDevice9* device, float limit) {
        if (limit < 0.0f) return;
        
        // Effective Limit Integer (Queue Size)
        // If limit < 1.0 (Hybrid), we want minimal queue (1 frame ahead)
        uint32_t queueSize = (uint32_t)limit;
        if (limit < 1.0f) queueSize = 1; 
        if (queueSize < 1) queueSize = 1; // Minimum 1

        // Ensure query array is sized correctly
        // We need queueSize + 1 slots for ring buffer logic?
        // No, if limit is 1, we issue query N, and check N-1.
        // So we need limit+1 slots.
        size_t needed = queueSize + 1;
        
        if (prerenderQueries.size() != needed) {
            for (auto& q : prerenderQueries) if (q.query) q.query->Release();
            prerenderQueries.clear();
            prerenderQueries.resize(needed);
            prerenderIdx = 0;
        }
        
        uint32_t oldestIdx = (prerenderIdx + 1) % (uint32_t)prerenderQueries.size();
        if (prerenderQueries[oldestIdx].query) {
            // Wait for oldest event query (Fence)
            HRESULT hr;
            while ((hr = prerenderQueries[oldestIdx].query->GetData(nullptr, 0, D3DGETDATA_FLUSH)) == S_FALSE) {
                // Spin/Yield loop
                // D3DGETDATA_FLUSH forces GPU to make progress
                Sleep(0); 
            }
        }
        
        // Push New Fence
        uint32_t currentIdx = prerenderIdx % (uint32_t)prerenderQueries.size();
        if (!prerenderQueries[currentIdx].query) {
            device->CreateQuery(D3DQUERYTYPE_EVENT, &prerenderQueries[currentIdx].query);
        }
        if (prerenderQueries[currentIdx].query) {
            prerenderQueries[currentIdx].query->Issue(D3DISSUE_END);
        }
        
        // Hybrid Pacing Sleep logic (if limit < 1.0)
        // This runs AFTER fence wait (so we know GPU is within range), 
        // effectively delaying THIS CPU frame submission.
        if (limit > 0.01f && limit < 1.0f) {
             float fps = g_PerfMetrics.GetCurrentFPS();
             double avgFrameTimeUs = (fps > 1.0f) ? (1000000.0 / fps) : 16666.0;
             int64_t sleepUs = (int64_t)(avgFrameTimeUs * (1.0 - limit) * 0.70); // 0.70 Safety Factor
             if (sleepUs > 0) PrecisionSleep(sleepUs);
        }

        prerenderIdx++;
    }
};

static DX9Capture g_DX9Capture;

// Draw overlay using ImGui DX9 backend
static void DrawDX9Overlay(IDirect3DDevice9 *device) {
    if (!g_ImGuiInitialized) {
        // Get the window handle
        D3DDEVICE_CREATION_PARAMETERS params;
        device->GetCreationParameters(&params);
        g_CachedHwnd = params.hFocusWindow;
        
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui_ImplWin32_Init(g_CachedHwnd);
        ImGui_ImplDX9_Init(device);
        g_ImGuiInitialized = true;
        EarlyLog("DX9: ImGui initialized");
    }
    
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    
    // Use shared overlay
    g_SharedOverlay.SetMetrics(&g_PerfMetrics);
    g_SharedOverlay.SetIPCClient(g_IPC);
    g_SharedOverlay.SetHwnd(g_CachedHwnd);
    g_SharedOverlay.SetDroppedFrames(g_DX9Capture.droppedFrames.load(std::memory_order_relaxed));
    g_SharedOverlay.SetGraphicsAPI("DX9");
    g_SharedOverlay.RenderUI();
    
    ImGui::EndFrame();
    ImGui::Render();
    
    if (SUCCEEDED(device->BeginScene())) {
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        device->EndScene();
    }
}

// Present hook helpers
static void PresentBegin(IDirect3DDevice9 *device, IDirect3DSurface9 *&backBuffer) {
    if (g_ShuttingDown) return;
    g_PresentRecurse++;
    if (g_PresentRecurse == 1) {
        std::lock_guard<std::mutex> lock(g_PresentMutex); // Protect against concurrent calls
        ApplySGSSAAProactive(device);
        
        // Get backbuffer
        if (FAILED(device->GetRenderTarget(0, &backBuffer))) {
            backBuffer = nullptr;
        }
        
        // ... (logging every 60 frames) ...
        static int frameCount = 0;
        frameCount++;
        IPCClient* ipc = g_IPC;
        if (frameCount % 60 == 0) {
             SharedMemoryLayout *shm = ipc ? ipc->GetSharedMem() : nullptr;
             EarlyLog("DX9: Present called (Frame %d). IPC=%p, SHM=%p, ShowOverlay=%d, Recording=%d", 
                      frameCount, ipc, shm, 
                      shm ? shm->overlayConfig.showOverlay : -1,
                      ipc ? ipc->IsRecording() : -1);
        }

        // Draw overlay
        SharedMemoryLayout *shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        if (shm && shm->overlayConfig.showOverlay) {
            DrawDX9Overlay(device);
        }
        
        // CPU Prerender Limit
        // CPU Prerender Limit (Hybrid Pacing)
        float limit = GetActivePrerenderLimit();
        if (limit > -0.5f) { // Active if >= 0.0
            g_DX9Capture.WaitPrerender(device, limit);
        }
        
        // Capture logic
        if (ipc && ipc->IsRecording()) {
            if (!g_DX9Capture.initialized) {
                EarlyLog("DX9: Recording detected, calling Init...");
                g_DX9Capture.Init(device);
            }
            
            if (g_DX9Capture.initialized && backBuffer) {
                g_DX9Capture.CaptureFrame(device, backBuffer);
            }
        } else if (g_DX9Capture.initialized) {
            g_DX9Capture.Cleanup();
        }
        
        // Update performance metrics
        static int64_t qpcFreq = 0;
        if (qpcFreq == 0) {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            qpcFreq = f.QuadPart;
        }
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;

        // Initialize CSV logging once - only if debug logging is enabled
        static bool csvLoggingInitialized = false;
        SharedMemoryLayout* csvShm = (ipc) ? ipc->GetSharedMem() : nullptr;
        if (!csvLoggingInitialized && csvShm && csvShm->debugLogging) {
            char modulePath[MAX_PATH];
            HMODULE hModule = NULL;
            // Get module handle from DetourPresent which is static in this file
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)&DetourPresent, &hModule);
            if (hModule) {
                GetModuleFileNameA(hModule, modulePath, MAX_PATH);
                char *lastSlash = strrchr(modulePath, '\\');
                if (lastSlash) {
                    *lastSlash = '\0';
                    strcat(modulePath, "\\logs");
                    CreateDirectoryA(modulePath, NULL);
                    strcat(modulePath, "\\frame_times.csv");
                    g_PerfMetrics.EnableCSVLogging(modulePath);
                    HookLog("DX9: Frame time CSV logging enabled (%s)", modulePath);
                }
            }
            csvLoggingInitialized = true;
        }

        g_PerfMetrics.Update(us);
        
        // Update recording state for CSV logging
        bool isRecording = ipc && ipc->IsRecording();
        g_PerfMetrics.SetRecording(isRecording);
        
        // Apply FPS limiter
        g_SharedFpsLimiter.SetIPCClient(ipc);
        g_SharedFpsLimiter.Apply();
        
        if (backBuffer) {
            backBuffer->Release();
        }
    }
}

static void PresentEnd(IDirect3DDevice9 *device, IDirect3DSurface9 *backBuffer) {
    g_PresentRecurse--;
}

// Hook: IDirect3DDevice9::Present
static HRESULT STDMETHODCALLTYPE DetourPresent(
    IDirect3DDevice9 *device,
    CONST RECT *pSourceRect,
    CONST RECT *pDestRect,
    HWND hDestWindowOverride,
    CONST RGNDATA *pDirtyRegion
) {
    IDirect3DSurface9 *backBuffer = nullptr;
    PresentBegin(device, backBuffer);
    HRESULT hr = oPresent(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
    PresentEnd(device, backBuffer);
    return hr;
}

// Hook: IDirect3DDevice9Ex::PresentEx
static HRESULT STDMETHODCALLTYPE DetourPresentEx(
    IDirect3DDevice9Ex *device,
    CONST RECT *pSourceRect,
    CONST RECT *pDestRect,
    HWND hDestWindowOverride,
    CONST RGNDATA *pDirtyRegion,
    DWORD dwFlags
) {
    IDirect3DSurface9 *backBuffer = nullptr;
    PresentBegin(device, backBuffer);
    HRESULT hr = oPresentEx(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
    PresentEnd(device, backBuffer);
    return hr;
}

// Hook: IDirect3DSwapChain9::Present
static HRESULT STDMETHODCALLTYPE DetourPresentSwap(
    IDirect3DSwapChain9 *swap,
    CONST RECT *pSourceRect,
    CONST RECT *pDestRect,
    HWND hDestWindowOverride,
    CONST RGNDATA *pDirtyRegion,
    DWORD dwFlags
) {
    IDirect3DSurface9 *backBuffer = nullptr;
    IDirect3DDevice9 *device = nullptr;
    
    if (g_PresentRecurse == 0) {
        if (SUCCEEDED(swap->GetDevice(&device))) {
            PresentBegin(device, backBuffer);
        }
    }
    
    HRESULT hr = oPresentSwap(swap, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
    
    if (device) {
        PresentEnd(device, backBuffer);
        device->Release();
    }
    
    return hr;
}

// Hook: IDirect3DDevice9::Reset
static HRESULT STDMETHODCALLTYPE DetourReset(
    IDirect3DDevice9 *device,
    D3DPRESENT_PARAMETERS *pPresentationParameters
) {
    HookLog("DX9: Reset called");
    
    // Cleanup ImGui before reset
    if (g_ImGuiInitialized) {
        ImGui_ImplDX9_InvalidateDeviceObjects();
    }
    
    // Cleanup capture resources
    g_DX9Capture.Cleanup();
    
    // VSync Override
    // VSync Override
    {
        std::string mode = GetActiveGraphicsConfig().vsyncMode;
        if (mode != "default") {
            if (mode == "off") pPresentationParameters->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
            else if (mode == "fifo") pPresentationParameters->PresentationInterval = D3DPRESENT_INTERVAL_ONE;
            else if (mode == "adaptive") pPresentationParameters->PresentationInterval = D3DPRESENT_INTERVAL_ONE; // No adaptive in DX9
            else if (mode == "mailbox") pPresentationParameters->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE; // Use immediate for mailbox-like behavior
        }
        
        // Backbuffer Count Override
        int count = GetActiveGraphicsConfig().backbufferCount;
        if (count >= 2 && count <= 6) {
            pPresentationParameters->BackBufferCount = (UINT)count - 1; // DX9: BackBufferCount is additional buffers (0=1 buffer total)
            HookLog("DX9: Reset: Overriding BackBufferCount to %d", count);
        }

        // MSAA Override
        const char* msaa = GetActiveGraphicsConfig().msaaSamples.c_str();
        if (msaa[0] != 'd') {
            IDirect3D9* d3d = nullptr;
            if (SUCCEEDED(device->GetDirect3D(&d3d))) {
                D3DDEVICE_CREATION_PARAMETERS cp;
                if (SUCCEEDED(device->GetCreationParameters(&cp))) {
                    ApplyMSAAOverride(d3d, cp.AdapterOrdinal, cp.DeviceType, pPresentationParameters);
                }
                d3d->Release();
            }
        }
    }

    HRESULT hr = oReset(device, pPresentationParameters);
    
    // Recreate ImGui resources after reset
    if (g_ImGuiInitialized && SUCCEEDED(hr)) {
        ImGui_ImplDX9_CreateDeviceObjects();
    }
    
    return hr;
}

// Hook: IDirect3DDevice9Ex::ResetEx
static HRESULT STDMETHODCALLTYPE DetourResetEx(
    IDirect3DDevice9Ex *device,
    D3DPRESENT_PARAMETERS *pPresentationParameters,
    D3DDISPLAYMODEEX *pFullscreenDisplayMode
) {
    HookLog("DX9: ResetEx called");
    
    // Cleanup ImGui before reset
    if (g_ImGuiInitialized) {
        ImGui_ImplDX9_InvalidateDeviceObjects();
    }
    
    // Cleanup capture resources
    g_DX9Capture.Cleanup();
    
    // VSync Override
    // VSync Override
    {
        std::string mode = GetActiveGraphicsConfig().vsyncMode;
        if (mode != "default") {
            if (mode == "off") pPresentationParameters->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
            else if (mode == "fifo") pPresentationParameters->PresentationInterval = D3DPRESENT_INTERVAL_ONE;
            else if (mode == "adaptive") pPresentationParameters->PresentationInterval = D3DPRESENT_INTERVAL_ONE;
            else if (mode == "mailbox") pPresentationParameters->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        }

        // Backbuffer Count Override
        int count = GetActiveGraphicsConfig().backbufferCount;
        if (count >= 2 && count <= 6) {
            pPresentationParameters->BackBufferCount = (UINT)count - 1;
            HookLog("DX9: CreateDevice: Overriding BackBufferCount to %d", count);
        }
    }

    HRESULT hr = oResetEx(device, pPresentationParameters, pFullscreenDisplayMode);
    
    // Recreate ImGui resources after reset
    if (g_ImGuiInitialized && SUCCEEDED(hr)) {
        ImGui_ImplDX9_CreateDeviceObjects();
    }
    
    return hr;
}



// Hook: IDirect3D9::CreateDevice (VTable)
typedef HRESULT(STDMETHODCALLTYPE *CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
static CreateDevice_t oCreateDevice = nullptr;

// Forward declarations for detours defined below
static HRESULT STDMETHODCALLTYPE DetourPresent(IDirect3DDevice9* device, const RECT* pSourceRect, const RECT* pDestRect, HWND hDestWindowOverride, const RGNDATA* pDirtyRegion);
static HRESULT STDMETHODCALLTYPE DetourPresentEx(IDirect3DDevice9Ex* device, const RECT* pSourceRect, const RECT* pDestRect, HWND hDestWindowOverride, const RGNDATA* pDirtyRegion, DWORD dwFlags);
static HRESULT STDMETHODCALLTYPE DetourReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pPresentationParameters);
static HRESULT STDMETHODCALLTYPE DetourResetEx(IDirect3DDevice9Ex* device, D3DPRESENT_PARAMETERS* pPresentationParameters, D3DDISPLAYMODEEX* pFullscreenDisplayMode);
static HRESULT STDMETHODCALLTYPE DetourPresentSwap(IDirect3DSwapChain9* self, const RECT* pSourceRect, const RECT* pDestRect, HWND hDestWindowOverride, const RGNDATA* pDirtyRegion, DWORD dwFlags);

// Hook: SetSamplerState
static HRESULT STDMETHODCALLTYPE DetourSetSamplerState(IDirect3DDevice9* device, DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) {
    if (g_IPC && g_IPC->GetSharedMem()) {
         const auto& gfx = GetActiveGraphicsConfig();
         // Anisotropic Filtering
        std::string af = gfx.anisotropicFiltering;
        if (af != "default") {
            if (Type == D3DSAMP_MAGFILTER || Type == D3DSAMP_MINFILTER) {
                 if (af == "off") {
                      if (Value == D3DTEXF_ANISOTROPIC) Value = D3DTEXF_LINEAR;
                 } else {
                      // Only force AF if the game tries to set a filter (don't force if Point/None?)
                      // Actually users usually want to force AF over everything.
                      Value = D3DTEXF_ANISOTROPIC;
                 }
            }
            if (Type == D3DSAMP_MAXANISOTROPY) {
                 if (af == "off") Value = 1;
                 else if (af == "2x") Value = 2;
                 else if (af == "4x") Value = 4;
                 else if (af == "8x") Value = 8;
                 else Value = 16; // 16x
            }
        }
        
        // Mip Mapping
        std::string mip = gfx.mipMapping;
        if (mip != "default") {
             if (Type == D3DSAMP_MIPFILTER) {
                  if (mip == "trilinear") Value = D3DTEXF_LINEAR;
                  else if (mip == "bilinear") Value = D3DTEXF_POINT; // Bilinear usually implies Point Mip (or Linear Min/Mag + Point Mip)
             }
        }
        
        // Mip Bias
        std::string bias = gfx.mipBias;
        if (bias != "default" && Type == D3DSAMP_MIPMAPLODBIAS) {
             try {
                // Value is DWORD (float cast to DWORD)
                float fBias = std::stof(bias);
                Value = *((DWORD*)&fBias);
             } catch (...) {}
        }
    }
    return oSetSamplerState(device, Sampler, Type, Value);
}

static void InstallDeviceHooks(IDirect3DDevice9* device) {
    if (!device) return;
    
    uintptr_t *vtable = *(uintptr_t**)device;
    EarlyLog("DX9: Installing vtable hooks for device %p (vtable=%p)", device, vtable);

    // 1. Hook Present (17)
    if (!oPresent) {
        if (MH_CreateHook((void*)vtable[17], (void*)&DetourPresent, (void**)&oPresent) == MH_OK) {
            MH_EnableHook((void*)vtable[17]);
            EarlyLog("DX9: Present hook installed");
        }
    }
    
    // 2b. Hook SetSamplerState (69)
    if (!oSetSamplerState) {
        if (MH_CreateHook((void*)vtable[69], (void*)&DetourSetSamplerState, (void**)&oSetSamplerState) == MH_OK) {
            MH_EnableHook((void*)vtable[69]);
            EarlyLog("DX9: SetSamplerState hook installed");
        }
    }

    // 3. Check for IDirect3DDevice9Ex functions and hook them
    // 3. Check for IDirect3DDevice9Ex functions and hook them
    IDirect3DDevice9Ex *deviceEx = nullptr;
    static const GUID IID_IDirect3DDevice9Ex = {0xb18b10ce, 0x263e, 0x42f4, {0xa4, 0xa0, 0x29, 0x1c, 0x18, 0xd4, 0x51, 0x2c}};
    if (SUCCEEDED(device->QueryInterface(IID_IDirect3DDevice9Ex, (void**)&deviceEx))) {
        EarlyLog("DX9: Device supports D3D9Ex interfaces");
        uintptr_t *vtableEx = *(uintptr_t**)deviceEx;
        
        // Hook ResetEx (129)
        if (!oResetEx) {
            if (MH_CreateHook((void*)vtableEx[129], (void*)&DetourResetEx, (void**)&oResetEx) == MH_OK) {
                MH_EnableHook((void*)vtableEx[129]);
                EarlyLog("DX9: ResetEx hook installed");
            }
        }
        
        // Hook PresentEx (132)
        if (!oPresentEx) {
            if (MH_CreateHook((void*)vtableEx[132], (void*)&DetourPresentEx, (void**)&oPresentEx) == MH_OK) {
                MH_EnableHook((void*)vtableEx[132]);
                EarlyLog("DX9: PresentEx hook installed");
            }
        }
        
        deviceEx->Release();
    }
    
    /*
    // 6. Hook SwapChain Present (index 3)
    
    /*
    // 6. Hook SwapChain Present (index 3)
    IDirect3DSwapChain9 *swapChain = nullptr;
    if (SUCCEEDED(device->GetSwapChain(0, &swapChain)) && swapChain) {
        uintptr_t *swapVtable = *(uintptr_t**)swapChain;
        if (!oPresentSwap) {
            if (MH_CreateHook((void*)swapVtable[3], (void*)&DetourPresentSwap, (void**)&oPresentSwap) == MH_OK) {
                MH_EnableHook((void*)swapVtable[3]);
                EarlyLog("DX9: SwapChain Present hook installed");
            }
        }
        swapChain->Release();
    }
    */
}

static HRESULT STDMETHODCALLTYPE DetourCreateDevice(
    IDirect3D9 *self,
    UINT Adapter,
    D3DDEVTYPE DeviceType,
    HWND hFocusWindow,
    DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS *pPresentationParameters,
    IDirect3DDevice9 **ppReturnedDeviceInterface
) {
    EarlyLog("DX9: IDirect3D9::CreateDevice called (hFocusWindow=%p)", hFocusWindow);
    
    // VSync Override for CreateDevice
    // VSync Override for CreateDevice
    if (pPresentationParameters) {
        std::string mode = GetActiveGraphicsConfig().vsyncMode;
        if (mode != "default") {
            if (mode == "off") pPresentationParameters->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
            else if (mode == "fifo") pPresentationParameters->PresentationInterval = D3DPRESENT_INTERVAL_ONE;
            else if (mode == "adaptive") pPresentationParameters->PresentationInterval = D3DPRESENT_INTERVAL_ONE;
            else if (mode == "mailbox") pPresentationParameters->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        }

        // Backbuffer Count Override
        int count = GetActiveGraphicsConfig().backbufferCount;
        if (count >= 2 && count <= 6) {
            pPresentationParameters->BackBufferCount = (UINT)count - 1;
            HookLog("DX9: CreateDevice: Overriding BackBufferCount to %d", count);
        }

        // MSAA Override
        ApplyMSAAOverride(self, Adapter, DeviceType, pPresentationParameters);
    }

    HRESULT hr = oCreateDevice(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
    if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
        EarlyLog("DX9: CreateDevice succeeded -> %p", *ppReturnedDeviceInterface);
        InstallDeviceHooks(*ppReturnedDeviceInterface);
    }
    return hr;
}

// Hook: Direct3DCreate9 (Export)
typedef IDirect3D9* (WINAPI *Direct3DCreate9_t)(UINT SDKVersion);
static Direct3DCreate9_t oDirect3DCreate9 = nullptr;

static IDirect3D9* WINAPI DetourDirect3DCreate9(UINT SDKVersion) {
    EarlyLog("DX9: Direct3DCreate9 called (Intercepted)");
    
    IDirect3D9 *d3d9 = oDirect3DCreate9(SDKVersion);
    if (d3d9) {
        uintptr_t *vtable = *(uintptr_t**)d3d9;
        if (!oCreateDevice) {
            if (MH_CreateHook((void*)vtable[16], (void*)&DetourCreateDevice, (void**)&oCreateDevice) == MH_OK) {
                MH_EnableHook((void*)vtable[16]);
                EarlyLog("DX9: IDirect3D9::CreateDevice hook installed");
            }
        }
    }
    return d3d9;
}

// Hook: Direct3DCreate9Ex (Export)
static Direct3DCreate9Ex_t oDirect3DCreate9Ex = nullptr;

static HRESULT WINAPI DetourDirect3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex **ppOut) {
    EarlyLog("DX9: Direct3DCreate9Ex called (Intercepted)");
    HRESULT hr = oDirect3DCreate9Ex(SDKVersion, ppOut);
    if (SUCCEEDED(hr) && ppOut && *ppOut) {
        uintptr_t *vtable = *(uintptr_t**)*ppOut;
        if (!oCreateDevice) {
            if (MH_CreateHook((void*)vtable[16], (void*)&DetourCreateDevice, (void**)&oCreateDevice) == MH_OK) {
                MH_EnableHook((void*)vtable[16]);
                EarlyLog("DX9: IDirect3D9::CreateDevice hook installed via Create9Ex");
            }
        }
    }
    return hr;
}

void DX9Hook::Init() {
    EarlyLog("DX9Hook::Init() Passive starting");
    
    HMODULE d3d9Module = GetModuleHandleA("d3d9.dll");
    if (!d3d9Module) {
        EarlyLog("DX9: d3d9.dll not loaded, skipping");
        return;
    }
    
    // Hook Export Functions
    void* pD3DCreate9 = (void*)GetProcAddress(d3d9Module, "Direct3DCreate9");
    if (pD3DCreate9 && !oDirect3DCreate9) {
        if (MH_CreateHook(pD3DCreate9, (void*)&DetourDirect3DCreate9, (void**)&oDirect3DCreate9) == MH_OK) {
            MH_EnableHook(pD3DCreate9);
            EarlyLog("DX9: Direct3DCreate9 hook installed");
        }
    }
    
    void* pD3DCreate9Ex = (void*)GetProcAddress(d3d9Module, "Direct3DCreate9Ex");
    if (pD3DCreate9Ex && !oDirect3DCreate9Ex) {
        if (MH_CreateHook(pD3DCreate9Ex, (void*)&DetourDirect3DCreate9Ex, (void**)&oDirect3DCreate9Ex) == MH_OK) {
            MH_EnableHook(pD3DCreate9Ex);
            EarlyLog("DX9: Direct3DCreate9Ex hook installed");
        }
    }
    
    EarlyLog("DX9Hook::Init() Passive Complete");
    
    // Active Hooking: Create a dummy device to force vtable hooks
    // This is needed for "late" injection where the game has already created its device
    
    // 1. Create a specific window class for our dummy window
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "DX9Hook_Dummy";
    RegisterClassExA(&wc);
    
    HWND hWnd = CreateWindowA("DX9Hook_Dummy", "DX9 Dummy", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, wc.hInstance, NULL);
    
    if (hWnd && d3d9Module) {
        // Try Direct3DCreate9Ex first
        if (pD3DCreate9Ex) {
            typedef HRESULT (WINAPI *Direct3DCreate9Ex_t)(UINT, IDirect3D9Ex**);
            Direct3DCreate9Ex_t create9Ex = (Direct3DCreate9Ex_t)pD3DCreate9Ex;
            IDirect3D9Ex *d3d9ex = nullptr;
            
            if (SUCCEEDED(create9Ex(D3D_SDK_VERSION, &d3d9ex))) {
                D3DPRESENT_PARAMETERS pp = {0};
                pp.Windowed = TRUE;
                pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
                pp.hDeviceWindow = hWnd;
                
                IDirect3DDevice9Ex *deviceEx = nullptr;
                if (SUCCEEDED(d3d9ex->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd, 
                    D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, NULL, &deviceEx))) {
                    
                    EarlyLog("DX9: Active Init - Triggering hooks for D3D9Ex");
                    InstallDeviceHooks(deviceEx);
                    deviceEx->Release();
                }
                d3d9ex->Release();
            }
        }
        
        // Fallback to Direct3DCreate9 if Ex failed or wasn't tried, AND hooks are not fully installed
        // (InstallDeviceHooks checks for oPresent/oReset internally)
        if ((!oPresent || !oReset) && pD3DCreate9) {
            typedef IDirect3D9* (WINAPI *Direct3DCreate9_t)(UINT);
            Direct3DCreate9_t create9 = (Direct3DCreate9_t)pD3DCreate9;
            IDirect3D9 *d3d9 = create9(D3D_SDK_VERSION);
            
            if (d3d9) {
                 D3DPRESENT_PARAMETERS pp = {0};
                pp.Windowed = TRUE;
                pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
                pp.hDeviceWindow = hWnd;
                
                IDirect3DDevice9 *device = nullptr;
                if (SUCCEEDED(d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd, 
                    D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device))) {
                    
                    EarlyLog("DX9: Active Init - Triggering hooks for D3D9");
                    InstallDeviceHooks(device);
                    device->Release();
                }
                d3d9->Release();
            }
        }
    }
    
    if (hWnd) {
        DestroyWindow(hWnd);
        UnregisterClassA("DX9Hook_Dummy", wc.hInstance);
    }
}

void DX9Hook::Shutdown() {
    EarlyLog("DX9Hook::Shutdown()");
    
    if (g_ImGuiInitialized) {
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_ImGuiInitialized = false;
    }
    
    g_DX9Capture.Cleanup();
}

void DX9Hook::OnHostDisconnect() {
    EarlyLog("DX9Hook::OnHostDisconnect()");
    g_DX9Capture.Cleanup();
}
