#include "dxgi_shared_internal.h"
#include "present_pacing_policy.h"
#include "backbuffer_reference_trace.h"
#include "resize_reference_holders.h"
#include "resize_reference_probe.h"
#include "swapchain_flag_policy.h"
#include "hook/hooking/vtable_slot_owner.h"

namespace DXGIShared {
namespace {

void DescribeCodeOwner(const void* address, char* out, size_t outCount) {
    char path[MAX_PATH] = {};
    HMODULE module = nullptr;
    if (address && TryGetModulePathFromCodeAddress(address, path, sizeof(path), &module) && module) {
        const char* base = strrchr(path, '\\');
        snprintf(out, outCount, "%s+0x%llX", base ? base + 1 : path,
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) -
                                                 reinterpret_cast<uintptr_t>(module)));
    } else {
        snprintf(out, outCount, "%p", address);
    }
}

// One line naming the owner of every swapchain slot an overlay hooks. A refused
// resize with an overlay's references on the buffers (BackBufferRefTrace) means
// that overlay's ResizeBuffers hook did not run; this says whether it was ever
// in the slot, and where the slot's function jumps on entry.
void LogSwapchainVTableSlotOwners(IDXGISwapChain* pSwapChain, const char* source, const char* when) {
    void** const vtable = pSwapChain ? *reinterpret_cast<void***>(pSwapChain) : nullptr;
    if (!vtable) {
        return;
    }
    char line[1400] = {};
    size_t used = 0;
    for (const auto& slot : ce::vtable_slot_owner::kOverlayHookedSwapchainSlots) {
        if (!IsReadableMemory(static_cast<const void*>(&vtable[slot.index]), sizeof(void*))) {
            break;
        }
        const void* function = vtable[slot.index];
        char owner[MAX_PATH + 32] = {};
        DescribeCodeOwner(function, owner, sizeof(owner));
        char jump[MAX_PATH + 48] = {};
        if (IsReadableMemory(function, ce::vtable_slot_owner::kEntryJumpBytes)) {
            const void* indirect = ce::vtable_slot_owner::IndirectJumpSlot(function);
            if (!indirect || IsReadableMemory(indirect, sizeof(void*))) {
                if (const void* target = ce::vtable_slot_owner::EntryJumpTarget(function)) {
                    char targetOwner[MAX_PATH + 32] = {};
                    DescribeCodeOwner(target, targetOwner, sizeof(targetOwner));
                    snprintf(jump, sizeof(jump), " entryJump->%s", targetOwner);
                }
            }
        }
        const int written = snprintf(line + used, sizeof(line) - used, "%s[%zu]%s=%s%s", used ? " " : "",
                                     slot.index, slot.method, owner, jump);
        if (written <= 0 || used + static_cast<size_t>(written) >= sizeof(line)) {
            break;
        }
        used += static_cast<size_t>(written);
    }
    HookLogImportant("%s: swapchain vtable %p slot owners %s: %s", source, vtable, when, line);
}

