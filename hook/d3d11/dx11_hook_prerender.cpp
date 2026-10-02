#include "dx11_hook_internal.h"


float ResolveDX11PrerenderLimit() {
    return GetActivePrerenderLimit();
}

void ApplyPrerenderLimit(IDXGISwapChain* pSwapChain, float limit) {
    if (limit < 0.0f)
        return;

    ID3D11Device* dev = nullptr;
    if (FAILED(pSwapChain->GetDevice(IID_PPV_ARGS(&dev)))) {
        // D3D10 limits 1-6 use IDXGIDevice1::SetMaximumFrameLatency. DXGI has
        // no zero-depth value, so serialize limit 0 with a native event query.
        ID3D10Device* dev10 = nullptr;
        if (limit == 0.0f && SUCCEEDED(pSwapChain->GetDevice(IID_PPV_ARGS(&dev10))) && dev10) {
            std::lock_guard<std::mutex> prerenderLock(dx11_hook_g_PrerenderMutex);
            if (dx11_hook_g_PrerenderQueryDevice10 != dev10) {
                if (dx11_hook_g_PrerenderSerialQuery10) {
                    dx11_hook_g_PrerenderSerialQuery10->Release();
                    dx11_hook_g_PrerenderSerialQuery10 = nullptr;
                }
                if (dx11_hook_g_PrerenderQueryDevice10)
                    dx11_hook_g_PrerenderQueryDevice10->Release();
                dx11_hook_g_PrerenderQueryDevice10 = dev10;
                dx11_hook_g_PrerenderQueryDevice10->AddRef();
                HookLogImportant("D3D10: Serial prerender query rebound to device=%p", dev10);
            }
            if (!dx11_hook_g_PrerenderSerialQuery10) {
                D3D10_QUERY_DESC queryDesc = {};
                queryDesc.Query = D3D10_QUERY_EVENT;
                dev10->CreateQuery(&queryDesc, &dx11_hook_g_PrerenderSerialQuery10);
            }
            if (dx11_hook_g_PrerenderSerialQuery10) {
                dx11_hook_g_PrerenderSerialQuery10->End();
                dev10->Flush();
                const int64_t waitStart = PerfLogger::GetQpcUs();
                while (dx11_hook_g_PrerenderSerialQuery10->GetData(nullptr, 0, 0) == S_FALSE)
                    SwitchToThread();
                const int64_t waitUs = PerfLogger::GetQpcUs() - waitStart;
                const int idx = dx11_hook_g_DiagPrerenderWaits.fetch_add(1, std::memory_order_relaxed);
                if (idx < 12)
                    HookLogImportant("D3D10: Prerender serial wait=%lldus (#%d)", (long long)waitUs, idx + 1);
                dx11_hook_g_DiagPrerenderFrames.fetch_add(1, std::memory_order_relaxed);
            }
            dev10->Release();
            return;
        }
        if (dev10)
            dev10->Release();
        static std::atomic<int> s_nonD3D11LogCount{0};
        if (limit == 0.0f && s_nonD3D11LogCount.fetch_add(1, std::memory_order_relaxed) < 5)
            HookLogImportant("D3D10/11: Manual prerender query path unavailable for swapchain=%p", pSwapChain);
        return;
    }

    std::lock_guard<std::mutex> prerenderLock(dx11_hook_g_PrerenderMutex);
    if (dx11_hook_g_PrerenderQueryDevice != dev) {
        for (auto* query : dx11_hook_g_PrerenderQueries) {
            if (query)
                query->Release();
        }
        dx11_hook_g_PrerenderQueries.clear();
        dx11_hook_g_PrerenderFrameIndex = 0;
        if (dx11_hook_g_PrerenderQueryDevice)
            dx11_hook_g_PrerenderQueryDevice->Release();
        dx11_hook_g_PrerenderQueryDevice = dev;
        dx11_hook_g_PrerenderQueryDevice->AddRef();
        HookLogImportant("DX11: Prerender query stream rebound to device=%p", dev);
    }

    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);

    if (dx11_hook_g_PrerenderQueries.empty() || dx11_hook_g_PrerenderQueries[0] == nullptr) {
        dx11_hook_g_PrerenderQueries.clear();
        for (int i = 0; i < 16; i++) {
            D3D11_QUERY_DESC qd = {};
            qd.Query = D3D11_QUERY_EVENT;
            ID3D11Query* q = nullptr;
            if (SUCCEEDED(dev->CreateQuery(&qd, &q))) {
                dx11_hook_g_PrerenderQueries.push_back(q);
            }
        }
        HookLogImportant("DX11: Created manual prerender query ring buffer (size: %d, limit=%.2f)",
                         (int)dx11_hook_g_PrerenderQueries.size(), limit);
    }

    if (!dx11_hook_g_PrerenderQueries.empty()) {
        if (limit == 0.0f) {
            // Strict Serial: Wait for current frame
            ID3D11Query* q = dx11_hook_g_PrerenderQueries[dx11_hook_g_PrerenderFrameIndex % dx11_hook_g_PrerenderQueries.size()];
            ctx->End(q);
            int64_t waitStart = PerfLogger::GetQpcUs();
            while (ctx->GetData(q, nullptr, 0, 0) == S_FALSE) {
                SwitchToThread();
            }
            int64_t waitUs = PerfLogger::GetQpcUs() - waitStart;
            int idx = dx11_hook_g_DiagPrerenderWaits.fetch_add(1, std::memory_order_relaxed);
            if (idx < 12) {
                HookLogImportant("DX11: Prerender serial wait frame=%llu wait=%lldus (#%d)",
                                 (unsigned long long)dx11_hook_g_PrerenderFrameIndex, (long long)waitUs, idx + 1);
            }
        } else {
            const int lookback = std::clamp(static_cast<int>(limit), 1, 6);


            ID3D11Query* currentQ = dx11_hook_g_PrerenderQueries[dx11_hook_g_PrerenderFrameIndex % dx11_hook_g_PrerenderQueries.size()];
            ctx->End(currentQ);

            if (dx11_hook_g_PrerenderFrameIndex >= (uint64_t)lookback) {
                ID3D11Query* waitQ = dx11_hook_g_PrerenderQueries[(dx11_hook_g_PrerenderFrameIndex - lookback) % dx11_hook_g_PrerenderQueries.size()];
                int64_t waitStart = PerfLogger::GetQpcUs();
                while (ctx->GetData(waitQ, nullptr, 0, 0) == S_FALSE) {
                    SwitchToThread();
                }
                int64_t waitUs = PerfLogger::GetQpcUs() - waitStart;
                int idx = dx11_hook_g_DiagPrerenderWaits.fetch_add(1, std::memory_order_relaxed);
                if (idx < 12) {
                    HookLogImportant("DX11: Prerender buffered wait lookback=%d frame=%llu wait=%lldus (#%d)", lookback,
                                     (unsigned long long)dx11_hook_g_PrerenderFrameIndex, (long long)waitUs, idx + 1);
                }
            }
        }
        dx11_hook_g_PrerenderFrameIndex++;
        dx11_hook_g_DiagPrerenderFrames.fetch_add(1, std::memory_order_relaxed);
    }

    ctx->Release();
    dev->Release();
}
