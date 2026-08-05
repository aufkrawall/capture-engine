#pragma once

namespace DXGIShared {
struct SteamNullCallbackRecoveryContext;
}

namespace DXGIShared {
class ScopedSteamNullCallbackRecoveryGuard;
}

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

namespace DXGIShared {
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
extern SharedState g_SharedState;
}

namespace DXGIShared {
extern std::mutex g_SharedMutex;
}

namespace DXGIShared {
// Present call counter — incremented by DetourPresent and DetourPresent1, read by
// SL hook to detect bypass.
extern std::atomic<uint64_t> g_PresentCallCounter;
}

// Original function pointers
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers1)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
                                                       const UINT*, IUnknown* const*);

typedef HRESULT(STDMETHODCALLTYPE* PFN_SetColorSpace1)(IDXGISwapChain*, DXGI_COLOR_SPACE_TYPE);

namespace DXGIShared {
enum class DX12StartupPresentMode {
    kNone,
    kPassThroughOriginal,
};
}

#if defined(__clang__) || defined(__GNUC__)
#define CE_CAPTURE_RETURN_ADDRESS() __builtin_return_address(0)
#elif defined(_MSC_VER)
#define CE_CAPTURE_RETURN_ADDRESS() _ReturnAddress()
#else
#define CE_CAPTURE_RETURN_ADDRESS() nullptr
#endif

extern bool IsInWrapperPresent();

namespace DXGIShared {
bool QuerySwapChainColorSpace(IDXGISwapChain* swapChain, DXGI_COLOR_SPACE_TYPE& colorSpace);
}

namespace DXGIShared {
bool RecordSwapChainColorSpace(IDXGISwapChain* swapChain, DXGI_COLOR_SPACE_TYPE colorSpace, bool* changed);
}

namespace DXGIShared {
ce::presentation_color::Encoding ResolveSwapChainPresentationEncoding(IDXGISwapChain* swapChain, DXGI_FORMAT format, DXGI_COLOR_SPACE_TYPE* trackedColorSpace, bool* hasTrackedColorSpace);
}

namespace DXGIShared {
void DX12_RegisterThirdPartyOverlaySwapchain(IDXGISwapChain* pSwapChain, const char* creatorModulePath);
}

namespace DXGIShared {
void DX12_UnregisterThirdPartyOverlaySwapchain(IDXGISwapChain* pSwapChain);
}

namespace DXGIShared {
bool DX12_IsThirdPartyOverlaySwapchain(IDXGISwapChain* pSwapChain);
}

namespace DXGIShared {
bool DX12_IsStartupBlockingOverlayTaggedSwapchain(IDXGISwapChain* pSwapChain);
}

namespace DXGIShared {
void BeginPostSLOffKeepAlivePresentScope();
}

namespace DXGIShared {
void EndPostSLOffKeepAlivePresentScope();
}

namespace DXGIShared {
void MarkPostSLOffKeepAlivePrePresentDrawn();
}

namespace DXGIShared {
bool WasPostSLOffKeepAlivePrePresentDrawn();
}

namespace DXGIShared {
APIType DetectAPIType(IDXGISwapChain* pSwapChain);
}

namespace DXGIShared {
bool IsVulkanPrimary();
}

namespace DXGIShared {
void WaitBackbufferFrameLatency(IDXGISwapChain* pSwapChain);
}

namespace DXGIShared {
void ApplyPresentFrameLatencyOverrides(IDXGISwapChain* pSwapChain);
}

namespace DXGIShared {
PerformanceMetrics* GetPerformanceMetrics();
}

namespace DXGIShared {
uint32_t GetLatestSourceFrameIndex();
}

namespace DXGIShared {
void SetLatestSourceFrameIndex(uint32_t frameIndex);
}

namespace DXGIShared {
APIType DetectAPIType(IDXGISwapChain* pSwapChain);
}

namespace DXGIShared {
HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
}

namespace DXGIShared {
HRESULT STDMETHODCALLTYPE DetourPresent1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* pPresentParameters);
}

namespace DXGIShared {
bool IsDlssToggleEagerOverlayEnabled();
}

namespace DXGIShared {
HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
}

namespace DXGIShared {
HRESULT STDMETHODCALLTYPE DetourPresent1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* pPresentParameters);
}

namespace DXGIShared {
HRESULT STDMETHODCALLTYPE DetourResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);
}

namespace DXGIShared {
HRESULT STDMETHODCALLTYPE DetourResizeBuffers1(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags, const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue);
}

namespace DXGIShared {
HRESULT STDMETHODCALLTYPE DetourSetColorSpace1(IDXGISwapChain* pSwapChain, DXGI_COLOR_SPACE_TYPE colorSpace);
}

namespace DXGIShared {
HRESULT SetSwapChainColorSpaceFromWrapper(IDXGISwapChain3* callableSwapChain, IDXGISwapChain* identitySwapChain, DXGI_COLOR_SPACE_TYPE colorSpace);
}

namespace DXGIShared {
bool InstallHooks(IDXGISwapChain* pSwapChain, bool presentOnly);
}

namespace DXGIShared {
bool HasPresentInlineHooks();
}

namespace DXGIShared {
bool HasPresentDetourHooks();
}

namespace DXGIShared {
bool CanSafelyInstallExternalPresentDetourPath(bool requiresBypassTrampoline, bool bypassTrampolineAvailable);
}

namespace DXGIShared {
void SetPendingSwapChainForLazyHook(IDXGISwapChain* pSwapChain);
}

namespace DXGIShared {
void Init();
}

namespace DXGIShared {
bool InstallPresentInlineHooks(IDXGISwapChain* pSwapChain);
}

namespace DXGIShared {
void RemovePresentHooks();
}

namespace DXGIShared {
void ReleaseSwapchainPresentVTableHooksForRuntimeHandoff(const char* reason);
}

namespace DXGIShared {
void RepairVTableHooksIfNeeded();
}

namespace DXGIShared {
void RemoveSwapchainVTableHooks();
}

namespace DXGIShared {
HRESULT CallOriginalPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
}

namespace DXGIShared {
HRESULT CallOriginalPresent1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* pParams);
}

namespace DXGIShared {
void DisableSLPresentRouting();
}

// Put shutdown check outside the DXGIShared namespace
inline bool IsShuttingDown() {
    return HookIsShuttingDown();
}

namespace DXGIShared {
// Global metrics for DXGI-based APIs
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
inline PerformanceMetrics dxgi_shared_g_DXGIPerfMetrics;
}

namespace DXGIShared {
// Recursion detection globals (avoiding thread_local which requires runtime
// init)
inline std::atomic<DWORD> dxgi_shared_g_presentThreadId{0};
}

namespace DXGIShared {
inline std::atomic<int> dxgi_shared_g_presentDepth{0};
}