// The application resize contract, in one place for ResizeBuffers and
// ResizeBuffers1.
//
// Two distinct jobs, and they must not be conflated:
//
//  1. The waitable-object bit CE may have added at creation has to be hidden
//     again.  DXGI compares the caller's flags against the chain's creation
//     flags and fails the call with E_INVALIDARG on any disagreement in that
//     bit, so the live descriptor - not the current config - is the authority.
//     Reading it back also makes the rewrite correct for a chain created before
//     the override existed and for a config reloaded mid-session, where
//     re-deriving intent from the config would produce the opposite error.
//
//  2. `backbuffer_count` is re-applied to BufferCount, but only when the caller
//     named a count at all.  BufferCount == 0 means "keep the existing count"
//     in DXGI, so substituting the configured depth there would silently resize
//     a chain the application intended to leave alone.
void ReconcileApplicationResizeRequest(IDXGISwapChain* pSwapChain, UINT& BufferCount, UINT& SwapChainFlags,
                                       const char* source) {
    if (!pSwapChain) {
        return;
    }

    DXGI_SWAP_CHAIN_DESC scDesc = {};
    const bool haveDesc = SUCCEEDED(pSwapChain->GetDesc(&scDesc));
    if (haveDesc) {
        const UINT reconciled =
            ce::swapchain_flag_policy::ReconcileApplicationResizeFlags(SwapChainFlags, scDesc.Flags);
        if (reconciled != SwapChainFlags) {
            static std::atomic<uint32_t> s_reconcileLogs{0};
            const uint32_t logIndex = s_reconcileLogs.fetch_add(1, std::memory_order_relaxed);
            if (logIndex < 8 || (logIndex % 256) == 0) {
                HookLogImportant(
                    "%s: Reconciling application resize flags 0x%X -> 0x%X against creation flags 0x%X - DXGI "
                    "rejects any disagreement in the frame-latency waitable bit with E_INVALIDARG",
                    source, SwapChainFlags, reconciled, scDesc.Flags);
            }
            SwapChainFlags = reconciled;
        }
    }

    const auto& cfg = GetActiveGraphicsConfig();
    // Same presentation-ownership rule as the pacing wait: while the CE Vulkan
    // layer owns presentation this swapchain is the Vulkan runtime's transport,
    // and the resize must forward the runtime's own BufferCount byte-for-byte.
    if (!ce::present_pacing_policy::ShouldApplyCePresentationPolicy(IsVulkanActive()) ||
        !HasBackbufferCountOverride(cfg.backbufferCount)) {
        return;
    }
    if (BufferCount == 0) {
        return;
    }

    const UINT requested = static_cast<UINT>(cfg.backbufferCount);
    if (requested == BufferCount) {
        return;
    }
    if (haveDesc && ce::swapchain_flag_policy::IsFlipSwapEffect(scDesc.SwapEffect) && requested < BufferCount) {
        HookLog("%s: Keeping the application's BufferCount %u above the configured %u (flip model)", source,
                BufferCount, requested);
        return;
    }
    HookLogImportant("%s: Overriding BufferCount %u -> %u", source, BufferCount, requested);
    BufferCount = requested;
}

// Before a D3D12 resize is forwarded: CE's shared capture lets go of the
// chain (DXGI refuses a resize while its back buffers carry references - see
// SharedCaptureD3D12::ReleaseForSwapChainResize), and the back buffers are
// probed before and after that release (resize_reference_probe.h). Resizes are
// rare, so every one is probed; the log is bounded for a title that resizes in
// a loop. Chains of other APIs probe empty and log nothing.
struct D3D12ResizePreparation {
    ce::resize_reference_probe::BackBufferReferences before;
    ce::resize_reference_probe::BackBufferReferences afterCaptureRelease;
    UINT bufferCount = 0;
    bool captureReleased = false;
    char capture[160] = {};
    char captureRelease[128] = {};
};

D3D12ResizePreparation PrepareD3D12Resize(IDXGISwapChain* pSwapChain) {
    D3D12ResizePreparation preparation;
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (!pSwapChain || FAILED(pSwapChain->GetDesc(&desc))) {
        return preparation;
    }
    preparation.bufferCount = desc.BufferCount;
    preparation.before =
        ce::resize_reference_probe::Probe<ID3D12Resource>(pSwapChain, desc.BufferCount, __uuidof(ID3D12Resource));
    if (preparation.before.probed == 0) {
        return preparation;  // not a D3D12 chain: CE's D3D12 capture cannot be bound to it
    }
    DX12_DescribeCaptureBindingForResize(pSwapChain, preparation.capture, sizeof(preparation.capture));
    preparation.captureReleased = DX12_ReleaseCaptureForSwapChainResize(pSwapChain, preparation.captureRelease,
                                                                        sizeof(preparation.captureRelease));
    if (preparation.captureReleased) {
        preparation.afterCaptureRelease = ce::resize_reference_probe::Probe<ID3D12Resource>(
            pSwapChain, desc.BufferCount, __uuidof(ID3D12Resource));
    }
    return preparation;
}

