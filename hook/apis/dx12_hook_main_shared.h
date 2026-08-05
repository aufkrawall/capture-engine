#pragma once

inline std::atomic<ExecuteCommandListsPtr> g_SLBypassECL{nullptr};

inline std::atomic<bool> g_PreferredOverlayFGPublicationStateValid{false};

inline std::atomic<bool> g_PreferredOverlayFGPublicationStateActive{false};

inline std::atomic<int> g_PreferredOverlayFGPublicationStateRuntimeMode{
    static_cast<int>(ce::fg_runtime::RuntimeMode::kOff)};

inline std::atomic<uint64_t> g_OverlayFGPublicationSequence{0};

inline std::atomic<uint64_t> g_PreferredOverlayFGPublicationStateSequence{0};

inline std::atomic<uint64_t> g_FrameIndex{0};

inline std::atomic<uint64_t> g_StreamlineStartupActivationSwapchainGeneration{0};

inline std::atomic<bool> g_PostSLStartupActivationServiceInProgress{false};

inline bool g_IPCReady = false;

inline std::atomic<bool> g_PiggybackDrawnThisFrame{false};

inline const GUID SKID_D3D12SwapChainBufferBitmap = {
    0xbc53df3b, 0x956f, 0x47db, {0xa6, 0x53, 0x5, 0xd7, 0xb8, 0x71, 0x53, 0x38}};

// File-scope variables defined in the first part (extern for the rest).

extern CreateCommittedResourcePtr oCreateCommittedResource;
extern CreateCommandQueuePtr oTraceCreateCommandQueue;
extern CreateDescriptorHeapPtr oTraceCreateDescriptorHeap;
extern CommandQueueSignalPtr oTraceCommandQueueSignal;
extern std::atomic<int> g_PostSLECLDiagCount;
extern std::atomic<ID3D12Device*> g_Device;
extern std::atomic<ID3D12CommandQueue*> g_CommandQueue;
extern std::recursive_mutex g_CommandQueueMutex;
extern ID3D12Resource* g_DummyBackBuffer;
extern DX12Hook* g_dx12HookInstance;
extern std::recursive_mutex g_DeviceQueuesMutex;
extern std::map<ID3D12Device*, ID3D12CommandQueue*> g_DeviceQueues;

// File-scope functions shared by the split parts (definitions in the first part).

void HookUpdatePreferredOverlayFGPublicationState(bool active, ce::fg_runtime::RuntimeMode runtimeMode, const char* source);
bool HookHasExplicitStreamlineSetOptionsActivation();
bool HookHasRuntimeOwnedNativeFGPresentPath();
const char* DescribeFocusLossPostPresentFenceSkip( const ce::dx12_overlay_policy::D3D12FocusLossOverlayFenceWaitContext& ctx, const ce::dx12_overlay_policy::D3D12DeferredOverlaySignalFlushInfo& info);

