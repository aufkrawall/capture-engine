#include <combaseapi.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <unknwn.h>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../../common/frame_timing.h"
#include "../../common/raii_helpers.h"
#include "../capture/shared_capture.h"
#include "../common/capture_base.h"
#include "../common/custom_overlay_dx12.h"
#include "../common/fg_detection.h"
#include "../common/hook_common.h"
#include "../common/input_manager.h"
#include "../common/overlay_compat.h"
#include "../common/overlay_adapter.h"
#include "../common/performance_metrics.h"
#include "../common/streamline_compat.h"

#include "../common/fps_limiter.h"
#include "../common/freeze_watchdog.h"
#include "../common/perf_logger.h"
#include "../common/swapchain_wrapper.h"
#include "../common/system_metrics.h"
#include "../wrappers/dxgi_swapchain_wrap.h"
#include "../wrappers/root_signature_parser.h"
#include "../wrappers/wrapper_hooks.h"
#include "dx11_hook.h"
#include "dx12_hook.h"
#include "ffx_hook.h"
#include "graphics_hook.h"
#include "lod_helper.h"

#include "../common/custom_overlay.h"
#include "../common/overlay_shader_bytecode.h"

#include "../wrappers/inline_hook.h"
#include "../wrappers/vtable_hook.h"
#include "../wrappers/wrapper_base.h"
#include "dxgi_shared.h"

// ============================================================================
// SpecialK-style Streamline Handling
// ============================================================================

// ============================================================================
// Typedefs for D3D12 functions
typedef void(STDMETHODCALLTYPE* ExecuteCommandListsPtr)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
typedef void(STDMETHODCALLTYPE* CreateSamplerPtr)(ID3D12Device*, const D3D12_SAMPLER_DESC*,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE);
typedef HRESULT(STDMETHODCALLTYPE* CreateCommittedResourcePtr)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*,
                                                               D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*,
                                                               D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID,
                                                               void**);
typedef HRESULT(WINAPI* PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)(const D3D12_ROOT_SIGNATURE_DESC*,
                                                            D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
typedef HRESULT(WINAPI* PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*,
                                                                      ID3DBlob**, ID3DBlob**);

// Global Function Pointers for detours (Visible to other modules)
ExecuteCommandListsPtr oExecuteCommandLists = nullptr;
CreateSamplerPtr oCreateSampler = nullptr;
CreateCommittedResourcePtr oCreateCommittedResource = nullptr;
PFN_D3D12_SERIALIZE_ROOT_SIGNATURE oSerializeRootSignature = nullptr;
PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE oSerializeVersionedRootSignature = nullptr;
static std::recursive_mutex g_ExecuteCommandListsHookStateMutex;
static std::map<void**, ExecuteCommandListsPtr> g_ExecuteCommandListsOriginalByVTable;
// ExecuteCommandLists runs many times per frame in CPU-bound workloads, so keep a
// lock-free cache for the most recently used vtable/original pair.
static std::atomic<void**> g_LastExecuteCommandListsVTable{nullptr};
static std::atomic<ExecuteCommandListsPtr> g_LastExecuteCommandListsOriginal{nullptr};

#if defined(__clang__) || defined(__GNUC__)
#define CE_RETURN_ADDRESS() __builtin_extract_return_addr(__builtin_return_address(0))
#elif defined(_MSC_VER)
#include <intrin.h>
#define CE_RETURN_ADDRESS() _ReturnAddress()
#else
#define CE_RETURN_ADDRESS() nullptr
#endif

// SwapChain Detour Pointers
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                        IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChainForHwnd)(IDXGIFactory2*, IUnknown*, HWND,
                                                               const DXGI_SWAP_CHAIN_DESC1*,
                                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                               IDXGISwapChain1**);

static PFN_CreateSwapChain oCreateSwapChain = nullptr;
static PFN_CreateSwapChainForHwnd oCreateSwapChainForHwnd = nullptr;

// ---------------------------------------------------------------------------
// Descriptor-free DX12 overlay backend.
//
// Renders the overlay using root constants + root SRV (ByteAddressBuffer for
// the font atlas).  No descriptor heaps are bound, so SetDescriptorHeaps is
// never called.  This avoids the NVIDIA driver stall triggered by
// SetDescriptorHeaps + OMSetRenderTargets(swapchain backbuffer) in the same
// command list.
//
// The command list and RTV are set externally before each Render() call via
// the static pointers below.
// ---------------------------------------------------------------------------

static ID3D12GraphicsCommandList* s_descFreeCmdList = nullptr;
static D3D12_CPU_DESCRIPTOR_HANDLE s_descFreeRtv = {};

class DX12DescFreeBackend : public CustomOverlay::RendererBackend {
public:
    ~DX12DescFreeBackend() override { Shutdown(); }

    // Non-virtual: create device-dependent resources (root sig, PSOs)
    bool InitDevice(ID3D12Device* dev, DXGI_FORMAT rtvFormat) {
        if (deviceReady_)
            return true;
        device_ = dev;
        rtvFormat_ = rtvFormat;
        if (!CreateRootSignature() || !CreatePSOs()) {
            Shutdown();
            return false;
        }
        if (!CreateBuffers()) {
            Shutdown();
            return false;
        }
        deviceReady_ = true;
        return true;
    }

    // RendererBackend: upload font atlas as a ByteAddressBuffer
    bool Initialize(int fontWidth, int fontHeight, const uint8_t* fontData) override {
        if (!device_ || !fontData)
            return false;
        fontWidth_ = fontWidth;
        fontHeight_ = fontHeight;

        size_t dataSize = (size_t)fontWidth * fontHeight * 4; // RGBA8

        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = dataSize;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        HRESULT hr = device_->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&fontBuffer_));
        if (FAILED(hr)) {
            HookLogImportant("DescFree: font buffer create failed hr=0x%08X", hr);
            return false;
        }

        void* mapped = nullptr;
        D3D12_RANGE readRange = {0, 0};
        hr = fontBuffer_->Map(0, &readRange, &mapped);
        if (FAILED(hr)) {
            HookLogImportant("DescFree: font buffer map failed hr=0x%08X", hr);
            fontBuffer_->Release();
            fontBuffer_ = nullptr;
            return false;
        }
        memcpy(mapped, fontData, dataSize);
        fontBuffer_->Unmap(0, nullptr);

        fontGpuAddr_ = fontBuffer_->GetGPUVirtualAddress();
        HookLogImportant("DescFree: font buffer ready (%dx%d, %zu bytes, gpu=0x%llX)",
                         fontWidth, fontHeight, dataSize, (unsigned long long)fontGpuAddr_);
        return true;
    }

    void Render(const std::vector<CustomOverlay::DrawVertex>& vertices,
                const std::vector<uint16_t>& indices,
                const std::vector<CustomOverlay::DrawCommand>& commands,
                int vpW, int vpH) override {
        auto* cmdList = s_descFreeCmdList;
        if (!cmdList || !deviceReady_ || !fontBuffer_ || vertices.empty())
            return;

        // Upload vertex data
        int slot = frameIdx_ % kPoolSize;
        frameIdx_++;
        size_t vbBytes = vertices.size() * sizeof(CustomOverlay::DrawVertex);
        if (vbBytes > vbSize_[slot]) {
            if (!ResizeBuffer(vb_[slot], vbPtr_[slot], vbSize_[slot], vbBytes))
                return;
        }
        memcpy(vbPtr_[slot], vertices.data(), vbBytes);

        // Upload index data
        size_t ibBytes = indices.size() * sizeof(uint16_t);
        if (ibBytes > ibSize_[slot]) {
            if (!ResizeBuffer(ib_[slot], ibPtr_[slot], ibSize_[slot], ibBytes))
                return;
        }
        memcpy(ibPtr_[slot], indices.data(), ibBytes);

        // Set pipeline — NO SetDescriptorHeaps!
        cmdList->SetGraphicsRootSignature(rootSig_);

        // Root constants: viewportW, viewportH, hdrMode, paperWhiteNits, fontW, fontH
        float constants[6] = {
            (float)vpW, (float)vpH,
            (float)hdrMode, paperWhiteNits,
            (float)fontWidth_, (float)fontHeight_
        };
        cmdList->SetGraphicsRoot32BitConstants(0, 6, constants, 0);

        // Root SRV: font buffer (ByteAddressBuffer at t0)
        cmdList->SetGraphicsRootShaderResourceView(1, fontGpuAddr_);

        // Render target + viewport
        cmdList->OMSetRenderTargets(1, &s_descFreeRtv, FALSE, nullptr);
        D3D12_VIEWPORT vp = {0, 0, (float)vpW, (float)vpH, 0, 1};
        D3D12_RECT scissor = {0, 0, (LONG)vpW, (LONG)vpH};
        cmdList->RSSetViewports(1, &vp);
        cmdList->RSSetScissorRects(1, &scissor);

        // Vertex/index buffers
        D3D12_VERTEX_BUFFER_VIEW vbv = {};
        vbv.BufferLocation = vb_[slot]->GetGPUVirtualAddress();
        vbv.SizeInBytes = (UINT)vbBytes;
        vbv.StrideInBytes = sizeof(CustomOverlay::DrawVertex);
        cmdList->IASetVertexBuffers(0, 1, &vbv);

        D3D12_INDEX_BUFFER_VIEW ibv = {};
        ibv.BufferLocation = ib_[slot]->GetGPUVirtualAddress();
        ibv.SizeInBytes = (UINT)ibBytes;
        ibv.Format = DXGI_FORMAT_R16_UINT;
        cmdList->IASetIndexBuffer(&ibv);

        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // Draw
        ID3D12PipelineState* lastPSO = nullptr;
        for (const auto& cmd : commands) {
            auto* pso = cmd.useTexture ? psoTextured_ : psoSolid_;
            if (pso != lastPSO) {
                cmdList->SetPipelineState(pso);
                lastPSO = pso;
            }
            cmdList->DrawIndexedInstanced(cmd.indexCount, 1, cmd.indexOffset, 0, 0);
        }
    }

    void Shutdown() override {
        for (int i = 0; i < kPoolSize; i++) {
            if (vb_[i]) { vb_[i]->Unmap(0, nullptr); vb_[i]->Release(); vb_[i] = nullptr; }
            if (ib_[i]) { ib_[i]->Unmap(0, nullptr); ib_[i]->Release(); ib_[i] = nullptr; }
            vbPtr_[i] = nullptr;
            ibPtr_[i] = nullptr;
            vbSize_[i] = 0;
            ibSize_[i] = 0;
        }
        if (fontBuffer_) { fontBuffer_->Release(); fontBuffer_ = nullptr; }
        if (psoTextured_) { psoTextured_->Release(); psoTextured_ = nullptr; }
        if (psoSolid_) { psoSolid_->Release(); psoSolid_ = nullptr; }
        if (rootSig_) { rootSig_->Release(); rootSig_ = nullptr; }
        fontGpuAddr_ = 0;
        deviceReady_ = false;
    }

private:
    bool CreateRootSignature() {
        // Parameter 0: 6 root constants at b0
        //   [0-1] viewportSize, [2] hdrMode, [3] paperWhiteNits, [4-5] fontTexSize
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.RegisterSpace = 0;
        params[0].Constants.Num32BitValues = 6;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Parameter 1: root SRV at t0 (font ByteAddressBuffer)
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[1].Descriptor.ShaderRegister = 0;
        params[1].Descriptor.RegisterSpace = 0;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
        rsDesc.NumParameters = 2;
        rsDesc.pParameters = params;
        rsDesc.NumStaticSamplers = 0;  // No sampler needed — manual bilinear in shader
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ID3DBlob* blob = nullptr;
        ID3DBlob* err = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(
            &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
        if (FAILED(hr)) {
            HookLogImportant("DescFree: SerializeRootSignature failed hr=0x%08X", hr);
            if (err) err->Release();
            return false;
        }
        hr = device_->CreateRootSignature(0, blob->GetBufferPointer(),
                                          blob->GetBufferSize(), IID_PPV_ARGS(&rootSig_));
        blob->Release();
        if (err) err->Release();
        if (FAILED(hr)) {
            HookLogImportant("DescFree: CreateRootSignature failed hr=0x%08X", hr);
            return false;
        }
        return true;
    }

    bool CreatePSOs() {
        D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16,
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = {inputLayout, 3};
        psoDesc.pRootSignature = rootSig_;
        psoDesc.VS = {g_VS_5_0, sizeof(g_VS_5_0)};
        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psoDesc.RasterizerState.DepthClipEnable = TRUE;

        // Alpha blending
        D3D12_RENDER_TARGET_BLEND_DESC& blendRT = psoDesc.BlendState.RenderTarget[0];
        blendRT.BlendEnable = TRUE;
        blendRT.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blendRT.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blendRT.BlendOp = D3D12_BLEND_OP_ADD;
        blendRT.SrcBlendAlpha = D3D12_BLEND_ONE;
        blendRT.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blendRT.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blendRT.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = rtvFormat_;
        psoDesc.SampleDesc.Count = 1;

        // Textured PSO — uses ByteAddressBuffer (descriptor-free)
        psoDesc.PS = {g_PS_Textured_DescFree_5_0, sizeof(g_PS_Textured_DescFree_5_0)};
        HRESULT hr = device_->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&psoTextured_));
        if (FAILED(hr)) {
            HookLogImportant("DescFree: CreatePSO(textured) failed hr=0x%08X", hr);
            return false;
        }

        // Solid PSO — no texture, uses same root sig (t0 unused)
        psoDesc.PS = {g_PS_Solid_5_0, sizeof(g_PS_Solid_5_0)};
        hr = device_->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&psoSolid_));
        if (FAILED(hr)) {
            HookLogImportant("DescFree: CreatePSO(solid) failed hr=0x%08X", hr);
            return false;
        }

        HookLogImportant("DescFree: PSOs created (fmt=%d)", rtvFormat_);
        return true;
    }

    bool CreateBuffers() {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        D3D12_RANGE readRange = {0, 0};
        for (int i = 0; i < kPoolSize; i++) {
            rd.Width = kInitVBBytes;
            HRESULT hr = device_->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vb_[i]));
            if (FAILED(hr)) return false;
            vb_[i]->Map(0, &readRange, &vbPtr_[i]);
            vbSize_[i] = kInitVBBytes;

            rd.Width = kInitIBBytes;
            hr = device_->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ib_[i]));
            if (FAILED(hr)) return false;
            ib_[i]->Map(0, &readRange, &ibPtr_[i]);
            ibSize_[i] = kInitIBBytes;
        }
        return true;
    }

    bool ResizeBuffer(ID3D12Resource*& buf, void*& ptr, size_t& curSize, size_t needed) {
        size_t newSize = curSize;
        while (newSize < needed) newSize *= 2;

        buf->Unmap(0, nullptr);
        buf->Release();

        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = newSize;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        HRESULT hr = device_->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buf));
        if (FAILED(hr)) return false;

        D3D12_RANGE readRange = {0, 0};
        buf->Map(0, &readRange, &ptr);
        curSize = newSize;
        return true;
    }

    static constexpr int kPoolSize = 4;
    static constexpr size_t kInitVBBytes = 4096 * 20;  // 4096 vertices * 20 bytes
    static constexpr size_t kInitIBBytes = 8192 * 2;   // 8192 indices * 2 bytes

    ID3D12Device* device_ = nullptr;
    DXGI_FORMAT rtvFormat_ = DXGI_FORMAT_R8G8B8A8_UNORM;
    bool deviceReady_ = false;

    ID3D12RootSignature* rootSig_ = nullptr;
    ID3D12PipelineState* psoTextured_ = nullptr;
    ID3D12PipelineState* psoSolid_ = nullptr;

    ID3D12Resource* fontBuffer_ = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS fontGpuAddr_ = 0;
    int fontWidth_ = 0;
    int fontHeight_ = 0;

    ID3D12Resource* vb_[kPoolSize] = {};
    ID3D12Resource* ib_[kPoolSize] = {};
    void* vbPtr_[kPoolSize] = {};
    void* ibPtr_[kPoolSize] = {};
    size_t vbSize_[kPoolSize] = {};
    size_t ibSize_[kPoolSize] = {};
    int frameIdx_ = 0;
};

static DX12DescFreeBackend* g_DescFreeBackend = nullptr;

// --- DX12 Overlay State Management ---
struct DX12OverlayState {
    // Large pool size ensures we never need to wait for GPU.
    // Even at 60fps with 100ms GPU latency, only 6 allocators are in flight.
    // 16 provides 2.5x headroom - allocator is always ready, zero waiting.
    static const int ALLOC_POOL_SIZE = 16;
    std::vector<ID3D12CommandAllocator*> allocators;
    ID3D12GraphicsCommandList* cmdList = nullptr;
    int allocIndex = 0;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = nullptr;
    UINT64 currentFenceValue = 0;
    std::vector<UINT64> fenceValues;
    ID3D12DescriptorHeap* rtvDescHeap = nullptr;
    ID3D12DescriptorHeap* srvDescHeap = nullptr;
    UINT rtvDescriptorSize = 0;
    std::vector<ID3D12Resource*> backBuffers;
    bool overlayInit = false;
    bool syncInit = false;
    int cachedWidth = 0;
    int cachedHeight = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT bufferCount = 0;
    IDXGISwapChain* cachedSwapChain = nullptr;

    // Offscreen render target for overlay compositing.
    // Avoids OMSetRenderTargets(swapchain) + SetDescriptorHeaps on the same ECL,
    // which causes GPU pipeline stalls (~40% utilization loss) in some games.
    // Flow: copy backbuffer → offscreen, render overlay → offscreen, copy back.
    ID3D12Resource* offscreenRT = nullptr;
    ID3D12DescriptorHeap* offscreenRtvHeap = nullptr;
    UINT offscreenWidth = 0;
    UINT offscreenHeight = 0;
    DXGI_FORMAT offscreenFormat = DXGI_FORMAT_UNKNOWN;

    // Dedicated overlay command queue for FG-safe rendering.
    // When FG is active, overlay commands execute on this queue with CPU-side
    // fence synchronization to avoid interfering with Streamline's game queue
    // management.  When FG is not active, overlay commands go on the game queue
    // directly (zero CPU waits).
    ID3D12CommandQueue* overlayQueue = nullptr;

    // Cross-queue fence: game queue signals to mark work completion, then
    // CPU-side wait before submitting overlay work on the overlay queue.
    // GPU-side CommandQueue::Wait was removed (NVIDIA WaitImpl Alt+Tab hang).
    ID3D12Fence* crossQueueFence = nullptr;
    UINT64 crossQueueFenceValue = 0;
    HANDLE crossQueueFenceEvent = nullptr;

    IDXGISwapChain3* cachedSC3 = nullptr;  // cached from first successful QI


    // D3D11On12 overlay bridge: renders overlay via D3D11 on top of the D3D12
    // backbuffer.  D3D11 doesn't use descriptor heaps, avoiding the NVIDIA
    // driver stall triggered by SetDescriptorHeaps + OMSetRenderTargets(swapchain).
    ID3D11Device* d3d11on12Device = nullptr;
    ID3D11DeviceContext* d3d11on12Context = nullptr;
    ID3D11On12Device* d3d11on12 = nullptr;
    std::vector<ID3D11Resource*> d3d11WrappedBBs;
    std::vector<ID3D11RenderTargetView*> d3d11RTVs;
    bool d3d11on12Init = false;

    void Cleanup() {
        // FG-SAFE: backBuffers no longer holds references
        backBuffers.clear();
        if (offscreenRT) {
            offscreenRT->Release();
            offscreenRT = nullptr;
        }
        if (offscreenRtvHeap) {
            offscreenRtvHeap->Release();
            offscreenRtvHeap = nullptr;
        }
        offscreenWidth = 0;
        offscreenHeight = 0;
        offscreenFormat = DXGI_FORMAT_UNKNOWN;
        if (rtvDescHeap) {
            rtvDescHeap->Release();
            rtvDescHeap = nullptr;
        }
        if (srvDescHeap) {
            srvDescHeap->Release();
            srvDescHeap = nullptr;
        }
        for (auto* alloc : allocators)
            if (alloc)
                alloc->Release();
        allocators.clear();
        if (cmdList) {
            cmdList->Release();
            cmdList = nullptr;
        }
        if (fence) {
            fence->Release();
            fence = nullptr;
        }
        if (fenceEvent) {
            CloseHandle(fenceEvent);
            fenceEvent = nullptr;
        }
        // CRITICAL: Release dedicated overlay queue
        if (overlayQueue) {
            overlayQueue->Release();
            overlayQueue = nullptr;
        }
        // Release cross-queue synchronization fence and event
        if (crossQueueFenceEvent) {
            CloseHandle(crossQueueFenceEvent);
            crossQueueFenceEvent = nullptr;
        }
        if (crossQueueFence) {
            crossQueueFence->Release();
            crossQueueFence = nullptr;
        }
        overlayInit = false;
        syncInit = false;
        crossQueueFenceValue = 0;
        cachedSC3 = nullptr;  // weak ref, no Release needed
        // D3D11On12 cleanup
        for (auto* rtv : d3d11RTVs) if (rtv) rtv->Release();
        d3d11RTVs.clear();
        for (auto* res : d3d11WrappedBBs) if (res) res->Release();
        d3d11WrappedBBs.clear();
        if (d3d11on12) { d3d11on12->Release(); d3d11on12 = nullptr; }
        if (d3d11on12Context) { d3d11on12Context->Release(); d3d11on12Context = nullptr; }
        if (d3d11on12Device) { d3d11on12Device->Release(); d3d11on12Device = nullptr; }
        d3d11on12Init = false;
    }
};

static DX12OverlayState g_State;
static SharedCaptureD3D12 g_SharedCaptureD3D12;
static OverlayAdapter g_D3D11On12Adapter;

// CRITICAL FIX: Use atomic pointers for thread-safe access
// These are read/written from multiple threads (hook thread, present thread, etc.)
std::atomic<ID3D12Device*> g_Device{nullptr};
std::atomic<ID3D12CommandQueue*> g_CommandQueue{nullptr};
std::recursive_mutex g_CommandQueueMutex;

// CRITICAL FIX: Thread-safe accessors for g_Device and g_CommandQueue
// These functions acquire the mutex and return a reference-counted pointer
// to prevent use-after-free when the queue/device is destroyed on another
// thread
struct DX12Context {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;

    DX12Context() = default;

    DX12Context(ID3D12Device* d, ID3D12CommandQueue* q) : device(d), queue(q) {
        if (device)
            device->AddRef();
        if (queue)
            queue->AddRef();
    }

    ~DX12Context() {
        if (device) {
            device->Release();
            device = nullptr;
        }
        if (queue) {
            queue->Release();
            queue = nullptr;
        }
    }

    // Disable copy to prevent accidental double-release
    DX12Context(const DX12Context&) = delete;
    DX12Context& operator=(const DX12Context&) = delete;

    // Enable move
    DX12Context(DX12Context&& other) noexcept : device(other.device), queue(other.queue) {
        other.device = nullptr;
        other.queue = nullptr;
    }

    DX12Context& operator=(DX12Context&& other) noexcept {
        if (this != &other) {
            if (device)
                device->Release();
            if (queue)
                queue->Release();
            device = other.device;
            queue = other.queue;
            other.device = nullptr;
            other.queue = nullptr;
        }
        return *this;
    }

    bool IsValid() const {
        return device != nullptr && queue != nullptr;
    }
};

// Thread-safe accessor - ALWAYS use this instead of direct
// g_Device/g_CommandQueue access
static DX12Context GetDX12Context() {
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    return DX12Context(g_Device.load(), g_CommandQueue.load());
}

static std::atomic<uint64_t> g_FrameIndex{0};
static std::atomic<int> g_CommandListsExecutedThisFrame{0};
static std::atomic<uint64_t> g_FGDebugFrameCount{0};

// Primary game queue — set once from the first ECL call (always the game's queue,
// since the game creates its queue before any FG runtime).  Used to filter ECL
// counting: only game-queue ECL calls count toward frame classification.
// FG runtimes (FSR FG) create their own queues that share the vtable, so our ECL
// hook fires for them too.  Without this filter, interpolated frames look like
// real frames (similar ECL counts).
static std::atomic<ID3D12CommandQueue*> g_PrimaryGameQueue{nullptr};
static std::atomic<bool> g_KnownDLSSFGModuleSeen{false};

// Last swapchain reference for device change detection
static IDXGISwapChain* g_LastSwapChain = nullptr;
// Pending swapchain cleanup - released after ResizeBuffers completes
static IDXGISwapChain* g_PendingSwapChainCleanup = nullptr;

// IPC ready flag
static bool g_IPCReady = false;

ID3D12Resource* g_DummyBackBuffer = nullptr;

// Swapchain queue - captured at swapchain creation time, preferred for overlay
// rendering to ensure barriers execute on the queue DXGI synchronises with.
static ID3D12CommandQueue* g_SwapchainQueue = nullptr;

// Guard flag: skip queue capture during temp swapchain creation
static std::atomic<bool> g_CreatingTempSwapchain{false};

// LOCK HIERARCHY (MUST be acquired in this order to prevent deadlocks):
// 1. g_OverlayMutex (outermost - protects overlay state)
// 2. g_CommandQueueMutex (protects command queue pointer)
// 3. g_DX12CaptureMutex (innermost - protects capture state)
//
// Rule: When acquiring multiple locks, always acquire in order above.
//       Use std::lock_guard with std::adopt_lock when using try_lock().
static std::recursive_mutex g_OverlayMutex;
static std::recursive_mutex g_DX12CaptureMutex;
static std::atomic<bool> g_InSwapchainResizeCleanup{false};

// Frame counter for post-ImGui-init delay (skip first frame to let GPU
// stabilize)
static std::atomic<int> s_framesSinceInit{0};
static std::atomic<int> s_framesBeforeInit{0};

// Use pointer to prevent static destructor execution in non-game processes
// (Explorer fix)
DX12Hook* g_dx12HookInstance = nullptr;

std::recursive_mutex g_DeviceQueuesMutex;
std::map<ID3D12Device*, ID3D12CommandQueue*> g_DeviceQueues;

// CPU Prerender Limit State (DX12)
static std::vector<ID3D12Fence*> g_PrerenderFences;
static std::vector<HANDLE> g_PrerenderEvents;
static uint64_t g_PrerenderFrameIndex = 0;
static std::mutex g_PrerenderMutex;

// ECL piggyback overlay: for games (like GTA5 Enhanced) that reject separate ECL
// submissions touching backbuffers, render the overlay by appending our command
// list to the game's own ExecuteCommandLists call.
static std::atomic<bool> g_PiggybackOverlayActive{false};
static std::atomic<bool> g_PiggybackDrawnThisFrame{false};


// Deferred Signal: avoid the NVIDIA driver stall caused by Signal between our
// overlay ECL and Present.  Instead of calling Signal immediately after our ECL,
// we defer it to the START of the NEXT frame's Present hook (after the previous
// Present has already been called).  This keeps the ECL→Present path clean.
static std::atomic<UINT64> g_deferredSignalValue{0};
static std::atomic<int> g_deferredSignalAllocIdx{-1};
// Track which queue the deferred ECL was submitted on, so the deferred Signal
// goes to the same queue.  When FG runtimes create swapchains with their own
// queue, this may differ from g_CommandQueue.
static std::atomic<ID3D12CommandQueue*> g_deferredSignalQueue{nullptr};

// Re-entrancy guard: set when the current thread is inside DetourECL.
// During Alt+Tab, D3D12's internal WaitImpl inside ECL can pump window messages
// (DefWindowProc), which may trigger Present → ProcessFrame.  If ProcessFrame
// submits an overlay ECL while the outer ECL is still inside WaitImpl, a second
// WaitImpl cascades and the render thread hangs.  ProcessFrame checks this flag
// and skips overlay rendering when it's set.
static thread_local bool s_insideECL = false;

