#include "dx12_hook_internal.h"
#include "dx12_hook_ffx_shared.h"
#include "dx12_ffx_suspend_overlay.h"

#include <d3dcompiler.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

// Native FSR frame generation with AMD owning the swapchain (no app present callback) draws CE's overlay through the
// UI-resource and topmost same-batch routes. Neither route runs ProcessFrame or PostSL on the presenter thread, so
// the screenshot request had no servicing point and waited for the full hook timeout before the desktop fallback.
//
// This composite services the request on the game queue during the FFX proxy-present prework, which runs before AMD's
// Present on the game thread and sees both the application frame and the game's UI resource before CE draws over it:
//   1. copy the presented frame (proxy backbuffer) and the game UI resource into one target: scene, then UI with the
//      straight-alpha "over" that the overlay itself uses when it draws onto the UI resource;
//   2. when the overlay is included, draw CE's overlay on top through the renderer's reserved screenshot slot;
//   3. read the target back through the same raw-payload path as every other route.
// Nothing here waits on the GPU: the readback is asynchronous and every resource is retired behind a fence signaled on
// the same queue after the last submission. AMD's queue is never touched.
namespace {

constexpr char kCompositeShaderSource[] = R"HLSL(
cbuffer CompositeConstants : register(b0) {
    uint hasUi;
    float2 uiScale;
};
Texture2D<float4> SceneTexture : register(t0);
Texture2D<float4> UiTexture : register(t1);

float4 VSMain(uint vertexId : SV_VertexID) : SV_Position {
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    return float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}

float4 PSMain(float4 position : SV_Position) : SV_Target {
    const float4 scene = SceneTexture.Load(int3(int2(position.xy), 0));
    float4 ui = float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (hasUi != 0) {
        ui = UiTexture.Load(int3(int2(position.xy * uiScale), 0));
    }
    return float4(ui.rgb * ui.a + scene.rgb * (1.0f - ui.a), 1.0f);
}
)HLSL";

using PFN_CompositeD3DCompile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
                                                LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

enum class CaptureOutcome {
    kQueued,      // Composite, overlay and readback were submitted; the worker completes the request.
    kRetryLater,  // The reserved slot is still in flight from a previous capture; the request stays pending.
    kFailed,      // The request cannot be serviced on this route; the caller completes it as failed.
};

// Retired captures keep every object their submitted command lists reference until the fence proves the GPU finished.
struct RetiredCapture {
    ComPtr<ID3D12Fence> fence;
    UINT64 value = 0;
    std::vector<ComPtr<IUnknown>> keepAlive;
};

// Root signature and pipelines are cached per device; the composite is a fixed fullscreen pass per backbuffer format.
struct PipelineCache {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> rootSignature;
    std::vector<std::pair<DXGI_FORMAT, ComPtr<ID3D12PipelineState>>> pipelines;
};

std::mutex g_captureMutex;  // guards every member below
std::deque<RetiredCapture> g_retiredCaptures;
PipelineCache g_pipelineCache;
uint64_t g_retryRequestId = 0;
uint32_t g_retryCount = 0;

// A retry is a whole Present's wait. Keep it bounded so a permanently refused slot fails the request instead of
// leaving the requester blocked until the hook timeout.
constexpr uint32_t kMaxRetryPresents = 60;

bool IsServiceableFormat(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return true;
        default:
            return false;
    }
}

bool SameComObject(IUnknown* left, IUnknown* right) {
    if (!left || !right) {
        return false;
    }
    ComPtr<IUnknown> leftIdentity;
    ComPtr<IUnknown> rightIdentity;
    return SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&leftIdentity))) &&
           SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&rightIdentity))) && leftIdentity.Get() == rightIdentity.Get();
}

bool DeviceOfResourceMatches(ID3D12Resource* resource, ID3D12Device* device) {
    ComPtr<ID3D12Device> resourceDevice;
    return resource && device && SUCCEEDED(resource->GetDevice(IID_PPV_ARGS(&resourceDevice))) &&
           SameComObject(resourceDevice.Get(), device);
}

