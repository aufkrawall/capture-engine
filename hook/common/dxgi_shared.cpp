#include "dxgi_shared.h"
#include "../../common/raii_helpers.h"
#include "../apis/dx11_hook.h"
#include "../apis/dx12_hook.h"
#include "../apis/streamline_hook.h"
#include "../wrappers/inline_hook.h"
#include "../wrappers/vtable_hook.h"
#include "../wrappers/wrapper_base.h"
#include "config.h"
#include "dx12_overlay_policy.h"
#include "fg_detection.h"
#include "fg_session_state.h"
#include "fps_limiter.h"
#include "freeze_watchdog.h"
#include "hook_common.h"
#include "logging.h"
#include "overlay_compat.h"
#include "overlay_metrics_publisher.h"
#include "perf_logger.h"
#include "performance_metrics.h"

#include <d3d10.h>
#include <d3d10_1.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <intrin.h>
#include <psapi.h>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <unordered_set>

// Vulkan is handled by VK_LAYER_CE_overlay (ICD layer approach)
// No global hook pointer needed - extern void* g_VulkanHook;

// Put shutdown check outside the DXGIShared namespace
static bool IsShuttingDown() {
    return HookIsShuttingDown();
}

// Check if we're inside a CWrapDXGISwapChain Present call
extern bool IsInWrapperPresent();

#ifdef BUILDING_CAPTURE_HOOK
extern "C" void DX12_WaitForOverlayCompletion(ID3D12CommandQueue* pQueue);
extern "C" void DX12_FlushDeferredSignal();

static void InvokeDX12WaitForOverlayCompletion(ID3D12CommandQueue* pQueue) {
    DX12_WaitForOverlayCompletion(pQueue);
}
static void InvokeDX12FlushDeferredSignal() {
    DX12_FlushDeferredSignal();
}
#else
using PFN_DX12WaitForOverlayCompletion = void (*)(ID3D12CommandQueue* pQueue);
using PFN_DX12FlushDeferredSignal = void (*)();

static PFN_DX12WaitForOverlayCompletion ResolveDX12WaitForOverlayCompletion() {
    static std::once_flag s_once;
    static PFN_DX12WaitForOverlayCompletion s_fn = nullptr;

    std::call_once(s_once, []() {
        HMODULE hHook = GetModuleHandleA("capture_hook_x64.dll");
        if (!hHook) {
            hHook = GetModuleHandleA("capture_hook_x86.dll");
        }
        if (hHook) {
            s_fn = reinterpret_cast<PFN_DX12WaitForOverlayCompletion>(
                GetProcAddress(hHook, "DX12_WaitForOverlayCompletion"));
        }
    });

    return s_fn;
}

static PFN_DX12FlushDeferredSignal ResolveDX12FlushDeferredSignal() {
    static std::once_flag s_once;
    static PFN_DX12FlushDeferredSignal s_fn = nullptr;

    std::call_once(s_once, []() {
        HMODULE hHook = GetModuleHandleA("capture_hook_x64.dll");
        if (!hHook) {
            hHook = GetModuleHandleA("capture_hook_x86.dll");
        }
        if (hHook) {
            s_fn = reinterpret_cast<PFN_DX12FlushDeferredSignal>(GetProcAddress(hHook, "DX12_FlushDeferredSignal"));
        }
    });

    return s_fn;
}

static void InvokeDX12WaitForOverlayCompletion(ID3D12CommandQueue* pQueue) {
    PFN_DX12WaitForOverlayCompletion fn = ResolveDX12WaitForOverlayCompletion();
    if (fn) {
        fn(pQueue);
    }
}

static void InvokeDX12FlushDeferredSignal() {
    PFN_DX12FlushDeferredSignal fn = ResolveDX12FlushDeferredSignal();
    if (fn) {
        fn();
    }
}
#endif

namespace DXGIShared {

SharedState g_SharedState;
std::mutex g_SharedMutex;
static std::mutex s_thirdPartyOverlaySwapchainMutex;
static std::unordered_set<IDXGISwapChain*> s_thirdPartyOverlaySwapchains;
static std::unordered_set<IDXGISwapChain*> s_startupBlockingOverlayTaggedSwapchains;

// Post-SL FG overlay callback (set by dx12_hook.cpp when SL FG is active).
std::atomic<PostSLOverlayRenderFn> g_PostSLOverlayRenderCallback{nullptr};

// Direct Streamline FG running signal (set by streamline_hook.cpp).
std::atomic<bool> g_StreamlineFGRunning{false};

// Present call counter — incremented by DetourPresent and DetourPresent1, read by
// SL hook to detect bypass.
std::atomic<uint64_t> g_PresentCallCounter{0};

void DX12_RegisterThirdPartyOverlaySwapchain(IDXGISwapChain* pSwapChain, const char* creatorModulePath) {
    if (!pSwapChain) {
        return;
    }

    std::lock_guard<std::mutex> lock(s_thirdPartyOverlaySwapchainMutex);
    s_thirdPartyOverlaySwapchains.insert(pSwapChain);
    if (ce::overlay_compat::IsStartupBlockingOverlayModulePath(creatorModulePath)) {
        s_startupBlockingOverlayTaggedSwapchains.insert(pSwapChain);
    } else {
        s_startupBlockingOverlayTaggedSwapchains.erase(pSwapChain);
    }
}

void DX12_UnregisterThirdPartyOverlaySwapchain(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain) {
        return;
    }

    std::lock_guard<std::mutex> lock(s_thirdPartyOverlaySwapchainMutex);
    s_thirdPartyOverlaySwapchains.erase(pSwapChain);
    s_startupBlockingOverlayTaggedSwapchains.erase(pSwapChain);
}

bool DX12_IsThirdPartyOverlaySwapchain(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain) {
        return false;
    }

    std::lock_guard<std::mutex> lock(s_thirdPartyOverlaySwapchainMutex);
    return s_thirdPartyOverlaySwapchains.find(pSwapChain) != s_thirdPartyOverlaySwapchains.end();
}

bool DX12_IsStartupBlockingOverlayTaggedSwapchain(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain) {
        return false;
    }

    std::lock_guard<std::mutex> lock(s_thirdPartyOverlaySwapchainMutex);
    return s_startupBlockingOverlayTaggedSwapchains.find(pSwapChain) != s_startupBlockingOverlayTaggedSwapchains.end();
}

// Global metrics for DXGI-based APIs
static PerformanceMetrics g_DXGIPerfMetrics;
static std::atomic<uint32_t> g_LatestSourceFrameIndex{0};

// Recursion detection globals (avoiding thread_local which requires runtime
// init)
static std::atomic<DWORD> g_presentThreadId{0};
static std::atomic<int> g_presentDepth{0};
static std::atomic<DWORD> g_resizeThreadId{0};
static std::atomic<int> g_resizeDepth{0};

// Helper to check if we're recursively entering from the same thread
static bool IsRecursivePresent() {
    DWORD currentId = GetCurrentThreadId();
    DWORD expected = g_presentThreadId.load(std::memory_order_acquire);

    // Fast path: same thread re-entering (recursion)
    if (expected == currentId && g_presentDepth.load(std::memory_order_acquire) > 0) {
        return true;
    }

    // Try to claim ownership atomically — only one thread can succeed
    DWORD zero = 0;
    if (g_presentThreadId.compare_exchange_strong(zero, currentId, std::memory_order_acq_rel)) {
        g_presentDepth.store(1, std::memory_order_release);
        return false;
    }

    // Another thread owns it — if it's us (race between check and CAS), re-check
    if (g_presentThreadId.load(std::memory_order_acquire) == currentId) {
        g_presentDepth.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }

    // Different thread owns it — during Streamline DLSS FG, SL calls Present
    // from worker threads for generated frames while the game thread is still
    // inside oPresent (SL's wrapper).  These cross-thread Present calls MUST
    // be treated as re-entrant so that:
    //   1. PostSL overlay callback fires (correct timing: after SL's GPU work)
    //   2. Pre-SL overlay rendering is skipped (would conflict with SL's GPU work)
    //   3. oPresentBypass is used (avoids re-entering SL's hook chain)
    // Without this, the cross-thread call runs the full non-re-entrant path
    // including DX12ProcessFrame, causing DEVICE_HUNG from concurrent backbuffer
    // access between our overlay and SL's FG pipeline.
    if (expected != 0 && g_StreamlineFGRunning.load(std::memory_order_acquire)) {
        return true;
    }

    // Different thread owns it — treat as non-recursive, let it proceed
    // (this matches original behavior: each thread processes Present
    // independently)
    return false;
}

static void ReleasePresent() {
    if (g_presentDepth.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        g_presentThreadId.store(0, std::memory_order_release);
    }
}

static bool IsRecursiveResize() {
    DWORD currentId = GetCurrentThreadId();
    DWORD expected = g_resizeThreadId.load(std::memory_order_acquire);

    if (expected == currentId && g_resizeDepth.load(std::memory_order_acquire) > 0) {
        return true;
    }

    DWORD zero = 0;
    if (g_resizeThreadId.compare_exchange_strong(zero, currentId, std::memory_order_acq_rel)) {
        g_resizeDepth.store(1, std::memory_order_release);
        return false;
    }

    if (g_resizeThreadId.load(std::memory_order_acquire) == currentId) {
        g_resizeDepth.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }

    return false;
}

static void ReleaseResize() {
    if (g_resizeDepth.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        g_resizeThreadId.store(0, std::memory_order_release);
    }
}

// Original function pointers
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers1)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
                                                       const UINT*, IUnknown* const*);

static PFN_Present oPresent = nullptr;
static PFN_Present1 oPresent1 = nullptr;
static PFN_ResizeBuffers oResizeBuffers = nullptr;
static PFN_ResizeBuffers1 oResizeBuffers1 = nullptr;

// Inline hook trampolines - calling these bypasses the hook entirely
static PFN_Present oPresentTrampoline = nullptr;
static PFN_Present1 oPresent1Trampoline = nullptr;

// Bypass trampolines — skip external E9/FF25 hooks (e.g. Streamline) at the
// function entry point by executing original prologue bytes read from disk.
// Used in re-entrant Present calls to actually present the frame without
// re-entering the external hook chain.
static PFN_Present oPresentBypass = nullptr;
static PFN_Present1 oPresent1Bypass = nullptr;

// Saved target of the external E9 JMP on dxgi!Present, installed by Steam overlay
// (gameoverlayrenderer64!OverlayHookD3D3).  Captured during InstallPresentInlineHooks
// BEFORE Streamline overwrites it with its own JMP.  CE may invoke this target
// from SL-originated Present stacks only while the Streamline plugin-lookup guard
// is active; otherwise those paths use the bypass trampoline.
static PFN_Present g_externalOverlayPresentHook = nullptr;
static thread_local int s_externalOverlayPresentInvokeDepth = 0;

// Stored vtable pointer for unhooking Present when COM wrapper takes over
static void** s_hookedVTable = nullptr;

// Saved original vtable[8] Present COM method captured from the temp swapchain
// at InstallPresentInlineHooks time, before any vtable modifications.  This is
// the real IDXGISwapChain::Present COM method (dxgi!CDXGISwapChain::Present or
// equivalent), not the inner dxgi!Present function that Steam hooks with an E9
// JMP.  Used in the E9 JMP path of CallOriginalPresent and
// AttemptSteamDX12OverlayInit to ensure DXGI COM method state management runs
// before dxgi!Present is called with Steam's E9 JMP.  Without this, calling
// dxgi!Present directly skips COM state management, which causes black screen
// on some DX12 games (e.g. Strange Brigade).
static PFN_Present s_originalVtable8Present = nullptr;

// State for one-time Steam DX12 overlay initialization.
// Steam's OverlayHookD3D3 lazily initializes its internal "next" Present handler
// on first E9 JMP entry by reading vtable[8].  When vtable[8] = DetourPresent
// (our vtable hook), Steam's init fails and sets "next" = NULL, causing RIP=0.
//
// Fix: temporarily restore vtable[8] to the original dxgi!Present on the very
// first non-SL Steam overlay Present call, allowing Steam's init to complete.
// Re-hook vtable[8] to DetourPresent after Steam returns.
static std::atomic<bool> s_steamDX12InitAttempted{false};
static bool s_steamInitCrashed = false;

// Steam overlay has a NULL internal rendering callback at RVA ~0x1621d8
// inside gameoverlayrenderer64.dll, used by OverlayHookD3D3.  CE's vtable
// hook prevents Steam's natural initialization of this pointer.  We write
// this no-op function there as a safe placeholder so Steam can complete
// its first E9 JMP entry without crashing (the first call goes through our
// VEH handler which patches the NULL pointer and retries).  Steam
// typically overwrites this with its own real function during that same
// E9 JMP entry.
static HRESULT WINAPI SteamDummyRenderingCallback(IDXGISwapChain* /*pSwapChain*/,
                                                    UINT /*SyncInterval*/, UINT /*Flags*/) {
    return S_OK;
}

// Forward declaration (defined at line ~817)
static bool IsReadableMemory(const void* ptr, size_t size);

// VEH handler: catches Steam's NULL rendering callback crash during the
// one-time overlay init in AttemptSteamDX12OverlayInit.  Patches the NULL
// pointer at the known RVA inside gameoverlayrenderer(64).dll to
// SteamDummyRenderingCallback and retries the `call (e)ax` instruction so
// Steam completes its initialization.
//
// Architecture notes:
//   x64: returnAddr from [RSP], RIP, RAX, RSP, call rax = FF D0 (2 bytes)
//   x86: returnAddr from [ESP], EIP, EAX, ESP, call eax = FF D0 (2 bytes)
//   Steam module: x64=gameoverlayrenderer64.dll, x86=gameoverlayrenderer.dll
//   RVA: x64=0x1621d8, x86=TODO (not yet observed, same offset concept)
static LONG CALLBACK SteamOverlayInitVehHandler(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode != STATUS_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

#ifdef _WIN64
    const wchar_t* steamModuleName = L"gameoverlayrenderer64.dll";
    const uintptr_t kSteamCallbackRva = 0x1621d8;
    const int kCallOpcodeSize = 2;  // FF D0 = call rax (2 bytes)
    // RIP=0, RAX=0: calling through NULL (`call rax` where RAX loaded from NULL ptr)
    if (ep->ContextRecord->Rip != 0 || ep->ContextRecord->Rax != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    uintptr_t returnAddress = 0;
    if (ep->ContextRecord->Rsp) {
        returnAddress = *(uintptr_t*)ep->ContextRecord->Rsp;
    }
#else
    const wchar_t* steamModuleName = L"gameoverlayrenderer.dll";
    const uintptr_t kSteamCallbackRva = 0x1621d8;
    const int kCallOpcodeSize = 2;  // FF D0 = call eax (2 bytes)
    // EIP=0, EAX=0: calling through NULL (`call eax` where EAX loaded from NULL ptr)
    if (ep->ContextRecord->Eip != 0 || ep->ContextRecord->Eax != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    uintptr_t returnAddress = 0;
    if (ep->ContextRecord->Esp) {
        returnAddress = *(uintptr_t*)ep->ContextRecord->Esp;
    }
#endif

    if (!returnAddress) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    HMODULE steamMod = GetModuleHandleW(steamModuleName);
    if (!steamMod) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    MODULEINFO modInfo = {};
    if (!GetModuleInformation(GetCurrentProcess(), steamMod, &modInfo, sizeof(modInfo))) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    uintptr_t steamStart = (uintptr_t)steamMod;
    uintptr_t steamEnd = steamStart + modInfo.SizeOfImage;
    if (returnAddress < steamStart || returnAddress >= steamEnd) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Patch the NULL callback at the known RVA.
    // Safety check: verify the pointer is actually NULL before patching,
    // in case Steam updates changed the RVA.
    void** nullFnPtr = (void**)(steamStart + kSteamCallbackRva);
    bool patched = false;
    if (IsReadableMemory(nullFnPtr, sizeof(void*)) && *nullFnPtr == 0) {
        DWORD oldProtect;
        if (VirtualProtect(nullFnPtr, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            *nullFnPtr = (void*)SteamDummyRenderingCallback;
            VirtualProtect(nullFnPtr, sizeof(void*), oldProtect, &oldProtect);
            patched = true;
            HookLogImportant(
                "SteamOverlayInitVehHandler: Patched NULL callback at %p (steam+0x%zX) "
                "-> SteamDummyRenderingCallback=%p",
                nullFnPtr, kSteamCallbackRva, (void*)SteamDummyRenderingCallback);
        } else {
            HookLogImportant(
                "SteamOverlayInitVehHandler: VirtualProtect failed for Steam callback at %p",
                nullFnPtr);
        }
    } else {
        HookLogImportant(
            "SteamOverlayInitVehHandler: RVA 0x%zX is not NULL (%p) - RVA may have changed, "
            "skipping patch and falling back to crash skip",
            kSteamCallbackRva, nullFnPtr);
    }

    if (patched) {
        // Retry `call (e)ax` with our patched callback.
#ifdef _WIN64
        ep->ContextRecord->Rsp += 8;  // undo the call's stack push (8 bytes on x64)
        ep->ContextRecord->Rax = (DWORD64)SteamDummyRenderingCallback;
        ep->ContextRecord->Rip = returnAddress - kCallOpcodeSize;
        HookLog("SteamOverlayInitVehHandler: Retrying call rax with RAX=%p",
                (void*)ep->ContextRecord->Rax);
#else
        ep->ContextRecord->Esp += 4;  // undo the call's stack push (4 bytes on x86)
        ep->ContextRecord->Eax = (DWORD)SteamDummyRenderingCallback;
        ep->ContextRecord->Eip = returnAddress - kCallOpcodeSize;
        HookLog("SteamOverlayInitVehHandler: Retrying call eax with EAX=%p",
                (void*)(DWORD_PTR)ep->ContextRecord->Eax);
#endif
    } else {
        // Could not patch - skip past the crash entirely.
        // Set (R/E)IP past the call and (R/E)AX = S_OK (0) so Steam continues.
        // This may cause Steam to crash elsewhere if the callback was mandatory,
        // but at least we tried.
#ifdef _WIN64
        ep->ContextRecord->Rax = 0;  // S_OK
        ep->ContextRecord->Rip = returnAddress;
        // NOTE: RSP already points to the return address (pushed by `call rax`).
        // By setting RIP=returnAddress, we consume that pushed return address
        // as the "function returned normally". RSP is preserved correctly.
        HookLog("SteamOverlayInitVehHandler: Skipped past crash (fallback) - RIP=%p RAX=0",
                (void*)ep->ContextRecord->Rip);
#else
        ep->ContextRecord->Eax = 0;  // S_OK
        ep->ContextRecord->Eip = returnAddress;
        HookLog("SteamOverlayInitVehHandler: Skipped past crash (fallback) - EIP=%p EAX=0",
                (void*)(DWORD_PTR)ep->ContextRecord->Eip);
#endif
    }

    return EXCEPTION_CONTINUE_EXECUTION;
}

// Forward declaration — defined later in this translation unit.
static PFN_Present EnsurePresentBypassTrampoline();

enum class DX12StartupPresentMode {
    kNone,
    kPassThroughOriginal,
};

static bool IsSteamOverlayModule(const char* overlayModule) {
    return overlayModule && ce::overlay_compat::detail::ContainsInsensitive(overlayModule, "gameoverlayrenderer");
}

static bool IsStreamlineModuleHandle(HMODULE moduleHandle) {
    if (!moduleHandle) {
        return false;
    }

    char modulePath[MAX_PATH] = {};
    if (GetModuleFileNameA(moduleHandle, modulePath, MAX_PATH) == 0) {
        return false;
    }

    return ce::overlay_compat::detail::ContainsInsensitive(modulePath, "sl.interposer") ||
           ce::overlay_compat::detail::ContainsInsensitive(modulePath, "sl.common") ||
           ce::overlay_compat::detail::ContainsInsensitive(modulePath, "sl.dlss_g");
}

#if defined(__clang__) || defined(__GNUC__)
#define CE_CAPTURE_RETURN_ADDRESS() __builtin_return_address(0)
#elif defined(_MSC_VER)
#define CE_CAPTURE_RETURN_ADDRESS() _ReturnAddress()
#else
#define CE_CAPTURE_RETURN_ADDRESS() nullptr
#endif

static bool IsCodeAddressFromStreamlineModule(const void* codeAddress) {
    if (!codeAddress) {
        return false;
    }

    HMODULE callerModule = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(codeAddress), &callerModule)) {
        return false;
    }
    return IsStreamlineModuleHandle(callerModule);
}

static bool TryGetModulePathFromCodeAddress(const void* codeAddress, char* modulePathOut, size_t modulePathOutCount,
                                            HMODULE* moduleOut = nullptr) {
    return ce::overlay_compat::TryGetModulePathFromCodeAddress(codeAddress, modulePathOut, modulePathOutCount,
                                                               moduleOut);
}

static bool HasStartupBlockingOverlayModuleInCurrentStack() {
    constexpr USHORT kMaxFrames = 16;
    void* stackFrames[kMaxFrames] = {};
    const USHORT frameCount = CaptureStackBackTrace(0, kMaxFrames, stackFrames, nullptr);
    for (USHORT i = 0; i < frameCount; ++i) {
        char modulePath[MAX_PATH] = {};
        if (TryGetModulePathFromCodeAddress(stackFrames[i], modulePath, sizeof(modulePath)) &&
            ce::overlay_compat::IsStartupBlockingOverlayModulePath(modulePath)) {
            return true;
        }
    }

    return false;
}

static bool HasStreamlineModuleInCurrentStack() {
    constexpr USHORT kMaxFrames = 24;
    void* stackFrames[kMaxFrames] = {};
    const USHORT frameCount = CaptureStackBackTrace(0, kMaxFrames, stackFrames, nullptr);
    for (USHORT i = 0; i < frameCount; ++i) {
        HMODULE module = nullptr;
        char modulePath[MAX_PATH] = {};
        if (TryGetModulePathFromCodeAddress(stackFrames[i], modulePath, sizeof(modulePath), &module) &&
            IsStreamlineModuleHandle(module)) {
            return true;
        }
    }

    return false;
}

// SL worker thread detection removed — handled directly in DetourPresent
// via !HookIsPostSLOverlayConfirmedRendering() guard.

static bool IsWrappedSwapChainObject(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain) {
        return false;
    }

    void* pWrapper = nullptr;
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_CWrapDXGISwapChain, &pWrapper))) {
        ((IUnknown*)pWrapper)->Release();
        return true;
    }

    return false;
}

APIType DetectAPIType(IDXGISwapChain* pSwapChain);