void EndD3D12ResizeDiagnostics(const D3D12ResizePreparation& preparation, IDXGISwapChain* pSwapChain, HRESULT hr,
                               const char* source, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat,
                               UINT SwapChainFlags) {
    if (preparation.before.probed == 0) {
        return;
    }
    if (SUCCEEDED(hr)) {
        // New buffers: count who takes and returns their references from here on.
        // try_lock: a CE path that resizes while it owns the vtable lock must
        // not deadlock on its own diagnostics.
        std::unique_lock<std::mutex> vtableLock(g_SharedMutex, std::try_to_lock);
        if (vtableLock.owns_lock()) {
            BackBufferReferenceTrace_Track(pSwapChain, preparation.bufferCount, source);
        } else {
            HookLog("%s: back-buffer reference trace not re-armed - vtable lock busy", source);
        }
    } else {
        BackBufferReferenceTrace_Log(pSwapChain, source);
    }
    static std::atomic<uint32_t> s_succeededLogs{0};
    static std::atomic<uint32_t> s_failedLogs{0};
    const uint32_t logIndex = (FAILED(hr) ? s_failedLogs : s_succeededLogs).fetch_add(1, std::memory_order_relaxed);
    if (logIndex >= (FAILED(hr) ? 32u : 16u) && (logIndex % 64) != 0) {
        return;
    }
    char before[128] = {};
    ce::resize_reference_probe::Format(preparation.before, before, sizeof(before));
    char afterCaptureRelease[128] = "n/a";
    if (preparation.captureReleased) {
        ce::resize_reference_probe::Format(preparation.afterCaptureRelease, afterCaptureRelease,
                                           sizeof(afterCaptureRelease));
    }
    char after[128] = "n/a";
    bool heldAfterFailure = false;
    if (FAILED(hr)) {
        // Still the old buffers: the probe shows whether the holder let go.
        const auto afterReferences = ce::resize_reference_probe::Probe<ID3D12Resource>(
            pSwapChain, preparation.bufferCount, __uuidof(ID3D12Resource));
        ce::resize_reference_probe::Format(afterReferences, after, sizeof(after));
        for (UINT i = 0; i < afterReferences.probed; ++i) {
            heldAfterFailure |= afterReferences.heldByOthers[i] != 0;
        }
    }
    HookLogImportant(
        "%s: D3D12 resize %s hr=0x%08lX sc=%p request(count=%u %ux%u fmt=%d flags=0x%X) buffers=%u "
        "backBufferRefsHeldByOthers before=%s afterCaptureRelease=%s after=%s %s %s tid=0x%04lX log=%u",
        source, FAILED(hr) ? "FAILED" : "ok", static_cast<unsigned long>(hr), pSwapChain, BufferCount, Width, Height,
        static_cast<int>(NewFormat), SwapChainFlags, preparation.bufferCount, before, afterCaptureRelease, after,
        preparation.capture, preparation.captureReleased ? preparation.captureRelease : "captureRelease(unbound)",
        GetCurrentThreadId(), logIndex + 1);
    // The first successes give the baseline a refused resize is compared against.
    static std::atomic<uint32_t> s_slotOwnerBaselines{0};
    if (FAILED(hr) || s_slotOwnerBaselines.fetch_add(1, std::memory_order_relaxed) < 2) {
        LogSwapchainVTableSlotOwners(pSwapChain, source, FAILED(hr) ? "at the refused resize" : "after a resize");
    }

    static std::atomic<bool> s_holdersScanned{false};
    if (ce::resize_reference_holders::ShouldScanFailedResize(hr, heldAfterFailure,
                                                             s_holdersScanned.load(std::memory_order_acquire)) &&
        !s_holdersScanned.exchange(true, std::memory_order_acq_rel)) {
        void* buffers[ce::resize_reference_holders::kMaxTargets] = {};
        UINT bufferCount = 0;
        const UINT wanted = preparation.bufferCount < ce::resize_reference_holders::kMaxTargets
                                ? preparation.bufferCount
                                : static_cast<UINT>(ce::resize_reference_holders::kMaxTargets);
        for (; bufferCount < wanted; ++bufferCount) {
            ID3D12Resource* buffer = nullptr;
            if (FAILED(pSwapChain->GetBuffer(bufferCount, IID_PPV_ARGS(&buffer))) || !buffer) {
                break;
            }
            buffers[bufferCount] = buffer;
        }
        LogBackBufferReferenceHolders(buffers, bufferCount, source);
        for (UINT i = 0; i < bufferCount; ++i) {
            static_cast<ID3D12Resource*>(buffers[i])->Release();
        }
    }
}

}  // namespace

// Trampolines of the reconcile-only ResizeBuffers body hooks on dxgi's own
// functions (the vtable slots stay pristine; see
// InstallResizeReconciliationHooks). Kept separate from the
// full DX11 resize detour: the only thing CE owes an application swapchain it
// otherwise leaves alone is that the flags it added at creation stay invisible,
// and running the whole resize pipeline for that would change behaviour far
// beyond the fix.
PFN_ResizeBuffers dxgi_shared_oResizeBuffersReconcile = nullptr;
PFN_ResizeBuffers1 dxgi_shared_oResizeBuffers1Reconcile = nullptr;

