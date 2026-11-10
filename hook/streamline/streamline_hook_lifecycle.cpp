#include "streamline_hook_lifecycle.h"

#include <sl.h>

#include "hook/fg/fg_transition_trace.h"
#include "hook/metrics/perf_logger.h"
#include "hook/present/backbuffer_reference_trace.h"
#include "hook/present/swapchain_lifetime_witness.h"
#include "streamline_hook_internal.h"
#include "streamline_inline_hook_batch.h"
#include "streamline_unload_order.h"

namespace ce::streamline_lifecycle {
namespace {

template <typename Function>
struct LifecycleExport {
    explicit LifecycleExport(const char* exportName) : name(exportName) {}

    const char* name;
    Function* original = nullptr;
    std::atomic<void*> target{nullptr};
    std::atomic<bool> installed{false};
    log_meter::ChangeGate entryLog;
    log_meter::ChangeGate returnLog;

    Function* Original() {
        return reinterpret_cast<Function*>(
            InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(&original), nullptr, nullptr));
    }

    // True when the module's unload invalidated this slot.
    bool OnUnload(const void* base, size_t size) {
        if (!ce::streamline_runtime_policy::IsStreamlineHookSlotInvalidatedByModuleUnload(
                target.load(std::memory_order_acquire), reinterpret_cast<void*>(Original()), base, size)) {
            return false;
        }
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&original), nullptr);
        target.store(nullptr, std::memory_order_release);
        installed.store(false, std::memory_order_release);
        entryLog.Reset();
        returnLog.Reset();
        return true;
    }

    void Queue(HMODULE module, StreamlineInlineHookBatch& batch, Function* detour) {
        QueueAddress(reinterpret_cast<void*>(GetProcAddress(module, name)), batch, detour);
    }

    void QueueAddress(void* address, StreamlineInlineHookBatch& batch, Function* detour) {
        batch.Queue(address, reinterpret_cast<void*>(detour), original, installed, target, name,
                    StreamlineInlineHookBatch::ForeignEntryPolicy::kLeaveToOwner);
    }
};

LifecycleExport<PFun_slSetFeatureLoaded> g_SetFeatureLoaded{"slSetFeatureLoaded"};
LifecycleExport<PFun_slIsFeatureLoaded> g_IsFeatureLoaded{"slIsFeatureLoaded"};
LifecycleExport<PFun_slFreeResources> g_FreeResources{"slFreeResources"};
// Streamline 2.x sl.api/internal.h's PFunSwapchainDestroyedBefore. We only
// observe the provider's dispatch. The title and interposer own the callback.
LifecycleExport<void(IDXGISwapChain*)> g_SwapchainDestroyed{"slHookSwapChainDestroyed"};
std::atomic<HMODULE> g_InspectedDlssg{nullptr};

template <typename Function, typename Argument>
sl::Result Observe(LifecycleExport<Function>& api, sl::Feature feature, const Argument& argument, uint32_t value) {
    auto* original = api.Original();
    if (!original) {
        return sl::Result::eErrorInvalidState;
    }
    if (feature != sl::kFeatureDLSS_G || HookIsShuttingDown()) {
        return original(feature, argument);
    }
    const int64_t startUs = PerfLogger::GetQpcUs();
    if (const auto verdict = api.entryLog.Observe(value)) {
        HookLogImportant("[SL LIFECYCLE] %s enter feature=DLSS-G arg=%u qpcUs=%lld%s", api.name, value,
                         static_cast<long long>(startUs), log_meter::SuppressedNote(verdict.suppressed).c_str());
    }
    const sl::Result result = original(feature, argument);
    const uint32_t status = static_cast<uint32_t>(result);
    const auto elapsedUs = PerfLogger::GetQpcUs() - startUs;
    if (const auto verdict = api.returnLog.Observe(log_meter::FieldKey(value, status))) {
        HookLogImportant("[SL LIFECYCLE] %s returned feature=DLSS-G arg=%u result=%u elapsedUs=%lld%s",
                         api.name, value, status, static_cast<long long>(elapsedUs),
                         log_meter::SuppressedNote(verdict.suppressed).c_str());
        fg_session::RecordStreamlineFeatureLifecycle(api.name, value, status);
    }
    return result;
}

sl::Result SetFeatureLoaded(sl::Feature feature, bool loaded) {
    if (feature == sl::kFeatureDLSS_G &&
        ce::streamline_unload_order::OnSetFeatureLoaded(loaded) ==
            ce::streamline_unload_order::SetLoadedAction::kAnswerOk) {
        // Held (or a held unload withdrawn): the title asked for it and is told so; Streamline is not called, so its
        // own destroy dispatch for the DLSS-G chain still finds the plugin loaded. The lines of the title's call and
        // the forwarded one are the [SL UNLOAD ORDER] transitions.
        return sl::Result::eOk;
    }
    return Observe(g_SetFeatureLoaded, feature, loaded, loaded ? 1u : 0u);
}

sl::Result IsFeatureLoaded(sl::Feature feature, bool& loaded) {
    auto* original = g_IsFeatureLoaded.Original();
    if (!original) {
        return sl::Result::eErrorInvalidState;
    }
    if (feature == sl::kFeatureDLSS_G && !HookIsShuttingDown() && ce::streamline_unload_order::AnswerNotLoaded()) {
        loaded = false;
        return sl::Result::eOk;
    }
    return original(feature, loaded);
}