static bool ShouldForceSteamDX12Bypass(IDXGISwapChain* pSwapChain, bool bypassAvailable, bool slLoaded,
                                       const char** overlayModuleOut = nullptr) {
    const char* overlayModule = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
    if (overlayModuleOut) {
        *overlayModuleOut = overlayModule;
    }
    if (!pSwapChain || !bypassAvailable || !IsSteamOverlayModule(overlayModule)) {
        return false;
    }

    return ShouldForceSteamDX12BypassForState(
        bypassAvailable, true, DetectAPIType(pSwapChain) == APIType::D3D12, IsInWrapperPresent(),
        IsWrappedSwapChainObject(pSwapChain), slLoaded, g_FGCompat.GetRuntimeMode(),
        g_StreamlineFGRunning.load(std::memory_order_acquire), g_FGCompat.IsNvPresentLoaded());
}

static bool ShouldForceThirdPartyOverlayBypass(IDXGISwapChain* pSwapChain, bool bypassAvailable,
                                               const char** overlayModuleOut = nullptr) {
    const char* overlayModule = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
    if (overlayModuleOut) {
        *overlayModuleOut = overlayModule;
    }
    if (!pSwapChain || !bypassAvailable || !overlayModule) {
        return false;
    }

    if (!IsInWrapperPresent() && !IsWrappedSwapChainObject(pSwapChain)) {
        return false;
    }

    return true;
}

static DX12StartupPresentMode GetDX12StartupPresentMode(bool bypassAvailable, const char** overlayModuleOut = nullptr,
                                                        int* passIndexOut = nullptr) {
    const char* overlayModule = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
    if (overlayModuleOut) {
        *overlayModuleOut = overlayModule;
    }
    const bool steamBypassShouldOwnPath = ShouldForceSteamDX12BypassForState(
        bypassAvailable, IsSteamOverlayModule(overlayModule), true, false, false,
        GetModuleHandleA("sl.interposer.dll") != nullptr, g_FGCompat.GetRuntimeMode(),
        g_StreamlineFGRunning.load(std::memory_order_acquire), g_FGCompat.IsNvPresentLoaded());
    const bool bypassReady = EnsurePresentBypassTrampoline() != nullptr;
    if (!DXGIShared::ShouldAllowDX12StartupPresentPassForState(overlayModule != nullptr, oPresentTrampoline != nullptr,
                                                               oPresent1Trampoline != nullptr, steamBypassShouldOwnPath,
                                                               bypassReady,
                                                               g_FGCompat.GetRuntimeMode(),
                                                               g_StreamlineFGRunning.load(std::memory_order_acquire))) {
        static std::atomic<int> s_startupPassBlockLogCount{0};
        const int blockNum = s_startupPassBlockLogCount.fetch_add(1, std::memory_order_relaxed);
        if (blockNum < 5) {
            HookLogImportant(
                "GetDX12StartupPresentMode: Startup compat pass blocked "
                "(overlay=%d trampoline=%d bypass=%d steamBypassOwn=%d runtimeMode=%d slFG=%d)",
                overlayModule != nullptr ? 1 : 0, oPresentTrampoline != nullptr ? 1 : 0,
                bypassReady ? 1 : 0, steamBypassShouldOwnPath ? 1 : 0,
                static_cast<int>(g_FGCompat.GetRuntimeMode()),
                g_StreamlineFGRunning.load(std::memory_order_acquire) ? 1 : 0);
        }
        return DX12StartupPresentMode::kNone;
    }

    static std::atomic<int> s_startupPassCount{0};
    const bool steamOverlay = IsSteamOverlayModule(overlayModule);
    const int startupCompatFrames = (steamOverlay && bypassAvailable) ? 16 : 3;
    int expected = s_startupPassCount.load(std::memory_order_acquire);
    while (expected < startupCompatFrames) {
        if (s_startupPassCount.compare_exchange_weak(expected, expected + 1, std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
            if (passIndexOut) {
                *passIndexOut = expected + 1;
            }
            return DX12StartupPresentMode::kPassThroughOriginal;
        }
    }
    return DX12StartupPresentMode::kNone;
}

// Vulkan detection via ICD layer - returns false since we use layer approach
bool IsVulkanPrimary() {
    // VK_LAYER_CE_overlay handles Vulkan separately
    return false;
}

static UINT ResolvePresentFrameLatencyOverride(const char** sourceOut) {
    const auto& cfg = GetActiveGraphicsConfig();

    if (cfg.frameLatency > 0) {
        if (sourceOut)
            *sourceOut = "frame_latency";
        return static_cast<UINT>(cfg.frameLatency);
    }
    if (cfg.cpuPrerenderLimit > 0) {
        if (sourceOut)
            *sourceOut = "cpu_prerender_limit";
        return static_cast<UINT>(cfg.cpuPrerenderLimit);
    }

    if (sourceOut)
        *sourceOut = nullptr;
    return 0;
}

// Apply user-configured present-queue latency overrides to an existing swapchain.
// NOTE: backbuffer_count is handled at swapchain creation and resize time.
void ApplyPresentFrameLatencyOverrides(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return;

    const char* source = nullptr;
    UINT requested = ResolvePresentFrameLatencyOverride(&source);
    if (requested > 16)
        requested = 16;

    static std::mutex s_latencyOverrideMutex;
    static uint64_t s_lastSwapchain = 0;
    static UINT s_lastRequested = 0;

    const uint64_t scKey = reinterpret_cast<uint64_t>(pSwapChain);

    if (requested == 0) {
        return;
    }

    IDXGISwapChain2* sc2 = nullptr;
    if (FAILED(pSwapChain->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&sc2)) || !sc2) {
        static std::atomic<int> s_qiFailLogCount{0};
        if (requested > 0 && s_qiFailLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
            HookLogImportant("ApplyPresentFrameLatencyOverrides: IDXGISwapChain2 unavailable for %s",
                             source ? source : "override");
        }
        return;
    }

    std::lock_guard<std::mutex> lock(s_latencyOverrideMutex);

    if (s_lastSwapchain == scKey && s_lastRequested == requested) {
        sc2->Release();
        return;
    }

    HRESULT hr = sc2->SetMaximumFrameLatency(requested);
    if (SUCCEEDED(hr)) {
        HookLogImportant("ApplyPresentFrameLatencyOverrides: SetMaximumFrameLatency(%u) OK (%s)", requested,
                         source ? source : "override");
    } else {
        HookLogImportant("ApplyPresentFrameLatencyOverrides: SetMaximumFrameLatency(%u) failed hr=0x%08X (%s)",
                         requested, hr, source ? source : "override");
    }

    s_lastSwapchain = scKey;
    s_lastRequested = requested;

    sc2->Release();
}

PerformanceMetrics* GetPerformanceMetrics() {
    return &g_DXGIPerfMetrics;
}

uint32_t GetLatestSourceFrameIndex() {
    return g_LatestSourceFrameIndex.load(std::memory_order_relaxed);
}

void SetLatestSourceFrameIndex(uint32_t frameIndex) {
    g_LatestSourceFrameIndex.store(frameIndex, std::memory_order_relaxed);
}

APIType DetectAPIType(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return APIType::Unknown;

    // Fast path: avoid expensive GetDevice() calls every Present on the same
    // swapchain/thread.
    thread_local IDXGISwapChain* s_cachedSwapchain = nullptr;
    thread_local APIType s_cachedApi = APIType::Unknown;
    if (pSwapChain == s_cachedSwapchain && s_cachedApi != APIType::Unknown) {
        return s_cachedApi;
    }

    bool hasD3D12Device = false;
    bool hasD3D11Device = false;
    bool hasD3D10Device = false;

    ID3D12Device* d12Device = nullptr;
    if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D12Device), (void**)&d12Device)) && d12Device) {
        d12Device->Release();
        hasD3D12Device = true;
    }

    // Always try all three — do NOT short-circuit when D3D11 succeeds.
    // On Windows 10+ the D3D10 runtime is implemented on D3D11
    // (D3D10-on-D3D11).  A D3D10 device will QI for BOTH ID3D11Device
    // and ID3D10Device, so checking D3D11 first and skipping D3D10
    // would wrongly classify the swapchain as D3D11.
    ID3D11Device* d11Device = nullptr;
    if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&d11Device)) && d11Device) {
        d11Device->Release();
        hasD3D11Device = true;
    }

    ID3D10Device* d10Device = nullptr;
    if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D10Device), (void**)&d10Device)) && d10Device) {
        d10Device->Release();
        hasD3D10Device = true;
    } else {
        ID3D10Device1* d10Device1 = nullptr;
        if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D10Device1), (void**)&d10Device1)) && d10Device1) {
            d10Device1->Release();
            hasD3D10Device = true;
        }
    }

    APIType detected = SelectPrimarySwapChainAPIType(hasD3D12Device, hasD3D11Device, hasD3D10Device);

    s_cachedSwapchain = pSwapChain;
    s_cachedApi = detected;
    return detected;
}

// Helper to get Present function address from a swapchain's vtable
static void* GetPresentAddress(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return nullptr;
    void** vtable = *(void***)pSwapChain;
    return vtable[8];  // Present is at vtable slot 8
}

// Helper to get Present1 function address from a swapchain's vtable
static void* GetPresent1Address(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return nullptr;
    void** vtable = *(void***)pSwapChain;
    return vtable[22];  // Present1 is at vtable slot 22
}

// Global flag to disable DXGI hooks when Vulkan is active
// This is set once at startup and prevents DXGI hooks from interfering with
// Vulkan WSI
static bool s_vulkanPresent = false;
static bool s_checkedVulkan = false;

// Forward declaration for lazy hook installation
static void InstallHooksIfPending(IDXGISwapChain* pSwapChain);

static bool IsVulkanActive() {
    if (!s_checkedVulkan) {
        HMODULE hVulkan = GetModuleHandleW(L"vulkan-1.dll");
        s_vulkanPresent = (hVulkan != nullptr);
        if (s_vulkanPresent) {
            HookLog(
                "DXGIShared: Vulkan detected (vulkan-1.dll), DXGI hooks will "
                "pass through");
        }
        s_checkedVulkan = true;
    }
    return s_vulkanPresent;
}

static bool IsThirdPartyOverlayLoaded() {
    return ce::overlay_compat::IsThirdPartyOverlayLoaded();
}

// Unified Detours
// For DX12 wrapped swapchains: CWrapDXGISwapChain handles Present, so when
// wrapper calls m_pReal->Present() and it re-enters here, we just passthrough.
// For DX12 pre-existing swapchains (not wrapped): full processing here.
// For DX11: full processing here.
static bool IsReadableMemory(const void* ptr, size_t size) {
    if (!ptr)
        return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return false;
    return (mbi.Protect &
            (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY)) != 0;
}

// Forward declarations for SL vtable hook (defined below).
HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
HRESULT STDMETHODCALLTYPE DetourPresent1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                                         const DXGI_PRESENT_PARAMETERS* pPresentParameters);

static bool HasExternalEntryHook(const void* target) {
    const auto* code = static_cast<const uint8_t*>(target);
    if (!IsReadableMemory(code, 16)) {
        return false;
    }
    return code[0] == 0xE9 || (code[0] == 0xFF && code[1] == 0x25);
}

// Resolves the target of an E9 (near JMP) hook at the given function address.
// Returns the absolute address of the hook handler, or nullptr if no E9 JMP
// is present or the function body is unreadable.
static void* ResolveE9JmpTarget(void* funcAddress) {
    if (!funcAddress) {
        return nullptr;
    }
    const auto* code = static_cast<const uint8_t*>(funcAddress);
    if (!IsReadableMemory(code, 5)) {
        return nullptr;
    }
    if (code[0] != 0xE9) {
        return nullptr;
    }
    int32_t relOffset;
    memcpy(&relOffset, code + 1, sizeof(relOffset));
    return static_cast<uint8_t*>(funcAddress) + 5 + relOffset;
}

static PFN_Present EnsurePresentBypassTrampoline() {
    if (oPresentBypass) {
        return oPresentBypass;
    }

    std::lock_guard<std::mutex> lock(g_SharedMutex);
    if (oPresentBypass) {
        return oPresentBypass;
    }

    const PFN_Present presentOriginal = oPresent;
    if (!presentOriginal || presentOriginal == DetourPresent || !HasExternalEntryHook((const void*)presentOriginal)) {
        return nullptr;
    }

    void* bypass = InlineHook::CreateBypassTrampoline((void*)presentOriginal);
    if (!bypass) {
        static int s_bypassFailLogCount = 0;
        if (s_bypassFailLogCount++ < 5) {
            HookLogImportant("DXGIShared: Failed to lazily create Present bypass trampoline from %p", presentOriginal);
        }
        return nullptr;
    }

    oPresentBypass = (PFN_Present)bypass;
    HookLogImportant("DXGIShared: Lazily created Present bypass trampoline at %p from %p", bypass, presentOriginal);
    return oPresentBypass;
}

static PFN_Present1 EnsurePresent1BypassTrampoline() {
    if (oPresent1Bypass) {
        return oPresent1Bypass;
    }

    std::lock_guard<std::mutex> lock(g_SharedMutex);
    if (oPresent1Bypass) {
        return oPresent1Bypass;
    }

    const PFN_Present1 present1Original = oPresent1;
    if (!present1Original || present1Original == DetourPresent1 ||
        !HasExternalEntryHook((const void*)present1Original)) {
        return nullptr;
    }

    void* bypass = InlineHook::CreateBypassTrampoline((void*)present1Original);
    if (!bypass) {
        static int s_bypassFailLogCount = 0;
        if (s_bypassFailLogCount++ < 5) {
            HookLogImportant("DXGIShared: Failed to lazily create Present1 bypass trampoline from %p",
                             present1Original);
        }
        return nullptr;
    }

    oPresent1Bypass = (PFN_Present1)bypass;
    HookLogImportant("DXGIShared: Lazily created Present1 bypass trampoline at %p from %p", bypass, present1Original);
    return oPresent1Bypass;
}

static bool TryInvokeGuardedExternalSteamOverlayPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                                                        const char* reason, HRESULT* resultOut) {
    if (!pSwapChain || !resultOut) {
        return false;
    }

    PFN_Present externalPresent = g_externalOverlayPresentHook;
    PFN_Present presentBypass = EnsurePresentBypassTrampoline();
    const char* overlayModule = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
    const bool isD3D12SwapChain = DetectAPIType(pSwapChain) == APIType::D3D12;
    const bool streamlineStackActive = isD3D12SwapChain && HasStreamlineModuleInCurrentStack();
    const bool streamlinePluginLookupGuardReady = StreamlineHook::IsExternalOverlayPluginLookupGuardReady();
    const bool streamlineFGRunning = g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool postSLConfirmedRendering = isD3D12SwapChain && HookIsPostSLOverlayConfirmedRendering();
    const bool postSLConfirmedButStartupSettling =
        isD3D12SwapChain && HookIsPostSLOverlayConfirmedButStartupSettling();
    const bool startupTransitionWindowActive =
        isD3D12SwapChain && DXGIShared::IsStreamlineStartupTransitionWindowActive();
    if (!DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForState(
            externalPresent != nullptr && externalPresent != DetourPresent, presentBypass != nullptr,
            IsSteamOverlayModule(overlayModule), isD3D12SwapChain, IsInWrapperPresent(),
            IsWrappedSwapChainObject(pSwapChain), s_externalOverlayPresentInvokeDepth > 0, streamlineStackActive,
            streamlinePluginLookupGuardReady)) {
        if (externalPresent && presentBypass && IsSteamOverlayModule(overlayModule) && isD3D12SwapChain &&
            streamlineStackActive) {
            static std::atomic<int> s_guardedSteamStreamlineStackSkipLogCount{0};
            const int skipNum =
                s_guardedSteamStreamlineStackSkipLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (skipNum <= 20 || skipNum == 50 || (skipNum % 500) == 0) {
                HookLogImportant(
                    "DXGIShared: Skipping guarded Steam Present hook #%d for %s because current stack is inside "
                    "Streamline startup/FG routing and plugin lookup guard is not ready; using bypass trampoline "
                    "instead (slFG=%d confirmed=%d settling=%d startupWindow=%d pluginGuard=%d invokeDepth=%d "
                    "tid=0x%04X)",
                    skipNum, reason ? reason : "Present", streamlineFGRunning ? 1 : 0,
                    postSLConfirmedRendering ? 1 : 0, postSLConfirmedButStartupSettling ? 1 : 0,
                    startupTransitionWindowActive ? 1 : 0, streamlinePluginLookupGuardReady ? 1 : 0,
                    s_externalOverlayPresentInvokeDepth, GetCurrentThreadId());
            }
        }
        return false;
    }

    static std::atomic<int> s_guardedSteamInvokeLogCount{0};
    const int invokeNum = s_guardedSteamInvokeLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (invokeNum <= 20 || invokeNum == 50 || (invokeNum % 500) == 0) {
        HookLogImportant(
            "DXGIShared: Invoking guarded Steam Present hook #%d for %s "
            "(hook=%p bypass=%p slLoaded=%d streamlineFG=%d streamlineStack=%d pluginGuard=%d tid=0x%04X)",
            invokeNum, reason ? reason : "Present", (void*)externalPresent, (void*)presentBypass,
            GetModuleHandleA("sl.interposer.dll") ? 1 : 0, streamlineFGRunning ? 1 : 0,
            streamlineStackActive ? 1 : 0, streamlinePluginLookupGuardReady ? 1 : 0, GetCurrentThreadId());
    }

    ++s_externalOverlayPresentInvokeDepth;
    auto depthGuard = ce::make_scope_guard([]() {
        if (s_externalOverlayPresentInvokeDepth > 0) {
            --s_externalOverlayPresentInvokeDepth;
        }
    });
    StreamlineHook::ExternalOverlayPresentGuard slGuard;

    const HRESULT hr = externalPresent(pSwapChain, SyncInterval, Flags);
    if (FAILED(hr)) {
        static std::atomic<int> s_guardedSteamFailureLogCount{0};
        const int failureNum = s_guardedSteamFailureLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (failureNum <= 10 || (failureNum % 100) == 0) {
            HookLogImportant("DXGIShared: Guarded Steam Present hook failed for %s hr=0x%08X (failure #%d)",
                             reason ? reason : "Present", (unsigned)hr, failureNum);
        }
    }

    *resultOut = hr;
    return true;
}

namespace {

static bool IsSLInterposerLoaded();

// Streamline FG routing state.
//
// Problem: When SL hooks Present with an E9 JMP at the function entry, our
// inline hook trampoline (oPresentTrampoline) bypasses SL's hook entirely,
// because the trampoline contains the ORIGINAL function bytes (from before
// any hooks).  With SL bypassed, Frame Generation never runs.
//
// Solution: Detect SL's E9 JMP on the Present function and route through it
// instead of through the trampoline.  This way:
//   Game → vtable[8] (DetourPresent) → overlay render →
//   oPresent (has SL E9 JMP) → SL_Detour → SL trampoline (has our FF 25) →
//   DetourPresent (re-entrant, forwarded to oPresentTrampoline) →
//   real Present → SL post-Present FG → return
//
// The vtable already points to DetourPresent (from inline hook install).
// We just need to change the FINAL call from oPresentTrampoline to oPresent.
static std::atomic<bool> s_slRoutingActive{false};

static bool IsSLInterposerLoaded() {
    static bool detected = false;
    if (detected)
        return true;
    detected = (GetModuleHandleA("sl.interposer.dll") != nullptr);
    return detected;
}

static bool ShouldKeepSLPresentRoutingDisabledNow(ce::fg_runtime::RuntimeMode* runtimeModeOut = nullptr,
                                                  bool* runtimeOwnedNativeFGPresentPathOut = nullptr) {
    const auto runtimeMode = g_FGCompat.GetRuntimeMode();
    const bool runtimeOwnedNativeFGPresentPath = HookHasRuntimeOwnedNativeFGPresentPath();
    if (runtimeModeOut) {
        *runtimeModeOut = runtimeMode;
    }
    if (runtimeOwnedNativeFGPresentPathOut) {
        *runtimeOwnedNativeFGPresentPathOut = runtimeOwnedNativeFGPresentPath;
    }
    return DXGIShared::ShouldKeepSLPresentRoutingDisabledForNativeFG(ce::fg_runtime::RuntimeModeUsesFSR(runtimeMode),
                                                                     runtimeOwnedNativeFGPresentPath);
}

// Detect if SL has hooked the Present function with an E9 JMP or FF 25
// indirect JMP.  If so, set up routing so our final Present call goes
// through SL's hook chain instead of bypassing it via the trampoline.
static void DetectSLPresentHook() {
    if (s_slRoutingActive.load(std::memory_order_acquire))
        return;
    if (!oPresent || !oPresentTrampoline) {
        // Vtable hook path (externally hooked Present): oPresentTrampoline is
        // NULL because we use vtable hooking instead of inline hooking when an
        // external E9 JMP (e.g. Steam overlay) is detected on dxgi!Present.
        // In this path, oPresent is the vtable's Present entry (whose dxgi.dll
        // bytes may be owned by Steam/SL), so SL routing detection via E9 JMP on
        // oPresent bytes does not apply here.  CE uses guarded Steam-overlay
        // invocations for the bypass-only paths and otherwise lets normal
        // vtable routing decide the live Present chain.
        // Log once so post-mortem analysis can distinguish the vtable path from
        // a missing inline hook bug.
        static std::atomic<uint32_t> s_vtablePathLogCount{0};
        if (s_vtablePathLogCount.fetch_add(1, std::memory_order_relaxed) < 3) {
            HookLogImportant(
                "DetectSLPresentHook: Skipping — vtable hook path (oPresent=%p, oPresentTrampoline=NULL). "
                "SL routing detection is not applicable on the external-overlay vtable path.",
                oPresent);
        }
        return;
    }

    // If oPresent is our own trampoline, SL hasn't hooked the vtable yet.
    // The vtable repair code sets oPresent to SL's hook when detected.
    if (oPresent == oPresentTrampoline)
        return;

    auto* funcBytes = (const uint8_t*)oPresent;
    if (!IsReadableMemory(funcBytes, 16))
        return;

    // Rate-limit diagnostic logging to avoid per-frame spam.
    static int s_checkCount = 0;
    int checkNum = ++s_checkCount;
    if (checkNum <= 5 || (checkNum <= 50 && (checkNum % 10) == 0) || (checkNum % 500) == 0) {
        HookLogImportant("DetectSLPresentHook: oPresent=%p bytes: %02X %02X %02X %02X %02X %02X (check #%d)", oPresent,
                         funcBytes[0], funcBytes[1], funcBytes[2], funcBytes[3], funcBytes[4], funcBytes[5], checkNum);
    }

    // Detect SL hooks: E9 relative JMP or FF 25 indirect JMP (JMP [RIP+0]).
    // SL may use either pattern depending on version and game.
    bool isE9 = (funcBytes[0] == 0xE9);
    bool isFF25 = (funcBytes[0] == 0xFF && funcBytes[1] == 0x25);

    if (!isE9 && !isFF25) {
        return;
    }

    // Verify that our trampoline is different (it should have the original
    // function bytes, not a JMP).
    auto* trampolineBytes = (const uint8_t*)oPresentTrampoline;
    HookLogImportant("DetectSLPresentHook: trampoline=%p bytes: %02X %02X %02X %02X %02X %02X", oPresentTrampoline,
                     trampolineBytes[0], trampolineBytes[1], trampolineBytes[2], trampolineBytes[3], trampolineBytes[4],
                     trampolineBytes[5]);

    ce::fg_runtime::RuntimeMode runtimeMode = ce::fg_runtime::RuntimeMode::kOff;
    bool runtimeOwnedNativeFGPresentPath = false;

    // Don't re-enable SL routing while the native/runtime-owned FSR path still
    // owns presentation. GTA showed that the old runtime=FSR_FG-only guard was
    // too narrow: during the explicit native-FSR OFF teardown window, the FFX
    // runtime can still own presentation even though ffxConfigure briefly
    // publishes Off. Re-attaching Streamline's Present hook chain in that window
    // reintroduces mixed-runtime routing on the native FSR path.
    if (ShouldKeepSLPresentRoutingDisabledNow(&runtimeMode, &runtimeOwnedNativeFGPresentPath)) {
        static int s_suppressedCount = 0;
        int suppressedNum = ++s_suppressedCount;
        if (suppressedNum <= 5 || (suppressedNum % 500) == 0) {
            HookLogImportant(
                "DetectSLPresentHook: SL %s JMP detected at oPresent=%p but SL routing NOT re-enabled "
                "(native FG path still owns Present routing, suppressed #%d, runtime=%s runtimeOwnedNativeFG=%d)",
                isE9 ? "E9" : "FF25", oPresent, suppressedNum, ce::fg_runtime::GetRuntimeModeName(runtimeMode),
                runtimeOwnedNativeFGPresentPath ? 1 : 0);
        }
        return;
    }

    s_slRoutingActive.store(true, std::memory_order_release);
    HookLogImportant(
        "SL routing ACTIVE: Present calls will go through oPresent=%p "
        "(%s JMP) instead of trampoline=%p.  SL FG chain will execute.",
        oPresent, isE9 ? "E9" : "FF25", oPresentTrampoline);
}

static void UpdateDXGIPresentMetricsAndPublish(bool isFirstHook, const char* publicationSource) {
    if (!isFirstHook) {
        return;
    }

    ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kPresentObserved, publicationSource);