HRESULT STDMETHODCALLTYPE DetourResizeBuffersReconcileOnly(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width,
                                                           UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    if (!dxgi_shared_oResizeBuffersReconcile) {
        return DXGI_ERROR_INVALID_CALL;
    }
    if (IsShuttingDown()) {
        return dxgi_shared_oResizeBuffersReconcile(pSwapChain, BufferCount, Width, Height, NewFormat,
                                                   SwapChainFlags);
    }
    ReconcileApplicationResizeRequest(pSwapChain, BufferCount, SwapChainFlags, "ResizeBuffers");
    const D3D12ResizePreparation preparation = PrepareD3D12Resize(pSwapChain);
    const HRESULT hr =
        dxgi_shared_oResizeBuffersReconcile(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    EndD3D12ResizeDiagnostics(preparation, pSwapChain, hr, "ResizeBuffers", BufferCount, Width, Height, NewFormat,
                              SwapChainFlags);
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourResizeBuffers1ReconcileOnly(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width,
                                                            UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags,
                                                            const UINT* pCreationNodeMask,
                                                            IUnknown* const* ppPresentQueue) {
    if (!dxgi_shared_oResizeBuffers1Reconcile) {
        return DXGI_ERROR_INVALID_CALL;
    }
    if (IsShuttingDown()) {
        return dxgi_shared_oResizeBuffers1Reconcile(pSwapChain, BufferCount, Width, Height, NewFormat,
                                                    SwapChainFlags, pCreationNodeMask, ppPresentQueue);
    }
    ReconcileApplicationResizeRequest(pSwapChain, BufferCount, SwapChainFlags, "ResizeBuffers1");
    const D3D12ResizePreparation preparation = PrepareD3D12Resize(pSwapChain);
    const HRESULT hr = dxgi_shared_oResizeBuffers1Reconcile(pSwapChain, BufferCount, Width, Height, NewFormat,
                                                            SwapChainFlags, pCreationNodeMask, ppPresentQueue);
    EndD3D12ResizeDiagnostics(preparation, pSwapChain, hr, "ResizeBuffers1", BufferCount, Width, Height, NewFormat,
                              SwapChainFlags);
    return hr;
}

}  // namespace DXGIShared

namespace DXGIShared {
HRESULT STDMETHODCALLTYPE DetourResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height,
                                              DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    // CRITICAL: Check for global shutdown - if app is closing, don't touch
    // anything
    if (IsShuttingDown()) {
        if (dxgi_shared_oResizeBuffers) {
            return dxgi_shared_oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
        }
        return DXGI_ERROR_INVALID_CALL;
    }

    ReconcileApplicationResizeRequest(pSwapChain, BufferCount, SwapChainFlags, "DetourResizeBuffers");

    // CRITICAL FIX: When Vulkan is active, pass through DXGI ResizeBuffers calls
    if (IsVulkanActive()) {
        return dxgi_shared_oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }

    // AGGRESSIVE RECURSION GUARD: Steam overlay causes infinite recursion through
    // hook chain
    if (IsRecursiveResize()) {
        // Recursion detected - resume at CE's saved predecessor rather than
        // re-reading vtable[13]: that slot can be CE's own detour once the
        // reconciliation claim is installed, and re-entering it recurses forever.
        return dxgi_shared_oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }

    if (g_SharedState.wrapperResizeDepth.fetch_add(1) > 0) {
        DX12_ReleaseCaptureForSwapChainResize(pSwapChain, nullptr, 0);
        HRESULT hr = dxgi_shared_oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
        g_SharedState.wrapperResizeDepth.fetch_sub(1);
        ReleaseResize();
        return hr;
    }

    // Check if this is our wrapper swapchain - if so, skip resize handling
    void* pWrapperTest = nullptr;
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_CWrapDXGISwapChain, &pWrapperTest))) {
        ((IUnknown*)pWrapperTest)->Release();
        DX12_ReleaseCaptureForSwapChainResize(pSwapChain, nullptr, 0);
        HRESULT hr = dxgi_shared_oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
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
        HookLog("DXGI: ResizeBuffers - FIRST D3D12 resize, calling CE's saved predecessor");
        // Not vtable[13]: that slot can be CE's own detour once the
        // reconciliation claim is installed, which would recurse forever.
        // A recording can already copy from the chain here.
        const D3D12ResizePreparation preparation = PrepareD3D12Resize(pSwapChain);
        HRESULT hr = dxgi_shared_oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
        EndD3D12ResizeDiagnostics(preparation, pSwapChain, hr, "DetourResizeBuffers(first)", BufferCount, Width,
                                  Height, NewFormat, SwapChainFlags);
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
    const D3D12ResizePreparation preparation =
        api == APIType::D3D12 ? PrepareD3D12Resize(pSwapChain) : D3D12ResizePreparation{};
    HRESULT hr = dxgi_shared_oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    HookLog("DXGI: ResizeBuffers - oResizeBuffers returned hr=0x%08X", hr);
    EndD3D12ResizeDiagnostics(preparation, pSwapChain, hr, "DetourResizeBuffers", BufferCount, Width, Height,
                              NewFormat, SwapChainFlags);

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
}