void TransitionIfNeeded(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                        D3D12_RESOURCE_STATES after) {
    if (before == after) {
        return;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
}

void PruneRetiredCaptures() {
    for (auto it = g_retiredCaptures.begin(); it != g_retiredCaptures.end();) {
        if (it->fence->GetCompletedValue() >= it->value) {
            it = g_retiredCaptures.erase(it);
        } else {
            ++it;
        }
    }
}

// Compiles through the system compiler at first use, the same runtime-compile pattern the cursor and face-camera
// renderers use. The hook never links d3dcompiler, so a missing compiler fails the request instead of the Present.
bool CompileCompositeStage(const char* entryPoint, const char* target, ComPtr<ID3DBlob>& blob) {
    static HMODULE compilerModule = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!compilerModule) {
        return false;
    }
    auto compile = reinterpret_cast<PFN_CompositeD3DCompile>(GetProcAddress(compilerModule, "D3DCompile"));
    if (!compile) {
        return false;
    }
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = compile(kCompositeShaderSource, sizeof(kCompositeShaderSource) - 1,
                               "ce_fsr_screenshot_composite.hlsl", nullptr, nullptr, entryPoint, target,
                               D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
    if (FAILED(hr) || !blob) {
        HookLogImportant("[Screenshot] FSR composite shader %s failed to compile hr=0x%08X: %s", entryPoint,
                         static_cast<unsigned>(hr),
                         errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no diagnostics");
        return false;
    }
    return true;
}

ComPtr<ID3D12RootSignature> CreateCompositeRootSignature(ID3D12Device* device) {
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 2;
    range.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    parameters[0].DescriptorTable.pDescriptorRanges = &range;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.ShaderRegister = 0;
    parameters[1].Constants.Num32BitValues = 3;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 2;
    desc.pParameters = parameters;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) || !blob) {
        return nullptr;
    }
    ComPtr<ID3D12RootSignature> rootSignature;
    if (FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           IID_PPV_ARGS(&rootSignature)))) {
        return nullptr;
    }
    return rootSignature;
}

ComPtr<ID3D12PipelineState> CreateCompositePipeline(ID3D12Device* device, ID3D12RootSignature* rootSignature,
                                                    DXGI_FORMAT format) {
    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShader;
    if (!CompileCompositeStage("VSMain", "vs_5_0", vertexShader) ||
        !CompileCompositeStage("PSMain", "ps_5_0", pixelShader)) {
        return nullptr;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = rootSignature;
    desc.VS = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
    desc.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = format;
    desc.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> pipeline;
    if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)))) {
        return nullptr;
    }
    return pipeline;
}

// Returns the cached pipeline for (device, format), replacing the cache when the device changes. Replaced objects stay
// alive through the retired captures that reference them.
ComPtr<ID3D12PipelineState> GetCompositePipeline(ID3D12Device* device, DXGI_FORMAT format,
                                                 ComPtr<ID3D12RootSignature>& rootSignature) {
    if (g_pipelineCache.device.Get() != device) {
        g_pipelineCache = PipelineCache{};
        g_pipelineCache.device = device;
        g_pipelineCache.rootSignature = CreateCompositeRootSignature(device);
    }
    rootSignature = g_pipelineCache.rootSignature;
    if (!rootSignature) {
        return nullptr;
    }
    for (const auto& [cachedFormat, pipeline] : g_pipelineCache.pipelines) {
        if (cachedFormat == format) {
            return pipeline;
        }
    }
    ComPtr<ID3D12PipelineState> pipeline = CreateCompositePipeline(device, rootSignature.Get(), format);
    if (pipeline) {
        g_pipelineCache.pipelines.emplace_back(format, pipeline);
    }
    return pipeline;
}

// Snapshot of the game's UI resource that AMD composites over every output, taken before CE draws its overlay onto it.
// The CE-owned substitute holds overlay storage rather than game HUD, so it is never blended in.
struct UiSnapshot {
    ComPtr<ID3D12Resource> texture;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
};

