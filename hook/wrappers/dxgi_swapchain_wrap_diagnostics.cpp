#include "dxgi_swapchain_wrap_internal.h"

#include "../common/dxgi_shared_internal.h"

namespace {

const char* ModuleBaseNameOfCodeAddress(const void* address, char* buffer, size_t bufferSize) {
    if (!buffer || bufferSize == 0) {
        return "unknown";
    }
    buffer[0] = '\0';
    if (!address) {
        return "null";
    }

    char modulePath[MAX_PATH] = {};
    if (!ce::overlay_compat::TryGetModulePathFromCodeAddress(address, modulePath, sizeof(modulePath))) {
        return "non-module";
    }

    const char* baseName = modulePath;
    for (const char* cursor = modulePath; *cursor; ++cursor) {
        if (*cursor == '\\' || *cursor == '/') {
            baseName = cursor + 1;
        }
    }
    snprintf(buffer, bufferSize, "%s", baseName);
    return buffer;
}

}  // namespace

// Diagnostic-only attribution of the real chain's lifetime state. Both call sites are rate-limited
// and read-only (the refcount probe is net-zero AddRef/Release). The vtable-slot ownership shows
// whether a foreign overlay hooked AddRef/Release/Present/ResizeBuffers on the chain — the missing
// piece for attributing the E_ACCESSDENIED pin (session 20260813_222058: 4 foreign refs remained
// after the game and CE released everything, and the replacement create never traverses a handler
// that releases them).
void LogSwapChainLifetimeDiagnostics(IDXGISwapChain* realChain, const char* stage) {
    if (!realChain || !stage) {
        return;
    }

    static std::atomic<int> s_lifetimeDiagnosticsLogCount{0};
    const int logCount = s_lifetimeDiagnosticsLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount >= 12 && (logCount % 256) != 0) {
        return;
    }

    MEMORY_BASIC_INFORMATION probeInfo = {};
    const bool objectCommitted =
        VirtualQuery(reinterpret_cast<const void*>(realChain), &probeInfo, sizeof(probeInfo)) != 0 &&
        probeInfo.State == MEM_COMMIT &&
        (probeInfo.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0;
    if (!objectCommitted) {
        WrapperLog("SwapChain: %s lifetime diagnostics — real chain %p memory not committed", stage, realChain);
        return;
    }

    ULONG liveRefs = 0;
    {
        ScopedAvGuard guard;
        liveRefs = realChain->AddRef();
        if (liveRefs > 0) {
            realChain->Release();
        }
    }

    char addRefOwner[MAX_PATH] = {};
    char releaseOwner[MAX_PATH] = {};
    char presentOwner[MAX_PATH] = {};
    char resizeBuffersOwner[MAX_PATH] = {};
    if (DXGIShared::IsReadableMemory(reinterpret_cast<const void*>(realChain), sizeof(void*))) {
        void** vtable = *reinterpret_cast<void***>(const_cast<IDXGISwapChain*>(realChain));
        if (DXGIShared::IsReadableMemory(reinterpret_cast<const void*>(vtable), 40 * sizeof(void*))) {
            ModuleBaseNameOfCodeAddress(vtable[1], addRefOwner, sizeof(addRefOwner));
            ModuleBaseNameOfCodeAddress(vtable[2], releaseOwner, sizeof(releaseOwner));
            ModuleBaseNameOfCodeAddress(vtable[8], presentOwner, sizeof(presentOwner));
            ModuleBaseNameOfCodeAddress(vtable[13], resizeBuffersOwner, sizeof(resizeBuffersOwner));
        }
    }

    WrapperLog(
        "SwapChain: %s lifetime diagnostics — real=%p realRefs=%u vtableSlots(addRef=%s release=%s present=%s "
        "resizeBuffers=%s)",
        stage, realChain, liveRefs, addRefOwner, releaseOwner, presentOwner, resizeBuffersOwner);
}
