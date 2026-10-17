#pragma once

// Closes a D3D11 `cpu_prerender_limit` frame: once the Present detour's forwarded Present returned,
// signals the fence marker ApplyPrerenderLimit left owed on this thread (dx11_hook_prerender.cpp).
// Declared in each Present detour before the call that applies the limit, so every route out of the
// detour - forwarded, bypassed or an early return - passes its destructor after the down-call.

#include "hook/d3d11/dx11_hook.h"
#include "hook/present/present_stage_cost.h"

namespace DXGIShared {

class D3D11PrerenderPresentScope {
public:
    D3D11PrerenderPresentScope() = default;
    ~D3D11PrerenderPresentScope() {
        if (D3D11PrerenderSignalPending()) {
            ce::present_stage_cost::StageScope stage(ce::present_stage_cost::Stage::kLimiter);
            D3D11PrerenderAfterPresent();
        }
    }
    D3D11PrerenderPresentScope(const D3D11PrerenderPresentScope&) = delete;
    D3D11PrerenderPresentScope& operator=(const D3D11PrerenderPresentScope&) = delete;
};

}  // namespace DXGIShared