UiSnapshot SnapshotGameUiResource() {
    UiSnapshot snapshot;
    std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_FFXUiCompositeMutex);
    ID3D12Resource* texture = g_CachedFFXUiTexture.load(std::memory_order_acquire);
    if (!texture || (g_CEUiSubstituteTexture && texture == g_CEUiSubstituteTexture)) {
        return snapshot;
    }
    snapshot.texture = texture;
    snapshot.state = GetDX12StateFromFFXResourceState(g_CachedFFXUiState.load(std::memory_order_acquire));
    return snapshot;
}

void LogCaptureFailure(const char* stage, uint64_t requestId, HRESULT hr) {
    static std::atomic<int> s_failureLogCount{0};
    const int logCount = s_failureLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 120) == 0) {
        HookLogImportant("[Screenshot] FSR no-callback capture failed at %s (request=%llu hr=0x%08X log=%d)", stage,
                         static_cast<unsigned long long>(requestId), static_cast<unsigned>(hr), logCount + 1);
    }
}

// Records, submits and retires one composite. Caller holds g_captureMutex.
CaptureOutcome CaptureLocked(IDXGISwapChain* proxy, SharedMemoryLayout* shm, uint64_t requestId,
                             bool includeOverlay) {
    ComPtr<IDXGISwapChain3> swapChain3;
    if (FAILED(proxy->QueryInterface(IID_PPV_ARGS(&swapChain3)))) {
        LogCaptureFailure("swapchain query", requestId, E_NOINTERFACE);
        return CaptureOutcome::kFailed;
    }
    const UINT bufferIndex = swapChain3->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> backBuffer;
    HRESULT hr = swapChain3->GetBuffer(bufferIndex, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr) || !backBuffer) {
        LogCaptureFailure("backbuffer query", requestId, hr);
        return CaptureOutcome::kFailed;
    }
    const D3D12_RESOURCE_DESC backBufferDesc = backBuffer->GetDesc();
    if (backBufferDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || backBufferDesc.SampleDesc.Count != 1 ||
        backBufferDesc.Width == 0 || backBufferDesc.Height == 0 || backBufferDesc.Width > UINT32_MAX ||
        backBufferDesc.Height > UINT32_MAX || !IsServiceableFormat(backBufferDesc.Format)) {
        LogCaptureFailure("unsupported backbuffer contract", requestId, E_INVALIDARG);
        return CaptureOutcome::kFailed;
    }
    ComPtr<ID3D12Device> device;
    if (FAILED(backBuffer->GetDevice(IID_PPV_ARGS(&device))) || !device) {
        LogCaptureFailure("backbuffer device query", requestId, E_FAIL);
        return CaptureOutcome::kFailed;
    }

    // The owner queue is the one the UI-resource overlay already submits on: the game's queue, not AMD's FG queue.
    AcquiredNativeFSROwnerQueue ownerQueue = AcquireNativeFSRSwapchainPresentationQueue(proxy, backBuffer.Get());
    ComPtr<ID3D12CommandQueue> queue;
    queue.Attach(ownerQueue.queue);
    if (!queue) {
        LogCaptureFailure("owner queue", requestId, E_FAIL);
        return CaptureOutcome::kFailed;
    }
    ComPtr<ID3D12Device> queueDevice;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) || !SameComObject(device.Get(), queueDevice.Get())) {
        LogCaptureFailure("owner queue device identity", requestId, E_INVALIDARG);
        return CaptureOutcome::kFailed;
    }

    UiSnapshot ui = SnapshotGameUiResource();
    D3D12_RESOURCE_DESC uiDesc{};
    if (ui.texture) {
        uiDesc = ui.texture->GetDesc();
        if (uiDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || uiDesc.SampleDesc.Count != 1 ||
            uiDesc.Width == 0 || uiDesc.Height == 0 || !IsServiceableFormat(uiDesc.Format) ||
            !DeviceOfResourceMatches(ui.texture.Get(), device.Get())) {
            LogCaptureFailure("game UI resource contract", requestId, E_INVALIDARG);
            return CaptureOutcome::kFailed;
        }
    }

    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipeline = GetCompositePipeline(device.Get(), backBufferDesc.Format, rootSignature);
    if (!pipeline) {
        LogCaptureFailure("composite pipeline", requestId, E_FAIL);
        return CaptureOutcome::kFailed;
    }

    // The composite target is CE-owned, so it can be the same size as the backbuffer and carry only the RTV usage.
    D3D12_RESOURCE_DESC targetDesc = backBufferDesc;
    targetDesc.Alignment = 0;
    targetDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    targetDesc.MipLevels = 1;
    targetDesc.DepthOrArraySize = 1;
    targetDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> target;
    hr = device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &targetDesc,
                                         D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&target));
    if (FAILED(hr) || !target) {
        LogCaptureFailure("composite target creation", requestId, hr);
        return CaptureOutcome::kFailed;
    }

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = 2;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> srvHeap;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    if (FAILED(device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap))) ||
        FAILED(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)))) {
        LogCaptureFailure("descriptor heap creation", requestId, E_FAIL);
        return CaptureOutcome::kFailed;
    }
    const UINT srvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = srvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sceneSrv{};
    sceneSrv.Format = backBufferDesc.Format;
    sceneSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sceneSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sceneSrv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(backBuffer.Get(), &sceneSrv, srvCpu);
    D3D12_SHADER_RESOURCE_VIEW_DESC uiSrv = sceneSrv;
    uiSrv.Format = ui.texture ? uiDesc.Format : backBufferDesc.Format;
    const D3D12_CPU_DESCRIPTOR_HANDLE uiCpu{srvCpu.ptr + srvStride};
    device->CreateShaderResourceView(ui.texture ? ui.texture.Get() : backBuffer.Get(), &uiSrv, uiCpu);
    device->CreateRenderTargetView(target.Get(), nullptr, rtvHeap->GetCPUDescriptorHandleForHeapStart());

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(),
                                         IID_PPV_ARGS(&list)))) {
        LogCaptureFailure("command list creation", requestId, E_FAIL);
        return CaptureOutcome::kFailed;
    }

    // Everything submitted below is retired behind this fence, including when a later step refuses the overlay.
    ComPtr<ID3D12Fence> retireFence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&retireFence)))) {
        LogCaptureFailure("retire fence creation", requestId, E_FAIL);
        return CaptureOutcome::kFailed;
    }
    bool submitted = false;
    auto retireSubmitted = ce::make_scope_guard([&]() {
        if (!submitted) {
            return;
        }
        if (SUCCEEDED(queue->Signal(retireFence.Get(), 1))) {
            RetiredCapture retired;
            retired.fence = retireFence;
            retired.value = 1;
            retired.keepAlive = {target, allocator, list, srvHeap, rtvHeap, rootSignature, pipeline,
                                 backBuffer, ui.texture, queue};
            g_retiredCaptures.push_back(std::move(retired));
        }
    });
    PruneRetiredCaptures();

    const UINT32 hasUi = ui.texture ? 1u : 0u;
    const float uiScaleX = ui.texture ? static_cast<float>(uiDesc.Width) / static_cast<float>(backBufferDesc.Width) : 1.0f;
    const float uiScaleY = ui.texture ? static_cast<float>(uiDesc.Height) / static_cast<float>(backBufferDesc.Height) : 1.0f;
    UINT32 constants[3] = {hasUi, 0u, 0u};
    std::memcpy(&constants[1], &uiScaleX, sizeof(float));
    std::memcpy(&constants[2], &uiScaleY, sizeof(float));

    TransitionIfNeeded(list.Get(), backBuffer.Get(), D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (ui.texture) {
        TransitionIfNeeded(list.Get(), ui.texture.Get(), ui.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    TransitionIfNeeded(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(backBufferDesc.Width),
                            static_cast<float>(backBufferDesc.Height), 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(backBufferDesc.Width), static_cast<LONG>(backBufferDesc.Height)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    ID3D12DescriptorHeap* heaps[] = {srvHeap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(rootSignature.Get());
    list->SetGraphicsRootDescriptorTable(0, srvHeap->GetGPUDescriptorHandleForHeapStart());
    list->SetGraphicsRoot32BitConstants(1, 3, constants, 0);
    list->SetPipelineState(pipeline.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    TransitionIfNeeded(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    TransitionIfNeeded(list.Get(), backBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_PRESENT);
    if (ui.texture) {
        TransitionIfNeeded(list.Get(), ui.texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, ui.state);
    }
    if (FAILED(list->Close())) {
        LogCaptureFailure("composite close", requestId, E_FAIL);
        return CaptureOutcome::kFailed;
    }
    if (!SubmitNativeFSROwnerQueueOverlayCommandList(queue.Get(), list.Get())) {
        LogCaptureFailure("composite submit", requestId, E_FAIL);
        return CaptureOutcome::kFailed;
    }
    submitted = true;

    if (includeOverlay) {
        ce::dx12_ffx_suspend_overlay::RenderRequest overlay{};
        overlay.proxySwapChain = proxy;
        overlay.presentationQueue = queue.Get();
        overlay.targetResource = target.Get();
        overlay.targetState = D3D12_RESOURCE_STATE_PRESENT;
        overlay.clearTransparent = false;
        overlay.renderOverlay = true;
        overlay.applyPostProcess = false;
        overlay.routeName = "fsr-screenshot";
        overlay.submitCommandList = &SubmitNativeFSROwnerQueueOverlayCommandList;
        overlay.signalFence = &SignalNativeFSROwnerQueueOverlayFence;
        overlay.hdr = DX12_ResolveRuntimeOwnedOverlayTargetHDRState(backBufferDesc.Format);
        overlay.screenshotSlot = true;
        if (!ce::dx12_ffx_suspend_overlay::Render(overlay)) {
            // The reserved slot is still owned by a previous capture, or the renderer refused this one. Nothing waits
            // here: the composite already submitted is retired above and the request stays pending for a later Present.
            return CaptureOutcome::kRetryLater;
        }
    }

    const auto encoding = DXGIShared::ResolveSwapChainPresentationEncoding(proxy, backBufferDesc.Format);
    const bool queued = SaveDX12TextureAsScreenshotRaw(device.Get(), queue.Get(), target.Get(), shm, requestId,
                                                       encoding);
    if (!queued) {
        return CaptureOutcome::kFailed;
    }
    static std::atomic<int> s_queuedLogCount{0};
    const int queuedLog = s_queuedLogCount.fetch_add(1, std::memory_order_relaxed);
    if (queuedLog < 20 || (queuedLog % 60) == 0) {
        HookLogImportant(
            "[Screenshot] FSR no-callback capture queued (request=%llu overlay=%d gameUi=%d size=%llux%u format=%u "
            "log=%d)",
            static_cast<unsigned long long>(requestId), includeOverlay ? 1 : 0, hasUi,
            static_cast<unsigned long long>(backBufferDesc.Width), backBufferDesc.Height,
            static_cast<unsigned>(backBufferDesc.Format), queuedLog + 1);
    }
    return CaptureOutcome::kQueued;
}

}  // namespace

void DX12_ServiceFSRNoCallbackScreenshotIfPending(IDXGISwapChain* proxy) {
    SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    const uint64_t requestId = GetPendingScreenshotRequestId(shm);
    if (requestId == 0 || !proxy) {
        return;
    }
    const OverlayConfig overlayConfig = GetActiveDX12OverlayConfig(shm);
    const bool includeOverlay = overlayConfig.showOverlay && overlayConfig.screenshotIncludeOverlay;

    std::lock_guard<std::mutex> lock(g_captureMutex);
    CaptureOutcome outcome = CaptureLocked(proxy, shm, requestId, includeOverlay);
    if (outcome == CaptureOutcome::kRetryLater) {
        if (g_retryRequestId != requestId) {
            g_retryRequestId = requestId;
            g_retryCount = 0;
        }
        if (++g_retryCount > kMaxRetryPresents) {
            outcome = CaptureOutcome::kFailed;
        }
    }
    if (outcome != CaptureOutcome::kRetryLater) {
        g_retryRequestId = 0;
        g_retryCount = 0;
    }
    if (outcome == CaptureOutcome::kFailed && GetPendingScreenshotRequestId(shm) == requestId) {
        CompleteScreenshotRequest(shm, requestId, ScreenshotRequestStatus::Failed, ERROR_READ_FAULT);
    }
}