namespace DXGIShared {
// Helper to check if we're recursively entering from the same thread
inline bool IsRecursivePresent() {
    DWORD currentId = GetCurrentThreadId();
    DWORD expected = dxgi_shared_g_presentThreadId.load(std::memory_order_acquire);

    // Fast path: same thread re-entering (recursion)
    if (expected == currentId && dxgi_shared_g_presentDepth.load(std::memory_order_acquire) > 0) {
        return true;
    }

    // Try to claim ownership atomically — only one thread can succeed
    DWORD zero = 0;
    if (dxgi_shared_g_presentThreadId.compare_exchange_strong(zero, currentId, std::memory_order_acq_rel)) {
        dxgi_shared_g_presentDepth.store(1, std::memory_order_release);
        return false;
    }

    // Another thread owns it — if it's us (race between check and CAS), re-check
    if (dxgi_shared_g_presentThreadId.load(std::memory_order_acquire) == currentId) {
        dxgi_shared_g_presentDepth.fetch_add(1, std::memory_order_acq_rel);
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
}

namespace DXGIShared {
inline void ReleasePresent() {
    if (dxgi_shared_g_presentDepth.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        dxgi_shared_g_presentThreadId.store(0, std::memory_order_release);
    }
}
}

namespace DXGIShared {
inline bool ShouldBypassDX12InvisibleWindowPresent(IDXGISwapChain* pSwapChain, const char* presentName) {
    if (!pSwapChain) {
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc = {};
    if (FAILED(pSwapChain->GetDesc(&desc))) {
        return false;
    }

    const bool hasOutputWindow = desc.OutputWindow != nullptr;
    const bool outputWindowVisible = hasOutputWindow && IsWindowVisible(desc.OutputWindow) != FALSE;
    if (!ce::dx12_overlay_policy::ShouldSkipDX12PresentProcessingForInvisibleWindowSwapchain(hasOutputWindow,
                                                                                             outputWindowVisible)) {
        return false;
    }

    static std::atomic<int> s_invisibleWindowPresentBypassLogCount{0};
    const int logCount = s_invisibleWindowPresentBypassLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 256) == 0) {
        HookLogImportant(
            "%s: Bypassing CE DX12 Present processing for invisible-window helper swapchain "
            "(sc=%p hwnd=%p size=%ux%u count=%d)",
            presentName ? presentName : "DetourPresent", pSwapChain, desc.OutputWindow, desc.BufferDesc.Width,
            desc.BufferDesc.Height, logCount + 1);
    }
    return true;
}
}

namespace DXGIShared {
inline PFN_Present dxgi_shared_oPresent = nullptr;
}

namespace DXGIShared {
inline PFN_Present1 dxgi_shared_oPresent1 = nullptr;
}

namespace DXGIShared {
// Inline hook trampolines - calling these bypasses the hook entirely
inline PFN_Present dxgi_shared_oPresentTrampoline = nullptr;
}

namespace DXGIShared {
inline PFN_Present1 dxgi_shared_oPresent1Trampoline = nullptr;
}

namespace DXGIShared {
// Bypass trampolines — skip external E9/FF25 hooks (e.g. Streamline) at the
// function entry point by executing original prologue bytes read from disk.
// Used in re-entrant Present calls to actually present the frame without
// re-entering the external hook chain.
inline PFN_Present dxgi_shared_oPresentBypass = nullptr;
}

namespace DXGIShared {
inline PFN_Present1 dxgi_shared_oPresent1Bypass = nullptr;
}

namespace DXGIShared {
// Saved target of the external E9 JMP on dxgi!Present, installed by Steam overlay
// (gameoverlayrenderer64!OverlayHookD3D3).  Captured during InstallPresentInlineHooks
// BEFORE Streamline overwrites it with its own JMP.  CE may invoke this target
// from SL-originated Present stacks only while the Streamline plugin-lookup guard
// is active; otherwise those paths use the bypass trampoline.
inline PFN_Present dxgi_shared_g_externalOverlayPresentHook = nullptr;
}

namespace DXGIShared {
inline thread_local int dxgi_shared_s_externalOverlayPresentInvokeDepth = 0;
}

namespace DXGIShared {
// Stored vtable pointer for unhooking Present when COM wrapper takes over
inline void** dxgi_shared_s_hookedVTable = nullptr;
}

namespace DXGIShared {
// Saved original vtable[8] Present COM method captured from the temp swapchain
// at InstallPresentInlineHooks time, before any vtable modifications.  This is
// the real IDXGISwapChain::Present COM method (dxgi!CDXGISwapChain::Present or
// equivalent), not the inner dxgi!Present function that Steam hooks with an E9
// JMP.  Used in the E9 JMP path of CallOriginalPresent and
// AttemptSteamDX12OverlayInit to ensure DXGI COM method state management runs
// before dxgi!Present is called with Steam's E9 JMP.  Without this, calling
// dxgi!Present directly skips COM state management, which causes black screen
// on some DX12 games (e.g. Strange Brigade).
inline PFN_Present dxgi_shared_s_originalVtable8Present = nullptr;
}

namespace DXGIShared {
// State for one-time Steam DX12 overlay initialization.
// Steam's OverlayHookD3D3 lazily initializes its internal "next" Present handler
// on first E9 JMP entry by reading vtable[8].  When vtable[8] = DetourPresent
// (our vtable hook), Steam's init fails and sets "next" = NULL, causing RIP=0.
//
// Fix: temporarily restore vtable[8] to the original dxgi!Present on the very
// first non-SL Steam overlay Present call, allowing Steam's init to complete.
// Re-hook vtable[8] to DetourPresent after Steam returns.
inline std::atomic<bool> dxgi_shared_s_steamDX12InitAttempted{false};
}

namespace DXGIShared {
inline bool dxgi_shared_s_steamInitCrashed = false;
}

namespace DXGIShared {
// Fallback only: Steam's NULL Present-shaped callbacks should normally be
// patched to CE's DXGI bypass trampoline so Steam can keep chaining to a real
// Present.  This no-op is used only when a bypass trampoline is not available.
inline HRESULT WINAPI SteamDummyRenderingCallback(IDXGISwapChain* /*pSwapChain*/, UINT /*SyncInterval*/,
                                                  UINT /*Flags*/) {
    return S_OK;
}
}

namespace DXGIShared {
struct SteamNullCallbackRecoveryContext {
    const char* context = "unknown";
    const char* reason = nullptr;
    void* hook = nullptr;
    void* bypass = nullptr;
    bool streamlineStackActive = false;
    bool pluginLookupGuardReady = false;
};
}

namespace DXGIShared {
inline thread_local SteamNullCallbackRecoveryContext dxgi_shared_s_steamNullCallbackRecoveryContext;
}

namespace DXGIShared {
// Forward declaration (defined at line ~817)
inline bool IsReadableMemory(const void* ptr, size_t size);
}

namespace DXGIShared {
inline void* SelectSteamNullCallbackRecoveryTarget(const SteamNullCallbackRecoveryContext& recoveryContext) {
    const bool hasBypass = recoveryContext.bypass != nullptr;
    return DXGIShared::SelectSteamNullCallbackRecoveryPatchTarget(hasBypass) ==
                   DXGIShared::SteamNullCallbackRecoveryPatchTarget::DXGIBypassPresent
               ? recoveryContext.bypass
               : reinterpret_cast<void*>(SteamDummyRenderingCallback);
}
}

namespace DXGIShared {
inline void** ResolveSteamNullCallbackSlotFromFault(uintptr_t returnAddress, uintptr_t steamStart, uintptr_t steamEnd) {

#ifdef _WIN64
    if (returnAddress < steamStart + 2 || returnAddress >= steamEnd) {
        return nullptr;
    }

    const uintptr_t callAddress = returnAddress - 2;
    const auto* callBytes = reinterpret_cast<const uint8_t*>(callAddress);
    if (!IsReadableMemory(callBytes, 2) || callBytes[0] != 0xFF || callBytes[1] != 0xD0) {
        return nullptr;
    }

    const uintptr_t scanStart = (callAddress > steamStart + 64) ? callAddress - 64 : steamStart;
    for (uintptr_t instr = callAddress; instr >= scanStart; --instr) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(instr);
        if (!IsReadableMemory(bytes, 7)) {
            if (instr == scanStart) {
                break;
            }
            continue;
        }

        if (bytes[0] == 0x48 && bytes[1] == 0x8B && bytes[2] == 0x05) {
            int32_t disp = 0;
            memcpy(&disp, bytes + 3, sizeof(disp));
            auto** slot = reinterpret_cast<void**>(instr + 7 + disp);
            const uintptr_t slotAddress = reinterpret_cast<uintptr_t>(slot);
            if (slotAddress >= steamStart && slotAddress + sizeof(void*) <= steamEnd &&
                IsReadableMemory(reinterpret_cast<const void*>(slot), sizeof(void*)) && *slot == nullptr) {
                return slot;
            }
        }

        if (instr == scanStart) {
            break;
        }
    }
#else
    if (returnAddress < steamStart + 2 || returnAddress >= steamEnd) {
        return nullptr;
    }