static bool KnownDLSSFGModuleLoaded() {
    if (g_KnownDLSSFGModuleSeen.load(std::memory_order_acquire)) {
        return true;
    }

    constexpr const wchar_t* kKnownDLSSFGModules[] = {
        L"sl.interposer.dll",
        L"sl.common.dll",
        L"sl.dlss.dll",
        L"sl.dlss_g.dll",
        L"nvngx_dlssg.dll",
        L"nvngx_dlss.dll",
    };

    for (const wchar_t* moduleName : kKnownDLSSFGModules) {
        if (GetModuleHandleW(moduleName)) {
            g_KnownDLSSFGModuleSeen.store(true, std::memory_order_release);
            return true;
        }
    }

    return false;
}

static bool CanUseFSRFGHeuristics(const char** blockedReason = nullptr) {
    // Only block when DLSS FG is confirmed active WITH a known multiplier.
    // When DLSS modules are merely loaded but FG is off (or API state is transiently
    // toggling — common when switching to FSR FG), heuristics are safe.  The
    // g_PrimaryGameQueue filter ensures only game-queue ECL calls are counted,
    // preventing false positives from FG runtime queues.
    if (g_FGCompat.IsDLSSFGApiActive()) {
        int mult = g_FGCompat.GetFGMultiplier();
        if (mult >= 2) {
            if (blockedReason) {
                *blockedReason = "DLSS FG is actively generating frames";
            }
            return false;
        }
    }

    if (blockedReason) {
        *blockedReason = nullptr;
    }
    return true;
}

static bool UpdateHeuristicFSRFGState(bool active, const char* source) {
    const char* blockedReason = nullptr;
    if (!CanUseFSRFGHeuristics(&blockedReason)) {
        g_FGCompat.SetHeuristicFSRFGActive(false);

        if (active) {
            static std::atomic<int> s_suppressedLogCount{0};
            if (s_suppressedLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
                HookLog("DX12: Suppressing %s FSR FG heuristic because %s", source,
                        blockedReason ? blockedReason : "it is unsafe");
            }
        }
        return false;
    }

    g_FGCompat.SetHeuristicFSRFGActive(active);
    return true;
}

void ShutdownImGui();
void CleanupOverlay();
void CleanupRTVs();
static void DrawOverlay(ID3D12GraphicsCommandList* list, bool isRealFrame, UINT bufferIdx,
                        D3D12_CPU_DESCRIPTOR_HANDLE* rtvOverride = nullptr);

static constexpr ULONGLONG kStartupOverlayWindowPollMs = 100;
static constexpr ULONGLONG kStartupOverlayInitGraceMs = 500;
static constexpr ULONGLONG kStartupOverlayWarmupMs = 500;
static constexpr ULONGLONG kStartupOverlayQuietPeriodMs = 200;
static constexpr ULONGLONG kStartupOverlayPostResumeSettleMs = 100;
static constexpr ULONGLONG kStartupOverlayPostBackendInitSettleMs = 0;
static constexpr ULONGLONG kStartupOverlayPostRTVInitSettleMs = 0;
static constexpr ULONGLONG kStartupOverlayPostSyncInitSettleMs = 100;
static constexpr ULONGLONG kStartupOverlayPostResourcePrimeSettleMs = 100;
static constexpr ULONGLONG kStartupOverlayFirstDrawProbeSettleMs = 0;
static constexpr ULONGLONG kStartupOverlayLoadedRenderModuleMaxBlockMs = 500;
static constexpr ULONGLONG kStartupOverlayRenderModuleQuietPeriodMs = 500;
static constexpr DWORD kOverlayCrossQueueWaitMs = 16;

enum class StartupOverlayActivationStage {
    kNone = 0,
    kDelayRTVInitAfterBackendInit,
    kDelaySyncInitAfterRTVInit,
};

enum class StartupOverlayFirstDrawProbeStage {
    kNone = 0,
    kBackbufferTouchOnly,
    kPipelineStateOnly,
    kActualRender,
    kComplete,
};

static StartupOverlayActivationStage s_startupOverlayActivationStage = StartupOverlayActivationStage::kNone;
static StartupOverlayFirstDrawProbeStage s_startupOverlayFirstDrawProbeStage =
    StartupOverlayFirstDrawProbeStage::kNone;
static ULONGLONG s_startupOverlayActivationStageMs = 0;
static ULONGLONG s_startupOverlaySyncInitMs = 0;
static ULONGLONG s_startupOverlayResourcePrimeMs = 0;
static ULONGLONG s_startupOverlayFirstDrawProbeMs = 0;
static std::atomic<ULONGLONG> s_lastStartupBlockingRenderModuleActivityMs{0};

static const char* GetStartupOverlayFirstDrawProbeStageName(StartupOverlayFirstDrawProbeStage stage) {
    switch (stage) {
    case StartupOverlayFirstDrawProbeStage::kBackbufferTouchOnly:
        return "backbuffer touch";
    case StartupOverlayFirstDrawProbeStage::kPipelineStateOnly:
        return "pipeline state setup";
    case StartupOverlayFirstDrawProbeStage::kActualRender:
        return "real overlay draw";
    case StartupOverlayFirstDrawProbeStage::kComplete:
        return "complete";
    case StartupOverlayFirstDrawProbeStage::kNone:
    default:
        return "overlay probe";
    }
}

static void ResetStartupOverlayBackendActivationStage() {
    s_startupOverlayActivationStage = StartupOverlayActivationStage::kNone;
    s_startupOverlayFirstDrawProbeStage = StartupOverlayFirstDrawProbeStage::kNone;
    s_startupOverlayActivationStageMs = 0;
    s_startupOverlaySyncInitMs = 0;
    s_startupOverlayResourcePrimeMs = 0;
    s_startupOverlayFirstDrawProbeMs = 0;
    s_lastStartupBlockingRenderModuleActivityMs.store(0, std::memory_order_release);
}

static bool IsActualFrameGenerationActive() {
    // FSR FG API (ffxCreateContext-based) detection
    if (g_FGCompat.IsFSRFGApiActive())
        return true;

    // DLSS FG: require both the API flag AND a confirmed multiplier (>= 2).
    // During FSR FG mode switches, the DLSS FG API state can toggle transiently
    // without setting a multiplier.  Without the multiplier check, the transient
    // state would trigger dedicated queue creation/destruction cycles → flickering.
    if (g_FGCompat.IsDLSSFGApiActive()) {
        int mult = g_FGCompat.GetFGMultiplier();
        if (mult >= 2)
            return true;
    }

    return false;
}

static ExecuteCommandListsPtr GetOriginalExecuteCommandLists(ID3D12CommandQueue* queue);

static bool ShouldUseDedicatedOverlayQueue(const char** disabledByOverlayModule = nullptr) {
    const char* overlayModule = ce::overlay_compat::GetStartupBlockingOverlayModuleName();
    const bool processNeedsDelay = ce::overlay_compat::ShouldPreemptivelyDelayDX12OverlayInitForProcess(g_ProcessName);
    const bool actualFGActive = IsActualFrameGenerationActive();
    const bool shouldUseDedicatedQueue =
        ce::overlay_compat::ShouldUseDedicatedDX12OverlayQueue(actualFGActive, processNeedsDelay, overlayModule);
    if (disabledByOverlayModule) {
        *disabledByOverlayModule = shouldUseDedicatedQueue ? nullptr : overlayModule;
    }

    return shouldUseDedicatedQueue;
}

static bool WaitForGameQueueBeforeDedicatedOverlaySubmission(ID3D12CommandQueue* gameQueue, const char* phase) {
    if (!g_State.overlayQueue || !g_State.crossQueueFence || !g_State.crossQueueFenceEvent) {
        return true;
    }
    if (!gameQueue) {
        HookLogImportant("DX12: Cannot synchronize dedicated overlay queue before %s because the game queue is null",
                         phase ? phase : "overlay submission");
        return false;
    }

    const UINT64 waitValue = g_State.crossQueueFenceValue + 1;
    HRESULT signalHr = gameQueue->Signal(g_State.crossQueueFence, waitValue);
    if (FAILED(signalHr)) {
        HookLogImportant("DX12: Failed to signal game queue before %s on dedicated overlay queue hr=0x%08X",
                         phase ? phase : "overlay submission", signalHr);
        return false;
    }

    g_State.crossQueueFenceValue = waitValue;
    if (g_State.crossQueueFence->GetCompletedValue() >= waitValue) {
        return true;
    }

    HRESULT setHr = g_State.crossQueueFence->SetEventOnCompletion(waitValue, g_State.crossQueueFenceEvent);
    if (FAILED(setHr)) {
        HookLogImportant("DX12: Failed to arm cross-queue wait before %s hr=0x%08X",
                         phase ? phase : "overlay submission", setHr);
        return false;
    }

    DWORD waitHr = WaitForSingleObject(g_State.crossQueueFenceEvent, kOverlayCrossQueueWaitMs);
    if (waitHr == WAIT_OBJECT_0) {
        static std::atomic<int> s_crossQueueWaitSuccessLogCount{0};
        if (s_crossQueueWaitSuccessLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
            HookLogImportant("DX12: Dedicated overlay queue synchronized with game queue for %s (value=%llu)",
                             phase ? phase : "overlay submission", static_cast<unsigned long long>(waitValue));
        }
        return true;
    }

    if (waitHr == WAIT_TIMEOUT) {
        HookLogImportant("DX12: Timed out waiting for game queue before %s on dedicated overlay queue (value=%llu)",
                         phase ? phase : "overlay submission", static_cast<unsigned long long>(waitValue));
    } else {
        HookLogImportant("DX12: WaitForSingleObject failed before %s on dedicated overlay queue result=%lu",
                         phase ? phase : "overlay submission", waitHr);
    }
    return false;
}

static bool SubmitOverlayCommandList(ID3D12CommandQueue* gameQueue, ID3D12CommandList* list, int allocatorIndex,
                                     const char* phase, bool requireGameQueueDrain) {
    // Use the dedicated queue only when FG is actually active.  The queue stays
    // alive across FG mode switches to avoid destructive reinit, but submissions
    // go to the game queue when FG is inactive.
    bool useDedicated = g_State.overlayQueue && ShouldUseDedicatedOverlayQueue();
    ID3D12CommandQueue* submitQueue = useDedicated ? g_State.overlayQueue : gameQueue;
    if (!submitQueue || !list) {
        HookLogImportant("DX12: Cannot submit %s (submitQueue=%p, list=%p)", phase ? phase : "overlay command list",
                         submitQueue, list);
        return false;
    }

    if (requireGameQueueDrain && submitQueue != gameQueue &&
        !WaitForGameQueueBeforeDedicatedOverlaySubmission(gameQueue, phase)) {
        return false;
    }

    static std::atomic<int> s_submitLogCount{0};
    if (s_submitLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
        HookLogImportant("DX12: Submitting %s on %s queue (submitQueue=%p, gameQueue=%p, allocator=%d)",
                         phase ? phase : "overlay command list", submitQueue == gameQueue ? "game" : "dedicated overlay",
                         submitQueue, gameQueue, allocatorIndex);
    }

    ID3D12CommandList* lists[] = {list};
    ExecuteCommandListsPtr origECL = GetOriginalExecuteCommandLists(submitQueue);
    if (origECL) {
        origECL(submitQueue, 1, lists);
    } else {
        submitQueue->ExecuteCommandLists(1, lists);
    }

    if (g_State.fence) {
        UINT64 next = g_State.currentFenceValue + 1;
        HRESULT signalHr = submitQueue->Signal(g_State.fence, next);
        if (SUCCEEDED(signalHr)) {
            g_State.currentFenceValue = next;
            if (allocatorIndex >= 0 && allocatorIndex < static_cast<int>(g_State.fenceValues.size())) {
                g_State.fenceValues[allocatorIndex] = next;
            }
        } else {
            HookLog("DX12: Overlay fence signal failed for %s hr=0x%08X",
                    phase ? phase : "overlay command list", signalHr);
        }
    }

    return true;
}


static void NoteStartupBlockingRenderModuleActivityFromECL(ID3D12CommandQueue* queue, const void* callerAddress) {
    // Fast early-out: once overlay probe is complete, no need to track anymore
    if (s_startupOverlayFirstDrawProbeStage == StartupOverlayFirstDrawProbeStage::kComplete) {
        return;
    }

    if (!queue || !callerAddress ||
        !ce::overlay_compat::ShouldPreemptivelyDelayDX12OverlayInitForProcess(g_ProcessName) ||
        IsActualFrameGenerationActive()) {
        return;
    }

    const char* blockingRenderModule = ce::overlay_compat::GetStartupBlockingOverlayRenderModuleName();
    if (!blockingRenderModule) {
        return;
    }

    // Cache the blocking module handle to avoid GetModuleHandleA kernel call
    // on every ECL invocation. The module won't unload during gameplay.
    static HMODULE s_cachedBlockingModule = nullptr;
    static bool s_cachedBlockingModuleLookedUp = false;
    if (!s_cachedBlockingModuleLookedUp) {
        s_cachedBlockingModule = GetModuleHandleA(blockingRenderModule);
        s_cachedBlockingModuleLookedUp = true;
    }
    HMODULE blockingModuleHandle = s_cachedBlockingModule;
    if (!blockingModuleHandle) {
        // Module not loaded yet — retry next time
        s_cachedBlockingModuleLookedUp = false;
        return;
    }

    HMODULE callerModuleHandle = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(callerAddress), &callerModuleHandle) ||
        !callerModuleHandle || callerModuleHandle != blockingModuleHandle) {
        return;
    }

    const ULONGLONG now = GetTickCount64();
    s_lastStartupBlockingRenderModuleActivityMs.store(now, std::memory_order_release);

    static std::atomic<int> s_blockingModuleActivityLogCount{0};
    if (s_blockingModuleActivityLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
        char modulePath[MAX_PATH] = {};
        const char* moduleForLog = blockingRenderModule;
        if (GetModuleFileNameA(callerModuleHandle, modulePath, MAX_PATH) > 0) {
            moduleForLog = modulePath;
        }
        HookLogImportant(
            "DX12: Startup-blocking render module activity detected via ExecuteCommandLists (module=%s, queue=%p, caller=%p)",
            moduleForLog, queue, callerAddress);
    }
}

static bool ShouldSuppressOverlayForStartupCompat(HWND gameWindow, const char** overlayModule = nullptr,
                                                  ULONGLONG* remainingMs = nullptr,
                                                  ce::overlay_compat::AuxiliaryProcessWindowInfo* activeWindow = nullptr) {
    const char* blockingOverlayModule = ce::overlay_compat::GetStartupBlockingOverlayModuleName();
    const bool actualFGActive = IsActualFrameGenerationActive();
    static ULONGLONG s_firstOverlayDetectedMs = 0;
    static ULONGLONG s_lastPollMs = 0;
    static ULONGLONG s_lastVisibleMs = 0;
    static bool s_auxiliaryWindowVisible = false;
    static ce::overlay_compat::AuxiliaryProcessWindowInfo s_auxiliaryWindow = {};
    if (overlayModule) {
        *overlayModule = blockingOverlayModule;
    }
    if (remainingMs) {
        *remainingMs = 0;
    }
    if (activeWindow) {
        *activeWindow = {};
    }

    if (!blockingOverlayModule || actualFGActive || !IsWindow(gameWindow)) {
        s_firstOverlayDetectedMs = 0;
        s_lastPollMs = 0;
        s_lastVisibleMs = 0;
        s_auxiliaryWindowVisible = false;
        s_auxiliaryWindow = {};
        return false;
    }

    const ULONGLONG now = GetTickCount64();
    if (s_firstOverlayDetectedMs == 0) {
        s_firstOverlayDetectedMs = now;
    }

    if (s_lastPollMs == 0 || now - s_lastPollMs >= kStartupOverlayWindowPollMs) {
        s_lastPollMs = now;

        ce::overlay_compat::AuxiliaryProcessWindowInfo visibleWindow = {};
        s_auxiliaryWindowVisible =
            ce::overlay_compat::FindAuxiliaryProcessWindow(GetCurrentProcessId(), gameWindow, &visibleWindow);
        if (s_auxiliaryWindowVisible) {
            s_lastVisibleMs = now;
            s_auxiliaryWindow = visibleWindow;
        } else {
            s_auxiliaryWindow = {};
        }
    }

    if (activeWindow) {
        *activeWindow = s_auxiliaryWindow;
    }

    const ULONGLONG msSinceOverlayDetected = now - s_firstOverlayDetectedMs;
    const ULONGLONG msSinceLastVisible =
        s_lastVisibleMs == 0 ? kStartupOverlayQuietPeriodMs : (now - s_lastVisibleMs);
    const bool suppress = ce::overlay_compat::ShouldSuppressDX12OverlayForStartup(
        true, actualFGActive, s_auxiliaryWindowVisible, msSinceOverlayDetected, kStartupOverlayWarmupMs,
        msSinceLastVisible, kStartupOverlayQuietPeriodMs);
    if (remainingMs && suppress) {
        ULONGLONG warmupRemaining =
            msSinceOverlayDetected < kStartupOverlayWarmupMs ? (kStartupOverlayWarmupMs - msSinceOverlayDetected) : 0;
        ULONGLONG quietRemaining =
            !s_auxiliaryWindowVisible && msSinceLastVisible < kStartupOverlayQuietPeriodMs
                ? (kStartupOverlayQuietPeriodMs - msSinceLastVisible)
                : 0;
        *remainingMs = std::max(warmupRemaining, quietRemaining);
    }
    return suppress;
}

static bool ShouldDeferOverlayInitForStartupCompat(HWND gameWindow, ULONGLONG* remainingMs = nullptr) {
    static ULONGLONG s_firstDeferredInitEligibleMs = 0;
    if (remainingMs) {
        *remainingMs = 0;
    }

    if (!ce::overlay_compat::ShouldPreemptivelyDelayDX12OverlayInitForProcess(g_ProcessName) ||
        IsActualFrameGenerationActive() || !IsWindow(gameWindow)) {
        s_firstDeferredInitEligibleMs = 0;
        return false;
    }

    if (g_State.overlayInit || ce::overlay_compat::GetStartupBlockingOverlayModuleName()) {
        return false;
    }

    const ULONGLONG now = GetTickCount64();
    if (s_firstDeferredInitEligibleMs == 0) {
        s_firstDeferredInitEligibleMs = now;
    }

    const ULONGLONG elapsedMs = now - s_firstDeferredInitEligibleMs;
    if (elapsedMs >= kStartupOverlayInitGraceMs) {
        return false;
    }

    if (remainingMs) {
        *remainingMs = kStartupOverlayInitGraceMs - elapsedMs;
    }
    return true;
}

static bool ShouldDelayOverlayInitAfterStartupResumeCompat(bool allowOverlayRender, HWND gameWindow,
                                                           ULONGLONG* remainingMs = nullptr) {
    static bool s_hadStartupSuppression = false;
    static ULONGLONG s_resumeStableSinceMs = 0;
    static HWND s_resumeWindow = nullptr;
    if (remainingMs) {
        *remainingMs = 0;
    }

    const bool processNeedsDelay =
        ce::overlay_compat::ShouldPreemptivelyDelayDX12OverlayInitForProcess(g_ProcessName);
    const bool actualFGActive = IsActualFrameGenerationActive();
    if (!processNeedsDelay || actualFGActive) {
        s_hadStartupSuppression = false;
        s_resumeStableSinceMs = 0;
        s_resumeWindow = nullptr;
        return false;
    }

    if (!allowOverlayRender) {
        s_hadStartupSuppression = true;
        s_resumeStableSinceMs = 0;
        s_resumeWindow = nullptr;
        return false;
    }

    if (!s_hadStartupSuppression || !IsWindow(gameWindow)) {
        return false;
    }

    RECT clientRect = {};
    LONG width = 0;
    LONG height = 0;
    if (GetClientRect(gameWindow, &clientRect)) {
        width = clientRect.right - clientRect.left;
        height = clientRect.bottom - clientRect.top;
    }

    const bool windowForeground = (GetForegroundWindow() == gameWindow);
    const ULONGLONG now = GetTickCount64();
    if (!windowForeground || !ce::overlay_compat::HasUsableDX12OverlayStartupWindowSize(width, height)) {
        s_resumeStableSinceMs = 0;
        s_resumeWindow = gameWindow;
        return true;
    }

    if (s_resumeWindow != gameWindow || s_resumeStableSinceMs == 0) {
        s_resumeWindow = gameWindow;
        s_resumeStableSinceMs = now;
    }

    const ULONGLONG msSinceResumeReady = now - s_resumeStableSinceMs;
    if (ce::overlay_compat::ShouldDelayDX12OverlayInitAfterStartupResume(
            processNeedsDelay, s_hadStartupSuppression, actualFGActive, windowForeground, width, height,
            msSinceResumeReady, kStartupOverlayPostResumeSettleMs)) {
        if (remainingMs) {
            *remainingMs = kStartupOverlayPostResumeSettleMs - std::min(msSinceResumeReady, kStartupOverlayPostResumeSettleMs);
        }
        return true;
    }

    s_hadStartupSuppression = false;
    s_resumeStableSinceMs = 0;
    s_resumeWindow = nullptr;
    return false;
}

static bool ApplyOverlayStartupCompatMode(HWND gameWindow) {
    const char* overlayModule = nullptr;
    ULONGLONG remainingMs = 0;
    ce::overlay_compat::AuxiliaryProcessWindowInfo activeWindow = {};
    const bool suppressOverlay =
        ShouldSuppressOverlayForStartupCompat(gameWindow, &overlayModule, &remainingMs, &activeWindow);
    const bool allowOverlay = !suppressOverlay;
    static bool s_overlayCompatSuppressed = false;
    static bool s_loggedVisibleWindowSuppression = false;
    static HWND s_loggedWindowHandle = nullptr;

    if (!allowOverlay) {
        if (activeWindow.hwnd) {
            if (!s_overlayCompatSuppressed || !s_loggedVisibleWindowSuppression ||
                s_loggedWindowHandle != activeWindow.hwnd) {
                HookLogImportant(
                    "DX12: Pausing DX12 overlay submissions while startup window from %s is visible "
                    "(hwnd=%p visible=%d class='%s' title='%s')",
                    overlayModule ? overlayModule : "module", activeWindow.hwnd,
                    activeWindow.visible ? 1 : 0,
                    activeWindow.className[0] ? activeWindow.className : "<unknown>",
                    activeWindow.title[0] ? activeWindow.title : "<untitled>");
                s_loggedVisibleWindowSuppression = true;
                s_loggedWindowHandle = activeWindow.hwnd;
            }
        } else if (!s_overlayCompatSuppressed || s_loggedVisibleWindowSuppression) {
            HookLogImportant(
                "DX12: Keeping DX12 overlay submissions paused for startup-overlay warm-up/cool-down "
                "(overlay=%s remaining=%llums)",
                overlayModule ? overlayModule : "module", remainingMs);
            s_loggedVisibleWindowSuppression = false;
            s_loggedWindowHandle = nullptr;
        }
        if (!s_overlayCompatSuppressed) {
            s_overlayCompatSuppressed = true;
        }
        return false;
    }

    if (s_overlayCompatSuppressed) {
        HookLogImportant("DX12: Resuming DX12 overlay after startup overlay windows settled");
        s_overlayCompatSuppressed = false;
        s_loggedVisibleWindowSuppression = false;
        s_loggedWindowHandle = nullptr;
    }

    return true;
}

static void DisableDedicatedOverlayQueueForOverlayCompat() {
    // When FG goes inactive, we keep the dedicated overlay queue alive to avoid
    // a destructive teardown/rebuild cycle during FG mode switches (e.g. 2x→3x).
    // Destroying and recreating queue + fence + allocators mid-transition causes
    // ERR_GFX_STATE because InitOverlaySync releases D3D12 objects while the GPU
    // still has in-flight work (deferred Signal not yet flushed).
    //
    // The queue sits idle when FG is inactive (submissions go to the game queue).
    // When FG reactivates, the queue is ready — no reinit needed.
    if (ShouldUseDedicatedOverlayQueue()) {
        return;
    }

    if (!g_State.overlayQueue) {
        return;
    }

    static bool s_loggedSuspend = false;
    if (!s_loggedSuspend) {
        const char* overlayModule = nullptr;
        ShouldUseDedicatedOverlayQueue(&overlayModule);
        if (overlayModule) {
            HookLogImportant(
                "DX12: Suspending dedicated overlay queue (FG inactive, external overlay %s) — queue kept alive",
                overlayModule);
        } else {
            HookLogImportant("DX12: Suspending dedicated overlay queue (FG inactive) — queue kept alive");
        }
        s_loggedSuspend = true;
    }
}

static void EnsureDedicatedOverlayQueueForFGCompat() {
    if (!ShouldUseDedicatedOverlayQueue()) {
        return;
    }

    if (!g_State.syncInit || g_State.overlayQueue) {
        // Queue already exists or not yet initialized — nothing to do.
        // If the queue was "suspended" (FG inactive), just resuming FG reactivates it
        // because SubmitOverlayCommandList checks ShouldUseDedicatedOverlayQueue().
        return;
    }

    // Only reach here on first FG activation (queue never created yet).
    HookLogImportant("DX12: FG became active — dedicated overlay queue not yet created, forcing sync reinit");
    g_State.syncInit = false;
    g_State.overlayInit = false;
}

// Forward Declarations
void STDMETHODCALLTYPE DetourExecuteCommandLists(ID3D12CommandQueue* pThis, UINT NumCommandLists,
                                                 ID3D12CommandList* const* ppCommandLists);
void DX12_HookQueueVTable(ID3D12CommandQueue* queue);
void DX12_HookDeviceVTable(ID3D12Device* device);

static ExecuteCommandListsPtr GetOriginalExecuteCommandLists(ID3D12CommandQueue* queue) {
    if (!queue)
        return oExecuteCommandLists;

    void** vtbl = *reinterpret_cast<void***>(queue);
    if (!vtbl)
        return oExecuteCommandLists;

    void** cachedVtable = g_LastExecuteCommandListsVTable.load(std::memory_order_acquire);
    if (cachedVtable == vtbl) {
        ExecuteCommandListsPtr cachedOriginal = g_LastExecuteCommandListsOriginal.load(std::memory_order_acquire);
        if (cachedOriginal)
            return cachedOriginal;
    }

    ExecuteCommandListsPtr original = oExecuteCommandLists;
    {
        std::lock_guard<std::recursive_mutex> lock(g_ExecuteCommandListsHookStateMutex);
        auto it = g_ExecuteCommandListsOriginalByVTable.find(vtbl);
        if (it != g_ExecuteCommandListsOriginalByVTable.end())
            original = it->second;
    }

    if (original) {
        g_LastExecuteCommandListsOriginal.store(original, std::memory_order_release);
        g_LastExecuteCommandListsVTable.store(vtbl, std::memory_order_release);
    }
    return original;
}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChain(IDXGIFactory* pThis, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
                                                IDXGISwapChain** ppSwapChain);
HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwnd(IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd,
                                                       const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                       const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFDesc, IDXGIOutput* pOut,
                                                       IDXGISwapChain1** ppSC);
void STDMETHODCALLTYPE DetourCreateSampler(ID3D12Device* pDevice, const D3D12_SAMPLER_DESC* pDesc,
                                           D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);

extern "C" BOOL WINAPI ApplyDX12SamplerOverridesCallback(D3D12_SAMPLER_DESC* pDesc);

// REQUIRED EXPORTS
void DX12_AdjustWrapperResizeDepth(int delta) {
    if (delta > 0)
        DXGIShared::g_SharedState.wrapperResizeDepth.fetch_add(delta);
    else
        DXGIShared::g_SharedState.wrapperResizeDepth.fetch_sub(-delta);
}

// Forward declaration for CleanupRTVs
void CleanupRTVs();