    static int64_t qpcFreq = 0;
    if (qpcFreq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpcFreq = f.QuadPart;
    }

    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    const int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;
    g_DXGIPerfMetrics.Update(us);
    const ce::fg_session::FGActionPlan plan = ce::fg_session::GetLatestFGActionPlan();
    ce::overlay_metrics::PublishOverlayFGMetrics(&g_DXGIPerfMetrics, plan, g_FGCompat.GetOutputFPS(),
                                                 g_FGCompat.GetBaseFPS(), g_FGCompat.GetFGMultiplier(),
                                                 publicationSource);
}

static void RefreshLivePresentHooksForSwapchainIfNeeded(IDXGISwapChain* pSwapChain, const char* source) {
    if (!pSwapChain || !IsReadableMemory(pSwapChain, sizeof(void*))) {
        return;
    }

    void** vtable = *(void***)pSwapChain;
    const bool hasReadableVtable = vtable && IsReadableMemory(vtable, 23 * sizeof(void*));
    const bool trackedVtableMatchesCurrent = hasReadableVtable && s_hookedVTable == vtable;
    const bool presentHookInstalled = hasReadableVtable && vtable[8] == (void*)DetourPresent;
    const bool present1HookInstalled = hasReadableVtable && vtable[22] == (void*)DetourPresent1;

    if (!DXGIShared::ShouldRefreshLivePresentHooksForSwapchainPath(hasReadableVtable, trackedVtableMatchesCurrent,
                                                                   presentHookInstalled, present1HookInstalled)) {
        return;
    }

    HookLogImportant(
        "DXGIShared: Refreshing live Present hook path via %s swapchain %p (oldVtable=%p newVtable=%p hooked8=%d "
        "hooked22=%d)",
        source ? source : "runtime", pSwapChain, s_hookedVTable, vtable, presentHookInstalled ? 1 : 0,
        present1HookInstalled ? 1 : 0);

    InstallHooks(pSwapChain, true);
    RepairVTableHooksIfNeeded();

    if (IsSLInterposerLoaded() && !ce::fg_runtime::RuntimeModeUsesFSR(g_FGCompat.GetRuntimeMode())) {
        DetectSLPresentHook();
    }
}
}  // namespace

HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    g_PresentCallCounter.fetch_add(1, std::memory_order_relaxed);

    // CRITICAL: Recursion guard using thread-local depth counter.
    // When using vtable hooking with Steam overlay, Steam's trampoline can call
    // back into DetourPresent via early-return paths (wrapped swapchain, shutdown, etc.)
    // before reaching the normal IsRecursivePresent() check at line ~530.
    // This caused stack overflow crashes (0xC00000FD) with ~500 recursive frames.
    static thread_local int s_presentRecurseDepth = 0;
    const bool wrappedSwapchain = IsWrappedSwapChainObject(pSwapChain);
    const bool inWrapperPresent = IsInWrapperPresent();
    const bool streamlineFGRunning = g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool isReentrant =
        (s_presentRecurseDepth > 0) && DXGIShared::ShouldTreatEarlyPresentRecursionAsForwardable(
                                           oPresentTrampoline != nullptr, oPresentBypass != nullptr, inWrapperPresent,
                                           wrappedSwapchain, streamlineFGRunning);
    s_presentRecurseDepth++;
    auto depthGuard = ce::make_scope_guard([]() { s_presentRecurseDepth--; });

    if (isReentrant) {
        // Re-entrant call - forward directly to bypass or return S_OK
        if (oPresentTrampoline) {
            return oPresentTrampoline(pSwapChain, SyncInterval, Flags);
        }
        if (oPresentBypass) {
            return oPresentBypass(pSwapChain, SyncInterval, Flags);
        }
        // No bypass available - return S_OK to break recursion loop
        return S_OK;
    }

    static int s_entryCount = 0;
    int entryNum = ++s_entryCount;

    // Present-call heartbeat diagnostic:
    // Logs periodically (every 1000th call) and whenever there's a gap >250ms.
    // Purpose: Detect whether the game stops calling Present during menus/pauses.
    //
    // GTA V Enhanced: During pause menu, Present calls stop entirely (10+ second
    // gaps observed).  This means our overlay can't render unless we detect the
    // gap and use an alternative rendering mechanism (like the pre-SL stall
    // fallback in ProcessFrame).
    //
    // Also logs: IsRecursivePresent (SL FG re-entrant calls), g_StreamlineFGRunning
    // (whether SL thinks FG is active), and thread ID (SL uses worker threads).
    {
        static LARGE_INTEGER s_lastPresentTime = {};
        static int s_heartbeatCount = 0;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (s_lastPresentTime.QuadPart != 0) {
            LARGE_INTEGER freq;
            QueryPerformanceFrequency(&freq);
            double gapMs = (double)(now.QuadPart - s_lastPresentTime.QuadPart) * 1000.0 / freq.QuadPart;
            static constexpr double kLargePresentGapMs = 250.0;

            // Treat quarter-second Present gaps as scene/load transitions. This
            // is conservative enough to ignore ordinary jitter while still
            // catching save-load handoff disruptions.
            if (gapMs > kLargePresentGapMs || (s_heartbeatCount % 1000 == 0)) {
                if (gapMs > kLargePresentGapMs) {
                    MarkLargePresentGap();
                }
                // READ-ONLY state peek: DO NOT call IsRecursivePresent() here!
                // IsRecursivePresent() has side effects (CAS on g_presentThreadId)
                // and would permanently corrupt the present ownership tracking,
                // making ALL subsequent calls appear recursive and blocking
                // ProcessFrame from ever running again.
                DWORD presentOwner = g_presentThreadId.load(std::memory_order_relaxed);
                int presentDepthVal = g_presentDepth.load(std::memory_order_relaxed);
                HookLogImportant(
                    "DetourPresent: heartbeat #%d gap=%.0fms presentOwner=0x%04X depth=%d slFG=%d tid=0x%04X",
                    s_heartbeatCount, gapMs, presentOwner, presentDepthVal,
                    g_StreamlineFGRunning.load(std::memory_order_relaxed) ? 1 : 0, GetCurrentThreadId());
            }
        }
        s_lastPresentTime = now;
        s_heartbeatCount++;
    }

    if (entryNum <= 10) {
        HookLog(
            "DetourPresent: ENTRY #%d (pSwapChain=%p, IsInWrapper=%d, "
            "trampoline=%p)",
            entryNum, pSwapChain, IsInWrapperPresent() ? 1 : 0, oPresentTrampoline);
    }

    if (!pSwapChain) {
        return DXGI_ERROR_INVALID_CALL;
    }

    const APIType api = DetectAPIType(pSwapChain);
    // Capture the caller here, not in a helper. We need the code that called
    // into DetourPresent, not the helper's own return address inside this DLL.
    const void* detourCallerAddress = CE_CAPTURE_RETURN_ADDRESS();
    char detourCallerModulePath[MAX_PATH] = {};
    const bool callerFromThirdPartyOverlay =
        TryGetModulePathFromCodeAddress(detourCallerAddress, detourCallerModulePath, sizeof(detourCallerModulePath)) &&
        ce::overlay_compat::IsThirdPartyOverlayModulePath(detourCallerModulePath);
    const bool streamlineStartupHandoffPending = (api == APIType::D3D12) && IsStreamlineStartupHandoffPending();
    const bool streamlineStartupTransitionWindowActive =
        (api == APIType::D3D12) && IsStreamlineStartupTransitionWindowActive();
    const bool streamlineStartupHandoffInProgress =
        streamlineStartupHandoffPending || streamlineStartupTransitionWindowActive;
    const DWORD currentThreadId = GetCurrentThreadId();
    const DWORD presentOwner = g_presentThreadId.load(std::memory_order_relaxed);
    const int presentDepthVal = g_presentDepth.load(std::memory_order_relaxed);
    const bool presentOwnershipActive = presentOwner != 0 || presentDepthVal > 0;
    const DWORD expectedPresentThreadId = g_RenderWatchdog.GetMonitoredThreadId();
    const bool matchesExpectedPresentThread =
        expectedPresentThreadId == 0 || expectedPresentThreadId == currentThreadId;
    const bool callerFromStreamlineModule = IsCodeAddressFromStreamlineModule(detourCallerAddress);
    const bool recentLargePresentGap = HasRecentLargePresentGap(500);
    const bool startupTopLevelPresentAlreadyConsumed =
        g_SharedState.streamlineStartupTopLevelPresentConsumed.load(std::memory_order_acquire);
    const bool postSLStartupActivationPending =
        g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
    const bool postSLActiveButUnconfirmed = api == APIType::D3D12 && HookIsPostSLOverlayActiveButUnconfirmed();
    const bool postSLConfirmedRendering = api == APIType::D3D12 && HookIsPostSLOverlayConfirmedRendering();
    const bool postSLConfirmedButStartupSettling =
        api == APIType::D3D12 && HookIsPostSLOverlayConfirmedButStartupSettling();
    const bool hadFSRFGPhase = api == APIType::D3D12 && HookHasFSRFGHistory();
    const bool explicitSetOptionsActivation = api == APIType::D3D12 && HookHasExplicitStreamlineSetOptionsActivation();
    const bool safePostFSRBootstrapPath = api == APIType::D3D12 && HookHasSafePostFSRBootstrapPath();
    const bool steamOverlayLoaded = IsSteamOverlayModule(ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName());
    if (s_externalOverlayPresentInvokeDepth > 0) {
        PFN_Present recursiveBypass = EnsurePresentBypassTrampoline();
        if (DXGIShared::ShouldBypassRecursiveExternalOverlayPresent(true, recursiveBypass != nullptr)) {
            static std::atomic<int> s_recursiveExternalOverlayBypassLogCount{0};
            const int bypassNum =
                s_recursiveExternalOverlayBypassLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (bypassNum <= 10 || (bypassNum % 500) == 0) {
                HookLogImportant(
                    "DetourPresent: Bypassing recursive external-overlay Present #%d while guarded Steam hook is "
                    "active (depth=%d bypass=%p tid=0x%04X)",
                    bypassNum, s_externalOverlayPresentInvokeDepth, (void*)recursiveBypass, currentThreadId);
            }
            return recursiveBypass(pSwapChain, SyncInterval, Flags);
        }
    }
    // Log Steam overlay state once for diagnostics.
    static std::atomic<uint32_t> s_steamStateLogCount{0};
    if (s_steamStateLogCount.fetch_add(1, std::memory_order_relaxed) == 0) {
        const char* overlayModuleName = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
        HookLogImportant(
            "DetourPresent: Steam overlay state: steamLoaded=%d overlayModule=%s g_externalOverlayHook=%p "
            "oPresentTrampoline=%p oPresentBypass=%p slLoaded=%d streamlineFGRunning=%d",
            steamOverlayLoaded ? 1 : 0, overlayModuleName ? overlayModuleName : "none",
            (void*)g_externalOverlayPresentHook, (void*)oPresentTrampoline, (void*)oPresentBypass,
            IsSLInterposerLoaded() ? 1 : 0, g_StreamlineFGRunning.load(std::memory_order_acquire) ? 1 : 0);
    }
    const bool staleThirdPartyPresentHookRisk =
        api == APIType::D3D12 &&
        ShouldForceSteamDX12Bypass(pSwapChain, EnsurePresentBypassTrampoline() != nullptr, IsSLInterposerLoaded());
    const bool observerOnlyMode = HookOverlayObserverOnlyEnabled();
    const bool observerStartupPresentOnlyMode = HookOverlayObserverStartupPresentOnlyEnabled();
    const bool ffxStartupBypass = ShouldBypassFFXPresentDuringStreamlineStartup(
        api == APIType::D3D12, ce::overlay_compat::IsCodeAddressFromFFXFrameGenerationModule(detourCallerAddress),
        streamlineStartupHandoffPending, streamlineStartupTransitionWindowActive, observerOnlyMode,
        observerStartupPresentOnlyMode);
    if (ffxStartupBypass) {
        g_FGCompat.SetFSRFGSupportPresent(true);
        PFN_Present presentBypass = EnsurePresentBypassTrampoline();
        if (presentBypass) {
            static std::atomic<int> s_ffxStartupBypassLogCount{0};
            int bypassNum = s_ffxStartupBypassLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (bypassNum <= 10 || (bypassNum % 200) == 0) {
                HookLogImportant(
                    "DetourPresent: Treating FFX-originated Present as startup handoff bypass #%d (bypass=%p, "
                    "tid=0x%04X)",
                    bypassNum, (void*)presentBypass, GetCurrentThreadId());
            }
            return presentBypass(pSwapChain, SyncInterval, Flags);
        }
    }
    bool streamlineSyntheticReentrant =
        ShouldAllowSpecialStreamlinePresentRouting(observerOnlyMode) &&
        ShouldTreatStreamlinePresentAsSyntheticReentrant(
            api == APIType::D3D12, streamlineFGRunning, callerFromStreamlineModule, postSLConfirmedRendering,
            postSLConfirmedButStartupSettling, streamlineStartupHandoffInProgress, presentOwnershipActive,
            recentLargePresentGap, matchesExpectedPresentThread, startupTopLevelPresentAlreadyConsumed);
    const bool startupTopLevelCandidate = !observerOnlyMode && !streamlineSyntheticReentrant &&
                                          callerFromStreamlineModule && api == APIType::D3D12 && streamlineFGRunning &&
                                          streamlineStartupHandoffInProgress && recentLargePresentGap &&
                                          matchesExpectedPresentThread;
    const bool stalePostFSRStartupHandoffPresentHookRisk =
        api == APIType::D3D12 &&
        ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRStartupHandoff(
            EnsurePresentBypassTrampoline() != nullptr, steamOverlayLoaded, api == APIType::D3D12, inWrapperPresent,
            wrappedSwapchain, hadFSRFGPhase, startupTopLevelCandidate);
    if (startupTopLevelCandidate) {
        bool expected = false;
        if (g_SharedState.streamlineStartupTopLevelPresentConsumed.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
            static std::atomic<int> s_streamlineStartupSuppressedTopLevelLogCount{0};
            int logCount = s_streamlineStartupSuppressedTopLevelLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (logCount <= 10 || (logCount % 100) == 0) {
                HookLogImportant(
                    "DetourPresent: Keeping Streamline startup-handoff Present on the normal SL route #%d "
                    "(owner=0x%04X depth=%d expectedTid=0x%04X currentTid=0x%04X recentGap=1)"
                    " — top-level promotion disabled; relying on startup-policy + wrapper-progress activation",
                    logCount, presentOwner, presentDepthVal, expectedPresentThreadId, currentThreadId);
            }
        }

        if (ShouldBypassPresentForPostFSRStartupHandoffPresentOnNormalRoute(
                api == APIType::D3D12, hadFSRFGPhase, startupTopLevelCandidate,
                staleThirdPartyPresentHookRisk || stalePostFSRStartupHandoffPresentHookRisk)) {
            RefreshLivePresentHooksForSwapchainIfNeeded(pSwapChain, "post-FSR startup-handoff Present");
            HRESULT guardedSteamHr = S_OK;
            if (TryInvokeGuardedExternalSteamOverlayPresent(pSwapChain, SyncInterval, Flags,
                                                            "post-FSR startup-handoff Present", &guardedSteamHr)) {
                return guardedSteamHr;
            }

            PFN_Present presentBypass = EnsurePresentBypassTrampoline();
            if (presentBypass) {
                static std::atomic<int> s_postFSRStartupHandoffBypassLogCount{0};
                int bypassCount = s_postFSRStartupHandoffBypassLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if (bypassCount <= 10 || (bypassCount % 100) == 0) {
                    HookLogImportant(
                        "DetourPresent: Post-FSR startup-handoff normal-route bypass #%d "
                        "(owner=0x%04X depth=%d tid=0x%04X)",
                        bypassCount, presentOwner, presentDepthVal, currentThreadId);
                }
                return presentBypass(pSwapChain, SyncInterval, Flags);
            }
        }
    }
    const bool keepStartupPresentOnNormalRoute = DXGIShared::ShouldKeepSyntheticStartupStreamlinePresentOnNormalRoute(
        observerOnlyMode, hadFSRFGPhase, explicitSetOptionsActivation, safePostFSRBootstrapPath,
        g_SharedState.streamlineStartupTopLevelPresentConsumed.load(std::memory_order_acquire),
        callerFromStreamlineModule, postSLStartupActivationPending, postSLActiveButUnconfirmed,
        postSLConfirmedButStartupSettling, streamlineSyntheticReentrant);
    const bool stalePostFSRStartupNormalRoutePresentHookRisk =
        api == APIType::D3D12 &&
        ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRStartupNormalRoute(
            EnsurePresentBypassTrampoline() != nullptr, steamOverlayLoaded, api == APIType::D3D12, inWrapperPresent,
            wrappedSwapchain, hadFSRFGPhase, keepStartupPresentOnNormalRoute);
    if (keepStartupPresentOnNormalRoute) {
        const bool shouldInvokePostSLCallbackOnNormalRoute =
            DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
                observerOnlyMode, hadFSRFGPhase, explicitSetOptionsActivation, safePostFSRBootstrapPath,
                postSLStartupActivationPending, postSLActiveButUnconfirmed, postSLConfirmedButStartupSettling,
                streamlineSyntheticReentrant);
        static std::atomic<int> s_streamlineSyntheticStartupNormalRouteLogCount{0};
        int logCount = s_streamlineSyntheticStartupNormalRouteLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (logCount <= 10 || (logCount % 100) == 0) {
            HookLogImportant(
                "DetourPresent: Keeping decisive synthetic Streamline startup Present on the normal SL route #%d "
                "(startupPending=%d unconfirmed=%d settling=%d callbackOnNormal=%d consumed=%d hadFSR=%d "
                "windowActive=%d tid=0x%04X)",
                logCount, postSLStartupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
                postSLConfirmedButStartupSettling ? 1 : 0, shouldInvokePostSLCallbackOnNormalRoute ? 1 : 0,
                g_SharedState.streamlineStartupTopLevelPresentConsumed.load(std::memory_order_relaxed) ? 1 : 0,
                hadFSRFGPhase ? 1 : 0, streamlineStartupTransitionWindowActive ? 1 : 0, currentThreadId);
        }
        if (shouldInvokePostSLCallbackOnNormalRoute) {
            auto postSLCallback = g_PostSLOverlayRenderCallback.load(std::memory_order_acquire);
            if (postSLCallback) {
                postSLCallback(pSwapChain);
            }
        } else {
            static std::atomic<int> s_skipPostSLCallbackLogCount{0};
            int skipCount = s_skipPostSLCallbackLogCount.fetch_add(1, std::memory_order_relaxed);
            if (skipCount < 5 || (skipCount % 100) == 0) {
                HookLogImportant(
                    "DetourPresent: PostSL callback skipped on normal route despite startup present kept "
                    "(startupPending=%d unconfirmed=%d hadFSR=%d explicitSetOptions=%d safeBootstrap=%d tid=0x%04X)",
                    postSLStartupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0, hadFSRFGPhase ? 1 : 0,
                    explicitSetOptionsActivation ? 1 : 0, safePostFSRBootstrapPath ? 1 : 0, currentThreadId);
            }
        }
        if (DXGIShared::ShouldBypassPresentWhileKeepingStreamlineStartupPresentOnNormalRoute(
                api == APIType::D3D12, keepStartupPresentOnNormalRoute, hadFSRFGPhase, postSLConfirmedRendering,
                postSLConfirmedButStartupSettling,
                staleThirdPartyPresentHookRisk || stalePostFSRStartupNormalRoutePresentHookRisk)) {
            RefreshLivePresentHooksForSwapchainIfNeeded(pSwapChain, "post-FSR startup normal-route Present");
            HRESULT guardedSteamHr = S_OK;
            if (TryInvokeGuardedExternalSteamOverlayPresent(pSwapChain, SyncInterval, Flags,
                                                            "post-FSR startup normal-route Present",
                                                            &guardedSteamHr)) {
                return guardedSteamHr;
            }

            PFN_Present presentBypass = EnsurePresentBypassTrampoline();
            if (presentBypass) {
                static std::atomic<int> s_postFSRStartupNormalRouteBypassLogCount{0};
                int bypassCount = s_postFSRStartupNormalRouteBypassLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if (bypassCount <= 10 || (bypassCount % 100) == 0) {
                    HookLogImportant(
                        "DetourPresent: Post-FSR startup normal-route bypass #%d "
                        "(startupPending=%d unconfirmed=%d confirmed=%d settling=%d tid=0x%04X)",
                        bypassCount, postSLStartupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
                        postSLConfirmedRendering ? 1 : 0, postSLConfirmedButStartupSettling ? 1 : 0, currentThreadId);
                }
                return presentBypass(pSwapChain, SyncInterval, Flags);
            }
        }
        streamlineSyntheticReentrant = false;
    }
    const bool shouldInvokePostSLCallbackForConfirmedStandaloneNormalRoute =
        DXGIShared::ShouldInvokePostSLCallbackForConfirmedStandaloneStreamlinePresentOnNormalRoute(
            observerOnlyMode, api == APIType::D3D12, streamlineFGRunning, callerFromStreamlineModule,
            postSLConfirmedRendering, postSLConfirmedButStartupSettling, presentOwnershipActive,
            streamlineSyntheticReentrant);
    const bool stalePostFSRConfirmedStandalonePresentHookRisk =
        api == APIType::D3D12 &&
        ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRConfirmedStandaloneNormalRoute(
            EnsurePresentBypassTrampoline() != nullptr, steamOverlayLoaded, api == APIType::D3D12, inWrapperPresent,
            wrappedSwapchain, hadFSRFGPhase, shouldInvokePostSLCallbackForConfirmedStandaloneNormalRoute);
    if (shouldInvokePostSLCallbackForConfirmedStandaloneNormalRoute) {
        auto postSLCallback = g_PostSLOverlayRenderCallback.load(std::memory_order_acquire);
        if (postSLCallback) {
            static std::atomic<int> s_confirmedStandaloneNormalRouteCallbackLogCount{0};
            int logCount = s_confirmedStandaloneNormalRouteCallbackLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (logCount <= 10 || (logCount % 100) == 0) {
                HookLogImportant(
                    "DetourPresent: Invoking PostSL on confirmed standalone Streamline Present while keeping the "
                    "normal "
                    "SL route #%d (settling=%d owner=0x%04X depth=%d tid=0x%04X)",
                    logCount, postSLConfirmedButStartupSettling ? 1 : 0, presentOwner, presentDepthVal,
                    currentThreadId);
            }
            postSLCallback(pSwapChain);
        }

        if (DXGIShared::ShouldBypassPresentForConfirmedStandaloneStreamlinePresentOnNormalRoute(
                api == APIType::D3D12, hadFSRFGPhase, shouldInvokePostSLCallbackForConfirmedStandaloneNormalRoute,
                staleThirdPartyPresentHookRisk || stalePostFSRConfirmedStandalonePresentHookRisk)) {
            RefreshLivePresentHooksForSwapchainIfNeeded(pSwapChain, "post-FSR confirmed standalone Present");
            HRESULT guardedSteamHr = S_OK;
            if (TryInvokeGuardedExternalSteamOverlayPresent(
                    pSwapChain, SyncInterval, Flags, "post-FSR confirmed standalone Present", &guardedSteamHr)) {
                return guardedSteamHr;
            }

            PFN_Present presentBypass = EnsurePresentBypassTrampoline();
            if (presentBypass) {
                static std::atomic<int> s_confirmedStandaloneNormalRouteBypassLogCount{0};
                int bypassCount =
                    s_confirmedStandaloneNormalRouteBypassLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if (bypassCount <= 10 || (bypassCount % 100) == 0) {
                    HookLogImportant(
                        "DetourPresent: Post-FSR confirmed standalone normal-route bypass #%d "
                        "(owner=0x%04X depth=%d tid=0x%04X)",
                        bypassCount, presentOwner, presentDepthVal, currentThreadId);
                }
                return presentBypass(pSwapChain, SyncInterval, Flags);
            }
        }
    }
    if (streamlineSyntheticReentrant) {
        auto postSLCallback =
            observerOnlyMode ? nullptr : g_PostSLOverlayRenderCallback.load(std::memory_order_acquire);
        if (postSLCallback) {
            postSLCallback(pSwapChain);
        }

        if (api == APIType::D3D12) {
            RefreshLivePresentHooksForSwapchainIfNeeded(pSwapChain, "Streamline synthetic Present");
        }

        // Service the deferred ECL probe: ProcessFrame may be dormant during
        // synthetic re-entrant Present routing, so the ProcessFrame-based
        // deferred probe check would never fire here.  The ECL detour also
        // services it, but may not fire if PostSL submits are being skipped.
        DX12_ServiceDeferredECLProbe();

        HRESULT guardedSteamHr = S_OK;
        if (TryInvokeGuardedExternalSteamOverlayPresent(pSwapChain, SyncInterval, Flags,
                                                        "Streamline synthetic Present", &guardedSteamHr)) {
            return guardedSteamHr;
        }

        PFN_Present presentBypass = EnsurePresentBypassTrampoline();
        if (presentBypass) {
            static std::atomic<int> s_streamlineSyntheticPresentLogCount{0};
            int syntheticNum = s_streamlineSyntheticPresentLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (syntheticNum <= 10 || syntheticNum == 50 || (syntheticNum % 500) == 0) {
                HookLogImportant(
                    "DetourPresent: Treating Streamline-originated Present as synthetic re-entrant #%d "
                    "(postSL=%p, bypass=%p, tid=0x%04X)",
                    syntheticNum, (void*)postSLCallback, (void*)presentBypass, GetCurrentThreadId());
            }
            return presentBypass(pSwapChain, SyncInterval, Flags);
        }
    }

    g_SharedState.presentInFlightDepth.fetch_add(1, std::memory_order_acq_rel);
    auto presentInFlightGuard =
        ce::make_scope_guard([]() { g_SharedState.presentInFlightDepth.fetch_sub(1, std::memory_order_acq_rel); });

    if (IsShuttingDown()) {
        if (IsReadableMemory(pSwapChain, sizeof(void*))) {
            return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
        }
        return DXGI_ERROR_INVALID_CALL;
    }

    if (!IsReadableMemory(pSwapChain, sizeof(void*))) {
        RequestHookShutdown();
        return DXGI_ERROR_INVALID_CALL;
    }
    void** vtable = *(void***)pSwapChain;
    if (!vtable || !IsReadableMemory(vtable, 9 * sizeof(void*)) || !vtable[8]) {
        RequestHookShutdown();
        return DXGI_ERROR_INVALID_CALL;
    }

    if (!oPresentTrampoline && !oPresent) {
        HookLog("DetourPresent: No original Present function available");
        return DXGI_ERROR_INVALID_CALL;
    }

    // Apply SetMaximumFrameLatency override (must be BEFORE wrapper/recursive checks)
    ApplyPresentFrameLatencyOverrides(pSwapChain);

    // Query-based CPU prerender limit for D3D11 (fallback when IDXGISwapChain2 is unavailable).
    // Applied for all D3D11 games regardless of wrapper/vtable path.
    if (api == APIType::D3D11 && g_GraphicsOverridesActive.load(std::memory_order_acquire)) {
        float prerenderLimit = GetActivePrerenderLimit();
        if (prerenderLimit >= 0.0f) {
            ApplyPrerenderLimit(pSwapChain, prerenderLimit);
        }
    }

    if (wrappedSwapchain) {
        static int s_wrappedPassCount = 0;
        if (s_wrappedPassCount < 5) {
            s_wrappedPassCount++;
            HookLogImportant("DetourPresent: WRAPPED swapchain early return #%d", s_wrappedPassCount);
        }
        return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
    }

    if (inWrapperPresent) {
        static int s_inWrapperPassCount = 0;
        if (s_inWrapperPassCount < 5) {
            s_inWrapperPassCount++;
            HookLogImportant("DetourPresent: IsInWrapperPresent early return #%d", s_inWrapperPassCount);
        }
        return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
    }

    // Re-entrant Present call. When SL is loaded, calling oPresent enters SL's
    // E9 hook, and SL may call pSwapChain->Present() via the vtable for FG frames.
    // That vtable call hits Steam → DetourPresent → re-entrant. If we forward
    // to oPresent here, it re-enters SL → infinite loop / stack overflow.
    //
    // Solution: use the bypass trampoline which executes original Present bytes
    // from disk, jumping past SL's E9 JMP. This actually presents the frame
    // without re-entering the external hook chain.
    if (IsRecursivePresent()) {
        // DEBUG: Log that we're treating this as recursive
        static std::atomic<int> s_recurseCount{0};
        int rc = s_recurseCount.fetch_add(1, std::memory_order_relaxed);
        if (rc == 0) {
            HookLogImportant("DetourPresent: IsRecursivePresent=TRUE - returning early");
        }

        // Post-SL overlay rendering: when SL FG is active, the overlay is
        // rendered HERE (after SL's FG interpolation), not in ProcessFrame
        // (which runs before SL).  This matches RTSS's approach — overlay
        // appears on both real and interpolated frames without interfering
        // with SL's FG pipeline.
        auto postSLCallback =
            observerOnlyMode ? nullptr : g_PostSLOverlayRenderCallback.load(std::memory_order_acquire);
        if (postSLCallback) {
            postSLCallback(pSwapChain);
        }
        if (api == APIType::D3D12) {
            RefreshLivePresentHooksForSwapchainIfNeeded(pSwapChain, "re-entrant Present");
        }
        static std::atomic<int> s_reentrantLogCount{0};
        int reentrantNum = s_reentrantLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (reentrantNum <= 10 || reentrantNum == 50 || reentrantNum == 100 || (reentrantNum % 500) == 0) {
            HookLogImportant("DetourPresent: Re-entrant #%d (postSL=%p, trampoline=%p, bypass=%p, tid=0x%04X)",
                             reentrantNum, (void*)postSLCallback, (void*)oPresentTrampoline, (void*)oPresentBypass,
                             GetCurrentThreadId());
        }
        if (oPresentTrampoline) {
            return oPresentTrampoline(pSwapChain, SyncInterval, Flags);
        }
        if (oPresentBypass) {
            return oPresentBypass(pSwapChain, SyncInterval, Flags);
        }
        if (reentrantNum <= 10) {
            HookLogImportant("DetourPresent: Re-entrant #%d → S_OK (no bypass trampoline)", reentrantNum);
        }
        return S_OK;
    }
    auto presentDepthGuard = ce::make_scope_guard([]() { ReleasePresent(); });

    // Detect SL's E9 JMP on Present if not already detected. DetectSLPresentHook
    // itself owns the native-FSR suppression rule so the explicit native-FSR OFF
    // teardown window stays protected too.
    if (!s_slRoutingActive.load(std::memory_order_relaxed)) {
        static int s_slCheckCount = 0;
        bool slLoaded = IsSLInterposerLoaded();
        if (s_slCheckCount++ < 10) {
            HookLogImportant("DetourPresent: SL check #%d (slLoaded=%d, oPresent=%p, oPresentTrampoline=%p)",
                             s_slCheckCount, slLoaded ? 1 : 0, oPresent, oPresentTrampoline);
        }
        if (slLoaded) {
            DetectSLPresentHook();
        }
    }

    static int s_processCount = 0;
    if (s_processCount < 5) {
        s_processCount++;
        HookLog("DetourPresent: Processing frame #%d (not wrapped, not in wrapper)", s_processCount);
    }

    // Only send heartbeat if device is healthy — after device removal,
    // suppressing heartbeats lets the freeze watchdog fire and create a dump.
    if (!g_SharedState.deviceRemovedFatal.load(std::memory_order_relaxed))
        g_RenderWatchdog.HeartbeatFromHelperThread();

    // Periodic flush: forward any suppressed slDLSSGSetOptions(OFF) call that was
    // buffered during the DLSS FG startup transition window now that the window
    // has expired.  This ensures the real Streamline runtime eventually receives
    // the OFF signal even if slDLSSGGetState/SetOptions calls are infrequent.
    StreamlineHook::FlushSuppressedSetOptionsOffIfNeeded();

    if (IsVulkanActive()) {
        return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
    }

    // Lazy check for NvPresent64.dll (may load after our hooks)
    g_FGCompat.CheckForNvPresent();

    // NVIDIA Smooth Motion compatibility: skip overlay/processing for invisible
    // windows. NvPresent64 creates invisible-window swapchains for DX11 frame
    // interpolation — processing them corrupts NvPresent64's internal state.
    if (g_FGCompat.IsNvPresentLoaded()) {
        DXGI_SWAP_CHAIN_DESC smDesc = {};
        if (SUCCEEDED(pSwapChain->GetDesc(&smDesc))) {
            if (!smDesc.OutputWindow || !IsWindowVisible(smDesc.OutputWindow)) {
                static int s_smSkipCount = 0;
                if (s_smSkipCount < 5) {
                    s_smSkipCount++;
                    HookLog(
                        "DetourPresent: Skipping invisible window (SM compat, "
                        "hwnd=%p) #%d",
                        smDesc.OutputWindow, s_smSkipCount);
                }
                return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
            }
        }
    }

    bool isFirstHook = !g_SharedState.inPresentHook.exchange(true);
    auto hookGuard = ::ce::make_scope_guard([&] {
        if (isFirstHook)
            g_SharedState.inPresentHook.store(false);
    });

    g_SharedState.presentCallCount.fetch_add(1, std::memory_order_relaxed);

    if (g_SharedState.deviceRemovedFatal.load()) {
        return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
    }

    {
        // Safety: auto-clear swapchainInvalid after 3 seconds if no resize arrives.
        // This prevents permanent overlay death from invalidation without a matching
        // ResizeBuffers (e.g., FG type transitions that don't recreate the swapchain).
        static int64_t s_invalidSinceQpc = 0;
        if (g_SharedState.swapchainInvalid.load(std::memory_order_acquire)) {
            if (s_invalidSinceQpc == 0) {
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                s_invalidSinceQpc = now.QuadPart;
            }
            LARGE_INTEGER now, freq;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&freq);
            double elapsedMs = (double)(now.QuadPart - s_invalidSinceQpc) * 1000.0 / (double)freq.QuadPart;
            if (elapsedMs > 3000.0) {
                HookLogImportant("DetourPresent: swapchainInvalid auto-cleared after %.0fms (no resize arrived)",
                                 elapsedMs);
                g_SharedState.swapchainInvalid.store(false, std::memory_order_release);
                s_invalidSinceQpc = 0;
                // Fall through to normal processing
            } else {
                return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
            }
        } else {
            s_invalidSinceQpc = 0;
        }
    }

    if (api == APIType::D3D12) {
        const char* overlayModule = nullptr;
        int startupPass = 0;
        DX12StartupPresentMode startupMode =
            GetDX12StartupPresentMode(oPresentBypass != nullptr, &overlayModule, &startupPass);
        if (startupMode == DX12StartupPresentMode::kPassThroughOriginal) {
            const bool steamOverlayPresent = IsSteamOverlayModule(overlayModule);
            const bool useBypass = steamOverlayPresent && oPresentBypass && !oPresentTrampoline;
            HookLogImportant("DetourPresent: Startup compatibility pass #%d for third-party overlay %s "
                             "(trampoline=%p bypass=%p steam=%d useBypass=%d)",
                             startupPass, overlayModule ? overlayModule : "module",
                             (void*)oPresentTrampoline, (void*)oPresentBypass,
                             steamOverlayPresent ? 1 : 0, useBypass ? 1 : 0);
            if (g_IPC) {
                g_SharedFpsLimiter.SetIPCClient(g_IPC);
                g_SharedFpsLimiter.Apply();
                ApplyPresentFrameLatencyOverrides(pSwapChain);
            }
            ProcessVSyncOverride(SyncInterval, Flags);
            if (useBypass) {
                return oPresentBypass(pSwapChain, SyncInterval, Flags);
            }
            return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
        }
        const bool knownThirdPartyOverlaySwapchain = DXGIShared::DX12_IsThirdPartyOverlaySwapchain(pSwapChain);
        const bool startupBlockingOverlaySwapchainStillOwnsPresent =
            ce::dx12_overlay_policy::ShouldKeepStartupBlockingOverlaySwapchainBypass(
                knownThirdPartyOverlaySwapchain, DXGIShared::DX12_IsStartupBlockingOverlayTaggedSwapchain(pSwapChain),
                HasStartupBlockingOverlayModuleInCurrentStack());
        if (ce::dx12_overlay_policy::ShouldSkipPresentProcessingForThirdPartyOverlaySwapchain(
                startupBlockingOverlaySwapchainStillOwnsPresent || callerFromThirdPartyOverlay)) {
            static std::atomic<int> s_overlayPresentBypassLogCount{0};
            const int logCount = s_overlayPresentBypassLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 256) == 0) {
                HookLogImportant(
                    "DetourPresent: Bypassing DX12 ProcessFrame for third-party overlay swapchain %p (caller=%s)",
                    pSwapChain, detourCallerModulePath[0] ? detourCallerModulePath : "tracked-overlay-swapchain");
            }
            if (g_IPC) {
                g_SharedFpsLimiter.SetIPCClient(g_IPC);
                g_SharedFpsLimiter.Apply();
                ApplyPresentFrameLatencyOverrides(pSwapChain);
            }
            ProcessVSyncOverride(SyncInterval, Flags);
            return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
        }
    }
    g_SharedState.frameCount.fetch_add(1, std::memory_order_relaxed);
    UpdateDXGIPresentMetricsAndPublish(isFirstHook, "DXGIShared::DetourPresent");

    // Initialize performance metrics for CSV logging early so the scope guard
    // captures total frame time even if HandleDX11/12ProcessFrame or the FPS
    // limiter takes non-trivial time.
    const int64_t perfMetricsQpcUs = PerfLogger::GetQpcUs();
    static uint64_t s_perfFrameNum = 0;
    ++s_perfFrameNum;
    auto perfGuard = ce::make_scope_guard([&]() {
        if (PerfLogger::Get().IsEnabled()) {
            FrameMetrics perfMetrics;
            perfMetrics.qpcUs = perfMetricsQpcUs;
            perfMetrics.totalUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - perfMetricsQpcUs);
            perfMetrics.frameNum = s_perfFrameNum;
            if (api == APIType::D3D12)
                strncpy(perfMetrics.api, "DX12", sizeof(perfMetrics.api) - 1);
            else if (api == APIType::D3D11)
                strncpy(perfMetrics.api, "DX11", sizeof(perfMetrics.api) - 1);
            else
                strncpy(perfMetrics.api, "DXGI", sizeof(perfMetrics.api) - 1);
            perfMetrics.api[sizeof(perfMetrics.api) - 1] = '\0';
            PerfLogger::Get().LogFrame(perfMetrics);
        }
    });

    if (!IsShuttingDown() && (oPresentTrampoline || oPresent)) {
        if (api == APIType::D3D12) {
            HandleDX12ProcessFrame(pSwapChain, true);
        } else if (api == APIType::D3D11) {
            HandleDX11ProcessFrame(pSwapChain, true);
        }
    }

    // FPS Limiter - arm frame pacing before present. Explicit CE-owned Reflex
    // cadence is finished after Present returns so the wait happens before the
    // game starts building the next frame.
    if (g_IPC) {
        g_SharedFpsLimiter.SetIPCClient(g_IPC);
        g_SharedFpsLimiter.Apply(true);
        ApplyPresentFrameLatencyOverrides(pSwapChain);
    }

    ProcessVSyncOverride(SyncInterval, Flags);

    if (api == APIType::D3D12) {
        InvokeDX12WaitForOverlayCompletion(nullptr);
    }

    // CRITICAL: SL startup guard.  During SL DllMain / startup and subsequent
    // SL-originated Present calls, Steam may query Streamline from inside its
    // overlay Present hook.  Once CE has hooked Streamline's plugin lookup, the
    // guarded Steam path can safely return no-op SL callbacks for that
    // re-entrant query. Until then, fall back to the bypass trampoline so the
    // game remains stable.
    if (callerFromStreamlineModule &&
        !s_slRoutingActive.load(std::memory_order_relaxed) &&
        steamOverlayLoaded) {
        HRESULT guardedSteamHr = S_OK;
        if (TryInvokeGuardedExternalSteamOverlayPresent(pSwapChain, SyncInterval, Flags, "SL startup bypass",
                                                        &guardedSteamHr)) {
            if (api == APIType::D3D12) {
                InvokeDX12FlushDeferredSignal();
            }
            if (SUCCEEDED(guardedSteamHr)) {
                g_SharedFpsLimiter.ApplyPostPresent();
            }
            return guardedSteamHr;
        }

        PFN_Present bypass = EnsurePresentBypassTrampoline();
        if (bypass) {
            static std::atomic<int> s_startupBypassCount{0};
            int bypassNum = s_startupBypassCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (bypassNum <= 10 || (bypassNum % 500) == 0) {
                HookLogImportant("DetourPresent: Startup bypass #%d (tid=0x%04X)", bypassNum, GetCurrentThreadId());
            }
            HRESULT bypassHr = bypass(pSwapChain, SyncInterval, Flags);
            if (api == APIType::D3D12) {
                InvokeDX12FlushDeferredSignal();
            }
            if (SUCCEEDED(bypassHr)) {
                g_SharedFpsLimiter.ApplyPostPresent();
            }
            return bypassHr;
        }
    }

    HRESULT hr;
    if (s_slRoutingActive.load(std::memory_order_acquire)) {
        ce::fg_runtime::RuntimeMode runtimeMode = ce::fg_runtime::RuntimeMode::kOff;
        bool runtimeOwnedNativeFGPresentPath = false;
        // Safety: if SL routing is still active while the native FSR path owns
        // presentation, force-disable it. The effective runtime label alone is
        // not sufficient here because the explicit native-FSR OFF teardown window
        // can still be runtime-owned while temporarily publishing Off.
        if (ShouldKeepSLPresentRoutingDisabledNow(&runtimeMode, &runtimeOwnedNativeFGPresentPath)) {
            static int s_fsrlatchCount = 0;
            int latchNum = ++s_fsrlatchCount;
            if (latchNum <= 5) {
                HookLogImportant(
                    "DetourPresent: SL routing was active while native FG still owned Present routing — "
                    "force-disabling SL routing (latch #%d, runtime=%s runtimeOwnedNativeFG=%d). Present will go "
                    "through trampoline directly.",
                    latchNum, ce::fg_runtime::GetRuntimeModeName(runtimeMode), runtimeOwnedNativeFGPresentPath ? 1 : 0);
            }
            s_slRoutingActive.store(false, std::memory_order_release);
            hr = CallOriginalPresent(pSwapChain, SyncInterval, Flags);
        } else {
            // Route through oPresent which has SL's JMP (E9 or FF 25).  This
            // lets SL process FG.  SL's trampoline will re-enter DetourPresent
            // (handled above — forwarded to oPresentTrampoline for real Present).
            static std::atomic<int> s_slCallCount{0};
            int slCallNum = s_slCallCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (slCallNum <= 20 || (slCallNum % 500) == 0) {
                HookLog("DetourPresent: Calling oPresent=%p (SL route, call #%d, tid=0x%04X)", oPresent, slCallNum,
                        GetCurrentThreadId());
            }
            hr = oPresent(pSwapChain, SyncInterval, Flags);
            if (slCallNum <= 20 || (slCallNum % 500) == 0) {
                HookLog("DetourPresent: oPresent returned hr=0x%08X (call #%d)", hr, slCallNum);
            }
        }
    } else {
        static std::atomic<int> s_nonSlPresentCount{0};
        int nonSlNum = s_nonSlPresentCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (nonSlNum == 1 || (nonSlNum % 1000) == 0) {
            HookLog("DetourPresent: non-SL routing path (call #%d, slRouting=%d, tid=0x%04X)", nonSlNum,
                    s_slRoutingActive.load(std::memory_order_relaxed), GetCurrentThreadId());
        }
        hr = CallOriginalPresent(pSwapChain, SyncInterval, Flags);
    }

    // Flush deferred overlay fence Signal AFTER Present.  The NVIDIA driver
    // stalls the GPU when Signal sits between our overlay ECL and Present.
    if (api == APIType::D3D12) {
        InvokeDX12FlushDeferredSignal();
    }

    if (SUCCEEDED(hr)) {
        g_SharedFpsLimiter.ApplyPostPresent();
    }

    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        if (!g_SharedState.deviceRemovedFatal.exchange(true)) {
            HookLog("DXGI: Device removed (hr=0x%08X), disabling hooks", hr);
        }
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE DetourPresent1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                                         const DXGI_PRESENT_PARAMETERS* pPresentParameters) {
    g_PresentCallCounter.fetch_add(1, std::memory_order_relaxed);

    if (!pSwapChain) {
        return DXGI_ERROR_INVALID_CALL;
    }

    const APIType api = DetectAPIType(pSwapChain);
    const void* detourCallerAddress = CE_CAPTURE_RETURN_ADDRESS();
    char detourCallerModulePath[MAX_PATH] = {};
    const bool callerFromThirdPartyOverlay =
        TryGetModulePathFromCodeAddress(detourCallerAddress, detourCallerModulePath, sizeof(detourCallerModulePath)) &&
        ce::overlay_compat::IsThirdPartyOverlayModulePath(detourCallerModulePath);
    const bool streamlineStartupHandoffPending = (api == APIType::D3D12) && IsStreamlineStartupHandoffPending();
    const bool streamlineStartupTransitionWindowActive =
        (api == APIType::D3D12) && IsStreamlineStartupTransitionWindowActive();
    const bool streamlineStartupHandoffInProgress =
        streamlineStartupHandoffPending || streamlineStartupTransitionWindowActive;
    const DWORD currentThreadId = GetCurrentThreadId();
    const bool streamlineFGRunning = g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool wrappedSwapchain = IsWrappedSwapChainObject(pSwapChain);
    const bool inWrapperPresent = IsInWrapperPresent();
    const DWORD presentOwner = g_presentThreadId.load(std::memory_order_relaxed);
    const int presentDepthVal = g_presentDepth.load(std::memory_order_relaxed);
    const bool presentOwnershipActive = presentOwner != 0 || presentDepthVal > 0;
    const DWORD expectedPresentThreadId = g_RenderWatchdog.GetMonitoredThreadId();
    const bool matchesExpectedPresentThread =
        expectedPresentThreadId == 0 || expectedPresentThreadId == currentThreadId;
    const bool callerFromStreamlineModule = IsCodeAddressFromStreamlineModule(detourCallerAddress);
    const bool recentLargePresentGap = HasRecentLargePresentGap(500);
    const bool startupTopLevelPresentAlreadyConsumed =
        g_SharedState.streamlineStartupTopLevelPresentConsumed.load(std::memory_order_acquire);
    const bool postSLStartupActivationPending =
        g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
    const bool postSLActiveButUnconfirmed = api == APIType::D3D12 && HookIsPostSLOverlayActiveButUnconfirmed();
    const bool postSLConfirmedRendering = api == APIType::D3D12 && HookIsPostSLOverlayConfirmedRendering();
    const bool postSLConfirmedButStartupSettling =
        api == APIType::D3D12 && HookIsPostSLOverlayConfirmedButStartupSettling();
    const bool hadFSRFGPhase = api == APIType::D3D12 && HookHasFSRFGHistory();
    const bool explicitSetOptionsActivation = api == APIType::D3D12 && HookHasExplicitStreamlineSetOptionsActivation();
    const bool safePostFSRBootstrapPath = api == APIType::D3D12 && HookHasSafePostFSRBootstrapPath();
    const bool steamOverlayLoaded = IsSteamOverlayModule(ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName());
    const bool staleThirdPartyPresentHookRisk =
        api == APIType::D3D12 &&
        ShouldForceSteamDX12Bypass(pSwapChain, EnsurePresent1BypassTrampoline() != nullptr, IsSLInterposerLoaded());
    const bool observerOnlyMode = HookOverlayObserverOnlyEnabled();
    const bool observerStartupPresentOnlyMode = HookOverlayObserverStartupPresentOnlyEnabled();
    const bool ffxStartupBypass = ShouldBypassFFXPresentDuringStreamlineStartup(
        api == APIType::D3D12, ce::overlay_compat::IsCodeAddressFromFFXFrameGenerationModule(detourCallerAddress),
        streamlineStartupHandoffPending, streamlineStartupTransitionWindowActive, observerOnlyMode,
        observerStartupPresentOnlyMode);
    if (ffxStartupBypass) {
        g_FGCompat.SetFSRFGSupportPresent(true);
        PFN_Present1 present1Bypass = EnsurePresent1BypassTrampoline();
        if (present1Bypass) {
            static std::atomic<int> s_ffxStartupBypassLogCount1{0};
            int bypassNum = s_ffxStartupBypassLogCount1.fetch_add(1, std::memory_order_relaxed) + 1;
            if (bypassNum <= 10 || (bypassNum % 200) == 0) {
                HookLogImportant(
                    "DetourPresent1: Treating FFX-originated Present1 as startup handoff bypass #%d (bypass=%p, "
                    "tid=0x%04X)",
                    bypassNum, (void*)present1Bypass, GetCurrentThreadId());
            }
            return present1Bypass(pSwapChain, SyncInterval, Flags, pPresentParameters);
        }
    }
    bool streamlineSyntheticReentrant =
        ShouldAllowSpecialStreamlinePresentRouting(observerOnlyMode) &&
        ShouldTreatStreamlinePresentAsSyntheticReentrant(
            api == APIType::D3D12, streamlineFGRunning, callerFromStreamlineModule, postSLConfirmedRendering,
            postSLConfirmedButStartupSettling, streamlineStartupHandoffInProgress, presentOwnershipActive,
            recentLargePresentGap, matchesExpectedPresentThread, startupTopLevelPresentAlreadyConsumed);
    const bool startupTopLevelCandidate = !observerOnlyMode && !streamlineSyntheticReentrant &&
                                          callerFromStreamlineModule && api == APIType::D3D12 && streamlineFGRunning &&
                                          streamlineStartupHandoffInProgress && recentLargePresentGap &&
                                          matchesExpectedPresentThread;
    const bool stalePostFSRStartupHandoffPresentHookRisk =
        api == APIType::D3D12 &&
        ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRStartupHandoff(
            EnsurePresent1BypassTrampoline() != nullptr, steamOverlayLoaded, api == APIType::D3D12, inWrapperPresent,
            wrappedSwapchain, hadFSRFGPhase, startupTopLevelCandidate);
    if (startupTopLevelCandidate) {
        bool expected = false;
        if (g_SharedState.streamlineStartupTopLevelPresentConsumed.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
            static std::atomic<int> s_streamlineStartupSuppressedTopLevelLogCount1{0};
            int logCount = s_streamlineStartupSuppressedTopLevelLogCount1.fetch_add(1, std::memory_order_relaxed) + 1;
            if (logCount <= 10 || (logCount % 100) == 0) {
                HookLogImportant(
                    "DetourPresent1: Keeping Streamline startup-handoff Present1 on the normal SL route #%d "
                    "(owner=0x%04X depth=%d expectedTid=0x%04X currentTid=0x%04X recentGap=1)"
                    " — top-level promotion disabled; relying on startup-policy + wrapper-progress activation",
                    logCount, presentOwner, presentDepthVal, expectedPresentThreadId, currentThreadId);
            }
        }

        if (ShouldBypassPresentForPostFSRStartupHandoffPresentOnNormalRoute(
                api == APIType::D3D12, hadFSRFGPhase, startupTopLevelCandidate,
                staleThirdPartyPresentHookRisk || stalePostFSRStartupHandoffPresentHookRisk)) {
            RefreshLivePresentHooksForSwapchainIfNeeded(pSwapChain, "post-FSR startup-handoff Present1");
            PFN_Present1 present1Bypass = EnsurePresent1BypassTrampoline();
            if (present1Bypass) {
                static std::atomic<int> s_postFSRStartupHandoffBypassLogCount1{0};
                int bypassCount = s_postFSRStartupHandoffBypassLogCount1.fetch_add(1, std::memory_order_relaxed) + 1;
                if (bypassCount <= 10 || (bypassCount % 100) == 0) {
                    HookLogImportant(
                        "DetourPresent1: Post-FSR startup-handoff normal-route bypass #%d "
                        "(owner=0x%04X depth=%d tid=0x%04X)",
                        bypassCount, presentOwner, presentDepthVal, currentThreadId);
                }
                return present1Bypass(pSwapChain, SyncInterval, Flags, pPresentParameters);
            }
        }
    }
    const bool keepStartupPresentOnNormalRoute = DXGIShared::ShouldKeepSyntheticStartupStreamlinePresentOnNormalRoute(
        observerOnlyMode, hadFSRFGPhase, explicitSetOptionsActivation, safePostFSRBootstrapPath,
        g_SharedState.streamlineStartupTopLevelPresentConsumed.load(std::memory_order_acquire),
        callerFromStreamlineModule, postSLStartupActivationPending, postSLActiveButUnconfirmed,
        postSLConfirmedButStartupSettling, streamlineSyntheticReentrant);
    const bool stalePostFSRStartupNormalRoutePresentHookRisk =
        api == APIType::D3D12 &&
        ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRStartupNormalRoute(
            EnsurePresent1BypassTrampoline() != nullptr, steamOverlayLoaded, api == APIType::D3D12, inWrapperPresent,
            wrappedSwapchain, hadFSRFGPhase, keepStartupPresentOnNormalRoute);
    if (keepStartupPresentOnNormalRoute) {
        const bool shouldInvokePostSLCallbackOnNormalRoute =
            DXGIShared::ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
                observerOnlyMode, hadFSRFGPhase, explicitSetOptionsActivation, safePostFSRBootstrapPath,
                postSLStartupActivationPending, postSLActiveButUnconfirmed, postSLConfirmedButStartupSettling,
                streamlineSyntheticReentrant);
        static std::atomic<int> s_streamlineSyntheticStartupNormalRouteLogCount1{0};
        int logCount = s_streamlineSyntheticStartupNormalRouteLogCount1.fetch_add(1, std::memory_order_relaxed) + 1;
        if (logCount <= 10 || (logCount % 100) == 0) {
            HookLogImportant(
                "DetourPresent1: Keeping decisive synthetic Streamline startup Present1 on the normal SL route #%d "
                "(startupPending=%d unconfirmed=%d settling=%d callbackOnNormal=%d consumed=%d hadFSR=%d "
                "windowActive=%d tid=0x%04X)",
                logCount, postSLStartupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
                postSLConfirmedButStartupSettling ? 1 : 0, shouldInvokePostSLCallbackOnNormalRoute ? 1 : 0,
                g_SharedState.streamlineStartupTopLevelPresentConsumed.load(std::memory_order_relaxed) ? 1 : 0,
                hadFSRFGPhase ? 1 : 0, streamlineStartupTransitionWindowActive ? 1 : 0, currentThreadId);
        }
        if (shouldInvokePostSLCallbackOnNormalRoute) {
            auto postSLCallback = g_PostSLOverlayRenderCallback.load(std::memory_order_acquire);
            if (postSLCallback) {
                postSLCallback(pSwapChain);
            }
        } else {
            static std::atomic<int> s_skipPostSLCallbackLogCount1{0};
            int skipCount = s_skipPostSLCallbackLogCount1.fetch_add(1, std::memory_order_relaxed);
            if (skipCount < 5 || (skipCount % 100) == 0) {
                HookLogImportant(
                    "DetourPresent1: PostSL callback skipped on normal route despite startup present kept "
                    "(startupPending=%d unconfirmed=%d hadFSR=%d explicitSetOptions=%d safeBootstrap=%d tid=0x%04X)",
                    postSLStartupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0, hadFSRFGPhase ? 1 : 0,
                    explicitSetOptionsActivation ? 1 : 0, safePostFSRBootstrapPath ? 1 : 0, currentThreadId);
            }
        }
        if (DXGIShared::ShouldBypassPresentWhileKeepingStreamlineStartupPresentOnNormalRoute(
                api == APIType::D3D12, keepStartupPresentOnNormalRoute, hadFSRFGPhase, postSLConfirmedRendering,
                postSLConfirmedButStartupSettling,
                staleThirdPartyPresentHookRisk || stalePostFSRStartupNormalRoutePresentHookRisk)) {
            RefreshLivePresentHooksForSwapchainIfNeeded(pSwapChain, "post-FSR startup normal-route Present1");
            PFN_Present1 present1Bypass = EnsurePresent1BypassTrampoline();
            if (present1Bypass) {
                static std::atomic<int> s_postFSRStartupNormalRouteBypassLogCount1{0};
                int bypassCount =
                    s_postFSRStartupNormalRouteBypassLogCount1.fetch_add(1, std::memory_order_relaxed) + 1;
                if (bypassCount <= 10 || (bypassCount % 100) == 0) {
                    HookLogImportant(
                        "DetourPresent1: Post-FSR startup normal-route bypass #%d "
                        "(startupPending=%d unconfirmed=%d confirmed=%d settling=%d tid=0x%04X)",
                        bypassCount, postSLStartupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
                        postSLConfirmedRendering ? 1 : 0, postSLConfirmedButStartupSettling ? 1 : 0, currentThreadId);
                }
                return present1Bypass(pSwapChain, SyncInterval, Flags, pPresentParameters);
            }
        }
        streamlineSyntheticReentrant = false;
    }
    const bool shouldInvokePostSLCallbackForConfirmedStandaloneNormalRoute =
        DXGIShared::ShouldInvokePostSLCallbackForConfirmedStandaloneStreamlinePresentOnNormalRoute(
            observerOnlyMode, api == APIType::D3D12, streamlineFGRunning, callerFromStreamlineModule,
            postSLConfirmedRendering, postSLConfirmedButStartupSettling, presentOwnershipActive,
            streamlineSyntheticReentrant);
    const bool stalePostFSRConfirmedStandalonePresentHookRisk =
        api == APIType::D3D12 &&
        ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRConfirmedStandaloneNormalRoute(
            EnsurePresent1BypassTrampoline() != nullptr, steamOverlayLoaded, api == APIType::D3D12, inWrapperPresent,
            wrappedSwapchain, hadFSRFGPhase, shouldInvokePostSLCallbackForConfirmedStandaloneNormalRoute);
    if (shouldInvokePostSLCallbackForConfirmedStandaloneNormalRoute) {
        auto postSLCallback = g_PostSLOverlayRenderCallback.load(std::memory_order_acquire);
        if (postSLCallback) {
            static std::atomic<int> s_confirmedStandaloneNormalRouteCallbackLogCount1{0};
            int logCount =
                s_confirmedStandaloneNormalRouteCallbackLogCount1.fetch_add(1, std::memory_order_relaxed) + 1;
            if (logCount <= 10 || (logCount % 100) == 0) {
                HookLogImportant(
                    "DetourPresent1: Invoking PostSL on confirmed standalone Streamline Present1 while keeping the "
                    "normal SL route #%d (settling=%d owner=0x%04X depth=%d tid=0x%04X)",
                    logCount, postSLConfirmedButStartupSettling ? 1 : 0, presentOwner, presentDepthVal,
                    currentThreadId);
            }
            postSLCallback(pSwapChain);
        }

        if (DXGIShared::ShouldBypassPresentForConfirmedStandaloneStreamlinePresentOnNormalRoute(
                api == APIType::D3D12, hadFSRFGPhase, shouldInvokePostSLCallbackForConfirmedStandaloneNormalRoute,
                staleThirdPartyPresentHookRisk || stalePostFSRConfirmedStandalonePresentHookRisk)) {
            RefreshLivePresentHooksForSwapchainIfNeeded(pSwapChain, "post-FSR confirmed standalone Present1");
            PFN_Present1 present1Bypass = EnsurePresent1BypassTrampoline();
            if (present1Bypass) {
                static std::atomic<int> s_confirmedStandaloneNormalRouteBypassLogCount1{0};
                int bypassCount =
                    s_confirmedStandaloneNormalRouteBypassLogCount1.fetch_add(1, std::memory_order_relaxed) + 1;
                if (bypassCount <= 10 || (bypassCount % 100) == 0) {
                    HookLogImportant(
                        "DetourPresent1: Post-FSR confirmed standalone normal-route bypass #%d "
                        "(owner=0x%04X depth=%d tid=0x%04X)",
                        bypassCount, presentOwner, presentDepthVal, currentThreadId);
                }
                return present1Bypass(pSwapChain, SyncInterval, Flags, pPresentParameters);
            }
        }
    }
    if (streamlineSyntheticReentrant) {
        auto postSLCallback =
            observerOnlyMode ? nullptr : g_PostSLOverlayRenderCallback.load(std::memory_order_acquire);
        if (postSLCallback) {
            postSLCallback(pSwapChain);
        }

        if (api == APIType::D3D12) {
            RefreshLivePresentHooksForSwapchainIfNeeded(pSwapChain, "Streamline synthetic Present1");
        }

        PFN_Present1 present1Bypass = EnsurePresent1BypassTrampoline();
        if (present1Bypass) {
            static std::atomic<int> s_streamlineSyntheticPresent1LogCount{0};
            int syntheticNum = s_streamlineSyntheticPresent1LogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (syntheticNum <= 10 || syntheticNum == 50 || (syntheticNum % 500) == 0) {
                HookLogImportant(
                    "DetourPresent1: Treating Streamline-originated Present1 as synthetic re-entrant #%d "
                    "(postSL=%p, bypass=%p, tid=0x%04X)",
                    syntheticNum, (void*)postSLCallback, (void*)present1Bypass, GetCurrentThreadId());
            }
            return present1Bypass(pSwapChain, SyncInterval, Flags, pPresentParameters);
        }
    }

    g_SharedState.presentInFlightDepth.fetch_add(1, std::memory_order_acq_rel);
    auto presentInFlightGuard =
        ce::make_scope_guard([]() { g_SharedState.presentInFlightDepth.fetch_sub(1, std::memory_order_acq_rel); });

    if (IsShuttingDown()) {
        if (IsReadableMemory(pSwapChain, sizeof(void*))) {
            return CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
        }
        return DXGI_ERROR_INVALID_CALL;
    }

    if (!IsReadableMemory(pSwapChain, sizeof(void*))) {
        RequestHookShutdown();
        return DXGI_ERROR_INVALID_CALL;
    }
    void** vtable = *(void***)pSwapChain;
    if (!vtable || !IsReadableMemory(vtable, 23 * sizeof(void*)) || !vtable[22]) {
        RequestHookShutdown();
        return DXGI_ERROR_INVALID_CALL;
    }

    if (!oPresent1Trampoline && !oPresent1) {
        return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
    }

    // Apply SetMaximumFrameLatency override (must be BEFORE wrapper/recursive checks)
    ApplyPresentFrameLatencyOverrides(pSwapChain);

    // Query-based CPU prerender limit for D3D11 (fallback when IDXGISwapChain2 is unavailable).
    if (api == APIType::D3D11 && g_GraphicsOverridesActive.load(std::memory_order_acquire)) {
        float prerenderLimit = GetActivePrerenderLimit();
        if (prerenderLimit >= 0.0f) {
            ApplyPrerenderLimit(pSwapChain, prerenderLimit);
        }
    }

    void* pWrapper = nullptr;
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_CWrapDXGISwapChain, &pWrapper))) {
        ((IUnknown*)pWrapper)->Release();
        return CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
    }

    if (IsInWrapperPresent()) {
        return CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
    }

    // Recursive external-overlay Present1 (e.g. Steam overlay called Present1
    // which re-entered through vtable[22]).  Same guard as DetourPresent.
    if (s_externalOverlayPresentInvokeDepth > 0) {
        PFN_Present1 recursiveBypass1 = EnsurePresent1BypassTrampoline();
        if (recursiveBypass1) {
            static std::atomic<int> s_recursiveExternalOverlayPresent1BypassLogCount{0};
            const int bypassNum =
                s_recursiveExternalOverlayPresent1BypassLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (bypassNum <= 10 || (bypassNum % 500) == 0) {
                HookLogImportant(
                    "DetourPresent1: Bypassing recursive external-overlay Present1 #%d while guarded Steam hook is "
                    "active (depth=%d bypass=%p tid=0x%04X)",
                    bypassNum, s_externalOverlayPresentInvokeDepth, (void*)recursiveBypass1, GetCurrentThreadId());
            }
            return recursiveBypass1(pSwapChain, SyncInterval, Flags, pPresentParameters);
        }
    }

    // Re-entrant Present1 call — same logic as DetourPresent.
    if (IsRecursivePresent()) {
        // Post-SL overlay rendering (same as DetourPresent).
        auto postSLCallback =
            observerOnlyMode ? nullptr : g_PostSLOverlayRenderCallback.load(std::memory_order_acquire);
        if (postSLCallback) {
            postSLCallback(pSwapChain);
        }
        if (api == APIType::D3D12) {
            RefreshLivePresentHooksForSwapchainIfNeeded(pSwapChain, "re-entrant Present1");
        }
        static std::atomic<int> s_reentrantLogCount1{0};
        int reentrantNum1 = s_reentrantLogCount1.fetch_add(1, std::memory_order_relaxed) + 1;
        if (reentrantNum1 <= 10 || reentrantNum1 == 50 || reentrantNum1 == 100 || (reentrantNum1 % 500) == 0) {
            HookLog("DetourPresent1: Re-entrant #%d (postSL=%p, trampoline=%p, bypass=%p)", reentrantNum1,
                    (void*)postSLCallback, (void*)oPresent1Trampoline, (void*)oPresent1Bypass);
        }
        if (oPresent1Trampoline) {
            return oPresent1Trampoline(pSwapChain, SyncInterval, Flags, pPresentParameters);
        }
        if (oPresent1Bypass) {
            return oPresent1Bypass(pSwapChain, SyncInterval, Flags, pPresentParameters);
        }
        if (reentrantNum1 <= 10) {
            HookLog("DetourPresent1: Re-entrant #%d → S_OK (no bypass trampoline)", reentrantNum1);
        }
        return S_OK;
    }
    auto presentDepthGuard = ce::make_scope_guard([]() { ReleasePresent(); });

    // Detect SL's E9 JMP if not yet done (same as DetourPresent). DetectSLPresentHook
    // itself owns the native-FSR suppression rule.
    if (!s_slRoutingActive.load(std::memory_order_relaxed) && IsSLInterposerLoaded()) {
        DetectSLPresentHook();
    }

    // Only send heartbeat if device is healthy — after device removal,
    // suppressing heartbeats lets the freeze watchdog fire and create a dump.
    if (!g_SharedState.deviceRemovedFatal.load(std::memory_order_relaxed))
        g_RenderWatchdog.HeartbeatFromHelperThread();

    // Periodic flush: forward any suppressed slDLSSGSetOptions(OFF) call that was
    // buffered during the DLSS FG startup transition window now that the window
    // has expired.
    StreamlineHook::FlushSuppressedSetOptionsOffIfNeeded();

    if (IsVulkanActive()) {
        return CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
    }

    // NVIDIA Smooth Motion compatibility: skip for invisible windows
    if (g_FGCompat.IsNvPresentLoaded()) {
        DXGI_SWAP_CHAIN_DESC smDesc = {};
        if (SUCCEEDED(pSwapChain->GetDesc(&smDesc))) {
            if (!smDesc.OutputWindow || !IsWindowVisible(smDesc.OutputWindow)) {
                return CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
            }
        }
    }

    bool isFirstHook = !g_SharedState.inPresentHook.exchange(true);
    auto hookGuard = ::ce::make_scope_guard([&] {
        if (isFirstHook)
            g_SharedState.inPresentHook.store(false);
    });

    if (g_SharedState.deviceRemovedFatal.load() || g_SharedState.swapchainInvalid.load(std::memory_order_acquire)) {
        return CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
    }

    if (api == APIType::D3D12) {
        const char* overlayModule = nullptr;
        int startupPass = 0;
        DX12StartupPresentMode startupMode =
            GetDX12StartupPresentMode(oPresent1Bypass != nullptr, &overlayModule, &startupPass);
        if (startupMode == DX12StartupPresentMode::kPassThroughOriginal) {
            HookLogImportant("DetourPresent1: Startup compatibility pass #%d for third-party overlay %s", startupPass,
                             overlayModule ? overlayModule : "module");
            if (g_IPC) {
                g_SharedFpsLimiter.SetIPCClient(g_IPC);
                g_SharedFpsLimiter.Apply();
                ApplyPresentFrameLatencyOverrides(pSwapChain);
            }
            ProcessVSyncOverride(SyncInterval, Flags);
            return CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
        }
        const bool knownThirdPartyOverlaySwapchain = DXGIShared::DX12_IsThirdPartyOverlaySwapchain(pSwapChain);
        const bool startupBlockingOverlaySwapchainStillOwnsPresent =
            ce::dx12_overlay_policy::ShouldKeepStartupBlockingOverlaySwapchainBypass(
                knownThirdPartyOverlaySwapchain, DXGIShared::DX12_IsStartupBlockingOverlayTaggedSwapchain(pSwapChain),
                HasStartupBlockingOverlayModuleInCurrentStack());
        if (ce::dx12_overlay_policy::ShouldSkipPresentProcessingForThirdPartyOverlaySwapchain(
                startupBlockingOverlaySwapchainStillOwnsPresent || callerFromThirdPartyOverlay)) {
            static std::atomic<int> s_overlayPresent1BypassLogCount{0};
            const int logCount = s_overlayPresent1BypassLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 256) == 0) {
                HookLogImportant(
                    "DetourPresent1: Bypassing DX12 ProcessFrame for third-party overlay swapchain %p (caller=%s)",
                    pSwapChain, detourCallerModulePath[0] ? detourCallerModulePath : "tracked-overlay-swapchain");
            }
            if (g_IPC) {
                g_SharedFpsLimiter.SetIPCClient(g_IPC);
                g_SharedFpsLimiter.Apply();
                ApplyPresentFrameLatencyOverrides(pSwapChain);
            }
            ProcessVSyncOverride(SyncInterval, Flags);
            return CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
        }
    }
    g_SharedState.frameCount.fetch_add(1, std::memory_order_relaxed);
    UpdateDXGIPresentMetricsAndPublish(isFirstHook, "DXGIShared::DetourPresent1");

    if (api == APIType::D3D12) {
        HandleDX12ProcessFrame(pSwapChain, true);
    } else if (api == APIType::D3D11) {
        HandleDX11ProcessFrame(pSwapChain, true);
    }

    // FPS Limiter - arm frame pacing before present. Explicit CE-owned Reflex
    // cadence is finished after Present returns so the wait happens before the
    // game starts building the next frame.
    if (g_IPC) {
        g_SharedFpsLimiter.SetIPCClient(g_IPC);
        g_SharedFpsLimiter.Apply(true);
        ApplyPresentFrameLatencyOverrides(pSwapChain);
    }

    ProcessVSyncOverride(SyncInterval, Flags);

    if (api == APIType::D3D12) {
        InvokeDX12WaitForOverlayCompletion(nullptr);
    }

    // CRITICAL: SL thread Steam bypass — handled in CallOriginalPresent1.

    HRESULT hr;
    if (s_slRoutingActive.load(std::memory_order_acquire) && oPresent1) {
        ce::fg_runtime::RuntimeMode runtimeMode = ce::fg_runtime::RuntimeMode::kOff;
        bool runtimeOwnedNativeFGPresentPath = false;
        if (ShouldKeepSLPresentRoutingDisabledNow(&runtimeMode, &runtimeOwnedNativeFGPresentPath)) {
            static int s_present1FsrlatchCount = 0;
            int latchNum = ++s_present1FsrlatchCount;
            if (latchNum <= 5) {
                HookLogImportant(
                    "DetourPresent1: SL routing was active while native FG still owned Present routing — "
                    "force-disabling SL routing (latch #%d, runtime=%s runtimeOwnedNativeFG=%d). Present1 will go "
                    "through trampoline directly.",
                    latchNum, ce::fg_runtime::GetRuntimeModeName(runtimeMode), runtimeOwnedNativeFGPresentPath ? 1 : 0);
            }
            s_slRoutingActive.store(false, std::memory_order_release);
            hr = CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
        } else {
            hr = oPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
        }
    } else {
        hr = CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
    }

    // Flush deferred overlay fence Signal AFTER Present.
    if (api == APIType::D3D12) {
        InvokeDX12FlushDeferredSignal();
    }

    if (SUCCEEDED(hr)) {
        g_SharedFpsLimiter.ApplyPostPresent();
    }

    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        if (!g_SharedState.deviceRemovedFatal.exchange(true)) {
            HookLog("DXGI: Device removed (hr=0x%08X), disabling hooks", hr);
        }
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE DetourResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height,
                                              DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    // CRITICAL: Check for global shutdown - if app is closing, don't touch
    // anything
    if (IsShuttingDown()) {
        if (oResizeBuffers) {
            return oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
        }
        return S_OK;
    }

    // Apply backbuffer count override from config
    // When the game calls ResizeBuffers (window resize, alt-tab, resolution change),
    // this ensures our buffer count is applied even if CreateSwapChain override was missed.
    {
        const auto& cfg = GetActiveGraphicsConfig();
        if (HasBackbufferCountOverride(cfg.backbufferCount)) {
            UINT requested = static_cast<UINT>(cfg.backbufferCount);
            if (requested > 0 && requested != BufferCount) {
                // Check swap effect for flip-model safety
                DXGI_SWAP_CHAIN_DESC scDesc = {};
                bool canOverride = true;
                if (SUCCEEDED(pSwapChain->GetDesc(&scDesc))) {
                    bool isFlip = (scDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                                   scDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
                    if (isFlip && requested < BufferCount) {
                        canOverride = false;
                        HookLog("DetourResizeBuffers: Skipping BufferCount override %u < game's %u (flip model)",
                                requested, BufferCount);
                    }
                }
                if (canOverride) {
                    HookLogImportant("DetourResizeBuffers: Overriding BufferCount %u -> %u", BufferCount, requested);
                    BufferCount = requested;
                }
            }
        }
    }

    // CRITICAL FIX: When Vulkan is active, pass through DXGI ResizeBuffers calls
    if (IsVulkanActive()) {
        return oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }

    // AGGRESSIVE RECURSION GUARD: Steam overlay causes infinite recursion through
    // hook chain
    if (IsRecursiveResize()) {
        // Recursion detected - call original directly through vtable to bypass
        // Steam's hook
        void** vtable = *(void***)pSwapChain;
        typedef HRESULT(STDMETHODCALLTYPE * PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
        PFN_ResizeBuffers originalResize = (PFN_ResizeBuffers)vtable[13];  // ResizeBuffers is at index 13
        return originalResize(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }

    if (g_SharedState.wrapperResizeDepth.fetch_add(1) > 0) {
        HRESULT hr = oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
        g_SharedState.wrapperResizeDepth.fetch_sub(1);
        ReleaseResize();
        return hr;
    }

    // Check if this is our wrapper swapchain - if so, skip resize handling
    void* pWrapperTest = nullptr;
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_CWrapDXGISwapChain, &pWrapperTest))) {
        ((IUnknown*)pWrapperTest)->Release();
        HRESULT hr = oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
        g_SharedState.wrapperResizeDepth.fetch_sub(1);
        ReleaseResize();
        return hr;
    }

    g_SharedState.swapchainInvalid.store(true);

    APIType api = DetectAPIType(pSwapChain);

    // CRITICAL FIX: Skip DX12 resize handling during initial swapchain creation
    // Some games call ResizeBuffers immediately after CreateSwapChain
    static std::atomic<int> s_initialResizeCount{0};
    if (api == APIType::D3D12 && s_initialResizeCount.fetch_add(1) == 0) {
        HookLog("DXGI: ResizeBuffers - FIRST D3D12 resize, direct vtable call");
        // Call directly through vtable to bypass any hook chain issues
        void** vtable = *(void***)pSwapChain;
        typedef HRESULT(STDMETHODCALLTYPE * PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
        PFN_ResizeBuffers originalResize = (PFN_ResizeBuffers)vtable[13];
        HRESULT hr = originalResize(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
        HookLog("DXGI: ResizeBuffers - first D3D12 resize returned hr=0x%08X", hr);
        g_SharedState.wrapperResizeDepth.fetch_sub(1);
        ReleaseResize();
        return hr;
    }

    if (api == APIType::D3D12)
        HandleDX12ResizeBegin();
    else if (api == APIType::D3D11)
        HandleDX11ResizeBegin();

    HookLog("DXGI: ResizeBuffers - calling oResizeBuffers...");
    HRESULT hr = oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    HookLog("DXGI: ResizeBuffers - oResizeBuffers returned hr=0x%08X", hr);

    if (FAILED(hr)) {
        HookLog("DXGI: ResizeBuffers FAILED with 0x%08X", hr);
    } else {
        HookLog("DXGI: ResizeBuffers SUCCESS");
    }

    // Reset resize flags after resize completes
    if (api == APIType::D3D12) {
        HookLog("DXGI: ResizeBuffers - calling HandleDX12ResizeEnd...");
        HandleDX12ResizeEnd();
        HookLog("DXGI: ResizeBuffers - HandleDX12ResizeEnd returned");
    }

    g_SharedState.swapchainInvalid.store(false);
    g_SharedState.wrapperResizeDepth.fetch_sub(1);
    ReleaseResize();
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourResizeBuffers1(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height,
                                               DXGI_FORMAT NewFormat, UINT SwapChainFlags,
                                               const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue) {
    // Apply backbuffer count override from config
    {
        const auto& cfg = GetActiveGraphicsConfig();
        if (HasBackbufferCountOverride(cfg.backbufferCount)) {
            UINT requested = static_cast<UINT>(cfg.backbufferCount);
            if (requested > 0 && requested != BufferCount) {
                DXGI_SWAP_CHAIN_DESC scDesc = {};
                bool canOverride = true;
                if (SUCCEEDED(pSwapChain->GetDesc(&scDesc))) {
                    bool isFlip = (scDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                                   scDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
                    if (isFlip && requested < BufferCount) {
                        canOverride = false;
                    }
                }
                if (canOverride) {
                    HookLogImportant("DetourResizeBuffers1: Overriding BufferCount %u -> %u", BufferCount, requested);
                    BufferCount = requested;
                }
            }
        }
    }

    // Vulkan passthrough
    if (IsVulkanActive()) {
        return oResizeBuffers1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags, pCreationNodeMask,
                               ppPresentQueue);
    }

    // AGGRESSIVE RECURSION GUARD: Steam overlay causes infinite recursion through
    // hook chain
    if (IsRecursiveResize()) {
        // Recursion detected - call original directly through vtable to bypass
        // Steam's hook
        void** vtable = *(void***)pSwapChain;
        typedef HRESULT(STDMETHODCALLTYPE * PFN_ResizeBuffers1)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
                                                                const UINT*, IUnknown* const*);
        PFN_ResizeBuffers1 originalResize1 = (PFN_ResizeBuffers1)vtable[39];  // ResizeBuffers1 is at index 39
        return originalResize1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags, pCreationNodeMask,
                               ppPresentQueue);
    }

    if (g_SharedState.wrapperResizeDepth.fetch_add(1) > 0) {
        HRESULT hr = oResizeBuffers1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags,
                                     pCreationNodeMask, ppPresentQueue);
        g_SharedState.wrapperResizeDepth.fetch_sub(1);
        ReleaseResize();
        return hr;
    }

    // Check if this is our wrapper swapchain - if so, skip resize handling
    void* pWrapperTest = nullptr;
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_CWrapDXGISwapChain, &pWrapperTest))) {
        ((IUnknown*)pWrapperTest)->Release();
        HRESULT hr = oResizeBuffers1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags,
                                     pCreationNodeMask, ppPresentQueue);
        g_SharedState.wrapperResizeDepth.fetch_sub(1);
        ReleaseResize();
        return hr;
    }

    g_SharedState.swapchainInvalid.store(true);

    APIType api = DetectAPIType(pSwapChain);

    // CRITICAL FIX: Skip DX12 resize handling during initial swapchain creation
    // Some games call ResizeBuffers immediately after CreateSwapChain
    static std::atomic<int> s_initialResizeCount{0};
    if (api == APIType::D3D12 && s_initialResizeCount.fetch_add(1) == 0) {
        HookLog("DXGI: ResizeBuffers - FIRST D3D12 resize, direct vtable call");
        // Call directly through vtable to bypass any hook chain issues
        void** vtable = *(void***)pSwapChain;
        typedef HRESULT(STDMETHODCALLTYPE * PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
        PFN_ResizeBuffers originalResize = (PFN_ResizeBuffers)vtable[13];
        HRESULT hr = originalResize(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
        HookLog("DXGI: ResizeBuffers - first D3D12 resize returned hr=0x%08X", hr);
        g_SharedState.wrapperResizeDepth.fetch_sub(1);
        ReleaseResize();
        return hr;
    }

    if (api == APIType::D3D12)
        HandleDX12ResizeBegin();
    else if (api == APIType::D3D11)
        HandleDX11ResizeBegin();

    HRESULT hr = oResizeBuffers1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags, pCreationNodeMask,
                                 ppPresentQueue);

    if (FAILED(hr)) {
        HookLog("DXGI: ResizeBuffers1 FAILED with 0x%08X", hr);
    } else {
        HookLog("DXGI: ResizeBuffers1 SUCCESS");
    }

    // Reset resize flags after resize completes
    if (api == APIType::D3D12)
        HandleDX12ResizeEnd();

    g_SharedState.swapchainInvalid.store(false);
    g_SharedState.wrapperResizeDepth.fetch_sub(1);
    ReleaseResize();
    return hr;
}

