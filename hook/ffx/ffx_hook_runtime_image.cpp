#include "ffx_hook_internal.h"

#include <atomic>
#include <mutex>

#include "common/logging/log_meter.h"

// The FFX runtime image CE observes when two share a name.
//
// Init() finds the runtime by name (GetModuleHandleW), which names the first image loaded under it. A tool that ships
// its own FidelityFX copy beside the game's loads a second, same-named image: OptiScaler (FGOutput=FSRFG) keeps
// amd_fidelityfx_framegeneration_dx12.dll 4.0.1 in its folder next to the engine plugin's 4.0.0, resolves its
// exports through the real kernelbase GetProcAddress, and generates with it (20261023_221541). CE hooked the
// engine's image, never saw OptiScaler's create or configure, kept the protected FFX startup quiesced and drew no
// overlay. The image whose code created a frame-generation swapchain is the runtime in use, so the create hooks name
// it here and the next hook-thread pass moves CE's FFX hooks to it. That pass, not the create: CE stays out of
// AMD's swapchain create beyond recording (the protected-startup rule).

namespace {

std::mutex g_creatorMutex;
char g_creatorPath[MAX_PATH] = {};
std::atomic<bool> g_creatorPending{false};
std::atomic<HMODULE> g_preferredImage{nullptr};
std::atomic<uintptr_t> g_preferredImageEnd{0};

bool IsLiveImage(HMODULE module) {
    HMODULE owner = nullptr;
    return module &&
           GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(module), &owner) &&
           owner == module;
}

}  // namespace

HMODULE ffx_hook_PreferredRuntimeImageFor(const char* baseName) {
    HMODULE preferred = g_preferredImage.load(std::memory_order_acquire);
    if (!preferred || !baseName) {
        return nullptr;
    }
    if (!IsLiveImage(preferred)) {
        g_preferredImage.compare_exchange_strong(preferred, nullptr, std::memory_order_acq_rel);
        return nullptr;
    }
    char path[MAX_PATH] = {};
    if (!GetModuleFileNameA(preferred, path, sizeof(path))) {
        return nullptr;
    }
    const char* name = ce::overlay_compat::ExtractModuleBaseName(path);
    return name && _stricmp(name, baseName) == 0 ? preferred : nullptr;
}

void ffx_hook_ForgetPreferredRuntimeImage(const void* moduleBase, size_t moduleSizeBytes) {
    HMODULE preferred = g_preferredImage.load(std::memory_order_acquire);
    if (preferred && FFXHook::detail::IsAddressInImage(preferred, moduleBase, moduleSizeBytes)) {
        g_preferredImage.compare_exchange_strong(preferred, nullptr, std::memory_order_acq_rel);
    }
}

namespace FFXHook {

void NoteFrameGenerationSwapchainCreator(const char* modulePath) {
    if (!modulePath || !modulePath[0]) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_creatorMutex);
        strncpy_s(g_creatorPath, sizeof(g_creatorPath), modulePath, _TRUNCATE);
    }
    g_creatorPending.store(true, std::memory_order_release);
}

void NoteRuntimeImageLoaded(HMODULE module, const char* moduleNameOrPath) {
    if (!module || !moduleNameOrPath || HookIsShuttingDown()) {
        return;
    }
    const char* baseName = ce::overlay_compat::ExtractModuleBaseName(moduleNameOrPath);
    // The same eligibility ffx_hook_InstallHooksForModule arms the create breakpoint with.
    if (!baseName || ce::ffx_api::ShouldInlineHookFFXExportsForModule(baseName) ||
        !ce::ffx_api::ShouldArmProtectedOfficialFFXConfigureBreakpoint(baseName)) {
        return;
    }
    const auto create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(module, "ffxCreateContext"));
    if (create) {
        ArmFfxCreateContextBreakpoint(module, create, baseName, "image load");
    }
}

void ServicePendingRuntimeRetarget() {
    if (!g_creatorPending.exchange(false, std::memory_order_acq_rel) || HookIsShuttingDown()) {
        return;
    }
    char path[MAX_PATH] = {};
    {
        std::lock_guard<std::mutex> lock(g_creatorMutex);
        strncpy_s(path, sizeof(path), g_creatorPath, _TRUNCATE);
    }
    HMODULE creator = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, path, &creator) || !creator) {
        HookLogImportant("FFX Hook: the frame-generation swapchain's creator %s is no loaded image any more", path);
        return;
    }
    HMODULE observed = nullptr;
    {
        std::lock_guard<std::mutex> lock(ffx_hook_g_InitMutex);
        observed = ffx_hook_g_HookedModule;
    }
    g_preferredImage.store(creator, std::memory_order_release);
    if (observed == creator) {
        return;
    }
    char observedPath[MAX_PATH] = {};
    if (!observed || !GetModuleFileNameA(observed, observedPath, sizeof(observedPath))) {
        strncpy_s(observedPath, sizeof(observedPath), "none", _TRUNCATE);
    }
    static std::atomic<uint32_t> s_retargets{0};
    const uint32_t retargets = s_retargets.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ce::log_meter::ShouldLogCadence(retargets, 10, 100)) {
        HookLogImportant(
            "FFX Hook: the frame-generation swapchain was created by %s (%p) while CE observed %s (%p) - moving the "
            "FFX hooks to the creating runtime (retargets=%u)",
            path, creator, observedPath, observed, retargets);
    }
    Init();
}

}  // namespace FFXHook
