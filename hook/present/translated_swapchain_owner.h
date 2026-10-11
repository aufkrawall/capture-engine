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
// A marker belongs to the application-facing object only, even if a proxy forwards private data
// to the native chain. That native output remains CE's sole overlay/capture/post-processing owner.
bool HasNativeOwner(IDXGISwapChain* applicationSwapchain);

}  // namespace ce::translated_swapchain
