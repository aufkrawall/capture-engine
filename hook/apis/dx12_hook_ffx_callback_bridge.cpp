#include "dx12_hook_internal.h"
#include "dx12_hook_ffx_shared.h"
#include "../common/callback_snapshot_cache.h"

namespace {
std::atomic<uint64_t> s_bridgeGeneration{1};
}

size_t DX12_GetFFXPresentCallbackBridgeCount() {
    std::lock_guard<std::mutex> lock(dx12_hook_g_FFXPresentCallbackBridgeMutex);
    return dx12_hook_g_FFXPresentCallbackBridges.size();
}

void DX12_ClearAllFFXPresentCallbackBridges() {
    std::lock_guard<std::mutex> lock(dx12_hook_g_FFXPresentCallbackBridgeMutex);
    dx12_hook_g_FFXPresentCallbackBridges.clear();
    s_bridgeGeneration.fetch_add(1, std::memory_order_release);
}

FFXPresentCallbackBridgeState DX12_ResolveFFXPresentCallbackBridge(void* bridgeKey) {
    using Cache = ce::CallbackSnapshotCache<FFXPresentCallbackBridgeState>;
    static thread_local Cache cache;
    return cache.Resolve(bridgeKey, s_bridgeGeneration, [](void* key) {
        std::lock_guard<std::mutex> lock(dx12_hook_g_FFXPresentCallbackBridgeMutex);
        Cache::Snapshot snapshot;
        const auto it = dx12_hook_g_FFXPresentCallbackBridges.find(key);
        if (it != dx12_hook_g_FFXPresentCallbackBridges.end())
            snapshot.state = it->second;
        snapshot.generation = s_bridgeGeneration.load(std::memory_order_relaxed);
        return snapshot;
    });
}

void DX12_SetFFXPresentCallbackBridge(void* bridgeKey, ce::ffx_api::PresentCallback originalCallback,
                                      void* originalUserContext) {
    if (!bridgeKey) {
        return;
    }

    std::lock_guard<std::mutex> lock(dx12_hook_g_FFXPresentCallbackBridgeMutex);
    auto it = dx12_hook_g_FFXPresentCallbackBridges.find(bridgeKey);
    if (originalCallback == &DX12_RenderOverlayViaFFXPresentCallback) {
        static std::atomic<int> s_selfBridgeLogCount{0};
        const int logCount = s_selfBridgeLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: Ignoring recursive FFX present-callback bridge original for key=%p "
                "(existing=%d originalUserCtx=%p log=%d)",
                bridgeKey, it != dx12_hook_g_FFXPresentCallbackBridges.end() ? 1 : 0, originalUserContext, logCount + 1);
        }
        if (it == dx12_hook_g_FFXPresentCallbackBridges.end()) {
            dx12_hook_g_FFXPresentCallbackBridges[bridgeKey] = {
                .originalCallback = nullptr,
                .originalUserContext = nullptr,
                .installed = true,
            };
            s_bridgeGeneration.fetch_add(1, std::memory_order_release);
        }
        return;
    }

    if (it != dx12_hook_g_FFXPresentCallbackBridges.end() && it->second.installed &&
        it->second.originalCallback == originalCallback && it->second.originalUserContext == originalUserContext) {
        return;
    }
    dx12_hook_g_FFXPresentCallbackBridges[bridgeKey] = {
        .originalCallback = originalCallback,
        .originalUserContext = originalUserContext,
        .installed = true,
    };
    s_bridgeGeneration.fetch_add(1, std::memory_order_release);
}

bool DX12_HasFFXPresentCallbackBridge(void* bridgeKey) {
    if (!bridgeKey) {
        return false;
    }

    std::lock_guard<std::mutex> lock(dx12_hook_g_FFXPresentCallbackBridgeMutex);
    const auto it = dx12_hook_g_FFXPresentCallbackBridges.find(bridgeKey);
    return it != dx12_hook_g_FFXPresentCallbackBridges.end() && it->second.installed;
}

bool DX12_HasFFXPresentCallbackBridgeWithOriginal(void* bridgeKey) {
    if (!bridgeKey) {
        return false;
    }

    std::lock_guard<std::mutex> lock(dx12_hook_g_FFXPresentCallbackBridgeMutex);
    const auto it = dx12_hook_g_FFXPresentCallbackBridges.find(bridgeKey);
    return it != dx12_hook_g_FFXPresentCallbackBridges.end() && it->second.installed && it->second.originalCallback != nullptr &&
           it->second.originalCallback != &DX12_RenderOverlayViaFFXPresentCallback;
}

bool DX12_IsFFXPresentCallbackBridgeCallback(ce::ffx_api::PresentCallback callback) {
    return callback == &DX12_RenderOverlayViaFFXPresentCallback;
}

void DX12_ClearFFXPresentCallbackBridge(void* bridgeKey) {
    if (!bridgeKey) {
        return;
    }

    std::lock_guard<std::mutex> lock(dx12_hook_g_FFXPresentCallbackBridgeMutex);
    if (dx12_hook_g_FFXPresentCallbackBridges.erase(bridgeKey) != 0) {
        s_bridgeGeneration.fetch_add(1, std::memory_order_release);
    }
}