void DX12_InvalidateSwapchain() {
    DXGIShared::g_SharedState.swapchainInvalid.store(true, std::memory_order_release);
    HookLog("DX12: Swapchain marked INVALID (FSR/FG transition detected)");
    // Log current state for debugging
    HookLog("DX12: Invalidating - overlayInit=%d, syncInit=%d, device=%p, queue=%p", g_State.overlayInit,
            g_State.syncInit, g_Device.load(), g_CommandQueue.load());

    // Only invalidate swapchain-level state, not device-level sync resources
    // This allows swapchain changes without full reinitialization
    if (g_State.overlayInit) {
        HookLog(
            "DX12: Invalidating swapchain resources (device-level resources "
            "preserved)");
        g_State.overlayInit = false;
        CleanupRTVs();
    }
}

void DX12_SignalFSR4SwapchainRecreated() {
    DXGIShared::g_SharedState.fsr4RecreationPending.store(true, std::memory_order_release);
    HookLog("DX12: FSR4 swapchain recreation signaled");
}

// Device-removed flag: once set, skip overlay rendering AND heartbeats so the
// freeze watchdog can detect the stuck state and create a diagnostic dump.
static std::atomic<bool> g_DeviceRemoved{false};

// C Linkage Exports for cross-module calls (e.g. from C clients or
// GetProcAddress)
extern "C" {
void DX12_SetCommandQueue(ID3D12CommandQueue* pQueue) {
    if (!pQueue)
        return;

    // ExecuteCommandLists may hit this many times per frame on the same queue.
    // Once we've captured the active DIRECT queue, avoid the repeated GetDesc /
    // lock / QueryInterface work on the hot path.
    if (g_CommandQueue.load(std::memory_order_acquire) == pQueue)
        return;

    // CRITICAL FIX: Only allow DIRECT queues for overlay rendering.
    // Strange Brigade and other DX12 games use Async Compute queues.
    // Submitting overlay (Direct) commands to a Compute queue causes a device
    // lost/crash.
    D3D12_COMMAND_QUEUE_DESC desc = pQueue->GetDesc();
    if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        // HookLog("DX12: Ignoring non-direct queue (Type=%d)", desc.Type);
        return;
    }

    // Set primary game queue once — the first DIRECT queue seen is always the
    // game's queue (created before any FG runtime initializes).  Used to filter
    // ECL counting for accurate real-vs-interpolated frame classification.
    ID3D12CommandQueue* expected = nullptr;
    g_PrimaryGameQueue.compare_exchange_strong(expected, pQueue, std::memory_order_acq_rel);

    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    if (g_CommandQueue.load() != pQueue) {
        if (g_CommandQueue.load())
            g_CommandQueue.load()->Release();
        g_CommandQueue.store(pQueue);
        pQueue->AddRef();
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(pQueue->GetDevice(IID_PPV_ARGS(&dev)))) {
            if (g_Device.load() != dev) {
                if (g_Device.load())
                    g_Device.load()->Release();
                g_Device.store(dev);

                // Clear device-removed flag — a new device means recovery.
                g_DeviceRemoved.store(false, std::memory_order_release);
                DXGIShared::g_SharedState.deviceRemovedFatal.store(false, std::memory_order_release);
                g_RenderWatchdog.SetForceMonitor(false);

                // Reset primary game queue — new device means new queues.
                g_PrimaryGameQueue.store(pQueue, std::memory_order_release);

                // Report GPU LUID for host metrics (PDH counter filtering).
                // ID3D12Device has GetAdapterLuid() directly — don't use
                // IDXGIDevice (D3D12 devices don't implement it).
                LUID adapterLuid = dev->GetAdapterLuid();
                ReportLUID(adapterLuid.LowPart, adapterLuid.HighPart);
                HookLog("DX12: Reported LUID %08x-%08x", adapterLuid.HighPart, adapterLuid.LowPart);
            } else
                dev->Release();
        }
    }

    // CRITICAL FIX: Hook queue vtable lazily here instead of during swapchain
    // creation This prevents hangs during DXGI internal operations
    DX12_HookQueueVTable(pQueue);
}
// Capture the queue that was passed to CreateSwapChain* so we can prefer it
// for overlay submission.  Only accepts DIRECT queues (same rule as
// DX12_SetCommandQueue).  Also hooks the queue vtable for ECL interception.
static void DX12_SetSwapchainQueue(ID3D12CommandQueue* pQueue) {
    if (!pQueue)
        return;

    D3D12_COMMAND_QUEUE_DESC desc = pQueue->GetDesc();
    if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return;

    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    if (g_SwapchainQueue != pQueue) {
        if (g_SwapchainQueue)
            g_SwapchainQueue->Release();
        g_SwapchainQueue = pQueue;
        g_SwapchainQueue->AddRef();
        HookLogImportant("DX12: Swapchain queue captured (queue=%p)", pQueue);
    }

    // Also hook the vtable so ECL fires for this queue
    DX12_HookQueueVTable(pQueue);
}

void DX12_AdjustWrapperResizeDepth_C(int delta) {
    DX12_AdjustWrapperResizeDepth(delta);
}

// Export for D3D12 wrapper to notify command list execution (frame
// classification)
void DX12_NotifyCommandLists(UINT numCommandLists) {
    g_CommandListsExecutedThisFrame.fetch_add(numCommandLists, std::memory_order_relaxed);
}
}

void DX12_OnSwapchainResizeEnd();
void CleanupOverlay();
void CleanupRTVs();
void DX12_InvalidateSwapchain();

// Helper to ensure global hook instance exists
void EnsureDX12Hook() {
    if (!g_dx12HookInstance) {
        g_dx12HookInstance = new DX12Hook();
    }
}

// Forward declarations
static void InstallGlobalVTableHooks();
static void HookSwapchainVTableViaTempSwapchain(bool presentOnly = false);
static void FindAndWrapPreExistingSwapchains();

void DX12Hook::Init() {
    EnsureDX12Hook();  // Self-init check
    static std::recursive_mutex s_InitMutex;
    static bool s_InitDone = false;
    std::lock_guard<std::recursive_mutex> lock(s_InitMutex);
    if (s_InitDone)
        return;
    s_InitDone = true;

    // CRITICAL FIX: Check if Vulkan is active before installing ANY DXGI hooks
    // Vulkan games using WSI-to-DXGI mapping can freeze if we hook DXGI
    HMODULE hVulkan = GetModuleHandleW(L"vulkan-1.dll");
    if (hVulkan) {
        HookLog(
            "DX12: Vulkan detected (vulkan-1.dll), SKIPPING ALL DXGI hook "
            "installation");
        return;
    }

    // Note: Crash handler is installed in DllMain (hook/main.cpp)

    // Start freeze detection watchdog with dynamic timeout based on game engine
    // The watchdog auto-detects UE5, DLSS FG and uses extended timeouts
    double timeout = g_RenderWatchdog.GetRecommendedTimeout();
    g_RenderWatchdog.SetMonitoredThread(GetCurrentThreadId());
    g_RenderWatchdog.Start(timeout);
    HookLog("DX12: Freeze watchdog started (%.0f second timeout)", timeout);

    // CRITICAL FIX: Install global swapchain vtable hooks by getting the vtable
    // directly from the DXGI module. This avoids creating a temp swapchain which
    // causes deadlocks with Steam overlay + Streamline.
    InstallGlobalVTableHooks();

    // NOTE: HookSwapchainVTableViaTempSwapchain() is NOT called here.
    // It is deferred to EnsurePresentHooks(), which is called from
    // Wrapped_D3D12CreateDevice only after the game has confirmed D3D12 usage.
    // This prevents creating a temp D3D12 device in DX11-only apps (which load
    // d3d12.dll via D3D11On12), which would corrupt shared DXGI internal state
    // and crash the DX11 swap chain.

    HookLog("DX12Hook: Initialized (factory hooks installed; Present hooks deferred)");

    FindAndWrapPreExistingSwapchains();
}

void DX12Hook::EnsurePresentHooks() {
    static std::atomic<bool> s_done{false};
    bool expected = false;
    if (!s_done.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;  // Already installed
    }
    HookLog("DX12: Installing Present inline hooks (D3D12 device created by game)");
    HookSwapchainVTableViaTempSwapchain();
    HookLog("DX12: Present inline hooks installed");
}

static void FindAndWrapPreExistingSwapchains() {
    // Pre-existing swapchains (created before injection) are now handled via
    // inline hooks on Present/Present1. The inline hook approach:
    // 1. Patches the function code in memory (not the vtable)
    // 2. Creates a trampoline that executes the original instructions
    // 3. Calling the trampoline GUARANTEED bypasses the hook - no re-entry
    //
    // This works for both pre-existing swapchains AND wrapped swapchains.
    // For wrapped swapchains, DetourPresent detects the wrapper and passes
    // through. For pre-existing swapchains, DetourPresent processes the frame
    // normally.
    HookLog("DX12: Pre-existing swapchain support enabled via inline Present hooks");
}

static void EnsurePresentInlineHooksForRealSwapchain(IDXGISwapChain* pSwapChain, const char* source) {
    if (!pSwapChain || DXGIShared::HasPresentInlineHooks()) {
        return;
    }

    static std::atomic<int> s_installAttemptCount{0};
    const int attempt = s_installAttemptCount.fetch_add(1, std::memory_order_relaxed) + 1;
    HookLog("DX12: Installing Present inline hooks via %s swapchain #%d (swapchain=%p)", source ? source : "real",
            attempt, pSwapChain);

    if (!DXGIShared::InstallPresentInlineHooks(pSwapChain)) {
        HookLog("DX12: Present inline hook installation via %s swapchain failed", source ? source : "real");
        return;
    }

    if (DXGIShared::HasPresentInlineHooks()) {
        HookLogImportant("DX12: Present inline hooks are active via %s swapchain", source ? source : "real");
    } else {
        HookLog("DX12: Present inline hook installation via %s swapchain deferred to existing external hook chain",
                source ? source : "real");
    }
}

// Function pointers for global factory vtable hooks
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                        IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChainForHwnd)(IDXGIFactory2*, IUnknown*, HWND,
                                                               const DXGI_SWAP_CHAIN_DESC1*,
                                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                               IDXGISwapChain1**);

static PFN_CreateSwapChain oCreateSwapChainGlobal = nullptr;
static PFN_CreateSwapChainForHwnd oCreateSwapChainForHwndGlobal = nullptr;

// Inline hook trampoline for CreateSwapChainForHwnd (code-level hook in dxgi.dll)
// This catches ALL calls regardless of which factory vtable is used (including
// Streamline SL proxy factories that bypass our vtable hooks).
static PFN_CreateSwapChainForHwnd s_oCreateSCForHwndInline = nullptr;

// Address of the real CreateSwapChainForHwnd in dxgi.dll (for deep hook removal)
static void* s_realCreateSCForHwndAddr = nullptr;

// Deep hook trampoline for calling the real CreateSwapChainForHwnd
static PFN_CreateSwapChainForHwnd s_deepHookTrampoline = nullptr;

// Overlay suspension: cooldown after swapchain creation (FG switch, resize, etc.)
// to reduce our D3D12 footprint while the game's internal state machine stabilizes.
static std::atomic<int64_t> g_OverlayCooldownUntilQpc{0};
static constexpr int64_t kTransitionCooldownMs = 1500;  // 1.5 s

static void StartTransitionCooldown() {
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    g_OverlayCooldownUntilQpc.store(now.QuadPart + freq.QuadPart * kTransitionCooldownMs / 1000,
                                    std::memory_order_release);
    // Discard any pending deferred Signal — the queue may change during FG switch
    g_deferredSignalValue.store(0, std::memory_order_release);
    g_deferredSignalAllocIdx.store(-1, std::memory_order_release);
    g_deferredSignalQueue.store(nullptr, std::memory_order_release);
    HookLogImportant("DX12: Overlay transition cooldown started (%lldms)", (long long)kTransitionCooldownMs);
}

// HWND → swapchain tracking for diagnostics and E_ACCESSDENIED recovery.
// We do NOT AddRef tracked swapchains — this avoids extending their lifetime
// beyond what the game intends, which previously caused UE5 assertion crashes
// during FG switching (our AddRef kept the old SC alive, holding the HWND,
// and our forced destruction happened at the wrong time in UE5's lifecycle).
static std::mutex s_hwndSwapchainMutex;
static std::map<HWND, std::vector<IDXGISwapChain*>> s_hwndSwapchainMap;

// Track a swapchain's HWND association (called from ProcessFrame and deep hook).
// NO AddRef — raw pointer tracking only. Pointers may become stale when the
// game destroys the swapchain, which is fine because we only use them for
// reactive E_ACCESSDENIED recovery with SEH protection.
static void TrackSwapchainHwnd(IDXGISwapChain* pSwapChain, HWND hWnd) {
    if (!hWnd || !pSwapChain)
        return;
    std::lock_guard<std::mutex> lock(s_hwndSwapchainMutex);
    auto& vec = s_hwndSwapchainMap[hWnd];
    for (auto* sc : vec) {
        if (sc == pSwapChain)
            return;  // Already tracked
    }
    vec.push_back(pSwapChain);
}

// Forward declaration — defined below near DetourCreateSwapChainGlobal
static bool IsStreamlineLoaded();

// Deep hook wrapper for CreateSwapChainForHwnd.
// Intercepts ALL callers (including Streamline's internal trampoline calls).
// Uses REACTIVE E_ACCESSDENIED recovery: tries the call first, only intervenes
// if it fails. This avoids destroying swapchains prematurely (which caused UE5
// assertion crashes when our proactive pre-check destroyed SCs that UE5's
// deferred viewport code still referenced).
static HRESULT STDMETHODCALLTYPE DeepHookCreateSwapChainForHwnd(IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd,
                                                                const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                                const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFDesc,
                                                                IDXGIOutput* pOut, IDXGISwapChain1** ppSC) {
    if (HookIsShuttingDown()) {
        if (s_deepHookTrampoline)
            return s_deepHookTrampoline(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);
        return E_FAIL;
    }

    HookLogImportant("DeepHook: CreateSwapChainForHwnd ENTER factory=%p device=%p hwnd=%p", pThis, pDevice, hWnd);

    // Suspend overlay rendering during the swapchain transition.
    StartTransitionCooldown();

    // Try the call first — let the game/SL handle SC lifecycle naturally
    HRESULT hr = s_deepHookTrampoline(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);
    HookLogImportant("DeepHook: Trampoline returned hr=0x%08X sc=%p", hr, (ppSC ? *ppSC : nullptr));

    // Reactive recovery: if E_ACCESSDENIED, an old SC still holds the HWND.
    // DON'T force-destroy — that invalidates game-held references and causes
    // delayed UE5 assertion crashes.  Clean up our overlay refs and do a very
    // brief retry.  If it still fails, return the error so the caller
    // (Streamline/game) can manage its own state machine — long blocking here
    // causes DLSS FG activation crashes.
    if (hr == E_ACCESSDENIED && hWnd) {
        if (IsStreamlineLoaded()) {
            HookLogImportant("DeepHook: E_ACCESSDENIED for HWND=%p — Streamline present, passing through", hWnd);
        } else {
            HookLogImportant("DeepHook: E_ACCESSDENIED for HWND=%p — cleaning up overlay refs", hWnd);

            // Clean up our overlay resources so we don't hold stale back-buffer refs
            g_LastSwapChain = nullptr;
            CleanupOverlay();

            // Clear our tracking entries (raw pointers, no Release needed)
            {
                std::lock_guard<std::mutex> lock(s_hwndSwapchainMutex);
                s_hwndSwapchainMap.erase(hWnd);
            }

            // Brief retry: 5 attempts × 10ms = 50ms max (safe for FG state machines)
            for (int attempt = 1; attempt <= 5 && hr == E_ACCESSDENIED; ++attempt) {
                Sleep(10);
                hr = s_deepHookTrampoline(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);
            }
            if (SUCCEEDED(hr)) {
                HookLogImportant("DeepHook: Retry succeeded hr=0x%08X sc=%p", hr, (ppSC ? *ppSC : nullptr));
            } else {
                HookLogImportant("DeepHook: Returning E_ACCESSDENIED to caller (HWND=%p)", hWnd);
            }
        }
    }

    // Post-track: record the new swapchain for future reactive recovery
    if (SUCCEEDED(hr) && ppSC && *ppSC && hWnd) {
        TrackSwapchainHwnd(*ppSC, hWnd);
        HookLogImportant("DeepHook: Created & tracked swapchain %p for HWND=%p", *ppSC, hWnd);
    } else if (FAILED(hr)) {
        HookLogImportant("DeepHook: CreateSwapChainForHwnd FAILED hr=0x%08X hwnd=%p", hr, hWnd);
    }

    return hr;
}

// Inline hook detour for CreateSwapChainForHwnd.
// This code-level hook fires for ALL calls to the real DXGI function,
// including internal calls by Streamline's DLFG module (linkSwapchainToCmdQueue).
// When E_ACCESSDENIED occurs (HWND already has a flip-model swapchain), we
// force-release the old swapchain and retry, preventing the fatal crash during
// FSR FG → DLSS FG runtime switching.
static HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwndInline(IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd,
                                                                    const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFDesc,
                                                                    IDXGIOutput* pOut, IDXGISwapChain1** ppSC) {
    if (HookIsShuttingDown()) {
        if (s_oCreateSCForHwndInline)
            return s_oCreateSCForHwndInline(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);
        return E_FAIL;
    }

    HookLogImportant("CreateSwapChainForHwnd INLINE: factory=%p device=%p hwnd=%p", pThis, pDevice, hWnd);

    HRESULT hr = s_oCreateSCForHwndInline(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);
    HookLogImportant("CreateSwapChainForHwnd INLINE: result hr=0x%08X sc=%p", hr, (ppSC && *ppSC) ? *ppSC : nullptr);

    if (hr == E_ACCESSDENIED && hWnd) {
        // When Streamline is managing swapchain lifecycle, don't interfere.
        // Our CleanupOverlay() flushes the GPU (200ms Signal+Wait) and destroys
        // overlay resources, which disrupts Streamline's internal state machine.
        if (IsStreamlineLoaded()) {
            HookLogImportant(
                "CreateSwapChainForHwnd INLINE: E_ACCESSDENIED for HWND=%p — "
                "Streamline present, passing through without cleanup",
                hWnd);
        } else {
            HookLogImportant(
                "CreateSwapChainForHwnd INLINE: E_ACCESSDENIED for HWND=%p — "
                "cleaning up overlay refs",
                hWnd);

            // Clean up overlay and clear tracking (same as deep hook)
            g_LastSwapChain = nullptr;
            CleanupOverlay();
            {
                std::lock_guard<std::mutex> lock(s_hwndSwapchainMutex);
                s_hwndSwapchainMap.erase(hWnd);
            }

            // Brief retry: 5 attempts × 10ms = 50ms max
            for (int attempt = 1; attempt <= 5 && hr == E_ACCESSDENIED; ++attempt) {
                Sleep(10);
                hr = s_oCreateSCForHwndInline(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);
            }
            if (SUCCEEDED(hr)) {
                HookLogImportant("CreateSwapChainForHwnd INLINE: Retry succeeded hr=0x%08X", hr);
            } else {
                HookLogImportant(
                    "CreateSwapChainForHwnd INLINE: Returning E_ACCESSDENIED to caller (HWND=%p)", hWnd);
            }
        }
    }

    if (SUCCEEDED(hr) && ppSC && *ppSC) {
        TrackSwapchainHwnd(*ppSC, hWnd);
        HookLogImportant("CreateSwapChainForHwnd INLINE: Created swapchain %p for HWND=%p", *ppSC, hWnd);

        // Capture the swapchain queue so overlay submissions use the correct queue.
        // Per DXGI spec, pDevice for DX12 is an ID3D12CommandQueue.
        // QI may fail if Streamline or other middleware wraps the queue object.
        ID3D12CommandQueue* pQueue = nullptr;
        if (pDevice) {
            HRESULT qiHr = pDevice->QueryInterface(IID_PPV_ARGS(&pQueue));
            if (SUCCEEDED(qiHr) && pQueue) {
                HookLogImportant("CreateSwapChainForHwnd INLINE: QI for queue succeeded (queue=%p)", pQueue);
                DX12_SetSwapchainQueue(pQueue);
                pQueue->Release();
            } else {
                // QI failed — pDevice may be wrapped by Streamline or similar.
                // Per DXGI spec pDevice MUST be a command queue for DX12, so try
                // a direct cast as a last resort.
                HookLogImportant(
                    "CreateSwapChainForHwnd INLINE: QI for ID3D12CommandQueue failed "
                    "(hr=0x%08X) on device=%p — trying direct cast",
                    qiHr, pDevice);
                auto* pDirectQueue = static_cast<ID3D12CommandQueue*>((IUnknown*)pDevice);
                D3D12_COMMAND_QUEUE_DESC testDesc = pDirectQueue->GetDesc();
                if (testDesc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
                    HookLogImportant(
                        "CreateSwapChainForHwnd INLINE: Direct cast succeeded — "
                        "DIRECT queue at %p",
                        pDirectQueue);
                    DX12_SetSwapchainQueue(pDirectQueue);
                } else {
                    HookLogImportant(
                        "CreateSwapChainForHwnd INLINE: Direct cast queue type=%d, "
                        "not DIRECT — skipping",
                        (int)testDesc.Type);
                }
            }
        }
    }

    return hr;
}

// Check if Streamline (DLSS FG interposer) is loaded.
// When present, we MUST NOT wrap swapchains with CWrapDXGISwapChain because:
// - Streamline manages the real swapchain lifecycle internally
// - Our wrapper adds an extra COM ref layer that prevents Streamline from
//   destroying the old SC before creating the FG SC on the same HWND
// - This causes E_ACCESSDENIED when DLSS FG tries to activate
// The inline Present hooks (installed on the real DXGI function) provide the
// same interception without interfering with Streamline's lifecycle management.
static bool IsStreamlineLoaded() {
    static bool detected = false;
    if (detected)
        return true;
    if (GetModuleHandleA("sl.interposer.dll") != nullptr) {
        detected = true;
        HookLogImportant("DX12: Streamline interposer detected — skipping swapchain wrapping for FG compat");
        return true;
    }
    return false;
}

// Detour for global CreateSwapChain hook
static HRESULT STDMETHODCALLTYPE DetourCreateSwapChainGlobal(IDXGIFactory* pThis, IUnknown* pDevice,
                                                             DXGI_SWAP_CHAIN_DESC* pDesc,
                                                             IDXGISwapChain** ppSwapChain) {
    // CRITICAL: Pass through during shutdown
    if (HookIsShuttingDown()) {
        if (oCreateSwapChainGlobal)
            return oCreateSwapChainGlobal(pThis, pDevice, pDesc, ppSwapChain);
        return E_FAIL;
    }

    HookLog("DetourCreateSwapChainGlobal: CALLED (factory=%p, device=%p)", pThis, pDevice);

    // Call original first
    HRESULT hr = oCreateSwapChainGlobal(pThis, pDevice, pDesc, ppSwapChain);

    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        // Log swapchain details
        if (pDesc) {
            HookLog("DetourCreateSwapChainGlobal: Creating swapchain %ux%u", pDesc->BufferDesc.Width,
                    pDesc->BufferDesc.Height);
        }

        EnsurePresentInlineHooksForRealSwapchain(*ppSwapChain, "CreateSwapChain");

        // When Streamline is loaded, skip wrapping to avoid blocking FG swapchain
        // lifecycle management.  Inline Present hooks provide the same interception.
        if (IsStreamlineLoaded()) {
            HookLog("DetourCreateSwapChainGlobal: Streamline present, skipping wrap (sc=%p)", *ppSwapChain);
            // Don't capture queue here — global hooks fire for non-game swapchains
            // (e.g. Social Club).  The inline hook handles queue capture for the game.
            return hr;
        }

        // NOTE: We don't install global Present vtable hooks for DX12.
        // The wrapper (CWrapDXGISwapChain) handles all Present interception.
        // This avoids conflicts between vtable hooks and wrapper interception
        // that caused stack overflow crashes.

        // CRITICAL: Check if this swapchain is already wrapped
        // This prevents double-wrapping which causes infinite Present recursion
        void* pExistingWrapper = nullptr;
        if (SUCCEEDED((*ppSwapChain)->QueryInterface(IID_CWrapDXGISwapChain, &pExistingWrapper))) {
            ((IUnknown*)pExistingWrapper)->Release();
            HookLog(
                "DetourCreateSwapChainGlobal: Swapchain already wrapped, "
                "skipping double-wrap");
            return hr;
        }

        // Wrap the swapchain with CWrapDXGISwapChain
        HookLog("DetourCreateSwapChainGlobal: Wrapping swapchain %p", *ppSwapChain);
        auto* wrapper = new CWrapDXGISwapChain(*ppSwapChain, pDevice);
        *ppSwapChain = wrapper;
        HookLog("DetourCreateSwapChainGlobal: Swapchain wrapped successfully");

        // Don't capture queue here — global hooks fire for non-game swapchains
        // (e.g. Social Club internal).  The inline hook handles queue capture.
    }

    return hr;
}

