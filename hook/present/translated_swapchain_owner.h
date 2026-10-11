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

}  // namespace ce::translated_swapchain
