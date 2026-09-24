/**
 * VK_LAYER_CE_gate - the library CaptureEngine's implicit-layer manifest names.
 *
 * An implicit layer is loaded into every Vulkan process on the machine, and the
 * loader has to map its library just to ask whether it wants to be there. When
 * that library was the full layer (VK_LAYER_CE_overlay.dll, ~1.5 MB), every
 * Vulkan application - Explorer, browsers, chat clients, anti-cheat-protected
 * games - mapped it, pulled in dxgi/user32/gdi32/winmm/version, ran its C++
 * static initialization, and kept the file locked, only to be declined.
 *
 * This gate is what those processes see instead. It imports only system DLLs
 * every process already has, decides participation with the same code the full
 * layer uses (layer_participation.cpp), and only for an admitted process loads
 * the full layer from its own directory and hands the loader the full layer's
 * negotiation result. From then on the loader calls the full layer directly;
 * the gate sits in no call chain.
 *
 * The gate deliberately exports vkNegotiateLoaderLayerInterfaceVersion ONLY. A
 * loader whose negotiation fails, or an old one that falls back to looking up
 * vkGetInstanceProcAddr by name, then finds nothing to call and skips the layer
 * (Vulkan-Loader loader_create_instance_chain). A gate that exported the proc
 * addresses could be chained anyway, as a layer with nothing behind it.
 * tools/verify_vulkan_layer_exports.py enforces both halves on the artifact.
 */

#include <windows.h>

#include <vulkan/vk_layer.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "layer_participation.h"

namespace {

#if defined(_WIN64)
constexpr wchar_t kLayerLibraryName[] = L"VK_LAYER_CE_overlay.dll";
#else
constexpr wchar_t kLayerLibraryName[] = L"VK_LAYER_CE_overlay_x86.dll";
#endif

// Appends one line to the running host's session vulkan_layer_early.log, only
// when that host has debug logging on - the same rule the full layer's early log
// follows. Without a host there is no log location to write to, so a no-host
// decision is silent by design.
void GateLog(const char* fmt, ...) {
    char logsPath[MAX_PATH] = {};
    if (!ce::vulkan_layer_participation::ReadPublishedHostLogging(logsPath, sizeof(logsPath)) || logsPath[0] == '\0')
        return;
    char filePath[MAX_PATH + 32] = {};
    snprintf(filePath, sizeof(filePath), "%s\\vulkan_layer_early.log", logsPath);
    FILE* file = fopen(filePath, "a");
    if (!file)
        return;

    char line[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(file, "[%02d:%02d:%02d.%03d] [VulkanLayer-Gate] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            line);
    fclose(file);
}

// The full layer is staged next to the gate; never search for it.
HMODULE LoadLayerBesideGate(DWORD* error) {
    *error = 0;
    HMODULE gate = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&LoadLayerBesideGate), &gate)) {
        *error = GetLastError();
        return nullptr;
    }
    wchar_t path[MAX_PATH + 64] = {};
    const DWORD length = GetModuleFileNameW(gate, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        *error = length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
        return nullptr;
    }
    wchar_t* separator = wcsrchr(path, L'\\');
    if (!separator) {
        *error = ERROR_BAD_PATHNAME;
        return nullptr;
    }
    wcscpy(separator + 1, kLayerLibraryName);
    HMODULE layer = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!layer)
        *error = GetLastError();
    return layer;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(instance);
    return TRUE;
}

extern "C" __declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
    if (!pVersionStruct)
        return VK_ERROR_INITIALIZATION_FAILED;

    wchar_t processName[MAX_PATH] = {};
    ce::vulkan_layer_participation::GetCurrentProcessBaseNameWide(processName, _countof(processName));
    // Matching is UTF-16 end to end; the log keeps a UTF-8 spelling.
    char processNameUtf8[MAX_PATH * 4] = {};
    ce::vulkan_layer_participation::GetCurrentProcessBaseName(processNameUtf8, sizeof(processNameUtf8));
    const ce::vulkan_layer_participation::Decision decision =
        ce::vulkan_layer_participation::DecideParticipation(processName);
    if (!decision.participate) {
        GateLog("Declined '%s' (hostPublished=%d eligibleByHost=%d listedTarget=%d); the full layer is not loaded",
                processNameUtf8, decision.hostPublished ? 1 : 0, decision.eligibleByHost ? 1 : 0,
                decision.listedTarget ? 1 : 0);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    DWORD error = 0;
    HMODULE layer = LoadLayerBesideGate(&error);
    if (!layer) {
        GateLog("Admitted '%s' but %ls could not be loaded (error=%lu); declining", processNameUtf8, kLayerLibraryName,
                error);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    auto negotiate = reinterpret_cast<PFN_vkNegotiateLoaderLayerInterfaceVersion>(
        GetProcAddress(layer, "vkNegotiateLoaderLayerInterfaceVersion"));
    const VkResult result = negotiate ? negotiate(pVersionStruct) : VK_ERROR_INITIALIZATION_FAILED;
    if (result != VK_SUCCESS) {
        // The full layer declined (a host appeared and says otherwise) and did
        // not pin itself, so this reference is the last one.
        FreeLibrary(layer);
        GateLog("Admitted '%s' but the full layer declined negotiation (result=%d)", processNameUtf8,
                static_cast<int>(result));
        return result;
    }
    // The full layer pinned itself on success; this reference is never
    // released, because the loader now holds the full layer's entry points.
    GateLog("Admitted '%s' (hostPublished=%d eligibleByHost=%d listedTarget=%d); full layer negotiated", processNameUtf8,
            decision.hostPublished ? 1 : 0, decision.eligibleByHost ? 1 : 0, decision.listedTarget ? 1 : 0);
    return VK_SUCCESS;
}
