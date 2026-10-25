#pragma once

#include <dxgi.h>

namespace ce::translated_swapchain {

// Observes synchronous native creation inside an application-facing API create. Evidence is
// thread-local and requires one exact native swapchain for the same window; no COM refs are retained.
class CreateScope {
public:
    explicit CreateScope(HWND window, bool applicationApi = true);
    ~CreateScope();
    CreateScope(const CreateScope&) = delete;
    CreateScope& operator=(const CreateScope&) = delete;
    bool HasSingleNativeOwner() const;
    void Publish(IDXGISwapChain* applicationSwapchain) const;

private:
    friend void ObserveNativeCreate(IDXGISwapChain*, HWND, bool);
    CreateScope* previous_;
    HWND window_;
    bool applicationApi_;
    IDXGISwapChain* native_ = nullptr;
    bool ambiguous_ = false;
};

void ObserveNativeCreate(IDXGISwapChain* swapchain, HWND window, bool presentCovered);
void ObserveNativeCreate(IDXGISwapChain* swapchain);
// Native creation also publishes an output marker for proxies whose cached creation exports
// bypassed the scope. Forwarded private data plus a frontend API device proves delegation.
// Cached decisions retire with the frontend's intercepted Release, independently of forwarded
// private data and other proxy layers. No COM refs are retained; native DX12/FG proxies keep their routing.
bool HasNativeOwner(IDXGISwapChain* applicationSwapchain);

// A late injection never sees the native output being created, so the marker a forwarding
// frontend proves itself with is never published, and the frontend and the native output both
// process every frame once both Present hooks are in place (Heroes of the Storm + DXUP, late
// inject 20261025_230718). The output's first Present CE processes publishes the same evidence.
// One atomic load per call once the output is known.
void ObserveNativePresent(IDXGISwapChain* swapchain);

}  // namespace ce::translated_swapchain
