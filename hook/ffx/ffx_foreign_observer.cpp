#include "ffx_foreign_observer.h"

#include <array>
#include <utility>
#include <vector>

#include "ffx_hook_internal.h"
#include "ffx_export_lifetime.h"
#include "common/logging/log_meter.h"

// OptiScaler EnableFfxInputs=true detours AMD's API and copies its Detours originals into the output runtime's
// dispatch table. Export-entry observation then sees the input, but never the native output configure/create.
// Observe below the foreign entry instead. No caller-owned function pointer or foreign patch is changed.
namespace {

constexpr size_t kSlots = 12;  // three exports in up to four concurrent FFX images
struct Observer {
    std::atomic<void*> target{nullptr};
    std::atomic<void*> trampoline{nullptr};
    std::atomic<bool> live{false};
    HMODULE module = nullptr;  // installation/shutdown under InitMutex
};
Observer g_observers[kSlots];
std::atomic<bool> g_hasObservers{false};

template <typename T>
class ScopedOriginal {
public:
    ScopedOriginal(T& slot, T value) : slot_(slot), previous_(slot) {
        slot_ = value;
    }
    ~ScopedOriginal() {
        slot_ = previous_;
    }
    ScopedOriginal(const ScopedOriginal&) = delete;
    ScopedOriginal& operator=(const ScopedOriginal&) = delete;

private:
    T& slot_;
    T previous_;
};

template <size_t I>
ffxReturnCode_t NativeCreate(ffxContext* context, ffxCreateContextDescHeader* desc, const ffxAllocationCallbacks* cb) {
    ScopedOriginal original(
        ffx_hook_t_FfxCreateContextOriginalOverride,
        reinterpret_cast<PfnFfxCreateContext>(g_observers[I].trampoline.load(std::memory_order_acquire)));
    return Hooked_ffxCreateContext(context, desc, cb);
}

template <size_t I>
ffxReturnCode_t NativeDestroy(ffxContext* context, const ffxAllocationCallbacks* cb) {
    ScopedOriginal original(
        ffx_hook_t_FfxDestroyContextOriginalOverride,
        reinterpret_cast<PfnFfxDestroyContext>(g_observers[I].trampoline.load(std::memory_order_acquire)));
    return Hooked_ffxDestroyContext(context, cb);
}

template <size_t I>
ffxReturnCode_t NativeConfigure(ffxContext* context, const ffxConfigureDescHeader* desc) {
    ScopedOriginal original(
        ffx_hook_t_FfxConfigureOriginalOverride,
        reinterpret_cast<PfnFfxConfigure>(g_observers[I].trampoline.load(std::memory_order_acquire)));
    return Hooked_ffxConfigure(context, desc);
}

template <size_t... I>
constexpr auto CreateWrappers(std::index_sequence<I...>) {
    return std::array<PfnFfxCreateContext, sizeof...(I)>{NativeCreate<I>...};
}
template <size_t... I>
constexpr auto DestroyWrappers(std::index_sequence<I...>) {
    return std::array<PfnFfxDestroyContext, sizeof...(I)>{NativeDestroy<I>...};
}
template <size_t... I>
constexpr auto ConfigureWrappers(std::index_sequence<I...>) {
    return std::array<PfnFfxConfigure, sizeof...(I)>{NativeConfigure<I>...};
}

void Publish(void* trampoline, void* context) {
    static_cast<Observer*>(context)->trampoline.store(trampoline, std::memory_order_release);
    if (trampoline)
        g_hasObservers.store(true, std::memory_order_release);
}

bool ObserveExport(HMODULE module, const char* name, unsigned arguments, unsigned kind) {
    void* target = reinterpret_cast<void*>(GetProcAddress(module, name));
    ce::ffx_export_lifetime::ModulePin pin(module, target, name);
    if (!pin.Get())
        return false;
    Observer* freeSlot = nullptr;
    for (auto& observer : g_observers) {
        if (observer.target.load(std::memory_order_acquire) == target && observer.live.load(std::memory_order_acquire))
            return observer.trampoline.load(std::memory_order_acquire) != nullptr;
        if (!observer.live.load(std::memory_order_acquire)) {
            // OnModuleUnloaded only invalidates atomics. Forget the departed image outside the loader lock;
            // a reload may already occupy the same address and must never receive a restore of the old bytes.
            if (void* old = observer.target.exchange(nullptr, std::memory_order_acq_rel))
                InlineHook::ForgetUnloadedDeepHook(old);
            if (!freeSlot)
                freeSlot = &observer;
        }
    }
    if (kind == 0)
        ffx_hook_UncoverForeignCreateEntry(target);
    if (kind == 2)
        ffx_hook_UncoverForeignConfigureEntry(target);
    if (!freeSlot || !InlineHook::HasForeignEntryHook(target))
        return false;

    const size_t index = static_cast<size_t>(freeSlot - g_observers);
    static constexpr auto creates = CreateWrappers(std::make_index_sequence<kSlots>{});
    static constexpr auto destroys = DestroyWrappers(std::make_index_sequence<kSlots>{});
    static constexpr auto configures = ConfigureWrappers(std::make_index_sequence<kSlots>{});
    void* wrapper = kind == 0   ? reinterpret_cast<void*>(creates[index])
                    : kind == 1 ? reinterpret_cast<void*>(destroys[index])
                                : reinterpret_cast<void*>(configures[index]);
    freeSlot->module = module;
    freeSlot->trampoline.store(nullptr, std::memory_order_release);
    freeSlot->target.store(target, std::memory_order_release);
    freeSlot->live.store(true, std::memory_order_release);
    void* trampoline = InlineHook::InstallDeepHookPublishedWithRetry("FFX Hook", name, target, wrapper, Publish,
                                                                     freeSlot, 0, arguments);
    if (!trampoline) {
        // A structural refusal cannot improve while this image and signature stay the same. Keep it known so
        // ordinary module scans do not repeatedly decode it and flood the log. Transient quiescence can retry.
        if (InlineHook::GetLastDeepHookQuiesceFailure() != ce::hook_patch::QuiesceFailure::kNone) {
            freeSlot->live.store(false, std::memory_order_release);
            freeSlot->target.store(nullptr, std::memory_order_release);
        }
    }
    static ce::log_meter::ChangeGate gate;
    if (const auto verdict = gate.Observe(ce::log_meter::FieldKey(target, trampoline != nullptr))) {
        HookLogImportant("FFX Hook: native %s observer %s below foreign entry at %p (module=%p)%s", name,
                         trampoline ? "installed" : "refused", target, module,
                         ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
    }
    return trampoline != nullptr;
}

}  // namespace

bool ffx_hook_ObserveForeignExports(HMODULE module, const char* moduleName) {
    if (!ce::ffx_api::IsOfficialAMDFFXRuntimeModuleName(moduleName) ||
        ce::ffx_api::IsVulkanFFXRuntimeModuleName(moduleName))
        return false;
    HMODULE pinned = nullptr;
    if (!module ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(module), &pinned))
        return false;
    if (pinned != module) {
        FreeLibrary(pinned);
        return false;
    }
    bool observed = ObserveExport(module, "ffxCreateContext", 3, 0);
    observed |= ObserveExport(module, "ffxDestroyContext", 2, 1);
    observed |= ObserveExport(module, "ffxConfigure", 2, 2);
    FreeLibrary(pinned);
    return observed;
}