// Detour for global CreateSwapChainForHwnd hook
static HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwndGlobal(IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd,
                                                                    const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFDesc,
                                                                    IDXGIOutput* pOut, IDXGISwapChain1** ppSC) {
    // CRITICAL: Pass through during shutdown
    if (HookIsShuttingDown()) {
        if (oCreateSwapChainForHwndGlobal)
            return oCreateSwapChainForHwndGlobal(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);
        return E_FAIL;
    }

    HookLogImportant(
        "DetourCreateSwapChainForHwndGlobal: CALLED (factory=%p, device=%p, "
        "hwnd=%p)",
        pThis, pDevice, hWnd);

    // Suspend overlay rendering during the swapchain transition.
    StartTransitionCooldown();

    HRESULT hr = oCreateSwapChainForHwndGlobal(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);

    if (SUCCEEDED(hr) && ppSC && *ppSC) {
        // Log swapchain details
        if (pDesc) {
            HookLog("DetourCreateSwapChainForHwndGlobal: Creating swapchain %ux%u", pDesc->Width, pDesc->Height);
        }

        EnsurePresentInlineHooksForRealSwapchain(*ppSC, "CreateSwapChainForHwnd");

        // When Streamline is loaded, skip wrapping to avoid blocking FG swapchain
        // lifecycle management.  Inline Present hooks provide the same interception.
        if (IsStreamlineLoaded()) {
            HookLog("DetourCreateSwapChainForHwndGlobal: Streamline present, skipping wrap (sc=%p)", *ppSC);
            // Don't capture queue here — inline hook handles it
            return hr;
        }

        // NOTE: We don't install global Present vtable hooks for DX12.
        // The wrapper (CWrapDXGISwapChain) handles all Present interception.
        // This avoids conflicts between vtable hooks and wrapper interception
        // that caused stack overflow crashes.

        // CRITICAL: Check if this swapchain is already wrapped
        // This prevents double-wrapping which causes infinite Present recursion
        void* pExistingWrapper = nullptr;
        if (SUCCEEDED((*ppSC)->QueryInterface(IID_CWrapDXGISwapChain, &pExistingWrapper))) {
            ((IUnknown*)pExistingWrapper)->Release();
            HookLog(
                "DetourCreateSwapChainForHwndGlobal: Swapchain already wrapped, "
                "skipping double-wrap");
            return hr;
        }

        HookLog("DetourCreateSwapChainForHwndGlobal: Wrapping swapchain %p", *ppSC);
        auto* wrapper = new CWrapDXGISwapChain(*ppSC, pDevice);
        *ppSC = (IDXGISwapChain1*)wrapper;
        HookLog("DetourCreateSwapChainForHwndGlobal: Swapchain wrapped successfully");

        // Don't capture queue here — inline hook handles queue capture for all
        // CreateSwapChainForHwnd calls, including FG runtime swapchains.
    }

    return hr;
}
// This hooks the factory vtable directly in the DXGI module
static void InstallGlobalVTableHooks() {
    HookLog("DX12: InstallGlobalVTableHooks called");

    // CRITICAL: Install global factory vtable hooks to catch swapchain creation
    // even for factories created before our IAT hooks were installed.
    // This ensures ALL swapchains get wrapped regardless of timing.

    HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
    if (!hDXGI) {
        HookLog("DX12: DXGI module not loaded, skipping factory vtable hooks");
        return;
    }

    // Get CreateDXGIFactory1 export to create a temp factory
    typedef HRESULT(WINAPI * PFN_CreateDXGIFactory1)(REFIID, void**);
    PFN_CreateDXGIFactory1 pCreateFactory = (PFN_CreateDXGIFactory1)GetProcAddress(hDXGI, "CreateDXGIFactory1");
    if (!pCreateFactory) {
        HookLog("DX12: CreateDXGIFactory1 not found");
        return;
    }

    // Create a temp factory to get its vtable
    IDXGIFactory2* pFactory = nullptr;
    HRESULT hr = pCreateFactory(IID_PPV_ARGS(&pFactory));
    if (FAILED(hr) || !pFactory) {
        HookLog("DX12: Failed to create temp factory for vtable extraction");
        return;
    }

    // Get the vtable - ALL IDXGIFactory instances share this vtable
    void** vtable = *(void***)pFactory;
    HookLog("DX12: Factory vtable at %p", vtable);

    // Save the real CreateSwapChainForHwnd address BEFORE vtable patching
    void* realCreateSCForHwndAddr = vtable[15];
    s_realCreateSCForHwndAddr = realCreateSCForHwndAddr;

    // Hook CreateSwapChain (vtable[10] for IDXGIFactory)
    // Hook CreateSwapChainForHwnd (vtable[15] for IDXGIFactory2)
    if (VTableHook::Create(&vtable[10], (LPVOID)DetourCreateSwapChainGlobal, (LPVOID*)&oCreateSwapChainGlobal)) {
        HookLog("DX12: Hooked global CreateSwapChain at vtable[10]");
    }

    if (VTableHook::Create(&vtable[15], (LPVOID)DetourCreateSwapChainForHwndGlobal,
                           (LPVOID*)&oCreateSwapChainForHwndGlobal)) {
        HookLog("DX12: Hooked global CreateSwapChainForHwnd at vtable[15]");
    }

    pFactory->Release();

    // Install inline hook on CreateSwapChainForHwnd in dxgi.dll.
    // VTable hooks only patch a single vtable and miss calls through
    // Streamline's SL proxy factory (different COM vtable). Inline hooks
    // patch the actual function code and catch ALL callers.
    if (realCreateSCForHwndAddr && !s_oCreateSCForHwndInline) {
        void* trampoline = nullptr;
        if (InlineHook::Install(realCreateSCForHwndAddr, (void*)DetourCreateSwapChainForHwndInline, &trampoline)) {
            s_oCreateSCForHwndInline = (PFN_CreateSwapChainForHwnd)trampoline;
            HookLog("DX12: Installed INLINE hook on CreateSwapChainForHwnd at %p", realCreateSCForHwndAddr);
        } else {
            HookLog("DX12: FAILED to install inline hook on CreateSwapChainForHwnd");
        }
    }

    // Install DEEP hook on CreateSwapChainForHwnd.
    // When Streamline hooks CreateSwapChainForHwnd at byte 0 and uses a saved
    // trampoline for internal calls (bypassing both our vtable and inline hooks),
    // the deep hook patches the function body past Streamline's JMP so ALL
    // callers are intercepted — including Streamline's linkSwapchainToCmdQueue.
    // The full wrapper pre-releases stale swapchains AND post-tracks new ones,
    // ensuring SL's shadow swapchains are tracked for subsequent releases.
    if (realCreateSCForHwndAddr) {
        void* trampoline = InlineHook::InstallDeepHook(realCreateSCForHwndAddr, (void*)DeepHookCreateSwapChainForHwnd);
        if (trampoline) {
            s_deepHookTrampoline = (PFN_CreateSwapChainForHwnd)trampoline;
            HookLog("DX12: Installed DEEP hook on CreateSwapChainForHwnd at %p (trampoline=%p)",
                    realCreateSCForHwndAddr, trampoline);
        } else {
            HookLog("DX12: Deep hook not needed or failed for CreateSwapChainForHwnd");
        }
    }

    HookLog("DX12: Global factory vtable hooks installed");
}

void RemoveGlobalVTableHooks() {
    // Remove deep hook first (patches function body past external JMP)
    if (s_realCreateSCForHwndAddr) {
        InlineHook::RemoveDeepHook(s_realCreateSCForHwndAddr);
        s_realCreateSCForHwndAddr = nullptr;
        s_deepHookTrampoline = nullptr;
    }

    // Remove inline CreateSwapChainForHwnd hook
    if (s_oCreateSCForHwndInline) {
        // InlineHook::RemoveAll() is called from dxgi_shared.cpp during shutdown,
        // but we also null our trampoline pointer to prevent use-after-free.
        s_oCreateSCForHwndInline = nullptr;
        HookLog("DX12: Cleared inline CreateSwapChainForHwnd hook trampoline");
    }

    // Clear tracked swapchains (no Release needed — we don't AddRef tracked SCs)
    {
        std::lock_guard<std::mutex> lock(s_hwndSwapchainMutex);
        s_hwndSwapchainMap.clear();
    }

    if (!oCreateSwapChainGlobal && !oCreateSwapChainForHwndGlobal) {
        return;
    }

    HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
    if (!hDXGI) {
        HookLog("DX12: DXGI module not loaded, skipping vtable hook removal");
        return;
    }

    typedef HRESULT(WINAPI * PFN_CreateDXGIFactory1)(REFIID, void**);
    PFN_CreateDXGIFactory1 pCreateFactory = (PFN_CreateDXGIFactory1)GetProcAddress(hDXGI, "CreateDXGIFactory1");
    if (!pCreateFactory) {
        HookLog("DX12: CreateDXGIFactory1 not found for vtable hook removal");
        return;
    }

    IDXGIFactory2* pFactory = nullptr;
    HRESULT hr = pCreateFactory(IID_PPV_ARGS(&pFactory));
    if (FAILED(hr) || !pFactory) {
        HookLog("DX12: Failed to create factory for vtable hook removal");
        return;
    }

    void** vtable = *(void***)pFactory;

    if (oCreateSwapChainGlobal) {
        VTableHook::Remove(&vtable[10], (void*)oCreateSwapChainGlobal);
        HookLog("DX12: Removed CreateSwapChain vtable hook");
        oCreateSwapChainGlobal = nullptr;
    }

    if (oCreateSwapChainForHwndGlobal) {
        VTableHook::Remove(&vtable[15], (void*)oCreateSwapChainForHwndGlobal);
        HookLog("DX12: Removed CreateSwapChainForHwnd vtable hook");
        oCreateSwapChainForHwndGlobal = nullptr;
    }

    pFactory->Release();
    HookLog("DX12: Global factory vtable hooks removed");
}

// Install Present vtable hooks for pre-existing swapchains (late injection)
// DISABLED: Global Present vtable hooks cause shutdown crashes
// Factory wrapping is now the primary mechanism for intercepting swapchains
void DX12_InstallPresentHooksForSwapchain(IDXGISwapChain* pSwapChain) {
    // DISABLED: Present vtable hooks are disabled to prevent crashes
    // Pre-existing swapchains (created before injection) won't have overlay
    (void)pSwapChain;
}

// Install inline hooks on Present/Present1 via temp swapchain creation.
// Inline hooks patch the function code in memory, creating a trampoline that
// bypasses the hook entirely. This solves the re-entry problem with vtable
// hooks. presentOnly: if true, only install Present hooks (defer ResizeBuffers
// for Strange Brigade)
static void HookSwapchainVTableViaTempSwapchain(bool presentOnly) {
    HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
    HMODULE hD3D12 = GetModuleHandleA("d3d12.dll");
    if (!hDXGI || !hD3D12)
        return;

    typedef HRESULT(WINAPI * PFN_CreateDXGIFactory1)(REFIID, void**);
    typedef HRESULT(WINAPI * PFN_D3D12CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

    PFN_CreateDXGIFactory1 pCreateFactory = (PFN_CreateDXGIFactory1)GetProcAddress(hDXGI, "CreateDXGIFactory1");
    PFN_D3D12CreateDevice pD3D12CreateDevice = (PFN_D3D12CreateDevice)GetProcAddress(hD3D12, "D3D12CreateDevice");
    if (!pCreateFactory || !pD3D12CreateDevice)
        return;

    IDXGIFactory2* pFactory = nullptr;
    if (FAILED(pCreateFactory(IID_PPV_ARGS(&pFactory))) || !pFactory)
        return;

    ID3D12Device* pDevice = nullptr;
    if (FAILED(pD3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&pDevice))) || !pDevice) {
        pFactory->Release();
        return;
    }

    // Hook CreateSampler on the device vtable
    // All D3D12 devices share the same vtable, so this hooks ALL devices
    DX12_HookDeviceVTable(pDevice);

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* pQueue = nullptr;
    if (FAILED(pDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&pQueue))) || !pQueue) {
        pDevice->Release();
        pFactory->Release();
        return;
    }

    // Create a minimal hidden window
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CE_Temp";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"CE_Temp", L"", WS_POPUP, 0, 0, 2, 2, nullptr, nullptr, wc.hInstance, nullptr);

    // Create temp swapchain
    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = 2;
    scd.Height = 2;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain1* pSwapChain = nullptr;
    HRESULT hr = E_FAIL;

    // CRITICAL: Call the ORIGINAL CreateSwapChainForHwnd to get an unwrapped
    // swapchain We must use oCreateSwapChainForHwndGlobal directly to bypass our
    // wrapper If the original is not available, skip vtable hook installation
    if (oCreateSwapChainForHwndGlobal) {
        // Call original directly - bypasses our wrapper
        hr = oCreateSwapChainForHwndGlobal(pFactory, pQueue, hwnd, &scd, nullptr, nullptr, &pSwapChain);
        if (SUCCEEDED(hr) && pSwapChain) {
            HookLog(
                "DX12: Created temp swapchain via original "
                "CreateSwapChainForHwnd (unwrapped)");
        }
    } else {
        HookLog(
            "DX12: oCreateSwapChainForHwndGlobal not available, skipping "
            "Present vtable hooks");
    }

    if (SUCCEEDED(hr) && pSwapChain) {
        HookLog("DX12: Installing Present inline hooks via temp swapchain");
        if (DXGIShared::InstallPresentInlineHooks(pSwapChain)) {
            HookLog("DX12: Present inline hooks installed successfully");
        } else {
            HookLog("DX12: Failed to install Present inline hooks");
        }
        pSwapChain->Release();
    } else {
        HookLog("DX12: Failed to create temp swapchain (hr=0x%08X)", hr);
    }

    // Hook ExecuteCommandLists on the temp queue's vtable.
    // All DX12 command queues share the same vtable, so this hooks ALL queues
    // (including the game's pre-existing queue). When ECL fires, it calls
    // DX12_SetCommandQueue which captures the game's actual queue pointer.
    DX12_HookQueueVTable(pQueue);

    // Cleanup
    if (hwnd)
        DestroyWindow(hwnd);
    UnregisterClassW(L"CE_Temp", wc.hInstance);
    pQueue->Release();
    pDevice->Release();
    pFactory->Release();
}

void ShutdownImGui() {
    if (!g_State.overlayInit)
        return;

    if (g_OverlayAdapter.IsInitialized()) {
        g_OverlayAdapter.Shutdown();
    }

    if (g_State.srvDescHeap) {
        g_State.srvDescHeap->Release();
        g_State.srvDescHeap = nullptr;
    }
    g_State.overlayInit = false;
}

bool InitImGui(ID3D12Device* device, int buffers, DXGI_FORMAT format, HWND hwnd) {
    std::lock_guard<std::recursive_mutex> lock(g_OverlayMutex);

    // OverlayAdapter re-init check
    if (g_OverlayAdapter.IsInitialized()) {
        HookLog(
            "InitImGui: OverlayAdapter already initialized, shutting down for "
            "re-init");
        g_OverlayAdapter.Shutdown();
    }

    if (g_State.overlayInit) {
        HookLog("InitImGui: Already initialized, returning early");
        return true;
    }

    HookLog(
        "InitImGui: Proceeding with initialization - buffers=%d, format=%d, "
        "hwnd=%p",
        buffers, format, hwnd);

    g_State.format = format;

    // Use OverlayAdapter instead of ImGui
    // Rendering always goes through the game queue since GPU drivers require
    // swapchain writes from the owning queue. The overlay queue handles fence
    // management independently.
    ID3D12CommandQueue* queueForBackend = nullptr;
    {
        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
        queueForBackend = g_CommandQueue.load();
    }
    g_OverlayAdapter.SetHwnd(hwnd);
    if (!g_OverlayAdapter.InitDX12(device, queueForBackend, format)) {
        HookLog("[Overlay] DX12: OverlayAdapter::InitDX12 FAILED (device=%p, queue=%p, fmt=%d)", device,
                queueForBackend, format);
        return false;
    }

    // OverlayAdapter handles its own initialization
    HookLog("[Overlay] DX12: OverlayAdapter::InitDX12 succeeded (hwnd=%p)", hwnd);

    InputManager::Get().HookWindow(hwnd);

    // We don't need SRV heap for ImGui anymore, OverlayAdapter manages its own
    // resources. But we might need it if we keep ImGui for menus? For now
    // assuming full replacement for overlay.

    g_State.overlayInit = true;

    // Reset frame delay counter on reinitialization
    extern void DX12_ResetOverlayFrameDelay();
    DX12_ResetOverlayFrameDelay();

    return true;
}

void DrawOverlay(ID3D12GraphicsCommandList* cmdList, bool isRealFrame, UINT bufferIdx,
                 D3D12_CPU_DESCRIPTOR_HANDLE* rtvOverride) {
    // CRITICAL FIX: Lock mutex to prevent concurrent access during overlay
    // shutdown/reinit
    std::lock_guard<std::recursive_mutex> lock(g_OverlayMutex);

    if (!g_State.overlayInit || !cmdList)
        return;

    static std::atomic<int> s_drawOverlayLogCount{0};
    const bool logThisDraw = s_drawOverlayLogCount.fetch_add(1, std::memory_order_relaxed) < 10;
    if (logThisDraw) {
        HookLogImportant("DX12: DrawOverlay begin (cmdList=%p, bufferIdx=%u, realFrame=%d, overlayInit=%d, syncInit=%d)",
                         cmdList, bufferIdx, isRealFrame ? 1 : 0, g_State.overlayInit ? 1 : 0, g_State.syncInit ? 1 : 0);
    }

    // CRITICAL FIX: Always set IPC client regardless of frame type.
    // RenderOverlay() guards on ipc being non-null, so if this was only set
    // on real frames, overlay would never render when isRealFrame is false.
    g_OverlayAdapter.SetIPCClient(g_IPC);

    if (isRealFrame) {
        g_OverlayAdapter.SetMetrics(DXGIShared::GetPerformanceMetrics());
        static const bool s_isVKD3D = []() {
            return GetModuleHandleA("d3d12core.dll") &&
                   (GetModuleHandleA("libvkd3d-1.dll") || GetModuleHandleA("vkd3d.dll"));
        }();
        const char* api = s_isVKD3D ? "DX12 (VKD3D)" : "DX12";
        g_OverlayAdapter.SetGraphicsAPI(api);
        // HDR state is set during overlay init (ProcessFrame) by querying the
        // display output's actual color space — not here, to avoid the false
        // positive of R10G10B10A2_UNORM being treated as HDR in SDR mode.
    }

    // Set Render Target for Custom Overlay
    // When rtvOverride is set (offscreen compositing path), use it instead of the backbuffer RTV.
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle;
    if (rtvOverride) {
        rtvHandle = *rtvOverride;
    } else {
        // CRITICAL FIX: Add null check for rtvDescHeap to prevent crash
        if (!g_State.rtvDescHeap) {
            HookLog("DrawOverlay: rtvDescHeap is null, skipping overlay");
            return;
        }
        rtvHandle = g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
        rtvHandle.ptr += bufferIdx * g_State.rtvDescriptorSize;
    }

    g_OverlayAdapter.SetDX12RenderTarget(cmdList, (void*)rtvHandle.ptr);

    // Render overlay content
    g_OverlayAdapter.RenderOverlay(g_State.cachedWidth, g_State.cachedHeight);
    if (logThisDraw) {
        HookLogImportant("DX12: DrawOverlay end (bufferIdx=%u)", bufferIdx);
    }
}

// Ensure offscreen render target exists and matches backbuffer dimensions/format.
// Used for the copy-render-copy overlay compositing path that avoids
// OMSetRenderTargets(swapchain) + SetDescriptorHeaps GPU pipeline stalls.
static bool EnsureOffscreenRT(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format) {
    if (g_State.offscreenRT && g_State.offscreenWidth == width && g_State.offscreenHeight == height &&
        g_State.offscreenFormat == format) {
        return true;
    }

    // Release old resources if dimensions/format changed
    if (g_State.offscreenRT) {
        g_State.offscreenRT->Release();
        g_State.offscreenRT = nullptr;
    }
    if (g_State.offscreenRtvHeap) {
        g_State.offscreenRtvHeap->Release();
        g_State.offscreenRtvHeap = nullptr;
    }

    // Create RTV descriptor heap for offscreen target
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = 1;
    HRESULT hr = device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_State.offscreenRtvHeap));
    if (FAILED(hr)) {
        HookLog("DX12: Failed to create offscreen RTV heap hr=0x%08X", hr);
        return false;
    }

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC resDesc = {};
    resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resDesc.Width = width;
    resDesc.Height = height;
    resDesc.DepthOrArraySize = 1;
    resDesc.MipLevels = 1;
    resDesc.Format = format;
    resDesc.SampleDesc.Count = 1;
    resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clearVal = {};
    clearVal.Format = format;

    hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
                                         D3D12_RESOURCE_STATE_COMMON, &clearVal,
                                         IID_PPV_ARGS(&g_State.offscreenRT));
    if (FAILED(hr)) {
        HookLog("DX12: Failed to create offscreen RT %ux%u fmt=%d hr=0x%08X", width, height, format, hr);
        g_State.offscreenRtvHeap->Release();
        g_State.offscreenRtvHeap = nullptr;
        return false;
    }

    device->CreateRenderTargetView(g_State.offscreenRT, nullptr,
                                   g_State.offscreenRtvHeap->GetCPUDescriptorHandleForHeapStart());

    g_State.offscreenWidth = width;
    g_State.offscreenHeight = height;
    g_State.offscreenFormat = format;

    HookLogImportant("DX12: Created offscreen RT %ux%u fmt=%d for overlay compositing", width, height, format);
    return true;
}

// ---------------------------------------------------------------------------
// D3D11On12 overlay bridge: renders overlay via D3D11 wrapping the D3D12
// backbuffer.  D3D11 doesn't use descriptor heaps, so the NVIDIA driver
// stall triggered by SetDescriptorHeaps + OMSetRenderTargets(swapchain) in
// the same D3D12 ECL is completely avoided.  The D3D11on12 layer handles
// all resource state transitions internally.
// ---------------------------------------------------------------------------

static bool InitD3D11On12(ID3D12Device* d3d12Dev, ID3D12CommandQueue* queue,
                           IDXGISwapChain* swapChain, UINT bufferCount) {
    if (g_State.d3d11on12Init)
        return true;

    HookLogImportant("DX12 D3D11On12: Initializing bridge (dev=%p queue=%p bufCnt=%u)",
                     d3d12Dev, queue, bufferCount);

    if (!d3d12Dev || !queue || !swapChain || bufferCount == 0)
        return false;

    // Dynamically load D3D11On12CreateDevice
    HMODULE d3d11Lib = GetModuleHandleA("d3d11.dll");
    if (!d3d11Lib)
        d3d11Lib = LoadLibraryA("d3d11.dll");
    if (!d3d11Lib) {
        HookLogImportant("DX12 D3D11On12: d3d11.dll not available");
        return false;
    }

    using PFN_D3D11On12 = decltype(&D3D11On12CreateDevice);
    auto pfnCreate = (PFN_D3D11On12)GetProcAddress(d3d11Lib, "D3D11On12CreateDevice");
    if (!pfnCreate) {
        HookLogImportant("DX12 D3D11On12: D3D11On12CreateDevice not found");
        return false;
    }

    // Create D3D11on12 device wrapping the game's D3D12 device + queue
    IUnknown* queues[] = {queue};
    ID3D11Device* d3d11Dev = nullptr;
    ID3D11DeviceContext* d3d11Ctx = nullptr;
    HRESULT hr = pfnCreate(d3d12Dev, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                           nullptr, 0, queues, 1, 0,
                           &d3d11Dev, &d3d11Ctx, nullptr);
    if (FAILED(hr) || !d3d11Dev) {
        HookLogImportant("DX12 D3D11On12: D3D11On12CreateDevice failed hr=0x%08X", hr);
        return false;
    }

    // Get the ID3D11On12Device interface for wrapping resources
    ID3D11On12Device* d3d11on12 = nullptr;
    hr = d3d11Dev->QueryInterface(IID_PPV_ARGS(&d3d11on12));
    if (FAILED(hr)) {
        HookLogImportant("DX12 D3D11On12: QI for ID3D11On12Device failed hr=0x%08X", hr);
        d3d11Ctx->Release();
        d3d11Dev->Release();
        return false;
    }

    HookLogImportant("DX12 D3D11On12: Bridge device created successfully");

    // Wrap each D3D12 backbuffer for D3D11 use
    IDXGISwapChain3* sc3 = nullptr;
    hr = swapChain->QueryInterface(IID_PPV_ARGS(&sc3));
    if (FAILED(hr)) {
        HookLogImportant("DX12 D3D11On12: QI for IDXGISwapChain3 failed hr=0x%08X", hr);
        d3d11on12->Release();
        d3d11Ctx->Release();
        d3d11Dev->Release();
        return false;
    }

    std::vector<ID3D11Resource*> wrappedBBs;
    std::vector<ID3D11RenderTargetView*> rtvs;
    bool wrapOk = true;

    for (UINT i = 0; i < bufferCount && wrapOk; i++) {
        ID3D12Resource* d3d12BB = nullptr;
        hr = sc3->GetBuffer(i, IID_PPV_ARGS(&d3d12BB));
        if (FAILED(hr) || !d3d12BB) {
            HookLogImportant("DX12 D3D11On12: GetBuffer(%u) failed hr=0x%08X", i, hr);
            wrapOk = false;
            break;
        }

        D3D11_RESOURCE_FLAGS flags = {};
        flags.BindFlags = D3D11_BIND_RENDER_TARGET;

        ID3D11Resource* wrapped = nullptr;
        hr = d3d11on12->CreateWrappedResource(
            d3d12BB, &flags,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PRESENT,
            IID_PPV_ARGS(&wrapped));
        d3d12BB->Release();
        if (FAILED(hr) || !wrapped) {
            HookLogImportant("DX12 D3D11On12: CreateWrappedResource(%u) failed hr=0x%08X", i, hr);
            wrapOk = false;
            break;
        }

        ID3D11RenderTargetView* rtv = nullptr;
        hr = d3d11Dev->CreateRenderTargetView(wrapped, nullptr, &rtv);
        if (FAILED(hr)) {
            HookLogImportant("DX12 D3D11On12: CreateRenderTargetView(%u) failed hr=0x%08X", i, hr);
            wrapped->Release();
            wrapOk = false;
            break;
        }

        wrappedBBs.push_back(wrapped);
        rtvs.push_back(rtv);
    }
    sc3->Release();

    if (!wrapOk) {
        for (auto* r : rtvs) if (r) r->Release();
        for (auto* w : wrappedBBs) if (w) w->Release();
        d3d11on12->Release();
        d3d11Ctx->Release();
        d3d11Dev->Release();
        return false;
    }

    // Initialize the overlay adapter with the D3D11on12 device.
    // This creates the DX11Backend (shaders, font texture, blend states).
    if (!g_D3D11On12Adapter.InitDX11(d3d11Dev, d3d11Ctx)) {
        HookLogImportant("DX12 D3D11On12: OverlayAdapter.InitDX11 failed");
        for (auto* r : rtvs) if (r) r->Release();
        for (auto* w : wrappedBBs) if (w) w->Release();
        d3d11on12->Release();
        d3d11Ctx->Release();
        d3d11Dev->Release();
        return false;
    }

    // Store everything
    g_State.d3d11on12Device = d3d11Dev;
    g_State.d3d11on12Context = d3d11Ctx;
    g_State.d3d11on12 = d3d11on12;
    g_State.d3d11WrappedBBs = std::move(wrappedBBs);
    g_State.d3d11RTVs = std::move(rtvs);
    g_State.d3d11on12Init = true;

    HookLogImportant("DX12 D3D11On12: Initialized (%u buffers wrapped)", bufferCount);
    return true;
}

static bool RenderOverlayViaD3D11On12(int bufferIdx, bool isRealFrame) {
    if (!g_State.d3d11on12Init || !g_State.d3d11on12 || !g_State.d3d11on12Context)
        return false;

    if (bufferIdx < 0 || bufferIdx >= (int)g_State.d3d11WrappedBBs.size())
        return false;

    auto* wrapped = g_State.d3d11WrappedBBs[bufferIdx];
    auto* rtv = g_State.d3d11RTVs[bufferIdx];
    if (!wrapped || !rtv)
        return false;

    static uint64_t s_d3d11on12FrameCount = 0;
    s_d3d11on12FrameCount++;
    if (s_d3d11on12FrameCount <= 5 || (s_d3d11on12FrameCount % 300) == 0) {
        HookLogImportant("DX12 D3D11On12: RenderOverlay frame #%llu (bufIdx=%d)",
                         (unsigned long long)s_d3d11on12FrameCount, bufferIdx);
    }

    // Acquire: internally transitions backbuffer to RENDER_TARGET
    g_State.d3d11on12->AcquireWrappedResources(&wrapped, 1);

    // Set render target on D3D11 context
    g_State.d3d11on12Context->OMSetRenderTargets(1, &rtv, nullptr);

    D3D11_VIEWPORT vp = {0.0f, 0.0f,
                          (float)g_State.cachedWidth, (float)g_State.cachedHeight,
                          0.0f, 1.0f};
    g_State.d3d11on12Context->RSSetViewports(1, &vp);

    // Feed data to the D3D11on12 overlay adapter
    g_D3D11On12Adapter.SetIPCClient(g_IPC);
    if (isRealFrame) {
        g_D3D11On12Adapter.SetMetrics(DXGIShared::GetPerformanceMetrics());
        static const bool s_isVKD3D = []() {
            return GetModuleHandleA("d3d12core.dll") &&
                   (GetModuleHandleA("libvkd3d-1.dll") || GetModuleHandleA("vkd3d.dll"));
        }();
        const char* api = s_isVKD3D ? "DX12 (VKD3D)" : "DX12";
        g_D3D11On12Adapter.SetGraphicsAPI(api);
    }

    // Render overlay via D3D11 backend (no descriptor heaps!)
    g_D3D11On12Adapter.RenderOverlay(g_State.cachedWidth, g_State.cachedHeight);

    // Release: internally transitions backbuffer back to PRESENT
    g_State.d3d11on12->ReleaseWrappedResources(&wrapped, 1);

    // Flush submits all D3D11 commands to the D3D12 queue
    g_State.d3d11on12Context->Flush();

    return true;
}

