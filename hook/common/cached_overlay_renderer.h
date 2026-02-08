#pragma once

// Cached Overlay Renderer - Zero CPU overhead for interpolated frames
// Supports rendering on all frames (real + interpolated) without stutter

#include <d3d12.h>
// #include <imgui.h>  // REMOVED: Using custom overlay instead
//  Minimal type definitions to allow compilation during migration
struct ImVec2 {
    float x, y;
};
struct ImDrawVert {
    float pos[2], uv[2];
    unsigned int col;
};
typedef unsigned short ImDrawIdx;
struct ImDrawCmd {
    unsigned int ElemCount;
};

// Named struct for CmdBuffer to allow range-based for loops
struct ImDrawCmdBuffer {
    int Size;
    ImDrawCmd* Data;
    ImDrawCmd* begin() const { return Data; }
    ImDrawCmd* end() const { return Data + Size; }
};

struct ImDrawList {
    struct {
        int Size;
        ImDrawVert* Data;
    } VtxBuffer;
    struct {
        int Size;
        ImDrawIdx* Data;
    } IdxBuffer;
    ImDrawCmdBuffer CmdBuffer;
};
struct ImDrawData {
    ImVec2 DisplayPos, DisplaySize;
    int CmdListsCount;
    ImDrawList** CmdLists;
};
struct ImGuiContext {};
namespace ImGui {
static ImDrawData* GetDrawData() { return nullptr; }
}  // namespace ImGui

// Stub for ImGui DX12 backend function - REMOVED: Using custom overlay
inline void ImGui_ImplDX12_RenderDrawData(ImDrawData* drawData, ID3D12GraphicsCommandList* commandList)
{
    (void)drawData;
    (void)commandList;
    // No-op: legacy ImGui rendering disabled
}
#include <atomic>
#include <memory>
#include <vector>
#include "performance_metrics.h"

namespace overlay {

// Forward declarations
class GraphicsContext;

// Per-frame cached resources
struct CachedFrameResources {
    ID3D12CommandAllocator* commandAllocator = nullptr;
    ID3D12GraphicsCommandList* commandList = nullptr;
    ID3D12Resource* vertexBuffer = nullptr;
    ID3D12Resource* indexBuffer = nullptr;
    void* vertexBufferCpuPtr = nullptr;
    void* indexBufferCpuPtr = nullptr;

    // GPU timestamps for synchronization
    uint64_t fenceValue = 0;

    // Validation
    bool isValid = false;
    bool isRecording = false;

    // Buffer sizes (fixed allocation)
    static constexpr size_t MAX_VERTICES = 65536;
    static constexpr size_t MAX_INDICES = 65536;
    static constexpr size_t VB_SIZE = MAX_VERTICES * sizeof(ImDrawVert);
    static constexpr size_t IB_SIZE = MAX_INDICES * sizeof(ImDrawIdx);
};

// Smooth-scrolling frame time graph data
struct FrameTimeGraphData {
    static constexpr int HISTORY_SIZE = 256;
    float history[HISTORY_SIZE];
    std::atomic<uint32_t> writeIndex{0};

    // For GPU sampling
    ID3D12Resource* gpuHistoryBuffer = nullptr;
    float* gpuHistoryCpuPtr = nullptr;

    void AddSample(float frameTimeMs)
    {
        uint32_t idx = writeIndex.fetch_add(1, std::memory_order_relaxed) % HISTORY_SIZE;
        history[idx] = frameTimeMs;
        if (gpuHistoryCpuPtr) {
            gpuHistoryCpuPtr[idx] = frameTimeMs;
        }
    }

    void InitGpuBuffer(ID3D12Device* device);
};

class CachedOverlayRenderer {
public:
    CachedOverlayRenderer();
    ~CachedOverlayRenderer();

    // Initialization
    bool Initialize(ID3D12Device* device, ID3D12CommandQueue* queue, uint32_t bufferCount);
    void Shutdown();

    // Main render entry - called EVERY Present (both real and interpolated frames)
    // cpuFrameType: True if this is a "real" frame (game submitted work)
    //               False if interpolated (FG generated frame)
    void Render(ID3D12GraphicsCommandList* targetCommandList, uint32_t backBufferIndex, bool isRealFrame,
                const ImVec2& displaySize);

    // Update overlay content - only call on real frames
    void UpdateContent(PerformanceMetrics* metrics, float displayWidth, float displayHeight, float deltaTime);

    // Check if overlay content needs update
    bool ShouldUpdateContent() const;

    // Set update throttle (Hz)
    void SetUpdateRate(float targetHz) { updateIntervalMs = 1000.0f / targetHz; }

private:
    // Device resources
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* commandQueue = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = nullptr;
    uint64_t nextFenceValue = 1;

    // Per-frame cached resources
    std::vector<CachedFrameResources> perFrameResources;
    uint32_t numBackBuffers = 0;

    // ImGui context for overlay
    ImGuiContext* overlayContext = nullptr;
    bool imGuiInitialized = false;

    // Update throttling
    float updateIntervalMs = 16.67f;  // Default 60Hz
    float timeSinceLastUpdate = 0.0f;
    bool contentDirty = true;

    // Frame time graph
    FrameTimeGraphData frameTimeGraph;

    // SRV heap for ImGui
    ID3D12DescriptorHeap* srvHeap = nullptr;
    UINT srvDescriptorSize = 0;

    // Internal methods
    bool CreateFrameResources(uint32_t bufferCount);
    void DestroyFrameResources();
    bool CreateGpuBuffers();
    void DestroyGpuBuffers();

    // Rebuild command list with current ImGui draw data
    void RebuildCommandList(CachedFrameResources& frame, const ImDrawData* drawData);

    // Submit cached command list (zero CPU work)
    void SubmitCachedCommandList(CachedFrameResources& frame, ID3D12GraphicsCommandList* targetList);

    // Wait for GPU to finish with frame resources
    bool WaitForFrameResources(uint32_t frameIndex, uint32_t timeoutMs = 0);

    // Check if GPU is done with frame
    bool IsFrameAvailable(uint32_t frameIndex);
};

// Helper class for managing ImGui render data
class ImGuiRenderCache {
public:
    struct CachedDrawData {
        std::vector<ImDrawVert> vertices;
        std::vector<ImDrawIdx> indices;
        std::vector<ImDrawCmd> commands;
        ImVec2 displayPos;
        ImVec2 displaySize;
        uint64_t frameCount;
    };

    void CacheDrawData(const ImDrawData* drawData);
    const CachedDrawData* GetCachedData() const { return &cachedData; }
    bool HasData() const { return !cachedData.vertices.empty(); }
    void Clear() { cachedData = CachedDrawData(); }

private:
    CachedDrawData cachedData;
};

}  // namespace overlay