void ffx_hook_ObserveLoadedForeignRuntimes() {
    // An input wrapper's runtime may already be resident before CE starts, under the game's same base name.
    // Observe all foreign-detoured images before the first context create, not just the name lookup's first image
    // or the creating image learned afterward. The unload callback still does only atomic invalidation.
    std::vector<HMODULE> modules(256);
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
                            &needed))
        return;
    if (needed > modules.size() * sizeof(HMODULE)) {
        modules.resize((needed + sizeof(HMODULE) - 1) / sizeof(HMODULE));
        if (!EnumProcessModules(GetCurrentProcess(), modules.data(),
                                static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &needed))
            return;
    }
    const size_t count = (std::min)(modules.size(), static_cast<size_t>(needed / sizeof(HMODULE)));
    for (size_t i = 0; i < count; ++i) {
        char path[MAX_PATH]{};
        if (GetModuleFileNameA(modules[i], path, sizeof(path)) && ce::ffx_api::IsOfficialAMDFFXRuntimeModuleName(path))
            ffx_hook_ObserveForeignExports(modules[i], path);
    }
}

bool ffx_hook_DelegatesToForeignObserver(const void* original) {
    if (!g_hasObservers.load(std::memory_order_acquire))
        return false;
    for (const auto& observer : g_observers) {
        if (observer.live.load(std::memory_order_acquire) &&
            observer.target.load(std::memory_order_acquire) == original &&
            observer.trampoline.load(std::memory_order_acquire))
            return true;
    }
    return false;
}

void* ffx_hook_NativeExportAnchor(const void* original) {
    if (!g_hasObservers.load(std::memory_order_acquire))
        return const_cast<void*>(original);
    for (const auto& observer : g_observers) {
        if (observer.live.load(std::memory_order_acquire) &&
            observer.trampoline.load(std::memory_order_acquire) == original)
            return observer.target.load(std::memory_order_acquire);
    }
    return const_cast<void*>(original);
}

void ffx_hook_InvalidateForeignObservers(const void* image, size_t size) {
    for (auto& observer : g_observers) {
        if (FFXHook::detail::IsAddressInImage(observer.target.load(std::memory_order_acquire), image, size))
            observer.live.store(false, std::memory_order_release);
    }
}

void ffx_hook_ShutdownForeignObservers() {
    for (auto& observer : g_observers) {
        void* target = observer.target.load(std::memory_order_acquire);
        if (!target)
            continue;
        if (observer.live.load(std::memory_order_acquire)) {
            // If removal refuses, keep the published original usable by the still-installed wrapper.
            if (observer.trampoline.load(std::memory_order_acquire) && !InlineHook::RemoveDeepHook(target))
                continue;
        } else {
            InlineHook::ForgetUnloadedDeepHook(target);
        }
        observer.live.store(false, std::memory_order_release);
        observer.target.store(nullptr, std::memory_order_release);
    }
}