bool InstallHooks(IDXGISwapChain* pSwapChain, bool presentOnly) {
    // NOTE: This function should only be called for DX11/DX10 games.
    // DX12 games use wrapper-based Present interception (CWrapDXGISwapChain).
    // Calling this for DX12 can cause conflicts and stack overflow crashes
    // due to two competing Present interception mechanisms.
    // See: dx12_hook.cpp for the wrapper-based approach.

    if (!pSwapChain)
        return false;

    static std::atomic<int> s_installCount{0};
    int count = s_installCount.fetch_add(1);
    HookLog("DXGIShared::InstallHooks CALLED #%d (swapchain=%p, presentOnly=%d)", count, pSwapChain,
            presentOnly ? 1 : 0);

    // Third-party overlays can install their own DXGI hooks and form recursive
    // Present chains with vtable patching. In that case the wrapper-based path
    // remains active and avoids hook wars.
    if (!DXGIShared::ShouldInstallSwapchainHooksWithThirdPartyOverlay(IsThirdPartyOverlayLoaded(),
                                                                      HasPresentDetourHooks())) {
        HookLog("DXGIShared::InstallHooks: External overlay detected, skipping DXGI swapchain hooks");
        return true;
    }

    if (s_hookedVTable) {
        void** newVTable = *(void***)pSwapChain;
        if (newVTable == s_hookedVTable) {
            HookLog("DXGIShared::InstallHooks: Hooks already installed on vtable %p", s_hookedVTable);
            return true;
        }
        // New swapchain with a DIFFERENT vtable — need to re-hook.
        HookLogImportant("DXGIShared::InstallHooks: NEW vtable detected (old=%p new=%p) — re-hooking", s_hookedVTable,
                         newVTable);
    }

    void** vtable = *(void***)pSwapChain;
    if (!vtable) {
        HookLog("DXGIShared::InstallHooks: Invalid vtable");
        return false;
    }

    DWORD oldProtect;
    if (!VirtualProtect(vtable, 40 * sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        HookLog("DXGIShared::InstallHooks: VirtualProtect failed");
        return false;
    }

    s_hookedVTable = vtable;

    oPresent = (PFN_Present)vtable[8];
    vtable[8] = (void*)DetourPresent;
    HookLog("DXGIShared: Hooked Present at vtable[8] (original=%p, detour=%p)", oPresent, DetourPresent);

    oPresent1 = (PFN_Present1)vtable[22];
    vtable[22] = (void*)DetourPresent1;
    HookLog("DXGIShared: Hooked Present1 at vtable[22] (original=%p, detour=%p)", oPresent1, DetourPresent1);

    if (!presentOnly) {
        oResizeBuffers = (PFN_ResizeBuffers)vtable[13];
        vtable[13] = (void*)DetourResizeBuffers;
        HookLog(
            "DXGIShared: Hooked ResizeBuffers at vtable[13] (original=%p, "
            "detour=%p)",
            oResizeBuffers, DetourResizeBuffers);

        oResizeBuffers1 = (PFN_ResizeBuffers1)vtable[39];
        vtable[39] = (void*)DetourResizeBuffers1;
        HookLog(
            "DXGIShared: Hooked ResizeBuffers1 at vtable[39] (original=%p, "
            "detour=%p)",
            oResizeBuffers1, DetourResizeBuffers1);
    }

    VirtualProtect(vtable, 40 * sizeof(void*), oldProtect, &oldProtect);
    HookLog("DXGIShared::InstallHooks: All vtable hooks installed successfully");
    return true;
}

bool HasPresentInlineHooks() {
    return oPresentTrampoline != nullptr || oPresent1Trampoline != nullptr;
}

bool HasPresentDetourHooks() {
    return s_hookedVTable != nullptr || oPresentTrampoline != nullptr || oPresent1Trampoline != nullptr;
}

bool CanSafelyInstallExternalPresentDetourPath(bool requiresBypassTrampoline, bool bypassTrampolineAvailable) {
    return !requiresBypassTrampoline || bypassTrampolineAvailable;
}

// Lazy hook installation - installs hooks on first Present if they were
// deferred during swapchain creation
static IDXGISwapChain* s_PendingSwapChainForLazyHook = nullptr;
static std::atomic<bool> s_LazyHooksInstalled{false};

void SetPendingSwapChainForLazyHook(IDXGISwapChain* pSwapChain) {
    if (pSwapChain) {
        pSwapChain->AddRef();
    }
    if (s_PendingSwapChainForLazyHook) {
        s_PendingSwapChainForLazyHook->Release();
    }
    s_PendingSwapChainForLazyHook = pSwapChain;
    HookLog("DXGIShared: SetPendingSwapChainForLazyHook called");
}

static void InstallHooksIfPending(IDXGISwapChain* pSwapChain) {
    if (s_LazyHooksInstalled.load(std::memory_order_acquire))
        return;

    // Check if this is the pending swapchain
    if (pSwapChain == s_PendingSwapChainForLazyHook) {
        HookLog("DXGIShared: Installing hooks lazily on first Present");
        // CRITICAL: Use presentOnly=true to only hook Present/Present1
        // ResizeBuffers hooks can cause stack overflow crashes with some overlays
        InstallHooks(pSwapChain, true);
        s_LazyHooksInstalled.store(true, std::memory_order_release);
        if (s_PendingSwapChainForLazyHook) {
            s_PendingSwapChainForLazyHook->Release();
            s_PendingSwapChainForLazyHook = nullptr;
        }
    }
}

void Init() {
    g_SharedState.lastSwapchainCreation = std::chrono::steady_clock::now();
    // Early detection of NVIDIA Smooth Motion module
    g_FGCompat.CheckForNvPresent();
}

bool InstallPresentInlineHooks(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return false;

    void* presentAddr = GetPresentAddress(pSwapChain);
    void* present1Addr = GetPresent1Address(pSwapChain);

    if (!presentAddr) {
        HookLog("InstallPresentInlineHooks: Failed to get Present address");
        return false;
    }

    // Save original vtable[8] before any modifications. This captures the real
    // COM method (dxgi!CDXGISwapChain::Present or equivalent) from the temp
    // swapchain, before CE patches it to DetourPresent. Used later in
    // CallOriginalPresent and AttemptSteamDX12OverlayInit to ensure DXGI COM
    // method state management runs before dxgi!Present is called with Steam's
    // E9 JMP.
    {
        void** vtable = *(void***)pSwapChain;
        if (vtable && IsReadableMemory(&vtable[8], sizeof(void*))) {
            if (!s_originalVtable8Present) {
                s_originalVtable8Present = (PFN_Present)vtable[8];
                // Log the saved address and compare with GetPresentAddress
                HookLogImportant(
                    "InstallPresentInlineHooks: Saved s_originalVtable8Present=%p from temp swapchain %p "
                    "(presentAddr=%p, same=%d)",
                    (void*)s_originalVtable8Present, (void*)pSwapChain, presentAddr,
                    s_originalVtable8Present == (PFN_Present)presentAddr ? 1 : 0);
                // Log which module presentAddr belongs to for debugging
                HMODULE hAddrModule = nullptr;
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCSTR)presentAddr, &hAddrModule)) {
                    char modulePath[MAX_PATH] = {};
                    GetModuleFileNameA(hAddrModule, modulePath, sizeof(modulePath));
                    HookLogImportant(
                        "InstallPresentInlineHooks: presentAddr=%p is in module: %s",
                        presentAddr, modulePath[0] ? modulePath : "(unknown)");
                }
            }
        } else {
            HookLog("InstallPresentInlineHooks: Cannot read vtable[8] from temp swapchain %p", (void*)pSwapChain);
        }
    }

    static bool s_inlineHooksInstalled = false;
    if (s_inlineHooksInstalled) {
        HookLog("InstallPresentInlineHooks: Inline hooks already installed");
        return true;
    }

    // CRITICAL: Check if an external overlay has already hooked Present
    // External overlays (NVIDIA, Steam, Discord, etc.) actively re-hook Present
    // Fighting them causes a hook war that corrupts the call chain
    const uint8_t* code = (const uint8_t*)presentAddr;
    bool externalJmpDetected = false;

    if (code[0] == 0xE9) {
        // JMP rel32 detected - check where it points
        int32_t disp;
        memcpy(&disp, code + 1, 4);
        uintptr_t jumpTarget = (uintptr_t)(code + 5) + disp;

        // Check if the jump target is outside dxgi.dll
        HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
        if (hDXGI) {
            MODULEINFO dxgiInfo;
            if (GetModuleInformation(GetCurrentProcess(), hDXGI, &dxgiInfo, sizeof(dxgiInfo))) {
                uintptr_t dxgiStart = (uintptr_t)hDXGI;
                uintptr_t dxgiEnd = dxgiStart + dxgiInfo.SizeOfImage;

                if (jumpTarget < dxgiStart || jumpTarget >= dxgiEnd) {
                    externalJmpDetected = true;
                    // JMP points outside dxgi.dll - external overlay detected
                    HookLog("InstallPresentInlineHooks: External overlay detected!");
                    HookLog("InstallPresentInlineHooks: JMP at %p targets %p (outside dxgi.dll %p-%p)", presentAddr,
                            (void*)jumpTarget, (void*)dxgiStart, (void*)dxgiEnd);

                    // Check if the target module is a known overlay
                    HMODULE hTargetModule = nullptr;
                    GetModuleHandleExA(
                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        (LPCSTR)jumpTarget, &hTargetModule);

                    if (hTargetModule) {
                        char moduleName[MAX_PATH] = {0};
                        GetModuleFileNameA(hTargetModule, moduleName, MAX_PATH);
                        HookLog("InstallPresentInlineHooks: External overlay module: %s", moduleName);

                        // Known overlay modules that we should cooperate with
                        std::string moduleLower = moduleName;
                        std::transform(moduleLower.begin(), moduleLower.end(), moduleLower.begin(), ::tolower);

                        if (moduleLower.find("nvidia") != std::string::npos ||
                            moduleLower.find("nvngx") != std::string::npos ||
                            moduleLower.find("steam") != std::string::npos ||
                            moduleLower.find("gameoverlay") != std::string::npos ||
                            moduleLower.find("discord") != std::string::npos ||
                            moduleLower.find("overlay") != std::string::npos) {
                            HookLog("InstallPresentInlineHooks: External hook detected - cooperating (known overlay)");
                        }
                    } else {
                        // Skip inline hooks to prevent prologue corruption (black screen fix)
                        HookLog("InstallPresentInlineHooks: Unknown external JMP - possibly stale hook");
                    }
                }
            }
        }
    }

    if (externalJmpDetected) {
#ifdef _WIN64
        constexpr bool kRequiresBypassTrampolineOnInstall = false;
#else
        constexpr bool kRequiresBypassTrampolineOnInstall = true;
#endif

        void* presentBypass = InlineHook::CreateBypassTrampoline(presentAddr);
        if (!presentBypass) {
            HookLog("InstallPresentInlineHooks: WARNING - Failed to create Present bypass trampoline");
            if (!CanSafelyInstallExternalPresentDetourPath(kRequiresBypassTrampolineOnInstall, false)) {
                HookLogImportant(
                    "InstallPresentInlineHooks: External Present hook detected but no bypass trampoline is available - "
                    "skipping DXGI Present detour path");
                return false;
            }
        }

        void* present1Bypass = nullptr;
        if (present1Addr) {
            present1Bypass = InlineHook::CreateBypassTrampoline(present1Addr);
            if (!present1Bypass &&
                !CanSafelyInstallExternalPresentDetourPath(kRequiresBypassTrampolineOnInstall, false)) {
                HookLogImportant(
                    "InstallPresentInlineHooks: External Present1 hook detected but no bypass trampoline is available "
                    "- skipping DXGI Present detour path");
                return false;
            }
        }

        HookLogImportant("InstallPresentInlineHooks: External E9 JMP detected — using vtable hook path");
        // Save the external overlay hook target (Steam's OverlayHookD3D3) so we
        // can invoke it explicitly later when SL FG routing bypasses Steam's JMP.
        // This is done BEFORE SL overwrites the JMP with its own.
        // If the JMP target is not resolved (e.g. non-E9 JMP or unknown pattern),
        // g_externalOverlayPresentHook stays NULL and Steam overlay will not
        // be explicitly invoked on the forced-bypass path — the overlay module
        // must hook a different Present entry point (e.g. vtable[8] or Present1).
        {
            void* hookTarget = ResolveE9JmpTarget(presentAddr);
            if (hookTarget) {
                g_externalOverlayPresentHook = (PFN_Present)hookTarget;
                HookLog("InstallPresentInlineHooks: External E9 JMP target = %p (saved, Steam overlay hook available)",
                        hookTarget);
            } else {
                HookLogImportant(
                    "InstallPresentInlineHooks: Could not resolve E9 JMP target at %p "
                    "(bytes: %02X %02X %02X %02X %02X) — external overlay hook not saved",
                    presentAddr, ((const uint8_t*)presentAddr)[0], ((const uint8_t*)presentAddr)[1],
                    ((const uint8_t*)presentAddr)[2], ((const uint8_t*)presentAddr)[3],
                    ((const uint8_t*)presentAddr)[4]);
            }
        }

        // External overlay (e.g. Streamline) has hooked Present with an E9 JMP.
        // DO NOT inline-hook the external detour — patching 14 bytes of the
        // external function's prologue corrupts its internal state and crashes
        // under Frame Generation (where more code paths are exercised).
        //
        // Instead, use a clean vtable hook:
        //   Game calls Present → vtable[8] → DetourPresent (our overlay) →
        //   CallOriginalPresent → oPresent (the SL-hooked function) →
        //   SL E9 JMP → SL processes FG normally → real Present
        //
        // Also create bypass trampolines from the original disk bytes so that
        // re-entrant Present calls (from SL's vtable callback) can call the real
        // DXGI Present without re-entering the external E9 JMP hook chain.

        void** vtable = *(void***)pSwapChain;
        if (!vtable) {
            HookLog("InstallPresentInlineHooks: External JMP detected but vtable is null");
            s_inlineHooksInstalled = true;
            return true;
        }

        DWORD oldProtect;
        if (!VirtualProtect(vtable, 40 * sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            HookLog("InstallPresentInlineHooks: VirtualProtect failed for vtable hook");
            s_inlineHooksInstalled = true;
            return true;
        }

        s_hookedVTable = vtable;

        // === STEAM DX12 OVERLAY PRE-INITIALIZATION ===
        //
        // Steam's OverlayHookD3D3 lazily initializes internal rendering
        // callbacks (including a global function pointer at RVA ~0x1621d8 in
        // gameoverlayrenderer64.dll) during its first E9 JMP entry.  CE's vtable
        // hook (setting vtable[8] = DetourPresent below) prevents this natural
        // initialization because Steam's E9 JMP never fires on the game's Present
        // calls — they go through DetourPresent instead of dxgi!Present.
        //
        // Best-effort pre-init: Call Present on the temp swapchain through the
        // REAL dxgi!Present (vtable[8] is still unhooked at this point) BEFORE
        // installing our vtable hook.  This initializes Steam's "next" handler
        // but does NOT initialize the rendering callback at RVA 0x1621d8 —
        // Steam only writes that when rendering on a REAL game swapchain (the
        // 2x2 hidden-window temp swapchain causes Steam to skip full init).
        //
        // The actual fix for the NULL rendering callback is the VEH-protected
        // call in AttemptSteamDX12OverlayInit (see below), which patches the
        // NULL pointer at crash time and lets Steam continue.
        //
        // Thread safety: InstallPresentInlineHooks runs once on the hook thread.
        // The temp swapchain is valid and vtable page is already writable
        // (from VirtualProtect at line ~2843).
        if (externalJmpDetected) {
            const char* overlayModule = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
            if (overlayModule && IsSteamOverlayModule(overlayModule)) {
                HookLogImportant(
                    "InstallPresentInlineHooks: Pre-initializing Steam overlay on temp swapchain %p "
                    "(vtable[8]=%p = dxgi!Present, before CE vtable hook)",
                    pSwapChain, (void*)vtable[8]);

                PFN_Present realPresent = (PFN_Present)vtable[8];
                HRESULT steamInitHr = realPresent(pSwapChain, 0, 0);

                HookLogImportant(
                    "InstallPresentInlineHooks: Steam overlay pre-init on temp swapchain "
                    "returned hr=0x%08X — proceeding with vtable hook installation",
                    (unsigned)steamInitHr);
            }
        }
        // === END STEAM PRE-INIT ===

        oPresent = (PFN_Present)vtable[8];
        vtable[8] = (void*)DetourPresent;
        HookLogImportant(
            "InstallPresentInlineHooks: VTable hook on Present (original=%p, vtable=%p) — "
            "external E9 JMP detected, using non-invasive hook for FG compat",
            oPresent, vtable);

        if (presentBypass) {
            oPresentBypass = (PFN_Present)presentBypass;
            HookLog("InstallPresentInlineHooks: Present bypass trampoline created at %p", presentBypass);
        }

        if (present1Addr) {
            oPresent1 = (PFN_Present1)vtable[22];
            vtable[22] = (void*)DetourPresent1;
            HookLog("InstallPresentInlineHooks: VTable hook on Present1 (original=%p)", oPresent1);

            if (present1Bypass) {
                oPresent1Bypass = (PFN_Present1)present1Bypass;
                HookLog("InstallPresentInlineHooks: Present1 bypass trampoline created at %p", present1Bypass);
            }
        }

        VirtualProtect(vtable, 40 * sizeof(void*), oldProtect, &oldProtect);

        s_inlineHooksInstalled = true;
        return true;
    }

    void* presentTrampoline = nullptr;
    if (!InlineHook::Install(presentAddr, (void*)DetourPresent, &presentTrampoline)) {
        HookLog("InstallPresentInlineHooks: Failed to install Present inline hook");
        return false;
    }
    oPresentTrampoline = (PFN_Present)presentTrampoline;
    oPresent = oPresentTrampoline;
    HookLogImportant(
        "InstallPresentInlineHooks: Present INLINE hook installed (addr=%p, "
        "trampoline=%p) — s_hookedVTable remains %p",
        presentAddr, presentTrampoline, s_hookedVTable);

    if (present1Addr) {
        void* present1Trampoline = nullptr;
        if (InlineHook::Install(present1Addr, (void*)DetourPresent1, &present1Trampoline)) {
            oPresent1Trampoline = (PFN_Present1)present1Trampoline;
            oPresent1 = oPresent1Trampoline;
            HookLog(
                "InstallPresentInlineHooks: Present1 inline hook installed "
                "(addr=%p, trampoline=%p)",
                present1Addr, present1Trampoline);
        }
    }

    s_inlineHooksInstalled = true;
    return true;
}

