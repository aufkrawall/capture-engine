#pragma once
#include <dxgi1_4.h>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

// Forward declaration
class PerformanceMetrics;

namespace DXGIShared {

enum class APIType {
    Unknown,
    D3D10,
    D3D11,
    D3D12,
    Vulkan  // For WSI-DXGI interop
};

struct SharedState {
    std::atomic<bool> swapchainInvalid{false};
    std::atomic<bool> fsr4RecreationPending{false};
    std::atomic<int> wrapperResizeDepth{0};
    std::atomic<uint64_t> frameCount{0};
    std::atomic<bool> deviceRemovedFatal{false};
    std::atomic<uint64_t> presentCallCount{0};
    std::chrono::steady_clock::time_point lastSwapchainCreation;
    std::atomic<bool> inPresentHook{false};
    std::atomic<bool> fgSwapchainStabilized{false};
};

extern SharedState g_SharedState;
extern std::mutex g_SharedMutex;

// Initialization
void Init();

// The unified hook installer
bool InstallHooks(IDXGISwapChain* pSwapChain);

// Common helpers
bool IsVulkanPrimary();
PerformanceMetrics* GetPerformanceMetrics();

// Exported handlers for specific APIs (implemented in their respective hook files)
void HandleDX11ProcessFrame(IDXGISwapChain* pSwapChain, bool isRealFrame);
void HandleDX12ProcessFrame(IDXGISwapChain* pSwapChain, bool isRealFrame);
void HandleDX12ResizeBegin();
void HandleDX12ResizeEnd();
void HandleDX11ResizeBegin();

}  // namespace DXGIShared