    const uintptr_t callAddress = returnAddress - 2;
    const auto* callBytes = reinterpret_cast<const uint8_t*>(callAddress);
    if (!IsReadableMemory(callBytes, 2) || callBytes[0] != 0xFF || callBytes[1] != 0xD0) {
        return nullptr;
    }

    const uintptr_t scanStart = (callAddress > steamStart + 32) ? callAddress - 32 : steamStart;
    for (uintptr_t instr = callAddress; instr >= scanStart; --instr) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(instr);
        uintptr_t slotAddress = 0;
        if (IsReadableMemory(bytes, 5) && bytes[0] == 0xA1) {
            uint32_t absolute = 0;
            memcpy(&absolute, bytes + 1, sizeof(absolute));
            slotAddress = absolute;
        } else if (IsReadableMemory(bytes, 6) && bytes[0] == 0x8B && bytes[1] == 0x05) {
            uint32_t absolute = 0;
            memcpy(&absolute, bytes + 2, sizeof(absolute));
            slotAddress = absolute;
        }

        auto** slot = reinterpret_cast<void**>(slotAddress);
        if (slotAddress >= steamStart && slotAddress + sizeof(void*) <= steamEnd &&
            IsReadableMemory(slot, sizeof(void*)) && *slot == nullptr) {
            return slot;
        }

        if (instr == scanStart) {
            break;
        }
    }
#endif

    return nullptr;
}
}