// The held unload, forwarded after the DLSS-G chain's proxy released the native chain.
void ForwardHeldUnload() {
    Observe(g_SetFeatureLoaded, sl::kFeatureDLSS_G, false, 0u);
}

bool DestroyHookInstalled() {
    return g_SwapchainDestroyed.installed.load(std::memory_order_acquire) &&
           g_SwapchainDestroyed.Original() != nullptr;
}

sl::Result FreeResources(sl::Feature feature, const sl::ViewportHandle& viewport) {
    return Observe(g_FreeResources, feature, viewport, static_cast<uint32_t>(viewport));
}

void SwapchainDestroyed(IDXGISwapChain* chain) {
    auto* original = g_SwapchainDestroyed.Original();
    if (!original) {
        return;
    }
    if (HookIsShuttingDown()) {
        original(chain);
        return;
    }
    const auto witness = swapchain_lifetime_witness::Describe(chain);
    if (witness.state != swapchain_lifetime_witness::ChainState::kAlive) {
        original(chain);
        ce::streamline_unload_order::NoteDestroyReturned(chain);
        return;
    }
    const auto enteredUs = PerfLogger::GetQpcUs();
    HookLogImportant("[SL LIFETIME] slHookSwapChainDestroyed enter sc=%p lifetime=%llu qpcUs=%lld",
                     chain, static_cast<unsigned long long>(witness.generation), static_cast<long long>(enteredUs));
    original(chain);
    HookLogImportant("[SL LIFETIME] slHookSwapChainDestroyed returned sc=%p lifetime=%llu elapsedUs=%lld",
                     chain, static_cast<unsigned long long>(witness.generation),
                     static_cast<long long>(PerfLogger::GetQpcUs() - enteredUs));
    if (HookTraceLoggingEnabled()) {
        BackBufferReferenceTrace_Log(chain, "slHookSwapChainDestroyed return", false);
    }
    ce::streamline_unload_order::NoteDestroyReturned(chain);
}

}  // namespace

bool IsDlssgCreatingSwapchain() {
    HMODULE plugin = nullptr;
    void* const target = g_SwapchainDestroyed.target.load(std::memory_order_acquire);
    if (!target || !DoesAddressBelongToLoadedModule(target, &plugin, nullptr, 0, nullptr)) {
        return false;
    }
    void* frames[32] = {};
    const USHORT count = CaptureStackBackTrace(0, 32, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        HMODULE owner = nullptr;
        if (DoesAddressBelongToLoadedModule(frames[i], &owner, nullptr, 0, nullptr) && owner == plugin) {
            return true;
        }
    }
    return false;
}

void QueueInterposerDiagnostics(HMODULE module, StreamlineInlineHookBatch& batch) {
    ce::streamline_unload_order::Callbacks callbacks;
    callbacks.forwardHeldUnload = &ForwardHeldUnload;
    callbacks.destroyHookInstalled = &DestroyHookInstalled;
    ce::streamline_unload_order::SetCallbacks(callbacks);
    g_SetFeatureLoaded.Queue(module, batch, &SetFeatureLoaded);
    g_IsFeatureLoaded.Queue(module, batch, &IsFeatureLoaded);
    g_FreeResources.Queue(module, batch, &FreeResources);
}

void QueueDlssgDestroyDiagnostic(HMODULE module, StreamlineInlineHookBatch& batch) {
    // Not trace-gated: the unload-order policy needs the destroy dispatch to be visible.
    if (g_InspectedDlssg.exchange(module, std::memory_order_acq_rel) == module) {
        return;
    }
    const auto lookup = reinterpret_cast<PFN_slGetPluginFunction>(GetProcAddress(module, "slGetPluginFunction"));
    HMODULE owner = nullptr;
    void* target = lookup ? lookup(g_SwapchainDestroyed.name) : nullptr;
    if (!target || !DoesAddressBelongToLoadedModule(target, &owner, nullptr, 0, nullptr) || owner != module) {
        HookLogImportant("[SL LIFETIME] slHookSwapChainDestroyed unavailable in DLSS-G module %p", module);
        return;
    }
    g_SwapchainDestroyed.QueueAddress(target, batch, &SwapchainDestroyed);
}

void OnModuleUnloaded(const void* moduleBase, size_t moduleSize) {
    if (g_SetFeatureLoaded.OnUnload(moduleBase, moduleSize)) {
        // The interposer is gone: a held unload has nothing to go to. Runs under the loader lock: the call takes a
        // short mutex and logs, like the rest of this function's callers.
        ce::streamline_unload_order::OnInterposerUnloaded();
    }
    g_IsFeatureLoaded.OnUnload(moduleBase, moduleSize);
    g_FreeResources.OnUnload(moduleBase, moduleSize);
    g_SwapchainDestroyed.OnUnload(moduleBase, moduleSize);
    HMODULE inspected = reinterpret_cast<HMODULE>(const_cast<void*>(moduleBase));
    g_InspectedDlssg.compare_exchange_strong(inspected, nullptr, std::memory_order_acq_rel);
}

}  // namespace ce::streamline_lifecycle