static void CleanupD3D11On12() {
    if (g_D3D11On12Adapter.IsInitialized()) {
        g_D3D11On12Adapter.SetShutdownMode(true);
        g_D3D11On12Adapter.Shutdown();
        // Shutdown() deleted g_DescFreeBackend via `delete backend`
        g_DescFreeBackend = nullptr;
    } else if (g_DescFreeBackend) {
        // InitCustom was never called — we still own the backend
        g_DescFreeBackend->Shutdown();
        delete g_DescFreeBackend;
        g_DescFreeBackend = nullptr;
    }
    // Device-level D3D11On12 cleanup happens in g_State.Cleanup()
}



void CreateRTVs(ID3D12Device* device, IDXGISwapChain3* swapChain, int bufferCount) {
    if (g_State.rtvDescHeap)
        return;

    HookLogImportant("CreateRTVs: ENTER (bufferCount=%d)", bufferCount);

    // DLSS FG FIX: Validate buffer count before creating RTVs
    if (bufferCount <= 0 || bufferCount > 8) {
        HookLog("CreateRTVs: Invalid buffer count %d, limiting to 3", bufferCount);
        bufferCount = 3;
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, (UINT)bufferCount,
                                              D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    HRESULT rtvHeapHr = device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_State.rtvDescHeap));
    if (FAILED(rtvHeapHr)) {
        HookLog("CreateRTVs: Failed to create RTV descriptor heap hr=0x%08X (count=%d)", rtvHeapHr, bufferCount);
        return;
    }
    g_State.bufferCount = bufferCount;
    g_State.cachedSwapChain = swapChain;
    g_State.rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    // FG-SAFE: Do NOT hold persistent references on backbuffers.
    // FSR FG monitors backbuffer reference counts and crashes if extra refs are held.
    // Create RTVs and release immediately — re-acquire per-frame in render path.
    g_State.backBuffers.clear();
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
    for (int i = 0; i < bufferCount; i++) {
        ID3D12Resource* bb = nullptr;
        if (SUCCEEDED(swapChain->GetBuffer(i, IID_PPV_ARGS(&bb))) && bb) {
            device->CreateRenderTargetView(bb, nullptr, rtvHandle);
            bb->Release();  // Release immediately - RTV descriptor remains valid
        }
        rtvHandle.ptr += g_State.rtvDescriptorSize;
    }
    HookLogImportant("CreateRTVs: Created %d RTVs (no held refs)", bufferCount);
}

void InitOverlaySync(ID3D12Device* device, int bufferCount, ID3D12CommandQueue* gameQueue) {
    HookLogImportant("InitOverlaySync: ENTER (syncInit=%d)", g_State.syncInit);

    if (g_State.syncInit) {
        HookLogImportant("InitOverlaySync: Already initialized, returning early");
        return;
    }

    // CRITICAL: Flush any pending deferred Signal and wait for all GPU work to
    // complete BEFORE releasing D3D12 objects.  Without this, allocators/fences
    // can be released while the GPU is still executing commands that reference
    // them, causing ERR_GFX_STATE (especially during FG mode switches where
    // EnsureDedicatedOverlayQueueForFGCompat triggers a reinit).
    {
        UINT64 deferredVal = g_deferredSignalValue.load(std::memory_order_acquire);
        if (deferredVal != 0 && g_State.fence) {
            // Use the queue that submitted the deferred ECL, fall back to gameQueue
            ID3D12CommandQueue* sigQueue = g_deferredSignalQueue.load(std::memory_order_acquire);
            if (!sigQueue)
                sigQueue = gameQueue;
            if (sigQueue) {
                HRESULT hr = sigQueue->Signal(g_State.fence, deferredVal);
                if (SUCCEEDED(hr)) {
                    int allocIdx = g_deferredSignalAllocIdx.load(std::memory_order_acquire);
                    g_State.currentFenceValue = deferredVal;
                    if (allocIdx >= 0 && allocIdx < (int)g_State.fenceValues.size())
                        g_State.fenceValues[allocIdx] = deferredVal;
                }
            }
            g_deferredSignalValue.store(0, std::memory_order_release);
            g_deferredSignalAllocIdx.store(-1, std::memory_order_release);
            g_deferredSignalQueue.store(nullptr, std::memory_order_release);
        }

        if (g_State.fence && g_State.currentFenceValue > 0) {
            ID3D12CommandQueue* flushQueue = g_State.overlayQueue ? g_State.overlayQueue : gameQueue;
            if (flushQueue) {
                UINT64 waitValue = g_State.currentFenceValue;
                if (g_State.fence->GetCompletedValue() < waitValue) {
                    HANDLE waitEvent = g_State.fenceEvent ? g_State.fenceEvent : CreateEvent(nullptr, FALSE, FALSE, nullptr);
                    bool createdEvent = (waitEvent != g_State.fenceEvent);
                    if (waitEvent) {
                        g_State.fence->SetEventOnCompletion(waitValue, waitEvent);
                        WaitForSingleObject(waitEvent, 200);
                        if (createdEvent)
                            CloseHandle(waitEvent);
                    }
                    HookLogImportant("InitOverlaySync: Waited for GPU completion (fenceValue=%llu, completed=%llu)",
                                     (unsigned long long)waitValue,
                                     (unsigned long long)g_State.fence->GetCompletedValue());
                }
            }
        }
    }

    // Release any previously allocated sync resources to prevent leaks when
    // syncInit was cleared by a resize or error path without calling Shutdown.
    if (g_State.fence) {
        if (g_State.fenceEvent) {
            CloseHandle(g_State.fenceEvent);
            g_State.fenceEvent = nullptr;
        }
        g_State.fence->Release();
        g_State.fence = nullptr;
    }
    if (g_State.cmdList) {
        g_State.cmdList->Release();
        g_State.cmdList = nullptr;
    }
    for (auto* a : g_State.allocators)
        if (a)
            a->Release();
    g_State.allocators.clear();
    g_State.fenceValues.clear();

    // Release previous overlay queue if any
    if (g_State.overlayQueue) {
        g_State.overlayQueue->Release();
        g_State.overlayQueue = nullptr;
    }
    // Release previous cross-queue fence and event if any
    if (g_State.crossQueueFenceEvent) {
        CloseHandle(g_State.crossQueueFenceEvent);
        g_State.crossQueueFenceEvent = nullptr;
    }
    if (g_State.crossQueueFence) {
        g_State.crossQueueFence->Release();
        g_State.crossQueueFence = nullptr;
    };

    g_State.crossQueueFenceValue = 0;

    // Dedicated overlay queue: when FG is active, overlay commands execute on
    // this queue instead of the game queue.  This avoids interfering with
    // Streamline's game queue management.  CPU-side fence waits provide
    // cross-queue synchronization (GPU-side Wait was removed due to NVIDIA
    // WaitImpl Alt+Tab hangs). Third-party overlays can also insert their own
    // queue transitions, so stay on the game queue in that compatibility mode.
    const char* overlayModule = nullptr;
    if (!ShouldUseDedicatedOverlayQueue(&overlayModule)) {
        if (overlayModule) {
            HookLogImportant(
                "InitOverlaySync: Real FG inactive while external overlay %s is present, using single-queue overlay mode",
                overlayModule);
        } else {
            HookLogImportant("InitOverlaySync: Real FG inactive, using single-queue overlay mode");
        }
    } else {
        const char* startupOverlayModule = ce::overlay_compat::GetStartupBlockingOverlayModuleName();
        const bool processNeedsStartupCompatDelay =
            ce::overlay_compat::ShouldPreemptivelyDelayDX12OverlayInitForProcess(g_ProcessName);
        if (!IsActualFrameGenerationActive() && processNeedsStartupCompatDelay && startupOverlayModule) {
            HookLogImportant("InitOverlaySync: Using dedicated overlay queue for %s startup compatibility with %s",
                             g_ProcessName, startupOverlayModule);
        } else {
            HookLog("InitOverlaySync: Creating dedicated overlay command queue");
        }
        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.NodeMask = 0;

        // Get the node mask from the game queue if available
        if (gameQueue) {
            D3D12_COMMAND_QUEUE_DESC gameQueueDesc;
            gameQueue->GetDesc(&gameQueueDesc);
            queueDesc.NodeMask = gameQueueDesc.NodeMask;
            HookLog("InitOverlaySync: Using node mask 0x%X from game queue", queueDesc.NodeMask);
        }

        if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_State.overlayQueue)))) {
            HookLog("InitOverlaySync: FAILED to create overlay queue, falling back to single-queue");
            // Continue without overlay queue - will fall back to game queue
        } else {
            HookLogImportant("InitOverlaySync: Dedicated overlay queue created (ptr=%p, gameQueue=%p)",
                             g_State.overlayQueue, gameQueue);
        }
    }

    // Cross-queue fence: game queue signals to mark work completion before
    // overlay queue starts.  CPU-side WaitForSingleObject is used instead of
    // GPU-side CommandQueue::Wait to avoid NVIDIA WaitImpl Alt+Tab hangs.
    if (g_State.overlayQueue &&
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_State.crossQueueFence)))) {
        HookLog("InitOverlaySync: FAILED to create cross-queue fence");
    }
    g_State.crossQueueFenceValue = 0;
    if (g_State.crossQueueFence) {
        g_State.crossQueueFenceEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (!g_State.crossQueueFenceEvent) {
            HookLog("InitOverlaySync: FAILED to create cross-queue fence event");
            g_State.crossQueueFence->Release();
            g_State.crossQueueFence = nullptr;
        }
    }

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_State.fence))))
        return;

    size_t allocatorPoolSize = DX12OverlayState::ALLOC_POOL_SIZE;
    const bool startupOverlayPresent = (overlayModule != nullptr);
    const bool processNeedsStartupCompatDelay =
        ce::overlay_compat::ShouldPreemptivelyDelayDX12OverlayInitForProcess(g_ProcessName);
    allocatorPoolSize = ce::overlay_compat::GetStartupCompatibleDX12AllocatorPoolSize(
        processNeedsStartupCompatDelay, startupOverlayPresent, IsActualFrameGenerationActive(),
        DX12OverlayState::ALLOC_POOL_SIZE);
    if (allocatorPoolSize < DX12OverlayState::ALLOC_POOL_SIZE) {
        HookLogImportant("InitOverlaySync: Using minimal %u-slot allocator pool for startup overlay %s",
                         static_cast<unsigned>(allocatorPoolSize), overlayModule ? overlayModule : "module");
    }

    g_State.allocators.resize(allocatorPoolSize);
    g_State.fenceValues.resize(allocatorPoolSize);

    // CRITICAL FIX: Reset all fence values to 0 for fresh start
    // After resize/reinit, old fence values could be stale and cause infinite
    // waits
    std::fill(g_State.fenceValues.begin(), g_State.fenceValues.end(), 0);
    g_State.currentFenceValue = 0;
    g_State.allocIndex = 0;

    HookLog("InitOverlaySync: Fence values reset to 0, currentFenceValue=0");

    bool success = true;
    for (size_t i = 0; i < allocatorPoolSize; i++) {
        if (FAILED(
                device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_State.allocators[i])))) {
            success = false;
            break;
        }
    }

    if (success) {
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_State.allocators[0], nullptr,
                                             IID_PPV_ARGS(&g_State.cmdList)))) {
            success = false;
        }
    }

    if (success) {
        g_State.cmdList->Close();
        g_State.fenceEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

        g_State.syncInit = true;
    } else {
        // Cleanup partial initialization
        for (auto* alloc : g_State.allocators)
            if (alloc)
                alloc->Release();
        g_State.allocators.clear();
        g_State.fenceValues.clear();
        if (g_State.cmdList) {
            g_State.cmdList->Release();
            g_State.cmdList = nullptr;
        }
        if (g_State.fence) {
            g_State.fence->Release();
            g_State.fence = nullptr;
        }
    }
}

static bool DrainCommandQueue(ID3D12CommandQueue* queue, ID3D12Device* device) {
    if (!queue || !device)
        return false;

    // NON-BLOCKING DRAIN: Use a flush approach instead of waiting
    // to avoid deadlocking when called from the submit thread.
    ID3D12Fence* fence = nullptr;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        return false;

    // Signal the fence but don't wait - if we're on the submit thread,
    // waiting would deadlock. The fence will be processed when the
    // game next submits work.
    queue->Signal(fence, 1);

    // Quick check if already completed (GPU was idle)
    if (fence->GetCompletedValue() >= 1) {
        fence->Release();
        return true;
    }

    // Optional: very short wait for already-in-flight work (1ms)
    // This helps if the GPU is just finishing up, without blocking
    // the submit thread for long.
    HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (event) {
        if (fence->SetEventOnCompletion(1, event) == S_OK) {
            WaitForSingleObject(event, 1);  // 1ms non-blocking wait
        }
        CloseHandle(event);
    }
    fence->Release();
    return true;
}

void CleanupOverlay() {
    if (!g_State.syncInit)
        return;

    // Dedicated overlay queue: flush overlay queue instead of game queue
    ID3D12CommandQueue* queueToFlush = g_State.overlayQueue;
    if (!queueToFlush) {
        // Fallback to game queue if no dedicated overlay queue
        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
        queueToFlush = g_CommandQueue;
    }

    if (g_State.fence && queueToFlush) {
        UINT64 waitValue = g_State.currentFenceValue + 1;
        if (SUCCEEDED(queueToFlush->Signal(g_State.fence, waitValue))) {
            if (g_State.fence->GetCompletedValue() < waitValue) {
                g_State.fence->SetEventOnCompletion(waitValue, g_State.fenceEvent);
                WaitForSingleObject(g_State.fenceEvent, 200);
            }
        }
    }
    if (g_State.fenceEvent) {
        CloseHandle(g_State.fenceEvent);
        g_State.fenceEvent = NULL;
    }
    for (auto alloc : g_State.allocators)
        if (alloc)
            alloc->Release();
    g_State.allocators.clear();
    g_State.fenceValues.clear();
    if (g_State.cmdList) {
        g_State.cmdList->Release();
        g_State.cmdList = nullptr;
    }
    if (g_State.fence) {
        g_State.fence->Release();
        g_State.fence = nullptr;
    }
    // Release dedicated overlay queue
    if (g_State.overlayQueue) {
        g_State.overlayQueue->Release();
        g_State.overlayQueue = nullptr;
    }
    // Release cross-queue synchronization fence
    if (g_State.crossQueueFence) {
        g_State.crossQueueFence->Release();
        g_State.crossQueueFence = nullptr;
    }
    g_State.currentFenceValue = 0;
    g_State.crossQueueFenceValue = 0;
    g_State.allocIndex = 0;
    g_State.syncInit = false;
    g_State.cachedSC3 = nullptr;
    // Discard any pending deferred Signal — fence is being released
    g_deferredSignalValue.store(0, std::memory_order_release);
    g_deferredSignalAllocIdx.store(-1, std::memory_order_release);
    g_deferredSignalQueue.store(nullptr, std::memory_order_release);
    ResetStartupOverlayBackendActivationStage();

    // Clean up piggyback state
    g_PiggybackOverlayActive.store(false, std::memory_order_relaxed);
}

void CleanupRTVs() {
    // FG-SAFE: backBuffers no longer holds references (released at create time)
    g_State.backBuffers.clear();
    if (g_DummyBackBuffer) {
        g_DummyBackBuffer->Release();
        g_DummyBackBuffer = nullptr;
    }
    // Clean up D3D11On12 overlay bridge (holds wrapped backbuffer references)
    CleanupD3D11On12();
    // Release offscreen compositing resources
    if (g_State.offscreenRT) {
        g_State.offscreenRT->Release();
        g_State.offscreenRT = nullptr;
    }
    if (g_State.offscreenRtvHeap) {
        g_State.offscreenRtvHeap->Release();
        g_State.offscreenRtvHeap = nullptr;
    }
    g_State.offscreenWidth = 0;
    g_State.offscreenHeight = 0;
    g_State.offscreenFormat = DXGI_FORMAT_UNKNOWN;
    if (g_State.rtvDescHeap) {
        g_State.rtvDescHeap->Release();
        g_State.rtvDescHeap = nullptr;
    }
    if (g_State.srvDescHeap) {
        g_State.srvDescHeap->Release();
        g_State.srvDescHeap = nullptr;
    }
    g_State.bufferCount = 0;
    g_State.cachedSwapChain = nullptr;
    g_State.cachedSC3 = nullptr;
}

void DX12_OnSwapchainResizeBegin() {
    bool wasAlreadySet = g_InSwapchainResizeCleanup.exchange(true);
    HookLog("DX12: DX12_OnSwapchainResizeBegin called, wasAlreadySet=%d", wasAlreadySet);

    // Prevent recursion - if already in resize, return immediately
    if (wasAlreadySet) {
        HookLog(
            "DX12: DX12_OnSwapchainResizeBegin - already in resize, returning "
            "early");
        return;
    }

    DXGIShared::g_SharedState.lastSwapchainCreation = std::chrono::steady_clock::now();

    std::lock_guard<std::recursive_mutex> lock(g_OverlayMutex);

    // CRITICAL: Flush GPU before releasing resources.  In-flight overlay
    // commands still reference backbuffers; ResizeBuffers returns
    // E_ACCESSDENIED if any GPU references remain.
    CleanupOverlay();  // waits on fence, releases sync resources
    CleanupRTVs();
    g_State.overlayInit = false;

    // g_LastSwapChain is stored as a raw (non-AddRef'd) pointer to avoid
    // interfering with FSR FG's reference count management.  Do NOT Release it.
    g_PendingSwapChainCleanup = nullptr;
    g_LastSwapChain = nullptr;
    HookLog("DX12: DX12_OnSwapchainResizeBegin - complete (GPU flushed)");
}

void DX12_OnSwapchainResizeEnd() {
    HookLog("DX12: DX12_OnSwapchainResizeEnd called");
    // Only clear if it was set - prevents unbalanced calls from clearing
    // prematurely
    if (g_InSwapchainResizeCleanup.load(std::memory_order_acquire)) {
        g_InSwapchainResizeCleanup.store(false, std::memory_order_release);
    }
    // g_PendingSwapChainCleanup is no longer used (swapchain stored without
    // AddRef), so nothing to release here.
    if (g_PendingSwapChainCleanup) {
        g_PendingSwapChainCleanup = nullptr;
    }
}