namespace DXGIShared {
// VEH handler: catches Steam's NULL rendering callback crash during the
// one-time init and guarded Present paths.  It resolves the exact Steam global
// slot that supplied NULL to `call (e)ax`, patches that slot to CE's DXGI
// bypass Present when possible, and retries the call so Steam can keep its own
// overlay chain alive.
//
// Architecture notes:
//   x64: returnAddr from [RSP], RIP, RAX, RSP, call rax = FF D0 (2 bytes)
//   x86: returnAddr from [ESP], EIP, EAX, ESP, call eax = FF D0 (2 bytes)
//   Steam module: x64=gameoverlayrenderer64.dll, x86=gameoverlayrenderer.dll
//   Legacy fallback RVA: x64=0x1621d8. Newer Steam builds can use nearby slots;
//   the handler first resolves the slot dynamically from the faulting mov/call.
inline LONG CALLBACK SteamOverlayInitVehHandler(PEXCEPTION_POINTERS ep) {
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

    const SteamNullCallbackRecoveryContext recoveryContext = dxgi_shared_s_steamNullCallbackRecoveryContext;
    const void* patchTarget = SelectSteamNullCallbackRecoveryTarget(recoveryContext);
    void** nullFnPtr = ResolveSteamNullCallbackSlotFromFault(returnAddress, steamStart, steamEnd);
    const bool dynamicallyResolvedSlot = nullFnPtr != nullptr;
    if (!nullFnPtr) {
        nullFnPtr = reinterpret_cast<void**>(steamStart + kSteamCallbackRva);
    }
    const uintptr_t resolvedRva = reinterpret_cast<uintptr_t>(nullFnPtr) - steamStart;
    void* callbackBefore = nullptr;
    const bool callbackSlotReadable = IsReadableMemory(reinterpret_cast<const void*>(nullFnPtr), sizeof(void*));
    if (callbackSlotReadable) {
        callbackBefore = *nullFnPtr;
    }
    bool patched = false;
    if (callbackSlotReadable && callbackBefore == nullptr) {
        DWORD oldProtect;
        if (VirtualProtect(reinterpret_cast<void*>(nullFnPtr), sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            *nullFnPtr = const_cast<void*>(patchTarget);
            VirtualProtect(reinterpret_cast<void*>(nullFnPtr), sizeof(void*), oldProtect, &oldProtect);
            patched = true;
            HookLogImportant(
                "SteamOverlayInitVehHandler: Patched NULL callback at %p (steam+0x%zX) "
                "-> %s=%p context=%s reason=%s hook=%p bypass=%p dynamicSlot=%d streamlineStack=%d pluginGuard=%d",
                nullFnPtr, resolvedRva,
                patchTarget == recoveryContext.bypass ? "DXGIBypassPresent" : "SteamDummyRenderingCallback",
                patchTarget, recoveryContext.context ? recoveryContext.context : "unknown",
                recoveryContext.reason ? recoveryContext.reason : "Present", recoveryContext.hook,
                recoveryContext.bypass, dynamicallyResolvedSlot ? 1 : 0, recoveryContext.streamlineStackActive ? 1 : 0,
                recoveryContext.pluginLookupGuardReady ? 1 : 0);
        } else {
            HookLogImportant(
                "SteamOverlayInitVehHandler: VirtualProtect failed for Steam callback at %p context=%s reason=%s",
                nullFnPtr, recoveryContext.context ? recoveryContext.context : "unknown",
                recoveryContext.reason ? recoveryContext.reason : "Present");
        }
    } else {
        HookLogImportant(
            "SteamOverlayInitVehHandler: RVA 0x%zX not patchable (slot=%p readable=%d value=%p) - RVA may have "
            "changed, skipping patch and falling back to crash skip context=%s reason=%s dynamicSlot=%d",
            resolvedRva, nullFnPtr, callbackSlotReadable ? 1 : 0, callbackBefore,
            recoveryContext.context ? recoveryContext.context : "unknown",
            recoveryContext.reason ? recoveryContext.reason : "Present", dynamicallyResolvedSlot ? 1 : 0);
    }

    if (patched) {
        // Retry `call (e)ax` with our patched callback.
#ifdef _WIN64
        ep->ContextRecord->Rsp += 8;  // undo the call's stack push (8 bytes on x64)
        ep->ContextRecord->Rax = (DWORD64)patchTarget;
        ep->ContextRecord->Rip = returnAddress - kCallOpcodeSize;
        HookLog("SteamOverlayInitVehHandler: Retrying call rax with RAX=%p", (void*)ep->ContextRecord->Rax);
#else
        ep->ContextRecord->Esp += 4;  // undo the call's stack push (4 bytes on x86)
        ep->ContextRecord->Eax = (DWORD)(uintptr_t)patchTarget;
        ep->ContextRecord->Eip = returnAddress - kCallOpcodeSize;
        HookLog("SteamOverlayInitVehHandler: Retrying call eax with EAX=%p", (void*)(DWORD_PTR)ep->ContextRecord->Eax);
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
}

namespace DXGIShared {
class ScopedSteamNullCallbackRecoveryGuard {
public:
    ScopedSteamNullCallbackRecoveryGuard(bool enabled, const char* context, const char* reason, void* hook,
                                         void* bypass, bool streamlineStackActive, bool pluginLookupGuardReady)
        : previousContext_(dxgi_shared_s_steamNullCallbackRecoveryContext) {
        if (!enabled) {
            return;
        }

        dxgi_shared_s_steamNullCallbackRecoveryContext = SteamNullCallbackRecoveryContext{
            context ? context : "unknown", reason, hook, bypass, streamlineStackActive, pluginLookupGuardReady,
        };
        handle_ = AddVectoredExceptionHandler(1, SteamOverlayInitVehHandler);
        if (handle_) {
            static std::atomic<int> s_guardInstallLogCount{0};
            const int logCount = s_guardInstallLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (logCount <= 20 || logCount == 50 || (logCount % 500) == 0) {
                HookLogImportant(
                    "Guarded Steam Present hook installed Steam null-callback VEH recovery #%d "
                    "(context=%s reason=%s hook=%p bypass=%p streamlineStack=%d pluginGuard=%d tid=0x%04X)",
                    logCount, context ? context : "unknown", reason ? reason : "Present", hook, bypass,
                    streamlineStackActive ? 1 : 0, pluginLookupGuardReady ? 1 : 0, GetCurrentThreadId());
            }
        } else {
            HookLogImportant(
                "Guarded Steam Present hook failed to install Steam null-callback VEH recovery "
                "(context=%s reason=%s hook=%p bypass=%p streamlineStack=%d pluginGuard=%d err=%lu)",
                context ? context : "unknown", reason ? reason : "Present", hook, bypass, streamlineStackActive ? 1 : 0,
                pluginLookupGuardReady ? 1 : 0, GetLastError());
        }
    }

    ~ScopedSteamNullCallbackRecoveryGuard() {
        if (handle_) {
            RemoveVectoredExceptionHandler(handle_);
        }
        dxgi_shared_s_steamNullCallbackRecoveryContext = previousContext_;
    }

    ScopedSteamNullCallbackRecoveryGuard(const ScopedSteamNullCallbackRecoveryGuard&) = delete;
    ScopedSteamNullCallbackRecoveryGuard& operator=(const ScopedSteamNullCallbackRecoveryGuard&) = delete;

    bool IsInstalled() const {
        return handle_ != nullptr;
    }

private:
    SteamNullCallbackRecoveryContext previousContext_;
    PVOID handle_ = nullptr;
};
}

namespace DXGIShared {
// Forward declaration — defined later in this translation unit.
inline PFN_Present EnsurePresentBypassTrampoline();
}

namespace DXGIShared {
inline bool IsSteamOverlayModule(const char* overlayModule) {
    return overlayModule && ce::overlay_compat::detail::ContainsInsensitive(overlayModule, "gameoverlayrenderer");
}
}

namespace DXGIShared {
inline bool IsStreamlineModuleHandle(HMODULE moduleHandle) {
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
}

namespace DXGIShared {
inline bool IsCaptureHookModulePath(const char* modulePath) {
    return modulePath && ce::overlay_compat::detail::ContainsInsensitive(modulePath, "capture_hook");
}
}

namespace DXGIShared {
inline bool IsCodeAddressFromStreamlineModule(const void* codeAddress) {
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
}

namespace DXGIShared {
inline bool TryGetModulePathFromCodeAddress(const void* codeAddress, char* modulePathOut, size_t modulePathOutCount,
                                            HMODULE* moduleOut = nullptr) {
    return ce::overlay_compat::TryGetModulePathFromCodeAddress(codeAddress, modulePathOut, modulePathOutCount,
                                                               moduleOut);
}
}

namespace DXGIShared {
inline bool HasStartupBlockingOverlayModuleInCurrentStack() {
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
}

namespace DXGIShared {
inline bool HasStreamlineModuleInCurrentStack() {
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
}

namespace DXGIShared {
inline bool IsWrappedSwapChainObject(IDXGISwapChain* pSwapChain) {
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
}

namespace DXGIShared {
inline bool ShouldForceSteamDX12Bypass(IDXGISwapChain* pSwapChain, bool bypassAvailable, bool slLoaded,
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
}

namespace DXGIShared {
inline bool ShouldForceThirdPartyOverlayBypass(IDXGISwapChain* pSwapChain, bool bypassAvailable,
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
}

namespace DXGIShared {
inline DX12StartupPresentMode GetDX12StartupPresentMode(bool bypassAvailable, const char** overlayModuleOut = nullptr,
                                                        int* passIndexOut = nullptr) {
    const char* overlayModule = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
    if (overlayModuleOut) {
        *overlayModuleOut = overlayModule;
    }
    const bool steamBypassShouldOwnPath = ShouldForceSteamDX12BypassForState(
        bypassAvailable, IsSteamOverlayModule(overlayModule), true, false, false,
        ce::overlay_compat::IsStreamlineInterposerModuleLoaded(), g_FGCompat.GetRuntimeMode(),
        g_StreamlineFGRunning.load(std::memory_order_acquire), g_FGCompat.IsNvPresentLoaded());
    const bool bypassReady = EnsurePresentBypassTrampoline() != nullptr;
    if (!DXGIShared::ShouldAllowDX12StartupPresentPassForState(overlayModule != nullptr, dxgi_shared_oPresentTrampoline != nullptr,
                                                               dxgi_shared_oPresent1Trampoline != nullptr, steamBypassShouldOwnPath,
                                                               bypassReady, g_FGCompat.GetRuntimeMode(),
                                                               g_StreamlineFGRunning.load(std::memory_order_acquire))) {
        static std::atomic<int> s_startupPassBlockLogCount{0};
        const int blockNum = s_startupPassBlockLogCount.fetch_add(1, std::memory_order_relaxed);
        if (blockNum < 5) {
            HookLogImportant(
                "GetDX12StartupPresentMode: Startup compat pass blocked "
                "(overlay=%d trampoline=%d bypass=%d steamBypassOwn=%d runtimeMode=%d slFG=%d)",
                overlayModule != nullptr ? 1 : 0, dxgi_shared_oPresentTrampoline != nullptr ? 1 : 0, bypassReady ? 1 : 0,
                steamBypassShouldOwnPath ? 1 : 0, static_cast<int>(g_FGCompat.GetRuntimeMode()),
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
}

namespace DXGIShared {
// Global flag to disable DXGI hooks when Vulkan is active
// This is set once at startup and prevents DXGI hooks from interfering with
// Vulkan WSI
inline bool dxgi_shared_s_vulkanPresent = false;
}

namespace DXGIShared {
inline bool dxgi_shared_s_checkedVulkan = false;
}

namespace DXGIShared {
inline bool IsVulkanActive() {
    if (!dxgi_shared_s_checkedVulkan) {
        HMODULE hVulkan = GetModuleHandleW(L"vulkan-1.dll");
        dxgi_shared_s_vulkanPresent = (hVulkan != nullptr);
        if (dxgi_shared_s_vulkanPresent) {
            HookLog(
                "DXGIShared: Vulkan detected (vulkan-1.dll), DXGI hooks will "
                "pass through");
        }
        dxgi_shared_s_checkedVulkan = true;
    }
    return dxgi_shared_s_vulkanPresent;
}
}

namespace DXGIShared {
// Unified Detours
// For DX12 wrapped swapchains: CWrapDXGISwapChain handles Present, so when
// wrapper calls m_pReal->Present() and it re-enters here, we just passthrough.
// For DX12 pre-existing swapchains (not wrapped): full processing here.
// For DX11: full processing here.
inline bool IsReadableMemory(const void* ptr, size_t size) {
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
}

namespace DXGIShared {
inline bool HasExternalEntryHook(const void* target) {
    const auto* code = static_cast<const uint8_t*>(target);
    if (!IsReadableMemory(code, 16)) {
        return false;
    }
    return code[0] == 0xE9 || (code[0] == 0xFF && code[1] == 0x25);
}
}

namespace DXGIShared {
// Resolves the target of an E9 (near JMP) hook at the given function address.
// Returns the absolute address of the hook handler, or nullptr if no E9 JMP
// is present or the function body is unreadable.
inline void* ResolveE9JmpTarget(void* funcAddress) {
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
}

namespace DXGIShared {
inline void* ResolveFF25JmpTarget(void* funcAddress) {
    if (!funcAddress) {
        return nullptr;
    }
    const auto* code = static_cast<const uint8_t*>(funcAddress);
    if (!IsReadableMemory(code, 14)) {
        return nullptr;
    }
    if (code[0] != 0xFF || code[1] != 0x25) {
        return nullptr;
    }

    int32_t dispOffset = 0;
    memcpy(&dispOffset, code + 2, sizeof(dispOffset));
    const auto* targetSlot = reinterpret_cast<void* const*>(code + 6 + dispOffset);
    if (!IsReadableMemory(reinterpret_cast<const void*>(targetSlot), sizeof(void*))) {
        return nullptr;
    }
    return *targetSlot;
}
}

namespace DXGIShared {
inline PFN_Present EnsurePresentBypassTrampoline() {
    if (dxgi_shared_oPresentBypass) {
        return dxgi_shared_oPresentBypass;
    }

    std::lock_guard<std::mutex> lock(g_SharedMutex);
    if (dxgi_shared_oPresentBypass) {
        return dxgi_shared_oPresentBypass;
    }

    const PFN_Present presentOriginal = dxgi_shared_oPresent;
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

    dxgi_shared_oPresentBypass = (PFN_Present)bypass;
    HookLogImportant("DXGIShared: Lazily created Present bypass trampoline at %p from %p", bypass, presentOriginal);
    return dxgi_shared_oPresentBypass;
}
}

namespace DXGIShared {
inline PFN_Present1 EnsurePresent1BypassTrampoline() {
    if (dxgi_shared_oPresent1Bypass) {
        return dxgi_shared_oPresent1Bypass;
    }

    std::lock_guard<std::mutex> lock(g_SharedMutex);
    if (dxgi_shared_oPresent1Bypass) {
        return dxgi_shared_oPresent1Bypass;
    }

    const PFN_Present1 present1Original = dxgi_shared_oPresent1;
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

    dxgi_shared_oPresent1Bypass = (PFN_Present1)bypass;
    HookLogImportant("DXGIShared: Lazily created Present1 bypass trampoline at %p from %p", bypass, present1Original);
    return dxgi_shared_oPresent1Bypass;
}
}

namespace DXGIShared {
inline bool TryReadSteamOverlayNullCallbackSlot(void** callbackValueOut) {
    if (!callbackValueOut) {
        return false;
    }

#ifdef _WIN64
    const wchar_t* steamModuleName = L"gameoverlayrenderer64.dll";
    const uintptr_t kSteamCallbackRva = 0x1621d8;
#else
    const wchar_t* steamModuleName = L"gameoverlayrenderer.dll";
    const uintptr_t kSteamCallbackRva = 0x1621d8;
#endif

    HMODULE steamMod = GetModuleHandleW(steamModuleName);
    if (!steamMod) {
        return false;
    }

    void** callbackSlot = reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(steamMod) + kSteamCallbackRva);
    if (!IsReadableMemory(reinterpret_cast<const void*>(callbackSlot), sizeof(void*))) {
        return false;
    }

    *callbackValueOut = *callbackSlot;
    return true;
}
}

namespace DXGIShared {
inline bool TryGetSwapChainBackBufferIndex(IDXGISwapChain* pSwapChain, UINT* indexOut) {
    if (!pSwapChain || !indexOut) {
        return false;
    }

    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) {
        return false;
    }

    *indexOut = sc3->GetCurrentBackBufferIndex();
    sc3->Release();
    return true;
}
}

namespace DXGIShared {
inline bool TryInvokeGuardedExternalSteamOverlayPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                                                        const char* reason, HRESULT* resultOut) {
    if (!pSwapChain || !resultOut) {
        return false;
    }

    PFN_Present externalPresent = dxgi_shared_g_externalOverlayPresentHook;
    PFN_Present presentBypass = EnsurePresentBypassTrampoline();
    const char* overlayModule = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
    const bool isSteamOverlay = IsSteamOverlayModule(overlayModule);
    const bool isD3D12SwapChain = DetectAPIType(pSwapChain) == APIType::D3D12;
    const bool streamlineStackActive = isD3D12SwapChain && HasStreamlineModuleInCurrentStack();
    const bool streamlinePluginLookupGuardReady = StreamlineHook::IsExternalOverlayPluginLookupGuardReady();
    const bool streamlineFGRunning = g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool postSLConfirmedRendering = isD3D12SwapChain && HookIsPostSLOverlayConfirmedRendering();
    const bool postSLConfirmedButStartupSettling = isD3D12SwapChain && HookIsPostSLOverlayConfirmedButStartupSettling();
    const bool startupTransitionWindowActive =
        isD3D12SwapChain && DXGIShared::IsStreamlineStartupTransitionWindowActive();
    void* steamCallbackBefore = nullptr;
    const bool steamCallbackReadable = TryReadSteamOverlayNullCallbackSlot(&steamCallbackBefore);
    const auto steamCallbackAddress = reinterpret_cast<uintptr_t>(steamCallbackBefore);
    const bool steamCallbackIsNull = steamCallbackReadable && steamCallbackBefore == nullptr;
    const bool steamCallbackIsCEDummy =
        steamCallbackReadable && steamCallbackBefore == reinterpret_cast<void*>(SteamDummyRenderingCallback);
    const bool steamCallbackIsInvalidLowAddress =
        steamCallbackReadable && steamCallbackBefore != nullptr && steamCallbackAddress < 0x10000;
    ScopedSteamNullCallbackRecoveryGuard steamNullCallbackGuard(
        externalPresent != nullptr && externalPresent != DetourPresent && presentBypass != nullptr && isSteamOverlay &&
            isD3D12SwapChain,
        "guarded external Present", reason, reinterpret_cast<void*>(externalPresent),
        reinterpret_cast<void*>(presentBypass), streamlineStackActive, streamlinePluginLookupGuardReady);
    const bool steamNullCallbackRecoveryReady = steamNullCallbackGuard.IsInstalled();
    const bool basePolicyAllowsGuardedSteamInvoke = DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForState(
        externalPresent != nullptr && externalPresent != DetourPresent, presentBypass != nullptr, isSteamOverlay,
        isD3D12SwapChain, IsInWrapperPresent(), IsWrappedSwapChainObject(pSwapChain),
        dxgi_shared_s_externalOverlayPresentInvokeDepth > 0, streamlineStackActive, streamlinePluginLookupGuardReady,
        steamNullCallbackRecoveryReady);
    const bool callbackStateAllowsGuardedSteamInvoke =
        DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForCallbackState(
            basePolicyAllowsGuardedSteamInvoke, steamCallbackReadable, steamCallbackIsNull, steamCallbackIsCEDummy,
            steamCallbackIsInvalidLowAddress, steamNullCallbackRecoveryReady);
    if (!callbackStateAllowsGuardedSteamInvoke) {
        if (basePolicyAllowsGuardedSteamInvoke && externalPresent && presentBypass && isSteamOverlay &&
            isD3D12SwapChain) {
            static std::atomic<int> s_guardedSteamCallbackStateSkipLogCount{0};
            const int skipNum = s_guardedSteamCallbackStateSkipLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (skipNum <= 20 || skipNum == 50 || (skipNum % 500) == 0) {
                HookLogImportant(
                    "DXGIShared: Skipping guarded Steam Present hook #%d for %s because Steam callback state is not "
                    "a real renderer; using bypass trampoline instead (callbackReadable=%d null=%d ceDummy=%d "
                    "lowAddress=%d callback=%p steamNullGuard=%d streamlineStack=%d pluginGuard=%d tid=0x%04X)",
                    skipNum, reason ? reason : "Present", steamCallbackReadable ? 1 : 0, steamCallbackIsNull ? 1 : 0,
                    steamCallbackIsCEDummy ? 1 : 0, steamCallbackIsInvalidLowAddress ? 1 : 0, steamCallbackBefore,
                    steamNullCallbackRecoveryReady ? 1 : 0, streamlineStackActive ? 1 : 0,
                    streamlinePluginLookupGuardReady ? 1 : 0, GetCurrentThreadId());
            }
        }
        if (externalPresent && presentBypass && isSteamOverlay && isD3D12SwapChain && streamlineStackActive) {
            static std::atomic<int> s_guardedSteamStreamlineStackSkipLogCount{0};
            const int skipNum = s_guardedSteamStreamlineStackSkipLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (skipNum <= 20 || skipNum == 50 || (skipNum % 500) == 0) {
                HookLogImportant(
                    "DXGIShared: Skipping guarded Steam Present hook #%d for %s because current stack is inside "
                    "Streamline startup/FG routing and required guard state is not ready; using bypass trampoline "
                    "instead (slFG=%d confirmed=%d settling=%d startupWindow=%d pluginGuard=%d invokeDepth=%d "
                    "steamNullGuard=%d steamCallbackReadable=%d steamCallback=%p tid=0x%04X)",
                    skipNum, reason ? reason : "Present", streamlineFGRunning ? 1 : 0, postSLConfirmedRendering ? 1 : 0,
                    postSLConfirmedButStartupSettling ? 1 : 0, startupTransitionWindowActive ? 1 : 0,
                    streamlinePluginLookupGuardReady ? 1 : 0, dxgi_shared_s_externalOverlayPresentInvokeDepth,
                    steamNullCallbackRecoveryReady ? 1 : 0, steamCallbackReadable ? 1 : 0, steamCallbackBefore,
                    GetCurrentThreadId());
            }
        }
        return false;
    }

    static std::atomic<int> s_guardedSteamInvokeLogCount{0};
    const int invokeNum = s_guardedSteamInvokeLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (invokeNum <= 20 || invokeNum == 50 || (invokeNum % 500) == 0) {
        HookLogImportant(
            "DXGIShared: Invoking guarded Steam Present hook #%d for %s "
            "(hook=%p bypass=%p slLoaded=%d streamlineFG=%d streamlineStack=%d pluginGuard=%d "
            "steamNullGuard=%d steamCallbackReadable=%d steamCallback=%p tid=0x%04X)",
            invokeNum, reason ? reason : "Present", (void*)externalPresent, (void*)presentBypass,
            ce::overlay_compat::IsStreamlineInterposerModuleLoaded() ? 1 : 0, streamlineFGRunning ? 1 : 0,
            streamlineStackActive ? 1 : 0, streamlinePluginLookupGuardReady ? 1 : 0,
            steamNullCallbackRecoveryReady ? 1 : 0, steamCallbackReadable ? 1 : 0, steamCallbackBefore,
            GetCurrentThreadId());
    }

    ++dxgi_shared_s_externalOverlayPresentInvokeDepth;
    auto depthGuard = ce::make_scope_guard([]() {
        if (dxgi_shared_s_externalOverlayPresentInvokeDepth > 0) {
            --dxgi_shared_s_externalOverlayPresentInvokeDepth;
        }
    });
    StreamlineHook::ExternalOverlayPresentGuard slGuard;

    UINT bbIdxBefore = UINT_MAX;
    UINT bbIdxAfter = UINT_MAX;
    const bool bbIdxBeforeMeasured = TryGetSwapChainBackBufferIndex(pSwapChain, &bbIdxBefore);
    const HRESULT hr = externalPresent(pSwapChain, SyncInterval, Flags);
    const bool bbIdxAfterMeasured = TryGetSwapChainBackBufferIndex(pSwapChain, &bbIdxAfter);
    const bool bbIdxMeasured = bbIdxBeforeMeasured && bbIdxAfterMeasured;
    const bool bbIdxAdvanced = bbIdxMeasured && bbIdxAfter != bbIdxBefore;
    if (FAILED(hr)) {
        static std::atomic<int> s_guardedSteamFailureLogCount{0};
        const int failureNum = s_guardedSteamFailureLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (failureNum <= 10 || (failureNum % 100) == 0) {
            HookLogImportant("DXGIShared: Guarded Steam Present hook failed for %s hr=0x%08X (failure #%d)",
                             reason ? reason : "Present", (unsigned)hr, failureNum);
        }
    }

    if (DXGIShared::ShouldFallbackGuardedExternalSteamOverlayPresentForResult(presentBypass != nullptr, hr,
                                                                              bbIdxMeasured, bbIdxAdvanced)) {
        static std::atomic<int> s_guardedSteamBypassFallbackLogCount{0};
        const int fallbackNum = s_guardedSteamBypassFallbackLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (fallbackNum <= 20 || fallbackNum == 50 || (fallbackNum % 500) == 0) {
            HookLogImportant(
                "DXGIShared: Guarded Steam Present hook fallback #%d for %s via bypass=%p "
                "(hr=0x%08X bbMeasured=%d bbIdx=%u->%u steamNullGuard=%d tid=0x%04X)",
                fallbackNum, reason ? reason : "Present", (void*)presentBypass, (unsigned)hr, bbIdxMeasured ? 1 : 0,
                bbIdxBefore, bbIdxAfter, steamNullCallbackRecoveryReady ? 1 : 0, GetCurrentThreadId());
        }
        *resultOut = presentBypass(pSwapChain, SyncInterval, Flags);
        return true;
    }

    *resultOut = hr;
    return true;
}
}

namespace DXGIShared {
namespace {
inline bool IsSLInterposerLoaded();
}
}

namespace DXGIShared {
namespace {
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
inline std::atomic<bool> dxgi_shared_s_slRoutingActive{false};
}
}

namespace DXGIShared {
namespace {
inline bool IsSLInterposerLoaded() {
    // Latch once SL is seen (SL routing decisions assume SL stays present for the session).
    // The presence check is LOADER-FREE (cached loaded-set maintained by the seed + DLL
    // load/unload notifications); the old per-call GetModuleHandleA("sl.interposer.dll") was a
    // per-Present loader walk that stalled the present thread during the Alt+Tab mode-switch
    // DLL churn. SL loads via LoadLibrary/LdrLoadDll, which feed the cache, so detection timing
    // is equivalent.
    static std::atomic<bool> detected{false};
    if (detected.load(std::memory_order_acquire))
        return true;
    if (ce::overlay_compat::IsStreamlineInterposerModuleLoaded()) {
        detected.store(true, std::memory_order_release);
        return true;
    }
    return false;
}
}
}

namespace DXGIShared {
namespace {
inline bool ShouldKeepSLPresentRoutingDisabledNow(ce::fg_runtime::RuntimeMode* runtimeModeOut = nullptr,
                                                  bool* runtimeOwnedNativeFGPresentPathOut = nullptr) {
    const auto runtimeMode = g_FGCompat.GetRuntimeMode();
    const bool runtimeOwnedNativeFGPresentPath = HookHasRuntimeOwnedNativeFGPresentPath();
    if (runtimeModeOut) {
        *runtimeModeOut = runtimeMode;
    }
    if (runtimeOwnedNativeFGPresentPathOut) {
        *runtimeOwnedNativeFGPresentPathOut = runtimeOwnedNativeFGPresentPath;
    }
    return DXGIShared::ShouldKeepSLPresentRoutingDisabledForRuntimeState(runtimeMode, runtimeOwnedNativeFGPresentPath);
}
}
}

namespace DXGIShared {
namespace {
// Detect if SL has hooked the Present function with an E9 JMP or FF 25
// indirect JMP.  If so, set up routing so our final Present call goes
// through SL's hook chain instead of bypassing it via the trampoline.
inline void DetectSLPresentHook() {
    if (dxgi_shared_s_slRoutingActive.load(std::memory_order_acquire))
        return;
    if (!dxgi_shared_oPresent || !dxgi_shared_oPresentTrampoline) {
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
                dxgi_shared_oPresent);
        }
        return;
    }

    // If oPresent is our own trampoline, SL hasn't hooked the vtable yet.
    // The vtable repair code sets oPresent to SL's hook when detected.
    if (dxgi_shared_oPresent == dxgi_shared_oPresentTrampoline)
        return;

    auto* funcBytes = (const uint8_t*)dxgi_shared_oPresent;
    if (!IsReadableMemory(funcBytes, 16))
        return;

    // Rate-limit diagnostic logging to avoid per-frame spam.
    static int s_checkCount = 0;
    int checkNum = ++s_checkCount;
    if (checkNum <= 5 || (checkNum <= 50 && (checkNum % 10) == 0) || (checkNum % 500) == 0) {
        HookLogImportant("DetectSLPresentHook: oPresent=%p bytes: %02X %02X %02X %02X %02X %02X (check #%d)", dxgi_shared_oPresent,
                         funcBytes[0], funcBytes[1], funcBytes[2], funcBytes[3], funcBytes[4], funcBytes[5], checkNum);
    }

    // Detect SL hooks: E9 relative JMP or FF 25 indirect JMP (JMP [RIP+0]).
    // SL may use either pattern depending on version and game.
    bool isE9 = (funcBytes[0] == 0xE9);
    bool isFF25 = (funcBytes[0] == 0xFF && funcBytes[1] == 0x25);

    if (!isE9 && !isFF25) {
        return;
    }

    void* hookTarget = isE9 ? ResolveE9JmpTarget((void*)dxgi_shared_oPresent) : ResolveFF25JmpTarget((void*)dxgi_shared_oPresent);
    char hookTargetModulePath[MAX_PATH] = {};
    HMODULE hookTargetModule = nullptr;
    const bool hookTargetResolved =
        hookTarget && TryGetModulePathFromCodeAddress(hookTarget, hookTargetModulePath, sizeof(hookTargetModulePath),
                                                      &hookTargetModule);
    const bool hookTargetFromStreamline = hookTargetResolved && IsStreamlineModuleHandle(hookTargetModule);
    const bool hookTargetFromCaptureHook = hookTargetResolved && IsCaptureHookModulePath(hookTargetModulePath);
    if (!DXGIShared::ShouldActivateStreamlinePresentRoutingForHookTarget(
            true, hookTargetResolved, hookTargetFromStreamline, hookTargetFromCaptureHook)) {
        static std::atomic<uint32_t> s_rejectedHookTargetLogCount{0};
        const uint32_t rejectedLogCount = s_rejectedHookTargetLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (rejectedLogCount <= 20 || (rejectedLogCount % 500) == 0) {
            HookLogImportant(
                "DetectSLPresentHook: %s JMP at oPresent=%p rejected as non-Streamline target "
                "(target=%p resolved=%d module=%s captureHook=%d streamline=%d log=%u)",
                isE9 ? "E9" : "FF25", dxgi_shared_oPresent, hookTarget, hookTargetResolved ? 1 : 0,
                hookTargetModulePath[0] ? hookTargetModulePath : "unknown", hookTargetFromCaptureHook ? 1 : 0,
                hookTargetFromStreamline ? 1 : 0, rejectedLogCount);
        }
        return;
    }

    // Verify that our trampoline is different (it should have the original
    // function bytes, not a JMP).
    auto* trampolineBytes = (const uint8_t*)dxgi_shared_oPresentTrampoline;
    static std::atomic<uint32_t> s_trampolineBytesLogCount{0};
    const uint32_t trampolineLogCount = s_trampolineBytesLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (trampolineLogCount <= 5 || (trampolineLogCount % 500) == 0) {
        HookLogImportant("DetectSLPresentHook: trampoline=%p bytes: %02X %02X %02X %02X %02X %02X (trampolineLog=%u)",
                         dxgi_shared_oPresentTrampoline, trampolineBytes[0], trampolineBytes[1], trampolineBytes[2],
                         trampolineBytes[3], trampolineBytes[4], trampolineBytes[5], trampolineLogCount);
    }

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
                "(native FG path owns Present routing, suppressed #%d, runtime=%s "
                "runtimeOwnedNativeFG=%d)",
                isE9 ? "E9" : "FF25", dxgi_shared_oPresent, suppressedNum, ce::fg_runtime::GetRuntimeModeName(runtimeMode),
                runtimeOwnedNativeFGPresentPath ? 1 : 0);
        }
        return;
    }

    dxgi_shared_s_slRoutingActive.store(true, std::memory_order_release);
    HookLogImportant(
        "SL routing ACTIVE: Present calls will go through oPresent=%p "
        "(%s JMP target=%p module=%s) instead of trampoline=%p.  SL FG chain will execute.",
        dxgi_shared_oPresent, isE9 ? "E9" : "FF25", hookTarget, hookTargetModulePath[0] ? hookTargetModulePath : "unknown",
        dxgi_shared_oPresentTrampoline);
}
}
}

namespace DXGIShared {
namespace {
inline void UpdateDXGIPresentMetricsAndPublish(bool isFirstHook, const char* publicationSource) {
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
    dxgi_shared_g_DXGIPerfMetrics.Update(us);
    const ce::fg_session::FGActionPlan plan = ce::fg_session::GetLatestFGActionPlan();
    ce::overlay_metrics::PublishOverlayFGMetrics(&dxgi_shared_g_DXGIPerfMetrics, plan, g_FGCompat.GetOutputFPS(),
                                                 g_FGCompat.GetBaseFPS(), g_FGCompat.GetFGMultiplier(),
                                                 publicationSource);
}
}
}

namespace DXGIShared {
namespace {
inline void RefreshLivePresentHooksForSwapchainIfNeeded(IDXGISwapChain* pSwapChain, const char* source) {
    if (!pSwapChain || !IsReadableMemory(pSwapChain, sizeof(void*))) {
        return;
    }

    void** vtable = *(void***)pSwapChain;
    const bool hasReadableVtable = vtable && IsReadableMemory(reinterpret_cast<const void*>(vtable), 23 * sizeof(void*));
    const bool trackedVtableMatchesCurrent = hasReadableVtable && dxgi_shared_s_hookedVTable == vtable;
    const bool presentHookInstalled = hasReadableVtable && vtable[8] == (void*)DetourPresent;
    const bool present1HookInstalled = hasReadableVtable && vtable[22] == (void*)DetourPresent1;

    if (!DXGIShared::ShouldRefreshLivePresentHooksForSwapchainPath(hasReadableVtable, trackedVtableMatchesCurrent,
                                                                   presentHookInstalled, present1HookInstalled)) {
        return;
    }

    HookLogImportant(
        "DXGIShared: Refreshing live Present hook path via %s swapchain %p (oldVtable=%p newVtable=%p hooked8=%d "
        "hooked22=%d)",
        source ? source : "runtime", pSwapChain, dxgi_shared_s_hookedVTable, vtable, presentHookInstalled ? 1 : 0,
        present1HookInstalled ? 1 : 0);

    InstallHooks(pSwapChain, true);
    RepairVTableHooksIfNeeded();

    if (IsSLInterposerLoaded() && !ce::fg_runtime::RuntimeModeUsesFSR(g_FGCompat.GetRuntimeMode())) {
        DetectSLPresentHook();
    }
}
}
}

namespace DXGIShared {
// RTSS-style: draw the overlay present-time before a Streamline-startup bypass present so the
// toggle-on / DLSS-G-init frozen frame carries the overlay. Opt-in + gated; HandleDX12ProcessFrame
// resolves the submit queue and does the same-queue safety check internally (see the pre-SL un-gate
// in dx12_hook.cpp). Gated so steady-state FG and the round-1..3 wins are untouched: D3D12 only,
// DLSS FG turning on, PostSL not yet confirmed (once PostSL owns the overlay this bypass path is not
// taken), and pure DLSS (no FSR history).
inline void MaybeEagerDrawOverlayBeforeStreamlineStartupBypass(IDXGISwapChain* pSwapChain, bool isD3D12,
                                                               bool streamlineFGRunning, bool postSLConfirmedRendering,
                                                               bool hadFSRFGPhase, const char* site) {
    if (!IsDlssToggleEagerOverlayEnabled() || !isD3D12 || !streamlineFGRunning || postSLConfirmedRendering ||
        hadFSRFGPhase)
        return;
    static std::atomic<int> s_log{0};
    const int n = s_log.fetch_add(1, std::memory_order_relaxed);
    if (n < 10 || (n % 120) == 0)
        HookLogImportant(
            "DX12: Eager present-time overlay draw before Streamline-startup bypass (RTSS-style, site=%s sc=%p)", site,
            (void*)pSwapChain);
    HandleDX12ProcessFrame(pSwapChain, false, true);
}
}

namespace DXGIShared {
// SAFETY NET: Attempt one-time Steam DX12 overlay initialization.
//
// The PRIMARY fix (InstallPresentInlineHooks) pre-initializes Steam overlay on
// the temp swapchain BEFORE our vtable hook is installed.  This function is a
// fallback for cases where pre-init didn't occur:
//   - Steam overlay loaded AFTER hook installation
//   - Another thread/process context
//
// It temporarily restores vtable[8] to the real dxgi!Present, calls through
// Steam's E9 JMP, then re-hooks vtable[8] to DetourPresent. If Steam still
// reaches a lazy NULL callback on the real swapchain, the scoped VEH guard
// patches the exact faulting slot to CE's DXGI bypass Present and retries.
//
// Thread safety: only one thread wins the compare-exchange.  The brief window
// where vtable[8] is unhooked is microseconds wide and limited to frame 1.
//
// Returns true if this thread performed the init call (result in *resultOut).
// Returns false if another thread won the init race or if init was skipped.
inline bool AttemptSteamDX12OverlayInit(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                                        PFN_Present presentOriginal, PFN_Present presentBypass, HRESULT* resultOut) {
    if (!pSwapChain || !resultOut || !dxgi_shared_s_hookedVTable || !presentOriginal || dxgi_shared_s_steamInitCrashed) {
        return false;
    }

    if (!IsReadableMemory(reinterpret_cast<const void*>(dxgi_shared_s_hookedVTable), 9 * sizeof(void*))) {
        return false;
    }

    // Only one thread wins the init race
    bool expected = false;
    if (!dxgi_shared_s_steamDX12InitAttempted.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                          std::memory_order_acquire)) {
        return false;  // Another thread is already handling init
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        HookLogImportant(
            "AttemptSteamDX12OverlayInit: VirtualProtect failed to unhook vtable[8] — will retry on next frame");
        dxgi_shared_s_steamDX12InitAttempted.store(false, std::memory_order_release);
        return false;
    }

    // Save current vtable[8] (= DetourPresent) and restore to the real dxgi!Present
    void* savedVtable8 = dxgi_shared_s_hookedVTable[8];
    dxgi_shared_s_hookedVTable[8] = (void*)presentOriginal;
    VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), oldProtect, &oldProtect);

    HookLogImportant(
        "AttemptSteamDX12OverlayInit: vtable[8] temporarily restored to dxgi!Present=%p — "
        "calling through E9 JMP for Steam overlay init (with VEH protection) "
        "[s_originalVtable8Present=%p, same=%d]",
        (void*)presentOriginal, (void*)dxgi_shared_s_originalVtable8Present, dxgi_shared_s_originalVtable8Present == presentOriginal ? 1 : 0);

    // Call through oPresent (E9 JMP at dxgi!Present) WITH VEH protection.
    //
    // Steam's OverlayHookD3D3 can still have lazy NULL callback slots on first
    // entry through the E9 JMP on a REAL game swapchain (the temp swapchain pre-
    // init in InstallPresentInlineHooks doesn't trigger full initialization
    // because Steam skips rendering on a 2x2 hidden-window swapchain).
    //
    // The SteamOverlayInitVehHandler catches this specific crash (RIP=0, RAX=0,
    // return address inside gameoverlayrenderer64.dll), patches the exact NULL
    // slot to CE's bypass Present when possible, and retries the `call rax` so
    // Steam completes its initialization and real Present chaining survives.
    //
    // If the crash is NOT the expected NULL callback (e.g. a different Steam bug),
    // the handler returns EXCEPTION_CONTINUE_SEARCH and CE's existing VEH crash
    // handler catches it and writes a crash dump.
    ScopedSteamNullCallbackRecoveryGuard steamInitGuard(true, "non-SL Steam init", "AttemptSteamDX12OverlayInit",
                                                        reinterpret_cast<void*>(presentOriginal),
                                                        reinterpret_cast<void*>(presentBypass), false, false);
    HRESULT initHr = presentOriginal(pSwapChain, SyncInterval, Flags);

    // Re-hook vtable[8] with DetourPresent (our vtable hook)
    if (VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        dxgi_shared_s_hookedVTable[8] = (void*)DetourPresent;
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), oldProtect, &oldProtect);
    } else {
        // CRITICAL: VirtualProtect for re-hook failed — vtable[8] is exposed.
        // Our DetourPresent hook may be lost. Log prominently and continue.
        HookLogImportant(
            "AttemptSteamDX12OverlayInit: CRITICAL — VirtualProtect failed to re-hook vtable[8]! "
            "CE overlay may be disabled for this session.");
    }

    // Check what Steam's legacy known callback slot contains after the init call.
    // New Steam builds can use nearby slots too; the VEH log reports the exact
    // dynamically resolved slot when it differs from this legacy address.
    {
        HMODULE steamMod = GetModuleHandleW(L"gameoverlayrenderer64.dll");
        if (steamMod) {
            void** steamCallbackPtr = (void**)((uintptr_t)steamMod + 0x1621d8);
            if (IsReadableMemory(reinterpret_cast<const void*>(steamCallbackPtr), sizeof(void*))) {
                void* callbackAfterInit = *steamCallbackPtr;
                if (callbackAfterInit != nullptr && callbackAfterInit != (void*)SteamDummyRenderingCallback &&
                    callbackAfterInit != (void*)presentBypass) {
                    HookLogImportant(
                        "AttemptSteamDX12OverlayInit: Steam legacy callback slot contains Steam-owned function %p "
                        "(bypass=%p dummy=%p)",
                        callbackAfterInit, (void*)presentBypass, (void*)SteamDummyRenderingCallback);
                } else {
                    HookLogImportant(
                        "AttemptSteamDX12OverlayInit: Steam legacy callback slot is %s (%p) "
                        "(bypass=%p dummy=%p)",
                        callbackAfterInit == nullptr
                            ? "NULL"
                            : (callbackAfterInit == (void*)presentBypass ? "CE bypass" : "CE dummy"),
                        callbackAfterInit, (void*)presentBypass, (void*)SteamDummyRenderingCallback);
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
}