void RemovePresentHooks() {
    InlineHook::RemoveAll();

    s_slRoutingActive.store(false, std::memory_order_release);
    oPresentBypass = nullptr;
    oPresent1Bypass = nullptr;

    if (!s_hookedVTable)
        return;

    DWORD oldProtect;
    if (oPresent && s_hookedVTable[8] == (void*)DetourPresent) {
        VirtualProtect(&s_hookedVTable[8], sizeof(void*), PAGE_READWRITE, &oldProtect);
        s_hookedVTable[8] = (void*)oPresent;
        VirtualProtect(&s_hookedVTable[8], sizeof(void*), oldProtect, &oldProtect);
        HookLog("DXGIShared: Removed Present vtable hook");
    }

    if (oPresent1 && s_hookedVTable[22] == (void*)DetourPresent1) {
        VirtualProtect(&s_hookedVTable[22], sizeof(void*), PAGE_READWRITE, &oldProtect);
        s_hookedVTable[22] = (void*)oPresent1;
        VirtualProtect(&s_hookedVTable[22], sizeof(void*), oldProtect, &oldProtect);
        HookLog("DXGIShared: Removed Present1 vtable hook");
    }
}

void RepairVTableHooksIfNeeded() {
    // CRITICAL: Do NOT access the swapchain vtable during Streamline's critical
    // initialization window.  Inside Hooked_slDLSSGGetState (called during
    // sl_common!slGetPluginFunction from SL's DllMain), reading the vtable
    // triggers Steam's overlay hook chain (gameoverlayrenderer64!OverlayHookD3D3)
    // which may still be partially initialized and crash with a null function
    // pointer call (RIP=0, RAX=0).  This guard is state-based (PostSL confirmed
    // rendering) rather than timer-based because SL's background DllMain duration
    // varies and can exceed the startup window timer.
    if (g_StreamlineFGRunning.load(std::memory_order_acquire) &&
        !HookIsPostSLOverlayConfirmedRendering()) {
        return;
    }

    if (!s_hookedVTable) {
        static std::atomic<uint32_t> s_nullLogCount{0};
        if (s_nullLogCount.fetch_add(1, std::memory_order_relaxed) < 3) {
            HookLogImportant("DXGIShared: RepairVTable — s_hookedVTable is NULL, cannot repair");
        }
        return;
    }
    if (!IsReadableMemory(s_hookedVTable, 23 * sizeof(void*))) {
        HookLogImportant("DXGIShared: RepairVTable — s_hookedVTable %p not readable", s_hookedVTable);
        return;
    }

    bool repaired = false;
    DWORD oldProtect;

    // Check Present hook at vtable[8]
    if (s_hookedVTable[8] != (void*)DetourPresent) {
        HookLogImportant("DXGIShared: vtable[8] OVERWRITTEN! was=%p expected=%p — re-hooking", s_hookedVTable[8],
                         (void*)DetourPresent);
        oPresent = (PFN_Present)s_hookedVTable[8];
        if (VirtualProtect(&s_hookedVTable[8], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            s_hookedVTable[8] = (void*)DetourPresent;
            VirtualProtect(&s_hookedVTable[8], sizeof(void*), oldProtect, &oldProtect);
            repaired = true;
            HookLogImportant("DXGIShared: vtable[8] re-hooked (new oPresent=%p)", oPresent);
        }
    }

    // Check Present1 hook at vtable[22]
    if (s_hookedVTable[22] != (void*)DetourPresent1) {
        HookLogImportant("DXGIShared: vtable[22] OVERWRITTEN! was=%p expected=%p — re-hooking", s_hookedVTable[22],
                         (void*)DetourPresent1);
        oPresent1 = (PFN_Present1)s_hookedVTable[22];
        if (VirtualProtect(&s_hookedVTable[22], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            s_hookedVTable[22] = (void*)DetourPresent1;
            VirtualProtect(&s_hookedVTable[22], sizeof(void*), oldProtect, &oldProtect);
            repaired = true;
            HookLogImportant("DXGIShared: vtable[22] re-hooked (new oPresent1=%p)", oPresent1);
        }
    }

    static std::atomic<uint32_t> s_intactLogCount{0};
    if (repaired) {
        s_intactLogCount.store(0, std::memory_order_relaxed);
    } else {
        if (s_intactLogCount.fetch_add(1, std::memory_order_relaxed) < 3) {
            HookLogImportant("DXGIShared: RepairVTableHooksIfNeeded — hooks intact (vtable=%p, [8]=%p, [22]=%p)",
                             s_hookedVTable, s_hookedVTable[8], s_hookedVTable[22]);
        }
    }
}

void RemoveSwapchainVTableHooks() {
    InlineHook::RemoveAll();

    s_slRoutingActive.store(false, std::memory_order_release);
    oPresentBypass = nullptr;
    oPresent1Bypass = nullptr;

    if (!s_hookedVTable)
        return;

    DWORD oldProtect;

    if (oPresent && s_hookedVTable[8] == (void*)DetourPresent) {
        VirtualProtect(&s_hookedVTable[8], sizeof(void*), PAGE_READWRITE, &oldProtect);
        s_hookedVTable[8] = (void*)oPresent;
        VirtualProtect(&s_hookedVTable[8], sizeof(void*), oldProtect, &oldProtect);
        HookLog("DXGIShared: Removed Present vtable hook");
    }

    if (oPresent1 && s_hookedVTable[22] == (void*)DetourPresent1) {
        VirtualProtect(&s_hookedVTable[22], sizeof(void*), PAGE_READWRITE, &oldProtect);
        s_hookedVTable[22] = (void*)oPresent1;
        VirtualProtect(&s_hookedVTable[22], sizeof(void*), oldProtect, &oldProtect);
        HookLog("DXGIShared: Removed Present1 vtable hook");
    }

    if (oResizeBuffers && s_hookedVTable[13] == (void*)DetourResizeBuffers) {
        VirtualProtect(&s_hookedVTable[13], sizeof(void*), PAGE_READWRITE, &oldProtect);
        s_hookedVTable[13] = (void*)oResizeBuffers;
        VirtualProtect(&s_hookedVTable[13], sizeof(void*), oldProtect, &oldProtect);
        HookLog("DXGIShared: Removed ResizeBuffers vtable hook");
    }

    if (oResizeBuffers1 && s_hookedVTable[39] == (void*)DetourResizeBuffers1) {
        VirtualProtect(&s_hookedVTable[39], sizeof(void*), PAGE_READWRITE, &oldProtect);
        s_hookedVTable[39] = (void*)oResizeBuffers1;
        VirtualProtect(&s_hookedVTable[39], sizeof(void*), oldProtect, &oldProtect);
        HookLog("DXGIShared: Removed ResizeBuffers1 vtable hook");
    }

    s_hookedVTable = nullptr;
    HookLog("DXGIShared: All swapchain vtable hooks removed");
}

// SAFETY NET: Attempt one-time Steam DX12 overlay initialization.
//
// The PRIMARY fix (InstallPresentInlineHooks) pre-initializes Steam overlay on
// the temp swapchain BEFORE our vtable hook is installed.  This function is a
// fallback for cases where pre-init didn't occur:
//   - Steam overlay loaded AFTER hook installation
//   - Another thread/process context
//
// It temporarily restores vtable[8] to the real dxgi!Present, calls through
// Steam's E9 JMP, then re-hooks vtable[8] to DetourPresent.  With the
// pre-init having already set up Steam's internal rendering callbacks (the
// NULL global function pointer at RVA ~0x1621d8 in gameoverlayrenderer64.dll),
// this E9 JMP call should succeed without crashing.
//
// Thread safety: only one thread wins the compare-exchange.  The brief window
// where vtable[8] is unhooked is microseconds wide and limited to frame 1.
//
// Returns true if this thread performed the init call (result in *resultOut).
// Returns false if another thread won the init race or if init was skipped.
static bool AttemptSteamDX12OverlayInit(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                                         PFN_Present presentOriginal, HRESULT* resultOut) {
    if (!pSwapChain || !resultOut || !s_hookedVTable || !presentOriginal || s_steamInitCrashed) {
        return false;
    }

    if (!IsReadableMemory(s_hookedVTable, 9 * sizeof(void*))) {
        return false;
    }

    // Only one thread wins the init race
    bool expected = false;
    if (!s_steamDX12InitAttempted.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                          std::memory_order_acquire)) {
        return false;  // Another thread is already handling init
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(&s_hookedVTable[8], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        HookLogImportant(
            "AttemptSteamDX12OverlayInit: VirtualProtect failed to unhook vtable[8] — will retry on next frame");
        s_steamDX12InitAttempted.store(false, std::memory_order_release);
        return false;
    }

    // Save current vtable[8] (= DetourPresent) and restore to the real dxgi!Present
    void* savedVtable8 = s_hookedVTable[8];
    s_hookedVTable[8] = (void*)presentOriginal;
    VirtualProtect(&s_hookedVTable[8], sizeof(void*), oldProtect, &oldProtect);

    HookLogImportant(
        "AttemptSteamDX12OverlayInit: vtable[8] temporarily restored to dxgi!Present=%p — "
        "calling through E9 JMP for Steam overlay init (with VEH protection) "
        "[s_originalVtable8Present=%p, same=%d]",
        (void*)presentOriginal, (void*)s_originalVtable8Present,
        s_originalVtable8Present == presentOriginal ? 1 : 0);

    // Call through oPresent (E9 JMP at dxgi!Present) WITH VEH protection.
    //
    // Steam's OverlayHookD3D3 has a NULL internal rendering callback at RVA
    // ~0x1621d8 in gameoverlayrenderer64.dll that causes a RIP=0 crash on first
    // entry through the E9 JMP on a REAL game swapchain (the temp swapchain pre-
    // init in InstallPresentInlineHooks doesn't trigger full initialization because
    // Steam skips rendering on a 2x2 hidden-window swapchain).
    //
    // The SteamOverlayInitVehHandler catches this specific crash (RIP=0, RAX=0,
    // return address inside gameoverlayrenderer64.dll), patches the NULL pointer
    // to SteamDummyRenderingCallback, and retries the `call rax` so Steam
    // completes its initialization.  Steam typically overwrites the dummy with
    // its own real callback during the same E9 JMP entry.
    //
    // If the crash is NOT the expected NULL callback (e.g. a different Steam bug),
    // the handler returns EXCEPTION_CONTINUE_SEARCH and CE's existing VEH crash
    // handler catches it and writes a crash dump.
    PVOID steamInitVehHandle = AddVectoredExceptionHandler(1, SteamOverlayInitVehHandler);
    HRESULT initHr = presentOriginal(pSwapChain, SyncInterval, Flags);
    RemoveVectoredExceptionHandler(steamInitVehHandle);

    // Re-hook vtable[8] with DetourPresent (our vtable hook)
    if (VirtualProtect(&s_hookedVTable[8], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        s_hookedVTable[8] = (void*)DetourPresent;
        VirtualProtect(&s_hookedVTable[8], sizeof(void*), oldProtect, &oldProtect);
    } else {
        // CRITICAL: VirtualProtect for re-hook failed — vtable[8] is exposed.
        // Our DetourPresent hook may be lost. Log prominently and continue.
        HookLogImportant(
            "AttemptSteamDX12OverlayInit: CRITICAL — VirtualProtect failed to re-hook vtable[8]! "
            "CE overlay may be disabled for this session.");
    }

    // Check if Steam overwrote the dummy callback with its real rendering function
    // during the init call.  If so, subsequent frames can route through the E9 JMP
    // path and Steam overlay will render normally.
    {
        HMODULE steamMod = GetModuleHandleW(L"gameoverlayrenderer64.dll");
        if (steamMod) {
            void** steamCallbackPtr = (void**)((uintptr_t)steamMod + 0x1621d8);
            if (IsReadableMemory(steamCallbackPtr, sizeof(void*))) {
                void* callbackAfterInit = *steamCallbackPtr;
                if (callbackAfterInit != nullptr &&
                    callbackAfterInit != (void*)SteamDummyRenderingCallback) {
                    HookLogImportant(
                        "AttemptSteamDX12OverlayInit: Steam overwrote dummy callback (%p) "
                        "with its own function (%p) — not expected for DX12 but logged "
                        "for diagnostics",
                        (void*)SteamDummyRenderingCallback, callbackAfterInit);
                } else {
                    HookLogImportant(
                        "AttemptSteamDX12OverlayInit: Steam callback is still %s (%p) — "
                        "this is expected, the callback is a Vulkan-only function "
                        "(symbol: VulkanSteamOverlayProcessCapturedFrame) irrelevant "
                        "for DX12 overlay rendering",
                        callbackAfterInit == nullptr ? "NULL" : "our no-op",
                        callbackAfterInit ? callbackAfterInit : (void*)SteamDummyRenderingCallback);
                }
            } else {
                HookLog("AttemptSteamDX12OverlayInit: Cannot read Steam callback pointer (not readable)");
            }
        } else {
            HookLog("AttemptSteamDX12OverlayInit: gameoverlayrenderer64.dll not loaded");
        }
    }

    HookLogImportant(
        "AttemptSteamDX12OverlayInit: Steam overlay init completed (hr=0x%08X) — "
        "vtable[8] re-hooked to DetourPresent.  Subsequent frames will invoke Steam "
        "overlay via g_externalOverlayPresentHook (explicit hook target, bypass trampoline fallback).",
        (unsigned)initHr);

    *resultOut = initHr;
    return true;
}

HRESULT CallOriginalPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    if (!pSwapChain) {
        return DXGI_ERROR_INVALID_CALL;
    }

    const PFN_Present presentTrampoline = oPresentTrampoline;
    const PFN_Present presentOriginal = oPresent;
    const PFN_Present presentBypass = EnsurePresentBypassTrampoline();
    bool slLoaded = IsSLInterposerLoaded();

    // Inline-hook path: trampoline always bypasses the detour safely.
    if (presentTrampoline) {
        static int s_copLogCount = 0;
        if (s_copLogCount++ < 5) {
            HookLog("CallOriginalPresent: trampoline path=%p", presentTrampoline);
        }
        return presentTrampoline(pSwapChain, SyncInterval, Flags);
    }

    const char* forcedBypassOverlay = nullptr;
    if (ShouldForceSteamDX12Bypass(pSwapChain, presentBypass != nullptr, slLoaded, &forcedBypassOverlay)) {
        // When Streamline is on the stack, the guarded Steam overlay invoke path
        // is safe because the Streamline plugin-lookup guard prevents Steam's
        // re-entrant slGetPluginFunction queries from returning NULL.
        if (slLoaded) {
            HRESULT guardedSteamHr = S_OK;
            if (TryInvokeGuardedExternalSteamOverlayPresent(pSwapChain, SyncInterval, Flags,
                                                             "Steam DX12 forced bypass", &guardedSteamHr)) {
                return guardedSteamHr;
            }
        } else {
            // Non-Streamline case (e.g. Strange Brigade DX12 with only Steam overlay):
            //
            // Steam's OverlayHookD3D3 has a NULL global function pointer at RVA
            // ~0x1621d8 in gameoverlayrenderer64.dll (internal rendering callback).
            // The temp swapchain pre-init in InstallPresentInlineHooks does NOT
            // initialize this callback (Steam only writes it when rendering on
            // a real game swapchain).
            //
            // AttemptSteamDX12OverlayInit handles the real fix: it temporarily
            // restores vtable[8] to dxgi!Present and calls Steam's E9 JMP with
            // VEH protection.  The VEH handler (SteamOverlayInitVehHandler)
            // catches the NULL callback crash, patches RVA 0x1621d8 to a no-op,
            // and retries the call.  Steam completes its initialization (and
            // typically overwrites the no-op with its own real callback during
            // the same E9 JMP entry).
            //
            // After init, subsequent frames route through oPresent (E9 JMP)
            // normally — Steam's overlay is fully initialized.

            // Phase A: One-time Steam DX12 overlay initialization.
            // If pre-init didn't happen (unusual), AttemptSteamDX12OverlayInit
            // handles it here. Only one thread wins the race; losers bypass.
            if (!s_steamInitCrashed) {
                const bool needInit = !s_steamDX12InitAttempted.load(std::memory_order_acquire);
                if (needInit) {
                    HRESULT initHr = S_OK;
                    if (AttemptSteamDX12OverlayInit(pSwapChain, SyncInterval, Flags,
                                                    presentOriginal, &initHr)) {
                        // This thread performed init successfully
                        return initHr;
                    }
                    // Race loser or init failed — use bypass for this frame.
                    // Next frame will check s_steamDX12InitAttempted and (if another
                    // thread's init succeeded) use the E9 JMP path.
                    static std::atomic<int> s_steamNonSLInitWaitCount{0};
                    if (s_steamNonSLInitWaitCount.fetch_add(1, std::memory_order_relaxed) < 10) {
                        HookLogImportant(
                            "CallOriginalPresent: non-SL Steam overlay — init race loser, "
                            "using bypass trampoline for this frame");
                    }
                } else {
                    // Init already attempted (by another thread or a prior frame).
                    // Steam's overlay init completed successfully (VEH handled the
                    // NULL Vulkan callback at RVA 0x1621d8).
                    //
                    // Route through the explicit Steam overlay invocation
                    // (g_externalOverlayPresentHook = resolved E9 JMP target from
                    // InstallPresentInlineHooks) which calls Steam's OverlayHookD3D3
                    // directly.  Steam renders its overlay, calls its "next" handler
                    // (original dxgi!Present body or re-entrant DetourPresent → bypass
                    // trampoline), and presents the frame normally.
                    //
                    // This replaces the old approach of calling s_originalVtable8Present
                    // (= dxgi!Present with Steam's E9 JMP).  The E9 JMP path produces
                    // black game content in some DX12 games (e.g. Strange Brigade)
                    // because Steam's overlay handler called through the inline JMP on
                    // dxgi!Present triggers a different internal path than when called
                    // as a standalone hook target, leading to incorrect rendering state
                    // for the game content.
                    //
                    // The bypass trampoline (presentBypass) is used as fallback when
                    // the explicit Steam invoke is unavailable or declined, skipping
                    // Steam's overlay rendering for that frame but preserving game
                    // content and CE overlay.
                    // Use the bypass trampoline which calls dxgi!Present+5 directly,
                    // skipping both CE's vtable hook and Steam's E9 JMP.  This is a
                    // safe path that preserves game content and CE overlay.  Steam
                    // overlay is NOT rendered on this path.
                    //
                    // Both the E9 JMP path (calling dxgi!Present with Steam's inline
                    // hook) and the explicit Steam invoke path (g_externalOverlayPresentHook)
                    // produce black game content in Strange Brigade DX12.  The exact
                    // mechanism is not yet determined but is consistent across both
                    // invocation methods.
                    //
                    // TODO: Find a way to render Steam overlay without black screen.
                    // Options:
                    //   - Read dxgi!Present's original vtable[8] COM method from PE
                    //     .rdata and call that instead of the inner function
                    //   - Inline-hook dxgi!Present with chain to Steam's E9 JMP
                    //     (instead of vtable hook), so CE and Steam can coexist in
                    //     the same hook chain without vtable manipulation
                    //   - Call Steam's overlay rendering via a separate device/queue
                    //     that doesn't interfere with the Present call chain
                    if (presentBypass) {
                        static std::atomic<int> s_steamNonSLBypassCount{0};
                        if (s_steamNonSLBypassCount.fetch_add(1, std::memory_order_relaxed) < 10) {
                            HookLogImportant(
                                "CallOriginalPresent: non-SL Steam overlay — using bypass "
                                "trampoline at %p (presentOriginal=%p, init done)",
                                (void*)presentBypass, (void*)presentOriginal);
                        }
                        return presentBypass(pSwapChain, SyncInterval, Flags);
                    }
                }
            }

            // Phase B: Bypass trampoline fallback (safe, no Steam overlay rendering).
            static std::atomic<int> s_steamNonSLFallbackCount{0};
            if (s_steamNonSLFallbackCount.fetch_add(1, std::memory_order_relaxed) < 10) {
                HookLogImportant(
                    "CallOriginalPresent: non-SL Steam overlay — bypass trampoline at %p "
                    "(initAttempted=%d initCrashed=%d)",
                    (void*)presentBypass,
                    s_steamDX12InitAttempted.load(std::memory_order_acquire) ? 1 : 0,
                    s_steamInitCrashed ? 1 : 0);
            }
        }

        static int s_forcedBypassLogCount = 0;
        if (s_forcedBypassLogCount++ < 10) {
            HookLogImportant("CallOriginalPresent: forcing DXGI bypass for %s (slLoaded=%d, bypass=%p)",
                             forcedBypassOverlay ? forcedBypassOverlay : "overlay",
                             slLoaded ? 1 : 0, (void*)presentBypass);
        }
        return presentBypass(pSwapChain, SyncInterval, Flags);
    }

    const char* thirdPartyBypassOverlay = nullptr;
    if (ShouldForceThirdPartyOverlayBypass(pSwapChain, presentBypass != nullptr, &thirdPartyBypassOverlay)) {
        static int s_wrapperBypassLogCount = 0;
        if (s_wrapperBypassLogCount++ < 10) {
            HookLogImportant("CallOriginalPresent: forcing bypass for wrapped present under overlay %s",
                             thirdPartyBypassOverlay);
        }
        return presentBypass(pSwapChain, SyncInterval, Flags);
    }

    // When SL is loaded (vtable hook mode), call oPresent directly.
    // Don't re-read vtable[8] — Steam or other overlays may have re-hooked it
    // after us, which would create a re-entrant loop:
    //   DetourPresent → vtable[8](SteamPresent) → Steam → DetourPresent → ...
    // oPresent = the saved original from vtable[8] at hook install time.
    //
    // SL-originated Steam bypass paths are handled before this fallback by
    // TryInvokeGuardedExternalSteamOverlayPresent. This branch preserves normal
    // vtable-chain behavior for ordinary Present calls.
    if (slLoaded && presentOriginal && presentOriginal != DetourPresent) {
        static std::atomic<int> s_copFastPathCount{0};
        int fastPathNum = s_copFastPathCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (fastPathNum <= 10 || (fastPathNum % 1000) == 0) {
            HookLog("CallOriginalPresent: SL fast-path oPresent=%p (#%d, tid=0x%04X)", presentOriginal,
                    fastPathNum, GetCurrentThreadId());
        }
        return presentOriginal(pSwapChain, SyncInterval, Flags);
    }

    // Prefer the current object's vtable entry when it is not detoured.
    // This avoids mixing wrapper and real swapchain original function pointers.
    if (IsReadableMemory(pSwapChain, sizeof(void*))) {
        void** vtable = *(void***)pSwapChain;
        if (vtable && IsReadableMemory(vtable, 9 * sizeof(void*)) && vtable[8]) {
            auto currentPresent = reinterpret_cast<PFN_Present>(vtable[8]);
            if (currentPresent != DetourPresent) {
                static int s_copLogCount3 = 0;
                if (s_copLogCount3++ < 5) {
                    HookLog("CallOriginalPresent: vtable[8] path=%p (slLoaded=%d, oPresent=%p)", currentPresent,
                            slLoaded, oPresent);
                }
                return currentPresent(pSwapChain, SyncInterval, Flags);
            }
        }
    }

    // Vtable-hook path fallback: use saved original only if it is not detoured.
    // NOTE: presentOriginal is dxgi!Present which may have an external overlay's
    // E9 JMP installed. Calling through it enters the external overlay hook chain
    // (e.g. Steam's gameoverlayrenderer64). This is safe only when the startup
    // compat pass has been blocked (see ShouldAllowDX12StartupPresentPassForState)
    // so that DetourPresent's full routing logic handles re-entrancy.
    if (presentOriginal && presentOriginal != DetourPresent) {
        static int s_copLogCount4 = 0;
        if (s_copLogCount4++ < 5) {
            const char* overlayModule = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
            const bool steamOverlay = IsSteamOverlayModule(overlayModule);
            HookLogImportant(
                "CallOriginalPresent: fallback oPresent=%p (trampoline=%p bypass=%p slLoaded=%d steamOverlay=%d "
                "overlay=%s)",
                presentOriginal, presentTrampoline, presentBypass, slLoaded, steamOverlay ? 1 : 0,
                overlayModule ? overlayModule : "none");
        }
        // When Steam overlay is loaded without Streamline, calling oPresent
        // (dxgi!Present with Steam's E9 JMP) re-enters Steam's overlay handler
        // which crashes because vtable[8] = DetourPresent. Use the bypass
        // trampoline instead to skip all in-memory hooks.
        if (!slLoaded && presentBypass) {
            const char* overlayModule = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
            if (IsSteamOverlayModule(overlayModule)) {
                static int s_steamNonSLBypassCount = 0;
                if (s_steamNonSLBypassCount++ < 10) {
                    HookLogImportant(
                        "CallOriginalPresent: Steam overlay without Streamline — using bypass trampoline %p instead of "
                        "oPresent %p to avoid Steam NULL-callback crash",
                        presentBypass, presentOriginal);
                }
                return presentBypass(pSwapChain, SyncInterval, Flags);
            }
        }
        // Use the saved original COM method (s_originalVtable8Present) if available,
        // which ensures DXGI kernel state management runs before dxgi!Present is
        // called internally with Steam's E9 JMP.  Fall back to presentOriginal
        // (= dxgi!Present inner function with E9 JMP) if the COM method wasn't
        // captured (e.g. non-DX12 paths).
        PFN_Present comTarget = s_originalVtable8Present ?
            s_originalVtable8Present : presentOriginal;
        return comTarget(pSwapChain, SyncInterval, Flags);
    }

    // Last resort: if the bypass trampoline exists but was skipped
    // (ShouldForceSteamDX12Bypass returned false), try it directly.
    // This handles the case where SL DllMain sets runtime mode to
    // DLSSFG before FG actually starts running, causing the bypass
    // check to fail.
    if (presentBypass) {
        static int s_copBypassFallbackCount = 0;
        if (s_copBypassFallbackCount++ < 10) {
            HookLogImportant(
                "CallOriginalPresent: LAST RESORT bypass trampoline at %p (oPresent=%p, oPresentTrampoline=%p, "
                "slLoaded=%d)",
                presentBypass, presentOriginal, presentTrampoline, slLoaded);
        }
        return presentBypass(pSwapChain, SyncInterval, Flags);
    }

    HookLog("CallOriginalPresent: NO PATH AVAILABLE (oPresent=%p, oPresentTrampoline=%p, slLoaded=%d)", presentOriginal,
            presentTrampoline, slLoaded);
    return DXGI_ERROR_INVALID_CALL;
}

HRESULT CallOriginalPresent1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                             const DXGI_PRESENT_PARAMETERS* pParams) {
    if (!pSwapChain) {
        return DXGI_ERROR_INVALID_CALL;
    }

    const PFN_Present1 present1Trampoline = oPresent1Trampoline;
    const PFN_Present1 present1Original = oPresent1;
    const PFN_Present1 present1Bypass = EnsurePresent1BypassTrampoline();
    bool slLoaded = IsSLInterposerLoaded();

    // Inline-hook path: trampoline always bypasses the detour safely.
    if (present1Trampoline) {
        return present1Trampoline(pSwapChain, SyncInterval, Flags, pParams);
    }

    const char* forcedBypassOverlay = nullptr;
    if (ShouldForceSteamDX12Bypass(pSwapChain, present1Bypass != nullptr, slLoaded, &forcedBypassOverlay)) {
        if (slLoaded) {
            // SL case: use bypass trampoline (same as before, no Present1 guard available).
            static int s_forcedBypass1LogCount = 0;
            if (s_forcedBypass1LogCount++ < 10) {
                HookLogImportant("CallOriginalPresent1: forcing DXGI bypass for %s (slLoaded=%d)",
                                 forcedBypassOverlay ? forcedBypassOverlay : "overlay", slLoaded ? 1 : 0);
            }
        } else {
            // Non-Streamline case (e.g. Strange Brigade DX12 with only Steam overlay):
            // Same root cause as CallOriginalPresent: Steam's OverlayHookD3D3
            // needs vtable[8] = dxgi!Present to initialize.  The init is handled
            // by CallOriginalPresent on the first Present call.  For Present1,
            // only route through oPresent1 if Steam init has been completed;
            // otherwise use the bypass trampoline (safe fallback).
            //
            // Steam does NOT hook Present1 with an E9 JMP, so calling
            // present1Original directly on an already-initialized Steam is safe.
            if (!s_steamInitCrashed &&
                s_steamDX12InitAttempted.load(std::memory_order_acquire) &&
                present1Original && present1Original != DetourPresent1 &&
                IsReadableMemory(pSwapChain, sizeof(void*))) {
                static std::atomic<int> s_steamNonSLPresent1ViaE9JmpCount{0};
                if (s_steamNonSLPresent1ViaE9JmpCount.fetch_add(1, std::memory_order_relaxed) < 10) {
                    HookLogImportant(
                        "CallOriginalPresent1: routing non-SL Steam overlay through "
                        "present1Original at %p (Steam init done)",
                        (void*)present1Original);
                }
                return present1Original(pSwapChain, SyncInterval, Flags, pParams);
            }

            static std::atomic<int> s_steamNonSLPresent1FallbackCount{0};
            if (s_steamNonSLPresent1FallbackCount.fetch_add(1, std::memory_order_relaxed) < 10) {
                HookLogImportant(
                    "CallOriginalPresent1: non-SL Steam overlay — Present1 bypass "
                    "(initAttempted=%d initCrashed=%d)",
                    s_steamDX12InitAttempted.load(std::memory_order_acquire) ? 1 : 0,
                    s_steamInitCrashed ? 1 : 0);
            }
        }
        return present1Bypass(pSwapChain, SyncInterval, Flags, pParams);
    }

    const char* thirdPartyBypassOverlay = nullptr;
    if (ShouldForceThirdPartyOverlayBypass(pSwapChain, present1Bypass != nullptr, &thirdPartyBypassOverlay)) {
        static int s_wrapperBypassLogCount = 0;
        if (s_wrapperBypassLogCount++ < 10) {
            HookLogImportant("CallOriginalPresent1: forcing bypass for wrapped present under overlay %s",
                             thirdPartyBypassOverlay);
        }
        return present1Bypass(pSwapChain, SyncInterval, Flags, pParams);
    }

    // CRITICAL: SL worker thread guard — same as CallOriginalPresent.
    // When SL is loaded, call oPresent1 directly (same reason as Present).
    if (slLoaded && present1Original && present1Original != DetourPresent1) {
        return present1Original(pSwapChain, SyncInterval, Flags, pParams);
    }

    // Prefer the current object's Present1 slot when it is not detoured.
    if (IsReadableMemory(pSwapChain, sizeof(void*))) {
        void** vtable = *(void***)pSwapChain;
        if (vtable && IsReadableMemory(vtable, 23 * sizeof(void*)) && vtable[22]) {
            auto currentPresent1 = reinterpret_cast<PFN_Present1>(vtable[22]);
            if (currentPresent1 != DetourPresent1) {
                return currentPresent1(pSwapChain, SyncInterval, Flags, pParams);
            }
        }
    }

    // Vtable-hook path fallback: use saved original only if it is not detoured.
    if (present1Original && present1Original != DetourPresent1) {
        return present1Original(pSwapChain, SyncInterval, Flags, pParams);
    }

    // Last resort: fall back to Present.
    return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
}

void DisableSLPresentRouting() {
    bool wasActive = s_slRoutingActive.exchange(false, std::memory_order_acq_rel);
    if (wasActive) {
        HookLogImportant(
            "SL routing DISABLED: Present calls will bypass SL hook chain and "
            "go through trampoline=%p directly (FSR FG or runtime-owned FG takeover)",
            oPresentTrampoline);
    }
}

}  // namespace DXGIShared
