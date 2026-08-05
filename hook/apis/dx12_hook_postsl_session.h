#pragma once

#include "dx12_hook_internal.h"

// Decomposed PostSLOverlayRender session (was 2648 lines inline in dx12_hook_internal.h).

enum class PostSLFlow {
    kContinue,
    kReturn,
};
// Function-scope statics that span chunks stay visible across the postsl units.
extern std::atomic<int> s_postSLRenders;
extern std::atomic<int> s_postSLSkipFence;
extern int s_reactivationEpoch;
extern int s_callsSinceReactivation;
extern int s_postSLProbeFrames;

class PostSLRenderSession {
public:
    explicit PostSLRenderSession(IDXGISwapChain* pSwapChain) : pSwapChain(pSwapChain) {}
    void Run();

private:
    IDXGISwapChain* pSwapChain;

    uint32_t entryLifecycleEpoch;
    bool cachedSLFGActive;
    bool processFrameRecentlySeen;
    bool safePostFSRBootstrapPathForPostSL;
    bool explicitEnablePureDLSSColdStartProof;
    bool keepAliveRenderAfterExplicitOff;
    bool exactExplicitOffKeepAliveSwapchain;
    bool active;
    ID3D12Device* dev;
    HRESULT devReason;
    bool retireOfficialUiCoverageAfterExactDraw;
    ID3D12CommandQueue* queue;
    ID3D12CommandQueue* scQueue;
    IDXGISwapChain3* sc3;
    UINT bufIdx;
    ID3D12Resource* bb;
    int idx;
    ID3D12GraphicsCommandList* list;
    ID3D12CommandAllocator* alloc;
    bool rendered;
    bool selectedQueueIsSwapchainQueue;
    bool fastPostFSRDLSSProbe;
    int postFSRProbeFramesPerLevel;
    ExecuteCommandListsPtr realECL;
    ID3D12CommandQueue* realQ;
    ExecuteCommandListsPtr selectedQueueOrigECL;
    bool selectedQueueOrigECLMatchesRealECL;
    bool isSLWrapperQ;
    bool useExplicitPostFSRSwapchainTransitions;
    bool usePostSLOffscreenComposite;
    bool useExplicitPostFSRBackbufferCopyTransitions;
    bool hasSelectedQueueSubmitPath;
    bool preferSelectedSwapchainQueueSubmitAfterFSR;
    bool preferSelectedQueueDirectSubmitAfterFSR;
    bool isPostFSRProbe;
    ID3D12CommandQueue* slWrapperQueue;
    ID3D12CommandQueue* liveSLWrapperQueue;
    bool usingPinnedPostFSRWrapperQueue;
    bool crossQueueSynced;
    bool extendRuntimeStateStabilization;

    PostSLFlow Chunk0();
    PostSLFlow Chunk1();
    PostSLFlow Chunk2();
    PostSLFlow Chunk3();
};

