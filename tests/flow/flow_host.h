#pragma once

// The game side of an FG flow test: a D3D12 game on the WARP adapter with a visible, never-activated window
// off the desktop, into which the flow-test hook DLL is loaded like an injection. Each scenario runs in its
// own process (the hook's state is process-global), so a FlowGame is created once per test.

#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <string>

#include "tests/flow/flow_api.h"

namespace ce::flow {

using Microsoft::WRL::ComPtr;

class FlowGame {
public:
    // Loads build/tests/flow/capture_hook_x64.dll next to the executable, logging into
    // logs/<testName> beside it, and runs CE's hook installation with the flow config.
    explicit FlowGame(const std::string& testName);
    ~FlowGame();

    FlowGame(const FlowGame&) = delete;
    FlowGame& operator=(const FlowGame&) = delete;

    // The game's own D3D12 setup: WARP device, direct queue, 3-buffer flip-discard swapchain.
    bool CreateDeviceAndSwapchain(UINT width = 640, UINT height = 360);

    // Renders (clears the back buffer), submits and presents one frame, then waits for the GPU so every
    // frame finishes before the next starts. Pumps the hook thread every `kFramesPerHookThreadPass` frames.
    bool RenderFrame();
    bool RenderFrames(int count);

    CEFlowOverlayCoverage Coverage() const;
    CEFlowPublishedFG PublishedFG() const;
    const std::string& LogDirectory() const { return logDirectory_; }
    const std::string& Error() const { return error_; }
    int FramesPresented() const { return frame_; }

    // The production hook thread passes every 100 ms; a scenario frame stands for ~7 ms of a 144 FPS game.
    static constexpr int kFramesPerHookThreadPass = 14;

private:
    bool Fail(const char* what, HRESULT hr);

    std::string logDirectory_;
    std::string error_;
    HMODULE hook_ = nullptr;
    CEFlow_PumpHookThread_t pumpHookThread_ = nullptr;
    CEFlow_GetOverlayCoverage_t getOverlayCoverage_ = nullptr;
    CEFlow_GetPublishedFG_t getPublishedFG_ = nullptr;
    CEFlow_Shutdown_t shutdown_ = nullptr;

    HWND window_ = nullptr;
    ComPtr<IDXGIFactory4> factory_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swapchain_;
    ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    UINT rtvStride_ = 0;
    ComPtr<ID3D12Resource> backBuffers_[3];
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    UINT64 fenceValue_ = 0;
    int frame_ = 0;
};

}  // namespace ce::flow
