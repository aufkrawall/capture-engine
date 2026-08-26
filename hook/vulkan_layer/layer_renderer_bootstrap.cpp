#include "layer_main.h"

#include <atomic>
#include <filesystem>

namespace {

using WaitForBootstrapFn = BOOL (*)(DWORD);
std::atomic<int> g_BootstrapResult{0};

std::filesystem::path GetLayerDirectory() {
    HMODULE layer = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&GetLayerDirectory), &layer)) {
        return {};
    }
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(layer, path, _countof(path));
    if (length == 0 || length >= _countof(path))
        return {};
    return std::filesystem::path(path).parent_path();
}

}  // namespace

bool LayerBootstrapInheritedRendererHook() {
    const int prior = g_BootstrapResult.load(std::memory_order_acquire);
    if (prior != 0)
        return prior > 0;

    const std::filesystem::path directory = GetLayerDirectory();
    if (directory.empty()) {
        LayerLog("Inherited renderer bootstrap: could not resolve the Vulkan layer directory");
        g_BootstrapResult.store(-1, std::memory_order_release);
        return false;
    }

#ifdef _WIN64
    constexpr wchar_t kHookName[] = L"capture_hook_x64.dll";
#else
    constexpr wchar_t kHookName[] = L"capture_hook_x86.dll";
#endif
    HMODULE hook = GetModuleHandleW(kHookName);
    DWORD loadError = ERROR_SUCCESS;
    if (!hook) {
        const std::filesystem::path hookPath = directory / kHookName;
        hook = LoadLibraryExW(hookPath.c_str(), nullptr,
                              LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        loadError = hook ? ERROR_SUCCESS : GetLastError();
    }
    if (!hook) {
        LayerLog("Inherited renderer bootstrap: failed to load %ls (error=%lu)", kHookName,
                 static_cast<unsigned long>(loadError));
        g_BootstrapResult.store(-1, std::memory_order_release);
        return false;
    }

    auto waitForBootstrap = reinterpret_cast<WaitForBootstrapFn>(
        GetProcAddress(hook, "CE_WaitForInheritedRendererBootstrap"));
    if (!waitForBootstrap) {
        LayerLog("Inherited renderer bootstrap: hook DLL lacks the readiness export");
        g_BootstrapResult.store(-1, std::memory_order_release);
        return false;
    }

    // This is an event-backed initialization handshake, not a scheduling delay:
    // Vulkan device creation must not race the process-local runtime hooks. The
    // timeout is only failure containment if the hook cannot initialize.
    const bool ready = waitForBootstrap(15000) != FALSE;
    g_BootstrapResult.store(ready ? 1 : -1, std::memory_order_release);
    LayerLog("Inherited renderer bootstrap: process-local graphics runtime overrides %s",
             ready ? "ready before Vulkan initialization" : "FAILED to become ready");
    return ready;
}