// --- CPU Prerender Limit Support (DX12) ---
static void ApplyPrerenderLimitDX12(float limit) {
    if (limit < 0.0f)
        return;
    // CRITICAL FIX: Use thread-safe accessor to prevent race conditions
    DX12Context ctx = GetDX12Context();
    if (!ctx.IsValid())
        return;

    std::lock_guard<std::mutex> lock(g_PrerenderMutex);

    // Initialize fence ring buffer if needed
    if (g_PrerenderFences.empty()) {
        for (int i = 0; i < 16; i++) {
            ID3D12Fence* fence = nullptr;
            HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            if (SUCCEEDED(ctx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
                g_PrerenderFences.push_back(fence);
                g_PrerenderEvents.push_back(event);
            } else if (event) {
                CloseHandle(event);
            }
        }
        HookLog("DX12: Created prerender limit fence ring buffer (size: %d)", (int)g_PrerenderFences.size());
    }

    if (g_PrerenderFences.empty())
        return;

    constexpr DWORD kPrerenderWaitTimeoutMs = 8;
    static std::atomic<int> s_prerenderWarnLogs{0};
    auto waitFenceBounded = [&](ID3D12Fence* waitFence, HANDLE waitEvent, uint64_t waitValue) -> bool {
        if (!waitFence || !waitEvent)
            return false;
        if (waitFence->GetCompletedValue() >= waitValue)
            return true;

        HRESULT setHr = waitFence->SetEventOnCompletion(waitValue, waitEvent);
        if (FAILED(setHr)) {
            if (s_prerenderWarnLogs.fetch_add(1, std::memory_order_relaxed) < 20) {
                HookLog("DX12: Prerender SetEventOnCompletion failed hr=0x%08X value=%llu", setHr, waitValue);
            }
            return false;
        }

        DWORD waitResult = WaitForSingleObject(waitEvent, kPrerenderWaitTimeoutMs);
        if (waitResult == WAIT_OBJECT_0)
            return true;

        if (s_prerenderWarnLogs.fetch_add(1, std::memory_order_relaxed) < 20) {
            if (waitResult == WAIT_TIMEOUT) {
                HookLog("DX12: Prerender wait timed out (%lums) value=%llu", kPrerenderWaitTimeoutMs, waitValue);
            } else {
                HookLog("DX12: Prerender wait failed result=%lu value=%llu", waitResult, waitValue);
            }
        }
        return false;
    };

    size_t idx = g_PrerenderFrameIndex % g_PrerenderFences.size();
    ID3D12Fence* fence = g_PrerenderFences[idx];
    HANDLE event = g_PrerenderEvents[idx];

    if (limit == 0.0f) {
        // Strict Serial: Signal and immediately wait
        uint64_t value = g_PrerenderFrameIndex + 1;
        HRESULT signalHr = ctx.queue->Signal(fence, value);
        if (SUCCEEDED(signalHr)) {
            waitFenceBounded(fence, event, value);
        } else if (s_prerenderWarnLogs.fetch_add(1, std::memory_order_relaxed) < 20) {
            HookLog("DX12: Prerender signal failed hr=0x%08X value=%llu", signalHr, value);
        }
    } else {
        // Buffered Limit: For fractional limits (e.g., 0.5), we use Buffered 1
        // (Lookback 1) combined with an idle gap to approximate sub-frame latency.
        bool isFractional = (limit > 0.01f && limit < 1.0f);
        int effectiveLimit = isFractional ? 1 : (int)limit;
        int lookback = effectiveLimit;

        // Signal current frame
        uint64_t signalValue = g_PrerenderFrameIndex + 1;
        HRESULT signalHr = ctx.queue->Signal(fence, signalValue);
        if (FAILED(signalHr)) {
            if (s_prerenderWarnLogs.fetch_add(1, std::memory_order_relaxed) < 20) {
                HookLog("DX12: Prerender signal failed hr=0x%08X value=%llu", signalHr, signalValue);
            }
            g_PrerenderFrameIndex++;
            return;
        }

        // Wait on N frames ago
        if (g_PrerenderFrameIndex >= (uint64_t)lookback) {
            size_t waitIdx = (g_PrerenderFrameIndex - lookback) % g_PrerenderFences.size();
            ID3D12Fence* waitFence = g_PrerenderFences[waitIdx];
            HANDLE waitEvent = g_PrerenderEvents[waitIdx];
            uint64_t waitValue = (g_PrerenderFrameIndex - lookback) + 1;

            if (waitFence->GetCompletedValue() < waitValue) {
                waitFenceBounded(waitFence, waitEvent, waitValue);
            }
        }
    }

    g_PrerenderFrameIndex++;
}

void ProcessFrame(IDXGISwapChain* pSwapChain, bool processCapture) {
    // Re-entrancy guard: NVIDIA driver can pump window messages during
    // ExecuteCommandLists (via WaitImpl → DefWindowProc), which can re-enter
    // our overlay code.  Detect and skip the re-entrant call.
    static thread_local bool s_inProcessFrame = false;
    if (s_inProcessFrame) {
        return;
    }
    s_inProcessFrame = true;
    auto reentryGuard = ce::make_scope_guard([&]() { s_inProcessFrame = false; });

    static bool s_firstFrame = true;
    if (s_firstFrame) {
        s_firstFrame = false;
        HookLog("DX12: ProcessFrame FIRST CALL (swapchain=%p)", (void*)pSwapChain);
    }

    // Performance metrics for this frame
    FrameMetrics perfMetrics;
    perfMetrics.qpcUs = PerfLogger::GetQpcUs();
    strncpy(perfMetrics.api, "DX12", sizeof(perfMetrics.api) - 1);
    perfMetrics.api[sizeof(perfMetrics.api) - 1] = '\0';
    static uint64_t s_perfFrameNum = 0;
    perfMetrics.frameNum = ++s_perfFrameNum;
    PresentDebugSample* activeDebugSample = PerfLogger::Get().GetActiveDebugSample();
    const int64_t processFrameStartUs = activeDebugSample ? PerfLogger::GetQpcUs() : 0;

    // Scope guard to log metrics on any exit path
    auto perfGuard = ce::make_scope_guard([&]() {
        if (activeDebugSample) {
            activeDebugSample->processFrameUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - processFrameStartUs);
            activeDebugSample->captureUs = perfMetrics.captureUs;
        }
        if (PerfLogger::Get().IsEnabled()) {
            perfMetrics.totalUs = static_cast<int32_t>((PerfLogger::GetQpcUs() - perfMetrics.qpcUs));
            perfMetrics.fpsLimitWaitUs = static_cast<int32_t>(g_SharedFpsLimiter.GetLastWaitUs());
            PerfLogger::Get().LogFrame(perfMetrics);
        }
    });

    // Skip performance logging if disabled
    if (!PerfLogger::Get().IsEnabled()) {
        perfGuard.dismiss();
    }

    // CRITICAL: Skip all rendering during shutdown to prevent crashes
    if (HookIsShuttingDown()) {
        return;
    }

    // Skip everything when device is removed — avoids reinit spam on a dead
    // device.  DX12_SetCommandQueue clears g_DeviceRemoved when a new device
    // arrives.
    if (g_DeviceRemoved.load(std::memory_order_relaxed)) {
        return;
    }

    // Actively detect device removal every frame — covers cases where the
    // device gets removed during a suspension/cooldown period and
    // g_DeviceRemoved hasn't been set yet (the render-path check only runs
    // when overlayInit is true).
    {
        ID3D12Device* devCheck = g_Device.load();
        if (devCheck && FAILED(devCheck->GetDeviceRemovedReason())) {
            g_DeviceRemoved.store(true, std::memory_order_release);
            DXGIShared::g_SharedState.deviceRemovedFatal.store(true, std::memory_order_release);
            g_RenderWatchdog.SetForceMonitor(true);
            HookLogImportant("DX12: GPU device removed (0x%08X) — stopping overlay",
                             (unsigned)devCheck->GetDeviceRemovedReason());
            g_State.overlayInit = false;
            CleanupRTVs();
            return;
        }
    }

    bool inResize = g_InSwapchainResizeCleanup.load(std::memory_order_acquire);
    if (!pSwapChain || inResize) {
        HookLog("DX12: ProcessFrame - early return (null=%d, inResize=%d)", !pSwapChain, inResize);
        return;
    }

    DXGI_SWAP_CHAIN_DESC frameDesc = {};
    if (FAILED(pSwapChain->GetDesc(&frameDesc))) {
        HookLog("DX12: ProcessFrame - failed to get swapchain desc (precheck)");
        return;
    }

    // Track HWND → swapchain mapping for FG-switch recovery
    if (frameDesc.OutputWindow) {
        TrackSwapchainHwnd(pSwapChain, frameDesc.OutputWindow);
    }

    const bool zeroSizedSwapchain = (frameDesc.BufferDesc.Width == 0 || frameDesc.BufferDesc.Height == 0);
    const bool iconicWindow = frameDesc.OutputWindow && IsIconic(frameDesc.OutputWindow);

    // Transition cooldown: after CreateSwapChainForHwnd, pause overlay D3D12
    // work so we don't interfere with the game's internal state machine (FG
    // switch, Anti-Lag2 teardown, etc.).
    bool inTransitionCooldown = false;
    {
        int64_t cooldownEnd = g_OverlayCooldownUntilQpc.load(std::memory_order_acquire);
        if (cooldownEnd > 0) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            if (now.QuadPart < cooldownEnd) {
                inTransitionCooldown = true;
            } else {
                g_OverlayCooldownUntilQpc.store(0, std::memory_order_release);
            }
        }
    }

    // Focus-change cooldown: during DXGI focus transitions the compositor
    // reconfigures the flip chain.  Our resource barriers can conflict with
    // that, causing TDR (DEVICE_HUNG).  Pause overlay briefly when the GAME
    // window itself transitions focus — NOT on arbitrary foreground changes
    // (which would trigger on tooltips, notifications, etc.).
    bool inFocusCooldown = false;
    if (frameDesc.OutputWindow) {
        static bool s_gameWasForeground = true;
        static int64_t s_focusCooldownEnd = 0;
        static constexpr int64_t kFocusCooldownMs = 500;

        HWND fg = GetForegroundWindow();
        bool gameIsForeground = (fg == frameDesc.OutputWindow);
        if (gameIsForeground != s_gameWasForeground) {
            s_gameWasForeground = gameIsForeground;
            LARGE_INTEGER freq, now;
            QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&now);
            s_focusCooldownEnd = now.QuadPart + freq.QuadPart * kFocusCooldownMs / 1000;
            HookLog("DX12: Focus cooldown started (gameForeground=%d)", gameIsForeground ? 1 : 0);
        }
        if (s_focusCooldownEnd > 0) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            if (now.QuadPart < s_focusCooldownEnd) {
                inFocusCooldown = true;
            } else {
                s_focusCooldownEnd = 0;
            }
        }
    }

    // Heavy suspension: zero-size, iconic, or transition cooldown requires
    // full resource teardown because the swapchain is in an invalid state.
    const bool suspendOverlayHeavy = zeroSizedSwapchain || iconicWindow || inTransitionCooldown;
    // Light suspension: focus cooldown just skips rendering — resources stay
    // alive to avoid expensive reinit cycles that stress the GPU driver.
    const bool suspendOverlayRender = suspendOverlayHeavy || inFocusCooldown;

    static bool s_swapchainSuspended = false;
    if (suspendOverlayHeavy) {
        if (!s_swapchainSuspended) {
            s_swapchainSuspended = true;
            g_State.overlayInit = false;
            CleanupRTVs();
            HookLog("DX12: ProcessFrame - suspending overlay (w=%u h=%u iconic=%d cooldown=%d)",
                    frameDesc.BufferDesc.Width, frameDesc.BufferDesc.Height, iconicWindow ? 1 : 0,
                    inTransitionCooldown ? 1 : 0);
        }
    } else if (s_swapchainSuspended) {
        s_swapchainSuspended = false;
        HookLog("DX12: ProcessFrame - resuming overlay after drawable swapchain restored");
    }

    // CPU Prerender Limit - Apply before any rendering
    float prerenderLimit = GetActivePrerenderLimit();
    if (prerenderLimit >= 0.0f) {
        int64_t prerenderStartUs = PerfLogger::GetQpcUs();
        ApplyPrerenderLimitDX12(prerenderLimit);
        perfMetrics.prerenderWaitUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - prerenderStartUs);
    }

    // PERFORMANCE FIX: Use try_lock instead of blocking lock_guard
    // This prevents stalling the render thread if another thread holds the lock
    if (!g_OverlayMutex.try_lock()) {
        // Another thread is processing, skip this frame
        if (activeDebugSample) {
            activeDebugSample->flags |= kPresentSampleFlagMutexBusy;
        }
        HookLog("DX12: ProcessFrame - mutex busy, skipping frame");
        return;
    }
    // RAII unlock when we exit
    std::lock_guard<std::recursive_mutex> lock(g_OverlayMutex, std::adopt_lock);

    // SAFETY: Check device state after acquiring lock
    if (g_InSwapchainResizeCleanup.load(std::memory_order_acquire)) {
        HookLog("DX12: ProcessFrame - in resize cleanup, returning");
        return;
    }

    bool allowOverlayRender = ApplyOverlayStartupCompatMode(frameDesc.OutputWindow);

    ULONGLONG postResumeSettleRemainingMs = 0;
    const bool deferOverlayInitAfterResume =
        ShouldDelayOverlayInitAfterStartupResumeCompat(allowOverlayRender, frameDesc.OutputWindow,
                                                       &postResumeSettleRemainingMs);
    const bool processNeedsStartupOverlayInitDelay =
        ce::overlay_compat::ShouldPreemptivelyDelayDX12OverlayInitForProcess(g_ProcessName) &&
        !IsActualFrameGenerationActive();
    if (processNeedsStartupOverlayInitDelay) {
        if ((!allowOverlayRender || deferOverlayInitAfterResume) &&
            s_startupOverlayActivationStage == StartupOverlayActivationStage::kNone) {
            s_startupOverlayActivationStage = StartupOverlayActivationStage::kDelayRTVInitAfterBackendInit;
        }
    } else {
        ResetStartupOverlayBackendActivationStage();
    }
    DisableDedicatedOverlayQueueForOverlayCompat();
    EnsureDedicatedOverlayQueueForFGCompat();

    // Dedicated overlay queue architecture: when FG is active, overlay commands
    // execute on the overlay queue with CPU-side fence sync to avoid interfering
    // with Streamline.  When FG is not active, commands go on the game queue.

    // FG-SAFE device resolution: avoid COM calls through swapchain when FG is
    // active.  FSR FG wraps the swapchain, and QueryInterface/GetDevice through
    // the wrapper can corrupt FG state (causing a delayed null-deref crash in
    // game code ~2 seconds later when FG activates).
    //
    // Instead, we rely on device/queue already captured by our hook infrastructure:
    //   - g_Device is set from CreateSwapChainForHwnd detour or ECL detour
    //   - g_CommandQueue / g_SwapchainQueue are set from ECL/CreateSwapChain hooks
    //
    // The only thing we need from ProcessFrame is swapchain change tracking.
    // FG-SAFE swapchain change tracking: track pointer value only, no AddRef/Release.
    // FSR FG wraps the swapchain and monitors reference counts. Holding an extra
    // reference prevents FSR FG from properly transitioning, causing a delayed
    // null-deref crash in game code ~2s later when FG activates.
    if (pSwapChain != g_LastSwapChain) {
        if (g_LastSwapChain) {
            CleanupRTVs();
            g_SharedCaptureD3D12.Reset();
            g_State.overlayInit = false;
            ResetStartupOverlayBackendActivationStage();
        }
        // Store raw pointer for change detection only - no AddRef to avoid
        // interfering with FSR FG's swapchain lifecycle management
        g_LastSwapChain = pSwapChain;

        if (!g_Device.load()) {
            return;
        }
        HookLog("DX12: ProcessFrame - new swapchain tracked (device=%p)", g_Device.load());
    }
    // Prefer the swapchain queue(captured at creation time) so that our
    // RENDER_TARGET -> PRESENT barrier executes on the queue DXGI syncs with.
    // Fall back to the last observed direct queue if it was not captured yet.
    ID3D12CommandQueue* gameQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
        // Prefer g_SwapchainQueue (captured from CreateSwapChainForHwnd) — this is
        // the queue associated with the current swapchain's backbuffers.  When FG
        // runtimes (FSR FG, DLSS FG) create new swapchains with their own queue,
        // g_CommandQueue may still point to the game's original queue, which differs
        // from the swapchain queue.  Submitting overlay ECL on a different queue
        // than the swapchain's causes D3D12 resource state conflicts (ERR_GFX_STATE).
        // Fall back to g_CommandQueue (set by ECL hook — always fresh) when the
        // swapchain queue hasn't been captured yet.
        gameQueue = g_SwapchainQueue;
        if (!gameQueue)
            gameQueue = g_CommandQueue.load();
    }
    if (!gameQueue) {
        HookLog("DX12: ProcessFrame - no game queue, skipping overlay");
        return;
    }

    // Queue-change-based FG detection: FSR FG creates its own command queue
    // and reroutes all ECL calls through it.  Detecting a queue pointer change
    // after the first few frames is a strong signal that FG has activated.
    {
        static ID3D12CommandQueue* s_initialQueue = nullptr;
        static ID3D12CommandQueue* s_currentFGQueue = nullptr;
        static int s_queueFrameCount = 0;
        ++s_queueFrameCount;
        if (s_queueFrameCount <= 5) {
            // Capture initial queue during first 5 frames (before FG activates)
            s_initialQueue = gameQueue;
        } else if (s_initialQueue) {
            bool isFGQueue = (gameQueue != s_initialQueue);
            if (isFGQueue && !s_currentFGQueue) {
                if (UpdateHeuristicFSRFGState(true, "queue-change")) {
                    // Queue changed away from initial → FSR FG activated
                    s_currentFGQueue = gameQueue;
                    HookLogImportant("DX12: FG detected via queue change (initial=%p, current=%p, frame=%d)",
                                     s_initialQueue, gameQueue, s_queueFrameCount);
                } else {
                    static std::atomic<int> s_ignoredQueueChangeLogCount{0};
                    if (s_ignoredQueueChangeLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
                        HookLog("DX12: Ignoring queue change heuristic (initial=%p, current=%p, frame=%d)",
                                s_initialQueue, gameQueue, s_queueFrameCount);
                    }
                }
            } else if (!isFGQueue && s_currentFGQueue) {
                // Queue reverted to initial → FSR FG deactivated (FG type switch)
                HookLogImportant("DX12: FG deactivated via queue revert (initial=%p, fgQueue=%p, frame=%d)",
                                 s_initialQueue, s_currentFGQueue, s_queueFrameCount);
                s_currentFGQueue = nullptr;
                UpdateHeuristicFSRFGState(false, "queue-change");
            }
        }
    }

    // Conditional block: when a startup-blocking overlay (Social Club, EOS) is present
    // and FG is inactive, only allow overlay rendering if we have the correct swapchain
    // queue captured.  GTA5 Enhanced rejects ECL submissions on any queue other than the
    // one the swapchain was created with.  Without g_SwapchainQueue we'd fall back to
    // g_CommandQueue (from ECL hooks) which is often a different queue → ERR_GFX_STATE.
    const bool startupOverlayPresent =
        ce::overlay_compat::ShouldPreemptivelyDelayDX12OverlayInitForProcess(g_ProcessName) &&
        ce::overlay_compat::GetStartupBlockingOverlayModuleName() != nullptr &&
        !IsActualFrameGenerationActive();
    if (startupOverlayPresent) {
        bool hasSwapchainQueue;
        {
            std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
            hasSwapchainQueue = (g_SwapchainQueue != nullptr);
        }
        if (!hasSwapchainQueue) {
            allowOverlayRender = false;
            g_PiggybackOverlayActive.store(false, std::memory_order_relaxed);
            static std::atomic<int> s_noQueueBlockLogCount{0};
            if (s_noQueueBlockLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
                HookLogImportant(
                    "DX12: Overlay blocked for %s - startup-blocking overlay present, "
                    "FG inactive, swapchain queue not captured (would use wrong queue)",
                    g_ProcessName);
            }
        } else {
            // Swapchain queue captured: allow normal overlay rendering.
            // The rendering block will use barrier-free mode when
            // startupOverlayPresent is true.
            static std::atomic<int> s_queueOkLogCount{0};
            if (s_queueOkLogCount.fetch_add(1, std::memory_order_relaxed) < 3) {
                HookLogImportant(
                    "DX12: Overlay allowed for %s - startup-blocking overlay present "
                    "but swapchain queue captured, using barrier-free mode",
                    g_ProcessName);
            }
        }
    } else {
        g_PiggybackOverlayActive.store(false, std::memory_order_relaxed);
    }

    // Remove delay - install overlay immediately(Strange Brigade compatibility)
    if (allowOverlayRender && !suspendOverlayRender && !g_State.overlayInit) {
        ULONGLONG startupInitDelayRemainingMs = 0;
        if (ShouldDeferOverlayInitForStartupCompat(frameDesc.OutputWindow, &startupInitDelayRemainingMs)) {
            static std::atomic<int> s_startupInitDelayLogCount{0};
            if (s_startupInitDelayLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
                HookLogImportant(
                    "DX12: Delaying overlay init during startup compatibility grace for %s (remaining=%llums)",
                    g_ProcessName, startupInitDelayRemainingMs);
            }
            return;
        }

        if (deferOverlayInitAfterResume) {
            static std::atomic<int> s_postResumeSettleLogCount{0};
            if (s_postResumeSettleLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
                HookLogImportant(
                    "DX12: Keeping overlay init deferred after startup-overlay resume for %s (remaining=%llums)",
                    g_ProcessName, postResumeSettleRemainingMs);
            }
            return;
        }

        if (s_insideECL) {
            static std::atomic<int> s_initDeferredInEclLogCount{0};
            if (s_initDeferredInEclLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
                HookLog("DX12: Deferring overlay init while inside ExecuteCommandLists re-entry");
            }
            return;
        }

        int frames = ++s_framesBeforeInit;
        if (frames < 1) {
            return;
        } else if (frames == 1) {
            HookLog("DX12: ProcessFrame - Proceeding with overlay init");
        }

        // Rate-limit reinit attempts: after 3 consecutive failures, back off
        // exponentially (wait 60, 120, 240… frames). This prevents the log-spam
        // and driver-stall loop that occurs when the device is removed but the
        // early health check at the top of ProcessFrame somehow misses it.
        static int s_consecutiveInitFails = 0;
        static int s_nextRetryFrame = 0;
        if (s_consecutiveInitFails >= 3 && frames < s_nextRetryFrame) {
            return;
        }

        // CRITICAL FIX: Don't initialize ImGui during FG suspension, FSR
        // stabilization, or native FSR FG This prevents initialization with
        // potentially unstable frame generation state and avoids initializing
        // overlay resources we'll never use (native FSR FG skips rendering)
        // CRITICAL FIX: Clean up any existing overlay context from previous
        // swapchain This happens when FSR FG recreates the swapchain and we
        // deferred cleanup MUST hold mutex to prevent race with DrawOverlay
        if (g_OverlayAdapter.IsInitialized()) {
            std::lock_guard<std::recursive_mutex> cleanupLock(g_OverlayMutex);
            HookLog("DX12: ProcessFrame - cleaning up stale OverlayAdapter (mutex held)");
            g_OverlayAdapter.Shutdown();
            CleanupOverlay();
            CleanupRTVs();
            HookLog("DX12: ProcessFrame - cleanup complete, proceeding with init");
        }

        DXGI_SWAP_CHAIN_DESC desc;
        if (SUCCEEDED(pSwapChain->GetDesc(&desc))) {
            g_State.cachedWidth = desc.BufferDesc.Width;
            g_State.cachedHeight = desc.BufferDesc.Height;

            // Use actual swapchain buffer count for ImGui initialization
            // The separate overlay queue (Change 1) eliminates the need for buffer
            // limiting
            int imguiBufferCount = desc.BufferCount;

            HookLog("DX12: ProcessFrame - initializing ImGui (%dx%d, buffers=%d)", g_State.cachedWidth,
                    g_State.cachedHeight, imguiBufferCount);

            // Validate swapchain buffers are accessible before initializing
            IDXGISwapChain3* sc3 = nullptr;
            if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3)))) {
                int validBuffers = 0;
                for (int i = 0; i < imguiBufferCount; i++) {
                    ID3D12Resource* bb = nullptr;
                    if (SUCCEEDED(sc3->GetBuffer(i, IID_PPV_ARGS(&bb)))) {
                        if (bb) {
                            bb->Release();
                            validBuffers++;
                        }
                    } else {
                        HookLog(
                            "DX12: ProcessFrame - buffer %d not accessible, stopping "
                            "validation",
                            i);
                        break;
                    }
                }

                if (validBuffers < imguiBufferCount) {
                    HookLog(
                        "DX12: ProcessFrame - only %d/%d buffers valid, skipping "
                        "ImGui init this frame",
                        validBuffers, imguiBufferCount);
                    sc3->Release();
                    return;
                }

                if (InitImGui(g_Device.load(), imguiBufferCount, desc.BufferDesc.Format, desc.OutputWindow)) {
                    s_consecutiveInitFails = 0;
                    s_nextRetryFrame = 0;

                    // Detect actual HDR state from the display output, not just format.
                    // R10G10B10A2_UNORM is used for both SDR 10-bit and HDR10/PQ.
                    // Only enable HDR overlay mode if the display is actually in HDR.
                    bool isActualHDR = false;
                    if (desc.BufferDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                        isActualHDR = true;  // FP16 is always HDR (scRGB)
                    } else if (desc.BufferDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM) {
                        IDXGIOutput* output = nullptr;
                        if (SUCCEEDED(sc3->GetContainingOutput(&output)) && output) {
                            IDXGIOutput6* output6 = nullptr;
                            if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output6)))) {
                                DXGI_OUTPUT_DESC1 desc1 = {};
                                if (SUCCEEDED(output6->GetDesc1(&desc1))) {
                                    isActualHDR = (desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
                                    HookLog("DX12: Display HDR check — colorSpace=%d, isHDR=%d", (int)desc1.ColorSpace,
                                            isActualHDR);
                                }
                                output6->Release();
                            }
                            output->Release();
                        }
                    }
                    g_OverlayAdapter.SetHDR(isActualHDR, (int)desc.BufferDesc.Format);

                    if (s_startupOverlayActivationStage == StartupOverlayActivationStage::kDelayRTVInitAfterBackendInit) {
                        s_startupOverlayActivationStageMs = GetTickCount64();
                        HookLogImportant(
                            "DX12: Startup compat staged activation - backend init complete, delaying RTV init for %llums",
                            kStartupOverlayPostBackendInitSettleMs);
                    } else {
                        int actualBufferCount = desc.BufferCount;
                        if (actualBufferCount > 8) {
                            HookLog("DX12: Swapchain has %d buffers, limiting RTVs to 8", actualBufferCount);
                            actualBufferCount = 8;
                        }
                        CreateRTVs(g_Device.load(), sc3, actualBufferCount);
                        if (!g_State.rtvDescHeap) {
                            HookLogImportant("DX12: RTV initialization failed during overlay init, deferring sync init");
                            sc3->Release();
                            return;
                        }
                        InitOverlaySync(g_Device.load(), imguiBufferCount, gameQueue);

                        HookLog(
                            "DX12: ProcessFrame - ImGui initialized with %d RTVs, "
                            "syncInit=%d",
                            actualBufferCount, g_State.syncInit);
                    }
                } else {
                    s_consecutiveInitFails++;
                    int backoffFrames = 60 * (1 << std::min(s_consecutiveInitFails - 3, 5));
                    s_nextRetryFrame = frames + backoffFrames;
                    if (s_consecutiveInitFails <= 5 || (s_consecutiveInitFails % 100) == 0) {
                        HookLog(
                            "DX12: ProcessFrame - ImGui initialization FAILED (attempt %d, next retry in %d frames)",
                            s_consecutiveInitFails, backoffFrames);
                    }
                }
                // SAFETY: Check sc3 is still valid before releasing
                if (sc3) {
                    sc3->Release();
                }
            } else {
                HookLog("DX12: ProcessFrame - failed to get IDXGISwapChain3 interface");
            }
        } else {
            HookLog("DX12: ProcessFrame - failed to get swapchain desc");
        }
    }

    if (allowOverlayRender && !suspendOverlayRender && !s_insideECL && g_State.overlayInit && !g_State.syncInit) {
        if (s_startupOverlayActivationStage == StartupOverlayActivationStage::kDelayRTVInitAfterBackendInit &&
            s_startupOverlayActivationStageMs != 0) {
            const ULONGLONG now = GetTickCount64();
            const ULONGLONG elapsedSinceBackendInit = now - s_startupOverlayActivationStageMs;
            if (elapsedSinceBackendInit < kStartupOverlayPostBackendInitSettleMs) {
                static std::atomic<int> s_postBackendStageLogCount{0};
                if (s_postBackendStageLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
                    HookLogImportant(
                        "DX12: Waiting to initialize staged overlay RTVs after backend init for %s (remaining=%llums)",
                        g_ProcessName, kStartupOverlayPostBackendInitSettleMs - elapsedSinceBackendInit);
                }
                return;
            }
        }

        DXGI_SWAP_CHAIN_DESC desc = {};
        if (FAILED(pSwapChain->GetDesc(&desc))) {
            HookLog("DX12: ProcessFrame - failed to get swapchain desc for staged activation");
            return;
        }

        IDXGISwapChain3* sc3 = nullptr;
        if (FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) {
            HookLog("DX12: ProcessFrame - failed to get SwapChain3 for staged activation");
            return;
        }

        int actualBufferCount = desc.BufferCount;
        if (actualBufferCount > 8) {
            HookLog("DX12: Swapchain has %d buffers during staged activation, limiting RTVs to 8", actualBufferCount);
            actualBufferCount = 8;
        }

        if (s_startupOverlayActivationStage == StartupOverlayActivationStage::kDelayRTVInitAfterBackendInit) {
            if (!g_State.rtvDescHeap) {
                HookLogImportant("DX12: Finalizing staged overlay activation step 1/2 - creating RTVs");
                CreateRTVs(g_Device.load(), sc3, actualBufferCount);
                if (!g_State.rtvDescHeap) {
                    HookLogImportant("DX12: Staged overlay RTV init failed, keeping sync init deferred");
                    sc3->Release();
                    return;
                }
            }
            s_startupOverlayActivationStage = StartupOverlayActivationStage::kDelaySyncInitAfterRTVInit;
            s_startupOverlayActivationStageMs = GetTickCount64();
            HookLogImportant(
                "DX12: Startup compat staged activation - RTV init complete, delaying sync init for %llums",
                kStartupOverlayPostRTVInitSettleMs);
            sc3->Release();
            return;
        }

        if (s_startupOverlayActivationStage == StartupOverlayActivationStage::kDelaySyncInitAfterRTVInit &&
            s_startupOverlayActivationStageMs != 0) {
            const ULONGLONG now = GetTickCount64();
            const ULONGLONG elapsedSinceRTVInit = now - s_startupOverlayActivationStageMs;
            if (elapsedSinceRTVInit < kStartupOverlayPostRTVInitSettleMs) {
                static std::atomic<int> s_postRtvStageLogCount{0};
                if (s_postRtvStageLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
                    HookLogImportant(
                        "DX12: Waiting to initialize staged overlay sync after RTV init for %s (remaining=%llums)",
                        g_ProcessName, kStartupOverlayPostRTVInitSettleMs - elapsedSinceRTVInit);
                }
                sc3->Release();
                return;
            }
        }

        if (!g_State.rtvDescHeap) {
            CreateRTVs(g_Device.load(), sc3, actualBufferCount);
            if (!g_State.rtvDescHeap) {
                HookLogImportant("DX12: RTV initialization failed during staged sync init, keeping overlay deferred");
                sc3->Release();
                return;
            }
        }
        HookLogImportant("DX12: Finalizing staged overlay activation step 2/2 - initializing sync");
        InitOverlaySync(g_Device.load(), desc.BufferCount, gameQueue);
        sc3->Release();

        if (g_State.syncInit) {
            ResetStartupOverlayBackendActivationStage();
            s_startupOverlaySyncInitMs = GetTickCount64();
            HookLogImportant(
                "DX12: Startup compat staged activation - sync init complete, delaying overlay rendering for %llums",
                kStartupOverlayPostSyncInitSettleMs);
            HookLogImportant("DX12: Staged overlay activation completed after backend-only init");
        }
        return;
    }

    // Single log on first frame to verify overlay system is entering
    static int s_firstFrameLogged = 0;
    if (s_firstFrameLogged == 0) {
        s_firstFrameLogged = 1;
        HookLog(
            "DX12: ProcessFrame first call - overlayInit=%d, syncInit=%d, "
            "gameQueue=%p",
            g_State.overlayInit, g_State.syncInit, gameQueue);
    }

    UINT currentBackBufferIdx = 0;
    bool hasCurrentBackBufferIdx = false;
    bool delayOverlayRenderAfterSyncInit = false;
    bool suppressOverlayRenderForLoadedStartupOverlay = false;
    bool delayOverlayRenderAfterResourcePrime = false;
    bool delayOverlayRenderAfterFirstDrawProbe = false;
    const bool shouldRunStartupOverlayDrawProbe =
        ce::overlay_compat::ShouldPreemptivelyDelayDX12OverlayInitForProcess(g_ProcessName) &&
        !IsActualFrameGenerationActive();

    if (allowOverlayRender && !suspendOverlayRender && !s_insideECL && g_State.overlayInit && g_State.syncInit &&
        s_startupOverlaySyncInitMs != 0) {
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG msSinceSyncInit = now - s_startupOverlaySyncInitMs;
        const bool processNeedsRenderDelay =
            ce::overlay_compat::ShouldPreemptivelyDelayDX12OverlayInitForProcess(g_ProcessName);
        const bool actualFGActive = IsActualFrameGenerationActive();
        const char* blockingOverlayModule = ce::overlay_compat::GetStartupBlockingOverlayRenderModuleName();
        const ULONGLONG lastBlockingRenderActivityMs =
            s_lastStartupBlockingRenderModuleActivityMs.load(std::memory_order_acquire);
        const bool hasRecentBlockingRenderActivity = ce::overlay_compat::HasRecentDX12StartupBlockingRenderActivity(
            lastBlockingRenderActivityMs, now, kStartupOverlayRenderModuleQuietPeriodMs);
        if (ce::overlay_compat::ShouldDelayDX12OverlayRenderAfterSyncInit(
                processNeedsRenderDelay, actualFGActive, msSinceSyncInit, kStartupOverlayPostSyncInitSettleMs)) {
            static std::atomic<int> s_postSyncStageLogCount{0};
            if (s_postSyncStageLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
                HookLogImportant("DX12: Waiting to render staged overlay after sync init for %s (remaining=%llums)",
                                 g_ProcessName, kStartupOverlayPostSyncInitSettleMs - msSinceSyncInit);
            }
            delayOverlayRenderAfterSyncInit = true;
        } else if (ce::overlay_compat::ShouldSuppressDX12OverlayRenderForLoadedStartupOverlay(
                       processNeedsRenderDelay, actualFGActive, blockingOverlayModule, msSinceSyncInit,
                       kStartupOverlayLoadedRenderModuleMaxBlockMs)) {
            static std::atomic<int> s_loadedStartupOverlayRenderSuppressLogCount{0};
            if (s_loadedStartupOverlayRenderSuppressLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
                HookLogImportant(
                    "DX12: Keeping overlay rendering disabled while startup-blocking render module %s remains loaded for %s (remaining=%llums)",
                    blockingOverlayModule, g_ProcessName, kStartupOverlayLoadedRenderModuleMaxBlockMs - msSinceSyncInit);
            }
            suppressOverlayRenderForLoadedStartupOverlay = true;
        } else if (ce::overlay_compat::ShouldSuppressDX12OverlayRenderForRecentBlockingRendererActivity(
                       processNeedsRenderDelay, actualFGActive, blockingOverlayModule, hasRecentBlockingRenderActivity)) {
            static std::atomic<int> s_recentBlockingRendererSuppressLogCount{0};
            const ULONGLONG msSinceLastActivity = now - lastBlockingRenderActivityMs;
            if (s_recentBlockingRendererSuppressLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
                HookLogImportant(
                    "DX12: Keeping overlay rendering disabled while startup-blocking render module %s still shows recent D3D12 activity for %s (quietRemaining=%llums)",
                    blockingOverlayModule, g_ProcessName,
                    kStartupOverlayRenderModuleQuietPeriodMs - msSinceLastActivity);
            }
            suppressOverlayRenderForLoadedStartupOverlay = true;
        } else {
            if (blockingOverlayModule && processNeedsRenderDelay && !actualFGActive) {
                if (lastBlockingRenderActivityMs != 0) {
                    HookLogImportant(
                        "DX12: Startup-blocking render module %s has been quiet for %llums; allowing overlay rendering for %s",
                        blockingOverlayModule, now - lastBlockingRenderActivityMs, g_ProcessName);
                } else {
                    HookLogImportant(
                        "DX12: Startup-blocking render module %s exceeded the startup safety window with no recent activity; allowing overlay rendering for %s",
                        blockingOverlayModule, g_ProcessName);
                }
            } else {
                HookLogImportant("DX12: Startup compat sync settle complete - allowing overlay rendering for %s",
                                 g_ProcessName);
            }
            s_startupOverlaySyncInitMs = 0;
        }
    }

    if (allowOverlayRender && !suspendOverlayRender && !s_insideECL && g_State.overlayInit && g_State.syncInit &&
        s_startupOverlayResourcePrimeMs != 0) {
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG msSinceResourcePrime = now - s_startupOverlayResourcePrimeMs;
        const bool processNeedsRenderDelay =
            ce::overlay_compat::ShouldPreemptivelyDelayDX12OverlayInitForProcess(g_ProcessName);
        if (processNeedsRenderDelay && !IsActualFrameGenerationActive() &&
            msSinceResourcePrime < kStartupOverlayPostResourcePrimeSettleMs) {
            static std::atomic<int> s_postResourcePrimeLogCount{0};
            if (s_postResourcePrimeLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
                HookLogImportant(
                    "DX12: Waiting to draw staged overlay after resource priming for %s (remaining=%llums)", g_ProcessName,
                    kStartupOverlayPostResourcePrimeSettleMs - msSinceResourcePrime);
            }
            delayOverlayRenderAfterResourcePrime = true;
        } else {
            HookLogImportant("DX12: Startup compat resource-prime settle complete - allowing first overlay draw for %s",
                             g_ProcessName);
            s_startupOverlayResourcePrimeMs = 0;
        }
    }

    if (allowOverlayRender && !suspendOverlayRender && !s_insideECL && g_State.overlayInit && g_State.syncInit &&
        s_startupOverlayFirstDrawProbeMs != 0) {
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG msSinceProbe = now - s_startupOverlayFirstDrawProbeMs;
        if (shouldRunStartupOverlayDrawProbe && msSinceProbe < kStartupOverlayFirstDrawProbeSettleMs) {
            static std::atomic<int> s_firstDrawProbeWaitLogCount{0};
            if (s_firstDrawProbeWaitLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
                HookLogImportant("DX12: Waiting to continue GTA overlay probe with %s for %s (remaining=%llums)",
                                 GetStartupOverlayFirstDrawProbeStageName(s_startupOverlayFirstDrawProbeStage),
                                 g_ProcessName,
                                 kStartupOverlayFirstDrawProbeSettleMs - msSinceProbe);
            }
            delayOverlayRenderAfterFirstDrawProbe = true;
        } else {
            HookLogImportant("DX12: GTA overlay probe settle complete - allowing %s for %s",
                             GetStartupOverlayFirstDrawProbeStageName(s_startupOverlayFirstDrawProbeStage), g_ProcessName);
            s_startupOverlayFirstDrawProbeMs = 0;
        }
    }

    if (allowOverlayRender && !suspendOverlayRender && !s_insideECL && g_State.overlayInit && g_State.syncInit &&
        !delayOverlayRenderAfterSyncInit && !suppressOverlayRenderForLoadedStartupOverlay &&
        !delayOverlayRenderAfterResourcePrime && !delayOverlayRenderAfterFirstDrawProbe) {
        // Single log on first successful overlay render
        static int s_firstOverlayLogged = 0;
        if (s_firstOverlayLogged == 0) {
            s_firstOverlayLogged = 1;
            HookLogImportant(
                "DX12: ProcessFrame - first overlay render attempt (fence=%p, "
                "cmdList=%p, fgActive=%d, fgType=%s)",
                g_State.fence, g_State.cmdList, g_FGCompat.IsFGActive() ? 1 : 0,
                g_FGCompat.GetFGTypeName(g_FGCompat.GetActiveFGType()));
        }

        // Log FG state transitions for debugging
        static bool s_lastFGActive = false;
        bool currentFGActive = g_FGCompat.IsFGActive();
        if (currentFGActive != s_lastFGActive) {
            HookLog("DX12: FG state changed: %s -> %s (type=%s)", s_lastFGActive ? "active" : "inactive",
                    currentFGActive ? "active" : "inactive", g_FGCompat.GetFGTypeName(g_FGCompat.GetActiveFGType()));
            s_lastFGActive = currentFGActive;
        }

        // Periodic health log every 500 frames for debugging stability
        static std::atomic<uint64_t> s_overlayFrameCount{0};
        uint64_t frameNum = s_overlayFrameCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (frameNum == 1 || frameNum == 10 || frameNum == 50 || frameNum == 100 || (frameNum % 500) == 0) {
            ID3D12Device* dev = g_Device.load();
            HRESULT devRemovedHr = dev ? dev->GetDeviceRemovedReason() : E_FAIL;
            HookLog(
                "DX12: Overlay frame #%llu (deviceRemoved=0x%08X, fgActive=%d, "
                "queue=%p, allocIdx=%d)",
                (unsigned long long)frameNum, (unsigned)devRemovedHr, currentFGActive ? 1 : 0, gameQueue,
                g_State.allocIndex);
        }

        // Check device removed BEFORE rendering.  On first detection, tear
        // down overlay resources and set g_DeviceRemoved so heartbeats stop
        // (letting the freeze watchdog create a dump if we spin forever).
        {
            ID3D12Device* devCheck = g_Device.load();
            if (devCheck && FAILED(devCheck->GetDeviceRemovedReason())) {
                if (!g_DeviceRemoved.load(std::memory_order_relaxed)) {
                    g_DeviceRemoved.store(true, std::memory_order_release);
                    DXGIShared::g_SharedState.deviceRemovedFatal.store(true, std::memory_order_release);
                    g_RenderWatchdog.SetForceMonitor(true);
                    HookLogImportant("DX12: GPU device removed (0x%08X) — cleaning up overlay",
                                     (unsigned)devCheck->GetDeviceRemovedReason());
                    g_State.overlayInit = false;
                    CleanupRTVs();
                }
                goto overlay_done;
            } else if (g_DeviceRemoved.load(std::memory_order_relaxed)) {
                // Device recovered (new device set via DX12_SetCommandQueue)
                g_DeviceRemoved.store(false, std::memory_order_release);
                DXGIShared::g_SharedState.deviceRemovedFatal.store(false, std::memory_order_release);
                g_RenderWatchdog.SetForceMonitor(false);
                HookLogImportant("DX12: Device recovered — overlay will reinitialize");
            }
        }

        {
            int allocatorPoolSize = static_cast<int>(g_State.allocators.size());
            if (allocatorPoolSize <= 0) {
                goto overlay_done;
            }

            int idx = g_State.allocIndex % allocatorPoolSize;
            g_State.allocIndex = (idx + 1) % allocatorPoolSize;

            // With 16 allocators, we never need to wait under normal conditions.
            // However, during Alt+Tab / GPU throttle, the GPU may stall and the
            // fence value for this allocator slot won't advance.  We must check
            // before Reset() to avoid undefined behaviour (driver hang / crash).
            auto* list = g_State.cmdList;
            auto* alloc = (idx < (int)g_State.allocators.size()) ? g_State.allocators[idx] : nullptr;
            if (list && alloc) {
                // Guard: skip overlay render if this allocator is still in flight.
                if (g_State.fence && idx < (int)g_State.fenceValues.size() && g_State.fenceValues[idx] > 0) {
                    UINT64 completed = g_State.fence->GetCompletedValue();
                    if (completed < g_State.fenceValues[idx]) {
                        if (activeDebugSample) {
                            activeDebugSample->flags |= kPresentSampleFlagAllocatorBusy;
                        }
                        static std::atomic<int> s_allocSkipLogs{0};
                        if (s_allocSkipLogs.fetch_add(1, std::memory_order_relaxed) < 30) {
                            HookLog(
                                "DX12: Allocator[%d] still in-flight (completed=%llu, needed=%llu), "
                                "skipping overlay this frame",
                                idx, completed, g_State.fenceValues[idx]);
                        }
                        goto overlay_done;
                    }
                }
                HRESULT allocResetHr = alloc->Reset();
                if (SUCCEEDED(allocResetHr)) {
                    HRESULT listResetHr = list->Reset(alloc, nullptr);
                    if (SUCCEEDED(listResetHr)) {
                        const bool shouldPrimeStartupOverlayResources =
                            ce::overlay_compat::ShouldPreemptivelyDelayDX12OverlayInitForProcess(g_ProcessName) &&
                            !IsActualFrameGenerationActive() && s_startupOverlayResourcePrimeMs == 0 &&
                            g_OverlayAdapter.HasPendingDX12Resources();
                        if (shouldPrimeStartupOverlayResources) {
                            HookLogImportant("DX12: Priming DX12 overlay resources before first GTA overlay draw");
                            if (!g_OverlayAdapter.PrimeDX12Resources(list)) {
                                HookLogImportant("DX12: DX12 overlay resource priming failed; deferring first overlay draw");
                                goto overlay_done;
                            }

                            HRESULT closeHr = list->Close();
                            if (FAILED(closeHr)) {
                                HookLog("DX12: Priming command list close failed hr=0x%08X, forcing reinit", closeHr);
                                g_State.syncInit = false;
                                goto overlay_done;
                            }

                            if (!SubmitOverlayCommandList(gameQueue, list, idx, "startup resource priming", false)) {
                                HookLogImportant("DX12: Startup resource priming submission failed; deferring first overlay draw");
                                goto overlay_done;
                            }

                            s_startupOverlayResourcePrimeMs = GetTickCount64();
                            HookLogImportant(
                                "DX12: DX12 overlay resource priming submitted, delaying first overlay draw for %llums",
                                kStartupOverlayPostResourcePrimeSettleMs);
                            goto overlay_done;
                        }

                        if (shouldRunStartupOverlayDrawProbe &&
                            s_startupOverlayFirstDrawProbeStage == StartupOverlayFirstDrawProbeStage::kNone) {
                            // Probe system removed: go straight to rendering.
                            // The 3-stage probe (backbuffer touch → pipeline state → real draw) caused
                            // ERR_GFX_STATE in GTA5 Enhanced because even barrier-only probes on a
                            // dedicated overlay queue conflict with the game's D3D12 state tracking.
                            // With single-queue mode (fix for dedicated queue), we can render directly.
                            s_startupOverlayFirstDrawProbeStage = StartupOverlayFirstDrawProbeStage::kActualRender;
                        }

                        IDXGISwapChain3* sc3 = g_State.cachedSC3;
                        if (!sc3) {
                            if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3)))) {
                                sc3->Release();  // drop QI ref — weak cache is safe
                                g_State.cachedSC3 = sc3;  // because swapchain is alive during Present
                            }
                        }
                        LARGE_INTEGER perfQI, perfGetBuf, perfRecord, perfSubmit, perfEnd, perfFreq;
                        QueryPerformanceFrequency(&perfFreq);
                        QueryPerformanceCounter(&perfQI);
                        if (sc3) {
                            UINT swapchainBufferIdx = sc3->GetCurrentBackBufferIndex();
                            currentBackBufferIdx = swapchainBufferIdx;
                            hasCurrentBackBufferIdx = true;
                            // CRITICAL FIX: Use actual swapchain buffer index directly
                            // CreateRTVs now creates RTVs for all swapchain buffers (up to 8)
                            // so no need to wrap the index - this prevents sync issues
                            UINT bufferIdx = swapchainBufferIdx;
                            // Validate buffer index is within our allocated range
                            if (bufferIdx >= (UINT)g_State.bufferCount) {
                                HookLog(
                                    "DX12: Buffer index %u exceeds allocated count %d, "
                                    "clamping",
                                    bufferIdx, g_State.bufferCount);
                                bufferIdx = g_State.bufferCount - 1;
                            }
                            // FG-SAFE: Acquire backbuffer per-frame via GetBuffer.
                            // We do NOT cache backbuffer pointers because FSR FG
                            // monitors reference counts and crashes if extra refs
                            // are held persistently.
                            ID3D12Resource* bb = nullptr;
                            bool bbNeedsRelease = false;
                            QueryPerformanceCounter(&perfGetBuf);
                            if (SUCCEEDED(sc3->GetBuffer(swapchainBufferIdx, IID_PPV_ARGS(&bb))) && bb) {
                                bbNeedsRelease = true;
                                // Recreate RTV for this buffer index (cheap CPU-side op).
                                // Ensures RTV matches current buffer even after FSR FG
                                // swapchain transitions.
                                D3D12_CPU_DESCRIPTOR_HANDLE rtvRecreate =
                                    g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
                                rtvRecreate.ptr += (SIZE_T)bufferIdx * g_State.rtvDescriptorSize;
                                g_Device.load()->CreateRenderTargetView(bb, nullptr, rtvRecreate);
                            }
                            if (bb) {
                                bool cmdRecordOk = false;
                                static std::atomic<int> s_firstBackBufferLogCount{0};
                                if (s_firstBackBufferLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
                                    HookLogImportant("DX12: Preparing overlay draw for backbuffer idx=%u resource=%p via %s queue (queue=%p)",
                                                     bufferIdx, bb,
                                                     g_State.overlayQueue ? "dedicated overlay" : (gameQueue == g_SwapchainQueue ? "swapchain" : "game"),
                                                     gameQueue);
                                }

                                // ================================================================
                                // PRIMARY PATH: Barrier-free offscreen compositing.
                                // Renders overlay to offscreen RT (no swapchain stall),
                                // then copies to backbuffer with implicit state promotion.
                                // DComp disabled — Direct Flip prevents reliable display.
                                // ================================================================

                                    // ============================================================
                                    // PRIMARY: Descriptor-free DX12 overlay.
                                    // Uses root constants + root SRV (ByteAddressBuffer for font)
                                    // instead of descriptor heaps.  SetDescriptorHeaps is never
                                    // called, avoiding the NVIDIA driver stall caused by
                                    // SetDescriptorHeaps + OMSetRenderTargets(swapchain) in the
                                    // same command list.
                                    // ============================================================
                                    bool usedDescFree = false;
                                    {
                                        auto* dev = g_Device.load();
                                        if (dev && !g_DescFreeBackend) {
                                            auto* backend = new DX12DescFreeBackend();
                                            if (backend->InitDevice(dev, g_State.format)) {
                                                g_DescFreeBackend = backend;
                                                g_D3D11On12Adapter.InitCustom(
                                                    g_DescFreeBackend, OverlayBackendType::DX12);
                                                HookLogImportant("DX12: Descriptor-free overlay backend ready");
                                            } else {
                                                delete backend;
                                                HookLogImportant("DX12: Descriptor-free backend init failed, "
                                                                 "falling back to standard DX12");
                                            }
                                        }
                                        if (g_DescFreeBackend && g_D3D11On12Adapter.IsInitialized()) {
                                            bool isRealFrame = g_FGCompat.IsCurrentFrameReal();

                                            // Barrier-free rendering: the backbuffer is in
                                            // PRESENT (= COMMON) state.  Swapchain backbuffers
                                            // have ALLOW_SIMULTANEOUS_ACCESS, so COMMON
                                            // promotes implicitly to RENDER_TARGET on the
                                            // DIRECT queue.  After ECL the promoted state
                                            // decays back to COMMON/PRESENT.
                                            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
                                                g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
                                            UINT rtvSize = dev->GetDescriptorHandleIncrementSize(
                                                D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                                            rtvHandle.ptr += (SIZE_T)bufferIdx * rtvSize;

                                            s_descFreeCmdList = list;
                                            s_descFreeRtv = rtvHandle;

                                            g_D3D11On12Adapter.SetIPCClient(g_IPC);
                                            if (isRealFrame) {
                                                g_D3D11On12Adapter.SetMetrics(
                                                    DXGIShared::GetPerformanceMetrics());
                                                static const bool s_isVKD3D = []() {
                                                    return GetModuleHandleA("d3d12core.dll") &&
                                                           (GetModuleHandleA("libvkd3d-1.dll") || GetModuleHandleA("vkd3d.dll"));
                                                }();
                                                const char* api = s_isVKD3D ? "DX12 (VKD3D)" : "DX12";
                                                g_D3D11On12Adapter.SetGraphicsAPI(api);
                                            }

                                            g_D3D11On12Adapter.RenderOverlay(
                                                g_State.cachedWidth, g_State.cachedHeight);

                                            s_descFreeCmdList = nullptr;
                                            usedDescFree = true;
                                        }
                                    }

                                    // Fallback: standard DX12 rendering (uses SetDescriptorHeaps —
                                    // may cause 60% GPU on some NVIDIA configs)
                                    if (!usedDescFree) {
                                        if (!startupOverlayPresent) {
                                            D3D12_RESOURCE_BARRIER barrier = {};
                                            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                            barrier.Transition.pResource = bb;
                                            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                                            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                                            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                                            list->ResourceBarrier(1, &barrier);
                                        }

                                        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
                                            g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
                                        UINT rtvSize =
                                            g_Device.load()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                                        rtvHandle.ptr += (SIZE_T)bufferIdx * rtvSize;
                                        list->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

                                        bool isRealFrame = g_FGCompat.IsCurrentFrameReal();
                                        DrawOverlay(list, isRealFrame, bufferIdx);

                                        if (!startupOverlayPresent) {
                                            D3D12_RESOURCE_BARRIER barrier = {};
                                            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                            barrier.Transition.pResource = bb;
                                            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                                            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                                            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                                            list->ResourceBarrier(1, &barrier);
                                        }
                                    }

                                    QueryPerformanceCounter(&perfRecord);

                                    HRESULT closeHr = list->Close();
                                    if (FAILED(closeHr)) {
                                        HookLog("DX12: list->Close failed hr=0x%08X, forcing reinit", closeHr);
                                        g_State.syncInit = false;
                                    } else {
                                        // Submit overlay ECL on gameQueue, then DEFER the fence
                                        // Signal to the next frame.  This avoids the NVIDIA driver
                                        // stall caused by Signal between ECL and Present.
                                        ExecuteCommandListsPtr origECL =
                                            GetOriginalExecuteCommandLists(gameQueue);
                                        ID3D12CommandList* lists[] = {list};
                                        if (origECL)
                                            origECL(gameQueue, 1, lists);
                                        else
                                            gameQueue->ExecuteCommandLists(1, lists);

                                        // Defer fence Signal to next frame's ProcessFrame entry
                                        if (g_State.fence) {
                                            UINT64 next = g_State.currentFenceValue + 1;
                                            g_deferredSignalQueue.store(gameQueue, std::memory_order_release);
                                            g_deferredSignalValue.store(next, std::memory_order_release);
                                            g_deferredSignalAllocIdx.store(idx, std::memory_order_release);
                                            // Don't update currentFenceValue yet — the deferred
                                            // flush at next frame start will do it.
                                        }
                                        cmdRecordOk = true;
                                        QueryPerformanceCounter(&perfSubmit);
                                    }

                                QueryPerformanceCounter(&perfEnd);
                                // Periodic perf dump every 300 frames
                                static int s_perfDumpCounter = 0;
                                if (++s_perfDumpCounter % 300 == 0) {
                                    double toUs = 1000000.0 / (double)perfFreq.QuadPart;
                                    double qiUs = (double)(perfGetBuf.QuadPart - perfQI.QuadPart) * toUs;
                                    double getBufUs = (double)(perfRecord.QuadPart - perfGetBuf.QuadPart) * toUs;
                                    double submitUs = (double)(perfSubmit.QuadPart - perfRecord.QuadPart) * toUs;
                                    double totalUs = (double)(perfEnd.QuadPart - perfQI.QuadPart) * toUs;
                                    HookLogImportant(
                                        "DX12: Overlay perf: QI+idx=%.0fus getBuf+record=%.0fus submit=%.0fus total=%.0fus",
                                        qiUs, getBufUs, submitUs, totalUs);
                                }

                                if (cmdRecordOk) {
                                    static int s_firstOverlaySubmitLogged = 0;
                                    if (s_firstOverlaySubmitLogged == 0) {
                                        s_firstOverlaySubmitLogged = 1;
                                        HookLogImportant(
                                            "DX12: ProcessFrame - first overlay render command list submitted successfully");
                                    }

                                    // Clear probe state if we were in a probe sequence
                                    if (shouldRunStartupOverlayDrawProbe &&
                                        s_startupOverlayFirstDrawProbeStage ==
                                            StartupOverlayFirstDrawProbeStage::kActualRender) {
                                        HookLogImportant("DX12: Startup overlay probe complete - rendering stably");
                                        s_startupOverlayFirstDrawProbeStage =
                                            StartupOverlayFirstDrawProbeStage::kComplete;
                                        s_startupOverlayFirstDrawProbeMs = 0;
                                    }
                                }
                                // FG-SAFE: Release per-frame backbuffer reference
                                if (bbNeedsRelease)
                                    bb->Release();
                            } else {
                                HookLog("DX12: GetBuffer(%u) failed, forcing RTV reinit", swapchainBufferIdx);
                                CleanupRTVs();
                                g_State.overlayInit = false;
                            }
                        } else {
                            HookLog("DX12: ProcessFrame - failed to get SwapChain3 interface");
                        }
                    } else {
                        HookLog("DX12: ProcessFrame - list->Reset failed hr=0x%08X, forcing reinit", listResetHr);
                        g_State.syncInit = false;
                    }
                } else {
                    HookLog("DX12: ProcessFrame - alloc->Reset failed hr=0x%08X, forcing reinit", allocResetHr);
                    g_State.syncInit = false;
                }
            } else {
                HookLog("DX12: ProcessFrame - null list or alloc");
            }
        }  // end device-removed-check scope
    overlay_done:;

    }

    // Change 6: Remove verbose debug logging - keep only error logging
    if (processCapture && g_IPC && g_IPC->IsRecording()) {
        int64_t captureStartUs = PerfLogger::GetQpcUs();
        SharedMemoryLayout* shm = g_IPC->GetSharedMem();
        if (shm) {
            if (shm->throttleCapture.load(std::memory_order_acquire)) {
                // Skip capture to let encoder catch up
            } else {
                if (!g_SharedCaptureD3D12.IsActive())
                    g_SharedCaptureD3D12.Initialize(g_Device.load(), pSwapChain);
                if (g_SharedCaptureD3D12.IsActive()) {
                    std::lock_guard<std::recursive_mutex> capLock(g_DX12CaptureMutex);
                    // Keep capture submission on the same queue selection as overlay/present
                    // work to avoid cross-queue sync jitter.
                    ID3D12CommandQueue* captureQueue = gameQueue;
                    UINT bbIdx = 0;
                    if (hasCurrentBackBufferIdx) {
                        bbIdx = currentBackBufferIdx;
                    } else {
                        IDXGISwapChain3* sc3 = nullptr;
                        pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3));
                        bbIdx = sc3 ? sc3->GetCurrentBackBufferIndex() : 0;
                        if (sc3)
                            sc3->Release();
                    }
                    if (captureQueue && g_SharedCaptureD3D12.CaptureFrame(captureQueue, bbIdx)) {
                        SharedFrameDescriptor desc;
                        if (g_SharedCaptureD3D12.GetCurrentFrame(&desc)) {
                            shm->SetSharedHandle(0, (uint64_t)g_SharedCaptureD3D12.GetSharedHandle(0));
                            shm->SetSharedHandle(1, (uint64_t)g_SharedCaptureD3D12.GetSharedHandle(1));
                            shm->SetFenceShareHandle((uint64_t)g_SharedCaptureD3D12.GetFenceShareHandle());
                            shm->SetWidth(desc.width);
                            shm->SetHeight(desc.height);
                            shm->SetFormat(desc.format);
                            // CRITICAL FIX: Use acquire ordering to see consumer's readIndex
                            // updates
                            uint32_t wIdx = shm->frameRing.writeIndex.load(std::memory_order_acquire);
                            uint32_t rIdx = shm->frameRing.readIndex.load(std::memory_order_acquire);
                            if ((uint32_t)(wIdx - rIdx) < (uint32_t)FRAME_RING_SIZE) {
                                FrameSlot& slot = shm->frameRing.slots[wIdx % FRAME_RING_SIZE];
                                slot.fenceValue = desc.fenceValue;
                                slot.timestamp = desc.presentTime;
                                slot.frameIndex = desc.frameNumber;
                                slot.textureIndex = desc.textureIndex;
                                slot.sourcePid = GetCurrentProcessId();
                                // CRITICAL FIX: Add release fence before setting valid flag
                                // Ensures all slot fields are visible to consumer before valid=1
                                std::atomic_thread_fence(std::memory_order_release);
                                slot.valid.store(1, std::memory_order_release);
                                shm->frameRing.writeIndex.store(wIdx + 1, std::memory_order_release);
                            } else
                                shm->frameRing.droppedFrames.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
            }
            perfMetrics.captureUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - captureStartUs);
        }
    }
}

