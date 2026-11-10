#include "streamline_hook_lifecycle.h"

#include <sl.h>

#include "hook/fg/fg_transition_trace.h"
#include "hook/metrics/perf_logger.h"
#include "hook/present/backbuffer_reference_trace.h"
#include "hook/present/swapchain_lifetime_witness.h"
#include "streamline_hook_internal.h"
#include "streamline_inline_hook_batch.h"

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

    void OnUnload(const void* base, size_t size) {
        if (!ce::streamline_runtime_policy::IsStreamlineHookSlotInvalidatedByModuleUnload(
                target.load(std::memory_order_acquire), reinterpret_cast<void*>(Original()), base, size)) {
            return;
        }
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&original), nullptr);
        target.store(nullptr, std::memory_order_release);
        installed.store(false, std::memory_order_release);
        entryLog.Reset();
        returnLog.Reset();
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
    return Observe(g_SetFeatureLoaded, feature, loaded, loaded ? 1u : 0u);
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
        return;
    }
    const auto enteredUs = PerfLogger::GetQpcUs();
    HookLogImportant("[SL LIFETIME] slHookSwapChainDestroyed enter sc=%p lifetime=%llu qpcUs=%lld",
                     chain, static_cast<unsigned long long>(witness.generation), static_cast<long long>(enteredUs));
    original(chain);
    HookLogImportant("[SL LIFETIME] slHookSwapChainDestroyed returned sc=%p lifetime=%llu elapsedUs=%lld",
                     chain, static_cast<unsigned long long>(witness.generation),
                     static_cast<long long>(PerfLogger::GetQpcUs() - enteredUs));
    BackBufferReferenceTrace_Log(chain, "slHookSwapChainDestroyed return", false);
}

}  // namespace

void QueueInterposerDiagnostics(HMODULE module, StreamlineInlineHookBatch& batch) {
    g_SetFeatureLoaded.Queue(module, batch, &SetFeatureLoaded);
    g_FreeResources.Queue(module, batch, &FreeResources);
}

void QueueDlssgDestroyDiagnostic(HMODULE module, StreamlineInlineHookBatch& batch) {
    if (!HookTraceLoggingEnabled() || g_InspectedDlssg.exchange(module, std::memory_order_acq_rel) == module) {
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
    g_SetFeatureLoaded.OnUnload(moduleBase, moduleSize);
    g_FreeResources.OnUnload(moduleBase, moduleSize);
    g_SwapchainDestroyed.OnUnload(moduleBase, moduleSize);
    HMODULE inspected = reinterpret_cast<HMODULE>(const_cast<void*>(moduleBase));
    g_InspectedDlssg.compare_exchange_strong(inspected, nullptr, std::memory_order_acq_rel);
}

}  // namespace ce::streamline_lifecycle
