#pragma once

inline ID3D12Device* g_FFXUiCompositeDevice = nullptr;

inline ID3D12DescriptorHeap* g_FFXUiCompositeRtvHeap = nullptr;

inline constexpr int kFFXUiCompositeSlotCount =
    3;  // 3-slot rotation recycled by fence value (signaled on CE's own queue).

inline ID3D12CommandAllocator* g_FFXUiCompositeAlloc[kFFXUiCompositeSlotCount] = {};

inline ID3D12GraphicsCommandList* g_FFXUiCompositeList = nullptr;

inline HANDLE g_FFXUiCompositeFenceEvent = nullptr;

inline UINT64 g_FFXUiCompositeAllocFenceVal[kFFXUiCompositeSlotCount] = {};

inline DXGI_FORMAT g_FFXUiCompositeAdapterFormat = DXGI_FORMAT_UNKNOWN;

inline std::atomic<bool> g_FFXUiResourceCompositionActive{false};

inline std::atomic<uint64_t> g_FFXUiCompositeLastTickMs{0};

inline std::atomic<ID3D12Resource*> g_CachedFFXUiTexture{nullptr};

inline std::atomic<uint32_t> g_CachedFFXUiState{0};

inline std::atomic<uint32_t> g_CachedFFXUiFlags{0};

inline std::atomic<bool> g_BundleTargetNeedsTransparentClear{false};

inline ID3D12Resource* g_CEUiSubstituteTexture = nullptr;

inline uint32_t g_CEUiSubstituteWidth = 0;

inline uint32_t g_CEUiSubstituteHeight = 0;

inline DXGI_FORMAT g_CEUiSubstituteFormat = DXGI_FORMAT_UNKNOWN;

inline D3D12_RESOURCE_STATES g_CEUiSubstituteInitialState = D3D12_RESOURCE_STATE_COMMON;

inline std::atomic<uint64_t> g_FFXUiPreparationSequence{0};

inline uint64_t g_FFXUiCommittedPreparationSequence = 0;  // guarded by g_FFXUiCompositeMutex

inline uint64_t g_FFXUiPresenterFallbackLastSequence = 0;  // guarded by g_FFXUiCompositeMutex

inline FFXUiCompositeTimelineEntry g_FFXUiCompositeTimeline[dx12_hook_kFFXUiCompositeTimelineSize];

inline std::atomic<uint32_t> g_FFXUiCompositeTimelineIdx{0};

inline std::atomic<uint64_t> g_LastFfxConfigureForwardQpc{0};

inline std::atomic<uint64_t> g_FfxConfigureFrame{0};

inline std::mutex g_NativeFSRSwapchainQueueBindingMutex;

inline std::unordered_map<void*, NativeFSRSwapchainQueueBinding> g_NativeFSRSwapchainQueueBindings;

inline std::mutex g_FFXProxyPresentHookMutex;

inline void* g_FFXProxySwapchain = nullptr;             // game-facing proxy object (identity/diagnostics only)

inline void** g_FFXProxyPresentVtableEntry = nullptr;   // &vtable[8] patched (class vtable in the FFX module)

inline void** g_FFXProxyPresent1VtableEntry = nullptr;  // &vtable[22] patched (nullptr if slot unavailable)

inline std::atomic<PFN_FFXProxyPresent> g_FFXProxyPresentOriginal{nullptr};

inline std::atomic<PFN_FFXProxyPresent1> g_FFXProxyPresent1Original{nullptr};

inline std::atomic<bool> g_FFXProxyPresentHookInstalled{false};

inline std::atomic<uint64_t> g_FFXProxyPresentHookInstallQpc{0};

inline std::atomic<uint64_t> g_FFXProxyPreworkCount{0};

inline std::atomic<uint64_t> g_FFXProxyPreworkLastQpc{0};

inline std::atomic<uint32_t> g_FFXProxyPreworkLastTid{0};

inline std::atomic<bool> g_FFXProxyPresentQuiescing{true};

inline std::atomic<uint32_t> g_FFXProxyPresentDetoursInFlight{0};

inline std::mutex g_FFXProxyPresentDrainMutex;

inline std::condition_variable g_FFXProxyPresentDrainCV;

inline thread_local bool t_InsideFFXProxyPresentPrework = false;

inline thread_local uint32_t t_FFXProxyPresentDetourDepth = 0;

// File-scope variables defined in the first part (extern for the rest).


// File-scope functions shared by the split parts (definitions in the first part).

DXGI_FORMAT FFXUiCompositeRtvFormat(DXGI_FORMAT texFormat);  // forward decl — defined belo;
D3D12_RESOURCE_STATES GetDX12StateFromFFXResourceState(uint32_t state);
bool DX12_ResolveRuntimeOwnedOverlayTargetHDRState(DXGI_FORMAT format);
void TransitionResourceIfNeeded(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
void CopyFFXPresentSourceToOutput(ID3D12GraphicsCommandList* cmdList, const ce::ffx_api::CallbackDescFrameGenerationPresent* desc);
bool RenderOverlayViaFFXPresentCallback(const ce::ffx_api::CallbackDescFrameGenerationPresent* desc);
uint32_t DX12_RenderOverlayViaFFXPresentCallback(ce::ffx_api::CallbackDescFrameGenerationPresent* desc, void* userCtx);
void RecordFFXUiCompositeTimelineEntry(const FFXUiCompositeTimelineEntry& entry);
bool DX12_IsFFXUiResourceCachedForBundle();
bool DX12_IsNativeFSRInternalNoCallbackCompositionActive();
bool DX12_IsLiveSwapchainQueueOriginalGameQueue();
bool DX12_IsNativeFSRFGSuspendedDisablePending();
bool IsResourceOwnedByDevice(ID3D12Resource* resource, ID3D12Device* expectedDevice);
bool DX12_PrepareFFXUiOverlayTarget(const ce::ffx_api::Resource& gameUi, uint32_t flags, ce::ffx_api::Resource* ceSubstitute, DX12FFXUiOverlayTargetPreparation* preparation);
void ReleaseFFXUiCompositeInfra();
DXGI_FORMAT FFXUiCompositeRtvFormat(DXGI_FORMAT texFormat);
bool DX12_CompositeOverlayOntoFFXUiResource(void* uiResourcePtr, uint32_t ffxState, uint32_t flags);
void DX12_UnregisterNativeFSRSwapchainPresentationQueue(void* context, const char* reason);
AcquiredNativeFSROwnerQueue AcquireNativeFSRSwapchainPresentationQueue(IDXGISwapChain* proxy, ID3D12Resource* target);
bool SubmitNativeFSROwnerQueueOverlayCommandList(ID3D12CommandQueue* queue, ID3D12CommandList* commandList);
HRESULT SignalNativeFSROwnerQueueOverlayFence(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value);
bool DX12_CompositeOverlayOntoSuspendBackbuffer(IDXGISwapChain* proxy, const char* routeName);
bool DX12_CompositeOverlayBelowForeignChainForRuntimeOwnedFSR(IDXGISwapChain* presentedSwapChain,
                                                              ID3D12CommandQueue* submitQueue);
void DX12_LogFFXProxyPresentHookFreezeDiagnostics(const char* reason);
