#include "layer_main.h"

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

#include "common/platform/secure_dll_loading.h"
#include "common/graphics/vulkan_layer_host_directory.h"

namespace {

using WaitForBootstrapFn = BOOL (*)(DWORD);

// 0 = not decided, 1 = the hook is up, -1 = the hook is loaded but never became
// ready. "Could not load the hook" is deliberately not latched: nothing was
// learned about the hook, and the pointer file it depends on can appear later.
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

// The install directory the controller recorded beside this staged layer, empty
// when there is none. `error` receives the reason for an empty answer.
std::wstring ReadStagedHostDirectory(const std::filesystem::path& layerDirectory, DWORD* error) {
    *error = ERROR_SUCCESS;
    const std::filesystem::path pointerPath =
        layerDirectory / ce::vulkan_layer_host_directory::kPointerFileName;
    // Full sharing: the controller replaces this file at start-up, and neither
    // side may make the other's access fail.
    HANDLE file = CreateFileW(pointerPath.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        return {};
    }
    std::string contents(ce::vulkan_layer_host_directory::kMaxPointerFileBytes + 1, '\0');
    DWORD read = 0;
    const BOOL ok = ReadFile(file, contents.data(), static_cast<DWORD>(contents.size()), &read, nullptr);
    const DWORD readError = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!ok) {
        *error = readError;
        return {};
    }
    contents.resize(read);
    std::wstring directory = ce::vulkan_layer_host_directory::Parse(contents);
    if (directory.empty())
        *error = ERROR_BAD_PATHNAME;
    return directory;
}

HMODULE LoadHookFromCandidates(const std::vector<std::wstring>& candidates, DWORD* lastError) {
    *lastError = ERROR_FILE_NOT_FOUND;
    for (const std::wstring& candidate : candidates) {
        const DWORD attributes = GetFileAttributesW(candidate.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            // Named separately from a load failure: "absent" means the install
            // moved or the pointer is stale, "present but refused" means a
            // dependency or a security policy - different fixes.
            const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_FILE_NOT_FOUND;
            LayerLog("Inherited renderer bootstrap: hook candidate %ls is not a file (error=%lu)",
                     candidate.c_str(), static_cast<unsigned long>(error));
            *lastError = error;
            continue;
        }
        DWORD error = ERROR_SUCCESS;
        if (HMODULE hook = ce::security::LoadLibraryFromSecurePath(candidate, &error)) {
            LayerLog("Inherited renderer bootstrap: loaded %ls", candidate.c_str());
            return hook;
        }
        LayerLog("Inherited renderer bootstrap: %ls exists but LoadLibrary failed (error=%lu)",
                 candidate.c_str(), static_cast<unsigned long>(error));
        *lastError = error;
    }
    return nullptr;
}

}  // namespace

InheritedRendererBootstrap LayerBootstrapInheritedRendererHook() {
    const int prior = g_BootstrapResult.load(std::memory_order_acquire);
    if (prior != 0) {
        return prior > 0 ? InheritedRendererBootstrap::Ready : InheritedRendererBootstrap::NotReady;
    }

    const std::filesystem::path directory = GetLayerDirectory();
    if (directory.empty()) {
        LayerLog("Inherited renderer bootstrap: could not resolve the Vulkan layer directory - the process-local "
                 "graphics runtime overrides (dlss_*_dll_path, streamline_dll_path, dlss_*_preset, "
                 "dlss_debug_overlay) cannot be applied in this renderer");
        return InheritedRendererBootstrap::HookUnavailable;
    }

#ifdef _WIN64
    constexpr wchar_t kHookName[] = L"capture_hook_x64.dll";
#else
    constexpr wchar_t kHookName[] = L"capture_hook_x86.dll";
#endif
    HMODULE hook = GetModuleHandleW(kHookName);
    if (!hook) {
        // The layer is a staged copy and the hook is not staged with it: the
        // controller records where the installed hook lives beside the layer.
        DWORD pointerError = ERROR_SUCCESS;
        const std::wstring hostDirectory = ReadStagedHostDirectory(directory, &pointerError);
        if (hostDirectory.empty()) {
            LayerLog("Inherited renderer bootstrap: no usable host directory pointer %ls beside the layer "
                     "(error=%lu); falling back to the layer's own directory",
                     ce::vulkan_layer_host_directory::kPointerFileName, static_cast<unsigned long>(pointerError));
        }
        const std::vector<std::wstring> candidates = ce::vulkan_layer_host_directory::HookLoadCandidates(
            hostDirectory, directory.wstring(), kHookName);
        DWORD loadError = ERROR_FILE_NOT_FOUND;
        hook = LoadHookFromCandidates(candidates, &loadError);
        if (!hook) {
            LayerLog("Inherited renderer bootstrap: failed to load %ls (error=%lu, %lu candidate(s)) - the "
                     "process-local graphics runtime overrides (dlss_*_dll_path, streamline_dll_path, "
                     "dlss_*_preset, dlss_debug_overlay) will NOT apply in this renderer",
                     kHookName, static_cast<unsigned long>(loadError), static_cast<unsigned long>(candidates.size()));
            return InheritedRendererBootstrap::HookUnavailable;
        }
    }

    auto waitForBootstrap = reinterpret_cast<WaitForBootstrapFn>(
        GetProcAddress(hook, "CE_WaitForInheritedRendererBootstrap"));
    if (!waitForBootstrap) {
        LayerLog("Inherited renderer bootstrap: hook DLL lacks the readiness export");
        g_BootstrapResult.store(-1, std::memory_order_release);
        return InheritedRendererBootstrap::NotReady;
    }

    // This is an event-backed initialization handshake, not a scheduling delay:
    // Vulkan device creation must not race the process-local runtime hooks. The
    // timeout is only failure containment if the hook cannot initialize.
    const bool ready = waitForBootstrap(15000) != FALSE;
    g_BootstrapResult.store(ready ? 1 : -1, std::memory_order_release);
    LayerLog("Inherited renderer bootstrap: process-local graphics runtime overrides %s",
             ready ? "ready before Vulkan initialization" : "FAILED to become ready");
    return ready ? InheritedRendererBootstrap::Ready : InheritedRendererBootstrap::NotReady;
}