// Delay overlay rendering for first frames after ImGui init
// This prevents GPU crashes when frame generation tech (DLSS FG/FSR FG) is
// initializing
static std::atomic<bool> s_initDelayComplete{false};

void DX12_ResetImGuiFrameCounter() {
    s_framesBeforeInit = 0;
    // Also reset the post-init frame counter
    s_framesSinceInit = 0;
    HookLog("DX12: Reset ImGui frame counter");
}

void DX12_ResetOverlayFrameDelay() {
    s_framesSinceInit = 0;
    s_initDelayComplete = false;
    HookLog("DX12: Reset overlay frame delay counter");
}

void DX12_ProcessFrameExternal(IDXGISwapChain* pSwapChain) {
    // CRITICAL: Skip all rendering during shutdown to prevent crashes
    if (HookIsShuttingDown()) {
        return;
    }

    // Heartbeat for freeze watchdog — skip when device is removed so the
    // watchdog can detect the stuck state and create a diagnostic dump.
    if (!g_DeviceRemoved.load(std::memory_order_relaxed)) {
        g_RenderWatchdog.Heartbeat();
    }

    // Retry FFX hook initialization periodically for late-loading FSR FG modules.
    // UE5 games often load amd_fidelityfx_framegeneration_dx12.dll after initial
    // hook setup completes, so we must retry until the module is found.
    static int s_ffxRetryCounter = 0;
    static bool s_ffxRetryLogged = false;
    if (!FFXHook::IsInitialized()) {
        // Retry FFX hook every 60 frames (UE5 games may load FFX modules late)
        if (++s_ffxRetryCounter % 60 == 0) {
            FFXHook::Init();
            if (FFXHook::IsInitialized()) {
                HookLog("DX12: FFX Hook installed on render-frame retry #%d", s_ffxRetryCounter / 60);
            } else if (!s_ffxRetryLogged && s_ffxRetryCounter >= 600) {
                s_ffxRetryLogged = true;
                HookLog("DX12: FFX Hook not found after %d render-frame retries (FG may use native integration)",
                        s_ffxRetryCounter / 60);
            }
        }
    }

    // CRITICAL FIX: Reset delay flag when ImGui is not initialized
    // This ensures we wait again after each init
    if (!g_State.overlayInit) {
        s_initDelayComplete = false;
        s_framesSinceInit = 0;
    }

    // Minimal delay after ImGui init before rendering overlay (for stability)
    if (g_State.overlayInit && !s_initDelayComplete.load()) {
        int frames = ++s_framesSinceInit;
        if (frames < 1) {
            // Skip - proceed immediately
            return;
        } else {
            s_initDelayComplete = true;
            HookLog(
                "DX12: ProcessFrameExternal - Overlay rendering enabled (frame "
                "%d after init)",
                frames);
        }
    }

    // CRITICAL FIX: Dynamically detect Vulkan WSI swapchains
    // When NVIDIA's Vulkan WSI-to-DXGI mapping is active, the swapchain is
    // presented through DXGI but the device is not a real D3D12 device we can
    // render to. Check this dynamically because games can switch between Vulkan
    // WSI (focused) and DXGI (unfocused) modes.
    static bool s_checkedForVulkan = false;
    static bool s_vulkanLayerActive = false;
    if (!s_checkedForVulkan) {
        HMODULE hVulkanLayer = GetModuleHandleW(L"VK_LAYER_CE_overlay.dll");
        if (!hVulkanLayer) {
            hVulkanLayer = GetModuleHandleW(L"VK_LAYER_CE_overlay_x86.dll");
        }
        s_vulkanLayerActive = (hVulkanLayer != nullptr);
        if (s_vulkanLayerActive) {
            HookLog(
                "DX12: Vulkan layer detected, will skip DXGI overlay for Vulkan "
                "WSI swapchains");
        }
        s_checkedForVulkan = true;
    }

    // If Vulkan layer is active, check if this is a Vulkan WSI swapchain
    // by attempting to get the D3D12 device - Vulkan WSI swapchains will fail
    // or return a device we can't use for rendering
    if (s_vulkanLayerActive && pSwapChain) {
        ID3D12Device* pDevice = nullptr;
        HRESULT hr = pSwapChain->GetDevice(IID_PPV_ARGS(&pDevice));
        if (FAILED(hr) || !pDevice) {
            // This is likely a Vulkan WSI swapchain - skip DX12 overlay
            // The Vulkan layer will handle overlay rendering
            return;
        }
        // Check if we can actually use this device (Vulkan WSI devices may fail
        // here)
        D3D12_FEATURE_DATA_FEATURE_LEVELS featureLevels = {};
        hr = pDevice->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &featureLevels, sizeof(featureLevels));
        pDevice->Release();
        if (FAILED(hr)) {
            // Vulkan WSI device that doesn't support full D3D12 features
            return;
        }
    }

    if (!pSwapChain) {
        HookLog("DX12: ProcessFrameExternal - null swapchain");
        return;
    }
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) {
        HookLog("DX12: ProcessFrameExternal - failed to get SwapChain3");
        return;
    }
    int count = g_CommandListsExecutedThisFrame.exchange(0);
    ++g_FGDebugFrameCount;
    g_FGCompat.RecordFrame(count);
    if (!CanUseFSRFGHeuristics()) {
        g_FGCompat.SetHeuristicFSRFGActive(false);
    }
    // Interpolated (FG) frame detection: the game submits zero command lists
    // between consecutive Present calls for frames generated by the FG engine.
    bool isInterpolatedFrame = (count == 0);
    if (PresentDebugSample* activeDebugSample = PerfLogger::Get().GetActiveDebugSample();
        activeDebugSample && isInterpolatedFrame) {
        activeDebugSample->flags |= kPresentSampleFlagInterpolatedFrame;
    }

    // ECL-count-based FG activation: detect frame generation via the pattern
    // of alternating real (ECL>0) and interpolated (ECL=0) frames.  This works
    // for UE5 native FSR FG and other implementations that don't use hookable
    // DLLs (e.g., statically linked into engine plugins).
    {
        static int s_eclRealFrames = 0;
        static int s_eclInterpFrames = 0;
        static bool s_eclFGDetected = false;
        if (s_eclFGDetected && !g_FGCompat.IsHeuristicFSRFGActive()) {
            s_eclFGDetected = false;
            s_eclRealFrames = 0;
            s_eclInterpFrames = 0;
        }
        if (isInterpolatedFrame)
            ++s_eclInterpFrames;
        else
            ++s_eclRealFrames;
        if (!s_eclFGDetected && s_eclInterpFrames >= 10 && s_eclRealFrames >= 5) {
            if (UpdateHeuristicFSRFGState(true, "ecl-pattern")) {
                s_eclFGDetected = true;
                HookLogImportant("DX12: FG detected via ECL count pattern (real=%d, interp=%d)", s_eclRealFrames,
                                 s_eclInterpFrames);
            }
        }
    }

    // With the dedicated overlay queue, overlay commands execute on a separate
    // GPU queue with CPU-side fence synchronization, so it is safe to render
    // on both real and interpolated FG frames.  Without an overlay queue, we
    // must skip interpolated frames to avoid submitting work on the game queue
    // during Streamline's Present pipeline.
    // EXCEPTION: For heuristic FSR FG in single-queue mode, the overlay submits
    // to the swapchain queue (which FSR FG owns), so rendering on interpolated
    // frames is safe and required to avoid flickering (otherwise overlay only
    // appears on real frames = half the output).
    // Check ShouldUseDedicatedOverlayQueue() (FG active) instead of just queue
    // existence, since the queue is now kept alive across FG mode switches.
    bool hasDedicatedQueue = ShouldUseDedicatedOverlayQueue() && g_State.overlayQueue != nullptr &&
                             g_State.crossQueueFence != nullptr && g_State.crossQueueFenceEvent != nullptr;
    bool heuristicFSRFG = g_FGCompat.IsHeuristicFSRFGActive();
    if (isInterpolatedFrame && !hasDedicatedQueue && !heuristicFSRFG) {
        sc3->Release();
        return;
    }
    // For interpolated frames, only render overlay (no capture processing) since
    // the backbuffer content is from the FG engine, not a real game frame.
    ProcessFrame(sc3, /*processCapture=*/!isInterpolatedFrame);
    sc3->Release();
}