namespace DXGIShared {
HRESULT STDMETHODCALLTYPE DetourResizeBuffers1(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height,
                                               DXGI_FORMAT NewFormat, UINT SwapChainFlags,
                                               const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue) {
    if (IsShuttingDown()) {
        return dxgi_shared_oResizeBuffers1
                   ? dxgi_shared_oResizeBuffers1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags,
                                                 pCreationNodeMask, ppPresentQueue)
                   : DXGI_ERROR_INVALID_CALL;
    }
    ReconcileApplicationResizeRequest(pSwapChain, BufferCount, SwapChainFlags, "DetourResizeBuffers1");

    // Vulkan passthrough
    if (IsVulkanActive()) {
        return dxgi_shared_oResizeBuffers1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags, pCreationNodeMask,
                               ppPresentQueue);
    }

    // AGGRESSIVE RECURSION GUARD: Steam overlay causes infinite recursion through
    // hook chain
    if (IsRecursiveResize()) {
        // Recursion detected - resume at CE's saved predecessor rather than
        // re-reading vtable[39]: that slot can be CE's own detour once the
        // reconciliation claim is installed, and re-entering it recurses forever.
        return dxgi_shared_oResizeBuffers1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags,
                                           pCreationNodeMask, ppPresentQueue);
    }

    if (g_SharedState.wrapperResizeDepth.fetch_add(1) > 0) {
        DX12_ReleaseCaptureForSwapChainResize(pSwapChain, nullptr, 0);
        HRESULT hr = dxgi_shared_oResizeBuffers1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags,
                                     pCreationNodeMask, ppPresentQueue);
        g_SharedState.wrapperResizeDepth.fetch_sub(1);
        ReleaseResize();
        return hr;
    }

    // Check if this is our wrapper swapchain - if so, skip resize handling
    void* pWrapperTest = nullptr;
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_CWrapDXGISwapChain, &pWrapperTest))) {
        ((IUnknown*)pWrapperTest)->Release();
        DX12_ReleaseCaptureForSwapChainResize(pSwapChain, nullptr, 0);
        HRESULT hr = dxgi_shared_oResizeBuffers1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags,
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
        HookLog("DXGI: ResizeBuffers1 - FIRST D3D12 resize, calling CE's saved predecessor");
        // Not vtable[13]: that is ResizeBuffers, so it silently dropped this
        // call's node mask and present queues, and it can now be CE's own detour.
        // A recording can already copy from the chain here.
        const D3D12ResizePreparation preparation = PrepareD3D12Resize(pSwapChain);
        HRESULT hr = dxgi_shared_oResizeBuffers1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags,
                                                 pCreationNodeMask, ppPresentQueue);
        EndD3D12ResizeDiagnostics(preparation, pSwapChain, hr, "DetourResizeBuffers1(first)", BufferCount, Width,
                                  Height, NewFormat, SwapChainFlags);
        HookLog("DXGI: ResizeBuffers1 - first D3D12 resize returned hr=0x%08X", hr);
        g_SharedState.wrapperResizeDepth.fetch_sub(1);
        ReleaseResize();
        return hr;
    }

    if (api == APIType::D3D12)
        HandleDX12ResizeBegin();
    else if (api == APIType::D3D11)
        HandleDX11ResizeBegin();

    const D3D12ResizePreparation preparation =
        api == APIType::D3D12 ? PrepareD3D12Resize(pSwapChain) : D3D12ResizePreparation{};
    HRESULT hr = dxgi_shared_oResizeBuffers1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags, pCreationNodeMask,
                                 ppPresentQueue);
    EndD3D12ResizeDiagnostics(preparation, pSwapChain, hr, "DetourResizeBuffers1", BufferCount, Width, Height,
                              NewFormat, SwapChainFlags);

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
}
