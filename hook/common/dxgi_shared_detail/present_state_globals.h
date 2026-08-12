#pragma once

// Split out of dxgi_shared_internal.h to keep both headers under the source-size
// ceiling: the shared DXGI present/resize/Steam/Streamline state globals.
// Included from dxgi_shared_internal.h; do not include directly.

namespace DXGIShared {
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
extern SharedState g_SharedState;
}

namespace DXGIShared {
extern std::mutex g_SharedMutex;
}

namespace DXGIShared {
// Post-SL FG overlay callback (set by dx12_hook.cpp when SL FG is active).
extern std::atomic<PostSLOverlayRenderFn> g_PostSLOverlayRenderCallback;
}

namespace DXGIShared {
// Direct Streamline FG running signal (set by streamline_hook.cpp).
extern std::atomic<bool> g_StreamlineFGRunning;
}

namespace DXGIShared {
// Present call counter — incremented by DetourPresent and DetourPresent1, read by
// SL hook to detect bypass.
extern std::atomic<uint64_t> g_PresentCallCounter;
}

namespace DXGIShared {
// Global metrics for DXGI-based APIs
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
extern PerformanceMetrics dxgi_shared_g_DXGIPerfMetrics;
}

namespace DXGIShared {
// Recursion detection globals (avoiding thread_local which requires runtime
// init)
extern std::atomic<DWORD> dxgi_shared_g_presentThreadId;
}

namespace DXGIShared {
extern std::atomic<int> dxgi_shared_g_presentDepth;
}

namespace DXGIShared {
extern std::atomic<DWORD> dxgi_shared_g_resizeThreadId;
}

namespace DXGIShared {
extern std::atomic<int> dxgi_shared_g_resizeDepth;
}

namespace DXGIShared {
extern PFN_Present dxgi_shared_oPresent;
}

namespace DXGIShared {
extern PFN_Present1 dxgi_shared_oPresent1;
}

namespace DXGIShared {
extern PFN_ResizeBuffers dxgi_shared_oResizeBuffers;
}

namespace DXGIShared {
extern PFN_ResizeBuffers1 dxgi_shared_oResizeBuffers1;
}

namespace DXGIShared {
// Inline hook trampolines - calling these bypasses the hook entirely
extern PFN_Present dxgi_shared_oPresentTrampoline;
}

namespace DXGIShared {
extern PFN_Present1 dxgi_shared_oPresent1Trampoline;
}

namespace DXGIShared {
extern std::atomic<PFN_SetColorSpace1> dxgi_shared_oSetColorSpace1Trampoline;
}

namespace DXGIShared {
extern std::mutex dxgi_shared_s_setColorSpace1HookMutex;
}

namespace DXGIShared {
extern thread_local unsigned dxgi_shared_s_wrapperSetColorSpaceForwardDepth;
}

namespace DXGIShared {
// Bypass trampolines — skip external E9/FF25 hooks (e.g. Streamline) at the
// function entry point by executing original prologue bytes read from disk.
// Used in re-entrant Present calls to actually present the frame without
// re-entering the external hook chain.
extern PFN_Present dxgi_shared_oPresentBypass;
}

namespace DXGIShared {
extern PFN_Present1 dxgi_shared_oPresent1Bypass;
}

namespace DXGIShared {
// Saved target of the external E9 JMP on dxgi!Present, installed by Steam overlay
// (gameoverlayrenderer64!OverlayHookD3D3).  Captured during InstallPresentInlineHooks
// BEFORE Streamline overwrites it with its own JMP.  CE may invoke this target
// from SL-originated Present stacks only while the Streamline plugin-lookup guard
// is active; otherwise those paths use the bypass trampoline.
extern PFN_Present dxgi_shared_g_externalOverlayPresentHook;
}

namespace DXGIShared {
// True when CE deliberately left the dxgi!Present entry to a foreign chain that already has
// more than one overlay in it. CE owns no entry bytes in that mode: forwards must run the
// live entry (never a trampoline or bypass), and CWrapDXGISwapChain is CE's interception.
extern std::atomic<bool> dxgi_shared_s_presentEntryLeftToForeignChain;
bool IsPresentEntryLeftToForeignChain();
}

namespace DXGIShared {
// dxgi!Present / dxgi!Present1 function-entry addresses CE prepended over, recorded at
// InstallPresentInlineHooks time. Needed for the ownership-checked un-prepend when a wrapped
// FG runtime swapchain later allows CE to leave the entry to a multi-overlay foreign chain.
extern void* dxgi_shared_s_presentEntryAddress;
extern void* dxgi_shared_s_present1EntryAddress;
}

namespace DXGIShared {
extern thread_local int dxgi_shared_s_externalOverlayPresentInvokeDepth;
}

namespace DXGIShared {
// Stored vtable pointer for unhooking Present when COM wrapper takes over
extern void** dxgi_shared_s_hookedVTable;
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
extern PFN_Present dxgi_shared_s_originalVtable8Present;
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
extern std::atomic<bool> dxgi_shared_s_steamDX12InitAttempted;
}

namespace DXGIShared {
extern bool dxgi_shared_s_steamInitCrashed;
}

namespace DXGIShared {
extern thread_local SteamNullCallbackRecoveryContext dxgi_shared_s_steamNullCallbackRecoveryContext;
}

namespace DXGIShared {
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
extern std::atomic<bool> dxgi_shared_s_slRoutingActive;
}

namespace DXGIShared {
// Lazy hook installation - installs hooks on first Present if they were
// deferred during swapchain creation
extern IDXGISwapChain* dxgi_shared_s_PendingSwapChainForLazyHook;
}

namespace DXGIShared {
extern std::atomic<bool> dxgi_shared_s_LazyHooksInstalled;
}
