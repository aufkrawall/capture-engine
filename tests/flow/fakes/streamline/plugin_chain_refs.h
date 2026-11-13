#pragma once

// What the fake DLSS-G plugin (sl_dlss_g.cpp) holds on a real swapchain: its own reference to the chain and its
// buffers (real DLSS-G re-takes every buffer after a ResizeBuffers). It lets go only when the interposer reports the
// chain dying, by the destroy hook or, as on GTA's real switch, without it (sl_interposer.cpp). A chain never
// reported stays pinned and DXGI refuses the window's replacement swapchain with E_ACCESSDENIED (GTA V Enhanced
// DLSS FG -> FSR FG, 20261110_182518).

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace ce::flow::fake {

struct PluginChainState {
    Microsoft::WRL::ComPtr<IDXGISwapChain4> realRef;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> realBuffers;
};

namespace plugin_chain_refs_detail {
inline std::mutex& Mutex() {
    static std::mutex mutex;
    return mutex;
}
inline std::map<IDXGISwapChain*, std::shared_ptr<PluginChainState>>& Chains() {
    static std::map<IDXGISwapChain*, std::shared_ptr<PluginChainState>> chains;
    return chains;
}
}  // namespace plugin_chain_refs_detail

inline void TrackPluginChain(IDXGISwapChain* chain, std::shared_ptr<PluginChainState> state) {
    std::lock_guard<std::mutex> lock(plugin_chain_refs_detail::Mutex());
    plugin_chain_refs_detail::Chains()[chain] = std::move(state);
}

// Drops the plugin's references to `chain`, the way the real plugin's destroy hook does.
inline void ReleasePluginChain(IDXGISwapChain* chain) {
    std::shared_ptr<PluginChainState> state;
    {
        std::lock_guard<std::mutex> lock(plugin_chain_refs_detail::Mutex());
        auto& chains = plugin_chain_refs_detail::Chains();
        const auto found = chains.find(chain);
        if (found == chains.end())
            return;
        state = std::move(found->second);
        chains.erase(found);
    }
    state->realBuffers.clear();
    state->realRef.Reset();
}

}  // namespace ce::flow::fake