namespace DXGIShared {
void HandleDX12ProcessFrame(IDXGISwapChain* pSwapChain, bool isRealFrame) {
    DX12_ProcessFrameExternal(pSwapChain);
}
void HandleDX12ResizeBegin() {
    HookLog("DX12: HandleDX12ResizeBegin CALLED from DetourResizeBuffers");
    DX12_OnSwapchainResizeBegin();
}
void HandleDX12ResizeEnd() {
    HookLog("DX12: HandleDX12ResizeEnd CALLED");
    DX12_OnSwapchainResizeEnd();
}
}  // namespace DXGIShared

// Flush the deferred fence Signal AFTER Present.  The NVIDIA driver stalls the
// GPU when a Signal call sits between the overlay ECL and Present.  By deferring
// the Signal to after Present, the presentation pipeline is uninterrupted.
extern "C" __declspec(dllexport) void DX12_FlushDeferredSignal() {
    UINT64 deferredVal = g_deferredSignalValue.load(std::memory_order_acquire);
    if (deferredVal == 0 || !g_State.fence)
        return;

    // Use the queue that actually submitted the overlay ECL.  When FG runtimes
    // create swapchains with their own queue, this may differ from g_CommandQueue.
    ID3D12CommandQueue* q = g_deferredSignalQueue.load(std::memory_order_acquire);
    if (!q)
        q = g_CommandQueue.load(std::memory_order_acquire);
    if (!q)
        return;

    HRESULT hr = q->Signal(g_State.fence, deferredVal);
    if (SUCCEEDED(hr)) {
        int allocIdx = g_deferredSignalAllocIdx.load(std::memory_order_acquire);
        g_State.currentFenceValue = deferredVal;
        if (allocIdx >= 0 && allocIdx < (int)g_State.fenceValues.size())
            g_State.fenceValues[allocIdx] = deferredVal;
    }
    g_deferredSignalValue.store(0, std::memory_order_release);
    g_deferredSignalAllocIdx.store(-1, std::memory_order_release);
    g_deferredSignalQueue.store(nullptr, std::memory_order_release);
}

// External function for swapchain wrapper to wait for overlay completion before
// Present
extern "C" __declspec(dllexport) void DX12_WaitForOverlayCompletion(ID3D12CommandQueue* pGameQueue) {
    (void)pGameQueue;
    if (!g_State.fence)
        return;

    UINT64 fenceValueToWait = g_State.currentFenceValue;
    if (fenceValueToWait == 0)
        return;

    const char* overlayModule = ce::overlay_compat::GetStartupBlockingOverlayModuleName();
    const bool actualFGActive = IsActualFrameGenerationActive();
    // Check ShouldUseDedicatedOverlayQueue() (FG active) instead of just queue
    // existence, since the queue is now kept alive across FG mode switches.
    const bool usingDedicatedQueue = ShouldUseDedicatedOverlayQueue() && (g_State.overlayQueue != nullptr);
    if ((!usingDedicatedQueue && (!overlayModule || actualFGActive)) || !g_State.fenceEvent)
        return;

    if (!usingDedicatedQueue) {
        DWORD foregroundPid = 0;
        HWND foregroundWindow = GetForegroundWindow();
        if (!foregroundWindow)
            return;
        GetWindowThreadProcessId(foregroundWindow, &foregroundPid);
        if (foregroundPid != GetCurrentProcessId())
            return;
    }

    if (g_State.fence->GetCompletedValue() >= fenceValueToWait)
        return;

    HRESULT setHr = g_State.fence->SetEventOnCompletion(fenceValueToWait, g_State.fenceEvent);
    if (FAILED(setHr))
        return;

    static std::atomic<int> s_waitLogCount{0};
    constexpr DWORD kCompatWaitTimeoutMs = 16;
    DWORD waitHr = WaitForSingleObject(g_State.fenceEvent, kCompatWaitTimeoutMs);
    if (waitHr == WAIT_TIMEOUT) {
        if (s_waitLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
            HookLog("DX12: Overlay completion wait timed out for %s mode (fence=%llu)",
                    usingDedicatedQueue ? "dedicated-queue" : (overlayModule ? overlayModule : "single-queue"),
                    (unsigned long long)fenceValueToWait);
        }
    } else if (waitHr == WAIT_OBJECT_0) {
        if (s_waitLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
            HookLog("DX12: Overlay completion wait finished for %s mode (fence=%llu)",
                    usingDedicatedQueue ? "dedicated-queue" : (overlayModule ? overlayModule : "single-queue"),
                    (unsigned long long)fenceValueToWait);
        }
    }
}

static const GUID SKID_D3D12SwapChainBufferBitmap = {
    0xbc53df3b, 0x956f, 0x47db, {0xa6, 0x53, 0x5, 0xd7, 0xb8, 0x71, 0x53, 0x38}};
static std::atomic<int> g_ECLCallCount{0};

void STDMETHODCALLTYPE DetourExecuteCommandLists(ID3D12CommandQueue* pThis, UINT NumCommandLists,
                                                 ID3D12CommandList* const* ppCommandLists) {
    // Heartbeat for freeze watchdog — skip when device is removed
    if (!g_DeviceRemoved.load(std::memory_order_relaxed)) {
        g_RenderWatchdog.Heartbeat();
    }

    // CRITICAL: Recursion depth guard.  If an FG engine (FSR FG, DLSS FG)
    // hooks ECL and its "original" pointer loops back to us, we'd recurse
    // infinitely.  Detect and break the cycle by calling the real ECL directly.
    static thread_local int s_eclRecursionDepth = 0;
    if (s_eclRecursionDepth > 0) {
        // We're being called recursively — an FG hook is looping back to us.
        // Call the original (real D3D12) ECL directly to break the cycle.
        ExecuteCommandListsPtr original = oExecuteCommandLists;
        if (original)
            original(pThis, NumCommandLists, ppCommandLists);
        return;
    }
    ++s_eclRecursionDepth;
    auto depthGuard = ce::make_scope_guard([&]() { --s_eclRecursionDepth; });

    // Track that this thread is inside an ECL call.  During Alt+Tab, D3D12's
    // internal WaitImpl can pump window messages which may trigger Present →
    // ProcessFrame.  ProcessFrame checks s_insideECL and skips overlay rendering
    // to prevent a cascading second WaitImpl that hangs the render thread.
    bool wasInsideECL = s_insideECL;
    s_insideECL = true;
    auto eclGuard = ce::make_scope_guard([&]() { s_insideECL = wasInsideECL; });

    // Skip our own overlay queue - don't count overlay submissions as game
    // command lists and don't re-register the overlay queue as the game queue.
    if (pThis == g_State.overlayQueue) {
        ExecuteCommandListsPtr original = GetOriginalExecuteCommandLists(pThis);
        if (original)
            original(pThis, NumCommandLists, ppCommandLists);
        return;
    }

    NoteStartupBlockingRenderModuleActivityFromECL(pThis, CE_RETURN_ADDRESS());

    // Debug: Log first few calls to verify hook is working
    int count = g_ECLCallCount.load(std::memory_order_relaxed);
    if (count < 5) {
        count = g_ECLCallCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 5) {
            HookLog("DX12: ExecuteCommandLists called #%d (queue=%p)", count, pThis);
        }
    }

    // Count command lists only from the primary game queue to detect real frames.
    // FG runtimes (FSR FG) create their own queues that share the vtable; without
    // this filter, interpolated frames have similar ECL counts to real frames.
    ID3D12CommandQueue* primaryQ = g_PrimaryGameQueue.load(std::memory_order_acquire);
    if (!primaryQ || pThis == primaryQ) {
        g_CommandListsExecutedThisFrame.fetch_add(NumCommandLists, std::memory_order_relaxed);
    }

    // Register game's queue for overlay execution
    DX12_SetCommandQueue(pThis);

    ExecuteCommandListsPtr original = GetOriginalExecuteCommandLists(pThis);


    if (original)
        original(pThis, NumCommandLists, ppCommandLists);
}

void DX12_HookQueueVTable(ID3D12CommandQueue* queue) {
    if (!queue)
        return;

    // Never hook our own overlay queue to avoid re-entry in ECL
    if (queue == g_State.overlayQueue)
        return;

    // We ALWAYS hook the queue for freeze detection heartbeat
    // The overlay rendering is skipped separately in ProcessFrameExternal if
    // needed This ensures freeze watchdog works even with DLSS/FSR FG

    void* unwrapped = nullptr;
    static const GUID IID_CWrapD3D12CommandQueue = {
        0xd4e5f678, 0x90ab, 0xcdef, {0x12, 0x34, 0x56, 0x78, 0x90, 0x12, 0x34, 0x56}};
    if (SUCCEEDED(queue->QueryInterface(IID_CWrapD3D12CommandQueue, &unwrapped))) {
        ((IUnknown*)unwrapped)->Release();
        return;
    }
    static std::recursive_mutex s_HookMutex;
    std::lock_guard<std::recursive_mutex> lock(s_HookMutex);
    void** vtbl = *reinterpret_cast<void***>(queue);

    // CRITICAL FIX: Check if we've already hooked this vtable BEFORE checking
    // the current vtable entry.  FG engines (FSR FG, DLSS FG) may overwrite
    // our vtable entry with their own hook.  If we detect the change and
    // re-hook, we create a circular hook chain:
    //   DetourECL → FG_ECL → DetourECL → FG_ECL → ... (stack overflow)
    // because FG's saved "original" points to our detour, and our new
    // "original" points to FG's hook.  The correct behavior is to leave
    // FG's hook in place — our detour is still in the chain via FG's
    // saved original pointer.
    {
        std::lock_guard<std::recursive_mutex> stateLock(g_ExecuteCommandListsHookStateMutex);
        bool alreadyHooked =
            g_ExecuteCommandListsOriginalByVTable.find(vtbl) != g_ExecuteCommandListsOriginalByVTable.end();
        if (alreadyHooked) {
            // Another hook (FSR FG, DLSS FG, etc.) may have replaced our
            // vtable entry, but the chain is intact:
            //   FG_ECL → DetourECL → realECL
            // Do NOT re-hook — that would create infinite recursion.
            if (vtbl[10] != (void*)DetourExecuteCommandLists) {
                static std::atomic<int> s_chainNotifyCount{0};
                if (s_chainNotifyCount.fetch_add(1, std::memory_order_relaxed) < 3) {
                    HookLogImportant(
                        "DX12: ECL vtable[%p] modified by FG engine (was our "
                        "detour, now %p) - chain intact, NOT re-hooking",
                        vtbl, vtbl[10]);
                }
            }
            return;
        }
    }

    if (vtbl[10] != (void*)DetourExecuteCommandLists) {
        HookLog("DX12: Hooking ExecuteCommandLists vtable for queue %p", queue);
        ExecuteCommandListsPtr original = nullptr;
        VTableHook::Status hookStatus =
            VTableHook::Create(&vtbl[10], (LPVOID)DetourExecuteCommandLists, (LPVOID*)&original);
        if (hookStatus == VTableHook::Success && original) {
            std::lock_guard<std::recursive_mutex> stateLock(g_ExecuteCommandListsHookStateMutex);
            g_ExecuteCommandListsOriginalByVTable[vtbl] = original;
            if (!oExecuteCommandLists)
                oExecuteCommandLists = original;
        }
    } else {
        std::lock_guard<std::recursive_mutex> stateLock(g_ExecuteCommandListsHookStateMutex);
        if (g_ExecuteCommandListsOriginalByVTable.find(vtbl) == g_ExecuteCommandListsOriginalByVTable.end() &&
            oExecuteCommandLists) {
            g_ExecuteCommandListsOriginalByVTable[vtbl] = oExecuteCommandLists;
        }
    }
}

// Hook device vtable for CreateSampler interception
void DX12_HookDeviceVTable(ID3D12Device* device) {
    if (!device)
        return;

    // Don't hook wrapped devices
    void* unwrapped = nullptr;
    static const GUID IID_CWrapD3D12Device = {
        0xc3d4e5f6, 0x7890, 0xabcd, {0xef, 0x12, 0x34, 0x56, 0x78, 0x90, 0x12, 0x34}};
    if (SUCCEEDED(device->QueryInterface(IID_CWrapD3D12Device, &unwrapped))) {
        ((IUnknown*)unwrapped)->Release();
        return;  // Already wrapped, skip vtable hook
    }

    static std::recursive_mutex s_DeviceHookMutex;
    std::lock_guard<std::recursive_mutex> lock(s_DeviceHookMutex);

    void** vtbl = *reinterpret_cast<void***>(device);

    // CreateSampler is at vtable index 20 in ID3D12Device
    // ID3D12Object: QueryInterface=0, AddRef=1, Release=2, GetPrivateData=3,
    // SetPrivateData=4, SetPrivateDataInterface=5, SetName=6 ID3D12Device:
    // GetNodeCount=7, CreateCommandQueue=8, CreateCommandAllocator=9,
    // CreateGraphicsPipelineState=10, CreateComputePipelineState=11,
    // CreateCommandList=12, CheckFeatureSupport=13, CreateDescriptorHeap=14,
    // GetDescriptorHandleIncrementSize=15, CreateRootSignature=16,
    // CreateConstantBufferView=17, CreateShaderResourceView=18,
    // CreateUnorderedAccessView=19, CreateRenderTargetView=20,
    // CreateDepthStencilView=21, CreateSampler=22 Let's use 22 for CreateSampler

    if (vtbl[22] != (void*)DetourCreateSampler) {
        HookLog("DX12: Hooking CreateSampler vtable for device %p", device);
        VTableHook::Create(&vtbl[22], (LPVOID)DetourCreateSampler, (LPVOID*)&oCreateSampler);
    }
}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChain(IDXGIFactory* pThis, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
                                                IDXGISwapChain** ppSwapChain) {
    // CRITICAL: ALWAYS hook the command queue vtable for frame detection
    if (pDevice) {
        ID3D12CommandQueue* q = nullptr;
        if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&q)))) {
            DX12_HookQueueVTable(q);
            q->Release();
        }
    }

    HRESULT hr = oCreateSwapChain(pThis, pDevice, pDesc, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        IDXGISwapChain3* sc3 = nullptr;
        if (SUCCEEDED((*ppSwapChain)->QueryInterface(IID_PPV_ARGS(&sc3)))) {
            sc3->Release();
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwnd(IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd,
                                                       const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                       const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFDesc, IDXGIOutput* pOut,
                                                       IDXGISwapChain1** ppSC) {
    // CRITICAL: ALWAYS hook the command queue vtable for frame detection
    if (pDevice) {
        ID3D12CommandQueue* q = nullptr;
        if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&q)))) {
            DX12_HookQueueVTable(q);
            q->Release();
        }
    }

    HRESULT hr = oCreateSwapChainForHwnd(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);

    if (SUCCEEDED(hr) && ppSC && *ppSC) {
        IDXGISwapChain3* sc3 = nullptr;
        if (SUCCEEDED((*ppSC)->QueryInterface(IID_PPV_ARGS(&sc3)))) {
            sc3->Release();
        }
        // NOTE: Do NOT call DX12_SetSwapchainQueue here.  This factory vtable
        // hook fires for ALL callers (including Streamline/Social Club internal
        // swapchain operations).  Capturing queues from non-game swapchains
        // corrupts g_SwapchainQueue and causes ERR_GFX_STATE.  The inline and
        // global hooks already capture the queue for legitimate game/FG calls.
    }

    return hr;
}

void STDMETHODCALLTYPE DetourCreateSampler(ID3D12Device* pDevice, const D3D12_SAMPLER_DESC* pDesc,
                                           D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    if (!pDesc || !oCreateSampler) {
        if (oCreateSampler)
            oCreateSampler(pDevice, pDesc, DestDescriptor);
        return;
    }

    D3D12_SAMPLER_DESC modifiedDesc = *pDesc;
    ApplyDX12SamplerOverridesCallback(&modifiedDesc);

    oCreateSampler(pDevice, &modifiedDesc, DestDescriptor);
}

HRESULT WINAPI DetourSerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* pRootSignature,
                                            D3D_ROOT_SIGNATURE_VERSION Version, ID3DBlob** ppBlob,
                                            ID3DBlob** ppErrorBlob) {
    if (!pRootSignature || !ppBlob) {
        if (oSerializeRootSignature)
            return oSerializeRootSignature(pRootSignature, Version, ppBlob, ppErrorBlob);
        return E_INVALIDARG;
    }

    HookLog("DetourSerializeRootSignature: CALLED, NumStaticSamplers=%u", pRootSignature->NumStaticSamplers);

    if (pRootSignature->NumStaticSamplers > 0 && pRootSignature->pStaticSamplers) {
        // Clone the descriptor with modified samplers
        D3D12_ROOT_SIGNATURE_DESC modified = *pRootSignature;
        std::vector<D3D12_STATIC_SAMPLER_DESC> modifiedSamplers(
            pRootSignature->pStaticSamplers, pRootSignature->pStaticSamplers + pRootSignature->NumStaticSamplers);

        bool anyModified = false;
        for (auto& sampler : modifiedSamplers) {
            if (RootSignatureParser::ApplyStaticSamplerOverrides(sampler)) {
                anyModified = true;
            }
        }

        if (anyModified) {
            HookLog(
                "DetourSerializeRootSignature: Modified %zu static samplers for "
                "AF/mip bias",
                modifiedSamplers.size());
            modified.pStaticSamplers = modifiedSamplers.data();
            if (oSerializeRootSignature)
                return oSerializeRootSignature(&modified, Version, ppBlob, ppErrorBlob);
        }
    }

    if (oSerializeRootSignature)
        return oSerializeRootSignature(pRootSignature, Version, ppBlob, ppErrorBlob);
    return E_FAIL;
}

HRESULT WINAPI DetourSerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* pRootSignature,
                                                     ID3DBlob** ppBlob, ID3DBlob** ppErrorBlob) {
    if (!pRootSignature || !ppBlob) {
        if (oSerializeVersionedRootSignature)
            return oSerializeVersionedRootSignature(pRootSignature, ppBlob, ppErrorBlob);
        return E_INVALIDARG;
    }

    uint32_t numStaticSamplers = 0;
    if (pRootSignature->Version == D3D_ROOT_SIGNATURE_VERSION_1)
        numStaticSamplers = pRootSignature->Desc_1_0.NumStaticSamplers;
    HookLog(
        "DetourSerializeVersionedRootSignature: CALLED, Version=%u, "
        "NumStaticSamplers=%u",
        pRootSignature->Version, numStaticSamplers);

    if (pRootSignature->Version == D3D_ROOT_SIGNATURE_VERSION_1) {
        const D3D12_ROOT_SIGNATURE_DESC* pDesc = &pRootSignature->Desc_1_0;

        if (pDesc->NumStaticSamplers > 0 && pDesc->pStaticSamplers) {
            D3D12_VERSIONED_ROOT_SIGNATURE_DESC modified = *pRootSignature;
            D3D12_ROOT_SIGNATURE_DESC modifiedDesc = *pDesc;

            std::vector<D3D12_STATIC_SAMPLER_DESC> modifiedSamplers(pDesc->pStaticSamplers,
                                                                    pDesc->pStaticSamplers + pDesc->NumStaticSamplers);

            bool anyModified = false;
            for (auto& sampler : modifiedSamplers) {
                if (RootSignatureParser::ApplyStaticSamplerOverrides(sampler)) {
                    anyModified = true;
                }
            }

            if (anyModified) {
                HookLog(
                    "DetourSerializeVersionedRootSignature: Modified %zu static "
                    "samplers (v1.0)",
                    modifiedSamplers.size());
                modifiedDesc.pStaticSamplers = modifiedSamplers.data();
                modified.Desc_1_0 = modifiedDesc;
                if (oSerializeVersionedRootSignature)
                    return oSerializeVersionedRootSignature(&modified, ppBlob, ppErrorBlob);
            }
        }
    }

    if (oSerializeVersionedRootSignature)
        return oSerializeVersionedRootSignature(pRootSignature, ppBlob, ppErrorBlob);
    return E_FAIL;
}

HRESULT STDMETHODCALLTYPE DetourCreateCommittedResource(ID3D12Device* device,
                                                        const D3D12_HEAP_PROPERTIES* pHeapProperties,
                                                        D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc,
                                                        D3D12_RESOURCE_STATES InitialResourceState,
                                                        const D3D12_CLEAR_VALUE* pOptimizedClearValue,
                                                        REFIID riidResource, void** ppvResource) {
    if (oCreateCommittedResource)
        return oCreateCommittedResource(device, pHeapProperties, HeapFlags, pDesc, InitialResourceState,
                                        pOptimizedClearValue, riidResource, ppvResource);
    return E_FAIL;
}

void DX12Hook::Shutdown() {
    CleanupResources();
    CleanupOverlay();
    CleanupRTVs();
    {
        std::lock_guard<std::recursive_mutex> lock(g_DeviceQueuesMutex);
        for (auto& pair : g_DeviceQueues)
            if (pair.second)
                pair.second->Release();
        g_DeviceQueues.clear();
    }
    if (g_SwapchainQueue) {
        g_SwapchainQueue->Release();
        g_SwapchainQueue = nullptr;
    }
    if (g_CommandQueue.load()) {
        g_CommandQueue.load()->Release();
        g_CommandQueue.store(nullptr);
    }
    {
        std::lock_guard<std::recursive_mutex> lock(g_ExecuteCommandListsHookStateMutex);
        g_ExecuteCommandListsOriginalByVTable.clear();
        oExecuteCommandLists = nullptr;
    }
    g_LastExecuteCommandListsVTable.store(nullptr, std::memory_order_release);
    g_LastExecuteCommandListsOriginal.store(nullptr, std::memory_order_release);
    if (g_Device.load()) {
        g_Device.load()->Release();
        g_Device.store(nullptr);
    }
    // g_LastSwapChain is a raw (non-AddRef'd) pointer — do NOT Release
    if (g_LastSwapChain) {
        g_LastSwapChain = nullptr;
    }
    if (g_SharedCaptureD3D12.IsActive())
        g_SharedCaptureD3D12.Reset();
    g_IPCReady = false;
}

void DX12Hook::OnHostDisconnect() {
    g_IPCReady = false;
}
void DX12Hook::TrackResource(IUnknown* res) {
    if (!res)
        return;
    std::lock_guard<std::recursive_mutex> lock(resourceMutex);
    res->AddRef();
    trackedResources.push_back(res);
}
void DX12Hook::CleanupResources() {
    std::lock_guard<std::recursive_mutex> lock(resourceMutex);
    for (auto* res : trackedResources)
        if (res)
            res->Release();
    trackedResources.clear();
}

bool DX12Hook::IsRealFrame() const {
    return g_FGCompat.IsCurrentFrameReal();
}

void DX12Hook::ClassifyFrame(int commandListCount) {
    g_FGCompat.RecordFrame(commandListCount);
}

// FIXED: Clean up the global hook instance if allocated
DWORD WINAPI UnloadThread(LPVOID lpParam) {
    Sleep(200);
    if (g_dx12HookInstance) {
        g_dx12HookInstance->Shutdown();
        delete g_dx12HookInstance;
        g_dx12HookInstance = nullptr;
    }
    return 0;
}
