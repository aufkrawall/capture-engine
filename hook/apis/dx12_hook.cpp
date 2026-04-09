#include <combaseapi.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <unknwn.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
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
#include "../common/capture_pacing.h"
#include "../common/custom_overlay_dx12.h"
#include "../common/fg_detection.h"
#include "../common/hook_common.h"
#include "../common/input_manager.h"
#include "../common/overlay_adapter.h"
#include "../common/overlay_compat.h"
#include "../common/performance_metrics.h"
#include "../common/streamline_compat.h"

#include "../common/fps_limiter.h"
#include "../common/freeze_watchdog.h"
#include "../common/perf_logger.h"

// Forward declarations for screenshot functions
bool SaveDX12TextureAsBMP(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backBuffer,
                          const char* outputPath);
bool SaveDX12TextureAsHDR(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* backBuffer, bool isPQ,
                          const char* outputPath);
static bool IsActualFrameGenerationActive();
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
#include "streamline_hook.h"

#include "../common/custom_overlay.h"
#include "../common/dx12_overlay_policy.h"
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

// Bypass trampoline for ECL that skips Streamline's hook.
// When SL FG is active, overlay ECLs are submitted through this trampoline
// so SL's internal frame tracking doesn't see our extra command lists.
static std::atomic<ExecuteCommandListsPtr> g_SLBypassECL{nullptr};

// Real D3D12 ExecuteCommandLists function pointer obtained by probing a
// COMPUTE queue (which SL doesn't hook for FG).  Used to bypass SL's
// vtable ECL hook when submitting overlay command lists.
static std::atomic<ExecuteCommandListsPtr> g_RealD3D12ECL{nullptr};

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

static void PostSLOverlayRender(IDXGISwapChain* pSwapChain);

// Post-SL overlay rendering state.  Controls whether the re-entrant Present
// callback should actually render or skip (e.g. during FG cooldown / resize).
static std::atomic<bool> g_PostSLOverlayActive{false};
static std::atomic<int> g_PostSLCooldownRemaining{0};
static std::atomic<ULONGLONG> g_LastProcessFrameTickMs{0};
static std::atomic<bool> g_PostSLSyntheticStartupActivationPending{false};
static std::atomic<bool> g_PostSLSyntheticStartupTakeoverLogged{false};
static std::atomic<uint32_t> g_PostSLLifecycleEpoch{0};

// Set to true when PostSLOverlayRender has confirmed it can render (i.e., re-entrant
// Present calls are actually happening).  In games like GTA V, SL FG bypasses our
// vtable hook for interpolated frames, so PostSL never fires.  When this is false,
// pre-SL rendering is NOT suppressed, allowing the overlay to render before SL.
static std::atomic<bool> g_PostSLConfirmedRendering{false};

// Counts Present calls where PostSL was expected but didn't fire.
// ProcessFrame increments this; PostSLOverlayRender resets it to 0.
// When it exceeds kPostSLStallThreshold (5), pre-SL rendering is allowed
// as a fallback for "FG suspension" (SL FG nominally on, but not generating
// re-entrant Present calls — e.g., game menu/pause).
//
// CONTEXT: During DLSS FG, SL generates re-entrant Present calls from worker
// threads for each interpolated frame.  Our PostSL callback renders the overlay
// in these re-entrant calls.  But when the game enters a menu or pause state,
// SL may stop generating FG frames while g_StreamlineFGRunning stays true
// (slDLSSGSetOptions isn't called with mode=0).  In this state:
//   - Pre-SL rendering is suppressed (g_StreamlineFGRunning = true)
//   - PostSL never fires (no re-entrant Present from SL)
//   - Result: overlay gap with BOTH paths blocked
//
// The stall counter detects this gap and temporarily allows pre-SL rendering.
// When PostSL fires again (FG resumes), it resets the counter and takes over.
//
// COMPATIBILITY: Tested in GTA V Enhanced (menu pauses FG) and Talos Principle
// Reawakened (continuous FG).  The threshold of 5 frames ensures PostSL has
// enough time to fire during normal FG before pre-SL takes over.
static std::atomic<int> g_PostSLStallCounter{0};

// Counts consecutive successful PostSL renders since the last FG transition.
// Incremented by PostSLOverlayRender, reset to 0 by FG transition handler.
// The stall fallback is only enabled once this exceeds kPostSLWarmupThreshold,
// preventing pre-SL rendering during FG warmup (SL pipeline still initializing).
//
// PROBLEM: After FG OFF→ON (menu close), SL generates ONE re-entrant Present
// immediately (setting PostSLConfirmed=true), then stalls briefly while the FG
// pipeline warms up.  Without this counter, the stall fallback fires during
// warmup and renders on origGame → DEVICE_HUNG (cross-queue backbuffer access).
static std::atomic<int> g_PostSLStableFrameCount{0};

// Flag to reset the queue-change heuristic's internal state.  Set during FG
// transitions so that the heuristic starts fresh afterward (re-captures the
// "initial queue" from the next 5 frames).  Without this, SL's leftover queue
// persists in the heuristic's state after FG OFF → immediate false FSR FG
// detection → wrong queue selection → DEVICE_HUNG.
static std::atomic<bool> g_ResetQueueChangeHeuristic{false};

// Grace counter after SL FG turns OFF.  Set by the outer block, decremented each
// frame in CanUseFSRFGHeuristics().  While active, the queue-change heuristic is
// suppressed — the queue naturally switches from SL's internal queue back to
// origGame, which would otherwise false-positive as FSR FG.
static std::atomic<int> g_SLOffHeuristicGrace{0};

// Grace counter after Streamline FG turns OFF. Set when PostSL/Streamline
// ownership tears down, and consumed by ProcessFrame swapchain-change handling
// to keep the first replacement swapchain on the guarded transition path.
static std::atomic<int> g_SLOffSwapchainReinitGrace{0};

// Reset flag for per-reinit submit diagnostic counter.
static std::atomic<bool> g_ResetReinitSubmitCounter{false};

// Outer SL transition epoch — incremented each time the outer FG state management
// block processes an SL FG ON/OFF transition.  The inner transition handler checks
// this to avoid redundant transition processing (double cooldowns, duplicate drain).
static std::atomic<uint32_t> g_OuterSLTransitionEpoch{0};

// Last Streamline FG signal observed by the outer ProcessFrame transition block.
// Kept at file scope so direct Streamline teardown paths can synchronize it when
// they invalidate overlay state before ProcessFrame reaches the outer tracker.
static std::atomic<bool> g_OuterTrackedSLFGRunning{false};

// Locked queue for PostSL overlay — stays on the first successful queue
// (game's render queue) instead of following SL's FG worker queue changes.
// Reset when PostSL rendering is disabled (FG transition off).
static ID3D12CommandQueue* g_PostSLLockedQueue = nullptr;

// Tracks whether FSR FG was ever active during this session.
// Once set, origGame is assumed corrupted (NVIDIA driver internal state broken
// after FSR FG phase) and PostSL uses g_CommandQueue (SL's wrapper) instead.
static bool g_HadFSRFGPhase = false;

// After FSR→DLSS→OFF: the swapchain's backbuffers have indeterminate GPU
// resource state from the FG pipeline teardown.  Direct rendering with
// explicit PRESENT→RENDER_TARGET barriers causes DEVICE_REMOVED when the
// barrier's StateBefore doesn't match the actual backbuffer state.  The
// offscreen compositing path (CopyTextureRegion + implicit state promotion)
// avoids ALL explicit barriers on the backbuffer, making it safe regardless
// of actual state.  Cleared on clean swapchain transition (non-FG).
static bool g_NeedOffscreenOverlayAfterPostFSRNonFG = false;

// GPU drain: flush all in-flight GPU work before first overlay render after
// FSR→DLSS transition.  SL's FG pipeline may have concurrent backbuffer
// access that causes DEVICE_HUNG if we draw simultaneously.
static bool g_NeedGPUDrainBeforeRender = false;
static ID3D12Fence* g_DrainFence = nullptr;
static HANDLE g_DrainEvent = nullptr;
static UINT64 g_DrainFenceValue = 0;

// PostSL ECL diagnostic counter — reset on each PostSL reactivation epoch.
std::atomic<int> g_PostSLECLDiagCount{0};

// Post-FSR graduated probe system: after FSR→DLSS transition, incrementally test
// what rendering operations are safe before committing to full overlay render.
static int g_PostFSRProbeLevel = 0;  // 0=scratch, 1=reserved, 2=offscreen-copy-only, 3=full allowed
static int g_PostFSRProbeFrames = 0;
static constexpr int kPostFSRProbeFramesPerLevel = 3;
static bool g_PostFSRDescFreeRecreated = false;

// Dedicated queue created specifically for PostSL overlay rendering.
// After FG transitions (FSR→DLSS), the NVIDIA driver's internal state for
// the game's original queue can become corrupted (null pointer dereference
// in nvwgf2umx during ECL).  A fresh queue bypasses this corruption.
// Created once on first PostSL need, re-created after FG transitions.
static ID3D12CommandQueue* g_PostSLDedicatedQueue = nullptr;

// Last queue that PostSL successfully submitted to without DEVICE_REMOVED.
// Survives FG transitions — used as preferred queue when PostSL re-activates
// after FSR→DLSS switch (where g_CommandQueue or scQueue might be wrong).
// AddRef'd to prevent use-after-free when g_CommandQueue is updated.
static ID3D12CommandQueue* g_PostSLLastWorkingQueue = nullptr;

static void SetPostSLLastWorkingQueue(ID3D12CommandQueue* queue) {
    if (queue == g_PostSLLastWorkingQueue)
        return;
    if (queue)
        queue->AddRef();
    if (g_PostSLLastWorkingQueue)
        g_PostSLLastWorkingQueue->Release();
    g_PostSLLastWorkingQueue = queue;
}

// File-scope scene transition cooldown.  Set by ProcessFrame when a large
// frametime gap is detected during FG, checked by PostSLOverlayRender to
// suppress overlay during scene loads.
static std::atomic<int> g_SceneTransitionCooldown{0};

// Captures ProcessFrame's resolved gameQueue right before FG transitions ON.
// When PostSL activates after FG, g_CommandQueue may have been polluted by
// SL's internal FG queues and g_SwapchainQueue may be null (e.g., Talos).
// This provides a reliable fallback to the game's real queue.
static ID3D12CommandQueue* g_PreFGGameQueue = nullptr;

// The game's very first command queue, captured before any FG ever activates.
// During FG transitions, ALL queue sources can get polluted by SL/FSR internal
// queues (g_CommandQueue from ECL hook, g_SwapchainQueue from CreateSwapChain
// called by SL).  The original queue is the only one guaranteed to be the game's
// own queue and works reliably for PostSL overlay during DLSS FG.
static ID3D12CommandQueue* g_OriginalGameQueue = nullptr;

// File-scope FG transition cooldown.  Counts down frames after any FG mode
// change (on/off, FSR→DLSS, SL signal change).  While >0, overlay reinit is
// suppressed so that SL / FSR FG can finish initializing without interference.
// Set by ProcessFrame's FG transition detection; checked by swapchain-change
// invalidation to prevent premature overlay reinit during FG mode switches.
static int g_FGTransitionCooldown = 0;

// Counts frames since FG was last active.  When a game switches FG modes
// (e.g., FSR FG → DLSS FG), FG heuristics may go inactive for many frames
// before the swapchain actually changes.  A simple "was active last frame"
// flag misses cases where the gap is >1 frame.  This counter lets the
// swapchain-change logic detect that FG was active recently (within a window)
// even if several "inactive" frames have elapsed.
static int g_FramesSinceFGActive = 9999;

class DX12DescFreeBackend : public CustomOverlay::RendererBackend {
public:
    ~DX12DescFreeBackend() override {
        Shutdown();
    }

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

        size_t dataSize = (size_t)fontWidth * fontHeight * 4;  // RGBA8

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

        HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                      nullptr, IID_PPV_ARGS(&fontBuffer_));
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
        HookLogImportant("DescFree: font buffer ready (%dx%d, %zu bytes, gpu=0x%llX)", fontWidth, fontHeight, dataSize,
                         (unsigned long long)fontGpuAddr_);
        return true;
    }

    void Render(const std::vector<CustomOverlay::DrawVertex>& vertices, const std::vector<uint16_t>& indices,
                const std::vector<CustomOverlay::DrawCommand>& commands, int vpW, int vpH) override {
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
        float constants[6] = {(float)vpW,     (float)vpH,        (float)hdrMode,
                              paperWhiteNits, (float)fontWidth_, (float)fontHeight_};
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
        // During process termination, D3D12/NVIDIA driver may be partially torn down.
        // Skip GPU resource cleanup to avoid access violations in driver code.
        if (IsProcessTerminating())
            return;
        for (int i = 0; i < kPoolSize; i++) {
            if (vb_[i]) {
                vb_[i]->Unmap(0, nullptr);
                vb_[i]->Release();
                vb_[i] = nullptr;
            }
            if (ib_[i]) {
                ib_[i]->Unmap(0, nullptr);
                ib_[i]->Release();
                ib_[i] = nullptr;
            }
            vbPtr_[i] = nullptr;
            ibPtr_[i] = nullptr;
            vbSize_[i] = 0;
            ibSize_[i] = 0;
        }
        if (fontBuffer_) {
            fontBuffer_->Release();
            fontBuffer_ = nullptr;
        }
        if (psoTextured_) {
            psoTextured_->Release();
            psoTextured_ = nullptr;
        }
        if (psoSolid_) {
            psoSolid_->Release();
            psoSolid_ = nullptr;
        }
        if (rootSig_) {
            rootSig_->Release();
            rootSig_ = nullptr;
        }
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
        HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
        if (FAILED(hr)) {
            HookLogImportant("DescFree: SerializeRootSignature failed hr=0x%08X", hr);
            if (err)
                err->Release();
            return false;
        }
        hr = device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSig_));
        blob->Release();
        if (err)
            err->Release();
        if (FAILED(hr)) {
            HookLogImportant("DescFree: CreateRootSignature failed hr=0x%08X", hr);
            return false;
        }
        return true;
    }

    bool CreatePSOs() {
        D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
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
                &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vb_[i]));
            if (FAILED(hr))
                return false;
            vb_[i]->Map(0, &readRange, &vbPtr_[i]);
            vbSize_[i] = kInitVBBytes;

            rd.Width = kInitIBBytes;
            hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                  nullptr, IID_PPV_ARGS(&ib_[i]));
            if (FAILED(hr))
                return false;
            ib_[i]->Map(0, &readRange, &ibPtr_[i]);
            ibSize_[i] = kInitIBBytes;
        }
        return true;
    }

    bool ResizeBuffer(ID3D12Resource*& buf, void*& ptr, size_t& curSize, size_t needed) {
        size_t newSize = curSize;
        while (newSize < needed)
            newSize *= 2;

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

        HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                      nullptr, IID_PPV_ARGS(&buf));
        if (FAILED(hr))
            return false;

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

    // The device used to create sync resources (allocators, command list, fence).
    // Must match the submission queue's device — cross-device submission = DEVICE_REMOVED.
    ID3D12Device* syncDevice = nullptr;

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
        for (auto* rtv : d3d11RTVs)
            if (rtv)
                rtv->Release();
        d3d11RTVs.clear();
        for (auto* res : d3d11WrappedBBs)
            if (res)
                res->Release();
        d3d11WrappedBBs.clear();
        if (d3d11on12) {
            d3d11on12->Release();
            d3d11on12 = nullptr;
        }
        if (d3d11on12Context) {
            d3d11on12Context->Release();
            d3d11on12Context = nullptr;
        }
        if (d3d11on12Device) {
            d3d11on12Device->Release();
            d3d11on12Device = nullptr;
        }
        d3d11on12Init = false;
    }
};

static DX12OverlayState g_State;
static SharedCaptureD3D12 g_SharedCaptureD3D12;
static OverlayAdapter g_D3D11On12Adapter;
// Separate overlay adapter for D3D11On12 rendering during Streamline FG.
// Uses the DX11 backend via D3D11On12 bridge to properly manage cross-queue
// resource transitions, which SL's FG pipeline can track.
static OverlayAdapter g_SLFGAdapter;

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
static std::atomic<int> g_AuthoritativeFSRRealFrameOnlyStreak{0};

static void ResetAuthoritativeFSRRealFrameOnlyStreak() {
    g_AuthoritativeFSRRealFrameOnlyStreak.store(0, std::memory_order_release);
}

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

// Track the game's Present thread ID. Captured from the first non-FG Present call.
// During SL FG, only this thread should run pre-SL overlay rendering.
// SL's FG worker threads call Present from different threads — they must NOT
// run ProcessFrame overlay rendering (wrong timing, wrong queue).
static std::atomic<DWORD> g_GamePresentThreadId{0};

// SL's COM wrapper queue for FG — captured in ECL detour when SL FG is active
// and the ECL is from a queue that's not origGame/scQueue/primaryQ.
// This queue routes through SL's ECL interception to the correct internal queue.
static std::atomic<ID3D12CommandQueue*> g_SLWrapperQueue{nullptr};

// Sticky wrapper queue for the current PostSL reactivation epoch.
// After FSR->DLSS, Streamline can churn through multiple wrapper queues within
// a few frames. Keep the post-FSR offscreen path on the first wrapper that was
// selected for the epoch instead of following later wrapper churn.
static ID3D12CommandQueue* g_PostSLPinnedSLWrapperQueue = nullptr;

// Real D3D12 queue behind SL's wrapper — captured from ECL detour when PostSL
// submits through SL's COM wrapper. Used for direct submission to bypass SL's
// metadata wrapping that causes cumulative DEVICE_REMOVED.
//
// DISCOVERY: Submitting command lists through SL's COM wrapper queue
// (g_SLWrapperQueue->ExecuteCommandLists) adds internal SL metadata per ECL.
// This metadata accumulates and causes DEVICE_REMOVED after ~500-2000 frames.
// The damage rate depends on rendering frequency: 1/10 rate = no crash (damage
// drains), full rate = crash at ~500 frames.  Empty ECLs through the wrapper
// are safe (damage requires actual rendering content).
//
// FIX: Capture the real D3D12 queue behind SL's wrapper and submit directly
// via g_RealD3D12ECL(realQueue, ...).  This bypasses SL's internal tracking
// entirely.  Proven stable for 16,798+ frames during active DLSS FG.
//
// CAPTURE MECHANISM: When PostSL submits through SL's wrapper (bootstrap frame),
// our ECL detour sees the real D3D12 queue as pThis (SL's wrapper dispatches
// to it).  s_insidePostSLOverlayECL=true during bootstrap marks the capture.
//
// CAUTION: If SL recreates internal queues, this pointer becomes stale.
// Currently no known trigger for SL queue recreation during a session.
static std::atomic<ID3D12CommandQueue*> g_RealQueueBehindSLWrapper{nullptr};
static std::atomic<bool> g_PostSLCallbackExecutionEnabled{false};
static std::atomic<uint32_t> g_PostSLCallbackInFlight{0};
static std::atomic<bool> g_PostSLDeferredQueueCleanupPending{false};

static void PostSLOverlayRenderGated(IDXGISwapChain* pSwapChain);
static void ClearPostSLQueues(const char* reason);

static void SetPostSLCallbackInstalled(bool installed, const char* reason) {
    if (installed) {
        g_PostSLCallbackExecutionEnabled.store(true, std::memory_order_release);
        if (DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) != &PostSLOverlayRenderGated) {
            DXGIShared::g_PostSLOverlayRenderCallback.store(&PostSLOverlayRenderGated, std::memory_order_release);
            HookLogImportant("%s — installed gated PostSL callback", reason);
        }
        return;
    }

    g_PostSLCallbackExecutionEnabled.store(false, std::memory_order_release);
    if (DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) != nullptr) {
        DXGIShared::g_PostSLOverlayRenderCallback.store(nullptr, std::memory_order_release);
        HookLogImportant("%s — disabled PostSL callback", reason);
    }
}

static void WaitForInFlightPostSLCallbacks(const char* reason) {
    for (int spin = 0; spin < 200; ++spin) {
        uint32_t inFlight = g_PostSLCallbackInFlight.load(std::memory_order_acquire);
        if (inFlight == 0) {
            return;
        }

        if (spin == 0 || spin == 10 || spin == 50) {
            HookLogImportant("%s — waiting for %u in-flight PostSL callback(s)", reason, inFlight);
        }
        Sleep(1);
    }

    uint32_t remaining = g_PostSLCallbackInFlight.load(std::memory_order_acquire);
    if (remaining != 0) {
        HookLogImportant("%s — timed out waiting for %u in-flight PostSL callback(s)", reason, remaining);
    }
}

static void WaitForOverlayGpuIdle(const char* reason) {
    if (!g_State.fence || g_State.currentFenceValue == 0) {
        return;
    }

    const UINT64 lastVal = g_State.currentFenceValue;
    HANDLE drainEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!drainEvent) {
        return;
    }

    HRESULT drainHr = g_State.fence->SetEventOnCompletion(lastVal, drainEvent);
    if (SUCCEEDED(drainHr)) {
        DWORD waitResult = WaitForSingleObject(drainEvent, 200);
        HookLogImportant("%s — drained overlay GPU work (fenceVal=%llu wait=%u)", reason, (unsigned long long)lastVal,
                         waitResult);
    } else {
        HookLogImportant("%s — fence drain failed hr=0x%08X", reason, drainHr);
    }
    CloseHandle(drainEvent);
}

static void CleanupDeferredPostSLQueuesIfSafe(const char* reason);
static void RealignInactiveCommandQueueToSwapchainQueue(const char* reason);
static std::atomic<ID3D12CommandQueue*> g_DeferredCommandQueueRelease{nullptr};
static std::atomic<ID3D12CommandQueue*> g_DeferredPostSLLockedQueueRelease{nullptr};
static std::atomic<ULONGLONG> g_PostSLRecentTeardownActivityUntilMs{0};
static void WaitForOverlayGpuIdle(const char* reason);

static void ResetPostSLLifecycleForTransition(const char* reason, bool clearRealQueueBehindSLWrapper,
                                              bool deferQueueReleaseUntilCallbacksDrain = false);

static void ClearPostSLPinnedSLWrapperQueue(const char* reason) {
    ID3D12CommandQueue* oldPinnedWrapperQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        oldPinnedWrapperQueue = g_PostSLPinnedSLWrapperQueue;
        g_PostSLPinnedSLWrapperQueue = nullptr;
    }

    if (oldPinnedWrapperQueue) {
        HookLogImportant("%s — releasing PostSL pinned SL wrapper queue %p", reason, oldPinnedWrapperQueue);
        oldPinnedWrapperQueue->Release();
    }
}

static void DetachPostSLQueuesLocked(ID3D12CommandQueue** lockedQueueOut, ID3D12CommandQueue** dedicatedQueueOut) {
    if (lockedQueueOut) {
        *lockedQueueOut = nullptr;
    }
    if (dedicatedQueueOut) {
        *dedicatedQueueOut = nullptr;
    }

    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    if (lockedQueueOut) {
        *lockedQueueOut = g_PostSLLockedQueue;
    }
    if (dedicatedQueueOut) {
        *dedicatedQueueOut = g_PostSLDedicatedQueue;
    }
    g_PostSLLockedQueue = nullptr;
    g_PostSLDedicatedQueue = nullptr;
}

static void ReleaseDetachedPostSLQueues(const char* reason, ID3D12CommandQueue* lockedQueue,
                                        ID3D12CommandQueue* dedicatedQueue) {
    if (lockedQueue) {
        HookLogImportant("%s — releasing PostSL locked queue %p", reason, lockedQueue);
        lockedQueue->Release();
    }

    if (dedicatedQueue) {
        HookLogImportant("%s — releasing PostSL dedicated queue %p", reason, dedicatedQueue);
        dedicatedQueue->Release();
    }
}

static void ClearPostSLQueues(const char* reason) {
    ID3D12CommandQueue* oldLockedQueue = nullptr;
    ID3D12CommandQueue* oldDedicatedQueue = nullptr;
    DetachPostSLQueuesLocked(&oldLockedQueue, &oldDedicatedQueue);
    ReleaseDetachedPostSLQueues(reason, oldLockedQueue, oldDedicatedQueue);
}

static void CleanupDeferredPostSLQueuesIfSafe(const char* reason) {
    ID3D12CommandQueue* deferredLockedQueue =
        g_DeferredPostSLLockedQueueRelease.exchange(nullptr, std::memory_order_acq_rel);
    if (deferredLockedQueue) {
        HookLogImportant("%s - releasing deferred PostSL locked queue %p", reason, deferredLockedQueue);
        deferredLockedQueue->Release();
    }

    ID3D12CommandQueue* deferredCommandQueue =
        g_DeferredCommandQueueRelease.exchange(nullptr, std::memory_order_acq_rel);
    if (deferredCommandQueue) {
        HookLogImportant("%s - releasing deferred stale command queue %p", reason, deferredCommandQueue);
        deferredCommandQueue->Release();
    }

    if (!g_PostSLDeferredQueueCleanupPending.load(std::memory_order_acquire)) {
        return;
    }

    if (DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire)) {
        return;
    }

    if (g_PostSLCallbackInFlight.load(std::memory_order_acquire) != 0) {
        return;
    }

    if (!g_PostSLDeferredQueueCleanupPending.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    ID3D12CommandQueue* oldLockedQueue = nullptr;
    ID3D12CommandQueue* oldDedicatedQueue = nullptr;
    DetachPostSLQueuesLocked(&oldLockedQueue, &oldDedicatedQueue);

    if (oldLockedQueue) {
        ID3D12CommandQueue* previouslyDeferred =
            g_DeferredPostSLLockedQueueRelease.exchange(oldLockedQueue, std::memory_order_acq_rel);
        if (previouslyDeferred) {
            HookLogImportant("%s - releasing superseded deferred PostSL locked queue %p", reason, previouslyDeferred);
            previouslyDeferred->Release();
        }
        HookLogImportant("%s - deferred PostSL locked queue release %p", reason, oldLockedQueue);
    }
    if (oldDedicatedQueue) {
        HookLogImportant("%s — releasing PostSL dedicated queue %p", reason, oldDedicatedQueue);
        oldDedicatedQueue->Release();
    }

    RealignInactiveCommandQueueToSwapchainQueue(reason);
}

static void MarkPostSLRecentTeardownActivity(const char* reason, ID3D12CommandQueue* queue) {
    if (!queue) {
        return;
    }

    constexpr ULONGLONG kPostSLRecentTeardownActivityMs = 250;
    g_PostSLRecentTeardownActivityUntilMs.store(GetTickCount64() + kPostSLRecentTeardownActivityMs,
                                                std::memory_order_release);
    static std::atomic<int> s_postSLRecentTeardownLogCount{0};
    const int logCount = s_postSLRecentTeardownLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 10 || (logCount % 128) == 0) {
        HookLogImportant("%s - marking PostSL queue %p as recently active during Streamline teardown (%llums)", reason,
                         queue, (unsigned long long)kPostSLRecentTeardownActivityMs);
    }
}

static void ResetPostSLLifecycleForTransition(const char* reason, bool clearRealQueueBehindSLWrapper,
                                              bool deferQueueReleaseUntilCallbacksDrain) {
    g_PostSLLifecycleEpoch.fetch_add(1, std::memory_order_acq_rel);

    if (deferQueueReleaseUntilCallbacksDrain) {
        SetPostSLCallbackInstalled(false, reason);
        WaitForInFlightPostSLCallbacks(reason);
        WaitForOverlayGpuIdle(reason);
        g_PostSLDeferredQueueCleanupPending.store(true, std::memory_order_release);
    } else {
        g_PostSLDeferredQueueCleanupPending.store(false, std::memory_order_release);
        ClearPostSLQueues(reason);
    }

    ClearPostSLPinnedSLWrapperQueue(reason);

    if (clearRealQueueBehindSLWrapper) {
        ID3D12CommandQueue* oldRealQueue = g_RealQueueBehindSLWrapper.exchange(nullptr, std::memory_order_acq_rel);
        if (oldRealQueue) {
            HookLogImportant("%s — cleared cached real queue behind SL wrapper %p", reason, oldRealQueue);
        }
    }
}

// IPC ready flag
static bool g_IPCReady = false;

ID3D12Resource* g_DummyBackBuffer = nullptr;

// Swapchain queue - captured at swapchain creation time, preferred for overlay
// rendering to ensure barriers execute on the queue DXGI synchronises with.
static ID3D12CommandQueue* g_SwapchainQueue = nullptr;
static ULONGLONG g_SwapchainQueueCaptureTime = 0;  // GetTickCount64() when scQueue was last set

// True when swapchain was (re)created on a queue != origGame.  This means an
// FG runtime (FSR FG / DLSS FG) owns the swapchain and its queue.  ANY GPU
// work we submit on that queue (ECLs, resource priming, even allocator/fence
// creation callbacks) can break the FG runtime's internal fence sync.
// Cleared when swapchain recreated back on origGame or FG heuristic is None
// for a sustained period.
static bool g_FGRuntimeOwnsSwapchain = false;
static ULONGLONG g_FGRuntimeOwnsSwapchainSince = 0;

static void RealignInactiveCommandQueueToSwapchainQueue(const char* reason) {
    ID3D12CommandQueue* oldCommandQueue = nullptr;
    ID3D12CommandQueue* swapchainQueue = nullptr;
    ID3D12CommandQueue* originalGameQueue = nullptr;
    bool realignedCommandQueue = false;
    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        swapchainQueue = g_SwapchainQueue;
        originalGameQueue = g_OriginalGameQueue;
        ID3D12CommandQueue* currentCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
        bool actualFGActive = IsActualFrameGenerationActive();
        bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        if (ce::dx12_overlay_policy::ShouldRealignInactiveCommandQueueToSwapchainQueue(
                actualFGActive, streamlineFGRunning, swapchainQueue != nullptr, originalGameQueue != nullptr,
                currentCommandQueue != nullptr, currentCommandQueue == swapchainQueue,
                currentCommandQueue == originalGameQueue,
                currentCommandQueue == g_PrimaryGameQueue.load(std::memory_order_acquire))) {
            oldCommandQueue = currentCommandQueue;
            g_CommandQueue.store(swapchainQueue, std::memory_order_release);
            swapchainQueue->AddRef();
            realignedCommandQueue = true;
        }
    }

    if (realignedCommandQueue) {
        HookLogImportant("%s - realigned stale command queue %p -> swapchain queue %p (origGame=%p)", reason,
                         oldCommandQueue, swapchainQueue, originalGameQueue);
        if (oldCommandQueue) {
            ID3D12CommandQueue* previouslyDeferred =
                g_DeferredCommandQueueRelease.exchange(oldCommandQueue, std::memory_order_acq_rel);
            if (previouslyDeferred) {
                HookLogImportant("%s - releasing superseded deferred stale command queue %p", reason,
                                 previouslyDeferred);
                previouslyDeferred->Release();
            }
        }
    }
}

// Guard flag: skip queue capture during temp swapchain creation
static std::atomic<bool> g_CreatingTempSwapchain{false};

static bool IsFFXFrameGenerationModuleHandle(HMODULE moduleHandle) {
    if (!moduleHandle) {
        return false;
    }

    char modulePath[MAX_PATH] = {};
    if (GetModuleFileNameA(moduleHandle, modulePath, MAX_PATH) == 0) {
        return false;
    }

    return ce::overlay_compat::detail::ContainsInsensitive(modulePath, "amd_fidelityfx_framegeneration_dx12") ||
           ce::overlay_compat::detail::ContainsInsensitive(modulePath, "amd_fidelityfx_framegeneration_vk");
}

static bool IsCodeAddressFromFFXFrameGenerationModule(const void* codeAddress, char* modulePathOut = nullptr,
                                                      size_t modulePathOutCount = 0) {
    if (!codeAddress) {
        return false;
    }

    HMODULE callerModule = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(codeAddress), &callerModule) ||
        !callerModule) {
        return false;
    }

    if (modulePathOut && modulePathOutCount > 0) {
        modulePathOut[0] = '\0';
        GetModuleFileNameA(callerModule, modulePathOut, static_cast<DWORD>(modulePathOutCount));
    }

    return IsFFXFrameGenerationModuleHandle(callerModule);
}

static bool HasFFXFrameGenerationModuleInCurrentStack(char* modulePathOut = nullptr, size_t modulePathOutCount = 0) {
    constexpr USHORT kMaxFrames = 16;
    void* stackFrames[kMaxFrames] = {};
    const USHORT frameCount = CaptureStackBackTrace(0, kMaxFrames, stackFrames, nullptr);
    for (USHORT i = 0; i < frameCount; ++i) {
        char candidatePath[MAX_PATH] = {};
        if (IsCodeAddressFromFFXFrameGenerationModule(stackFrames[i], candidatePath, sizeof(candidatePath))) {
            if (modulePathOut && modulePathOutCount > 0) {
                strncpy_s(modulePathOut, modulePathOutCount, candidatePath, _TRUNCATE);
            }
            return true;
        }
    }

    return false;
}

static void ClearStaleStreamlineOwnershipForFSRTakeover(const void* callerAddress, bool runtimeOwnsSwapchain,
                                                        bool runtimeOwnershipJustActivated,
                                                        ID3D12CommandQueue* capturedQueue) {
    char callerModulePath[MAX_PATH] = {};
    bool callerFromFFXFGModule =
        IsCodeAddressFromFFXFrameGenerationModule(callerAddress, callerModulePath, sizeof(callerModulePath));
    if (!callerFromFFXFGModule && runtimeOwnsSwapchain) {
        callerFromFFXFGModule = HasFFXFrameGenerationModuleInCurrentStack(callerModulePath, sizeof(callerModulePath));
    }
    const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool streamlineStartupHandoffPending = DXGIShared::IsStreamlineStartupHandoffPending();
    const bool staleStreamlineOwnershipCandidate = runtimeOwnsSwapchain && streamlineFGRunning &&
                                                   !streamlineStartupHandoffPending && runtimeOwnershipJustActivated;
    if (!ce::dx12_overlay_policy::ShouldForceEndStreamlineOwnershipForSwapchainTakeover(
            runtimeOwnsSwapchain, callerFromFFXFGModule, streamlineFGRunning, streamlineStartupHandoffPending,
            runtimeOwnershipJustActivated)) {
        if (staleStreamlineOwnershipCandidate && !callerFromFFXFGModule) {
            static std::atomic<int> s_nonFfxTakeoverPreserveLogCount{0};
            const int logCount = s_nonFfxTakeoverPreserveLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 10) {
                HookLogImportant(
                    "DX12: Runtime-owned swapchain transition on %p while Streamline FG is active had no FFX FG "
                    "module in caller stack (caller=%s) — preserving existing Streamline/PostSL ownership",
                    capturedQueue, callerModulePath[0] ? callerModulePath : "unknown");
            }
        }
        return;
    }

    if (!callerModulePath[0] && runtimeOwnershipJustActivated) {
        strncpy_s(callerModulePath, sizeof(callerModulePath), "runtime-owned swapchain transition", _TRUNCATE);
    }

    g_FGCompat.SetFSRFGSupportPresent(true);
    g_FGCompat.SetFSRFGMultiplier(2);
    g_FGCompat.SetFSRFGActive(true);
    ResetAuthoritativeFSRRealFrameOnlyStreak();
    if (!g_HadFSRFGPhase) {
        g_HadFSRFGPhase = true;
        HookLogImportant("DX12: FFX swapchain takeover implies FSR FG history — latching post-FSR handoff state");
    }

    const bool staleStreamlineSignal = DXGIShared::g_StreamlineFGRunning.exchange(false, std::memory_order_acq_rel);
    g_FGCompat.SetStreamlineFGSignal(false);
    g_FGCompat.SetDLSSFGActive(false);
    g_PostSLOverlayActive.store(false, std::memory_order_release);
    SetPostSLCallbackInstalled(false, "DX12: FFX swapchain takeover");
    ResetPostSLLifecycleForTransition("DX12: FFX swapchain takeover", true, true);
    g_PostSLConfirmedRendering.store(false, std::memory_order_release);
    g_PostSLStallCounter.store(0, std::memory_order_release);
    g_PostSLStableFrameCount.store(0, std::memory_order_release);
    g_PostSLSyntheticStartupActivationPending.store(false, std::memory_order_release);
    g_PostSLSyntheticStartupTakeoverLogged.store(false, std::memory_order_release);
    StreamlineHook::OnAuthoritativeFFXTakeover();

    HookLogImportant("DX12: FFX swapchain takeover via %s (queue=%p, staleSL=%d) — cleared Streamline/PostSL ownership",
                     callerModulePath[0] ? callerModulePath : "unknown", capturedQueue, staleStreamlineSignal ? 1 : 0);
}

// LOCK HIERARCHY (MUST be acquired in this order to prevent deadlocks):
// 1. g_OverlayMutex (outermost - protects overlay state)
// 2. g_CommandQueueMutex (protects command queue pointer)
// 3. g_DX12CaptureMutex (innermost - protects capture state)
//
// Rule: When acquiring multiple locks, always acquire in order above.
//       Use std::lock_guard with std::adopt_lock when using try_lock().
static std::recursive_mutex g_OverlayMutex;
static std::recursive_mutex g_DX12CaptureMutex;

static OverlayConfig GetActiveDX12OverlayConfig(SharedMemoryLayout* shm) {
    OverlayConfig cfg{};
    cfg.captureIncludeOverlay = true;
    cfg.screenshotIncludeOverlay = true;
    if (shm) {
        cfg = shm->ReadOverlayConfig();
    }
    return cfg;
}

static bool ShouldUseConfirmedPostSLForOverlayIncludedWork(const OverlayConfig& cfg) {
    return cfg.showOverlay && g_PostSLOverlayActive.load(std::memory_order_acquire) &&
           g_PostSLConfirmedRendering.load(std::memory_order_acquire);
}

static void CompleteRequestedDX12Screenshot(SharedMemoryLayout* shm) {
    if (!shm)
        return;

    shm->runtimeState.cmdTakeScreenshot.store(false, std::memory_order_release);
    shm->runtimeState.ackScreenshotTaken.store(true, std::memory_order_release);
    shm->runtimeState.notificationType.store(1, std::memory_order_release);
    shm->runtimeState.notificationExpiry.store(GetTickCount64() + 3000ULL, std::memory_order_release);
}

static void CaptureRequestedDX12Screenshot(IDXGISwapChain3* sc3, SharedMemoryLayout* shm,
                                           ID3D12CommandQueue* queueOverride = nullptr) {
    if (!sc3 || !shm)
        return;

    ID3D12Device* dx12Device = g_Device.load();
    ID3D12CommandQueue* dx12Queue = queueOverride ? queueOverride : g_CommandQueue.load();
    if (dx12Device && dx12Queue) {
        UINT bbIdx = sc3->GetCurrentBackBufferIndex();
        ID3D12Resource* backBuffer = nullptr;
        if (SUCCEEDED(sc3->GetBuffer(bbIdx, IID_PPV_ARGS(&backBuffer)))) {
            D3D12_RESOURCE_DESC desc = backBuffer->GetDesc();
            bool isHDR =
                (desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM || desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
            if (isHDR) {
                bool isPQ = (desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM);
                std::string rawPath(shm->runtimeState.screenshotPath);
                rawPath += ".raw";
                SaveDX12TextureAsHDR(dx12Device, dx12Queue, backBuffer, isPQ, rawPath.c_str());
            } else {
                SaveDX12TextureAsBMP(dx12Device, dx12Queue, backBuffer, shm->runtimeState.screenshotPath);
            }
            backBuffer->Release();
        }
    }

    CompleteRequestedDX12Screenshot(shm);
}

static void PublishDX12CapturedFrame(IDXGISwapChain* pSwapChain, SharedMemoryLayout* shm,
                                     ID3D12CommandQueue* captureQueue, bool hasCurrentBackBufferIdx,
                                     UINT currentBackBufferIdx) {
    if (!pSwapChain || !shm || !captureQueue)
        return;
    if (shm->throttleCapture.load(std::memory_order_acquire))
        return;

    if (!g_SharedCaptureD3D12.IsActive())
        g_SharedCaptureD3D12.Initialize(g_Device.load(), pSwapChain);
    if (!g_SharedCaptureD3D12.IsActive())
        return;

    std::lock_guard<std::recursive_mutex> capLock(g_DX12CaptureMutex);
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

    if (!g_SharedCaptureD3D12.CaptureFrame(captureQueue, bbIdx))
        return;

    SharedFrameDescriptor desc;
    if (!g_SharedCaptureD3D12.GetCurrentFrame(&desc))
        return;

    for (UINT i = 0; i < SharedCaptureD3D12::kSharedTextureCount; ++i) {
        shm->SetSharedHandle(static_cast<int>(i), (uint64_t)g_SharedCaptureD3D12.GetSharedHandle((int)i));
    }
    shm->SetFenceShareHandle((uint64_t)g_SharedCaptureD3D12.GetFenceShareHandle());
    shm->SetWidth(desc.width);
    shm->SetHeight(desc.height);
    shm->SetFormat(desc.format);

    uint32_t wIdx = shm->frameRing.writeIndex.load(std::memory_order_acquire);
    uint32_t rIdx = shm->frameRing.readIndex.load(std::memory_order_acquire);
    if ((uint32_t)(wIdx - rIdx) < (uint32_t)FRAME_RING_SIZE) {
        FrameSlot& slot = shm->frameRing.slots[wIdx % FRAME_RING_SIZE];
        slot.fenceValue = desc.fenceValue;
        slot.timestamp = desc.presentTime;
        slot.frameIndex = desc.frameNumber;
        slot.textureIndex = desc.textureIndex;
        slot.sourcePid = GetCurrentProcessId();
        std::atomic_thread_fence(std::memory_order_release);
        slot.valid.store(1, std::memory_order_release);
        shm->frameRing.writeIndex.store(wIdx + 1, std::memory_order_release);
        DXGIShared::SetLatestSourceFrameIndex(desc.frameNumber);
        static uint64_t s_lastPublishLineageLogTick = 0;
        uint64_t nowTick = GetTickCount64();
        if (nowTick - s_lastPublishLineageLogTick >= 1000) {
            HookLog("DX12: Publish frame=%u ring=%u tex=%d fence=%llu ts=%llu bb=%u depth=%u", desc.frameNumber, wIdx,
                    desc.textureIndex, static_cast<unsigned long long>(desc.fenceValue),
                    static_cast<unsigned long long>(desc.presentTime), bbIdx, static_cast<unsigned>(wIdx - rIdx));
            s_lastPublishLineageLogTick = nowTick;
        }
    } else {
        shm->frameRing.droppedFrames.fetch_add(1, std::memory_order_relaxed);
    }
}

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

// Flag to indicate the current thread is inside a PostSL overlay ECL virtual call.
// When we submit our overlay ECL through SL's COM wrapper (virtual call on origGame),
// SL dispatches to the real D3D12 queue, which re-enters our ECL detour.  The detour
// must NOT update queue tracking (g_CommandQueue, g_SLWrapperQueue, etc.) for these
// overlay submissions — they'd pollute the game's queue state.
static thread_local bool s_insidePostSLOverlayECL = false;

static bool KnownDLSSFGModuleLoaded() {
    if (g_KnownDLSSFGModuleSeen.load(std::memory_order_acquire)) {
        return true;
    }

    constexpr const wchar_t* kKnownDLSSFGModules[] = {
        L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss.dll", L"sl.dlss_g.dll", L"nvngx_dlssg.dll", L"nvngx_dlss.dll",
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
    if (g_FGCompat.IsFSRFGApiActive()) {
        if (blockedReason) {
            *blockedReason = "authoritative FSR FG state is already active";
        }
        return false;
    }

    // Block when Streamline FG is running — SL creates internal queues that
    // trigger queue-change heuristics.  Without this check, enabling DLSS FG
    // causes false FSR FG detection (SL's queue ≠ origGame → "queue change"
    // heuristic fires → pre-SL renders on wrong queue → DEVICE_HUNG).
    if (DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire)) {
        if (blockedReason) {
            *blockedReason = "Streamline FG is running (queue changes are from SL, not FSR)";
        }
        return false;
    }

    // Block during grace period after SL FG turns OFF.  The queue naturally
    // changes from SL's internal queue back to origGame — this must not be
    // misinterpreted as FSR FG.  The heuristic runs BEFORE the outer block in
    // ProcessFrame, so g_StreamlineFGRunning alone can't prevent the false
    // positive on the same frame SL OFF fires.
    // NOTE: Do NOT decrement here — this function is called per-ECL (thousands/sec).
    // The counter is decremented once per ProcessFrame in the queue-change heuristic.
    if (g_SLOffHeuristicGrace.load(std::memory_order_acquire) > 0) {
        if (blockedReason) {
            *blockedReason = "SL FG just turned OFF (grace period)";
        }
        return false;
    }

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
static StartupOverlayFirstDrawProbeStage s_startupOverlayFirstDrawProbeStage = StartupOverlayFirstDrawProbeStage::kNone;
static ULONGLONG s_startupOverlayActivationStageMs = 0;
static ULONGLONG s_startupOverlaySyncInitMs = 0;
static ULONGLONG s_startupOverlayResourcePrimeMs = 0;
static ULONGLONG s_startupOverlayFirstDrawProbeMs = 0;
static std::atomic<ULONGLONG> s_lastStartupBlockingRenderModuleActivityMs{0};
// Once the GTA startup-overlay path has completed a stable first draw, later
// swapchain/sync reinitializations should keep the normal allocator pool.
static std::atomic<bool> s_startupOverlayCompatSettled{false};

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
    const auto runtimeMode = g_FGCompat.GetRuntimeMode();
    return runtimeMode == ce::fg_runtime::RuntimeMode::kDLSSFG || runtimeMode == ce::fg_runtime::RuntimeMode::kFSRFG;
}

static bool IsFSRFrameGenerationActive() {
    return g_FGCompat.GetRuntimeMode() == ce::fg_runtime::RuntimeMode::kFSRFG;
}

static bool IsNvidiaSmoothMotionActiveRuntime() {
    return g_FGCompat.GetRuntimeMode() == ce::fg_runtime::RuntimeMode::kNvidiaSmoothMotion;
}

static ExecuteCommandListsPtr GetOriginalExecuteCommandLists(ID3D12CommandQueue* queue);
static bool IsStreamlineLoaded();

static bool ShouldUseDedicatedOverlayQueue(const char** disabledByOverlayModule = nullptr) {
    const char* overlayModule = ce::overlay_compat::GetStartupBlockingOverlayModuleName();
    const bool processNeedsDelay = ce::overlay_compat::ShouldPreemptivelyDelayDX12OverlayInitForProcess(g_ProcessName);
    const bool actualFGActive = IsActualFrameGenerationActive();

    // When Streamline FG is active, do NOT use a dedicated overlay queue.
    // D3D12 rejects cross-queue access to swapchain backbuffers with
    // DXGI_ERROR_ACCESS_DENIED during SL FG (SL takes exclusive control
    // of the swapchain queue association).  Render on the game queue
    // instead, skipping fence operations to avoid interfering with SL's
    // internal frame synchronization.
    if (actualFGActive && IsStreamlineLoaded()) {
        if (disabledByOverlayModule)
            *disabledByOverlayModule = nullptr;
        return false;
    }

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

// Probe the real D3D12 ECL by creating a temporary COMPUTE queue.
// SL only vtable-hooks DIRECT queues for FG; COMPUTE queues keep the
// pristine d3d12.dll function pointer.  When DIRECT and COMPUTE queues
// share the same vtable (all hooks applied to the shared vtable), we
// fall back to scanning SL's hook for an indirect JMP/CALL target.
static void ProbeRealD3D12ECL(ID3D12Device* device) {
    if (g_RealD3D12ECL.load(std::memory_order_acquire))
        return;
    if (!device)
        return;

    // Create a temporary COMPUTE queue
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    desc.NodeMask = 0;

    ID3D12CommandQueue* probeQueue = nullptr;
    HRESULT hr = device->CreateCommandQueue(&desc, IID_PPV_ARGS(&probeQueue));
    if (FAILED(hr) || !probeQueue) {
        HookLogImportant("DX12: ECL probe - COMPUTE queue creation failed (hr=0x%08X)", (unsigned)hr);
        return;
    }

    void** probeVtable = *(void***)probeQueue;
    void* probeECL = probeVtable[10];

    // Check which module owns the COMPUTE queue's ECL
    HMODULE probeModule = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)probeECL, &probeModule);
    char probeMod[MAX_PATH] = {};
    if (probeModule)
        GetModuleFileNameA(probeModule, probeMod, MAX_PATH);

    // Compare with the current DIRECT queue's vtable[10] (our hooked version)
    ID3D12CommandQueue* directQueue = g_SwapchainQueue;
    void* directECL = nullptr;
    char directMod[MAX_PATH] = {};
    if (directQueue) {
        void** directVtable = *(void***)directQueue;
        directECL = directVtable[10];
        HMODULE dMod = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)directECL, &dMod);
        if (dMod)
            GetModuleFileNameA(dMod, directMod, MAX_PATH);
    }

    bool sameVtable = (probeVtable == (directQueue ? *(void***)directQueue : nullptr));
    bool sameECL = (probeECL == directECL);
    bool probeIsD3D12 = (strstr(probeMod, "d3d12") != nullptr || strstr(probeMod, "D3D12") != nullptr);

    HookLogImportant("DX12: ECL probe - COMPUTE ECL=%p (%s), DIRECT ECL=%p (%s), sameVtable=%d sameECL=%d isD3D12=%d",
                     probeECL, probeMod, directECL, directMod, sameVtable ? 1 : 0, sameECL ? 1 : 0,
                     probeIsD3D12 ? 1 : 0);

    if (probeIsD3D12) {
        g_RealD3D12ECL.store((ExecuteCommandListsPtr)probeECL, std::memory_order_release);
        HookLogImportant("DX12: Real D3D12 ECL found via COMPUTE probe: %p", probeECL);
    }

    // Always check saved original — in GTA V both COMPUTE and DIRECT share
    // the same vtable (sameECL=1) so our hook is on both, but
    // oExecuteCommandLists still holds the real D3D12 function.
    if (!g_RealD3D12ECL.load(std::memory_order_acquire)) {
        ExecuteCommandListsPtr savedOrig = oExecuteCommandLists;
        if (savedOrig) {
            HMODULE origMod = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)savedOrig, &origMod);
            char origModName[MAX_PATH] = {};
            if (origMod)
                GetModuleFileNameA(origMod, origModName, MAX_PATH);
            bool origIsD3D12 = (strstr(origModName, "d3d12") != nullptr || strstr(origModName, "D3D12") != nullptr);
            HookLogImportant("DX12: ECL probe - saved oECL=%p (%s) isD3D12=%d", (void*)savedOrig, origModName,
                             origIsD3D12 ? 1 : 0);
            if (origIsD3D12) {
                g_RealD3D12ECL.store(savedOrig, std::memory_order_release);
                HookLogImportant("DX12: Real D3D12 ECL found via saved original: %p", (void*)savedOrig);
            }
        }
    }

    // If still not found, try to follow the saved original's JMP chain
    if (!g_RealD3D12ECL.load(std::memory_order_acquire)) {
        ExecuteCommandListsPtr savedOrig = oExecuteCommandLists;
        if (savedOrig) {
            const uint8_t* fn = (const uint8_t*)savedOrig;
            void* target = nullptr;
            // Check for E9 rel32 (JMP rel32) — SL's hook might be a simple JMP
            if (fn[0] == 0xE9) {
                int32_t rel = *(const int32_t*)(fn + 1);
                target = (void*)(fn + 5 + rel);
            }
            // Check for FF 25 (JMP [rip+disp32]) — indirect JMP
            else if (fn[0] == 0xFF && fn[1] == 0x25) {
                int32_t disp = *(const int32_t*)(fn + 2);
                void** addr = (void**)(fn + 6 + disp);
                target = *addr;
            }
            // Check for 48 FF 25 (REX.W JMP [rip+disp32])
            else if (fn[0] == 0x48 && fn[1] == 0xFF && fn[2] == 0x25) {
                int32_t disp = *(const int32_t*)(fn + 3);
                void** addr = (void**)(fn + 7 + disp);
                target = *addr;
            }

            if (target) {
                HMODULE targetMod = nullptr;
                GetModuleHandleExA(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    (LPCSTR)target, &targetMod);
                char targetModName[MAX_PATH] = {};
                if (targetMod)
                    GetModuleFileNameA(targetMod, targetModName, MAX_PATH);
                bool isD3D12 = (strstr(targetModName, "d3d12") != nullptr || strstr(targetModName, "D3D12") != nullptr);
                HookLogImportant("DX12: ECL probe - followed JMP chain: target=%p (%s) isD3D12=%d", target,
                                 targetModName, isD3D12 ? 1 : 0);
                if (isD3D12) {
                    g_RealD3D12ECL.store((ExecuteCommandListsPtr)target, std::memory_order_release);
                    HookLogImportant("DX12: Real D3D12 ECL found via JMP chain: %p", target);
                }
            }
        }
    }

    if (!g_RealD3D12ECL.load(std::memory_order_acquire)) {
        HookLogImportant(
            "DX12: ECL probe - FAILED to find real D3D12 ECL! "
            "Overlay will be disabled during SL FG to prevent crash");
    }

    probeQueue->Release();
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
                         phase ? phase : "overlay command list",
                         submitQueue == gameQueue ? "game" : "dedicated overlay", submitQueue, gameQueue,
                         allocatorIndex);
    }

    ID3D12CommandList* lists[] = {list};

    // When using the dedicated overlay queue during SL FG, use the REAL
    // D3D12 ECL (bypassing SL's vtable hook) to prevent SL's internal
    // state tracking from seeing our overlay command lists.
    // ALSO prefer realECL in non-FG mode to avoid going through stale
    // SL/hook vtable entries after FG teardown (same logic as main path).
    ExecuteCommandListsPtr realECL = g_RealD3D12ECL.load(std::memory_order_acquire);
    bool slActive = IsStreamlineLoaded() && IsActualFrameGenerationActive();
    if (useDedicated && slActive && realECL) {
        realECL(submitQueue, 1, lists);
    } else if (!useDedicated && realECL) {
        realECL(submitQueue, 1, lists);
    } else {
        ExecuteCommandListsPtr origECL = GetOriginalExecuteCommandLists(submitQueue);
        if (origECL) {
            origECL(submitQueue, 1, lists);
        } else {
            submitQueue->ExecuteCommandLists(1, lists);
        }
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
            HookLog("DX12: Overlay fence signal failed for %s hr=0x%08X", phase ? phase : "overlay command list",
                    signalHr);
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
        IsActualFrameGenerationActive() ||
        g_FGCompat.IsFGActive()) {  // Also skip for heuristic FG (FSR FG) — avoids GetModuleHandleExA overhead
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
            "DX12: Startup-blocking render module activity detected via ExecuteCommandLists (module=%s, queue=%p, "
            "caller=%p)",
            moduleForLog, queue, callerAddress);
    }
}

static bool ShouldSuppressOverlayForStartupCompat(
    HWND gameWindow, const char** overlayModule = nullptr, ULONGLONG* remainingMs = nullptr,
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
    const ULONGLONG msSinceLastVisible = s_lastVisibleMs == 0 ? kStartupOverlayQuietPeriodMs : (now - s_lastVisibleMs);
    const bool suppress = ce::overlay_compat::ShouldSuppressDX12OverlayForStartup(
        true, actualFGActive, s_auxiliaryWindowVisible, msSinceOverlayDetected, kStartupOverlayWarmupMs,
        msSinceLastVisible, kStartupOverlayQuietPeriodMs);
    if (remainingMs && suppress) {
        ULONGLONG warmupRemaining =
            msSinceOverlayDetected < kStartupOverlayWarmupMs ? (kStartupOverlayWarmupMs - msSinceOverlayDetected) : 0;
        ULONGLONG quietRemaining = !s_auxiliaryWindowVisible && msSinceLastVisible < kStartupOverlayQuietPeriodMs
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

    const bool processNeedsDelay = ce::overlay_compat::ShouldPreemptivelyDelayDX12OverlayInitForProcess(g_ProcessName);
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
            *remainingMs =
                kStartupOverlayPostResumeSettleMs - std::min(msSinceResumeReady, kStartupOverlayPostResumeSettleMs);
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
                    overlayModule ? overlayModule : "module", activeWindow.hwnd, activeWindow.visible ? 1 : 0,
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
        return;
    }

    // Non-SL FG cases (e.g., FSR FG with third-party overlay) may still need
    // a dedicated queue.  For SL FG, ShouldUseDedicatedOverlayQueue() returns
    // false so we never reach here; overlay draws are skipped instead.
    HookLogImportant(
        "DX12: FG active with overlay compat — dedicated overlay queue not yet created, forcing sync reinit");
    g_State.syncInit = false;
    g_State.syncDevice = nullptr;
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

static ID3D12CommandQueue* GetFrameClassificationQueue() {
    ID3D12CommandQueue* primaryQueue = g_PrimaryGameQueue.load(std::memory_order_acquire);
    ID3D12CommandQueue* originalQueue = g_OriginalGameQueue;
    ID3D12CommandQueue* swapchainQueue = nullptr;
    bool actualFGActive = false;
    bool streamlineFGRunning = false;
    bool recoveringPostFSRNonFG = false;
    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        swapchainQueue = g_SwapchainQueue;
        actualFGActive = IsActualFrameGenerationActive();
        streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        recoveringPostFSRNonFG = ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(
            g_HadFSRFGPhase, g_NeedOffscreenOverlayAfterPostFSRNonFG, actualFGActive, streamlineFGRunning,
            swapchainQueue != nullptr);
    }

    if (ce::dx12_overlay_policy::ShouldUsePrimaryQueueForFrameClassificationDuringPostFSRNonFGRecovery(
            recoveringPostFSRNonFG, actualFGActive, streamlineFGRunning, swapchainQueue != nullptr,
            originalQueue != nullptr, primaryQueue != nullptr, originalQueue == primaryQueue)) {
        static std::atomic<int> s_postFSRClassificationPrimaryLogCount{0};
        int logCount = s_postFSRClassificationPrimaryLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: Frame classification using primary queue %p during post-FSR non-FG recovery "
                "(origGame=%p scQ=%p lastWorking=%p offscreen=%d)",
                primaryQueue, originalQueue, swapchainQueue, g_PostSLLastWorkingQueue,
                g_NeedOffscreenOverlayAfterPostFSRNonFG ? 1 : 0);
        }
        return primaryQueue;
    }

    if (originalQueue) {
        return originalQueue;
    }

    return primaryQueue;
}

static bool ShouldSuppressLikelyDuplicateTopLevelPresent(IDXGISwapChain3* sc3, UINT backBufferIdx) {
    if (!sc3 || !g_IPC || !g_IPC->IsCaptureRequested()) {
        return false;
    }

    SharedMemoryLayout* shm = g_IPC->GetSharedMem();
    if (!shm) {
        return false;
    }

    const int captureFps = shm->fpsLimiter.GetCaptureFps();
    if (captureFps <= 0) {
        return false;
    }

    const int64_t targetIntervalUs = 1000000LL / static_cast<int64_t>(captureFps);
    const int64_t suppressWindowUs = std::clamp((targetIntervalUs * 3) / 4, 1500LL, 7000LL);
    const int64_t nowUs = PerfLogger::GetQpcUs();
    IDXGISwapChain* swapchain = static_cast<IDXGISwapChain*>(sc3);

    static std::atomic<IDXGISwapChain*> s_lastAcceptedSwapchain{nullptr};
    static std::atomic<uint32_t> s_lastAcceptedBackBufferIdx{UINT32_MAX};
    static std::atomic<int64_t> s_lastAcceptedPresentUs{0};
    static std::atomic<uint64_t> s_suppressedPresentCount{0};

    IDXGISwapChain* lastSwapchain = s_lastAcceptedSwapchain.load(std::memory_order_acquire);
    uint32_t lastBackBufferIdx = s_lastAcceptedBackBufferIdx.load(std::memory_order_acquire);
    int64_t lastAcceptedPresentUs = s_lastAcceptedPresentUs.load(std::memory_order_acquire);
    int64_t sinceLastUs = nowUs - lastAcceptedPresentUs;

    if (lastSwapchain == swapchain && lastBackBufferIdx == backBufferIdx && lastAcceptedPresentUs != 0 &&
        sinceLastUs > 0 && sinceLastUs < suppressWindowUs) {
        uint64_t suppressCount = s_suppressedPresentCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (suppressCount <= 10 || (suppressCount % 1000) == 0) {
            HookLogImportant(
                "DX12: Suppressing likely duplicate top-level Present #%llu "
                "(sc=%p bb=%u since=%lldus window=%lldus captureFps=%d)",
                static_cast<unsigned long long>(suppressCount), swapchain, backBufferIdx,
                static_cast<long long>(sinceLastUs), static_cast<long long>(suppressWindowUs), captureFps);
        }
        return true;
    }

    s_lastAcceptedSwapchain.store(swapchain, std::memory_order_release);
    s_lastAcceptedBackBufferIdx.store(backBufferIdx, std::memory_order_release);
    s_lastAcceptedPresentUs.store(nowUs, std::memory_order_release);
    return false;
}

static bool ShouldSkipCaptureForTargetCadence() {
    SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    return ShouldSkipCaptureForTargetCadence(shm, "DX12");
}

// C Linkage Exports for cross-module calls (e.g. from C clients or
// GetProcAddress)
extern "C" {
// NOINLINE: Prevents LTO from inlining into the ECL detour, which would
// allow the compiler to merge vtable reads and optimize away our safety checks.
__attribute__((noinline)) void DX12_SetCommandQueue(ID3D12CommandQueue* pQueue) {
    if (!pQueue)
        return;

    // Safety: during FG transitions, SL may call ECL on a queue that's
    // concurrently being freed.  Freed COM objects have null vtable pointers.
    // Use volatile to prevent compiler from caching the vtable across calls.
    auto vtblPtr = *reinterpret_cast<void* volatile const*>(pQueue);
    if (!vtblPtr)
        return;

    // ExecuteCommandLists may hit this many times per frame on the same queue.
    // Once we've captured the active DIRECT queue, avoid the repeated GetDesc /
    // lock / QueryInterface work on the hot path.
    if (g_CommandQueue.load(std::memory_order_acquire) == pQueue)
        return;

    ID3D12CommandQueue* primaryQ = g_PrimaryGameQueue.load(std::memory_order_acquire);
    ID3D12CommandQueue* currentSwapchainQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        currentSwapchainQueue = g_SwapchainQueue;
    }

    const bool recentStreamlineTeardown = g_SLOffHeuristicGrace.load(std::memory_order_acquire) > 0;
    if (ce::dx12_overlay_policy::ShouldIgnoreCommandQueueRegistrationAfterRecentStreamlineTeardown(
            recentStreamlineTeardown, pQueue == primaryQ, pQueue == g_OriginalGameQueue,
            pQueue == currentSwapchainQueue)) {
        if (g_PostSLLastWorkingQueue && pQueue == g_PostSLLastWorkingQueue) {
            MarkPostSLRecentTeardownActivity("DX12: SetCommandQueue recent PostSL teardown activity", pQueue);
        }
        static std::atomic<int> s_recentSLTeardownSetQueueIgnoreLogCount{0};
        int logCount = s_recentSLTeardownSetQueueIgnoreLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 256) == 0) {
            HookLogImportant(
                "DX12: SetCommandQueue ignoring departed queue %p during recent Streamline teardown "
                "(primary=%p orig=%p scQ=%p current=%p slOffGrace=%d)",
                pQueue, primaryQ, g_OriginalGameQueue, currentSwapchainQueue,
                g_CommandQueue.load(std::memory_order_acquire), g_SLOffHeuristicGrace.load(std::memory_order_acquire));
        }
        return;
    }

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

        // Re-check vtable before GetDevice — another thread may have freed
        // the queue between GetDesc and here.  Volatile prevents caching.
        auto vtblRecheck = *reinterpret_cast<void* volatile const*>(pQueue);
        if (!vtblRecheck) {
            HookLogImportant("DX12: SetCommandQueue — queue %p freed during registration (vtable null after store)",
                             pQueue);
            return;
        }

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

}  // extern "C" (DX12_SetCommandQueue)

// Capture the queue that was passed to CreateSwapChain* so we can prefer it
// for overlay submission.  Only accepts DIRECT queues (same rule as
// DX12_SetCommandQueue).  Also hooks the queue vtable for ECL interception.
static bool DX12_SetSwapchainQueue(ID3D12CommandQueue* pQueue) {
    if (!pQueue)
        return false;

    // Safety: freed COM objects have null vtable — skip
    void** vtblCheck = *reinterpret_cast<void***>(pQueue);
    if (!vtblCheck)
        return false;

    D3D12_COMMAND_QUEUE_DESC desc = pQueue->GetDesc();
    if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return false;

    bool runtimeOwnershipJustActivated = false;

    // Diagnostic: log the queue's device to detect cross-device issues
    ID3D12Device* queueDev = nullptr;
    if (SUCCEEDED(pQueue->GetDevice(IID_PPV_ARGS(&queueDev)))) {
        auto* curDev = g_Device.load(std::memory_order_acquire);
        if (queueDev != curDev) {
            HookLogImportant("DX12: SetSwapchainQueue — queue %p device %p DIFFERS from g_Device %p", pQueue, queueDev,
                             curDev);
        }
        queueDev->Release();
    }

    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    if (g_SwapchainQueue != pQueue) {
        if (g_SwapchainQueue)
            g_SwapchainQueue->Release();
        g_SwapchainQueue = pQueue;
        g_SwapchainQueue->AddRef();
        g_SwapchainQueueCaptureTime = GetTickCount64();

        // Track whether an FG runtime owns this swapchain/queue
        bool runtimeOwns = (g_OriginalGameQueue && pQueue != g_OriginalGameQueue);
        if (runtimeOwns && !g_FGRuntimeOwnsSwapchain) {
            g_FGRuntimeOwnsSwapchain = true;
            DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(true, std::memory_order_release);
            g_FGRuntimeOwnsSwapchainSince = GetTickCount64();
            runtimeOwnershipJustActivated = true;
            HookLogImportant(
                "DX12: FG runtime now owns swapchain queue %p (origGame=%p) — overlay will skip GPU work on this queue",
                pQueue, g_OriginalGameQueue);
        } else if (!runtimeOwns && g_FGRuntimeOwnsSwapchain) {
            g_FGRuntimeOwnsSwapchain = false;
            DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(false, std::memory_order_release);
            g_FGRuntimeOwnsSwapchainSince = 0;
            if (g_FGCompat.IsFSRFGApiActive()) {
                HookLogImportant("DX12: Swapchain returned to origGame queue %p — ending authoritative FSR FG state",
                                 pQueue);
                g_FGCompat.SetFSRFGActive(false);
                g_FGCompat.SetFSRFGMultiplier(0);
                ResetAuthoritativeFSRRealFrameOnlyStreak();
            }
            HookLogImportant("DX12: Swapchain returned to origGame queue %p — FG runtime ownership cleared", pQueue);
        }

        HookLogImportant("DX12: Swapchain queue captured (queue=%p, origGame=%p, same=%d, fgOwned=%d)", pQueue,
                         g_OriginalGameQueue, (pQueue == g_OriginalGameQueue) ? 1 : 0,
                         g_FGRuntimeOwnsSwapchain ? 1 : 0);
    }

    // Only hook the vtable if this is the game's original queue (or we haven't
    // captured origGame yet).  FG runtimes (FSR FG) create their own queues and
    // rely on tight ECL timing.  Hooking their vtable adds overhead to every ECL
    // call (safety checks, heartbeat, queue tracking, lock acquisition, etc.).
    // This cumulative overhead breaks FSR FG's internal fence synchronization,
    // causing ffxQuery to spin-wait or WaitForSingleObject indefinitely.
    // We already hook origGame's queue for watchdog/heartbeat — that's sufficient.
    if (pQueue == g_OriginalGameQueue || !g_OriginalGameQueue) {
        DX12_HookQueueVTable(pQueue);
    } else {
        HookLogImportant("DX12: Skipping vtable hook for FG runtime queue %p (origGame=%p) — preserving FSR timing",
                         pQueue, g_OriginalGameQueue);
    }

    return runtimeOwnershipJustActivated;
}

static bool IsDX12Swapchain(IDXGISwapChain1* pSwapChain) {
    if (!pSwapChain)
        return false;

    ID3D12Device* pDX12Device = nullptr;
    HRESULT hr = pSwapChain->GetDevice(IID_PPV_ARGS(&pDX12Device));
    if (FAILED(hr) || !pDX12Device)
        return false;

    pDX12Device->Release();
    return true;
}

static void CaptureSwapchainQueueFromCreateDevice(IUnknown* pDevice, IDXGISwapChain1* pSwapChain, const char* context,
                                                  const void* callerAddress = nullptr) {
    if (!pDevice || !pSwapChain)
        return;

    ID3D12CommandQueue* pQueue = nullptr;
    HRESULT qiHr = pDevice->QueryInterface(IID_PPV_ARGS(&pQueue));
    if (SUCCEEDED(qiHr) && pQueue) {
        HookLogImportant("%s: QI for queue succeeded (queue=%p)", context, pQueue);
        const bool runtimeOwnershipJustActivated = DX12_SetSwapchainQueue(pQueue);
        ClearStaleStreamlineOwnershipForFSRTakeover(callerAddress, g_OriginalGameQueue && pQueue != g_OriginalGameQueue,
                                                    runtimeOwnershipJustActivated, pQueue);
        pQueue->Release();
        return;
    }

    // CreateSwapChainForHwnd is shared by DX10/11/12. Avoid treating arbitrary
    // DXGI callers as ID3D12CommandQueue objects when QI already proved they are not.
    if (IsDX12Swapchain(pSwapChain)) {
        HookLogImportant(
            "%s: DX12 swapchain created with device=%p but ID3D12CommandQueue QI failed (hr=0x%08X) — "
            "leaving swapchain queue unchanged",
            context, pDevice, qiHr);
    } else {
        HookLogImportant("%s: Non-DX12 swapchain for device=%p (queue QI hr=0x%08X) — skipping queue capture", context,
                         pDevice, qiHr);
    }
}

void DX12_AdjustWrapperResizeDepth_C(int delta) {
    DX12_AdjustWrapperResizeDepth(delta);
}

// Queue-aware wrapper fallback for frame classification.
// The wrapper path is only used when the real queue has not been registered yet;
// once registration succeeds, the vtable ECL detour becomes the authoritative
// source of command-list counts.
__attribute__((noinline)) void DX12_NotifyCommandListsForQueue(ID3D12CommandQueue* pQueue, UINT numCommandLists) {
    if (!pQueue || numCommandLists == 0) {
        return;
    }

    auto vtblPtr = *reinterpret_cast<void* volatile const*>(pQueue);
    if (!vtblPtr) {
        return;
    }

    D3D12_COMMAND_QUEUE_DESC desc = pQueue->GetDesc();
    if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        return;
    }

    ID3D12CommandQueue* classificationQueue = GetFrameClassificationQueue();
    if (!classificationQueue || pQueue != classificationQueue) {
        static std::atomic<int> s_skippedWrapperNotifyLogCount{0};
        int skipCount = s_skippedWrapperNotifyLogCount.fetch_add(1, std::memory_order_relaxed);
        if (skipCount < 10 || (skipCount % 2048) == 2047) {
            HookLog(
                "DX12: Ignoring wrapper queue notify for non-classification queue "
                "%p (class=%p, primary=%p, orig=%p, num=%u)",
                pQueue, classificationQueue, g_PrimaryGameQueue.load(std::memory_order_relaxed), g_OriginalGameQueue,
                numCommandLists);
        }
        return;
    }

    g_CommandListsExecutedThisFrame.fetch_add(numCommandLists, std::memory_order_relaxed);
}

// Legacy queue-less wrapper notify. Ignore it so stale helper traffic cannot
// mark auxiliary command queue work as a real frame.
void DX12_NotifyCommandLists(UINT numCommandLists) {
    static std::atomic<int> s_legacyNotifyLogCount{0};
    int logCount = s_legacyNotifyLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 5) {
        HookLog(
            "DX12: Ignoring legacy queue-less DX12_NotifyCommandLists(%u) to avoid "
            "false real-frame classification",
            numCommandLists);
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

#ifdef ENABLE_D3D12_WRAPPER
    // When D3D12 wrapper is enabled, Present inline hooks are deferred to
    // EnsurePresentHooks() (called from Wrapped_D3D12CreateDevice) to avoid
    // creating a temp D3D12 device in DX11-only apps that load d3d12.dll via
    // D3D11On12.
    HookLog("DX12Hook: Initialized (factory hooks installed; Present hooks deferred to D3D12CreateDevice)");
#else
    // Without D3D12 wrapper, D3D12CreateDevice isn't hooked and the deferred
    // trigger never fires.  Install Present inline hooks now via a temp
    // swapchain so pre-existing swapchains (created before injection) are
    // covered.  The temp device/swapchain is destroyed immediately after
    // hooking, so DX11 state corruption is not a concern.
    HookLog("DX12Hook: Installing Present hooks eagerly (no D3D12 wrapper)");
    HookSwapchainVTableViaTempSwapchain();
    HookLog("DX12Hook: Initialized (factory + Present hooks installed)");
#endif

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
    if (!pSwapChain || DXGIShared::HasPresentDetourHooks()) {
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
    } else if (DXGIShared::HasPresentDetourHooks()) {
        HookLogImportant("DX12: Present detour hooks are active via %s swapchain (external overlay-compatible path)",
                         source ? source : "real");
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

void DX12_StartTransitionCooldown() {
    StartTransitionCooldown();
}

bool DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration() {
    return g_FGRuntimeOwnsSwapchain;
}

void DX12_OnStreamlineFGStateChanged(bool active) {
    if (active) {
        SetPostSLCallbackInstalled(true, "DX12: Streamline FG ON");
        HookLogImportant("DX12: Streamline FG ON — pre-armed PostSL callback for startup routing");
        int cooldownLeft = g_PostSLCooldownRemaining.load(std::memory_order_acquire);
        while (cooldownLeft < 60 && !g_PostSLCooldownRemaining.compare_exchange_weak(
                                        cooldownLeft, 60, std::memory_order_acq_rel, std::memory_order_acquire)) {}
        g_PostSLOverlayActive.store(false, std::memory_order_release);
        g_PostSLConfirmedRendering.store(false, std::memory_order_release);
        g_PostSLStallCounter.store(0, std::memory_order_release);
        g_PostSLStableFrameCount.store(0, std::memory_order_release);
        g_PostSLSyntheticStartupActivationPending.store(true, std::memory_order_release);
        DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(true, std::memory_order_release);
        g_PostSLSyntheticStartupTakeoverLogged.store(false, std::memory_order_release);
        ResetPostSLLifecycleForTransition("DX12: Streamline FG ON transition", true);
        if (g_HadFSRFGPhase) {
            ID3D12CommandQueue* staleScQueue = nullptr;
            {
                std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
                if (g_SwapchainQueue && g_SwapchainQueue != g_OriginalGameQueue) {
                    staleScQueue = g_SwapchainQueue;
                    g_SwapchainQueue = nullptr;
                    g_SwapchainQueueCaptureTime = 0;
                    g_FGRuntimeOwnsSwapchain = false;
                    DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(false, std::memory_order_release);
                    if (g_FGCompat.IsFSRFGApiActive()) {
                        g_FGCompat.SetFSRFGActive(false);
                        g_FGCompat.SetFSRFGMultiplier(0);
                        ResetAuthoritativeFSRRealFrameOnlyStreak();
                    }
                    HookLogImportant(
                        "DX12: Streamline FG ON after FSR — cleared stale FSR swapchain queue %p (origGame=%p) "
                        "to prevent DEVICE_REMOVED on FSR→DLSS transition",
                        staleScQueue, g_OriginalGameQueue);
                }
            }
            if (staleScQueue) {
                staleScQueue->Release();
            }
        }
        return;
    }

    g_PostSLOverlayActive.store(false, std::memory_order_release);
    SetPostSLCallbackInstalled(false, "DX12: Streamline FG OFF");
    g_PostSLConfirmedRendering.store(false, std::memory_order_release);
    g_PostSLStallCounter.store(0, std::memory_order_release);
    g_PostSLStableFrameCount.store(0, std::memory_order_release);
    g_PostSLSyntheticStartupActivationPending.store(false, std::memory_order_release);
    DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(false, std::memory_order_release);
    g_PostSLSyntheticStartupTakeoverLogged.store(false, std::memory_order_release);
    g_SLOffHeuristicGrace.store(600, std::memory_order_release);
    g_ResetQueueChangeHeuristic.store(true, std::memory_order_release);
    g_FGCompat.SetHeuristicFSRFGActive(false);
    g_FGCompat.ClearNvidiaSMState();
    if (auto* perf = DXGIShared::GetPerformanceMetrics()) {
        perf->SetFGMetrics(0.0f, 0.0f, 1, 0);
    }
    g_OverlayAdapter.InvalidateCachedFrame();
    g_D3D11On12Adapter.InvalidateCachedFrame();
    g_SLOffSwapchainReinitGrace.store(300, std::memory_order_release);
    ResetPostSLLifecycleForTransition("DX12: Streamline FG OFF transition", true, true);

    if (g_HadFSRFGPhase) {
        ID3D12CommandQueue* staleScQueue = nullptr;
        {
            std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);

            if (g_FGRuntimeOwnsSwapchain) {
                g_FGRuntimeOwnsSwapchain = false;
                DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(false, std::memory_order_release);
                g_FGRuntimeOwnsSwapchainSince = 0;
                if (g_FGCompat.IsFSRFGApiActive()) {
                    g_FGCompat.SetFSRFGActive(false);
                    g_FGCompat.SetFSRFGMultiplier(0);
                    ResetAuthoritativeFSRRealFrameOnlyStreak();
                }
                HookLogImportant("DX12: Streamline FG OFF after FSR history — clearing lingering FG runtime ownership");
            }

            if (g_SwapchainQueue && g_SwapchainQueue != g_OriginalGameQueue) {
                staleScQueue = g_SwapchainQueue;
                g_SwapchainQueue = nullptr;
                g_SwapchainQueueCaptureTime = 0;
                HookLogImportant(
                    "DX12: Streamline FG OFF after FSR history — releasing stale swapchain queue %p so top-level "
                    "recovery can recapture the live non-FG queue (origGame=%p)",
                    staleScQueue, g_OriginalGameQueue);
            }
        }

        if (staleScQueue) {
            staleScQueue->Release();
        }

        HookLogImportant(
            "DX12: Streamline FG OFF after FSR history — leaving swapchain queue uncaptured until a live non-FG "
            "queue is observed again (origGame=%p primary=%p cmdQ=%p)",
            g_OriginalGameQueue, g_PrimaryGameQueue.load(std::memory_order_acquire),
            g_CommandQueue.load(std::memory_order_acquire));

        // The post-FSR DLSS path rendered through Streamline/PostSL against a
        // different swapchain topology than the resumed non-FG path. Force a
        // swapchain-level reinit so the next top-level Present rebuilds RTVs and
        // overlay state against the live non-FG swapchain/queue pair.
        if (g_State.overlayInit) {
            HookLogImportant(
                "DX12: Streamline FG OFF after FSR history — forcing overlay swapchain reinit for non-FG recovery");
            g_State.overlayInit = false;
            CleanupRTVs();
        }

        if (ce::dx12_overlay_policy::ShouldDeferOverlayReinitAfterDirectPostFSRStreamlineTeardown(
                g_HadFSRFGPhase, g_State.overlayInit, g_State.syncInit)) {
            g_State.syncInit = false;
            g_ResetReinitSubmitCounter.store(true, std::memory_order_release);
            g_OuterTrackedSLFGRunning.store(false, std::memory_order_release);
            g_OuterSLTransitionEpoch.fetch_add(1, std::memory_order_release);
            auto* oldRealECL = (void*)g_RealD3D12ECL.load(std::memory_order_acquire);
            const ID3D12CommandQueue* currentPrimaryQueue = g_PrimaryGameQueue.load(std::memory_order_acquire);
            const ID3D12CommandQueue* currentCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
            const bool commandQueueSettledToPrimary =
                currentCommandQueue != nullptr && currentCommandQueue == currentPrimaryQueue;
            const int cooldownFrames =
                ce::dx12_overlay_policy::ShouldUseShortPostFSRInactiveCooldown(commandQueueSettledToPrimary,
                                                                               g_HadFSRFGPhase, true)
                    ? 15
                    : 60;
            g_FGTransitionCooldown = ce::dx12_overlay_policy::ResolveTransitionCooldownFrames(
                g_FGTransitionCooldown, cooldownFrames,
                ce::dx12_overlay_policy::ShouldUseShortPostFSRInactiveCooldown(commandQueueSettledToPrimary,
                                                                               g_HadFSRFGPhase, true));
            g_PostSLCooldownRemaining.store(g_FGTransitionCooldown, std::memory_order_release);
            HookLogImportant(
                "DX12: Streamline FG OFF after FSR history — deferring non-FG overlay reinit for %d frames so "
                "Talos/Streamline teardown can settle before pre-SL resources are rebuilt",
                g_FGTransitionCooldown);
            HookLogImportant(
                "DX12: Streamline FG OFF after FSR history — invalidated sync resources for delayed reinit");
            if (ce::dx12_overlay_policy::ShouldPreserveRealECLForDelayedPostFSRNonFGRecovery(
                    commandQueueSettledToPrimary, g_HadFSRFGPhase)) {
                HookLogImportant(
                    "DX12: Streamline FG OFF after FSR history — preserving realECL %p for delayed non-FG "
                    "recovery because cmdQ=%p already settled to primary",
                    oldRealECL, currentCommandQueue);
            } else {
                g_RealD3D12ECL.store(nullptr, std::memory_order_release);
                HookLogImportant(
                    "DX12: Streamline FG OFF after FSR history — cleared realECL %p for delayed non-FG recovery",
                    oldRealECL);
            }
            g_NeedOffscreenOverlayAfterPostFSRNonFG = true;
            HookLogImportant(
                "DX12: Streamline FG OFF after FSR history — enabled offscreen overlay compositing for non-FG "
                "recovery (backbuffer state indeterminate after FG teardown)");
        }
    }

    HookLogImportant("DX12: Streamline FG OFF — seeded heuristic reset/grace (slOffGrace=600)");
    HookLogImportant("DX12: Streamline FG OFF — cleared PostSL callback state");
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

    // Skip side-effects for temp swapchains created during hook installation
    if (g_CreatingTempSwapchain.load(std::memory_order_acquire)) {
        HookLog("DeepHook: Temp swapchain creation — passthrough (no tracking)");
        return s_deepHookTrampoline(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);
    }

    const void* callerAddress = CE_RETURN_ADDRESS();
    char callerModulePath[MAX_PATH] = {};
    const bool callerFromFFXFGModule =
        IsCodeAddressFromFFXFrameGenerationModule(callerAddress, callerModulePath, sizeof(callerModulePath));
    const bool ffxFrameGenerationInStack = HasFFXFrameGenerationModuleInCurrentStack();

    HookLogImportant("DeepHook: CreateSwapChainForHwnd ENTER factory=%p device=%p hwnd=%p BufferCount=%u SwapEffect=%d",
                     pThis, pDevice, hWnd, pDesc ? pDesc->BufferCount : 0, pDesc ? (int)pDesc->SwapEffect : -1);

    // Apply backbuffer count override from config
    DXGI_SWAP_CHAIN_DESC1 modifiedDesc;
    const DXGI_SWAP_CHAIN_DESC1* pDescToUse = pDesc;
    if (pDesc) {
        modifiedDesc = *pDesc;
        const auto& gfx = GetActiveGraphicsConfig();
        if (HasBackbufferCountOverride(gfx.backbufferCount)) {
            UINT requested = (UINT)gfx.backbufferCount;
            bool isFlip = (modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                           modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
            if (isFlip && requested < modifiedDesc.BufferCount) {
                HookLogImportant("DeepHook: Skipping BufferCount override %u < game's %u (flip model)", requested,
                                 modifiedDesc.BufferCount);
            } else {
                HookLogImportant("DeepHook: Overriding BufferCount %u -> %u", modifiedDesc.BufferCount, requested);
                modifiedDesc.BufferCount = requested;
            }
        }
        pDescToUse = &modifiedDesc;
    }

    // Try the call first — let the game/SL handle SC lifecycle naturally
    HRESULT hr = s_deepHookTrampoline(pThis, pDevice, hWnd, pDescToUse, pFDesc, pOut, ppSC);
    HookLogImportant("DeepHook: Trampoline returned hr=0x%08X sc=%p", hr, (ppSC ? *ppSC : nullptr));

    if (SUCCEEDED(hr) && ppSC && *ppSC) {
        // Only start transition cooldown when a swapchain recreation actually
        // succeeded. Starting it on E_ACCESSDENIED leaves the overlay in a
        // half-transitioned state while Streamline/game keeps the old chain.
        StartTransitionCooldown();
    }

    // Reactive recovery: if E_ACCESSDENIED, an old SC still holds the HWND.
    // DON'T force-destroy — that invalidates game-held references and causes
    // delayed UE5 assertion crashes.  Clean up our overlay refs and do a very
    // brief retry.  If it still fails, return the error so the caller
    // (Streamline/game) can manage its own state machine — long blocking here
    // causes DLSS FG activation crashes.
    if (hr == E_ACCESSDENIED && hWnd) {
        const bool streamlineModuleLoaded = IsStreamlineLoaded();
        const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        const bool streamlineStartupHandoffPending = DXGIShared::IsStreamlineStartupHandoffPending();
        const bool passThroughForStreamline =
            ce::dx12_overlay_policy::ShouldPassThroughCreateSwapchainAccessDeniedForStreamline(
                streamlineModuleLoaded, streamlineFGRunning, streamlineStartupHandoffPending, callerFromFFXFGModule,
                ffxFrameGenerationInStack);
        if (passThroughForStreamline) {
            HookLogImportant(
                "DeepHook: E_ACCESSDENIED for HWND=%p — Streamline active, passing through "
                "(slFG=%d startupPending=%d callerFFX=%d stackFFX=%d)",
                hWnd, streamlineFGRunning ? 1 : 0, streamlineStartupHandoffPending ? 1 : 0,
                callerFromFFXFGModule ? 1 : 0, ffxFrameGenerationInStack ? 1 : 0);
        } else {
            HookLogImportant(
                "DeepHook: E_ACCESSDENIED for HWND=%p — cleaning up overlay refs "
                "(slLoaded=%d slFG=%d startupPending=%d callerFFX=%d stackFFX=%d module=%s)",
                hWnd, streamlineModuleLoaded ? 1 : 0, streamlineFGRunning ? 1 : 0,
                streamlineStartupHandoffPending ? 1 : 0, callerFromFFXFGModule ? 1 : 0,
                ffxFrameGenerationInStack ? 1 : 0, callerModulePath[0] ? callerModulePath : "unknown");

            // Clean up ALL overlay resources so we don't hold stale refs that
            // prevent DXGI from releasing the HWND association.  Must match the
            // cleanup sequence in DX12_OnSwapchainResizeBegin which successfully
            // avoids E_ACCESSDENIED before ResizeBuffers.
            {
                std::lock_guard<std::recursive_mutex> overlayLock(g_OverlayMutex);
                g_LastSwapChain = nullptr;
                CleanupOverlay();
                CleanupRTVs();
                g_State.overlayInit = false;
            }
            HookLogImportant("DeepHook: Released overlay + RTV refs for HWND=%p", hWnd);

            // Clear our tracking entries (raw pointers, no Release needed)
            {
                std::lock_guard<std::mutex> lock(s_hwndSwapchainMutex);
                s_hwndSwapchainMap.erase(hWnd);
            }

            // Retry: 10 attempts × 20ms = 200ms max.  FSR FG activation may
            // need time for the game to release its own swapchain refs after
            // we've released ours.
            for (int attempt = 1; attempt <= 10 && hr == E_ACCESSDENIED; ++attempt) {
                Sleep(20);
                hr = s_deepHookTrampoline(pThis, pDevice, hWnd, pDescToUse, pFDesc, pOut, ppSC);
                if (SUCCEEDED(hr)) {
                    HookLogImportant("DeepHook: Retry attempt %d succeeded hr=0x%08X sc=%p", attempt, hr,
                                     (ppSC ? *ppSC : nullptr));
                    break;
                }
            }
            if (FAILED(hr)) {
                HookLogImportant("DeepHook: All retries exhausted — returning E_ACCESSDENIED to caller (HWND=%p)",
                                 hWnd);
            }
        }
    }

    // Post-track: record the new swapchain for future reactive recovery
    if (SUCCEEDED(hr) && ppSC && *ppSC && hWnd) {
        TrackSwapchainHwnd(*ppSC, hWnd);
        HookLogImportant("DeepHook: Created & tracked swapchain %p for HWND=%p", *ppSC, hWnd);

        // SL (or game) just created a new swapchain.  Install our Present
        // hook on it — the new swapchain may have a different vtable than
        // the one we initially hooked.
        IDXGISwapChain* newSC = static_cast<IDXGISwapChain*>(*ppSC);
        DXGIShared::InstallHooks(newSC, /*presentOnly=*/true);
        DXGIShared::RepairVTableHooksIfNeeded();

        CaptureSwapchainQueueFromCreateDevice(pDevice, *ppSC, "DeepHook", callerAddress);
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

    // Skip side-effects for temp swapchains created during hook installation
    if (g_CreatingTempSwapchain.load(std::memory_order_acquire)) {
        HookLog("CreateSwapChainForHwnd INLINE: Temp swapchain — passthrough");
        return s_oCreateSCForHwndInline(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);
    }

    const void* callerAddress = CE_RETURN_ADDRESS();
    char callerModulePath[MAX_PATH] = {};
    const bool callerFromFFXFGModule =
        IsCodeAddressFromFFXFrameGenerationModule(callerAddress, callerModulePath, sizeof(callerModulePath));
    const bool ffxFrameGenerationInStack = HasFFXFrameGenerationModuleInCurrentStack();

    HookLogImportant("CreateSwapChainForHwnd INLINE: factory=%p device=%p hwnd=%p", pThis, pDevice, hWnd);

    // Apply backbuffer count override from config
    DXGI_SWAP_CHAIN_DESC1 modifiedDesc;
    const DXGI_SWAP_CHAIN_DESC1* pDescToUse = pDesc;
    if (pDesc) {
        modifiedDesc = *pDesc;
        const auto& gfx = GetActiveGraphicsConfig();
        if (HasBackbufferCountOverride(gfx.backbufferCount)) {
            UINT requested = (UINT)gfx.backbufferCount;
            bool isFlip = (modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                           modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
            if (isFlip && requested < modifiedDesc.BufferCount) {
                HookLogImportant("INLINE: Skipping BufferCount override %u < game's %u (flip model)", requested,
                                 modifiedDesc.BufferCount);
            } else {
                HookLogImportant("INLINE: Overriding BufferCount %u -> %u", modifiedDesc.BufferCount, requested);
                modifiedDesc.BufferCount = requested;
            }
        }
        pDescToUse = &modifiedDesc;
    }

    HRESULT hr = s_oCreateSCForHwndInline(pThis, pDevice, hWnd, pDescToUse, pFDesc, pOut, ppSC);
    HookLogImportant("CreateSwapChainForHwnd INLINE: result hr=0x%08X sc=%p", hr, (ppSC && *ppSC) ? *ppSC : nullptr);

    if (hr == E_ACCESSDENIED && hWnd) {
        // When Streamline is managing swapchain lifecycle, don't interfere.
        // Our CleanupOverlay() flushes the GPU (200ms Signal+Wait) and destroys
        // overlay resources, which disrupts Streamline's internal state machine.
        const bool streamlineModuleLoaded = IsStreamlineLoaded();
        const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        const bool streamlineStartupHandoffPending = DXGIShared::IsStreamlineStartupHandoffPending();
        const bool passThroughForStreamline =
            ce::dx12_overlay_policy::ShouldPassThroughCreateSwapchainAccessDeniedForStreamline(
                streamlineModuleLoaded, streamlineFGRunning, streamlineStartupHandoffPending, callerFromFFXFGModule,
                ffxFrameGenerationInStack);
        if (passThroughForStreamline) {
            HookLogImportant(
                "CreateSwapChainForHwnd INLINE: E_ACCESSDENIED for HWND=%p — "
                "Streamline active, passing through without cleanup "
                "(slFG=%d startupPending=%d callerFFX=%d stackFFX=%d)",
                hWnd, streamlineFGRunning ? 1 : 0, streamlineStartupHandoffPending ? 1 : 0,
                callerFromFFXFGModule ? 1 : 0, ffxFrameGenerationInStack ? 1 : 0);
        } else {
            HookLogImportant(
                "CreateSwapChainForHwnd INLINE: E_ACCESSDENIED for HWND=%p — "
                "cleaning up overlay refs "
                "(slLoaded=%d slFG=%d startupPending=%d callerFFX=%d stackFFX=%d module=%s)",
                hWnd, streamlineModuleLoaded ? 1 : 0, streamlineFGRunning ? 1 : 0,
                streamlineStartupHandoffPending ? 1 : 0, callerFromFFXFGModule ? 1 : 0,
                ffxFrameGenerationInStack ? 1 : 0, callerModulePath[0] ? callerModulePath : "unknown");

            // Clean up ALL overlay resources — same sequence as deep hook and
            // DX12_OnSwapchainResizeBegin to fully release the HWND association.
            {
                std::lock_guard<std::recursive_mutex> overlayLock(g_OverlayMutex);
                g_LastSwapChain = nullptr;
                CleanupOverlay();
                CleanupRTVs();
                g_State.overlayInit = false;
            }
            HookLogImportant("CreateSwapChainForHwnd INLINE: Released overlay + RTV refs for HWND=%p", hWnd);
            {
                std::lock_guard<std::mutex> lock(s_hwndSwapchainMutex);
                s_hwndSwapchainMap.erase(hWnd);
            }

            // Retry: 10 attempts × 20ms = 200ms max
            for (int attempt = 1; attempt <= 10 && hr == E_ACCESSDENIED; ++attempt) {
                Sleep(20);
                hr = s_oCreateSCForHwndInline(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);
                if (SUCCEEDED(hr)) {
                    HookLogImportant("CreateSwapChainForHwnd INLINE: Retry attempt %d succeeded hr=0x%08X", attempt,
                                     hr);
                    break;
                }
            }
            if (FAILED(hr)) {
                HookLogImportant(
                    "CreateSwapChainForHwnd INLINE: All retries exhausted — returning E_ACCESSDENIED to caller "
                    "(HWND=%p)",
                    hWnd);
            }
        }
    }

    if (SUCCEEDED(hr) && ppSC && *ppSC) {
        TrackSwapchainHwnd(*ppSC, hWnd);
        HookLogImportant("CreateSwapChainForHwnd INLINE: Created swapchain %p for HWND=%p", *ppSC, hWnd);

        CaptureSwapchainQueueFromCreateDevice(pDevice, *ppSC, "CreateSwapChainForHwnd INLINE", callerAddress);
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

    HookLog("DetourCreateSwapChainGlobal: CALLED (factory=%p, device=%p, swapEffect=%d)", pThis, pDevice,
            pDesc ? (int)pDesc->SwapEffect : -1);

    // Apply backbuffer count override from config
    DXGI_SWAP_CHAIN_DESC modifiedDesc;
    DXGI_SWAP_CHAIN_DESC* pDescToUse = pDesc;
    if (pDesc) {
        modifiedDesc = *pDesc;
        const auto& gfx = GetActiveGraphicsConfig();
        if (HasBackbufferCountOverride(gfx.backbufferCount)) {
            UINT requested = (UINT)gfx.backbufferCount;
            bool isFlip = (modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                           modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
            if (isFlip && requested < modifiedDesc.BufferCount) {
                HookLog("DetourCreateSwapChainGlobal: Skipping BufferCount override %u < game's %u (flip model)",
                        requested, modifiedDesc.BufferCount);
            } else {
                HookLog("DetourCreateSwapChainGlobal: Overriding BufferCount %u -> %u", modifiedDesc.BufferCount,
                        requested);
                modifiedDesc.BufferCount = requested;
            }
        }
        pDescToUse = &modifiedDesc;
    }

    // Call original with (possibly) modified descriptor
    HRESULT hr = oCreateSwapChainGlobal(pThis, pDevice, pDescToUse, ppSwapChain);

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

    // Apply backbuffer count override from config
    DXGI_SWAP_CHAIN_DESC1 modifiedDesc;
    const DXGI_SWAP_CHAIN_DESC1* pDescToUse = pDesc;
    if (pDesc) {
        modifiedDesc = *pDesc;
        const auto& gfx = GetActiveGraphicsConfig();
        if (HasBackbufferCountOverride(gfx.backbufferCount)) {
            UINT requested = (UINT)gfx.backbufferCount;
            bool isFlip = (modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                           modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
            if (isFlip && requested < modifiedDesc.BufferCount) {
                HookLogImportant(
                    "DetourCreateSwapChainForHwndGlobal: Skipping BufferCount override %u < game's %u (flip model)",
                    requested, modifiedDesc.BufferCount);
            } else {
                HookLogImportant("DetourCreateSwapChainForHwndGlobal: Overriding BufferCount %u -> %u",
                                 modifiedDesc.BufferCount, requested);
                modifiedDesc.BufferCount = requested;
            }
        }
        pDescToUse = &modifiedDesc;
    }

    HRESULT hr = oCreateSwapChainForHwndGlobal(pThis, pDevice, hWnd, pDescToUse, pFDesc, pOut, ppSC);

    if (SUCCEEDED(hr) && ppSC && *ppSC) {
        StartTransitionCooldown();

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

    // Also hook IDXGIFactory4 and IDXGIFactory6 vtables to catch games that
    // QueryInterface for higher factory versions (different vtable pointers).
    // CreateSwapChainForHwnd is at the same slot (15) in all factory versions
    // because IDXGIFactory4 inherits from IDXGIFactory3 → IDXGIFactory2.
    IDXGIFactory4* pFactory4 = nullptr;
    if (SUCCEEDED(pCreateFactory(IID_PPV_ARGS(&pFactory4)))) {
        void** vtable4 = *(void***)pFactory4;
        HookLog("DX12: IDXGIFactory4 available, vtable=%p (IDXGIFactory2=%p, same=%d)", vtable4, vtable,
                (int)(vtable4 == vtable));
        if (vtable4 != vtable) {  // Different vtable pointer
            VTableHook::Create(&vtable4[10], (LPVOID)DetourCreateSwapChainGlobal, nullptr);
            VTableHook::Create(&vtable4[15], (LPVOID)DetourCreateSwapChainForHwndGlobal, nullptr);
            HookLog("DX12: Hooked IDXGIFactory4 vtable[10] and vtable[15]");
        }
        pFactory4->Release();
    } else {
        HookLog("DX12: IDXGIFactory4 not available");
    }

    IDXGIFactory6* pFactory6 = nullptr;
    if (SUCCEEDED(pCreateFactory(IID_PPV_ARGS(&pFactory6)))) {
        void** vtable6 = *(void***)pFactory6;
        HookLog("DX12: IDXGIFactory6 available, vtable=%p (IDXGIFactory2=%p, same=%d)", vtable6, vtable,
                (int)(vtable6 == vtable));
        if (vtable6 != vtable) {  // Different vtable pointer
            VTableHook::Create(&vtable6[10], (LPVOID)DetourCreateSwapChainGlobal, nullptr);
            VTableHook::Create(&vtable6[15], (LPVOID)DetourCreateSwapChainForHwndGlobal, nullptr);
            HookLog("DX12: Hooked IDXGIFactory6 vtable[10] and vtable[15]");
        }
        pFactory6->Release();
    } else {
        HookLog("DX12: IDXGIFactory6 not available");
    }

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

    // Mark that we're creating a temp swapchain for hook installation.
    // This prevents the CreateSwapChainForHwnd hooks from capturing the temp
    // queue as g_SwapchainQueue or tracking the temp swapchain.
    g_CreatingTempSwapchain.store(true, std::memory_order_release);

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

    g_CreatingTempSwapchain.store(false, std::memory_order_release);

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
    // CRITICAL: Use same queue preference as ProcessFrame.
    // SL FG: use origGame (SL manages cross-queue internally).
    // FSR FG: use g_SwapchainQueue (FSR's swapchain uses FSR's queue;
    //   submitting on origGame causes cross-queue DEVICE_REMOVED).
    ID3D12CommandQueue* queueForBackend = nullptr;
    {
        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
        bool slFGNow = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        bool fsrFGNow = IsFSRFrameGenerationActive();
        ID3D12CommandQueue* currentCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
        ID3D12CommandQueue* currentPrimaryQueue = g_PrimaryGameQueue.load(std::memory_order_acquire);
        const bool lastWorkingQueueStillActiveDuringRecentTeardown =
            g_PostSLLastWorkingQueue != nullptr &&
            GetTickCount64() < g_PostSLRecentTeardownActivityUntilMs.load(std::memory_order_acquire);

        const auto routingDecision = ce::dx12_overlay_policy::DecideSwapchainOverlayRouting(
            g_FGRuntimeOwnsSwapchain, slFGNow, fsrFGNow, g_HadFSRFGPhase, g_SwapchainQueue != nullptr,
            g_OriginalGameQueue != nullptr, g_PostSLLastWorkingQueue != nullptr,
            currentCommandQueue != nullptr && currentCommandQueue == currentPrimaryQueue);

        if (routingDecision == ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUsePostFSRStreamlineQueue) {
            // After FSR→DLSS: use scQueue (swapchain creation queue)
            queueForBackend = g_SwapchainQueue ? g_SwapchainQueue : g_OriginalGameQueue;
        } else if (routingDecision ==
                   ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUseStreamlineOriginalQueue) {
            // During SL FG, prefer scQueue when it differs from origGame.
            // SL may recreate the swapchain on its own internal queue.
            if (g_SwapchainQueue && g_SwapchainQueue != g_OriginalGameQueue) {
                queueForBackend = g_SwapchainQueue;
            } else if (g_OriginalGameQueue) {
                queueForBackend = g_OriginalGameQueue;
            }
        } else if (routingDecision ==
                   ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUsePostFSRInactiveLastWorkingQueue) {
            if (lastWorkingQueueStillActiveDuringRecentTeardown && g_OriginalGameQueue) {
                queueForBackend = g_OriginalGameQueue;
                static std::atomic<int> s_postFSRBackendOriginalRouteLogCount{0};
                int logCount = s_postFSRBackendOriginalRouteLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 10 || (logCount % 300) == 0) {
                    HookLogImportant(
                        "DX12: InitImGui — deferring preserved PostSL lastWorking queue %p because teardown traffic is "
                        "still active (cmdQ=%p origQ=%p primaryQ=%p)",
                        g_PostSLLastWorkingQueue, currentCommandQueue, g_OriginalGameQueue, currentPrimaryQueue);
                }
            } else {
                queueForBackend = g_PostSLLastWorkingQueue;
            }
        } else if (routingDecision ==
                   ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUsePostFSRInactiveOriginalQueue) {
            if (currentCommandQueue != nullptr && currentCommandQueue == currentPrimaryQueue) {
                queueForBackend = currentCommandQueue;
                static std::atomic<int> s_postFSRBackendPrimaryRouteLogCount{0};
                int logCount = s_postFSRBackendPrimaryRouteLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 10 || (logCount % 300) == 0) {
                    HookLogImportant(
                        "DX12: InitImGui — post-FSR inactive recovery using settled primary queue %p "
                        "instead of origGame %p (cmdQ=%p)",
                        queueForBackend, g_OriginalGameQueue, currentCommandQueue);
                }
            } else {
                queueForBackend = g_OriginalGameQueue;
            }
        } else if (routingDecision == ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUseFSRSwapchainQueue ||
                   routingDecision ==
                       ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUseRuntimeOwnedSwapchainQueue) {
            queueForBackend = g_SwapchainQueue;
        } else if (fsrFGNow && g_OriginalGameQueue) {
            queueForBackend = g_OriginalGameQueue;  // fallback
        } else {
            queueForBackend = g_SwapchainQueue;
            if (!queueForBackend)
                queueForBackend = g_CommandQueue.load();
        }
    }
    HookLogImportant(
        "[Overlay] DX12: InitImGui backend queue=%p (origQ=%p, primaryQ=%p, scQueue=%p, cmdQueue=%p, "
        "lastWorkingQ=%p, slFG=%d, fgCooldown=%d)",
        queueForBackend, g_OriginalGameQueue, g_PrimaryGameQueue.load(std::memory_order_acquire), g_SwapchainQueue,
        (void*)g_CommandQueue.load(), g_PostSLLastWorkingQueue,
        DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) ? 1 : 0, g_FGTransitionCooldown);
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
        HookLogImportant(
            "DX12: DrawOverlay begin (cmdList=%p, bufferIdx=%u, realFrame=%d, overlayInit=%d, syncInit=%d)", cmdList,
            bufferIdx, isRealFrame ? 1 : 0, g_State.overlayInit ? 1 : 0, g_State.syncInit ? 1 : 0);
    }

    // CRITICAL FIX: Always set IPC client regardless of frame type.
    // RenderOverlay() guards on ipc being non-null, so if this was only set
    // on real frames, overlay would never render when isRealFrame is false.
    g_OverlayAdapter.SetIPCClient(g_IPC);
    const auto metricsBinding = ce::dx12_overlay_policy::DecideOverlayMetricsBinding(isRealFrame);
    if (metricsBinding.bindMetrics) {
        g_OverlayAdapter.SetMetrics(DXGIShared::GetPerformanceMetrics());
    }

    if (metricsBinding.refreshFrameMetadata) {
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

    hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc, D3D12_RESOURCE_STATE_COMMON,
                                         &clearVal, IID_PPV_ARGS(&g_State.offscreenRT));
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

static bool InitD3D11On12(ID3D12Device* d3d12Dev, ID3D12CommandQueue* queue, IDXGISwapChain* swapChain,
                          UINT bufferCount) {
    if (g_State.d3d11on12Init)
        return true;

    HookLogImportant("DX12 D3D11On12: Initializing bridge (dev=%p queue=%p bufCnt=%u)", d3d12Dev, queue, bufferCount);

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
    HRESULT hr =
        pfnCreate(d3d12Dev, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, queues, 1, 0, &d3d11Dev, &d3d11Ctx, nullptr);
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
        hr = d3d11on12->CreateWrappedResource(d3d12BB, &flags, D3D12_RESOURCE_STATE_PRESENT,
                                              D3D12_RESOURCE_STATE_PRESENT, IID_PPV_ARGS(&wrapped));
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
        for (auto* r : rtvs)
            if (r)
                r->Release();
        for (auto* w : wrappedBBs)
            if (w)
                w->Release();
        d3d11on12->Release();
        d3d11Ctx->Release();
        d3d11Dev->Release();
        return false;
    }

    // Initialize the SL FG overlay adapter with the D3D11on12 device.
    // This creates the DX11Backend (shaders, font texture, blend states).
    if (!g_SLFGAdapter.InitDX11(d3d11Dev, d3d11Ctx)) {
        HookLogImportant("DX12 D3D11On12: OverlayAdapter.InitDX11 failed");
        for (auto* r : rtvs)
            if (r)
                r->Release();
        for (auto* w : wrappedBBs)
            if (w)
                w->Release();
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

    D3D11_VIEWPORT vp = {0.0f, 0.0f, (float)g_State.cachedWidth, (float)g_State.cachedHeight, 0.0f, 1.0f};
    g_State.d3d11on12Context->RSSetViewports(1, &vp);

    // Feed data to the SL FG overlay adapter
    g_SLFGAdapter.SetIPCClient(g_IPC);
    const auto metricsBinding = ce::dx12_overlay_policy::DecideOverlayMetricsBinding(isRealFrame);
    if (metricsBinding.bindMetrics) {
        g_SLFGAdapter.SetMetrics(DXGIShared::GetPerformanceMetrics());
    }
    if (metricsBinding.refreshFrameMetadata) {
        static const bool s_isVKD3D = []() {
            return GetModuleHandleA("d3d12core.dll") &&
                   (GetModuleHandleA("libvkd3d-1.dll") || GetModuleHandleA("vkd3d.dll"));
        }();
        const char* api = s_isVKD3D ? "DX12 (VKD3D)" : "DX12";
        g_SLFGAdapter.SetGraphicsAPI(api);
    }

    // Render overlay via D3D11 backend (no descriptor heaps!)
    g_SLFGAdapter.RenderOverlay(g_State.cachedWidth, g_State.cachedHeight);

    // Release: internally transitions backbuffer back to PRESENT
    g_State.d3d11on12->ReleaseWrappedResources(&wrapped, 1);

    // Flush submits all D3D11 commands to the D3D12 queue
    g_State.d3d11on12Context->Flush();

    return true;
}

static void CleanupD3D11On12() {
    // Clean up descriptor-free adapter (primary DX12 rendering path)
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
    // Clean up SL FG D3D11On12 adapter
    if (g_SLFGAdapter.IsInitialized()) {
        g_SLFGAdapter.SetShutdownMode(true);
        g_SLFGAdapter.Shutdown();
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

    // Always create with capacity for 8 buffers.  SL's DLSS FG can create new
    // swapchains with more buffers after FG mode switches (e.g., 3→4).  Rather
    // than re-creating the heap each time, we allocate the max upfront.
    constexpr UINT kMaxRTVSlots = 8;
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kMaxRTVSlots,
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
    HookLogImportant("InitOverlaySync: ENTER (syncInit=%d, device=%p, gameQueue=%p)", g_State.syncInit, device,
                     gameQueue);

    if (g_State.syncInit) {
        HookLogImportant("InitOverlaySync: Already initialized, returning early");
        return;
    }

    // CRITICAL: Prefer the queue's own device over g_Device.  After swapchain
    // recreation (e.g. FSR→DLSS switch), g_Device may still point to the old
    // device from the ECL hook while the swapchain queue belongs to a different
    // D3D12 device (SL wraps/creates devices independently).  Submitting command
    // lists created on device A to a queue on device B → DEVICE_REMOVED.
    ID3D12Device* queueDevice = nullptr;
    if (gameQueue) {
        if (SUCCEEDED(gameQueue->GetDevice(IID_PPV_ARGS(&queueDevice))) && queueDevice) {
            if (queueDevice != device) {
                HookLogImportant("InitOverlaySync: Queue device %p DIFFERS from g_Device %p — using queue device",
                                 queueDevice, device);
                device = queueDevice;
            } else {
                HookLogImportant("InitOverlaySync: Queue device matches g_Device (%p)", device);
                queueDevice->Release();
                queueDevice = nullptr;
            }
        }
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
                    HANDLE waitEvent =
                        g_State.fenceEvent ? g_State.fenceEvent : CreateEvent(nullptr, FALSE, FALSE, nullptr);
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
                "InitOverlaySync: Real FG inactive while external overlay %s is present, using single-queue overlay "
                "mode",
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
            D3D12_COMMAND_QUEUE_DESC gameQueueDesc = gameQueue->GetDesc();
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

    // Cross-queue fence: used for two purposes:
    // 1. Dedicated overlay queue: game queue signals before overlay queue starts
    // 2. PostSL cross-queue sync: scQueue signals before overlay draws on game queue
    // Always create regardless of overlayQueue — PostSL needs it after FG transitions
    // when scQueue (SL's FG queue) differs from the overlay submission queue.
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_State.crossQueueFence)))) {
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
    const bool startupOverlayCompatSettled = s_startupOverlayCompatSettled.load(std::memory_order_acquire);
    const bool processNeedsStartupCompatDelay =
        ce::overlay_compat::ShouldPreemptivelyDelayDX12OverlayInitForProcess(g_ProcessName);
    allocatorPoolSize = ce::overlay_compat::GetStartupCompatibleDX12AllocatorPoolSize(
        processNeedsStartupCompatDelay, startupOverlayPresent, IsActualFrameGenerationActive(),
        startupOverlayCompatSettled, DX12OverlayState::ALLOC_POOL_SIZE);
    if (allocatorPoolSize < DX12OverlayState::ALLOC_POOL_SIZE) {
        HookLogImportant("InitOverlaySync: Using minimal %u-slot allocator pool for startup overlay %s",
                         static_cast<unsigned>(allocatorPoolSize), overlayModule ? overlayModule : "module");
    } else if (startupOverlayPresent && startupOverlayCompatSettled) {
        static std::atomic<int> s_startupSettledAllocatorLogCount{0};
        if (s_startupSettledAllocatorLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
            HookLogImportant(
                "InitOverlaySync: Startup overlay %s already settled - keeping full %u-slot allocator pool",
                overlayModule ? overlayModule : "module", static_cast<unsigned>(DX12OverlayState::ALLOC_POOL_SIZE));
        }
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

        // Track which device owns these sync resources so we can detect
        // cross-device submission attempts in PostSLOverlayRender.
        g_State.syncDevice = device;

        g_State.syncInit = true;
        HookLogImportant("InitOverlaySync: SUCCESS (syncDevice=%p, allocators=%d, fence=%p)", device,
                         (int)g_State.allocators.size(), g_State.fence);
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

    // Release the extra ref from QI if we got the device from the queue
    if (queueDevice) {
        queueDevice->Release();
        queueDevice = nullptr;
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
    g_State.syncDevice = nullptr;
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

    // Disable post-SL overlay rendering IMMEDIATELY to prevent rendering
    // to invalidated backbuffers during the resize.
    g_PostSLOverlayActive.store(false, std::memory_order_release);
    ResetPostSLLifecycleForTransition("DX12: swapchain resize", true);
    SetPostSLLastWorkingQueue(nullptr);  // Swapchain resize — rendering setup changed
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

// ============================================================================
// Post-SL FG overlay renderer.
//
// Called from the RE-ENTRANT Present path (dxgi_shared.cpp) — i.e. AFTER
// Streamline's FG pipeline has finished generating/presenting its frames.
// By rendering here we avoid submitting extra ECLs before SL sees Present,
// which is what caused DXGI_ERROR_DEVICE_REMOVED with every previous approach.
//
// Flow:
//   1. Game calls Present → our DetourPresent → ProcessFrame (skips overlay
//      draw because SL FG is active) → calls oPresent (enters SL via E9 JMP)
//   2. SL processes FG → for each output frame SL calls Present via vtable
//   3. Re-entrant DetourPresent → g_PostSLOverlayRenderCallback → THIS function
//   4. We render overlay on the current backbuffer → bypass trampoline → real DXGI Present
//
// This matches RTSS's strategy: overlay is drawn after FG, before the real Present.
//
// KEY DESIGN DECISIONS (confirmed by diagnostics):
//
// 1. DIRECT QUEUE SUBMISSION: We submit ECL via g_RealD3D12ECL(g_RealQueueBehindSLWrapper)
//    instead of slQueue->ExecuteCommandLists().  SL's COM wrapper adds internal
//    metadata per ECL that accumulates and causes DEVICE_REMOVED after ~500-2000
//    frames.  Direct submission bypasses this — proven stable 16,798+ frames.
//
// 2. UAV BARRIERS (not state transitions): We use UAV barriers (global GPU flush)
//    instead of PRESENT→RT / RT→PRESENT state transitions.  State transition type
//    doesn't affect the cumulative crash (confirmed: all barrier types crash at
//    similar timing through SL's wrapper).  UAV barriers avoid resource state
//    tracking conflicts with SL's internal state management.
//
// 3. CACHED FG STATE: g_StreamlineFGRunning is cached ONCE at function entry
//    into cachedSLFGActive.  Reading it multiple times caused mid-function
//    transition races where barrier/queue selection became inconsistent →
//    instant DEVICE_REMOVED on the first inconsistent frame.
//
// 4. FG DEACTIVATION SUSPEND: When cachedSLFGActive transitions true→false,
//    PostSL suspends permanently (s_postSLFGSuspended=true) until FG reactivates.
//    This prevents using stale queue/state from the FG phase.  Pre-SL path
//    takes over for non-FG rendering.
//
// 5. FG "SUSPENSION" FALLBACK: When g_StreamlineFGRunning stays true but SL stops
//    generating re-entrant Present calls (game menu/pause), PostSL never fires.
//    ProcessFrame detects this via g_PostSLStallCounter and falls back to pre-SL.
//    When PostSL fires again (FG resumes), it resets the counter and takes over.
//
// COMPATIBILITY:
//   - GTA V Enhanced: DLSS FG with SL, menu pauses FG (stall fallback needed)
//   - Talos Principle Reawakened: DLSS FG + FSR FG, continuous rendering
//   - Both need the direct queue bypass to avoid cumulative SL damage
// ============================================================================
static void PostSLOverlayRender(IDXGISwapChain* pSwapChain) {
    // --- PostSL per-frame statistics (declared early for lock-skip path) ---
    static std::atomic<int> s_postSLCalls{0};
    static std::atomic<int> s_postSLRenders{0};
    static std::atomic<int> s_postSLSkipLock{0};
    static std::atomic<int> s_postSLSkipFence{0};
    static std::atomic<int> s_postSLSkipOther{0};

    // THREAD SAFETY: During FG, SL may fire Present from multiple threads.
    // Our rendering resources (allocators, command list, descriptor heap) are NOT
    // thread-safe. Use a try-lock to ensure only one thread renders at a time.
    static std::atomic<bool> s_renderLock{false};
    bool expected = false;
    if (!s_renderLock.compare_exchange_strong(expected, true, std::memory_order_acquire)) {
        s_postSLSkipLock.fetch_add(1, std::memory_order_relaxed);
        static int s_lockSkip = 0;
        if (s_lockSkip++ < 10)
            HookLogImportant("DX12: PostSL SKIP — another thread already rendering (tid=0x%04X)", GetCurrentThreadId());
        return;
    }
    // RAII unlock — ensures s_renderLock is released on ALL exit paths
    struct RenderLockGuard {
        std::atomic<bool>& lock;
        ~RenderLockGuard() {
            lock.store(false, std::memory_order_release);
        }
    } lockGuard{s_renderLock};

    // Cache FG state ONCE at function entry to avoid mid-function transition races.
    // g_StreamlineFGRunning can change between reads if FG transitions during PostSL.
    const bool cachedSLFGActive = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    constexpr ULONGLONG kDormantProcessFrameThresholdMs = 100;
    const ULONGLONG nowMs = GetTickCount64();
    const ULONGLONG lastProcessFrameTickMs = g_LastProcessFrameTickMs.load(std::memory_order_acquire);
    const bool processFrameRecentlySeen = lastProcessFrameTickMs != 0 && nowMs >= lastProcessFrameTickMs &&
                                          (nowMs - lastProcessFrameTickMs) < kDormantProcessFrameThresholdMs;

    // --- PostSL periodic stats logging ---
    int callNum = s_postSLCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((callNum % 500) == 0) {
        int renders = s_postSLRenders.load(std::memory_order_relaxed);
        int skipL = s_postSLSkipLock.load(std::memory_order_relaxed);
        int skipF = s_postSLSkipFence.load(std::memory_order_relaxed);
        int skipO = s_postSLSkipOther.load(std::memory_order_relaxed);
        HookLogImportant(
            "DX12: PostSL stats: calls=%d renders=%d skipLock=%d skipFence=%d skipOther=%d (render%%=%.0f%%)", callNum,
            renders, skipL, skipF, skipO, callNum > 0 ? (renders * 100.0 / callNum) : 0.0);
    }

    // Reactivation tracking: log the first N calls after reactivation to diagnose
    // silent early returns.  All early-return paths use HookLog (not in hook_debug.log),
    // so without this, PostSL failures after FG transitions are invisible.
    static int s_reactivationEpoch = 0;
    static int s_callsSinceReactivation = 0;
    static int s_postSLProbeFrames = 0;
    static bool s_wasActive = false;
    static uint32_t s_seenLifecycleEpoch = 0;
    static HANDLE s_dedicatedFenceEvent = nullptr;
    static ID3D12Fence* s_dedicatedSyncFence = nullptr;
    static UINT64 s_dedicatedSyncFenceValue = 0;

    // Streamline signal guard: a real FG shutdown must stop PostSL immediately,
    // but a transient signal drop during reactivation must not permanently strand
    // PostSL in a locally suspended state while synthetic re-entrant Presents are
    // still arriving.
    static bool s_wasSLFGActive = false;
    static bool s_postSLFGSuspended = false;
    const bool postSLActive = g_PostSLOverlayActive.load(std::memory_order_acquire);
    const bool postSLConfirmedRendering = g_PostSLConfirmedRendering.load(std::memory_order_acquire);
    const bool startupActivationPending = g_PostSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
    if (cachedSLFGActive) {
        s_wasSLFGActive = true;
        s_postSLFGSuspended = false;
    } else if (s_wasSLFGActive) {
        s_wasSLFGActive = false;
        s_postSLFGSuspended = ce::dx12_overlay_policy::ShouldLatchPostSLSuspensionOnStreamlineSignalDrop(
            cachedSLFGActive, postSLActive, postSLConfirmedRendering, startupActivationPending);
        HookLogImportant("DX12: PostSL FG signal dropped — %s (active=%d confirmed=%d startupPending=%d)",
                         s_postSLFGSuspended ? "suspending until clean reactivation"
                                             : "treating as transient and waiting for signal recovery",
                         postSLActive ? 1 : 0, postSLConfirmedRendering ? 1 : 0, startupActivationPending ? 1 : 0);
    }
    if (!cachedSLFGActive || s_postSLFGSuspended) {
        s_postSLSkipOther.fetch_add(1, std::memory_order_relaxed);
        static int s_suspendLog = 0;
        if (s_suspendLog < 5 || (s_suspendLog % 500 == 0)) {
            HookLog("DX12: PostSL SKIP — Streamline FG signal inactive (latched=%d frame=%d)",
                    s_postSLFGSuspended ? 1 : 0, s_suspendLog);
        }
        s_suspendLog++;
        return;
    }

    {
        if (ce::dx12_overlay_policy::ShouldSyntheticPostSLAdvanceDormantStartup(
                g_PostSLSyntheticStartupActivationPending.load(std::memory_order_acquire), cachedSLFGActive,
                g_PostSLOverlayActive.load(std::memory_order_acquire), processFrameRecentlySeen)) {
            if (!g_PostSLSyntheticStartupTakeoverLogged.exchange(true, std::memory_order_acq_rel)) {
                HookLogImportant(
                    "DX12: PostSL synthetic startup takeover — ProcessFrame dormant for %llums (cooldown=%d)",
                    lastProcessFrameTickMs != 0 && nowMs >= lastProcessFrameTickMs ? (nowMs - lastProcessFrameTickMs)
                                                                                   : 0,
                    g_PostSLCooldownRemaining.load(std::memory_order_relaxed));
            }

            int cooldownLeft = g_PostSLCooldownRemaining.load(std::memory_order_acquire);
            if (cooldownLeft > 0) {
                g_PostSLCooldownRemaining.store(cooldownLeft - 1, std::memory_order_release);
                if (cooldownLeft > 1) {
                    s_postSLSkipOther.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }

            auto* probeDev = g_Device.load(std::memory_order_acquire);
            if (!g_RealD3D12ECL.load(std::memory_order_acquire) && probeDev && IsStreamlineLoaded()) {
                ProbeRealD3D12ECL(probeDev);
                HookLogImportant("DX12: PostSL synthetic startup activation probed realECL=%p",
                                 (void*)g_RealD3D12ECL.load(std::memory_order_acquire));
            }

            ID3D12CommandQueue* directQueue = g_RealQueueBehindSLWrapper.load(std::memory_order_acquire);
            ExecuteCommandListsPtr directECL = g_RealD3D12ECL.load(std::memory_order_acquire);
            ID3D12CommandQueue* slWrapperQueue = g_SLWrapperQueue.load(std::memory_order_acquire);
            if (ce::dx12_overlay_policy::ShouldDelayPostSLActivationUntilSafeBootstrapPath(
                    g_HadFSRFGPhase, directQueue != nullptr, directECL != nullptr, slWrapperQueue != nullptr)) {
                static int s_waitForSafePathLog = 0;
                if (s_waitForSafePathLog < 10 || (s_waitForSafePathLog % 100) == 0) {
                    HookLogImportant(
                        "DX12: PostSL synthetic startup waiting for safe bootstrap path after FSR phase "
                        "(realQ=%p realECL=%p slWrapper=%p)",
                        directQueue, (void*)directECL, slWrapperQueue);
                }
                s_waitForSafePathLog++;
                s_postSLSkipOther.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            g_PostSLOverlayActive.store(true, std::memory_order_release);
            g_PostSLSyntheticStartupActivationPending.store(false, std::memory_order_release);
            DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(false, std::memory_order_release);
            HookLogImportant("DX12: PostSL synthetic startup activation complete — enabling PostSL rendering");
        }
    }

    uint32_t lifecycleEpoch = g_PostSLLifecycleEpoch.load(std::memory_order_acquire);
    bool lifecycleChanged = lifecycleEpoch != s_seenLifecycleEpoch;
    if (lifecycleChanged) {
        s_wasActive = false;
        s_seenLifecycleEpoch = lifecycleEpoch;
    }

    bool active = g_PostSLOverlayActive.load(std::memory_order_acquire);
    if (ce::dx12_overlay_policy::ShouldTreatPostSLAsReactivated(active, s_wasActive, lifecycleChanged)) {
        s_reactivationEpoch++;
        s_callsSinceReactivation = 0;
        s_postSLProbeFrames = 0;  // Reset probe counter for new reactivation
        // Clean up dedicated queue from previous epochs (no longer used — virtual
        // call through SL's COM wrapper is now the primary submission path).
        ClearPostSLQueues("DX12: PostSL reactivation");
        if (s_dedicatedSyncFence) {
            s_dedicatedSyncFence->Release();
            s_dedicatedSyncFence = nullptr;
        }
        if (s_dedicatedFenceEvent) {
            CloseHandle(s_dedicatedFenceEvent);
            s_dedicatedFenceEvent = nullptr;
        }
        s_dedicatedSyncFenceValue = 0;
        HookLogImportant("DX12: PostSL REACTIVATED (epoch=%d hadFSR=%d origGame=%p)", s_reactivationEpoch,
                         g_HadFSRFGPhase ? 1 : 0, g_OriginalGameQueue);
        // Reset ECL diagnostic counter for fresh diagnostics after transition
        g_PostSLECLDiagCount.store(0, std::memory_order_relaxed);
        // Reset post-FSR probe state for fresh graduated probing
        g_PostFSRProbeLevel = 0;
        g_PostFSRProbeFrames = 0;
        g_PostFSRDescFreeRecreated = false;
    }
    s_wasActive = active;
    s_callsSinceReactivation++;

    // Gate: only render when explicitly enabled (not during cooldown / resize).
    if (!active) {
        s_postSLSkipOther.fetch_add(1, std::memory_order_relaxed);
        static int s_gateSkip = 0;
        if (s_gateSkip++ < 5)
            HookLog("DX12: PostSL SKIP — g_PostSLOverlayActive=false");
        return;
    }

    // Secondary gate: don't render during FG transition cooldown.
    // g_PostSLOverlayActive may be stale if set before ProcessFrame disables it.
    int cooldownLeft = g_PostSLCooldownRemaining.load(std::memory_order_acquire);
    if (cooldownLeft > 0) {
        s_postSLSkipOther.fetch_add(1, std::memory_order_relaxed);
        static int s_cooldownSkip = 0;
        if (s_cooldownSkip++ < 5)
            HookLog("DX12: PostSL SKIP — FG transition cooldown active (%d frames left)", cooldownLeft);
        return;
    }

    // Post-reactivation warm-up: after FG transition reactivation, skip rendering
    // for the first N frames to let DLSS FG's internal pipeline fully stabilize.
    // Observed: first ECL on origGame queue after FSR→DLSS switch causes
    // DEVICE_REMOVED, even with correct queue and no cross-queue sync.
    // Waiting ~30 frames lets SL's FG pipeline establish its internal state.
    constexpr int kPostSLReactivationWarmup = 30;
    if (s_reactivationEpoch > 1 && s_callsSinceReactivation <= kPostSLReactivationWarmup) {
        s_postSLSkipOther.fetch_add(1, std::memory_order_relaxed);
        if (s_callsSinceReactivation <= 5 || s_callsSinceReactivation == kPostSLReactivationWarmup) {
            HookLogImportant("DX12: PostSL warm-up after reactivation epoch=%d frame=%d/%d", s_reactivationEpoch,
                             s_callsSinceReactivation, kPostSLReactivationWarmup);
        }
        return;
    }

    // Use the sync device (the one that created allocators/cmdList/fence) for all
    // per-frame D3D12 operations.  g_Device may have been updated by the ECL hook
    // to a different device pointer (SL wraps devices), causing cross-device
    // CreateRenderTargetView or descriptor heap access → DEVICE_REMOVED.
    auto* dev = g_State.syncDevice;
    if (!dev)
        dev = g_Device.load(std::memory_order_acquire);

    if (dev && ce::dx12_overlay_policy::ShouldBootstrapPostSLOverlayState(cachedSLFGActive, active, g_State.overlayInit,
                                                                          processFrameRecentlySeen)) {
        DXGI_SWAP_CHAIN_DESC bootstrapDesc = {};
        const HRESULT descHr = pSwapChain->GetDesc(&bootstrapDesc);
        if (SUCCEEDED(descHr)) {
            IDXGISwapChain3* bootstrapSc3 = nullptr;
            const HRESULT sc3Hr = pSwapChain->QueryInterface(IID_PPV_ARGS(&bootstrapSc3));
            if (SUCCEEDED(sc3Hr) && bootstrapSc3) {
                ID3D12CommandQueue* bootstrapScQueue = nullptr;
                ID3D12CommandQueue* bootstrapCmdQueue = nullptr;
                ID3D12CommandQueue* bootstrapOrigQueue = nullptr;
                {
                    std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                    bootstrapScQueue = g_SwapchainQueue;
                    bootstrapCmdQueue = g_CommandQueue.load(std::memory_order_acquire);
                    bootstrapOrigQueue = g_OriginalGameQueue;
                }

                g_State.cachedWidth = bootstrapDesc.BufferDesc.Width;
                g_State.cachedHeight = bootstrapDesc.BufferDesc.Height;
                g_State.format = bootstrapDesc.BufferDesc.Format;

                HookLogImportant(
                    "DX12: PostSL bootstrap — rebuilding torn-down overlay state after dormant reactivation "
                    "(fmt=%d buffers=%u hwnd=%p scQueue=%p cmdQueue=%p origQueue=%p)",
                    (int)bootstrapDesc.BufferDesc.Format, bootstrapDesc.BufferCount, bootstrapDesc.OutputWindow,
                    bootstrapScQueue, bootstrapCmdQueue, bootstrapOrigQueue);

                if (InitImGui(dev, (int)bootstrapDesc.BufferCount, bootstrapDesc.BufferDesc.Format,
                              bootstrapDesc.OutputWindow)) {
                    int actualBufferCount = (int)bootstrapDesc.BufferCount;
                    if (actualBufferCount > 8) {
                        actualBufferCount = 8;
                    }
                    CreateRTVs(dev, bootstrapSc3, actualBufferCount);

                    ID3D12CommandQueue* bootstrapQueue = bootstrapScQueue;
                    if (!bootstrapQueue) {
                        bootstrapQueue = bootstrapCmdQueue;
                    }
                    if (!bootstrapQueue) {
                        bootstrapQueue = bootstrapOrigQueue;
                    }

                    if (bootstrapQueue && g_State.rtvDescHeap) {
                        HookLogImportant(
                            "DX12: PostSL bootstrap — inline InitOverlaySync (queue=%p overlayInit=%d syncInit=%d)",
                            bootstrapQueue, g_State.overlayInit ? 1 : 0, g_State.syncInit ? 1 : 0);
                        InitOverlaySync(dev, (int)bootstrapDesc.BufferCount, bootstrapQueue);
                        dev = g_State.syncDevice;
                        if (!dev) {
                            dev = g_Device.load(std::memory_order_acquire);
                        }
                    } else {
                        HookLogImportant(
                            "DX12: PostSL bootstrap — waiting for missing init prerequisites (queue=%p rtvHeap=%p)",
                            bootstrapQueue, g_State.rtvDescHeap);
                    }
                } else {
                    HookLogImportant("DX12: PostSL bootstrap — InitImGui failed (fmt=%d buffers=%u hwnd=%p)",
                                     (int)bootstrapDesc.BufferDesc.Format, bootstrapDesc.BufferCount,
                                     bootstrapDesc.OutputWindow);
                }

                bootstrapSc3->Release();
            } else {
                HookLogImportant("DX12: PostSL bootstrap — swapchain3 query failed hr=0x%08X", (unsigned)sc3Hr);
            }
        } else {
            HookLogImportant("DX12: PostSL bootstrap — swapchain desc unavailable hr=0x%08X", (unsigned)descHr);
        }
    }

    // After FG type transitions, syncInit is reset to force fresh sync resources.
    // PostSL re-initializes inline with the current queue (scQueue or g_CommandQueue).
    if (dev && g_State.overlayInit && !g_State.syncInit) {
        ID3D12CommandQueue* reinitQueue = nullptr;
        {
            std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
            reinitQueue = g_SwapchainQueue;
            if (!reinitQueue)
                reinitQueue = g_CommandQueue.load(std::memory_order_acquire);
        }
        if (reinitQueue) {
            HookLogImportant("DX12: PostSL triggering inline InitOverlaySync (queue=%p dev=%p)", reinitQueue, dev);
            InitOverlaySync(dev, g_State.bufferCount, reinitQueue);
            dev = g_State.syncDevice;
            if (!dev)
                dev = g_Device.load(std::memory_order_acquire);
        }
    }

    if (!dev || !g_State.overlayInit || !g_State.syncInit || !g_State.cmdList || g_State.allocators.empty()) {
        static int s_stateSkip = 0;
        const int stateSkip = s_stateSkip++;
        if (stateSkip < 5 || s_callsSinceReactivation <= 20) {
            HookLogImportant(
                "DX12: PostSL SKIP — state unavailable (epoch=%d call#=%d dev=%p syncDev=%p init=%d sync=%d "
                "list=%p alloc=%d)",
                s_reactivationEpoch, s_callsSinceReactivation, (void*)g_Device.load(), dev, g_State.overlayInit ? 1 : 0,
                g_State.syncInit ? 1 : 0, g_State.cmdList, (int)g_State.allocators.size());
        }
        return;
    }

    // Don't render if device is removed
    if (g_DeviceRemoved.load(std::memory_order_relaxed))
        return;

    HRESULT devReason = dev->GetDeviceRemovedReason();
    if (FAILED(devReason)) {
        g_DeviceRemoved.store(true, std::memory_order_release);
        DXGIShared::g_SharedState.deviceRemovedFatal.store(true, std::memory_order_release);
        HookLogImportant("DX12: PostSLOverlayRender — device removed (0x%08X), disabling", (unsigned)devReason);
        g_PostSLOverlayActive.store(false, std::memory_order_release);
        return;
    }

    // Don't render if swapchain is being resized
    if (DXGIShared::g_SharedState.swapchainInvalid.load(std::memory_order_acquire)) {
        static int s_scInvalid = 0;
        if (s_scInvalid++ < 5)
            HookLog("DX12: PostSL SKIP — swapchainInvalid=true");
        return;
    }

    // After FSR→DLSS: PostSL rendering causes DEVICE_REMOVED. Use graduated
    // probes so we do not jump directly from an empty submit to a full
    // copy-render-copy overlay pass on the first real PostSL frame.

    // Scene transition cooldown: skip overlay during scene loads/transitions
    int cd = g_SceneTransitionCooldown.load(std::memory_order_acquire);
    if (cd > 0) {
        g_SceneTransitionCooldown.store(cd - 1, std::memory_order_release);
        if (cd == 1) {
            ID3D12CommandQueue* resumeQueue = nullptr;
            {
                std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                resumeQueue = g_PostSLLastWorkingQueue;
                if (!resumeQueue)
                    resumeQueue = g_CommandQueue.load(std::memory_order_acquire);
                if (!resumeQueue)
                    resumeQueue = g_SwapchainQueue;
            }
            HookLogImportant(
                "DX12: Post-SL scene transition cooldown complete — resuming overlay "
                "(queue=%p overlayInit=%d syncInit=%d bufCount=%d)",
                resumeQueue, g_State.overlayInit ? 1 : 0, g_State.syncInit ? 1 : 0, g_State.bufferCount);
        }
        return;
    }

    // Get the submission queue for PostSL overlay.
    //
    // CRITICAL: Lock to the first queue that works and DON'T follow g_CommandQueue
    // changes.  During DLSS FG, SL creates internal FG worker queues and starts
    // calling ECL from them.  Our DetourECL hook updates g_CommandQueue to these
    // new SL queues, but they may be COM wrapper/aggregation objects incompatible
    // with realECL.  The game's original queue (captured at the start of FG) is
    // a real D3D12 queue that works with realECL.
    //
    // Queue selection:
    // 1. g_PostSLLockedQueue — if set, always use it (proven working this epoch)
    // 2. g_OriginalGameQueue — the game's very first queue (SL synchronizes with it)
    // 3. g_CommandQueue — last resort, may be SL's internal queue during FG
    //
    // After FG transitions (FSR→DLSS), the NVIDIA driver's internal state for
    // existing queues (including origGame) can become corrupted.  On reactivation
    // PostSL queue selection strategy:
    //
    // PREFER scQueue (swapchain queue): SL transitions the backbuffer to PRESENT
    // on scQueue before calling Present.  By submitting our ECL on scQueue too,
    // D3D12 resource state tracking is correct (same queue = serialized execution).
    // PRESENT→RT and RT→PRESENT barriers work reliably because the before-state
    // matches.  A dedicated queue would break state tracking (different queue
    // doesn't know the resource's current state), causing DEVICE_REMOVED even
    // if the queue itself is healthy.
    //
    // For scQueue, use origECL (SL's original ECL captured from the vtable hook)
    // instead of realECL, because scQueue may be an SL COM wrapper object whose
    // memory layout differs from raw D3D12 CCommandQueue.
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandQueue* scQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
        scQueue = g_SwapchainQueue;

        // Queue selection strategy for PostSL:
        //
        // DLSS FG (no prior FSR FG): use origGame.  It's the swapchain creation
        //   queue with valid NVIDIA driver state and authorized backbuffer access.
        //
        // DLSS FG (after FSR FG was active): prefer the runtime-owned swapchain
        //   queue or a captured direct queue behind SL's wrapper. Keeping PostSL
        //   locked to the wrapper itself can poison long-running FG state and later
        //   crash on teardown, so wrapper use is bootstrap-only at most.
        //
        // Outside SL FG: locked > scQueue > origGame > preFG > cmdQueue
        bool slFGNow = cachedSLFGActive;
        // GTA V's DLSS FG activation triggers a heuristic FSR ghost (brief swapchain
        // queue change) that clears within frames.  Setting hadFSR from heuristic forces
        // PostSL onto SL's internal queues which causes DEVICE_HUNG.
        if (ce::dx12_overlay_policy::ShouldLatchFSRFGHistory(g_FGCompat.IsFSRFGApiActive(), false)) {
            if (!g_HadFSRFGPhase) {
                g_HadFSRFGPhase = true;
                HookLogImportant("DX12: PostSL — FSR FG history confirmed, origGame driver state may be stale");
            }
        }

        ID3D12CommandQueue* directQueueBehindWrapper = g_RealQueueBehindSLWrapper.load(std::memory_order_acquire);
        ID3D12CommandQueue* latestSLWrapperQueue = g_SLWrapperQueue.load(std::memory_order_acquire);
        ID3D12CommandQueue* validatedCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
        ExecuteCommandListsPtr currentRealECL = g_RealD3D12ECL.load(std::memory_order_acquire);
        const bool validatedCommandQueueIsWrapper =
            validatedCommandQueue && validatedCommandQueue != g_OriginalGameQueue && validatedCommandQueue != scQueue;
        ID3D12CommandQueue* wrapperBootstrapQueue = latestSLWrapperQueue;
        if (ce::dx12_overlay_policy::ShouldUseValidatedCommandQueueWrapperBootstrapAfterFSR(
                g_HadFSRFGPhase, slFGNow, directQueueBehindWrapper != nullptr, validatedCommandQueueIsWrapper)) {
            wrapperBootstrapQueue = validatedCommandQueue;
        }
        bool hasDirectQueueBehindWrapper = directQueueBehindWrapper != nullptr;
        bool preferRealQueueBehindWrapper = ce::dx12_overlay_policy::ShouldUsePostSLRealQueueBehindWrapperAfterFSR(
            g_HadFSRFGPhase, slFGNow, hasDirectQueueBehindWrapper);
        const bool preferValidatedDirectQueueForLock =
            ce::dx12_overlay_policy::ShouldPreferValidatedDirectQueueForPostFSRLock(g_HadFSRFGPhase, slFGNow,
                                                                                    hasDirectQueueBehindWrapper);
        bool allowWrapperBootstrapQueue = ce::dx12_overlay_policy::ShouldUsePostSLWrapperBootstrapQueueAfterFSR(
            g_HadFSRFGPhase, slFGNow, hasDirectQueueBehindWrapper, wrapperBootstrapQueue != nullptr);
        const bool lockedQueueIsSLWrapper =
            g_PostSLLockedQueue && g_PostSLLockedQueue != g_OriginalGameQueue && g_PostSLLockedQueue != scQueue;
        ExecuteCommandListsPtr scQueueOrigECL = scQueue ? GetOriginalExecuteCommandLists(scQueue) : nullptr;
        const bool hasSwapchainQueueSubmitPath = scQueue && (scQueueOrigECL != nullptr || currentRealECL != nullptr);
        const bool hasWrapperDerivedDirectPath = directQueueBehindWrapper != nullptr && currentRealECL != nullptr;
        const bool selectDirectQueueInsteadOfLockedWrapper =
            ce::dx12_overlay_policy::ShouldSelectPostSLRealQueueBehindWrapperInsteadOfLockedQueueAfterFSR(
                g_PostSLLockedQueue != nullptr, g_HadFSRFGPhase, slFGNow, lockedQueueIsSLWrapper,
                hasDirectQueueBehindWrapper);
        const bool selectSwapchainQueueInsteadOfLockedWrapper =
            ce::dx12_overlay_policy::ShouldSelectPostSLSwapchainQueueInsteadOfLockedWrapperAfterFSR(
                g_PostSLLockedQueue != nullptr, g_HadFSRFGPhase, slFGNow, lockedQueueIsSLWrapper, scQueue != nullptr,
                scQueue != g_OriginalGameQueue, hasSwapchainQueueSubmitPath, hasWrapperDerivedDirectPath);

        if (preferValidatedDirectQueueForLock && directQueueBehindWrapper) {
            queue = directQueueBehindWrapper;
            static int s_directQueuePreferredLog = 0;
            if (s_directQueuePreferredLog++ < 10) {
                HookLogImportant(
                    "DX12: PostSL queue candidate — validated direct queue %p preferred over scQueue %p after FSR",
                    queue, scQueue);
            }
        } else if (selectDirectQueueInsteadOfLockedWrapper) {
            queue = directQueueBehindWrapper;
            static int s_promoteSelectionLog = 0;
            if (s_promoteSelectionLog++ < 5) {
                HookLog("DX12: PostSL queue candidate — direct real queue %p replacing locked wrapper %p", queue,
                        g_PostSLLockedQueue);
            }
        } else if (selectSwapchainQueueInsteadOfLockedWrapper) {
            queue = scQueue;
            static int s_swapchainSelectionLog = 0;
            if (s_swapchainSelectionLog++ < 10) {
                HookLogImportant(
                    "DX12: PostSL queue candidate — swapchain queue %p replacing locked wrapper %p after FSR", queue,
                    g_PostSLLockedQueue);
            }
        } else if (g_PostSLLockedQueue) {
            queue = g_PostSLLockedQueue;
        } else if (slFGNow) {
            if (preferRealQueueBehindWrapper) {
                queue = directQueueBehindWrapper;
                static int s_realQueueLog = 0;
                if (s_realQueueLog++ < 5) {
                    HookLog("DX12: PostSL queue — realQueueBehindWrapper %p (scQueue=%p hadFSR=%d)", queue, scQueue,
                            g_HadFSRFGPhase ? 1 : 0);
                }
            } else if (allowWrapperBootstrapQueue && wrapperBootstrapQueue &&
                       wrapperBootstrapQueue != g_OriginalGameQueue && wrapperBootstrapQueue != scQueue) {
                queue = wrapperBootstrapQueue;
                static int s_wrapperBootstrapLog = 0;
                if (s_wrapperBootstrapLog++ < 10) {
                    HookLogImportant(
                        "DX12: PostSL queue — wrapper bootstrap %p (validatedCmdQ=%p latestWrapper=%p scQueue=%p "
                        "hadFSR=%d)",
                        queue, validatedCommandQueue, latestSLWrapperQueue, scQueue, g_HadFSRFGPhase ? 1 : 0);
                }
            } else if (scQueue && scQueue != g_OriginalGameQueue) {
                if (g_HadFSRFGPhase) {
                    HookLogImportant(
                        "DX12: PostSL queue — WARNING: falling back to scQueue %p in post-FSR DLSS path "
                        "(origGame=%p, hadFSR=%d, no wrapper/direct queue available)",
                        scQueue, g_OriginalGameQueue, g_HadFSRFGPhase ? 1 : 0);
                }
                queue = scQueue;
                static int s_scQLog = 0;
                if (s_scQLog++ < 5)
                    HookLog("DX12: PostSL queue — scQueue %p (SL swapchain, origGame=%p, hadFSR=%d)", queue,
                            g_OriginalGameQueue, g_HadFSRFGPhase ? 1 : 0);
            } else if (g_OriginalGameQueue) {
                queue = g_OriginalGameQueue;
                static int s_origLog = 0;
                if (s_origLog++ < 5)
                    HookLog("DX12: PostSL queue — origGame %p (slFG, hadFSR=%d)", queue, g_HadFSRFGPhase ? 1 : 0);
            }
        } else if (scQueue) {
            queue = scQueue;
        } else if (g_OriginalGameQueue) {
            queue = g_OriginalGameQueue;
        } else if (g_PreFGGameQueue) {
            queue = g_PreFGGameQueue;
        } else {
            queue = g_CommandQueue.load(std::memory_order_acquire);
        }

        // AddRef the selected queue under the mutex to prevent it from being
        // freed by DX12_SetCommandQueue (which also uses this mutex) or SL's
        // internal cleanup while we use it.  Released by scope guard below.
        if (queue)
            queue->AddRef();
    }
    // Scope guard ensures Release on all exit paths
    auto queueReleaseGuard = ce::make_scope_guard([&]() {
        if (queue)
            queue->Release();
    });

    if (!queue) {
        static int s_noQueue = 0;
        if (s_noQueue++ < 5)
            HookLog("DX12: PostSL SKIP — no queue (cmdQueue=%p scQueue=%p)", (void*)g_CommandQueue.load(),
                    g_SwapchainQueue);
        return;
    }

    // Lock to the selected queue for the current epoch, but allow a one-time
    // post-FSR migration from the wrapper bootstrap queue to the captured real
    // queue behind it once the ECL detour has observed that path.
    {
        ID3D12CommandQueue* oldLockedQueue = nullptr;
        bool lockedQueueWasUpdated = false;
        bool shouldKeepExistingLockedQueue = false;
        {
            std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
            ID3D12CommandQueue* directQueueBehindWrapper = g_RealQueueBehindSLWrapper.load(std::memory_order_acquire);
            ExecuteCommandListsPtr currentRealECL = g_RealD3D12ECL.load(std::memory_order_acquire);
            ExecuteCommandListsPtr lockedScQueueOrigECL = scQueue ? GetOriginalExecuteCommandLists(scQueue) : nullptr;
            const bool lockedQueueIsSLWrapper =
                g_PostSLLockedQueue && g_PostSLLockedQueue != g_OriginalGameQueue && g_PostSLLockedQueue != scQueue;
            const bool hasSwapchainQueueSubmitPath =
                scQueue && (lockedScQueueOrigECL != nullptr || currentRealECL != nullptr);
            const bool hasWrapperDerivedDirectPath = directQueueBehindWrapper != nullptr && currentRealECL != nullptr;
            bool shouldReplaceLockedQueue =
                ce::dx12_overlay_policy::ShouldBootstrapPostSLRealQueueBehindWrapperAfterFSR(
                    g_HadFSRFGPhase, cachedSLFGActive, lockedQueueIsSLWrapper, directQueueBehindWrapper != nullptr) &&
                queue == directQueueBehindWrapper;
            shouldReplaceLockedQueue =
                shouldReplaceLockedQueue ||
                (ce::dx12_overlay_policy::ShouldSelectPostSLSwapchainQueueInsteadOfLockedWrapperAfterFSR(
                     g_PostSLLockedQueue != nullptr, g_HadFSRFGPhase, cachedSLFGActive, lockedQueueIsSLWrapper,
                     scQueue != nullptr, scQueue != g_OriginalGameQueue, hasSwapchainQueueSubmitPath,
                     hasWrapperDerivedDirectPath) &&
                 queue == scQueue);
            const bool selectedQueueMatchesLockedQueue = queue == g_PostSLLockedQueue;

            if (ce::dx12_overlay_policy::ShouldMutatePostSLLockedQueue(
                    g_PostSLLockedQueue != nullptr, selectedQueueMatchesLockedQueue, shouldReplaceLockedQueue)) {
                oldLockedQueue = g_PostSLLockedQueue;
                g_PostSLLockedQueue = queue;
                queue->AddRef();  // prevent locked queue from being freed between PostSL calls
                lockedQueueWasUpdated = true;

                if (oldLockedQueue) {
                    if (queue == directQueueBehindWrapper) {
                        HookLogImportant(
                            "DX12: PostSL promoting locked queue %p -> real queue behind wrapper %p after post-FSR "
                            "bootstrap",
                            oldLockedQueue, directQueueBehindWrapper);
                    } else {
                        HookLogImportant(
                            "DX12: PostSL replacing locked queue %p -> swapchain queue %p after post-FSR direct path "
                            "recovery",
                            oldLockedQueue, queue);
                    }
                } else {
                    bool usingSLWrapper = (queue != g_OriginalGameQueue && queue != scQueue);
                    bool slFGAtLock = cachedSLFGActive;
                    HookLogImportant(
                        "DX12: PostSL locked to queue %p (origGame=%p scQueue=%p cmdQueue=%p preFG=%p epoch=%d "
                        "slWrapper=%d slFG=%d hadFSR=%d)",
                        queue, g_OriginalGameQueue, scQueue, (void*)g_CommandQueue.load(), g_PreFGGameQueue,
                        s_reactivationEpoch, usingSLWrapper ? 1 : 0, slFGAtLock ? 1 : 0, g_HadFSRFGPhase ? 1 : 0);
                }
            } else if (!selectedQueueMatchesLockedQueue) {
                shouldKeepExistingLockedQueue = true;
                queue->Release();  // Release per-call AddRef on the rejected queue
                queue = g_PostSLLockedQueue;
                if (queue) {
                    queue->AddRef();  // Per-call AddRef on the locked queue instead
                }
            }
        }

        if (oldLockedQueue) {
            oldLockedQueue->Release();
        }

        if (shouldKeepExistingLockedQueue && queue) {
            ID3D12CommandQueue* newCmdQueue = g_CommandQueue.load(std::memory_order_acquire);
            HookLogImportant("DX12: PostSL REFUSING queue change: locked=%p, cmdQueue=%p (changed!), scQueue=%p", queue,
                             newCmdQueue, scQueue);
        }

        if (!queue) {
            static int s_missingLockedQueue = 0;
            if (s_missingLockedQueue++ < 5) {
                HookLogImportant("DX12: PostSL SKIP — locked queue disappeared during synchronized selection");
            }
            return;
        }
    }

    // CRITICAL: Verify device compatibility before using sync resources.
    // After swapchain recreation (e.g. FSR→DLSS switch), the submission queue may
    // belong to a different D3D12 device than the one used to create allocators,
    // command list, and fence in InitOverlaySync.  Cross-device ECL submission
    // causes DEVICE_REMOVED.  Detect this and force full re-initialization.
    if (g_State.syncDevice) {
        // Belt-and-suspenders: verify queue vtable is intact before virtual call.
        // The AddRef above should keep the queue alive, but if something else
        // (SL internal cleanup) bypassed COM refcounting, the vtable may be gone.
        void* vtbl = *reinterpret_cast<void* volatile*>(queue);
        if (!vtbl) {
            HookLogImportant("DX12: PostSL SKIP — queue %p has null vtable (freed?), clearing lock", queue);
            ClearPostSLQueues("DX12: PostSL null vtable");
            return;
        }
        ID3D12Device* queueDevice = nullptr;
        if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) && queueDevice) {
            if (queueDevice != g_State.syncDevice) {
                HookLogImportant(
                    "DX12: PostSL DEVICE MISMATCH! queue=%p queueDev=%p != syncDev=%p — "
                    "forcing overlay re-init to prevent cross-device DEVICE_REMOVED",
                    queue, queueDevice, g_State.syncDevice);
                queueDevice->Release();
                // Force full re-initialization on next ProcessFrame
                g_State.overlayInit = false;
                g_State.syncInit = false;
                g_State.syncDevice = nullptr;
                ClearPostSLQueues("DX12: PostSL device mismatch");
                ClearPostSLPinnedSLWrapperQueue("DX12: PostSL device mismatch");
                SetPostSLLastWorkingQueue(nullptr);  // Cross-device — old queue invalid
                return;
            }
            queueDevice->Release();
        }
    }

    // Get current backbuffer from the re-entrant swapchain
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) {
        if (s_callsSinceReactivation <= 20)
            HookLogImportant("DX12: PostSL EARLY-EXIT: QI for IDXGISwapChain3 failed (call#%d)",
                             s_callsSinceReactivation);
        return;
    }

    UINT bufIdx = sc3->GetCurrentBackBufferIndex();
    ID3D12Resource* bb = nullptr;
    HRESULT getBufHr = sc3->GetBuffer(bufIdx, IID_PPV_ARGS(&bb));
    sc3->Release();
    if (FAILED(getBufHr) || !bb) {
        if (s_callsSinceReactivation <= 20)
            HookLogImportant("DX12: PostSL EARLY-EXIT: GetBuffer(%u) failed hr=0x%08X (call#%d)", bufIdx, getBufHr,
                             s_callsSinceReactivation);
        return;
    }

    // Validate buffer index against current overlay state.
    // After FG mode switches, SL may create a new swapchain with more buffers
    // (e.g., 3→4 for DLSS FG triple buffering).  g_State.bufferCount reflects
    // the count at init time and may be stale.  Dynamically expand to match.
    if (bufIdx >= (UINT)g_State.bufferCount) {
        if (bufIdx < 8) {
            int newCount = (int)bufIdx + 1;
            HookLogImportant("DX12: PostSL expanding bufferCount %d -> %d (bufIdx=%u from swapchain)",
                             g_State.bufferCount, newCount, bufIdx);
            g_State.bufferCount = newCount;
        } else {
            if (s_callsSinceReactivation <= 20)
                HookLogImportant("DX12: PostSL EARLY-EXIT: bufIdx=%u too large (>8) (call#%d)", bufIdx,
                                 s_callsSinceReactivation);
            bb->Release();
            return;
        }
    }

    // Pick an allocator from the pool (round-robin)
    int allocPoolSize = static_cast<int>(g_State.allocators.size());
    int idx = g_State.allocIndex % allocPoolSize;
    g_State.allocIndex = (idx + 1) % allocPoolSize;

    auto* list = g_State.cmdList;
    auto* alloc = (idx < allocPoolSize) ? g_State.allocators[idx] : nullptr;
    if (!list || !alloc) {
        if (s_callsSinceReactivation <= 20)
            HookLogImportant("DX12: PostSL EARLY-EXIT: list=%p alloc=%p (idx=%d poolSize=%d call#%d)", list, alloc, idx,
                             allocPoolSize, s_callsSinceReactivation);
        bb->Release();
        return;
    }

    // Fence check: ensure allocator's GPU work is complete before reset.
    // Uses event-based wait (SetEventOnCompletion + WaitForSingleObject) instead
    // of instant bail — at 100% GPU load, the allocator may be just microseconds
    // from completing, and a skip causes visible overlay flicker.  Event-based
    // wait has zero CPU overhead (thread sleeps until GPU signals) with a 1ms
    // timeout cap to avoid blocking the game.
    if (g_State.fence && idx < (int)g_State.fenceValues.size() && g_State.fenceValues[idx] > 0) {
        UINT64 completed = g_State.fence->GetCompletedValue();
        if (completed < g_State.fenceValues[idx]) {
            // Reusable event handle — created once, persists for the DLL lifetime
            static HANDLE s_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            bool fenceReady = false;
            if (s_fenceEvent) {
                HRESULT evHr = g_State.fence->SetEventOnCompletion(g_State.fenceValues[idx], s_fenceEvent);
                if (SUCCEEDED(evHr)) {
                    DWORD waitResult = WaitForSingleObject(s_fenceEvent, 1);  // 1ms max
                    completed = g_State.fence->GetCompletedValue();
                    fenceReady = (completed >= g_State.fenceValues[idx]);

                    static int s_fenceWaitLog = 0;
                    if (fenceReady && s_fenceWaitLog++ < 10)
                        HookLogImportant(
                            "DX12: PostSL fence wait resolved via event (alloc[%d] completed=%llu needed=%llu "
                            "waitResult=%lu)",
                            idx, completed, g_State.fenceValues[idx], waitResult);
                }
            }
            if (!fenceReady) {
                s_postSLSkipFence.fetch_add(1, std::memory_order_relaxed);
                if (s_callsSinceReactivation <= 20)
                    HookLogImportant(
                        "DX12: PostSL EARLY-EXIT: alloc[%d] in-flight after 1ms wait (completed=%llu needed=%llu "
                        "call#%d)",
                        idx, completed, g_State.fenceValues[idx], s_callsSinceReactivation);
                bb->Release();
                return;
            }
        }
    }

    HRESULT allocResetHr = alloc->Reset();
    if (FAILED(allocResetHr)) {
        if (s_callsSinceReactivation <= 20)
            HookLogImportant("DX12: PostSL EARLY-EXIT: alloc->Reset failed hr=0x%08X (call#%d)", allocResetHr,
                             s_callsSinceReactivation);
        bb->Release();
        return;
    }
    HRESULT listResetHr = list->Reset(alloc, nullptr);
    if (FAILED(listResetHr)) {
        if (s_callsSinceReactivation <= 20)
            HookLogImportant("DX12: PostSL EARLY-EXIT: list->Reset failed hr=0x%08X (call#%d)", listResetHr,
                             s_callsSinceReactivation);
        bb->Release();
        return;
    }

    // Pre-submit device health check: if device is already removed (e.g. SL's
    // FG re-init failed), bail early instead of causing a cascade crash.
    {
        HRESULT preDevHr = dev->GetDeviceRemovedReason();
        if (FAILED(preDevHr)) {
            HookLogImportant(
                "DX12: PostSL EARLY-EXIT: device already removed BEFORE submit "
                "(hr=0x%08X epoch=%d call#%d)",
                preDevHr, s_reactivationEpoch, s_callsSinceReactivation);
            bb->Release();
            return;
        }
    }

    bool rendered = false;

    const bool selectedQueueIsSwapchainQueue = (queue == scQueue);
    ExecuteCommandListsPtr realECL = g_RealD3D12ECL.load(std::memory_order_acquire);
    ID3D12CommandQueue* realQ = g_RealQueueBehindSLWrapper.load(std::memory_order_acquire);
    ExecuteCommandListsPtr selectedQueueOrigECL = GetOriginalExecuteCommandLists(queue);
    const bool selectedQueueOrigECLMatchesRealECL = selectedQueueOrigECL && selectedQueueOrigECL == realECL;
    bool isSLWrapperQ = ce::dx12_overlay_policy::ShouldTreatPostSLSelectedQueueAsWrapper(
        queue == g_OriginalGameQueue, queue == g_PostSLDedicatedQueue, selectedQueueIsSwapchainQueue,
        selectedQueueOrigECLMatchesRealECL);
    const bool useExplicitPostFSRSwapchainTransitions =
        ce::dx12_overlay_policy::ShouldUseExplicitBackbufferTransitionsForPostFSRSwapchainQueuePath(
            g_HadFSRFGPhase, cachedSLFGActive, selectedQueueIsSwapchainQueue, isSLWrapperQ);
    const bool usePostSLOffscreenComposite = ce::dx12_overlay_policy::ShouldUsePostSLOffscreenCompositeAfterFSR(
        g_HadFSRFGPhase, cachedSLFGActive, selectedQueueIsSwapchainQueue, isSLWrapperQ);
    const bool useExplicitPostFSRBackbufferCopyTransitions =
        ce::dx12_overlay_policy::ShouldUseExplicitBackbufferCopyTransitionsForPostFSROffscreenComposite(
            usePostSLOffscreenComposite, useExplicitPostFSRSwapchainTransitions);
    const bool hasSelectedQueueSubmitPath = selectedQueueOrigECL != nullptr || realECL != nullptr;
    const bool hasWrapperDerivedDirectPath = realQ != nullptr && realECL != nullptr;
    const bool preferSelectedSwapchainQueueSubmitAfterFSR =
        ce::dx12_overlay_policy::ShouldUsePostSLSelectedSwapchainQueueSubmitAfterFSR(
            g_HadFSRFGPhase, selectedQueueIsSwapchainQueue, isSLWrapperQ, hasSelectedQueueSubmitPath,
            hasWrapperDerivedDirectPath);
    const bool preferSelectedQueueDirectSubmitAfterFSR =
        ce::dx12_overlay_policy::ShouldUsePostSLSelectedQueueDirectSubmitAfterFSR(
            g_HadFSRFGPhase, selectedQueueIsSwapchainQueue, selectedQueueOrigECL != nullptr,
            selectedQueueOrigECLMatchesRealECL, realQ != nullptr);

    // --- Post-FSR graduated probing ---
    // Level 0: Scratch resource barrier (confirms queue/device path works)
    // Level 1: Reserved for future backbuffer-specific probes
    // Level 2: Offscreen copy-only pass (touch swapchain only via copy ops)
    // Level 3+: Full offscreen composite/render is allowed
    bool isPostFSRProbe = g_HadFSRFGPhase && g_PostFSRProbeLevel < 3;

    // For post-FSR rendering, use SL's wrapper queue captured from ECL detour.
    // origGame's driver-internal state tracking for FSR-created swapchain backbuffers is
    // invalid (FSR created the swapchain on its own queue, origGame never saw the backbuffers).
    // SL's wrapper queue dispatches through SL's ECL interception which knows the correct state.
    ID3D12CommandQueue* slWrapperQueue = nullptr;
    ID3D12CommandQueue* liveSLWrapperQueue = nullptr;
    bool usingPinnedPostFSRWrapperQueue = false;
    if (g_HadFSRFGPhase) {
        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
        liveSLWrapperQueue = g_SLWrapperQueue.load(std::memory_order_acquire);

        ID3D12CommandQueue* pinnedSLWrapperQueue = g_PostSLPinnedSLWrapperQueue;
        ID3D12CommandQueue* wrapperCandidate = pinnedSLWrapperQueue ? pinnedSLWrapperQueue : liveSLWrapperQueue;
        if (!wrapperCandidate) {
            // Fallback: try g_CommandQueue if it's not origGame or scQueue.
            ID3D12CommandQueue* cmdQ = g_CommandQueue.load(std::memory_order_acquire);
            if (cmdQ && cmdQ != g_OriginalGameQueue && cmdQ != g_SwapchainQueue)
                wrapperCandidate = cmdQ;
        }
        if (wrapperCandidate == g_OriginalGameQueue || wrapperCandidate == g_SwapchainQueue)
            wrapperCandidate = nullptr;

        if (ce::dx12_overlay_policy::ShouldPinPostSLWrapperQueueAfterFSR(
                g_HadFSRFGPhase, usePostSLOffscreenComposite, selectedQueueIsSwapchainQueue,
                pinnedSLWrapperQueue != nullptr, wrapperCandidate != nullptr,
                preferSelectedSwapchainQueueSubmitAfterFSR)) {
            wrapperCandidate->AddRef();
            g_PostSLPinnedSLWrapperQueue = wrapperCandidate;
            pinnedSLWrapperQueue = wrapperCandidate;
            usingPinnedPostFSRWrapperQueue = true;
            HookLogImportant("DX12: PostSL pinned post-FSR SL wrapper queue %p for epoch=%d (source=%s scQueue=%p)",
                             wrapperCandidate, s_reactivationEpoch,
                             liveSLWrapperQueue ? "captured" : "cmdQueue-fallback", scQueue);
        } else {
            usingPinnedPostFSRWrapperQueue = pinnedSLWrapperQueue != nullptr;
        }

        slWrapperQueue = pinnedSLWrapperQueue ? pinnedSLWrapperQueue : wrapperCandidate;
        if (slWrapperQueue)
            slWrapperQueue->AddRef();
    }
    auto slWrapperQueueReleaseGuard = ce::make_scope_guard([&]() {
        if (slWrapperQueue)
            slWrapperQueue->Release();
    });

    if (isPostFSRProbe) {
        // Log comprehensive diagnostics on first probe frame
        if (g_PostFSRProbeFrames == 0 && g_PostFSRProbeLevel == 0) {
            D3D12_RESOURCE_DESC bbDesc = bb->GetDesc();
            HookLogImportant("DX12: PostSL post-FSR DIAG: pSwapChain=%p bb=%p bufIdx=%u bbW=%u bbH=%u", pSwapChain, bb,
                             bufIdx, (unsigned)bbDesc.Width, bbDesc.Height);
            HookLogImportant("DX12: PostSL post-FSR DIAG: queue=%p origGame=%p slWrapper=%p scQ=%p", queue,
                             g_OriginalGameQueue, slWrapperQueue, g_SwapchainQueue);
        }

        bool probeHandled = true;
        const bool preferRealQueueBehindWrapperAfterFSR =
            ce::dx12_overlay_policy::ShouldUsePostSLRealQueueBehindWrapperAfterFSR(g_HadFSRFGPhase, cachedSLFGActive,
                                                                                   realQ != nullptr);
        const bool bootstrapRealQueueCaptureViaWrapperProbe =
            ce::dx12_overlay_policy::ShouldBootstrapPostSLRealQueueCaptureViaWrapperProbeAfterFSR(
                g_HadFSRFGPhase, cachedSLFGActive, g_PostFSRProbeLevel, realQ != nullptr, slWrapperQueue != nullptr,
                hasSelectedQueueSubmitPath, isSLWrapperQ);
        if (preferRealQueueBehindWrapperAfterFSR && g_PostFSRProbeLevel >= 2) {
            g_PostFSRProbeLevel = 3;
            g_PostFSRProbeFrames = 0;
            HookLogImportant(
                "DX12: PostSL post-FSR switching to direct real queue behind wrapper %p — skipping level 2 probe",
                realQ);
            bb->Release();
            return;
        }
        // CRITICAL: Always use the locked queue (stable across frames) for probe
        // submissions, NOT the transient slWrapperQueue (g_SLWrapperQueue) which
        // changes as different SL wrapper queues are seen by the ECL detour on
        // other threads.  Using a transient wrapper mid-probe causes DEVICE_REMOVED
        // when the new wrapper doesn't own the swapchain's resource state.
        ID3D12CommandQueue* probeQueue =
            bootstrapRealQueueCaptureViaWrapperProbe
                ? queue
                : ((g_PostFSRProbeLevel >= 1 && slWrapperQueue && !preferSelectedSwapchainQueueSubmitAfterFSR)
                       ? slWrapperQueue
                       : queue);

        if (g_PostFSRProbeLevel == 0) {
            // Probe 0: Scratch resource barrier on origGame — confirms queue works.
            D3D12_HEAP_PROPERTIES heapProps = {};
            heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC scratchDesc = {};
            scratchDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            scratchDesc.Width = 64;
            scratchDesc.Height = 64;
            scratchDesc.DepthOrArraySize = 1;
            scratchDesc.MipLevels = 1;
            scratchDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            scratchDesc.SampleDesc.Count = 1;
            scratchDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            ID3D12Resource* scratch = nullptr;
            HRESULT scratchHr =
                dev->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &scratchDesc,
                                             D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&scratch));
            if (SUCCEEDED(scratchHr) && scratch) {
                D3D12_RESOURCE_BARRIER barriers[2] = {};
                barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barriers[0].Transition.pResource = scratch;
                barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barriers[1].Transition.pResource = scratch;
                barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                list->ResourceBarrier(2, barriers);
                scratch->Release();
            }
        } else if (g_PostFSRProbeLevel == 1) {
            // Probe 1: PRESENT→RT→PRESENT on backbuffer via SL's wrapper queue.
            // SL's ECL interception dispatches to its internal queue which has correct
            // resource state tracking for the swapchain backbuffers.
            D3D12_RESOURCE_BARRIER barriers[2] = {};
            barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[0].Transition.pResource = bb;
            barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[1].Transition.pResource = bb;
            barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(2, barriers);
        } else if (ce::dx12_overlay_policy::ShouldUsePostSLOffscreenCopyOnlyProbeAfterFSR(
                       g_HadFSRFGPhase, g_PostFSRProbeLevel, usePostSLOffscreenComposite,
                       selectedQueueIsSwapchainQueue)) {
            if (!EnsureOffscreenRT(dev, g_State.cachedWidth, g_State.cachedHeight, g_State.format)) {
                HookLogImportant(
                    "DX12: PostSL post-FSR copy-only probe could not create offscreen RT (w=%d h=%d fmt=%d)",
                    g_State.cachedWidth, g_State.cachedHeight, g_State.format);
                bb->Release();
                return;
            }

            D3D12_RESOURCE_BARRIER toCopyDest = {};
            toCopyDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toCopyDest.Transition.pResource = g_State.offscreenRT;
            toCopyDest.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            toCopyDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            toCopyDest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1, &toCopyDest);

            D3D12_RESOURCE_BARRIER bbToCopySource = {};
            if (useExplicitPostFSRBackbufferCopyTransitions) {
                bbToCopySource.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bbToCopySource.Transition.pResource = bb;
                bbToCopySource.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                bbToCopySource.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                bbToCopySource.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                list->ResourceBarrier(1, &bbToCopySource);
            }

            D3D12_TEXTURE_COPY_LOCATION bbSrc = {};
            bbSrc.pResource = bb;
            bbSrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            bbSrc.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION offDst = {};
            offDst.pResource = g_State.offscreenRT;
            offDst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            offDst.SubresourceIndex = 0;
            list->CopyTextureRegion(&offDst, 0, 0, 0, &bbSrc, nullptr);

            D3D12_RESOURCE_BARRIER toCopySource = {};
            toCopySource.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toCopySource.Transition.pResource = g_State.offscreenRT;
            toCopySource.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            toCopySource.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            toCopySource.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1, &toCopySource);

            D3D12_RESOURCE_BARRIER bbToCopyDest = {};
            if (useExplicitPostFSRBackbufferCopyTransitions) {
                bbToCopyDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bbToCopyDest.Transition.pResource = bb;
                bbToCopyDest.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                bbToCopyDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                bbToCopyDest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                list->ResourceBarrier(1, &bbToCopyDest);
            }

            D3D12_TEXTURE_COPY_LOCATION offSrc = {};
            offSrc.pResource = g_State.offscreenRT;
            offSrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            offSrc.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION bbDst = {};
            bbDst.pResource = bb;
            bbDst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            bbDst.SubresourceIndex = 0;
            list->CopyTextureRegion(&bbDst, 0, 0, 0, &offSrc, nullptr);

            if (useExplicitPostFSRBackbufferCopyTransitions) {
                D3D12_RESOURCE_BARRIER bbToPresent = {};
                bbToPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bbToPresent.Transition.pResource = bb;
                bbToPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                bbToPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                bbToPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                list->ResourceBarrier(1, &bbToPresent);
            }
        } else if (g_PostFSRProbeLevel == 2) {
            probeHandled = false;
        }

        if (probeHandled) {
            list->Close();
            ID3D12CommandList* lists[] = {list};
            // Keep probe submission on the queue that actually owns the tested path.
            // Post-FSR copy probes have only been observed to survive when routed
            // through the SL wrapper path rather than forcing an immediate direct
            // queue handoff.
            if (bootstrapRealQueueCaptureViaWrapperProbe && isSLWrapperQ) {
                s_insidePostSLOverlayECL = true;
                probeQueue->ExecuteCommandLists(1, lists);
                s_insidePostSLOverlayECL = false;
            } else if (isSLWrapperQ && !g_RealQueueBehindSLWrapper.load(std::memory_order_acquire)) {
                s_insidePostSLOverlayECL = true;
                probeQueue->ExecuteCommandLists(1, lists);
                s_insidePostSLOverlayECL = false;
                ID3D12CommandQueue* capturedReal = g_RealQueueBehindSLWrapper.load(std::memory_order_acquire);
                if (capturedReal) {
                    HookLogImportant("DX12: PostSL post-FSR probe captured real queue %p behind wrapper bootstrap %p",
                                     capturedReal, probeQueue);
                }
            } else {
                probeQueue->ExecuteCommandLists(1, lists);
            }

            if (g_State.fence) {
                UINT64 next = g_State.currentFenceValue + 1;
                HRESULT sigHr = probeQueue->Signal(g_State.fence, next);
                if (SUCCEEDED(sigHr)) {
                    g_State.currentFenceValue = next;
                    if (idx >= 0 && idx < (int)g_State.fenceValues.size())
                        g_State.fenceValues[idx] = next;
                }
            }

            HRESULT probeHr = dev->GetDeviceRemovedReason();
            g_PostFSRProbeFrames++;
            const char* probeNames[] = {"scratch-barrier", "SLwrapper-bb-barrier", "offscreen-copy-only"};
            const char* probeName = g_PostFSRProbeLevel < 3 ? probeNames[g_PostFSRProbeLevel] : "unknown";
            HookLogImportant("DX12: PostSL post-FSR PROBE level=%d (%s) frame=%d/%d queue=%p devRemoved=0x%08X %s",
                             g_PostFSRProbeLevel, probeName, g_PostFSRProbeFrames, kPostFSRProbeFramesPerLevel,
                             probeQueue, probeHr, FAILED(probeHr) ? "FAILED" : "OK");

            if (FAILED(probeHr)) {
                // DEVICE_REMOVED from BB barrier is FATAL — skip to barrier-free.
                // Scratch barrier failures are non-fatal (queue just isn't ready).
                int skipTo = (g_PostFSRProbeLevel >= 1) ? 2 : g_PostFSRProbeLevel + 1;
                g_PostFSRProbeLevel = skipTo;
                g_PostFSRProbeFrames = 0;
                HookLogImportant("DX12: PostSL post-FSR probe FAILED, advancing to level %d", g_PostFSRProbeLevel);
                bb->Release();
                return;
            }

            if (g_PostFSRProbeFrames >= kPostFSRProbeFramesPerLevel) {
                // Skip level 1 (BB barrier): go directly from level 0 to level 2.
                // BB barriers cause FATAL DEVICE_REMOVED on queues that don't own the
                // swapchain's resource state. Level 2 only validates copy traffic on
                // the swapchain timeline before any real overlay rendering is attempted.
                int nextLevel =
                    (g_PostFSRProbeLevel == 0) ? (selectedQueueIsSwapchainQueue ? 3 : 2) : g_PostFSRProbeLevel + 1;
                g_PostFSRProbeLevel = nextLevel;
                g_PostFSRProbeFrames = 0;
                HookLogImportant(
                    "DX12: PostSL post-FSR probe PASSED, advancing to level %d (selectedScQueue=%d skipped BB barrier "
                    "probe)",
                    g_PostFSRProbeLevel, selectedQueueIsSwapchainQueue ? 1 : 0);
            }
            bb->Release();
            return;
        }
    }

    // Post-FSR: the DescFree backend contains device-level objects (PSO, root sig)
    // that work on any queue. Format mismatch is handled below (~line 4325).
    // No need to force-destroy — just reuse the existing backend.

    // Lazy-init DescFree backend if needed (same logic as pre-SL path at ~5479).
    // After overlay reinit during FG transitions, g_DescFreeBackend is destroyed
    // but the pre-SL draw is suppressed (can't recreate it there during SL FG).
    // PostSL must create it itself.
    if (!g_DescFreeBackend) {
        if (dev) {
            auto* backend = new DX12DescFreeBackend();
            if (backend->InitDevice(dev, g_State.format)) {
                g_DescFreeBackend = backend;
                g_D3D11On12Adapter.InitCustom(g_DescFreeBackend, OverlayBackendType::DX12);
                HookLogImportant("DX12: PostSL created descriptor-free overlay backend (fmt=%d)", g_State.format);
            } else {
                delete backend;
                HookLogImportant("DX12: PostSL descriptor-free backend init FAILED (fmt=%d)", g_State.format);
            }
        }
    }

    bool willRender = g_DescFreeBackend && g_D3D11On12Adapter.IsInitialized();

    // Validate backbuffer format matches DescFree PSO format.
    // After FG transitions the swapchain may be recreated with a different format.
    // PSO/RTV format mismatch causes DEVICE_REMOVED.
    if (willRender && g_DescFreeBackend) {
        D3D12_RESOURCE_DESC bbDesc = bb->GetDesc();
        DXGI_FORMAT bbFmt = bbDesc.Format;
        DXGI_FORMAT psoFmt = g_State.format;
        static int s_fmtLogCount = 0;
        if (s_fmtLogCount < 5 || (bbFmt != psoFmt && s_fmtLogCount < 50)) {
            HookLogImportant("DX12: PostSL format check — backbuffer=%d psoFmt=%d %s", (int)bbFmt, (int)psoFmt,
                             bbFmt == psoFmt ? "MATCH" : "MISMATCH");
            s_fmtLogCount++;
        }
        if (bbFmt != psoFmt) {
            // Recreate DescFree backend with correct format
            HookLogImportant("DX12: PostSL format MISMATCH (bb=%d pso=%d) — recreating DescFree backend", (int)bbFmt,
                             (int)psoFmt);
            g_D3D11On12Adapter.Shutdown();
            delete g_DescFreeBackend;
            g_DescFreeBackend = nullptr;
            g_State.format = bbFmt;

            auto* backend = new DX12DescFreeBackend();
            if (backend->InitDevice(dev, bbFmt)) {
                g_DescFreeBackend = backend;
                g_D3D11On12Adapter.InitCustom(g_DescFreeBackend, OverlayBackendType::DX12);
                HookLogImportant("DX12: PostSL recreated DescFree backend (fmt=%d)", (int)bbFmt);
            } else {
                delete backend;
                HookLogImportant("DX12: PostSL DescFree backend recreate FAILED (fmt=%d)", (int)bbFmt);
                willRender = false;
            }
            willRender = willRender && g_DescFreeBackend && g_D3D11On12Adapter.IsInitialized();
        }
    }

    // PROBE: After FG transitions (epoch > 1), test queue health before full render.
    // Only do Probe 1 (empty ECL). Probe 2 (ClearRTV+barriers) is unsafe during PostSL
    // because the backbuffer state on origGame's timeline is unknown — SL manages
    // backbuffer state transitions internally, and cross-queue barrier assumptions fail.
    int probesNeeded = 1;
    bool isPostTransitionProbe = (s_reactivationEpoch > 1 && s_postSLProbeFrames < probesNeeded);
    if (isPostTransitionProbe) {
        s_postSLProbeFrames++;
        ID3D12CommandList* probeList[] = {list};

        if (s_postSLProbeFrames == 1) {
            // Probe 1: empty ECL — tests basic queue health
            list->Close();
        } else {
            // Probe 2: ClearRTV with barriers — tests backbuffer access (non-SL only)
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = bb;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1, &barrier);

            D3D12_CPU_DESCRIPTOR_HANDLE probeRtv = g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
            UINT rtvSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            probeRtv.ptr += (SIZE_T)bufIdx * rtvSize;
            dev->CreateRenderTargetView(bb, nullptr, probeRtv);

            float clearColor[4] = {0, 0, 0, 0};
            list->ClearRenderTargetView(probeRtv, clearColor, 0, nullptr);

            D3D12_RESOURCE_BARRIER barrierBack = {};
            barrierBack.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrierBack.Transition.pResource = bb;
            barrierBack.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrierBack.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            barrierBack.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1, &barrierBack);
            list->Close();
        }

        // Submit probe — use origECL for SL wrapper queue, realECL otherwise
        if (isSLWrapperQ) {
            ExecuteCommandListsPtr origECL = GetOriginalExecuteCommandLists(queue);
            if (origECL) {
                origECL(queue, 1, probeList);
            } else {
                queue->ExecuteCommandLists(1, probeList);
            }
        } else {
            ExecuteCommandListsPtr eclFn = g_RealD3D12ECL.load(std::memory_order_acquire);
            if (eclFn) {
                eclFn(queue, 1, probeList);
            } else {
                queue->ExecuteCommandLists(1, probeList);
            }
        }

        // Signal fence for allocator tracking
        if (g_State.fence) {
            UINT64 next = g_State.currentFenceValue + 1;
            HRESULT sigHr = queue->Signal(g_State.fence, next);
            if (SUCCEEDED(sigHr)) {
                g_State.currentFenceValue = next;
                if (idx >= 0 && idx < (int)g_State.fenceValues.size())
                    g_State.fenceValues[idx] = next;
            }
        }

        HRESULT probeHr = dev->GetDeviceRemovedReason();
        HookLogImportant("DX12: PostSL PROBE #%d on queue=%p (scQ=%p epoch=%d slWrapper=%d) — %s devRemoved=0x%08X %s",
                         s_postSLProbeFrames, queue, scQueue, s_reactivationEpoch, isSLWrapperQ ? 1 : 0,
                         s_postSLProbeFrames == 1 ? "empty ECL" : "ClearRTV+barriers", probeHr,
                         FAILED(probeHr) ? "FAILED" : "OK");
        if (FAILED(probeHr)) {
            bb->Release();
            return;
        }
        bb->Release();
        return;
    }

    // Cross-queue sync fence — used for SL wrapper queue ↔ origGame synchronization
    // Created lazily when needed for PostSL ECL dispatch on SL's wrapper queue.
    static ID3D12Fence* s_xqSyncFence = nullptr;
    static uint64_t s_xqSyncVal = 0;
    bool didXQSync = false;

    if (willRender && !s_xqSyncFence) {
        // Create fence lazily (needed for SL queue → origGame post-submit sync)
        HRESULT fhr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s_xqSyncFence));
        if (SUCCEEDED(fhr)) {
            HookLogImportant("DX12: PostSL created cross-queue sync fence for SL↔origGame sync");
        } else {
            HookLogImportant("DX12: PostSL FAILED to create cross-queue sync fence hr=0x%08X", fhr);
        }
    }

    // Use cached FG state for barrier/queue decisions (prevents mid-function race).
    // The post-FSR selected-scQueue path is special: probes and the stable non-FG
    // ProcessFrame path both indicate the backbuffer behaves like PRESENT on that
    // queue, so keep using explicit PRESENT<->RT transitions there.
    const auto postSLBarrierMode = ce::dx12_overlay_policy::DecidePostSLBackbufferBarrierMode(
        cachedSLFGActive, useExplicitPostFSRSwapchainTransitions);
    bool slFGBarrierFree = postSLBarrierMode == ce::dx12_overlay_policy::PostSLBackbufferBarrierMode::kUavBarrierOnly;

    // During SL FG with direct submission (bypassing SL's wrapper), we can render
    // every frame since we no longer pollute SL's internal pipeline.
    // Keep real-frame detection for metrics updates but don't skip interpolated frames.

    // BB health diagnostic: log BB pointer, dimensions, ref count periodically
    if (willRender && bb) {
        D3D12_RESOURCE_DESC bbDesc = bb->GetDesc();
        // AddRef/Release to get refcount without side effects
        bb->AddRef();
        ULONG refCount = bb->Release();
        static int s_bbHealthLog = 0;
        if (s_bbHealthLog < 10 || (s_bbHealthLog % 200 == 0)) {
            HookLogImportant("DX12: PostSL BB health #%d — bb=%p refCnt=%lu w=%u h=%u fmt=%u bufIdx=%d slFG=%d",
                             s_bbHealthLog, bb, refCount, (unsigned)bbDesc.Width, (unsigned)bbDesc.Height,
                             (unsigned)bbDesc.Format, bufIdx, cachedSLFGActive ? 1 : 0);
        }
        s_bbHealthLog++;
    }
    if (willRender && !usePostSLOffscreenComposite && slFGBarrierFree) {
        // UAV barrier: full GPU pipeline flush, no state tracking modification
        D3D12_RESOURCE_BARRIER uavBarrier = {};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.UAV.pResource = nullptr;  // NULL = global flush
        list->ResourceBarrier(1, &uavBarrier);
    } else if (willRender && !usePostSLOffscreenComposite) {
        D3D12_RESOURCE_BARRIER preBarrier = {};
        preBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        preBarrier.Transition.pResource = bb;
        preBarrier.Transition.StateBefore =
            postSLBarrierMode == ce::dx12_overlay_policy::PostSLBackbufferBarrierMode::kPresentToRenderTarget
                ? D3D12_RESOURCE_STATE_PRESENT
                : D3D12_RESOURCE_STATE_COMMON;
        preBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        preBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &preBarrier);
    }
    if (willRender) {
        static bool s_loggedBarrierMode = false;
        if (!s_loggedBarrierMode) {
            s_loggedBarrierMode = true;
            const char* barrierModeName = "common->rt";
            if (postSLBarrierMode == ce::dx12_overlay_policy::PostSLBackbufferBarrierMode::kUavBarrierOnly) {
                barrierModeName = "uav-only";
            } else if (postSLBarrierMode ==
                       ce::dx12_overlay_policy::PostSLBackbufferBarrierMode::kPresentToRenderTarget) {
                barrierModeName = "present->rt";
            }
            HookLogImportant(
                "DX12: PostSL barrier mode — mode=%s slFGBarrierFree=%d explicitPostFSR=%d offscreen=%d hadFSR=%d "
                "xqSync=%d",
                barrierModeName, slFGBarrierFree ? 1 : 0, useExplicitPostFSRSwapchainTransitions ? 1 : 0,
                usePostSLOffscreenComposite ? 1 : 0, g_HadFSRFGPhase ? 1 : 0, didXQSync ? 1 : 0);
        }
    }
    if (willRender) {
        if (ce::dx12_overlay_policy::ShouldSyntheticPostSLRefreshMetrics(cachedSLFGActive, processFrameRecentlySeen)) {
            if (auto* perf = DXGIShared::GetPerformanceMetrics()) {
                perf->Update(PerfLogger::GetQpcUs());
                if (g_FGCompat.IsFGActive()) {
                    auto fgType = g_FGCompat.GetActiveFGType();
                    int typeInt = 0;
                    if (fgType == FGCompatibility::FGType::DLSS_FG || fgType == FGCompatibility::FGType::DLSS_MSFG)
                        typeInt = 1;
                    else if (fgType == FGCompatibility::FGType::FSR_FG)
                        typeInt = 2;
                    else if (fgType == FGCompatibility::FGType::NVIDIA_SM)
                        typeInt = 3;
                    perf->SetFGMetrics(g_FGCompat.GetOutputFPS(), g_FGCompat.GetBaseFPS(), g_FGCompat.GetFGMultiplier(),
                                       typeInt);
                } else {
                    perf->SetFGMetrics(0.0f, 0.0f, 1, 0);
                }
            }
        }

        // Update text/API labels on real frames, but always keep the overlay
        // bound to the shared metrics object so FPS/history remain visible when
        // the first frame after an FG-driven reinit is classified as interpolated.
        bool isRealFrame = g_FGCompat.IsCurrentFrameReal();
        g_D3D11On12Adapter.SetIPCClient(g_IPC);
        const auto metricsBinding = ce::dx12_overlay_policy::DecideOverlayMetricsBinding(isRealFrame);
        if (metricsBinding.bindMetrics) {
            g_D3D11On12Adapter.SetMetrics(DXGIShared::GetPerformanceMetrics());
        }
        if (metricsBinding.refreshFrameMetadata) {
            static const bool s_isVKD3D = []() {
                return GetModuleHandleA("d3d12core.dll") &&
                       (GetModuleHandleA("libvkd3d-1.dll") || GetModuleHandleA("vkd3d.dll"));
            }();
            const char* api = s_isVKD3D ? "DX12 (VKD3D)" : "DX12";
            g_D3D11On12Adapter.SetGraphicsAPI(api);
        }

        if (usePostSLOffscreenComposite &&
            EnsureOffscreenRT(dev, g_State.cachedWidth, g_State.cachedHeight, g_State.format)) {
            // Avoid binding the post-FSR DLSS backbuffer as an RTV on the first real
            // PostSL render. Instead composite through an offscreen RT and copy back.
            D3D12_RESOURCE_BARRIER toCopyDest = {};
            toCopyDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toCopyDest.Transition.pResource = g_State.offscreenRT;
            toCopyDest.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            toCopyDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            toCopyDest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1, &toCopyDest);

            D3D12_RESOURCE_BARRIER bbToCopySource = {};
            if (useExplicitPostFSRBackbufferCopyTransitions) {
                bbToCopySource.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bbToCopySource.Transition.pResource = bb;
                bbToCopySource.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                bbToCopySource.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                bbToCopySource.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                list->ResourceBarrier(1, &bbToCopySource);
            }

            D3D12_TEXTURE_COPY_LOCATION bbSrc = {};
            bbSrc.pResource = bb;
            bbSrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            bbSrc.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION offDst = {};
            offDst.pResource = g_State.offscreenRT;
            offDst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            offDst.SubresourceIndex = 0;
            list->CopyTextureRegion(&offDst, 0, 0, 0, &bbSrc, nullptr);

            D3D12_RESOURCE_BARRIER toRenderTarget = {};
            toRenderTarget.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toRenderTarget.Transition.pResource = g_State.offscreenRT;
            toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            toRenderTarget.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1, &toRenderTarget);

            s_descFreeCmdList = list;
            s_descFreeRtv = g_State.offscreenRtvHeap->GetCPUDescriptorHandleForHeapStart();
            g_D3D11On12Adapter.RenderOverlay(g_State.cachedWidth, g_State.cachedHeight);
            s_descFreeCmdList = nullptr;

            D3D12_RESOURCE_BARRIER toCopySource = {};
            toCopySource.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toCopySource.Transition.pResource = g_State.offscreenRT;
            toCopySource.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            toCopySource.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            toCopySource.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1, &toCopySource);

            D3D12_RESOURCE_BARRIER bbToCopyDest = {};
            if (useExplicitPostFSRBackbufferCopyTransitions) {
                bbToCopyDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bbToCopyDest.Transition.pResource = bb;
                bbToCopyDest.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                bbToCopyDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                bbToCopyDest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                list->ResourceBarrier(1, &bbToCopyDest);
            }

            D3D12_TEXTURE_COPY_LOCATION offSrc = {};
            offSrc.pResource = g_State.offscreenRT;
            offSrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            offSrc.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION bbDst = {};
            bbDst.pResource = bb;
            bbDst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            bbDst.SubresourceIndex = 0;
            list->CopyTextureRegion(&bbDst, 0, 0, 0, &offSrc, nullptr);

            if (useExplicitPostFSRBackbufferCopyTransitions) {
                D3D12_RESOURCE_BARRIER bbToPresent = {};
                bbToPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bbToPresent.Transition.pResource = bb;
                bbToPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                bbToPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                bbToPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                list->ResourceBarrier(1, &bbToPresent);
            }
        } else {
            // Recreate RTV for this buffer index (cheap CPU-side op)
            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
            UINT rtvSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            rtvHandle.ptr += (SIZE_T)bufIdx * rtvSize;
            dev->CreateRenderTargetView(bb, nullptr, rtvHandle);

            s_descFreeCmdList = list;
            s_descFreeRtv = rtvHandle;
            g_D3D11On12Adapter.RenderOverlay(g_State.cachedWidth, g_State.cachedHeight);
            s_descFreeCmdList = nullptr;
        }
        rendered = true;
    } else {
        // Log why rendering was skipped (HookLogImportant for visibility after reactivation)
        static int s_backendSkip = 0;
        s_backendSkip++;
        if (s_backendSkip <= 10 || (s_backendSkip % 100) == 0)
            HookLogImportant("DX12: PostSL SKIP render #%d — backend=%p adapterInit=%d", s_backendSkip,
                             (void*)g_DescFreeBackend, g_D3D11On12Adapter.IsInitialized() ? 1 : 0);
    }

    // Post-rendering barrier: UAV during SL FG, standard RT→PRESENT otherwise
    if (rendered && !usePostSLOffscreenComposite && slFGBarrierFree) {
        D3D12_RESOURCE_BARRIER uavBarrier = {};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.UAV.pResource = nullptr;
        list->ResourceBarrier(1, &uavBarrier);

        static int s_postBarrierLog = 0;
        if (s_postBarrierLog < 5 || (s_postBarrierLog % 500 == 0)) {
            HookLogImportant("DX12: PostSL UAV post-barrier #%d epoch=%d", s_postBarrierLog, s_reactivationEpoch);
        }
        s_postBarrierLog++;
    } else if (rendered && !usePostSLOffscreenComposite) {
        D3D12_RESOURCE_BARRIER postBarrier = {};
        postBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        postBarrier.Transition.pResource = bb;
        postBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        postBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        postBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &postBarrier);

        static int s_postBarrierLog = 0;
        if (s_postBarrierLog < 5 || (s_postBarrierLog % 500 == 0)) {
            HookLogImportant("DX12: PostSL RT→PRESENT post-barrier #%d epoch=%d", s_postBarrierLog,
                             s_reactivationEpoch);
        }
        s_postBarrierLog++;
    }

    // If we can't render, bail — don't submit empty command lists.
    if (!willRender) {
        list->Close();
        bb->Release();
        return;
    }

    HRESULT closeHr = list->Close();
    if (FAILED(closeHr)) {
        static int s_closeFailCount = 0;
        if (s_closeFailCount++ < 10)
            HookLog("DX12: PostSLOverlayRender — list->Close failed hr=0x%08X", closeHr);
        bb->Release();
        return;
    }

    // Pre-submit device health check: if device is already removed (e.g., by
    // SL's internal FG queue transition), don't submit — it would fail anyway.
    HRESULT preDevReason = dev->GetDeviceRemovedReason();
    if (FAILED(preDevReason)) {
        HookLogImportant("DX12: PostSL PRE-submit device already removed (hr=0x%08X queue=%p) — skipping",
                         (unsigned)preDevReason, queue);
        bb->Release();
        return;
    }

    // CROSS-QUEUE GPU SYNC: When our overlay queue differs from the swapchain
    // queue (scQueue), the backbuffer was last used by SL's FG pipeline on
    // scQueue.  We MUST ensure SL's GPU work completes before our barriers
    // touch the backbuffer on a different queue.  Without this sync, the GPU
    // may execute our PRESENT→RT barrier in parallel with SL's FG work on the
    // same backbuffer, causing DEVICE_REMOVED.
    //
    // Pattern: Signal on scQueue (records SL's completion point) →
    //          Wait on our queue (stalls until SL finishes)
    //
    // This is the standard D3D12 cross-queue synchronization pattern.
    // During initial DLSS FG (scQueue=NULL), this is skipped — same-queue
    // guarantees GPU ordering naturally.
    //
    // EXCEPTION: During SL FG, scQueue may be SL's internal queue (captured
    // from CreateSwapChainForHwnd during FG init).  Signal/Wait on SL's queue
    // with our fence causes DEVICE_REMOVED.  Skip cross-queue sync entirely
    // during SL FG — SL manages its own synchronization.
    bool crossQueueSynced = didXQSync;  // SL→origGame sync from above
    if (scQueue && scQueue != queue && g_State.crossQueueFence && !cachedSLFGActive) {
        UINT64 syncVal = ++g_State.crossQueueFenceValue;
        // Signal on scQueue: "record SL's GPU progress"
        HRESULT sigHr = scQueue->Signal(g_State.crossQueueFence, syncVal);
        if (SUCCEEDED(sigHr)) {
            // Wait on our queue: "don't execute until scQueue catches up"
            HRESULT waitHr = queue->Wait(g_State.crossQueueFence, syncVal);
            if (SUCCEEDED(waitHr)) {
                crossQueueSynced = true;
            } else {
                HookLog("DX12: PostSL cross-queue pre-sync Wait failed hr=0x%08X", waitHr);
            }
        } else {
            static int s_preSyncFail = 0;
            if (s_preSyncFail++ < 5)
                HookLog(
                    "DX12: PostSL cross-queue pre-sync Signal failed hr=0x%08X "
                    "(scQueue=%p may reject external signals)",
                    sigHr);
        }
    }

    // Submit ECL via virtual call on origGame during SL FG.
    //
    // CRITICAL: Do NOT use realECL(g_OriginalGameQueue, ...) — g_OriginalGameQueue
    // may be SL's COM wrapper object.  Calling the raw D3D12 ECL with an SL wrapper
    // as `this` is type confusion (internal field offsets differ).
    //
    // Virtual call → SL's COM wrapper vtable → SL processes → SL calls real D3D12
    // queue internally.  This lets SL properly track our ECL in its FG pipeline.
    //
    // For non-SL-FG paths (origECL/realECL): no change, those work as before.
    ID3D12CommandList* lists[] = {list};
    bool usedRealECL = false;
    bool usedOrigECL = false;
    bool usedVirtualCall = false;
    ID3D12CommandQueue* submittedQueue = queue;

    // Pre-submit device health check — if the device is already removed
    // (e.g. after FG teardown), skip the ECL to avoid triggering ERR_GFX_STATE.
    {
        auto* preSubmitDev = g_Device.load(std::memory_order_acquire);
        HRESULT preSubmitHr = preSubmitDev ? preSubmitDev->GetDeviceRemovedReason() : E_FAIL;
        if (FAILED(preSubmitHr)) {
            HookLogImportant("DX12: PostSL SKIPPING ECL — device removed 0x%08X (queue=%p)", (unsigned)preSubmitHr,
                             queue);
            g_DeviceRemoved.store(true, std::memory_order_release);
            bb->Release();
            return;
        }
    }

    // Diagnostic: on first few submits after each transition, log ECL function pointer comparison
    // (reset to 0 on PostSL REACTIVATION for fresh diagnostics)
    bool slFGAtDispatch = cachedSLFGActive;
    if (g_PostSLECLDiagCount.load(std::memory_order_relaxed) < 10) {
        ExecuteCommandListsPtr origECLDiag = GetOriginalExecuteCommandLists(queue);
        HookLogImportant(
            "DX12: PostSL ECL diag — queue=%p scQueue=%p origECL=%p realECL=%p match=%d sameQueue=%d slWrapper=%d "
            "slFG=%d hadFSR=%d",
            queue, scQueue, (void*)origECLDiag, (void*)realECL, origECLDiag == realECL ? 1 : 0,
            queue == scQueue ? 1 : 0, isSLWrapperQ ? 1 : 0, slFGAtDispatch ? 1 : 0, g_HadFSRFGPhase ? 1 : 0);
        g_PostSLECLDiagCount.fetch_add(1, std::memory_order_relaxed);
    }

    if (slFGAtDispatch) {
        // When SL FG recreated the swapchain on a different queue (scQueue != origGame),
        // submit directly on scQueue.  SL's wrapper routes to origGame, causing
        // cross-queue backbuffer access → DEVICE_REMOVED.
        // PostSL fires after SL's FG pipeline completes, so scQueue is idle.
        bool scQueueDiffers = (scQueue && scQueue != g_OriginalGameQueue);

        // DIRECT QUEUE SUBMISSION (bypasses SL's COM wrapper):
        //
        // SL's COM wrapper (g_SLWrapperQueue) adds internal metadata to each ECL.
        // This metadata accumulates and causes DEVICE_REMOVED after ~500-2000 frames.
        // Confirmed by testing:
        //   - Full-rate through wrapper: crash at ~500 frames
        //   - 1/10 rate through wrapper: stable (damage drains between submits)
        //   - Direct to real queue: 16,798+ frames stable
        //   - Empty ECL through wrapper: stable (damage requires content)
        //
        // The fix: submit directly to the real D3D12 queue behind SL's wrapper
        // using g_RealD3D12ECL (raw D3D12 function from d3d12core.dll vtable).
        //
        // Bootstrap: First frame submits through SL's wrapper with
        // s_insidePostSLOverlayECL=true.  Our ECL detour sees the real queue
        // as pThis and captures it into g_RealQueueBehindSLWrapper.
        // Subsequent frames use the direct path.
        //
        // CAUTION FOR TALOS/OTHER GAMES: If the game uses FSR FG → DLSS FG
        // transitions, the real queue behind SL might change.  Monitor for
        // DEVICE_REMOVED after transitions and re-bootstrap if needed.
        ID3D12CommandQueue* slQueue = slWrapperQueue;

        const bool allowScQueueVirtualSubmit =
            ce::dx12_overlay_policy::ShouldUsePostSLScQueueVirtualSubmit(g_HadFSRFGPhase, scQueueDiffers);

        const bool useWrapperSubmitAfterFSR = ce::dx12_overlay_policy::ShouldUsePostSLWrapperSubmitAfterFSR(
            g_HadFSRFGPhase, usePostSLOffscreenComposite, selectedQueueIsSwapchainQueue, slQueue != nullptr,
            preferSelectedSwapchainQueueSubmitAfterFSR);

        if (useWrapperSubmitAfterFSR) {
            // After an FSR phase, keep swapchain-touching PostSL work on the SL
            // wrapper path when that is the only path that has successfully
            // survived the post-FSR copy probes. We can still capture the real
            // queue behind the wrapper for diagnostics and later promotion.
            submittedQueue = slQueue;
            s_insidePostSLOverlayECL = true;
            slQueue->ExecuteCommandLists(1, lists);
            s_insidePostSLOverlayECL = false;
            usedVirtualCall = true;

            static int s_postFSRWrapperSubmitLog = 0;
            if (s_postFSRWrapperSubmitLog < 5 || (s_postFSRWrapperSubmitLog % 200) == 0) {
                HookLogImportant(
                    "DX12: PostSL post-FSR submit #%d via SL wrapper %p (liveWrapper=%p scQueue=%p realQ=%p "
                    "offscreen=%d pinned=%d)",
                    s_postFSRWrapperSubmitLog, slQueue, liveSLWrapperQueue, scQueue, realQ,
                    usePostSLOffscreenComposite ? 1 : 0, usingPinnedPostFSRWrapperQueue ? 1 : 0);
            }
            s_postFSRWrapperSubmitLog++;
        } else if (preferSelectedSwapchainQueueSubmitAfterFSR) {
            // After an FSR phase, if PostSL already resolved to the runtime's
            // swapchain queue and probe submits on that queue succeeded, keep
            // using that queue directly. Falling back to the SL wrapper here
            // reintroduces the cross-queue handoff we are trying to avoid.
            submittedQueue = queue;
            if (selectedQueueOrigECL) {
                selectedQueueOrigECL(queue, 1, lists);
                usedOrigECL = true;
            } else {
                realECL(queue, 1, lists);
                usedRealECL = true;
            }

            static int s_postFSRDirectScQueueLog = 0;
            if (s_postFSRDirectScQueueLog < 5 || (s_postFSRDirectScQueueLog % 200) == 0) {
                HookLogImportant(
                    "DX12: PostSL post-FSR submit #%d on selected scQueue %p (origECL=%d realECL=%d wrapper=%p)",
                    s_postFSRDirectScQueueLog, queue, selectedQueueOrigECL ? 1 : 0, realECL ? 1 : 0,
                    liveSLWrapperQueue);
            }
            s_postFSRDirectScQueueLog++;
        } else if (preferSelectedQueueDirectSubmitAfterFSR) {
            // After an FSR phase, the selected queue may already expose the real
            // D3D12 submit entrypoint directly. In that case, do not bounce to a
            // different late-captured "wrapper" queue for the first rendered
            // frame; stay on the queue that already passed our probes.
            submittedQueue = queue;
            selectedQueueOrigECL(queue, 1, lists);
            usedOrigECL = true;

            static int s_postFSRDirectSelectedQueueLog = 0;
            if (s_postFSRDirectSelectedQueueLog < 10 || (s_postFSRDirectSelectedQueueLog % 200) == 0) {
                HookLogImportant(
                    "DX12: PostSL post-FSR direct submit #%d on selected queue %p "
                    "(origECL matches realECL, scQueue=%p latestWrapper=%p)",
                    s_postFSRDirectSelectedQueueLog, queue, scQueue, liveSLWrapperQueue);
            }
            s_postFSRDirectSelectedQueueLog++;
        } else if (allowScQueueVirtualSubmit) {
            // Direct submission on scQueue — backbuffers belong to this queue.
            // Bypass SL's wrapper entirely (routes to origGame → wrong queue).
            submittedQueue = scQueue;
            s_insidePostSLOverlayECL = true;
            scQueue->ExecuteCommandLists(1, lists);
            s_insidePostSLOverlayECL = false;
            usedVirtualCall = true;

            static int s_scQSubmitLog = 0;
            if (s_scQSubmitLog < 5 || (s_scQSubmitLog % 500 == 0))
                HookLogImportant("DX12: PostSL scQueue submit #%d on %p (origGame=%p, bypassing SL wrapper)",
                                 s_scQSubmitLog, scQueue, g_OriginalGameQueue);
            s_scQSubmitLog++;
        } else if (realQ && realECL) {
            // Direct submission: bypass SL's wrapper entirely
            submittedQueue = realQ;
            s_insidePostSLOverlayECL = true;
            realECL(realQ, 1, lists);
            s_insidePostSLOverlayECL = false;
            usedRealECL = true;

            static int s_directLog = 0;
            if (s_directLog < 5 || (s_directLog % 500 == 0))
                HookLogImportant("DX12: PostSL DIRECT submit #%d on real queue %p (bypass SL wrapper)", s_directLog,
                                 realQ);
            s_directLog++;
        } else {
            const bool allowWrapperBootstrap = ce::dx12_overlay_policy::ShouldAllowPostSLWrapperBootstrap(
                g_HadFSRFGPhase, realQ != nullptr, realECL != nullptr);
            if (!allowWrapperBootstrap) {
                HookLogImportant(
                    "DX12: PostSL refusing SL wrapper bootstrap without direct path (queue=%p scQueue=%p wrapper=%p)",
                    queue, scQueue, (void*)slQueue);
                bb->Release();
                return;
            }

            // Bootstrap: submit through SL's wrapper to capture real queue on first call
            if (!slQueue && g_HadFSRFGPhase) {
                HookLogImportant(
                    "DX12: PostSL refusing post-FSR bootstrap without SL wrapper queue (queue=%p scQueue=%p)", queue,
                    scQueue);
                bb->Release();
                return;
            }
            if (slQueue) {
                submittedQueue = slQueue;
                s_insidePostSLOverlayECL = true;
                slQueue->ExecuteCommandLists(1, lists);
                s_insidePostSLOverlayECL = false;
                usedVirtualCall = true;
                HookLogImportant("DX12: PostSL bootstrap via SL wrapper %p (will capture real queue for direct path)",
                                 slQueue);
            } else {
                if (!ce::dx12_overlay_policy::ShouldAllowPostSLDirectVirtualBootstrapWithoutWrapper(
                        slFGAtDispatch, slQueue != nullptr, realQ != nullptr, realECL != nullptr,
                        selectedQueueIsSwapchainQueue, selectedQueueOrigECLMatchesRealECL)) {
                    HookLogImportant(
                        "DX12: PostSL refusing no-wrapper virtual bootstrap during Streamline FG "
                        "(queue=%p scQueue=%p realQ=%p realECL=%p)",
                        queue, scQueue, realQ, (void*)realECL);
                    bb->Release();
                    return;
                }
                if (slFGAtDispatch && selectedQueueIsSwapchainQueue && selectedQueueOrigECLMatchesRealECL &&
                    selectedQueueOrigECL) {
                    submittedQueue = queue;
                    selectedQueueOrigECL(queue, 1, lists);
                    usedOrigECL = true;
                    static int s_noWrapperDirectSelectedQueueLog = 0;
                    if (s_noWrapperDirectSelectedQueueLog < 10 || (s_noWrapperDirectSelectedQueueLog % 200) == 0) {
                        HookLogImportant(
                            "DX12: PostSL no-wrapper direct selected-queue submit #%d on %p "
                            "(scQueue=%p origECL matches realECL)",
                            s_noWrapperDirectSelectedQueueLog, queue, scQueue);
                    }
                    s_noWrapperDirectSelectedQueueLog++;
                } else {
                s_insidePostSLOverlayECL = true;
                queue->ExecuteCommandLists(1, lists);
                s_insidePostSLOverlayECL = false;
                usedVirtualCall = true;
                static int s_noSlQ = 0;
                if (s_noSlQ++ < 3)
                    HookLogImportant("DX12: PostSL no SL wrapper queue, using origGame %p", queue);
                }
            }
        }
    } else if (isSLWrapperQ) {
        ExecuteCommandListsPtr origECL = GetOriginalExecuteCommandLists(queue);
        if (origECL) {
            origECL(queue, 1, lists);
            usedOrigECL = true;
        } else {
            queue->ExecuteCommandLists(1, lists);
            usedVirtualCall = true;
        }
    } else if (realECL) {
        realECL(queue, 1, lists);
        usedRealECL = true;
    } else {
        ExecuteCommandListsPtr origECL = GetOriginalExecuteCommandLists(queue);
        if (origECL) {
            origECL(queue, 1, lists);
            usedOrigECL = true;
        } else {
            queue->ExecuteCommandLists(1, lists);
            usedVirtualCall = true;
        }
    }

    if (rendered) {
        SharedMemoryLayout* postSLShm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        OverlayConfig postSLOverlayCfg = GetActiveDX12OverlayConfig(postSLShm);
        bool isRealFrame = g_FGCompat.IsCurrentFrameReal();
        if (postSLShm && g_IPC && g_IPC->IsRecording() && isRealFrame && postSLOverlayCfg.showOverlay &&
            postSLOverlayCfg.captureIncludeOverlay) {
            PublishDX12CapturedFrame(pSwapChain, postSLShm, submittedQueue, true, bufIdx);
        }
        if (postSLShm && postSLShm->runtimeState.cmdTakeScreenshot.load(std::memory_order_acquire) &&
            postSLOverlayCfg.showOverlay && postSLOverlayCfg.screenshotIncludeOverlay) {
            CaptureRequestedDX12Screenshot(sc3, postSLShm, submittedQueue);
        }
    }

    // Fence signal for allocator tracking.
    // CRITICAL: Signal on the SAME queue we submitted the command list to.
    // During SL FG with direct submission, use the real D3D12 queue behind SL's wrapper.
    bool slFGSubmit = cachedSLFGActive;
    if (g_State.fence) {
        UINT64 next = g_State.currentFenceValue + 1;
        ID3D12CommandQueue* submitQueue = submittedQueue ? submittedQueue : queue;
        HRESULT sigHr = submitQueue->Signal(g_State.fence, next);
        if (SUCCEEDED(sigHr)) {
            g_State.currentFenceValue = next;
            if (idx >= 0 && idx < (int)g_State.fenceValues.size())
                g_State.fenceValues[idx] = next;

            // Cross-queue GPU sync: only for non-SL-FG, non-same-queue scenarios
            bool crossQueueSafe = scQueue && scQueue != submitQueue && !slFGSubmit;
            if (crossQueueSafe) {
                HRESULT waitHr = scQueue->Wait(g_State.fence, next);
                crossQueueSynced = true;
                if (FAILED(waitHr)) {
                    static int s_waitFail = 0;
                    if (s_waitFail++ < 5)
                        HookLog(
                            "DX12: PostSL cross-queue Wait failed hr=0x%08X "
                            "(scQueue=%p fence=%p val=%llu)",
                            waitHr, scQueue, g_State.fence, (unsigned long long)next);
                }
            }
        }
    }

    // (Dedicated queue post-sync removed — no longer using dedicated queue.)

    // Periodic allocator fence health check — detect tracking issues before crash
    if (g_State.fence) {
        UINT64 completed = g_State.fence->GetCompletedValue();
        static int s_fenceHealthLog = 0;
        s_fenceHealthLog++;
        UINT64 expected = g_State.currentFenceValue;
        UINT64 gap = (expected > completed) ? (expected - completed) : 0;
        if (s_fenceHealthLog <= 10 || (s_fenceHealthLog % 200 == 0) || gap > 10) {
            HookLogImportant(
                "DX12: PostSL fence health #%d — completed=%llu current=%llu gap=%llu allocators=%d idx=%d",
                s_fenceHealthLog, completed, expected, gap, (int)g_State.allocators.size(), idx);
        }
    }

    // Diagnostic logging — log queue info and device health after submit
    static std::atomic<int> s_postSLRenderCount{0};
    int renderNum = s_postSLRenderCount.fetch_add(1, std::memory_order_relaxed) + 1;
    s_postSLRenders.fetch_add(1, std::memory_order_relaxed);
    HRESULT postDevReason = dev->GetDeviceRemovedReason();

    // Mark PostSL as confirmed rendering — pre-SL draw can now be suppressed.
    if (!g_PostSLConfirmedRendering.load(std::memory_order_relaxed)) {
        g_PostSLConfirmedRendering.store(true, std::memory_order_release);
        HookLogImportant("DX12: PostSL CONFIRMED rendering via re-entrant Present — suppressing pre-SL draw");
    }
    // Reset stall counter — PostSL is actively rendering, no need for pre-SL fallback
    g_PostSLStallCounter.store(0, std::memory_order_release);
    // Track PostSL warmup — stable frame count since last FG transition.
    // Stall fallback is only enabled after this exceeds warmup threshold.
    g_PostSLStableFrameCount.fetch_add(1, std::memory_order_release);

    // Track last working queue — survives FG transitions so we can prefer
    // a proven-safe queue when PostSL re-activates after FSR→DLSS switch.
    if (SUCCEEDED(postDevReason) && submittedQueue != g_PostSLLastWorkingQueue &&
        ce::dx12_overlay_policy::ShouldRememberPostSLLastWorkingQueue(isSLWrapperQ)) {
        HookLogImportant("DX12: PostSL updating lastWorkingQueue %p -> %p", g_PostSLLastWorkingQueue, submittedQueue);
        SetPostSLLastWorkingQueue(submittedQueue);
    }

    if (renderNum <= 20 || (renderNum % 10) == 0 || renderNum >= 1800 || FAILED(postDevReason)) {
        HookLogImportant(
            "DX12: Post-SL overlay SUBMIT #%d (bufIdx=%u queue=%p scQueue=%p slWrapper=%d rendered=%d "
            "virtualCall=%d realECL=%d origECL=%d xqSync=%d tid=0x%04X devRemoved=0x%08X epoch=%d)",
            renderNum, bufIdx, submittedQueue, scQueue, isSLWrapperQ ? 1 : 0, rendered ? 1 : 0, usedVirtualCall ? 1 : 0,
            usedRealECL ? 1 : 0, usedOrigECL ? 1 : 0, crossQueueSynced ? 1 : 0, GetCurrentThreadId(),
            (unsigned)postDevReason, s_reactivationEpoch);
    }
    // Early warning: if device just failed, log immediately
    if (FAILED(postDevReason)) {
        HookLogImportant(
            "DX12: DEVICE_REMOVED detected after PostSL ECL submit #%d "
            "(queue=%p scQueue=%p hr=0x%08X)",
            renderNum, submittedQueue, scQueue, (unsigned)postDevReason);
    }

    bb->Release();
}

static void PostSLOverlayRenderGated(IDXGISwapChain* pSwapChain) {
    if (!g_PostSLCallbackExecutionEnabled.load(std::memory_order_acquire)) {
        return;
    }

    g_PostSLCallbackInFlight.fetch_add(1, std::memory_order_acq_rel);
    auto inFlightGuard =
        ce::make_scope_guard([]() { g_PostSLCallbackInFlight.fetch_sub(1, std::memory_order_acq_rel); });

    if (!g_PostSLCallbackExecutionEnabled.load(std::memory_order_acquire)) {
        return;
    }

    PostSLOverlayRender(pSwapChain);
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
    g_LastProcessFrameTickMs.store(GetTickCount64(), std::memory_order_release);
    CleanupDeferredPostSLQueuesIfSafe("DX12: ProcessFrame deferred PostSL cleanup");

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
    if (g_pSharedMem) {
        perfMetrics.sourceFrameIndex = DXGIShared::GetLatestSourceFrameIndex();
        perfMetrics.sourceCapturePhase = g_pSharedMem->runtimeState.capturePhase.load(std::memory_order_relaxed);
        perfMetrics.sourceEncoderQueueDepth = g_pSharedMem->encoderQueueDepth.load(std::memory_order_relaxed);
        perfMetrics.sourceMuxQueueKb =
            (g_pSharedMem->runtimeState.muxQueueBytes.load(std::memory_order_relaxed) + 1023u) / 1024u;
        perfMetrics.sourceOverloadFlags =
            g_pSharedMem->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
    }
    if (auto* perf = DXGIShared::GetPerformanceMetrics()) {
        perfMetrics.sourceCurrentFpsTimes100 = static_cast<int32_t>(perf->GetCurrentFPS() * 100.0f + 0.5f);
        perfMetrics.source1PctLowTimes100 = static_cast<int32_t>(perf->Get1PercentLowFPS() * 100.0f + 0.5f);
        perfMetrics.sourcePoint1PctLowTimes100 = static_cast<int32_t>(perf->Get01PercentLowFPS() * 100.0f + 0.5f);
        perfMetrics.sourceFrameTimeStdDevUs = static_cast<int32_t>(perf->GetWindowStdDev() + 0.5);
    }
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

    // Post-FG-OFF frame counter: log every ProcessFrame for first 50 calls after FG
    // transition.  If Present stops being called, this gap will be visible in the log.
    {
        static std::atomic<int> s_postFGOffFrames{-1};
        bool slFGNow = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        // Detect FG OFF transition
        static bool s_prevSLFG = false;
        if (s_prevSLFG && !slFGNow) {
            s_postFGOffFrames.store(0, std::memory_order_release);
        }
        s_prevSLFG = slFGNow;

        int pfCount = s_postFGOffFrames.load(std::memory_order_acquire);
        if (pfCount >= 0 && pfCount < 300) {
            s_postFGOffFrames.store(pfCount + 1, std::memory_order_release);
            auto* pfDev = g_Device.load(std::memory_order_acquire);
            HRESULT pfDevHr = pfDev ? pfDev->GetDeviceRemovedReason() : E_FAIL;
            // Log first 50 every frame, then every 10th frame up to 300
            if (pfCount < 50 || pfCount % 10 == 0) {
                HookLogImportant(
                    "DX12: PostFGOff-PF #%d (overlayInit=%d syncInit=%d cooldown=%d "
                    "slFG=%d fgActive=%d devRemoved=0x%08X tid=0x%04X)",
                    pfCount + 1, g_State.overlayInit ? 1 : 0, g_State.syncInit ? 1 : 0, g_FGTransitionCooldown,
                    slFGNow ? 1 : 0, g_FGCompat.IsFGActive() ? 1 : 0, (unsigned)pfDevHr, GetCurrentThreadId());
            }
            // Immediately abort overlay rendering if device was removed
            if (FAILED(pfDevHr)) {
                HookLogImportant("DX12: PostFGOff-PF #%d DEVICE REMOVED 0x%08X — aborting overlay", pfCount + 1,
                                 (unsigned)pfDevHr);
                g_DeviceRemoved.store(true, std::memory_order_release);
                return;
            }
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
    const bool deferOverlayInitAfterResume = ShouldDelayOverlayInitAfterStartupResumeCompat(
        allowOverlayRender, frameDesc.OutputWindow, &postResumeSettleRemainingMs);
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

    // Update the FG recency counter BEFORE swapchain change check.
    {
        bool fgNow =
            IsActualFrameGenerationActive() || DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        if (fgNow)
            g_FramesSinceFGActive = 0;
        else if (g_FramesSinceFGActive < 9999)
            ++g_FramesSinceFGActive;

        int slOffSwapchainGrace = g_SLOffSwapchainReinitGrace.load(std::memory_order_acquire);
        if (slOffSwapchainGrace > 0) {
            g_SLOffSwapchainReinitGrace.store(slOffSwapchainGrace - 1, std::memory_order_release);
        }
    }

    if (pSwapChain != g_LastSwapChain) {
        if (g_LastSwapChain) {
            CleanupRTVs();
            {
                std::lock_guard<std::recursive_mutex> capLock(g_DX12CaptureMutex);
                g_SharedCaptureD3D12.Reset();
            }
            g_State.overlayInit = false;
            ResetStartupOverlayBackendActivationStage();

            // FG TRANSITION PROTECTION: If FG is currently active (or was recently
            // active per the cooldown), the swapchain change is likely caused by an
            // FG mode switch (e.g., FSR FG → DLSS FG).  SL / FSR runtimes need time
            // to finish initializing before we reinit overlay resources on the new
            // swapchain.  Set a transition cooldown so the reinit path (below) defers
            // until the FG runtime is stable.
            bool fgCurrentlyActive =
                IsActualFrameGenerationActive() || DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
            // Also protect if FG was active within the last ~5 seconds (~300 frames).
            // When switching FG modes, the game may disable one FG type many frames
            // before the swapchain actually changes.  Heuristic detection goes inactive
            // immediately, but the swapchain change is delayed.
            constexpr int kFGRecentWindowFrames = 300;
            bool fgRecentlyWasActive = (g_FramesSinceFGActive < kFGRecentWindowFrames);
            ID3D12CommandQueue* currentSwapchainQueue = nullptr;
            ID3D12CommandQueue* currentOriginalGameQueue = nullptr;
            ID3D12CommandQueue* currentCommandQueue = nullptr;
            ID3D12CommandQueue* currentPrimaryQueue = nullptr;
            {
                std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                currentSwapchainQueue = g_SwapchainQueue;
                currentOriginalGameQueue = g_OriginalGameQueue;
                currentCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
                currentPrimaryQueue = g_PrimaryGameQueue.load(std::memory_order_acquire);
            }
            int slOffSwapchainGrace = g_SLOffSwapchainReinitGrace.load(std::memory_order_acquire);
            const bool commandQueueSettledToPrimary =
                currentCommandQueue != nullptr && currentCommandQueue == currentPrimaryQueue;
            bool guardSwapchainReinit = ce::dx12_overlay_policy::ShouldGuardSwapchainReinitAfterChange(
                fgCurrentlyActive, fgRecentlyWasActive, g_FGTransitionCooldown > 0, slOffSwapchainGrace > 0,
                g_FGRuntimeOwnsSwapchain, currentSwapchainQueue != nullptr, currentOriginalGameQueue != nullptr,
                currentSwapchainQueue != nullptr && currentOriginalGameQueue != nullptr &&
                    currentSwapchainQueue != currentOriginalGameQueue);
            if (guardSwapchainReinit) {
                int cooldownFrames = 90;  // ~1.5s at 60fps — longer than normal transition
                if (ce::dx12_overlay_policy::ShouldUseShortPostFSRInactiveCooldown(commandQueueSettledToPrimary,
                                                                                   g_HadFSRFGPhase,
                                                                                   slOffSwapchainGrace > 0)) {
                    cooldownFrames = 15;
                }
                g_FGTransitionCooldown = ce::dx12_overlay_policy::ResolveTransitionCooldownFrames(
                    g_FGTransitionCooldown, cooldownFrames,
                    ce::dx12_overlay_policy::ShouldUseShortPostFSRInactiveCooldown(commandQueueSettledToPrimary,
                                                                                   g_HadFSRFGPhase,
                                                                                   slOffSwapchainGrace > 0));
                g_PostSLCooldownRemaining.store(g_FGTransitionCooldown, std::memory_order_release);
                g_PostSLOverlayActive.store(false, std::memory_order_release);
                g_PostSLConfirmedRendering.store(false, std::memory_order_release);
                // Force sync re-init: old allocators/fence were on the old queue.
                if (g_State.syncInit) {
                    g_State.syncInit = false;
                }
                HookLogImportant(
                    "DX12: Swapchain change during active FG — cooldown %d frames "
                    "(fgActive=%d, fgRecentFrames=%d, slSignal=%d, prevCooldown=%d, slOffGrace=%d, "
                    "fgOwned=%d, scQueue=%p, origGame=%p cmdQ=%p primaryQ=%p)",
                    cooldownFrames, fgCurrentlyActive ? 1 : 0, g_FramesSinceFGActive,
                    DXGIShared::g_StreamlineFGRunning.load() ? 1 : 0, g_FGTransitionCooldown, slOffSwapchainGrace,
                    g_FGRuntimeOwnsSwapchain ? 1 : 0, currentSwapchainQueue, currentOriginalGameQueue,
                    currentCommandQueue, currentPrimaryQueue);
            } else {
                HookLogImportant("DX12: Swapchain change (no FG active) — normal reinit");
                g_NeedOffscreenOverlayAfterPostFSRNonFG = false;
                HookLogImportant(
                    "DX12: Cleared offscreen overlay flag — clean swapchain transition, backbuffer state is reliable");
            }
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
    //
    // EXCEPTION: During SL DLSS FG, g_SwapchainQueue may have been overwritten
    // by SL's CreateSwapChainForHwnd (SL creates its own swapchain with its
    // internal queue).  In that case, use g_OriginalGameQueue — the game's
    // real queue captured before any FG ever activated.
    //
    // FSR FG: FSR creates a NEW swapchain with its own queue. Our Present
    // detour sees pSwapChain = FSR's swapchain, so GetBuffer returns FSR's
    // backbuffers.  We MUST submit on the swapchain's associated queue
    // (g_SwapchainQueue = FSR's queue) — submitting on origGame causes
    // cross-queue resource access without synchronization → DEVICE_REMOVED.
    ID3D12CommandQueue* gameQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
        bool slFGNow = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        bool fsrFGNow = IsFSRFrameGenerationActive();
        ID3D12CommandQueue* currentCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
        ID3D12CommandQueue* currentPrimaryQueue = g_PrimaryGameQueue.load(std::memory_order_acquire);
        const bool recentStreamlineTeardown = g_SLOffHeuristicGrace.load(std::memory_order_acquire) > 0;
        const bool lastWorkingQueueStillActiveDuringRecentTeardown =
            g_PostSLLastWorkingQueue != nullptr &&
            GetTickCount64() < g_PostSLRecentTeardownActivityUntilMs.load(std::memory_order_acquire);
        const auto routingDecision = ce::dx12_overlay_policy::DecideSwapchainOverlayRouting(
            g_FGRuntimeOwnsSwapchain, slFGNow, fsrFGNow, g_HadFSRFGPhase, g_SwapchainQueue != nullptr,
            g_OriginalGameQueue != nullptr, g_PostSLLastWorkingQueue != nullptr,
            currentCommandQueue != nullptr && currentCommandQueue == currentPrimaryQueue);

        if (routingDecision ==
            ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kSkipRuntimeOwnedSwapchainWithoutQueue) {
            static int s_fgOwnSkipLog = 0;
            if (s_fgOwnSkipLog++ < 10 || (s_fgOwnSkipLog % 300) == 0) {
                HookLogImportant(
                    "DX12: ProcessFrame — FG runtime owns swapchain but scQueue is null, SKIPPING overlay "
                    "(origGame=%p, fsrFGHeur=%d, fgOwnedSince=%llums ago) #%d",
                    g_OriginalGameQueue, fsrFGNow ? 1 : 0, GetTickCount64() - g_FGRuntimeOwnsSwapchainSince,
                    s_fgOwnSkipLog);
            }
            return;
        } else if (routingDecision ==
                   ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUsePostFSRStreamlineQueue) {
            // After FSR→DLSS: use scQueue (swapchain creation queue).
            // The swapchain was created on FSR's queue; backbuffers are
            // associated with it.  origGame can't access them (cross-queue).
            // SL's wrapper queue also fails.  scQueue is the ONLY queue
            // with authorized backbuffer access.
            if (g_SwapchainQueue) {
                gameQueue = g_SwapchainQueue;
                static bool s_loggedPostFSR = false;
                if (!s_loggedPostFSR) {
                    s_loggedPostFSR = true;
                    HookLogImportant(
                        "DX12: ProcessFrame — post-FSR SL FG, using scQueue %p (swapchain creation queue, origGame=%p)",
                        gameQueue, g_OriginalGameQueue);
                }
            } else {
                // Shouldn't happen — scQueue should be kept alive during hadFSR
                gameQueue = g_OriginalGameQueue;
                HookLogImportant("DX12: ProcessFrame — post-FSR SL FG but scQueue is null, fallback to origGame %p",
                                 gameQueue);
            }
        } else if (routingDecision ==
                   ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUseStreamlineOriginalQueue) {
            // SL FG (no FSR history): use origGame.
            gameQueue = g_OriginalGameQueue;
        } else if (routingDecision ==
                   ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUsePostFSRInactiveLastWorkingQueue) {
            // After FSR->DLSS->off with scQueue intentionally unset, reuse the
            // last queue that already proved it could render the live swapchain.
            if (lastWorkingQueueStillActiveDuringRecentTeardown && g_OriginalGameQueue) {
                gameQueue = g_OriginalGameQueue;
                static std::atomic<int> s_postFSRProcessFrameOriginalRouteLogCount{0};
                int logCount = s_postFSRProcessFrameOriginalRouteLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 10 || (logCount % 300) == 0) {
                    HookLogImportant(
                        "DX12: ProcessFrame — deferring preserved PostSL lastWorking queue %p because teardown traffic "
                        "is still active (cmdQ=%p origQ=%p primaryQ=%p)",
                        g_PostSLLastWorkingQueue, currentCommandQueue, g_OriginalGameQueue, currentPrimaryQueue);
                }
            } else {
                gameQueue = g_PostSLLastWorkingQueue;
            }
        } else if (routingDecision ==
                   ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUsePostFSRInactiveOriginalQueue) {
            // After FSR->DLSS->off with scQueue intentionally unset, prefer the
            // known original Present queue over the most recent ECL queue.
            // Talos uses separate render/present DIRECT queues; falling back to
            // g_CommandQueue/primary picked the render queue and immediately hit
            // DEVICE_REMOVED on the first recovered non-FG offscreen composite.
            if (currentCommandQueue != nullptr && currentCommandQueue == currentPrimaryQueue) {
                gameQueue = currentCommandQueue;
                static std::atomic<int> s_postFSRInactivePrimaryRouteLogCount{0};
                int logCount = s_postFSRInactivePrimaryRouteLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 10 || (logCount % 300) == 0) {
                    HookLogImportant(
                        "DX12: ProcessFrame — post-FSR inactive recovery using settled primary queue %p "
                        "instead of origGame %p (cmdQ=%p)",
                        gameQueue, g_OriginalGameQueue, currentCommandQueue);
                }
            } else {
                gameQueue = g_OriginalGameQueue;
            }
        } else if (routingDecision == ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUseFSRSwapchainQueue) {
            // FSR FG: pSwapChain is FSR's swapchain, backbuffers belong to
            // FSR's queue.  Submit on the swapchain queue to avoid cross-queue
            // resource state conflicts.  We use realECL to bypass FSR's ECL
            // hook on this queue.
            gameQueue = g_SwapchainQueue;
            if (!g_HadFSRFGPhase &&
                ce::dx12_overlay_policy::ShouldLatchFSRFGHistory(g_FGCompat.IsFSRFGApiActive(), true)) {
                g_HadFSRFGPhase = true;
                HookLogImportant(
                    "DX12: ProcessFrame — FSR FG history confirmed, origGame potentially corrupted for future DLSS FG");
            }
        } else if (routingDecision ==
                   ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUseRuntimeOwnedSwapchainQueue) {
            // Runtime-owned swapchain without FSR evidence. This covers DLSS/
            // Streamline suspend-resume windows where the live swapchain stays on
            // a non-game queue but must NOT be promoted into post-FSR recovery.
            gameQueue = g_SwapchainQueue;
            static int s_runtimeOwnedQueueLogCount = 0;
            if (s_runtimeOwnedQueueLogCount++ < 10 || (s_runtimeOwnedQueueLogCount % 300) == 0) {
                const bool authoritativeFSR = g_FGCompat.IsFSRFGApiActive();
                HookLogImportant(
                    "DX12: ProcessFrame — runtime-owned swapchain %s, using scQueue %p "
                    "(origGame=%p slFG=%d hadFSR=%d apiFSR=%d) #%d",
                    authoritativeFSR ? "with authoritative FSR FG state" : "without FSR evidence", gameQueue,
                    g_OriginalGameQueue, slFGNow ? 1 : 0, g_HadFSRFGPhase ? 1 : 0, authoritativeFSR ? 1 : 0,
                    s_runtimeOwnedQueueLogCount);
            }
        } else if (routingDecision ==
                   ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kSkipFSRWithoutSwapchainQueue) {
            // FSR FG active but g_SwapchainQueue not captured.
            // DO NOT fall back to origGame — FSR FG uses origGame internally
            // and injecting our ECLs on it will corrupt FSR's fence tracking,
            // causing an internal FSR deadlock (ffxQuery spin-wait or WaitForSingleObject).
            // Instead, skip rendering entirely until scQueue is recaptured.
            static int s_fsrSkipLog = 0;
            if (s_fsrSkipLog++ < 5 || (s_fsrSkipLog % 300) == 0) {
                HookLogImportant(
                    "DX12: ProcessFrame — FSR FG active but scQueue=null, SKIPPING overlay (origGame=%p used by FSR, "
                    "#%d)",
                    g_OriginalGameQueue, s_fsrSkipLog);
            }
            return;
        } else {
            gameQueue = g_SwapchainQueue;
            if (!gameQueue)
                gameQueue = g_CommandQueue.load();
        }
    }
    if (!gameQueue) {
        HookLog("DX12: ProcessFrame - no game queue, skipping overlay");
        return;
    }

    // Log queue selection decision (rate-limited: first 10, then every 300)
    {
        static int s_queueLogCount = 0;
        ++s_queueLogCount;
        if (s_queueLogCount <= 10 || (s_queueLogCount % 300) == 0) {
            bool slFGNow = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
            bool fsrFGActive = IsFSRFrameGenerationActive();
            const char* qPath = "unknown";
            if (slFGNow && g_OriginalGameQueue && gameQueue == g_OriginalGameQueue)
                qPath = "origGame(SL-FG)";
            else if (fsrFGActive && gameQueue == g_SwapchainQueue)
                qPath = "scQueue(FSR-FG)";
            else if (fsrFGActive && gameQueue == g_OriginalGameQueue)
                qPath = "origGame(FSR-FG-fallback)";
            else if (!slFGNow && !fsrFGActive && g_HadFSRFGPhase && !g_SwapchainQueue && g_PostSLLastWorkingQueue &&
                     gameQueue == g_PostSLLastWorkingQueue)
                qPath = "lastWorking(post-FSR)";
            else if (!slFGNow && !fsrFGActive && g_HadFSRFGPhase && !g_SwapchainQueue && g_OriginalGameQueue &&
                     gameQueue == g_OriginalGameQueue)
                qPath = "origGame(post-FSR)";
            else if (gameQueue == g_SwapchainQueue)
                qPath = "scQueue";
            else if (gameQueue == g_OriginalGameQueue)
                qPath = "origGame";
            else if (gameQueue == g_PrimaryGameQueue.load(std::memory_order_acquire))
                qPath = "primaryQ";
            else if (gameQueue == g_CommandQueue.load(std::memory_order_acquire))
                qPath = "cmdQueue";
            else
                qPath = "otherQ";
            HookLogImportant(
                "DX12: ProcessFrame queue=%p (slFG=%d fsrFG=%d origQ=%p primaryQ=%p scQ=%p cmdQ=%p lastWorkingQ=%p "
                "path=%s) #%d",
                gameQueue, slFGNow ? 1 : 0, fsrFGActive ? 1 : 0, g_OriginalGameQueue,
                g_PrimaryGameQueue.load(std::memory_order_acquire), g_SwapchainQueue, (void*)g_CommandQueue.load(),
                g_PostSLLastWorkingQueue, qPath, s_queueLogCount);
        }
    }

    // Track the game's Present thread ID for pre-SL overlay rendering.
    // During SL FG, SL's worker threads also call Present (for generated frames).
    // Pre-SL overlay must ONLY run on the game thread — SL's workers call Present
    // at the wrong timing (during FG frame Present, not game frame Present).
    {
        DWORD currentTid = GetCurrentThreadId();
        bool slFGNow = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        if (!slFGNow) {
            // When SL FG is NOT active, the current thread IS the game thread.
            // Update the tracked ID (game might switch render threads).
            g_GamePresentThreadId.store(currentTid, std::memory_order_release);
        }
    }

    // Capture the game's original queue ONCE before any FG activation.
    // This queue is guaranteed to be the game's own D3D12 queue (not SL's).
    // During FG transitions, g_SwapchainQueue and g_CommandQueue can both
    // get polluted by SL/FSR internal queues (via CreateSwapChainForHwnd
    // and ECL hooks respectively).
    if (!g_OriginalGameQueue) {
        g_OriginalGameQueue = gameQueue;
        gameQueue->AddRef();  // prevent queue from being freed during FG transitions
        HookLogImportant("DX12: Captured original game queue %p (sc=%p cmd=%p)", gameQueue, g_SwapchainQueue,
                         (void*)g_CommandQueue.load());
    }

    // Queue-change-based FG detection: FSR FG creates its own command queue
    // and reroutes all ECL calls through it.  Detecting a queue pointer change
    // after the first few frames is a strong signal that FG has activated.
    //
    // IMPORTANT: FSR FG alternates between origGame queue and FSR's internal
    // queue every frame (real frame vs interpolated frame).  We use hysteresis
    // to avoid rapid on/off oscillation:
    //   - Activation: trigger immediately on first queue change
    //   - Deactivation: require N CONSECUTIVE frames on initial queue
    //
    // CRITICAL: Use the RAW command queue (g_CommandQueue from ECL hook), NOT
    // the overridden gameQueue.  When FG is active, gameQueue is forced to
    // g_OriginalGameQueue, which would mask FSR's queue alternation and cause
    // the deactivation counter to fire incorrectly.
    {
        static ID3D12CommandQueue* s_initialQueue = nullptr;
        static ID3D12CommandQueue* s_currentFGQueue = nullptr;
        static int s_queueFrameCount = 0;
        static int s_consecutiveInitialQueueFrames = 0;
        constexpr int kDeactivationThreshold = 120;  // ~2s at 60fps before declaring FG off

        // Decrement SL OFF heuristic grace once per ProcessFrame (not per ECL call).
        int slGrace = g_SLOffHeuristicGrace.load(std::memory_order_acquire);
        if (slGrace > 0) {
            g_SLOffHeuristicGrace.store(slGrace - 1, std::memory_order_release);
            // Force-clear any lingering heuristic FSR_FG during the grace window.
            // CanUseFSRFGHeuristics blocks new detections, but a stale true from
            // before SL FG activated can persist because no code path overwrites it.
            if (g_FGCompat.IsHeuristicFSRFGActive()) {
                g_FGCompat.SetHeuristicFSRFGActive(false);
            }
            // Also suppress phantom NVIDIA_SM re-detection during the grace window.
            // ClearNvidiaSMState resets the confirm counter and cached multiplier,
            // but DetectPattern can re-detect within 3 frames if the multiplier
            // rebuilds from recent frame history.  Force-clear each frame.
            if (IsNvidiaSmoothMotionActiveRuntime()) {
                g_FGCompat.ClearNvidiaSMState();
                static int s_phantomSMClears = 0;
                if (s_phantomSMClears++ < 5)
                    HookLogImportant("DX12: Cleared phantom NVIDIA_SM during SL grace (remaining=%d)", slGrace - 1);
            }
        }

        // FG transition handler sets this flag to force a full reset.
        // Without this, SL's leftover queue persists in s_initialQueue/
        // s_currentFGQueue and immediately re-triggers false FSR FG detection.
        if (g_ResetQueueChangeHeuristic.exchange(false, std::memory_order_acquire)) {
            // After SL FG OFF, SL may have created a new swapchain on a
            // different queue.  The game continues using SL's swapchain queue
            // even after FG teardown.  Anchoring to origGame would permanently
            // see the new queue as "different" → endless false FSR_FG.
            //
            // Instead, allow recapture: set s_queueFrameCount = 0 so the next
            // 5 frames capture s_initialQueue from g_CommandQueue.  During the
            // grace period, CanUseFSRFGHeuristics blocks false detections.  By
            // the time grace expires, s_initialQueue reflects the actual queue
            // the game is using (whether that's origGame or SL's persistent queue).
            HookLog(
                "DX12: Queue-change heuristic reset (FG transition) — "
                "was initial=%p fgQ=%p frame=%d (allowing recapture from g_CommandQueue)",
                s_initialQueue, s_currentFGQueue, s_queueFrameCount);
            s_initialQueue = nullptr;
            s_currentFGQueue = nullptr;
            s_queueFrameCount = 0;  // Recapture initial queue from live g_CommandQueue
            s_consecutiveInitialQueueFrames = 0;
        }

        ID3D12CommandQueue* rawQueue = g_CommandQueue.load(std::memory_order_acquire);
        ++s_queueFrameCount;
        if (s_queueFrameCount <= 5) {
            // Capture initial queue during first 5 frames (before FG activates)
            s_initialQueue = rawQueue;
        } else if (s_initialQueue) {
            bool isFGQueue = (rawQueue != s_initialQueue);
            if (isFGQueue) {
                // Reset consecutive-initial counter — we just saw the FG queue
                s_consecutiveInitialQueueFrames = 0;

                if (!s_currentFGQueue) {
                    if (UpdateHeuristicFSRFGState(true, "queue-change")) {
                        // Queue changed away from initial → FSR FG activated
                        s_currentFGQueue = rawQueue;
                        HookLogImportant(
                            "DX12: FG detected via queue change (initial=%p, current=%p, gameQ=%p, frame=%d)",
                            s_initialQueue, rawQueue, gameQueue, s_queueFrameCount);
                    } else {
                        static std::atomic<int> s_ignoredQueueChangeLogCount{0};
                        if (s_ignoredQueueChangeLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
                            HookLog("DX12: Ignoring queue change heuristic (initial=%p, current=%p, rawQ=%p, frame=%d)",
                                    s_initialQueue, rawQueue, gameQueue, s_queueFrameCount);
                        }
                    }
                }
                // else: FG already active, FG queue seen again — normal FSR FG alternation
            } else {
                // Seeing initial queue.  During FSR FG this happens every other frame.
                // Only deactivate after many CONSECUTIVE initial-queue frames.
                if (s_currentFGQueue) {
                    ++s_consecutiveInitialQueueFrames;
                    if (s_consecutiveInitialQueueFrames >= kDeactivationThreshold) {
                        HookLogImportant(
                            "DX12: FG deactivated via queue revert after %d consecutive initial-queue frames "
                            "(initial=%p, fgQueue=%p, frame=%d)",
                            s_consecutiveInitialQueueFrames, s_initialQueue, s_currentFGQueue, s_queueFrameCount);
                        s_currentFGQueue = nullptr;
                        s_consecutiveInitialQueueFrames = 0;
                        UpdateHeuristicFSRFGState(false, "queue-change");
                    } else if (s_consecutiveInitialQueueFrames == 1 || s_consecutiveInitialQueueFrames == 30) {
                        HookLog("DX12: Seeing initial queue while FG active (consecutive=%d/%d, frame=%d)",
                                s_consecutiveInitialQueueFrames, kDeactivationThreshold, s_queueFrameCount);
                    }
                }
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
        ce::overlay_compat::GetStartupBlockingOverlayModuleName() != nullptr && !IsActualFrameGenerationActive();
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
        // CRITICAL: Don't reinitialize overlay during FG transition cooldown.
        // During FG mode switches (e.g., FSR FG → DLSS FG), the SL / FSR runtime
        // is mid-initialization.  Creating D3D12 resources (allocators, fences,
        // PSOs) on a potentially wrong queue can corrupt GPU state, causing the
        // FG runtime to crash (observed: sl_dlss_g exception 0x00008000 in Talos).
        if (g_FGTransitionCooldown > 0) {
            --g_FGTransitionCooldown;
            // Suppress post-SL rendering during cooldown
            g_PostSLOverlayActive.store(false, std::memory_order_release);
            g_PostSLCooldownRemaining.store(g_FGTransitionCooldown, std::memory_order_release);
            static std::atomic<int> s_fgCooldownReinitBlockLogCount{0};
            int logCount = s_fgCooldownReinitBlockLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 10 || g_FGTransitionCooldown == 0) {
                HookLogImportant("DX12: Deferring overlay reinit during FG transition cooldown (%d frames remaining)",
                                 g_FGTransitionCooldown);
            }
            if (g_FGTransitionCooldown == 0) {
                s_fgCooldownReinitBlockLogCount.store(0, std::memory_order_relaxed);
                // Re-enable PostSL if SL FG is active NOW.
                // The main cooldown code (inside overlayInit block) won't run because
                // we're about to reinit (overlayInit=false).  Without this, PostSL
                // stays inactive and the pre-SL render runs — which crashes during
                // DLSS FG because pre-SL ECL perturbs SL's FG pipeline.
                bool slFGNow = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
                if (slFGNow && DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed)) {
                    g_PostSLOverlayActive.store(true, std::memory_order_release);
                    g_PostSLSyntheticStartupActivationPending.store(false, std::memory_order_release);
                    DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(false, std::memory_order_release);
                    HookLogImportant(
                        "DX12: FG transition cooldown complete — reactivated PostSL (slFG=1, reinit path)");
                } else {
                    HookLogImportant(
                        "DX12: FG transition cooldown complete — overlay reinit will proceed next frame (slFG=%d)",
                        slFGNow ? 1 : 0);
                }
            }
            // Skip reinit but continue ProcessFrame.
            goto skipOverlayInit;
        }

        // Don't reinit during active SL FG if PostSL callback isn't registered yet.
        // Without PostSL, the overlay would try pre-SL rendering on origGame while
        // backbuffers are on SL's swapchain queue → cross-queue ERR_GFX_STATE.
        // Once PostSL is registered, reinit is safe — PostSL renders on scQueue.
        {
            bool slFGNow = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
            if (slFGNow) {
                auto* callback = DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire);
                if (!callback) {
                    static int s_slDeferLogCount = 0;
                    if (s_slDeferLogCount++ < 5) {
                        HookLogImportant("DX12: Deferring overlay reinit — SL FG active, PostSL not registered yet");
                    }
                    goto skipOverlayInit;
                }
            }
        }

        {
            ID3D12CommandQueue* currentSwapchainQueue = nullptr;
            {
                std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
                currentSwapchainQueue = g_SwapchainQueue;
            }
            ID3D12CommandQueue* currentCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
            bool actualFGActive = IsActualFrameGenerationActive();
            bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
            const bool recentStreamlineTeardown = g_SLOffHeuristicGrace.load(std::memory_order_acquire) > 0;
            const bool lastWorkingQueueStillActiveDuringRecentTeardown =
                g_PostSLLastWorkingQueue != nullptr &&
                GetTickCount64() < g_PostSLRecentTeardownActivityUntilMs.load(std::memory_order_acquire);
            int slOffSwapchainGrace = g_SLOffSwapchainReinitGrace.load(std::memory_order_acquire);
            if (ce::dx12_overlay_policy::ShouldDeferInactiveRuntimeOwnedSwapchainOverlayInit(
                    actualFGActive, streamlineFGRunning, g_FGRuntimeOwnsSwapchain, currentSwapchainQueue != nullptr,
                    currentCommandQueue != nullptr,
                    currentCommandQueue != nullptr && currentCommandQueue == currentSwapchainQueue)) {
                static std::atomic<int> s_runtimeOwnedInactiveInitDeferLogCount{0};
                int logCount = s_runtimeOwnedInactiveInitDeferLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 20 || (logCount % 120) == 0) {
                    HookLogImportant(
                        "DX12: Deferring inactive runtime-owned swapchain overlay init until queue settles "
                        "(slOffGrace=%d scQ=%p cmdQ=%p fgOwned=%d)",
                        slOffSwapchainGrace, currentSwapchainQueue, currentCommandQueue,
                        g_FGRuntimeOwnsSwapchain ? 1 : 0);
                }
                goto skipOverlayInit;
            }

            if (ce::dx12_overlay_policy::ShouldDeferPostFSRRecoveryWhileLastWorkingQueueStillSeesRecentTeardown(
                    recentStreamlineTeardown, g_PostSLLastWorkingQueue != nullptr,
                    lastWorkingQueueStillActiveDuringRecentTeardown) &&
                !actualFGActive && !streamlineFGRunning && g_HadFSRFGPhase && currentSwapchainQueue == nullptr) {
                static std::atomic<int> s_postFSRRecentTeardownDeferLogCount{0};
                int logCount = s_postFSRRecentTeardownDeferLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 10 || (logCount % 128) == 0) {
                    HookLogImportant(
                        "DX12: Deferring post-FSR non-FG overlay init because lastWorking queue %p is still seeing "
                        "recent Streamline teardown activity (cmdQ=%p origQ=%p primaryQ=%p)",
                        g_PostSLLastWorkingQueue, currentCommandQueue, g_OriginalGameQueue,
                        g_PrimaryGameQueue.load(std::memory_order_acquire));
                }
                goto skipOverlayInit;
            }

            if (ce::dx12_overlay_policy::ShouldDeferOverlayInitUntilCommandQueueSettlesAfterRecentStreamlineTeardown(
                    actualFGActive, streamlineFGRunning, recentStreamlineTeardown, currentSwapchainQueue != nullptr,
                    g_OriginalGameQueue != nullptr, g_PostSLLastWorkingQueue != nullptr, currentCommandQueue != nullptr,
                    currentCommandQueue != nullptr && currentCommandQueue == currentSwapchainQueue,
                    currentCommandQueue != nullptr && currentCommandQueue == g_OriginalGameQueue,
                    currentCommandQueue != nullptr &&
                        currentCommandQueue == g_PrimaryGameQueue.load(std::memory_order_acquire))) {
                static std::atomic<int> s_recentSLTeardownInitDeferLogCount{0};
                int logCount = s_recentSLTeardownInitDeferLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 20 || (logCount % 120) == 0) {
                    HookLogImportant(
                        "DX12: Deferring overlay init until command queue settles after recent Streamline teardown "
                        "(scQ=%p cmdQ=%p origQ=%p primaryQ=%p lastWorkingQ=%p slOffGrace=%d)",
                        currentSwapchainQueue, currentCommandQueue, g_OriginalGameQueue,
                        g_PrimaryGameQueue.load(std::memory_order_acquire), g_PostSLLastWorkingQueue,
                        g_SLOffHeuristicGrace.load(std::memory_order_acquire));
                }
                goto skipOverlayInit;
            }
        }

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

                    // Propagate HDR state to media engine via shared memory
                    if (g_pSharedMem) {
                        g_pSharedMem->SetIsHDR(isActualHDR);
                    }

                    if (s_startupOverlayActivationStage ==
                        StartupOverlayActivationStage::kDelayRTVInitAfterBackendInit) {
                        s_startupOverlayActivationStageMs = GetTickCount64();
                        HookLogImportant(
                            "DX12: Startup compat staged activation - backend init complete, delaying RTV init for "
                            "%llums",
                            kStartupOverlayPostBackendInitSettleMs);
                    } else {
                        int actualBufferCount = desc.BufferCount;
                        if (actualBufferCount > 8) {
                            HookLog("DX12: Swapchain has %d buffers, limiting RTVs to 8", actualBufferCount);
                            actualBufferCount = 8;
                        }
                        CreateRTVs(g_Device.load(), sc3, actualBufferCount);
                        if (!g_State.rtvDescHeap) {
                            HookLogImportant(
                                "DX12: RTV initialization failed during overlay init, deferring sync init");
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
skipOverlayInit:  // FG cooldown guard jumps here to skip reinit but continue ProcessFrame

    // CRITICAL FIX: Decrement FG transition cooldown when overlayInit=true but syncInit=false.
    // The !overlayInit path (line 4783) decrements when overlay needs full reinit.
    // The overlayInit+syncInit path (line 5334) decrements during normal rendering.
    // But when overlayInit=true and syncInit=false (FG transition invalidated sync
    // resources only), NEITHER path runs — the cooldown stays forever, permanently
    // blocking staged activation and overlay rendering.
    if (g_State.overlayInit && !g_State.syncInit && g_FGTransitionCooldown > 0) {
        --g_FGTransitionCooldown;
        g_PostSLOverlayActive.store(false, std::memory_order_release);
        g_PostSLCooldownRemaining.store(g_FGTransitionCooldown, std::memory_order_release);
        static std::atomic<int> s_synclessCooldownLogCount{0};
        int logCount = s_synclessCooldownLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || g_FGTransitionCooldown == 0) {
            HookLogImportant("DX12: FG cooldown (sync-invalidated path): %d frames remaining", g_FGTransitionCooldown);
        }
        if (g_FGTransitionCooldown == 0) {
            s_synclessCooldownLogCount.store(0, std::memory_order_relaxed);
            HookLogImportant("DX12: FG cooldown complete (sync-invalidated) — staged activation can proceed");
            // Device health check after cooldown — if device already dead,
            // sync reinit would be futile and might trigger secondary crashes.
            auto* cooldownDev = g_Device.load(std::memory_order_acquire);
            if (cooldownDev) {
                HRESULT cooldownDevHr = cooldownDev->GetDeviceRemovedReason();
                if (FAILED(cooldownDevHr)) {
                    HookLogImportant("DX12: WARNING — device already dead at cooldown end! hr=0x%08X", cooldownDevHr);
                }
            }
        }
    }

    // CRITICAL: Don't run staged sync activation during FG transition cooldown.
    // The cooldown goto above skips normal overlay init but lands HERE — and with
    // syncInit=false (cleared by the transition), this block would run InitOverlaySync
    // while the FG runtime is mid-initialization.  Destroying and recreating sync
    // resources during the transition corrupts GPU state → DEVICE_REMOVED.
    if (allowOverlayRender && !suspendOverlayRender && !s_insideECL && g_State.overlayInit && !g_State.syncInit &&
        g_FGTransitionCooldown <= 0) {
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
    SharedMemoryLayout* captureShm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    OverlayConfig captureOverlayCfg = GetActiveDX12OverlayConfig(captureShm);
    const bool captureWantsOverlay = captureOverlayCfg.showOverlay && captureOverlayCfg.captureIncludeOverlay;
    const bool captureUsePostSL = processCapture && g_IPC && g_IPC->IsRecording() && captureWantsOverlay &&
                                  ShouldUseConfirmedPostSLForOverlayIncludedWork(captureOverlayCfg);
    const bool captureAfterOverlay =
        processCapture && g_IPC && g_IPC->IsRecording() && captureWantsOverlay && !captureUsePostSL;
    const bool captureBeforeOverlay = processCapture && g_IPC && g_IPC->IsRecording() && !captureWantsOverlay;
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
                    "DX12: Keeping overlay rendering disabled while startup-blocking render module %s remains loaded "
                    "for %s (remaining=%llums)",
                    blockingOverlayModule, g_ProcessName,
                    kStartupOverlayLoadedRenderModuleMaxBlockMs - msSinceSyncInit);
            }
            suppressOverlayRenderForLoadedStartupOverlay = true;
        } else if (ce::overlay_compat::ShouldSuppressDX12OverlayRenderForRecentBlockingRendererActivity(
                       processNeedsRenderDelay, actualFGActive, blockingOverlayModule,
                       hasRecentBlockingRenderActivity)) {
            static std::atomic<int> s_recentBlockingRendererSuppressLogCount{0};
            const ULONGLONG msSinceLastActivity = now - lastBlockingRenderActivityMs;
            if (s_recentBlockingRendererSuppressLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
                HookLogImportant(
                    "DX12: Keeping overlay rendering disabled while startup-blocking render module %s still shows "
                    "recent D3D12 activity for %s (quietRemaining=%llums)",
                    blockingOverlayModule, g_ProcessName,
                    kStartupOverlayRenderModuleQuietPeriodMs - msSinceLastActivity);
            }
            suppressOverlayRenderForLoadedStartupOverlay = true;
        } else {
            if (blockingOverlayModule && processNeedsRenderDelay && !actualFGActive) {
                if (lastBlockingRenderActivityMs != 0) {
                    HookLogImportant(
                        "DX12: Startup-blocking render module %s has been quiet for %llums; allowing overlay rendering "
                        "for %s",
                        blockingOverlayModule, now - lastBlockingRenderActivityMs, g_ProcessName);
                } else {
                    HookLogImportant(
                        "DX12: Startup-blocking render module %s exceeded the startup safety window with no recent "
                        "activity; allowing overlay rendering for %s",
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
                    "DX12: Waiting to draw staged overlay after resource priming for %s (remaining=%llums)",
                    g_ProcessName, kStartupOverlayPostResourcePrimeSettleMs - msSinceResourcePrime);
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
                                 g_ProcessName, kStartupOverlayFirstDrawProbeSettleMs - msSinceProbe);
            }
            delayOverlayRenderAfterFirstDrawProbe = true;
        } else {
            HookLogImportant("DX12: GTA overlay probe settle complete - allowing %s for %s",
                             GetStartupOverlayFirstDrawProbeStageName(s_startupOverlayFirstDrawProbeStage),
                             g_ProcessName);
            s_startupOverlayFirstDrawProbeMs = 0;
        }
    }

    // =========================================================================
    // FG STATE MANAGEMENT — runs unconditionally when overlay resources exist.
    // =========================================================================
    // Early PostSL registration — break the chicken-and-egg deadlock:
    //   - Reinit guard blocks init during SL FG when PostSL isn't registered
    //   - Outer block registers PostSL but requires overlayInit (which needs init)
    // Fix: register PostSL BEFORE the outer block, regardless of overlayInit.
    // The PostSL callback safely handles !overlayInit (returns early at line 4026).
    // =========================================================================
    {
        bool slFGNow = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        if (slFGNow && g_FGTransitionCooldown == 0) {
            if (DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) !=
                &PostSLOverlayRenderGated) {
                SetPostSLCallbackInstalled(true, "DX12: Early PostSL registration");
                HookLogImportant("DX12: Early PostSL registration (overlayInit=%d syncInit=%d)",
                                 g_State.overlayInit ? 1 : 0, g_State.syncInit ? 1 : 0);
            }
        }
    }

    // =========================================================================
    //
    // CRITICAL: This block must run even when overlay RENDERING is blocked
    // (e.g., by the startup overlay blocker setting allowOverlayRender=false).
    // Without it, FG ON↔OFF transitions are missed while the overlay is blocked,
    // causing:
    //   - g_SwapchainQueue stays null → startup blocker keeps blocking forever
    //   - Queue-change heuristic never reset → false FSR FG on next FG cycle
    //   - PostSL warmup counter never reset → stall fallback fires during warmup
    //   - PostSL callback not managed → stale callbacks fire on wrong state
    //
    // The inner block (inside allowOverlayRender gate) also handles transitions
    // but only runs when rendering is allowed.  This outer block is the safety
    // net that ensures state is ALWAYS correct.
    if (!s_insideECL && g_State.overlayInit && g_State.syncInit) {
        bool outerSLFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        bool previousOuterSLFGRunning = g_OuterTrackedSLFGRunning.load(std::memory_order_acquire);

        if (outerSLFGRunning != previousOuterSLFGRunning) {
            bool slTurnedOff = previousOuterSLFGRunning && !outerSLFGRunning;
            bool slTurnedOn = !previousOuterSLFGRunning && outerSLFGRunning;
            g_OuterTrackedSLFGRunning.store(outerSLFGRunning, std::memory_order_release);

            HookLogImportant("DX12: [outer] SL FG %s (allowOverlayRender=%d)", slTurnedOn ? "ON" : "OFF",
                             allowOverlayRender ? 1 : 0);

            // Set cooldown — prevents rendering during transition window
            g_FGTransitionCooldown = std::max(g_FGTransitionCooldown, 60);
            g_PostSLCooldownRemaining.store(g_FGTransitionCooldown, std::memory_order_release);

            // Reset PostSL state for fresh start after transition.
            // Keep the callback installed on Streamline FG activation so
            // startup synthetic presents can immediately find it.
            g_PostSLOverlayActive.store(false, std::memory_order_release);
            if (!DXGIShared::ShouldKeepPostSLCallbackInstalledDuringTransition(outerSLFGRunning)) {
                SetPostSLCallbackInstalled(false, "DX12: [outer] SL transition");
            }
            g_PostSLStallCounter.store(0, std::memory_order_release);
            g_PostSLStableFrameCount.store(0, std::memory_order_release);
            g_PostSLConfirmedRendering.store(false, std::memory_order_release);

            // Clear false heuristic FSR FG (SL's queues trigger queue-change heuristic)
            if (g_FGCompat.IsHeuristicFSRFGActive()) {
                g_FGCompat.SetHeuristicFSRFGActive(false);
                HookLogImportant("DX12: [outer] Cleared heuristic FSR FG during SL FG %s", slTurnedOn ? "ON" : "OFF");
            }

            // Clear NVIDIA_SM detection state — the cached 2× multiplier from
            // departing DLSS FG would otherwise trigger false NVIDIA_SM detection
            // in DetectPattern() within a few frames.
            g_FGCompat.ClearNvidiaSMState();

            // Reset queue-change heuristic so it re-captures initial queue
            g_ResetQueueChangeHeuristic.store(true, std::memory_order_release);

            // Bump epoch so the inner transition handler skips redundant processing
            g_OuterSLTransitionEpoch.fetch_add(1, std::memory_order_release);

            if (slTurnedOff) {
                // Suppress queue-change heuristic for frames after SL OFF.
                // The heuristic runs BEFORE this outer block in ProcessFrame, so
                // on the frame SL turns off, it sees queue switch (SL→origGame)
                // before the reset flag is set → false FSR_FG.
                // Use 600 frames (~4s@150fps) to cover high-fps menus where
                // SL's swapchain queue persists after FG teardown.
                g_SLOffHeuristicGrace.store(600, std::memory_order_release);
                g_SLOffSwapchainReinitGrace.store(300, std::memory_order_release);

                // DO NOT restore g_SwapchainQueue to g_OriginalGameQueue here.
                // When SL activates FG, it calls CreateSwapChainForHwnd with its
                // own queue (e.g. F0A0).  After FG teardown, SL's swapchain
                // PERSISTS — the game continues presenting on F0A0, not the
                // original game queue (F620).  Restoring to F620 causes a
                // queue/swapchain mismatch: we'd render to F0A0's backbuffers
                // on F620 → DXGI_ERROR_ACCESS_DENIED → DEVICE_REMOVED.
                //
                // g_SwapchainQueue already holds the correct value from the
                // CreateSwapChainForHwnd hook.  If the game creates a new
                // swapchain later, the hook updates g_SwapchainQueue.
                {
                    std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                    HookLogImportant(
                        "DX12: [outer] FG→off — keeping g_SwapchainQueue=%p "
                        "(origGame=%p) — SL swapchain persists after teardown",
                        g_SwapchainQueue, g_OriginalGameQueue);
                }

                // Disable PostSL immediately — SL is tearing down
                g_PostSLOverlayActive.store(false, std::memory_order_release);
                SetPostSLCallbackInstalled(false, "DX12: [outer] FG->off");

                // Drain in-flight GPU work
                if (g_State.fence) {
                    UINT64 lastVal = g_State.currentFenceValue;
                    HANDLE drainEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                    if (drainEvent) {
                        HRESULT drainHr = g_State.fence->SetEventOnCompletion(lastVal, drainEvent);
                        if (SUCCEEDED(drainHr)) {
                            DWORD waitResult = WaitForSingleObject(drainEvent, 200);
                            HookLogImportant("DX12: [outer] FG→off GPU drain: fenceVal=%llu wait=%s", lastVal,
                                             waitResult == WAIT_OBJECT_0 ? "OK" : "TIMEOUT");
                        } else {
                            HookLogImportant("DX12: [outer] FG→off GPU drain FAILED: hr=0x%08X", (unsigned)drainHr);
                        }
                        CloseHandle(drainEvent);
                    }
                }

                // Force overlay reinit — PostSL's RTVs reference SL's swapchain
                // backbuffers, which become invalid after SL tears down FG.  Without
                // reinit, pre-SL rendering uses stale RTVs → DEVICE_HUNG.
                if (g_State.overlayInit) {
                    HookLogImportant("DX12: [outer] FG→off — forcing overlay reinit (stale SL backbuffers)");
                    g_State.overlayInit = false;
                    CleanupRTVs();
                }
                g_ResetReinitSubmitCounter.store(true, std::memory_order_release);

                // Clear realECL — it was probed from a temporary queue during
                // SL activation and may reference per-instance driver dispatch
                // state that doesn't match the game queue.  After SL teardown,
                // fall back to origECL (saved from the game queue's vtable
                // before our hook was installed).
                {
                    auto* oldRealECL = (void*)g_RealD3D12ECL.load(std::memory_order_acquire);
                    g_RealD3D12ECL.store(nullptr, std::memory_order_release);
                    HookLogImportant("DX12: [outer] FG→off — cleared realECL %p (will use origECL after reinit)",
                                     oldRealECL);
                }
            }

            if (slTurnedOn) {
                // Probe real D3D12 ECL when SL FG first activates — PostSL needs it
                // to bypass SL's COM wrapper.  The inner transition handler also does
                // this, but the epoch sync skips it for transitions already handled here.
                auto* dev = g_Device.load(std::memory_order_acquire);
                if (dev && IsStreamlineLoaded()) {
                    ProbeRealD3D12ECL(dev);
                    auto* probed = (void*)g_RealD3D12ECL.load(std::memory_order_acquire);
                    HookLogImportant("DX12: [outer] SL FG ON — probed realECL=%p (dev=%p)", probed, dev);
                } else {
                    HookLogImportant("DX12: [outer] SL FG ON — skipped ECL probe (dev=%p, SL=%d)", dev,
                                     IsStreamlineLoaded() ? 1 : 0);
                }
            }
        }

        // Cooldown countdown — must always tick even when overlay blocked
        if (g_FGTransitionCooldown > 0 && !allowOverlayRender) {
            // Only decrement here when the inner block won't run.
            // The inner block (inside allowOverlayRender gate) has its own
            // countdown logic.  Avoid double-decrementing.
            --g_FGTransitionCooldown;
            g_PostSLOverlayActive.store(false, std::memory_order_release);
            g_PostSLCooldownRemaining.store(g_FGTransitionCooldown, std::memory_order_release);
            if (g_FGTransitionCooldown == 0) {
                HookLogImportant("DX12: [outer] FG transition cooldown complete (slFG=%d)", outerSLFGRunning ? 1 : 0);
                if (outerSLFGRunning) {
                    g_PostSLOverlayActive.store(true, std::memory_order_release);
                    DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(false, std::memory_order_release);
                }
            }
        }

        // PostSL callback management — register when SL FG active, even if overlay blocked
        if (outerSLFGRunning && g_FGTransitionCooldown == 0) {
            if (DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) !=
                &PostSLOverlayRenderGated) {
                SetPostSLCallbackInstalled(true, "DX12: [outer] Registered PostSL callback");
                g_PostSLOverlayActive.store(true, std::memory_order_release);
                g_PostSLSyntheticStartupActivationPending.store(false, std::memory_order_release);
                DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(false, std::memory_order_release);
                HookLogImportant("DX12: [outer] Registered PostSL callback (overlay blocked, SL FG active)");
            }
        } else if (!outerSLFGRunning && g_FGTransitionCooldown == 0) {
            if (DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) != nullptr) {
                SetPostSLCallbackInstalled(false, "DX12: [outer] cooldown complete");
                g_PostSLOverlayActive.store(false, std::memory_order_release);
                g_PostSLConfirmedRendering.store(false, std::memory_order_release);
                DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(false, std::memory_order_release);
            }
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

        // FG state transition cooldown: skip overlay draws for a brief window
        // after FG mode changes to let Streamline stabilize its internal state.
        // Unlike the old cooldown (which did teardown/reinit and caused resource
        // churn crashes), this only pauses the draw — no resources are destroyed.
        static bool s_lastFGActive = false;
        static ce::fg_runtime::RuntimeMode s_lastRuntimeMode = ce::fg_runtime::RuntimeMode::kOff;
        static bool s_lastSLFGRunning = false;
        // NOTE: FG transition cooldown is now file-scope g_FGTransitionCooldown
        // so swapchain-change detection (earlier in ProcessFrame) can check it.
        bool currentFGActive = g_FGCompat.IsFGActive();
        auto currentRuntimeMode = g_FGCompat.GetRuntimeMode();
        bool currentSLFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);

        // Grace period counter — declared here so epoch sync can reference it.
        static int s_slOffGraceFrames = 0;

        // Epoch sync: when the outer FG state management block has already
        // processed an SL FG transition, bring our tracking variables in sync
        // to avoid redundant transition processing (double cooldowns, duplicate
        // GPU drain, swapchain queue re-clearing).
        static uint32_t s_innerSyncedEpoch = 0;
        uint32_t outerEpoch = g_OuterSLTransitionEpoch.load(std::memory_order_acquire);
        if (s_innerSyncedEpoch != outerEpoch) {
            bool wasSlOn = s_lastSLFGRunning;
            s_lastFGActive = currentFGActive;
            s_lastRuntimeMode = currentRuntimeMode;
            s_lastSLFGRunning = currentSLFGRunning;
            s_innerSyncedEpoch = outerEpoch;
            // If SL turned off, start grace period (mirroring normal detection)
            if (wasSlOn && !currentSLFGRunning) {
                s_slOffGraceFrames = 300;
            }
            HookLogImportant(
                "DX12: [inner] Synced tracking to outer epoch %u (fgActive=%d runtime=%s slFG=%d grace=%d)", outerEpoch,
                currentFGActive ? 1 : 0, ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode),
                currentSLFGRunning ? 1 : 0, s_slOffGraceFrames);
        }

        // If SL directly signals FG is running, force currentFGActive true.
        // Heuristic detection may lag behind the SL hook's immediate signal,
        // creating a gap where IsFGActive() returns false even though SL FG
        // is already processing frames.
        if (currentSLFGRunning && !currentFGActive) {
            currentFGActive = true;
        }

        // When SL signal goes from ON→OFF, the ECL heuristic may briefly
        // false-positive as FSR_FG (elevated ECL count from departing DLSS FG
        // looks like frame generation).  Suppress non-API FG detection for a
        // grace period after SL deactivates.  Also suppresses NVIDIA_SM false
        // positives from the cached 2× multiplier.
        if (s_lastSLFGRunning && !currentSLFGRunning) {
            // SL just turned OFF — start grace period.
            // 300 frames covers the slow ECL ratio decay after DLSS FG shutdown.
            s_slOffGraceFrames = 300;
            HookLogImportant("DX12: SL FG OFF — suppressing heuristic FG for 300 frames");
        }
        if (s_slOffGraceFrames > 0) {
            s_slOffGraceFrames--;
            // During grace period after SL FG OFF, suppress ALL non-API-confirmed
            // FG types.  The cached 2× multiplier from departing DLSS FG falsely
            // activates NVIDIA_SM detection, and elevated ECL counts falsely
            // trigger heuristic FSR_FG.  Only trust explicit API hooks
            // (fsrFGApiActive from ffxCreateContext).  DLSS_FG API requires SL
            // running, which is false during this grace period.
            if (!currentSLFGRunning && currentFGActive) {
                bool fsrApiConfirmed = g_FGCompat.IsFSRFGApiActive();
                if (!fsrApiConfirmed) {
                    currentFGActive = false;
                    currentRuntimeMode = ce::fg_runtime::RuntimeMode::kOff;
                }
            }
        }

        // Detect FG on/off changes AND FG type changes (e.g., FSR FG → DLSS FG)
        // Also detect SL FG signal changes (immediate from SL hook)
        bool fgChanged = (currentFGActive != s_lastFGActive);
        bool runtimeModeChanged = (currentRuntimeMode != s_lastRuntimeMode);
        bool slSignalChanged = (currentSLFGRunning != s_lastSLFGRunning);

        if (fgChanged || runtimeModeChanged || slSignalChanged) {
            auto s_lastRuntimeMode_saved = s_lastRuntimeMode;  // save before update for syncInit logic
            const bool previousWasFG = ce::fg_runtime::IsActualGeneratedFrameMode(s_lastRuntimeMode_saved);
            const bool targetIsFGOff = !ce::fg_runtime::IsActualGeneratedFrameMode(currentRuntimeMode);
            const ID3D12CommandQueue* currentPrimaryQueue = g_PrimaryGameQueue.load(std::memory_order_acquire);
            const ID3D12CommandQueue* currentCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
            const bool commandQueueSettledToPrimary =
                currentCommandQueue != nullptr && currentCommandQueue == currentPrimaryQueue;
            const int transitionCooldownFrames =
                ce::dx12_overlay_policy::ShouldUseShortPostFSRInactiveCooldown(
                    commandQueueSettledToPrimary, g_HadFSRFGPhase,
                    previousWasFG && targetIsFGOff && !currentSLFGRunning)
                    ? 15
                    : 60;
            HookLogImportant(
                "DX12: FG %s%s%s: %s(%s) -> %s(%s) slSignal=%d->%d — cooldown %d frames",
                fgChanged ? "state " : "", runtimeModeChanged ? "mode " : "", slSignalChanged ? "slSignal " : "",
                s_lastFGActive ? "active" : "inactive", ce::fg_runtime::GetRuntimeModeName(s_lastRuntimeMode),
                currentFGActive ? "active" : "inactive", ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode),
                s_lastSLFGRunning ? 1 : 0, currentSLFGRunning ? 1 : 0, transitionCooldownFrames);
            s_lastFGActive = currentFGActive;
            s_lastRuntimeMode = currentRuntimeMode;
            s_lastSLFGRunning = currentSLFGRunning;
            g_FGTransitionCooldown = ce::dx12_overlay_policy::ResolveTransitionCooldownFrames(
                g_FGTransitionCooldown, transitionCooldownFrames,
                ce::dx12_overlay_policy::ShouldUseShortPostFSRInactiveCooldown(
                    commandQueueSettledToPrimary, g_HadFSRFGPhase,
                    previousWasFG && targetIsFGOff && !currentSLFGRunning));
            g_PostSLCooldownRemaining.store(g_FGTransitionCooldown, std::memory_order_release);

            // Immediately disable post-SL rendering during FG transitions.
            // Keep the callback installed when Streamline is still running so
            // synthetic startup presents can route through PostSL safely while
            // the active gate and cooldown still suppress real rendering.
            g_PostSLOverlayActive.store(false, std::memory_order_release);
            if (!DXGIShared::ShouldKeepPostSLCallbackInstalledDuringTransition(currentSLFGRunning)) {
                SetPostSLCallbackInstalled(false, "DX12: inner FG transition");
            }
            g_PostSLStallCounter.store(0, std::memory_order_release);      // Fresh start after transition
            g_PostSLStableFrameCount.store(0, std::memory_order_release);  // Reset warmup counter

            // When SL FG turns ON or OFF, clear any false heuristic FSR FG state.
            // SL's queue changes trigger the queue-change heuristic, causing
            // false FSR FG detection.  Clear it during ANY SL FG transition:
            //   - ON: SL creates new queues → queue-change heuristic fires falsely
            //   - OFF: SL's queue was "current FG queue" → not cleared until
            //          consecutive initial-queue frames pass the threshold
            if (g_FGCompat.IsHeuristicFSRFGActive()) {
                g_FGCompat.SetHeuristicFSRFGActive(false);
                HookLogImportant("DX12: Cleared heuristic FSR FG during SL FG %s transition",
                                 currentSLFGRunning ? "ON" : "OFF");
            }

            // Reset the queue-change heuristic's internal state so it re-captures
            // the "initial queue" after the transition.  SL's leftover queue would
            // otherwise persist as s_initialQueue/s_currentFGQueue and immediately
            // re-trigger false FSR FG detection.
            g_ResetQueueChangeHeuristic.store(true, std::memory_order_release);

            // Drain in-flight overlay GPU work on ANY FG transition.
            // When FG activates (especially FSR FG), it may use the same queue
            // our overlay was rendering on.  In-flight overlay ECLs on that queue
            // can cause FSR's internal synchronization to deadlock (spin-wait in
            // ffxQuery).  Drain ensures the queue is clean before FG takes over.
            //
            // Original: only drained on SL OFF.  Extended to all transitions
            // because FSR FG also needs a clean queue at activation.
            if (g_State.fence && g_State.currentFenceValue > 0) {
                UINT64 lastVal = g_State.currentFenceValue;
                HANDLE drainEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                if (drainEvent) {
                    HRESULT drainHr = g_State.fence->SetEventOnCompletion(lastVal, drainEvent);
                    if (SUCCEEDED(drainHr)) {
                        DWORD waitResult = WaitForSingleObject(drainEvent, 200);
                        HookLogImportant(
                            "DX12: FG transition — drained overlay GPU work (fenceVal=%llu wait=%u slSignalChanged=%d "
                            "fgChanged=%d)",
                            (unsigned long long)lastVal, waitResult, slSignalChanged ? 1 : 0, fgChanged ? 1 : 0);
                    } else {
                        HookLogImportant("DX12: FG transition — fence drain failed hr=0x%08X", drainHr);
                    }
                    CloseHandle(drainEvent);
                }
            }

            ClearPostSLQueues("DX12: FG transition queue reset");
            // Keep the SL wrapper queue alive while Streamline still owns the
            // presentation path, even if FG is temporarily idle.
            {
                bool targetUsesStreamline = ce::fg_runtime::RuntimeModeUsesStreamline(currentRuntimeMode);
                if (!targetUsesStreamline) {
                    ID3D12CommandQueue* oldWrapper = g_SLWrapperQueue.exchange(nullptr, std::memory_order_acq_rel);
                    if (oldWrapper) {
                        oldWrapper->Release();
                        HookLogImportant("DX12: runtime=%s — released SL wrapper queue %p",
                                         ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode), oldWrapper);
                    }
                } else {
                    ID3D12CommandQueue* kept = g_SLWrapperQueue.load(std::memory_order_acquire);
                    HookLogImportant("DX12: runtime=%s — keeping SL wrapper queue %p alive",
                                     ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode), kept);
                }
            }
            g_PostSLConfirmedRendering.store(false, std::memory_order_release);  // Re-probe needed
            if (!ce::dx12_overlay_policy::ShouldPreservePostSLLastWorkingQueueForPostFSROffRecovery(
                    g_HadFSRFGPhase, previousWasFG, targetIsFGOff)) {
                // Old SL queues may be destroyed after most FG mode switches
                // (e.g., DLSS FG phase 1 -> FSR FG -> DLSS FG phase 2). Keep the
                // last validated queue only for the immediate post-FSR FG-off
                // recovery window, where it is the only queue that already proved
                // safe for the live swapchain.
                SetPostSLLastWorkingQueue(nullptr);
            } else {
                HookLogImportant("DX12: Preserving PostSL lastWorkingQueue %p for immediate post-FSR FG-off recovery",
                                 g_PostSLLastWorkingQueue);
            }

            // Save the current ProcessFrame gameQueue as a pre-FG snapshot.
            // When PostSL activates after the cooldown, g_CommandQueue may have
            // been polluted by SL's internal queues.  gameQueue (resolved at the
            // top of ProcessFrame from scQueue or cmdQueue) is still the game's
            // real queue at this point.
            if (g_PreFGGameQueue)
                g_PreFGGameQueue->Release();
            g_PreFGGameQueue = gameQueue;
            if (gameQueue)
                gameQueue->AddRef();

            // Force sync resources re-initialization on next overlay render.
            // After FG type transitions (e.g., FSR→DLSS), the sync resources
            // (allocators, fence, cmdList) were used on a different queue during
            // the previous FG phase.  Re-using them on a new queue after swapchain
            // recreation causes DEVICE_REMOVED.  Fresh resources avoid this.
            //
            // EXCEPTION 1: FG→off transitions do NOT invalidate sync.  There is no
            // swapchain recreation when FG simply turns off, and the allocators/
            // fence are device-level objects that work on any DIRECT queue.  The
            // GPU drain above ensures all in-flight work completes.
            //
            // EXCEPTION 2: off→on transitions (None→FSR_FG or None→DLSS_FG) also
            // do NOT invalidate sync.  The existing resources are device-level and
            // work on any DIRECT queue.  Forcing re-init here is unnecessary and
            // causes the "FG→FG" misclassification for what is really "off→on".
            bool actualFGToFG = previousWasFG && !targetIsFGOff;
            if (g_State.syncInit && actualFGToFG) {
                HookLogImportant("DX12: FG transition (FG→FG: %s→%s) — forcing syncInit=false for fresh resources",
                                 ce::fg_runtime::GetRuntimeModeName(s_lastRuntimeMode_saved),
                                 ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode));
                g_State.syncInit = false;
            } else if (g_State.syncInit && targetIsFGOff) {
                HookLogImportant("DX12: FG→off transition — keeping syncInit=true (reusing existing resources)");
            } else if (g_State.syncInit && !previousWasFG && !targetIsFGOff) {
                HookLogImportant(
                    "DX12: FG off→on transition (%s→%s) — keeping syncInit=true (resources work on any queue)",
                    ce::fg_runtime::GetRuntimeModeName(s_lastRuntimeMode_saved),
                    ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode));
            }

            // Clear stale swapchain queue only when transitioning TO SL-based FG
            // if we've never had an FSR FG phase. If FSR FG already ran, SL might
            // reuse FSR's swapchain (no new CreateSwapChainForHwnd), so scQueue is
            // the CORRECT queue for backbuffer access. Keep it alive via AddRef.
            if (runtimeModeChanged) {
                bool newTypeNeedsScQueue = (currentRuntimeMode == ce::fg_runtime::RuntimeMode::kFSRFG);
                bool targetIsNone = !ce::fg_runtime::IsActualGeneratedFrameMode(currentRuntimeMode);
                if (targetIsNone && !g_HadFSRFGPhase) {
                    // FG→off: keep g_SwapchainQueue as-is.  Same rationale as
                    // the outer slTurnedOff handler: SL's swapchain may persist
                    // after FG teardown, so g_SwapchainQueue (set by the
                    // CreateSwapChainForHwnd hook) already points to the correct
                    // queue.  Restoring to origGame causes queue/swapchain
                    // mismatch → DXGI_ERROR_ACCESS_DENIED.
                    std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                    // FG is off — FG runtime no longer owns the queue
                    if (g_FGRuntimeOwnsSwapchain) {
                        g_FGRuntimeOwnsSwapchain = false;
                        DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(false, std::memory_order_release);
                        g_FGRuntimeOwnsSwapchainSince = 0;
                        HookLogImportant("DX12: FG→off — clearing FG runtime ownership of swapchain queue");
                    }
                    HookLogImportant("DX12: FG→off — keeping g_SwapchainQueue %p (origGame=%p)", g_SwapchainQueue,
                                     g_OriginalGameQueue);
                    if (!g_SwapchainQueue && g_OriginalGameQueue) {
                        // Swapchain queue not captured yet — fall back to origGame
                        g_OriginalGameQueue->AddRef();
                        g_SwapchainQueue = g_OriginalGameQueue;
                        HookLogImportant("DX12: FG→off — scQueue was null, falling back to origGame %p",
                                         g_OriginalGameQueue);
                    }
                } else if (!newTypeNeedsScQueue && !g_HadFSRFGPhase) {
                    std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                    // Clear FG runtime ownership when transitioning away from FSR FG
                    if (g_FGRuntimeOwnsSwapchain && targetIsNone) {
                        g_FGRuntimeOwnsSwapchain = false;
                        DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(false, std::memory_order_release);
                        g_FGRuntimeOwnsSwapchainSince = 0;
                        HookLogImportant("DX12: FG type change to None — clearing FG runtime ownership");
                    }
                    if (g_SwapchainQueue) {
                        // Protect recently-captured scQueue from phantom FG detections.
                        // After swapchain recreation, the heuristic may briefly detect
                        // NVIDIA_SM (false positive from Present rate measurement).
                        // Clearing scQueue in that window causes ProcessFrame to fall
                        // back to origGame, which FSR FG uses internally → deadlock.
                        ULONGLONG age = GetTickCount64() - g_SwapchainQueueCaptureTime;
                        if (age < 5000) {
                            HookLogImportant(
                                "DX12: FG type change to %s — PRESERVING g_SwapchainQueue %p (captured %llu ms ago, "
                                "too recent to clear)",
                                ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode), g_SwapchainQueue, age);
                        } else {
                            HookLogImportant(
                                "DX12: FG type change to %s — clearing stale g_SwapchainQueue %p (no FSR history, "
                                "age=%llu ms)",
                                ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode), g_SwapchainQueue, age);
                            g_SwapchainQueue->Release();
                            g_SwapchainQueue = nullptr;
                        }
                    }
                } else if (!newTypeNeedsScQueue && g_HadFSRFGPhase) {
                    if (targetIsNone) {
                        // FSR→DLSS→Off: wait for live non-FG command traffic to
                        // prove which queue owns the resumed Present path again.
                        // Forcing origGame back into g_SwapchainQueue here caused
                        // Talos to submit the first recovered non-FG overlay ECL
                        // on the wrong queue/backbuffer pairing, immediately
                        // triggering DEVICE_REMOVED.
                        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                        const bool preserveRuntimeOwnedFSRTeardown =
                            ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedFSRTeardown(
                                targetIsNone, g_HadFSRFGPhase, g_FGRuntimeOwnsSwapchain,
                                DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire));
                        if (preserveRuntimeOwnedFSRTeardown) {
                            HookLogImportant(
                                "DX12: FG→off after FSR phase classified while runtime still owns swapchain — "
                                "preserving FSR queue ownership until a stronger off signal appears "
                                "(scQ=%p origGame=%p primary=%p cmdQ=%p)",
                                g_SwapchainQueue, g_OriginalGameQueue,
                                g_PrimaryGameQueue.load(std::memory_order_acquire),
                                g_CommandQueue.load(std::memory_order_acquire));
                            return;
                        }
                        if (g_FGRuntimeOwnsSwapchain) {
                            g_FGRuntimeOwnsSwapchain = false;
                            DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(false, std::memory_order_release);
                            g_FGRuntimeOwnsSwapchainSince = 0;
                            const bool preserveAuthoritativeFSRDuringTransition =
                                ce::dx12_overlay_policy::ShouldPreserveAuthoritativeFSRDuringTransitionCooldown(
                                    g_FGCompat.IsFSRFGApiActive(), targetIsNone, g_FGTransitionCooldown);
                            if (preserveAuthoritativeFSRDuringTransition) {
                                HookLogImportant(
                                    "DX12: FG→off after FSR phase detected during active transition cooldown — "
                                    "preserving authoritative FSR state until queue topology settles (cooldown=%d "
                                    "scQ=%p origGame=%p primary=%p cmdQ=%p)",
                                    g_FGTransitionCooldown, g_SwapchainQueue, g_OriginalGameQueue,
                                    g_PrimaryGameQueue.load(std::memory_order_acquire),
                                    g_CommandQueue.load(std::memory_order_acquire));
                            } else if (g_FGCompat.IsFSRFGApiActive()) {
                                g_FGCompat.SetFSRFGActive(false);
                                g_FGCompat.SetFSRFGMultiplier(0);
                            }
                            HookLogImportant(
                                "DX12: FG→off after FSR phase — clearing FG runtime ownership of swapchain queue");
                        }
                        if (g_SwapchainQueue && g_SwapchainQueue != g_OriginalGameQueue) {
                            HookLogImportant(
                                "DX12: FG→off after FSR phase — releasing stale g_SwapchainQueue %p and waiting for "
                                "the live non-FG queue to be recaptured (origGame=%p primary=%p cmdQ=%p)",
                                g_SwapchainQueue, g_OriginalGameQueue,
                                g_PrimaryGameQueue.load(std::memory_order_acquire),
                                g_CommandQueue.load(std::memory_order_acquire));
                            g_SwapchainQueue->Release();
                            g_SwapchainQueue = nullptr;
                            g_SwapchainQueueCaptureTime = 0;
                        }
                        if (!g_SwapchainQueue) {
                            HookLogImportant(
                                "DX12: FG→off after FSR phase — keeping g_SwapchainQueue null until non-wrapper "
                                "command traffic settles (origGame=%p primary=%p cmdQ=%p)",
                                g_OriginalGameQueue, g_PrimaryGameQueue.load(std::memory_order_acquire),
                                g_CommandQueue.load(std::memory_order_acquire));
                        }
                    } else {
                        // FSR→DLSS transition: the swapchain was created on FSR's queue
                        // (g_SwapchainQueue), so backbuffers belong to it. Keep it alive.
                        // Render pre-SL on scQueue — the swapchain's own queue has
                        // authorized access to backbuffers without cross-queue issues.
                        HookLogImportant(
                            "DX12: FG type change to %s — KEEPING g_SwapchainQueue %p for backbuffer access (it's the "
                            "swapchain creation queue)",
                            ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode), g_SwapchainQueue);
                        g_NeedGPUDrainBeforeRender = false;
                    }
                } else {
                    HookLogImportant("DX12: FG type change to %s — keeping g_SwapchainQueue %p (FSR needs it)",
                                     ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode), g_SwapchainQueue);
                }
            }

            // Probe real D3D12 ECL when SL FG first activates.  Must happen
            // before the first overlay ECL submission so we have the bypass
            // ready.
            if (currentFGActive && IsStreamlineLoaded()) {
                auto* dev = g_Device.load(std::memory_order_acquire);
                if (dev)
                    ProbeRealD3D12ECL(dev);
            }

            // Flush any pending deferred signal immediately so GPU work from the
            // previous frame completes before SL reconfigures.
            UINT64 deferredVal = g_deferredSignalValue.load(std::memory_order_acquire);
            if (deferredVal != 0 && g_State.fence) {
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
        }
        bool skipOverlayDraw = false;
        if (g_FGTransitionCooldown > 0) {
            --g_FGTransitionCooldown;
            const bool preserveConfirmedPostSLDuringCooldown =
                ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLDuringFGCooldown(
                    currentSLFGRunning, g_PostSLConfirmedRendering.load(std::memory_order_acquire));
            if (preserveConfirmedPostSLDuringCooldown) {
                // Synthetic startup can confirm a working PostSL path before the
                // game-thread cooldown has fully counted down. Do not let the
                // slower ProcessFrame cooldown re-disable that confirmed path.
                g_PostSLOverlayActive.store(true, std::memory_order_release);
                g_PostSLCooldownRemaining.store(0, std::memory_order_release);

                static int s_preserveConfirmedPostSLLog = 0;
                if (s_preserveConfirmedPostSLLog < 10 || g_FGTransitionCooldown == 0) {
                    HookLogImportant(
                        "DX12: FG cooldown preserving confirmed PostSL rendering "
                        "(remaining=%d slSignal=%d)",
                        g_FGTransitionCooldown, currentSLFGRunning ? 1 : 0);
                }
                s_preserveConfirmedPostSLLog++;
            } else {
                // During cooldown, suppress BOTH pre-SL and post-SL rendering.
                g_PostSLOverlayActive.store(false, std::memory_order_release);
                g_PostSLCooldownRemaining.store(g_FGTransitionCooldown, std::memory_order_release);
            }

            // Periodic device-removed check during cooldown to pinpoint
            // when the device dies (overlay is NOT rendering during this time).
            if ((g_FGTransitionCooldown % 10) == 0) {
                auto* cooldownDev = g_Device.load(std::memory_order_acquire);
                if (cooldownDev) {
                    HRESULT cooldownDevHr = cooldownDev->GetDeviceRemovedReason();
                    if (FAILED(cooldownDevHr)) {
                        HookLogImportant(
                            "DX12: DEVICE REMOVED DURING COOLDOWN (cooldown=%d devRemoved=0x%08X tid=0x%04X)",
                            g_FGTransitionCooldown, (unsigned)cooldownDevHr, GetCurrentThreadId());
                    }
                }
            }

            if (g_FGTransitionCooldown == 0) {
                auto fgType = g_FGCompat.GetActiveFGType();
                bool slFG = currentFGActive && DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
                HookLogImportant(
                    "DX12: FG transition cooldown complete — resuming overlay (slFG=%d, fgType=%s, slSignal=%d)",
                    slFG ? 1 : 0, g_FGCompat.GetFGTypeName(fgType), DXGIShared::g_StreamlineFGRunning.load() ? 1 : 0);
                // Re-enable post-SL rendering if SL FG is active
                if (slFG) {
                    g_PostSLOverlayActive.store(true, std::memory_order_release);
                    g_PostSLSyntheticStartupActivationPending.store(false, std::memory_order_release);
                    DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(false, std::memory_order_release);
                }
            }
            skipOverlayDraw = true;
        }

        // POST-SL overlay rendering during SL FG:
        //
        // Why post-SL: Pre-SL rendering submits ECLs on the game queue before SL
        // processes Present.  This crashes at ~600-770 frames because the extra ECL
        // perturbs SL's frame generation pipeline (confirmed by binary search: empty
        // ECL=no crash, drawing ECL=crash, regardless of barriers/fences).
        //
        // Post-SL rendering submits the ECL in the re-entrant Present callback —
        // after SL's FG work but before the real Present flip.  Post-SL empty ECL
        // was proven stable (∞ frames).  The overlay draws to the backbuffer that SL
        // is about to present, using implicit state promotion (no explicit barriers).
        // Real D3D12 ECL bypasses all hooks.  No fence signal.
        // SL captures our overlay as part of the scene, FG interpolates it naturally.
        // Overlay appears on all output frames (real + interpolated).
        const bool slFGActive = currentFGActive && DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        {
            if (slFGActive) {
                // SL FG active: enable POST-SL overlay rendering.
                // The overlay ECL is submitted in the re-entrant Present callback
                // AFTER SL's FG processing but BEFORE the real Present call.
                // This avoids submitting extra ECLs before SL processes the frame
                // (which caused crashes at ~600-770 frames with pre-SL rendering).
                //
                // EXCEPTION: After FSR→DLSS transition, PostSL rendering causes
                // DEVICE_HUNG because the backbuffer resource state is invalid from
                // any queue we have (FSR created the swapchain, SL resized it).
                // In this case, keep rendering pre-SL: the overlay is rendered
                // BEFORE SL's FG pipeline processes the frame, so origGame's state
                // tracking is still valid (game just finished rendering on it).

                // CRITICAL FIX: PostSL rendering now works for pure DLSS FG
                // (no prior FSR) because IsRecursivePresent() correctly treats
                // SL's cross-thread FG Presents as re-entrant.
                //
                // For post-FSR DLSS FG, we use pre-SL rendering instead.
                // PostSL has irreconcilable cross-queue issues: SL's FG pipeline
                // uses its own internal queue, but the swapchain was created by
                // FSR on scQueue.  All queue options (scQueue, origGame, SL
                // wrapper) cause DEVICE_HUNG from cross-queue backbuffer conflicts.
                // POST-SL overlay for ALL SL FG modes (pure DLSS and post-FSR DLSS).
                //
                // Uses origGame queue: SL routes everything through origGame
                // (via its COM wrapper).  PostSL fires in the re-entrant Present
                // path AFTER SL's FG processing is complete.  The backbuffer is
                // in PRESENT state on origGame — same as pure DLSS.
                //
                // Previously we disabled PostSL for post-FSR and used pre-SL,
                // but SL intercepts the game thread's Present at the COM wrapper
                // level during DLSS FG — only SL's worker threads reach our
                // detour.  PostSL (in the re-entrant path) is the only reliable
                // rendering timing for DLSS FG.
                {
                    // Step 1: Register callback (idempotent)
                    if (DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) !=
                        &PostSLOverlayRenderGated) {
                        SetPostSLCallbackInstalled(true, "DX12: SL FG active");
                        HookLogImportant("DX12: SL FG active - registered POST-SL overlay callback (hadFSR=%d)",
                                         g_HadFSRFGPhase ? 1 : 0);
                    }

                    // Step 2: Activate PostSL rendering
                    if (!skipOverlayDraw) {
                        if (!g_PostSLOverlayActive.load(std::memory_order_acquire)) {
                            g_PostSLOverlayActive.store(true, std::memory_order_release);
                            g_PostSLSyntheticStartupActivationPending.store(false, std::memory_order_release);
                            DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(false,
                                                                                            std::memory_order_release);
                            HookLogImportant("DX12: SL FG active - activated POST-SL overlay rendering (hadFSR=%d)",
                                             g_HadFSRFGPhase ? 1 : 0);
                        }
                    } else {
                        if (g_PostSLOverlayActive.load(std::memory_order_acquire)) {
                            g_PostSLOverlayActive.store(false, std::memory_order_release);
                        }
                    }
                    if (!g_RealD3D12ECL.load(std::memory_order_acquire)) {
                        static bool s_noRealECLLogged = false;
                        if (!s_noRealECLLogged) {
                            s_noRealECLLogged = true;
                            HookLogImportant("DX12: No real D3D12 ECL available - disabling overlay during SL FG");
                        }
                        g_PostSLOverlayActive.store(false, std::memory_order_release);
                    }
                    if (g_PostSLConfirmedRendering.load(std::memory_order_acquire)) {
                        // FG "SUSPENSION" STALL DETECTION:
                        //
                        // PostSL was previously confirmed rendering, but it may have
                        // stalled.  This happens when:
                        //   1. DLSS FG is "nominally on" (g_StreamlineFGRunning=true,
                        //      slDLSSGSetOptions was NOT called with mode=0)
                        //   2. But SL stops generating re-entrant Present calls
                        //      (game menu, pause, loading screen)
                        //
                        // In this state, BOTH rendering paths are blocked:
                        //   - Pre-SL: suppressed by skipOverlayDraw (PostSL confirmed)
                        //   - PostSL: never fires (no re-entrant Present from SL)
                        //
                        // FIX: Count consecutive Present calls without PostSL firing.
                        // PostSLOverlayRender resets g_PostSLStallCounter to 0 on
                        // each successful render.  After kPostSLStallThreshold frames
                        // without a reset, allow pre-SL as fallback.
                        //
                        // WARMUP GUARD: The stall fallback is ONLY safe when SL's FG
                        // pipeline is genuinely idle (suspension).  During FG warmup
                        // (just after OFF→ON or re-confirmation), SL's pipeline is
                        // actively processing, and pre-SL ECLs on origGame cause
                        // DEVICE_HUNG.  g_PostSLStableFrameCount tracks consecutive
                        // PostSL frames since the last FG transition.  Fallback is
                        // only enabled after kPostSLWarmupThreshold frames of stable
                        // PostSL rendering, proving the FG pipeline is fully operational.
                        //
                        // TESTED: GTA V Enhanced (menu pauses FG), Talos Reawakened
                        // (continuous FG — stall never triggers during normal play).
                        constexpr int kPostSLStallThreshold = 5;
                        constexpr int kPostSLWarmupThreshold = 30;  // ~0.5s at 60fps
                        int stableFrames = g_PostSLStableFrameCount.load(std::memory_order_acquire);
                        int stallCount = g_PostSLStallCounter.fetch_add(1, std::memory_order_acq_rel) + 1;

                        if (stableFrames < kPostSLWarmupThreshold) {
                            // FG pipeline still warming up — don't fall back to pre-SL.
                            // Just skip rendering until PostSL stabilizes.
                            skipOverlayDraw = true;
                            static int s_warmupSuppressLog = 0;
                            if (s_warmupSuppressLog++ < 5 || (s_warmupSuppressLog % 200) == 0) {
                                HookLogImportant(
                                    "DX12: PostSL warmup — suppressing stall fallback "
                                    "(stableFrames=%d stallCount=%d threshold=%d) #%d",
                                    stableFrames, stallCount, kPostSLWarmupThreshold, s_warmupSuppressLog);
                            }
                        } else if (stallCount <= kPostSLStallThreshold) {
                            skipOverlayDraw = true;  // PostSL recently active — suppress pre-SL
                        } else {
                            // PostSL has stalled — SL FG is nominally on but not generating
                            // frames.  Allow pre-SL rendering as fallback.
                            static int s_stallFallbackLog = 0;
                            if (s_stallFallbackLog < 10 || (s_stallFallbackLog % 200) == 0) {
                                HookLogImportant(
                                    "DX12: PostSL stalled (%d frames, stableFrames=%d) — falling back to pre-SL "
                                    "rendering #%d",
                                    stallCount, stableFrames, s_stallFallbackLog);
                            }
                            s_stallFallbackLog++;
                            // Don't skip pre-SL draw — it will render the overlay
                        }
                    }
                }
            } else {
                // SL FG not active (FSR FG, no FG, etc.): disable post-SL callback, render pre-SL.
                if (DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) != nullptr) {
                    SetPostSLCallbackInstalled(false, "DX12: pre-SL fallback");
                    g_PostSLOverlayActive.store(false, std::memory_order_release);
                    // Reset PostSL confirmed flag so pre-SL rendering resumes immediately
                    g_PostSLConfirmedRendering.store(false, std::memory_order_release);
                    g_PostSLStallCounter.store(0, std::memory_order_release);
                    g_PostSLStableFrameCount.store(0, std::memory_order_release);
                    HookLogImportant("DX12: Disabled post-SL callback — rendering pre-SL in ProcessFrame (fgType=%s)",
                                     g_FGCompat.GetFGTypeName(g_FGCompat.GetActiveFGType()));
                }
            }
        }

        // Periodic routing state diagnostic (every 300 frames)
        {
            static uint64_t s_routingFrameCount = 0;
            ++s_routingFrameCount;
            if ((s_routingFrameCount % 300) == 0) {
                HookLogImportant(
                    "DX12: Routing state: frame=%llu fgActive=%d slFGActive=%d slSignal=%d "
                    "cooldown=%d sceneCool=%d postSLCallback=%d postSLActive=%d skip=%d stallCount=%d stableFrames=%d "
                    "runtime=%s",
                    s_routingFrameCount, currentFGActive ? 1 : 0, slFGActive ? 1 : 0, currentSLFGRunning ? 1 : 0,
                    g_FGTransitionCooldown, g_SceneTransitionCooldown.load(std::memory_order_relaxed),
                    DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) != nullptr ? 1 : 0,
                    g_PostSLOverlayActive.load(std::memory_order_relaxed) ? 1 : 0, skipOverlayDraw ? 1 : 0,
                    g_PostSLStallCounter.load(std::memory_order_relaxed),
                    g_PostSLStableFrameCount.load(std::memory_order_relaxed),
                    ce::fg_runtime::GetRuntimeModeName(currentRuntimeMode));
            }
        }

        // Scene transition cooldown: detect large frametime gaps (loading screens,
        // scene changes) and skip overlay rendering briefly.  This runs BEFORE
        // the skipOverlayDraw check so it works for both pre-SL (normal) and
        // post-SL (SL FG) overlay paths.
        {
            static LARGE_INTEGER s_lastProcessFrameTime = {};
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);

            if (s_lastProcessFrameTime.QuadPart != 0) {
                LARGE_INTEGER freq;
                QueryPerformanceFrequency(&freq);
                double deltaMs =
                    (double)(now.QuadPart - s_lastProcessFrameTime.QuadPart) * 1000.0 / (double)freq.QuadPart;

                if (deltaMs > 1000.0 && currentFGActive) {
                    int cooldown = 30;
                    g_SceneTransitionCooldown.store(cooldown, std::memory_order_release);
                    HookLogImportant(
                        "DX12: Scene transition detected (gap=%.0fms) during FG — overlay cooldown %d frames", deltaMs,
                        cooldown);
                }
            }
            s_lastProcessFrameTime = now;
        }

        if (captureBeforeOverlay) {
            int64_t captureStartUs = PerfLogger::GetQpcUs();
            PublishDX12CapturedFrame(pSwapChain, captureShm, gameQueue, hasCurrentBackBufferIdx, currentBackBufferIdx);
            perfMetrics.captureUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - captureStartUs);
        }

        if (!skipOverlayDraw) {
            // PRE-SL RENDERING GATE — controls when pre-SL overlay is suppressed during SL FG.
            //
            // Two suppression points, both with stall fallback:
            //
            // 1. HERE (render site): Suppresses when SL FG is on and PostSL hasn't
            //    confirmed yet.  Gives PostSL ~5 frames to fire before falling back.
            //
            // 2. ABOVE (routing logic, line ~6305): Suppresses via skipOverlayDraw when
            //    PostSL IS confirmed but the stall counter exceeds threshold.
            //
            // Both use kPostSLStallThreshold/kPreSLFallbackThreshold (same value) to
            // detect "FG suspension" (SL nominally on, but not generating frames).
            //
            // PRE-SL RENDERING DURING FG SUSPENSION:
            // When pre-SL fallback activates, the overlay renders BEFORE SL's Present
            // trampoline.  This is safe because SL's FG pipeline is idle (not generating
            // frames).  The game's Present call goes through:
            //   ProcessFrame (overlay renders) → oPresent → SL passes through → real Present
            // Resource state is correct: game transitioned BB to PRESENT before Present,
            // we do PRESENT→RT→PRESENT round-trip, then SL sees PRESENT state.
            //
            // REGRESSION RISK: In Talos Reawakened, SL FG runs continuously (no menu
            // suspension).  The stall counter should never exceed 5 during normal play
            // because PostSL fires multiple times per game Present (real + interpolated).
            // If this regresses, increase kPreSLFallbackThreshold.
            {
                bool slFGNow = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
                bool postSLActive = g_PostSLOverlayActive.load(std::memory_order_acquire);
                bool postSLConfirmed = g_PostSLConfirmedRendering.load(std::memory_order_relaxed);
                auto postSLCallback = DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed);
                int stallCount = g_PostSLStallCounter.load(std::memory_order_relaxed);
                constexpr int kPreSLFallbackThreshold = 5;

                if (slFGNow && !postSLConfirmed) {
                    // SL FG active, PostSL never confirmed yet (FG STARTUP, not suspension).
                    //
                    // CRITICAL: Do NOT fall back to pre-SL rendering here!
                    // During FG startup:
                    //   - SL creates a new swapchain with its own queue
                    //   - Backbuffers belong to SL's swapchain queue
                    //   - Pre-SL renders on origGame queue → cross-queue access → DEVICE_HUNG
                    //   - SL's FG pipeline hasn't started generating re-entrant Presents yet
                    //
                    // Pre-SL fallback is ONLY safe during FG SUSPENSION (PostSL was confirmed
                    // working but stopped firing — the game's backbuffer state is still valid
                    // on the game's queue because SL's FG pipeline is idle).
                    //
                    // During startup, we simply wait for PostSL to confirm. The overlay will
                    // be invisible for a few frames during FG initialization — acceptable.
                    static int s_preSLSuppressLog = 0;
                    if (s_preSLSuppressLog++ < 10 || (s_preSLSuppressLog % 300) == 0) {
                        HookLogImportant(
                            "DX12: Suppressing pre-SL draw during SL FG startup — waiting for PostSL "
                            "(postSLCallback=%d postSLActive=%d hadFSR=%d stallCount=%d) #%d",
                            postSLCallback ? 1 : 0, postSLActive ? 1 : 0, g_HadFSRFGPhase ? 1 : 0, stallCount,
                            s_preSLSuppressLog);
                    }
                    goto skip_overlay_draw;
                }
            }

            // Check scene transition cooldown for pre-SL path
            {
                int cd = g_SceneTransitionCooldown.load(std::memory_order_acquire);
                if (cd > 0) {
                    g_SceneTransitionCooldown.store(cd - 1, std::memory_order_release);
                    if (cd == 1)
                        HookLogImportant("DX12: Scene transition cooldown complete — resuming overlay");
                    goto skip_overlay_draw;
                }
            }

            // Periodic health log for debugging stability
            static std::atomic<uint64_t> s_overlayFrameCount{0};
            uint64_t frameNum = s_overlayFrameCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (frameNum == 1 || frameNum == 10 || frameNum == 50 || frameNum == 100 || (frameNum % 500) == 0) {
                ID3D12Device* dev = g_Device.load();
                HRESULT devRemovedHr = dev ? dev->GetDeviceRemovedReason() : E_FAIL;
                HookLogImportant(
                    "DX12: Overlay frame #%llu (deviceRemoved=0x%08X, fgActive=%d, "
                    "queue=%p, allocIdx=%d, slFGRunning=%d)",
                    (unsigned long long)frameNum, (unsigned)devRemovedHr, currentFGActive ? 1 : 0, gameQueue,
                    g_State.allocIndex, DXGIShared::g_StreamlineFGRunning.load(std::memory_order_relaxed) ? 1 : 0);
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
                        // Log Reset results during FG for diagnostics
                        if (g_FGCompat.IsFGActive() || slFGActive) {
                            static std::atomic<int> s_fgResetLogs{0};
                            int fgResetLog = s_fgResetLogs.fetch_add(1, std::memory_order_relaxed);
                            if (fgResetLog < 5) {
                                HookLogImportant(
                                    "DX12: FG overlay alloc/list Reset (allocHr=0x%08X listHr=0x%08X idx=%d)",
                                    (unsigned)allocResetHr, (unsigned)listResetHr, idx);
                            }
                        }
                        if (SUCCEEDED(listResetHr)) {
                            const bool shouldPrimeStartupOverlayResources =
                                ce::overlay_compat::ShouldPreemptivelyDelayDX12OverlayInitForProcess(g_ProcessName) &&
                                !IsActualFrameGenerationActive() && s_startupOverlayResourcePrimeMs == 0 &&
                                g_OverlayAdapter.HasPendingDX12Resources();
                            if (shouldPrimeStartupOverlayResources) {
                                // Check device before priming — after FG teardown the
                                // device may already be removed (async GPU fault).
                                {
                                    auto* primeDev = g_Device.load(std::memory_order_acquire);
                                    HRESULT primeDevHr = primeDev ? primeDev->GetDeviceRemovedReason() : E_FAIL;
                                    if (FAILED(primeDevHr)) {
                                        HookLogImportant("DX12: SKIPPING resource priming — device removed 0x%08X",
                                                         (unsigned)primeDevHr);
                                        g_DeviceRemoved.store(true, std::memory_order_release);
                                        goto overlay_done;
                                    }
                                }
                                HookLogImportant("DX12: Priming DX12 overlay resources before first GTA overlay draw");
                                if (!g_OverlayAdapter.PrimeDX12Resources(list)) {
                                    HookLogImportant(
                                        "DX12: DX12 overlay resource priming failed; deferring first overlay draw");
                                    goto overlay_done;
                                }

                                HRESULT closeHr = list->Close();
                                if (FAILED(closeHr)) {
                                    HookLog("DX12: Priming command list close failed hr=0x%08X, forcing reinit",
                                            closeHr);
                                    g_State.syncInit = false;
                                    goto overlay_done;
                                }

                                if (!SubmitOverlayCommandList(gameQueue, list, idx, "startup resource priming",
                                                              false)) {
                                    HookLogImportant(
                                        "DX12: Startup resource priming submission failed; deferring first overlay "
                                        "draw");
                                    goto overlay_done;
                                }

                                // Check device after priming submit — catch async GPU fault immediately
                                {
                                    auto* postPrimeDev = g_Device.load(std::memory_order_acquire);
                                    HRESULT postPrimeDevHr =
                                        postPrimeDev ? postPrimeDev->GetDeviceRemovedReason() : E_FAIL;
                                    if (FAILED(postPrimeDevHr)) {
                                        HookLogImportant("DX12: Resource priming CAUSED device removal 0x%08X!",
                                                         (unsigned)postPrimeDevHr);
                                        g_DeviceRemoved.store(true, std::memory_order_release);
                                        goto overlay_done;
                                    }
                                }

                                s_startupOverlayResourcePrimeMs = GetTickCount64();
                                HookLogImportant(
                                    "DX12: DX12 overlay resource priming submitted, delaying first overlay draw for "
                                    "%llums",
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
                                    sc3->Release();           // drop QI ref — weak cache is safe
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
                                        HookLogImportant(
                                            "DX12: Preparing overlay draw for backbuffer idx=%u resource=%p via %s "
                                            "queue (queue=%p)",
                                            bufferIdx, bb,
                                            g_State.overlayQueue
                                                ? "dedicated overlay"
                                                : (gameQueue == g_SwapchainQueue ? "swapchain" : "game"),
                                            gameQueue);
                                    }

                                    // SL FG diagnostic: log every overlay draw during FG
                                    if (slFGActive) {
                                        static std::atomic<int> s_slFGDrawCount{0};
                                        int fgDraw = s_slFGDrawCount.fetch_add(1, std::memory_order_relaxed) + 1;
                                        if (fgDraw <= 20 || (fgDraw % 10) == 0) {
                                            auto* diagDev = g_Device.load(std::memory_order_acquire);
                                            HRESULT devHr = diagDev ? diagDev->GetDeviceRemovedReason() : E_FAIL;
                                            bool dedicated = g_State.overlayQueue && ShouldUseDedicatedOverlayQueue();
                                            HookLogImportant(
                                                "DX12: SL-FG overlay ENTER #%d (bufIdx=%u bb=%p queue=%p dedQ=%d "
                                                "tid=0x%04X devRemoved=0x%08X)",
                                                fgDraw, bufferIdx, bb, gameQueue, dedicated ? 1 : 0,
                                                GetCurrentThreadId(), (unsigned)devHr);
                                        }
                                    }

                                    // GPU drain: flush all in-flight GPU work before first
                                    // overlay render after FSR→DLSS transition.  This ensures
                                    // SL's FG pipeline has fully completed before we touch
                                    // the backbuffer, preventing GPU-side deadlock/TDR.
                                    if (g_NeedGPUDrainBeforeRender && gameQueue) {
                                        auto* drainDev = g_Device.load(std::memory_order_acquire);
                                        if (drainDev) {
                                            if (!g_DrainFence) {
                                                HRESULT hr = drainDev->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                                                                   IID_PPV_ARGS(&g_DrainFence));
                                                if (SUCCEEDED(hr)) {
                                                    g_DrainEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
                                                    g_DrainFenceValue = 0;
                                                    HookLogImportant("DX12: GPU drain fence created");
                                                } else {
                                                    HookLogImportant("DX12: GPU drain fence creation failed hr=0x%08X",
                                                                     (unsigned)hr);
                                                }
                                            }
                                            if (g_DrainFence && g_DrainEvent) {
                                                UINT64 drainVal = ++g_DrainFenceValue;
                                                HRESULT sigHr = gameQueue->Signal(g_DrainFence, drainVal);
                                                if (SUCCEEDED(sigHr)) {
                                                    if (g_DrainFence->GetCompletedValue() < drainVal) {
                                                        g_DrainFence->SetEventOnCompletion(drainVal, g_DrainEvent);
                                                        DWORD waitResult = WaitForSingleObject(g_DrainEvent, 5000);
                                                        HookLogImportant(
                                                            "DX12: GPU drain completed (wait=%s val=%llu queue=%p)",
                                                            waitResult == WAIT_OBJECT_0
                                                                ? "OK"
                                                                : (waitResult == WAIT_TIMEOUT ? "TIMEOUT" : "FAIL"),
                                                            drainVal, gameQueue);
                                                    } else {
                                                        HookLogImportant(
                                                            "DX12: GPU drain — already complete (val=%llu)", drainVal);
                                                    }
                                                } else {
                                                    HookLogImportant("DX12: GPU drain Signal failed hr=0x%08X",
                                                                     (unsigned)sigHr);
                                                }
                                            }
                                        }
                                        g_NeedGPUDrainBeforeRender = false;
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
                                        // Pre-DescFree device health check
                                        if (dev) {
                                            HRESULT preDescFreeDevHr = dev->GetDeviceRemovedReason();
                                            if (FAILED(preDescFreeDevHr)) {
                                                HookLogImportant(
                                                    "DX12: DEVICE ALREADY REMOVED before DescFree init "
                                                    "(devRemoved=0x%08X tid=0x%04X)",
                                                    (unsigned)preDescFreeDevHr, GetCurrentThreadId());
                                            }
                                        }
                                        if (dev && !g_DescFreeBackend) {
                                            auto* backend = new DX12DescFreeBackend();
                                            if (backend->InitDevice(dev, g_State.format)) {
                                                g_DescFreeBackend = backend;
                                                g_D3D11On12Adapter.InitCustom(g_DescFreeBackend,
                                                                              OverlayBackendType::DX12);
                                                HookLogImportant("DX12: Descriptor-free overlay backend ready");
                                            } else {
                                                delete backend;
                                                HookLogImportant(
                                                    "DX12: Descriptor-free backend init failed, "
                                                    "falling back to standard DX12");
                                            }
                                        }
                                        if (g_DescFreeBackend && g_D3D11On12Adapter.IsInitialized()) {
                                            bool isRealFrame = g_FGCompat.IsCurrentFrameReal();

                                            // After FSR→DLSS: ANY direct backbuffer access (barriers,
                                            // RT, ClearRTV) causes DEVICE_HUNG because SL's FG pipeline
                                            // has in-flight work on the backbuffer from another queue.
                                            // Instead, render to offscreen RT, then CopyTextureRegion
                                            // to the backbuffer.  CopyTextureRegion uses COPY_DEST state
                                            // which IS implicitly promotable from COMMON/PRESENT — no
                                            // explicit barrier needed on the backbuffer.
                                            // Two-copy offscreen compositing was designed for PostSL
                                            // rendering where the backbuffer's state is unknown.
                                            // For pre-SL rendering on the game thread, the backbuffer
                                            // is in PRESENT state (game transitioned it before Present).
                                            // Use normal direct rendering with PRESENT→RT→PRESENT barriers.
                                            //
                                            // EXCEPTION: After FSR→DLSS→OFF, the backbuffer state is
                                            // indeterminate (FG pipeline may have left it in any state).
                                            // Use offscreen compositing to avoid explicit barriers on
                                            // the backbuffer entirely.  Cleared on clean swapchain
                                            // transition.
                                            bool useOffscreenCopy = g_NeedOffscreenOverlayAfterPostFSRNonFG;

                                            if (useOffscreenCopy && bb) {
                                                static int s_postFSROffscreenLog = 0;
                                                if (s_postFSROffscreenLog++ < 10) {
                                                    HookLogImportant(
                                                        "DX12: Using offscreen compositing for post-FSR non-FG overlay "
                                                        "(bb=%p queue=%p bufIdx=%u #%d)",
                                                        bb, gameQueue, bufferIdx, s_postFSROffscreenLog);
                                                }
                                                // Two-copy compositing: avoids ALL explicit barriers on backbuffer.
                                                // 1. Copy bb→offscreen (bb implicitly promotes COMMON→COPY_SOURCE)
                                                // 2. Barrier offscreen COPY_DEST→RT
                                                // 3. Render overlay on top of game frame in offscreen
                                                // 4. Barrier offscreen RT→COPY_SOURCE
                                                // 5. Copy offscreen→bb (bb implicitly promotes COMMON→COPY_DEST)
                                                // After ECL, both resources decay back to COMMON.
                                                if (EnsureOffscreenRT(dev, g_State.cachedWidth, g_State.cachedHeight,
                                                                      g_State.format)) {
                                                    // Step 1: Copy backbuffer → offscreen RT
                                                    // bb: implicit promotion COMMON→COPY_SOURCE (no explicit barrier!)
                                                    // offscreen: explicit COMMON→COPY_DEST
                                                    {
                                                        D3D12_RESOURCE_BARRIER b = {};
                                                        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                                        b.Transition.pResource = g_State.offscreenRT;
                                                        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                                                        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                                                        b.Transition.Subresource =
                                                            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                                                        list->ResourceBarrier(1, &b);
                                                    }
                                                    {
                                                        D3D12_TEXTURE_COPY_LOCATION src = {};
                                                        src.pResource = bb;
                                                        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                                                        src.SubresourceIndex = 0;
                                                        D3D12_TEXTURE_COPY_LOCATION dst = {};
                                                        dst.pResource = g_State.offscreenRT;
                                                        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                                                        dst.SubresourceIndex = 0;
                                                        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                                                    }

                                                    // Step 2: Barrier offscreen COPY_DEST → RENDER_TARGET
                                                    {
                                                        D3D12_RESOURCE_BARRIER b = {};
                                                        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                                        b.Transition.pResource = g_State.offscreenRT;
                                                        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                                                        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                                                        b.Transition.Subresource =
                                                            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                                                        list->ResourceBarrier(1, &b);
                                                    }

                                                    // Step 3: Render overlay to offscreen RT (on top of game frame)
                                                    D3D12_CPU_DESCRIPTOR_HANDLE offRtv =
                                                        g_State.offscreenRtvHeap->GetCPUDescriptorHandleForHeapStart();
                                                    s_descFreeCmdList = list;
                                                    s_descFreeRtv = offRtv;

                                                    g_D3D11On12Adapter.SetIPCClient(g_IPC);
                                                    const auto metricsBinding =
                                                        ce::dx12_overlay_policy::DecideOverlayMetricsBinding(
                                                            isRealFrame);
                                                    if (metricsBinding.bindMetrics) {
                                                        g_D3D11On12Adapter.SetMetrics(
                                                            DXGIShared::GetPerformanceMetrics());
                                                    }
                                                    if (metricsBinding.refreshFrameMetadata) {
                                                        const char* api = "DX12";
                                                        g_D3D11On12Adapter.SetGraphicsAPI(api);
                                                    }
                                                    g_D3D11On12Adapter.RenderOverlay(g_State.cachedWidth,
                                                                                     g_State.cachedHeight);
                                                    s_descFreeCmdList = nullptr;

                                                    // Step 4: Barrier offscreen RT → COPY_SOURCE
                                                    {
                                                        D3D12_RESOURCE_BARRIER b = {};
                                                        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                                        b.Transition.pResource = g_State.offscreenRT;
                                                        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                                                        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                                                        b.Transition.Subresource =
                                                            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                                                        list->ResourceBarrier(1, &b);
                                                    }

                                                    // Step 5: Copy offscreen → backbuffer
                                                    // bb: implicit promotion COMMON→COPY_DEST (no explicit barrier!)
                                                    {
                                                        D3D12_TEXTURE_COPY_LOCATION src = {};
                                                        src.pResource = g_State.offscreenRT;
                                                        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                                                        src.SubresourceIndex = 0;
                                                        D3D12_TEXTURE_COPY_LOCATION dst = {};
                                                        dst.pResource = bb;
                                                        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                                                        dst.SubresourceIndex = 0;
                                                        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                                                    }

                                                    // After ECL: bb decays COPY_DEST→COMMON, offscreen decays
                                                    // COPY_SOURCE→COMMON

                                                    static int s_offscreenLog = 0;
                                                    if (s_offscreenLog++ < 5) {
                                                        HookLogImportant(
                                                            "DX12: Post-FSR DLSS overlay via 2-copy compositing (bb=%p "
                                                            "offRT=%p queue=%p)",
                                                            bb, g_State.offscreenRT, gameQueue);
                                                    }
                                                    usedDescFree = true;
                                                } else {
                                                    HookLogImportant(
                                                        "DX12: Failed to create offscreen RT for post-FSR DLSS "
                                                        "overlay");
                                                }
                                            } else {
                                                // Normal path: render directly to backbuffer
                                                D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
                                                    g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
                                                UINT rtvSize = dev->GetDescriptorHandleIncrementSize(
                                                    D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                                                rtvHandle.ptr += (SIZE_T)bufferIdx * rtvSize;

                                                // ALWAYS add barriers in DescFree path.
                                                // At startup, extOverlay may be false
                                                // (socialclub.dll not loaded yet); after
                                                // FG teardown, Social Club may not render
                                                // in the menu screen.  The backbuffer is
                                                // in PRESENT state in both cases, so the
                                                // PRESENT→RT transition is correct.
                                                bool fgBarriersNeeded = true;
                                                if (fgBarriersNeeded && bb) {
                                                    D3D12_RESOURCE_BARRIER barrier = {};
                                                    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                                    barrier.Transition.pResource = bb;
                                                    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                                                    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                                                    barrier.Transition.Subresource =
                                                        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                                                    list->ResourceBarrier(1, &barrier);
                                                }

                                                s_descFreeCmdList = list;
                                                s_descFreeRtv = rtvHandle;

                                                g_D3D11On12Adapter.SetIPCClient(g_IPC);
                                                const auto metricsBinding =
                                                    ce::dx12_overlay_policy::DecideOverlayMetricsBinding(isRealFrame);
                                                if (metricsBinding.bindMetrics) {
                                                    g_D3D11On12Adapter.SetMetrics(DXGIShared::GetPerformanceMetrics());
                                                }
                                                if (metricsBinding.refreshFrameMetadata) {
                                                    static const bool s_isVKD3D = []() {
                                                        return GetModuleHandleA("d3d12core.dll") &&
                                                               (GetModuleHandleA("libvkd3d-1.dll") ||
                                                                GetModuleHandleA("vkd3d.dll"));
                                                    }();
                                                    const char* api = s_isVKD3D ? "DX12 (VKD3D)" : "DX12";
                                                    g_D3D11On12Adapter.SetGraphicsAPI(api);
                                                }

                                                g_D3D11On12Adapter.RenderOverlay(g_State.cachedWidth,
                                                                                 g_State.cachedHeight);

                                                // Transition back to PRESENT after overlay draw
                                                if (fgBarriersNeeded && bb) {
                                                    D3D12_RESOURCE_BARRIER barrier = {};
                                                    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                                    barrier.Transition.pResource = bb;
                                                    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                                                    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                                                    barrier.Transition.Subresource =
                                                        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                                                    list->ResourceBarrier(1, &barrier);
                                                }

                                                s_descFreeCmdList = nullptr;
                                                usedDescFree = true;
                                            }  // end normal path
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
                                        UINT rtvSize = g_Device.load()->GetDescriptorHandleIncrementSize(
                                            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
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
                                    // Log Close result during FG
                                    if (g_FGCompat.IsFGActive() || slFGActive) {
                                        static std::atomic<int> s_fgCloseLogs{0};
                                        if (s_fgCloseLogs.fetch_add(1, std::memory_order_relaxed) < 5) {
                                            HookLogImportant("DX12: FG overlay list->Close hr=0x%08X",
                                                             (unsigned)closeHr);
                                        }
                                    }
                                    // Always log Close result for first N reinit frames
                                    {
                                        static int s_reinitCloseLogCount = 0;
                                        if (g_ResetReinitSubmitCounter.load(std::memory_order_relaxed))
                                            s_reinitCloseLogCount = 0;
                                        if (s_reinitCloseLogCount < 5) {
                                            s_reinitCloseLogCount++;
                                            auto* closeDev = g_Device.load(std::memory_order_acquire);
                                            HRESULT closeDevHr = closeDev ? closeDev->GetDeviceRemovedReason() : E_FAIL;
                                            HookLogImportant(
                                                "DX12: Reinit Close #%d hr=0x%08X devRemoved=0x%08X descFree=%d",
                                                s_reinitCloseLogCount, (unsigned)closeHr, (unsigned)closeDevHr,
                                                usedDescFree ? 1 : 0);
                                        }
                                    }
                                    if (FAILED(closeHr)) {
                                        HookLog("DX12: list->Close failed hr=0x%08X, forcing reinit", closeHr);
                                        g_State.syncInit = false;
                                    } else {
                                        // Choose submit queue: dedicated overlay queue when
                                        // available (SL FG active), otherwise game queue.
                                        bool useDedicated = g_State.overlayQueue && ShouldUseDedicatedOverlayQueue();
                                        ID3D12CommandQueue* eclQueue = useDedicated ? g_State.overlayQueue : gameQueue;

                                        // One-time diagnostic: check if SL also hooked
                                        // the overlay queue's ECL vtable entry.
                                        if (useDedicated && slFGActive) {
                                            static bool s_eclVtableChecked = false;
                                            if (!s_eclVtableChecked) {
                                                s_eclVtableChecked = true;
                                                void** vtable = *(void***)eclQueue;
                                                void* eclAddr = vtable[10];
                                                HMODULE eclMod = nullptr;
                                                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                                   (LPCSTR)eclAddr, &eclMod);
                                                char modName[MAX_PATH] = {};
                                                if (eclMod)
                                                    GetModuleFileNameA(eclMod, modName, MAX_PATH);
                                                HookLogImportant(
                                                    "DX12: Overlay queue ECL vtable[10]=%p module='%s' (SL hooked=%d)",
                                                    eclAddr, modName, (strstr(modName, "sl.") != nullptr) ? 1 : 0);
                                                // Also log game queue for comparison
                                                void** gvtable = *(void***)gameQueue;
                                                void* geclAddr = gvtable[10];
                                                HMODULE geclMod = nullptr;
                                                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                                   (LPCSTR)geclAddr, &geclMod);
                                                char gModName[MAX_PATH] = {};
                                                if (geclMod)
                                                    GetModuleFileNameA(geclMod, gModName, MAX_PATH);
                                                HookLogImportant("DX12: Game queue ECL vtable[10]=%p module='%s'",
                                                                 geclAddr, gModName);
                                            }
                                        }

                                        // Cross-queue sync: drain game queue before submitting
                                        // on dedicated queue so game rendering completes first.
                                        if (useDedicated && gameQueue) {
                                            WaitForGameQueueBeforeDedicatedOverlaySubmission(gameQueue, "overlay ECL");
                                        }

                                        ExecuteCommandListsPtr origECL = GetOriginalExecuteCommandLists(eclQueue);
                                        ID3D12CommandList* lists[] = {list};

                                        // Pre-ECL device health check — distinguish
                                        // "device already dead" from "our ECL killed it"
                                        {
                                            auto* preEclDev = g_Device.load(std::memory_order_acquire);
                                            if (preEclDev) {
                                                HRESULT preEclDevHr = preEclDev->GetDeviceRemovedReason();
                                                if (FAILED(preEclDevHr)) {
                                                    HookLogImportant(
                                                        "DX12: DEVICE ALREADY REMOVED before overlay ECL "
                                                        "(devRemoved=0x%08X queue=%p realECL=%p origECL=%p tid=0x%04X) "
                                                        "— SKIPPING",
                                                        (unsigned)preEclDevHr, eclQueue, (void*)g_RealD3D12ECL.load(),
                                                        (void*)origECL, GetCurrentThreadId());
                                                    g_DeviceRemoved.store(true, std::memory_order_release);
                                                    goto overlay_done;
                                                }
                                            }
                                        }

                                        // During ANY FG on the game queue, call the real
                                        // D3D12 ECL directly (bypasses our vtable detour
                                        // AND any FG runtime ECL hooks).
                                        // SL FG: avoids incrementing ECL count + SL detour.
                                        // FSR FG: avoids FSR's ECL hook which counts our
                                        //   overlay submission as a game command list,
                                        //   confusing FSR's frame interpolation tracking.
                                        //
                                        // ALWAYS use realECL when available (not just during FG).
                                        // Without this, our overlay ECL goes through the vtable
                                        // → our ECL detour → counted by FG heuristic → false
                                        // FSR FG detection after DLSS FG turns off (2:1 ratio
                                        // from game ECL + overlay ECL looks like frame gen).
                                        //
                                        // EXCEPTION: After FSR→DLSS, eclQueue is SL's wrapper
                                        // (g_CommandQueue). Must use vtable call (origECL) so
                                        // SL's ECL interception handles resource state for
                                        // the FSR-created backbuffers.
                                        ExecuteCommandListsPtr realECL = g_RealD3D12ECL.load(std::memory_order_acquire);
                                        bool usedRealECL = false;
                                        bool isSLWrapperECL =
                                            g_HadFSRFGPhase && slFGActive &&
                                            eclQueue == g_CommandQueue.load(std::memory_order_acquire) &&
                                            eclQueue != g_OriginalGameQueue;

                                        // Log ECL path decision for first N frames per reinit
                                        {
                                            static int s_eclPathLogCount = 0;
                                            if (g_ResetReinitSubmitCounter.load(std::memory_order_relaxed))
                                                s_eclPathLogCount = 0;
                                            if (s_eclPathLogCount < 3) {
                                                s_eclPathLogCount++;
                                                const char* path = (!useDedicated && realECL && !isSLWrapperECL)
                                                                       ? "realECL"
                                                                   : origECL ? "origECL"
                                                                             : "vtable";
                                                HookLogImportant(
                                                    "DX12: ECL path=%s (eclQ=%p realECL=%p origECL=%p "
                                                    "dedicated=%d slWrapper=%d scQ=%p origGame=%p)",
                                                    path, eclQueue, (void*)realECL, (void*)origECL,
                                                    useDedicated ? 1 : 0, isSLWrapperECL ? 1 : 0, g_SwapchainQueue,
                                                    g_OriginalGameQueue);
                                            }
                                        }

                                        if (!useDedicated && realECL && !isSLWrapperECL) {
                                            realECL(eclQueue, 1, lists);
                                            usedRealECL = true;
                                        } else if (origECL) {
                                            origECL(eclQueue, 1, lists);
                                        } else {
                                            eclQueue->ExecuteCommandLists(1, lists);
                                        }

                                        // SL/FSR FG diagnostic: log after ECL submission
                                        if (slFGActive || g_FGCompat.IsFGActive()) {
                                            static std::atomic<int> s_fgSubmitCount{0};
                                            int fgSubmit = s_fgSubmitCount.fetch_add(1, std::memory_order_relaxed) + 1;
                                            if (fgSubmit <= 20 || (fgSubmit % 100) == 0) {
                                                auto* diagDev2 = g_Device.load(std::memory_order_acquire);
                                                HRESULT devHr2 = diagDev2 ? diagDev2->GetDeviceRemovedReason() : E_FAIL;
                                                HookLogImportant(
                                                    "DX12: FG overlay SUBMIT #%d (queue=%p descFree=%d realECL=%d "
                                                    "slFG=%d fsrFG=%d gameQ=%d devRemoved=0x%08X tid=0x%04X)",
                                                    fgSubmit, eclQueue, usedDescFree ? 1 : 0, usedRealECL ? 1 : 0,
                                                    slFGActive ? 1 : 0, g_FGCompat.IsFGActive() ? 1 : 0,
                                                    !useDedicated ? 1 : 0, (unsigned)devHr2, GetCurrentThreadId());
                                            }
                                        }

                                        // Unconditional post-submit diagnostic: log first 50
                                        // submits after each overlay reinit.  Catches
                                        // DEVICE_REMOVED even when FG is inactive.
                                        {
                                            static int s_reinitSubmitCount = 0;
                                            if (g_ResetReinitSubmitCounter.exchange(false, std::memory_order_acquire))
                                                s_reinitSubmitCount = 0;
                                            if (s_reinitSubmitCount < 50) {
                                                s_reinitSubmitCount++;
                                                auto* diagDevR = g_Device.load(std::memory_order_acquire);
                                                HRESULT devHrR = diagDevR ? diagDevR->GetDeviceRemovedReason() : E_FAIL;
                                                HookLogImportant(
                                                    "DX12: Reinit SUBMIT #%d (queue=%p descFree=%d realECL=%d "
                                                    "extOverlay=%d bb=%p bufIdx=%d devRemoved=0x%08X tid=0x%04X)",
                                                    s_reinitSubmitCount, eclQueue, usedDescFree ? 1 : 0,
                                                    usedRealECL ? 1 : 0, startupOverlayPresent ? 1 : 0, bb, bufferIdx,
                                                    (unsigned)devHrR, GetCurrentThreadId());
                                                if (FAILED(devHrR)) {
                                                    HookLogImportant("DX12: DEVICE REMOVED after reinit submit #%d!",
                                                                     s_reinitSubmitCount);
                                                    g_DeviceRemoved.store(true, std::memory_order_release);
                                                }
                                            }
                                        }

                                        // Post-FG-transition diagnostic: log first 20 frames after any FG change.
                                        // Catches DEVICE_REMOVED right after overlay resumes following FG switches.
                                        {
                                            static int s_postTransitionFrames = 0;
                                            static int s_lastTransitionCooldown = -1;
                                            int curCooldown = g_FGTransitionCooldown;
                                            if (curCooldown > 0 && s_lastTransitionCooldown <= 0)
                                                s_postTransitionFrames = 0;  // new transition started
                                            if (curCooldown <= 0 && s_lastTransitionCooldown > 0)
                                                s_postTransitionFrames = 0;  // transition just ended
                                            s_lastTransitionCooldown = curCooldown;
                                            if (s_postTransitionFrames < 50) {
                                                s_postTransitionFrames++;
                                                auto* diagDev3 = g_Device.load(std::memory_order_acquire);
                                                HRESULT devHr3 = diagDev3 ? diagDev3->GetDeviceRemovedReason() : E_FAIL;
                                                HookLogImportant(
                                                    "DX12: Post-transition SUBMIT #%d (queue=%p origQ=%p cmdQ=%p "
                                                    "fgActive=%d slFG=%d descFree=%d realECL=%d devRemoved=0x%08X "
                                                    "bb=%p bufIdx=%d tid=0x%04X)",
                                                    s_postTransitionFrames, eclQueue, g_OriginalGameQueue,
                                                    (void*)g_CommandQueue.load(), g_FGCompat.IsFGActive() ? 1 : 0,
                                                    DXGIShared::g_StreamlineFGRunning.load(std::memory_order_relaxed)
                                                        ? 1
                                                        : 0,
                                                    usedDescFree ? 1 : 0, usedRealECL ? 1 : 0, (unsigned)devHr3, bb,
                                                    bufferIdx, GetCurrentThreadId());
                                            }
                                        }

                                        if (g_State.fence) {
                                            UINT64 next = g_State.currentFenceValue + 1;
                                            bool anyFGForFence = slFGActive || g_FGCompat.IsFGActive();
                                            if (anyFGForFence && !useDedicated) {
                                                // During ANY FG on the game queue, SKIP the
                                                // deferred fence signal entirely.  The Signal
                                                // virtual call goes through the game queue's
                                                // vtable, which SL/FSR might monitor for frame
                                                // synchronization.  Extra Signals between
                                                // Present calls can desync the FG pipeline.
                                                // Allocator reuse is safe: stale fence values
                                                // from pre-FG frames are long-completed.
                                            } else if (useDedicated) {
                                                // Signal immediately on dedicated queue (SL
                                                // doesn't see it).
                                                HRESULT sigHr = eclQueue->Signal(g_State.fence, next);
                                                if (SUCCEEDED(sigHr)) {
                                                    g_State.currentFenceValue = next;
                                                    if (idx >= 0 && idx < (int)g_State.fenceValues.size())
                                                        g_State.fenceValues[idx] = next;
                                                }
                                            } else {
                                                // Game queue: defer fence Signal to next frame
                                                // (avoids NVIDIA driver stall between Signal and
                                                // Present).
                                                g_deferredSignalQueue.store(eclQueue, std::memory_order_release);
                                                g_deferredSignalValue.store(next, std::memory_order_release);
                                                g_deferredSignalAllocIdx.store(idx, std::memory_order_release);
                                            }
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
                                            "DX12: Overlay perf: QI+idx=%.0fus getBuf+record=%.0fus submit=%.0fus "
                                            "total=%.0fus",
                                            qiUs, getBufUs, submitUs, totalUs);
                                    }

                                    if (cmdRecordOk) {
                                        static int s_firstOverlaySubmitLogged = 0;
                                        if (s_firstOverlaySubmitLogged == 0) {
                                            s_firstOverlaySubmitLogged = 1;
                                            HookLogImportant(
                                                "DX12: ProcessFrame - first overlay render command list submitted "
                                                "successfully");
                                        }

                                        // Clear probe state if we were in a probe sequence
                                        if (shouldRunStartupOverlayDrawProbe &&
                                            s_startupOverlayFirstDrawProbeStage ==
                                                StartupOverlayFirstDrawProbeStage::kActualRender) {
                                            HookLogImportant("DX12: Startup overlay probe complete - rendering stably");
                                            s_startupOverlayFirstDrawProbeStage =
                                                StartupOverlayFirstDrawProbeStage::kComplete;
                                            s_startupOverlayFirstDrawProbeMs = 0;
                                            if (!s_startupOverlayCompatSettled.exchange(true,
                                                                                        std::memory_order_acq_rel)) {
                                                HookLogImportant(
                                                    "DX12: Startup overlay compat settled - future sync reinit will "
                                                    "keep "
                                                    "the full allocator pool");
                                            }
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
        }  // end !skipOverlayDraw
    skip_overlay_draw:;
    overlay_done:;
    }

    // Change 6: Remove verbose debug logging - keep only error logging
    if (captureAfterOverlay) {
        int64_t captureStartUs = PerfLogger::GetQpcUs();
        PublishDX12CapturedFrame(pSwapChain, captureShm, gameQueue, hasCurrentBackBufferIdx, currentBackBufferIdx);
        perfMetrics.captureUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - captureStartUs);
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
    const bool canUseFSRHeuristics = CanUseFSRFGHeuristics();
    if (!canUseFSRHeuristics) {
        // Do not immediately clear a live heuristic/native-FSR latch just
        // because heuristics are temporarily unsafe. Talos can keep the FSR
        // runtime-owned swapchain active while transient startup/menu state
        // makes one frame look ambiguous. A hard clear here collapses runtime
        // classification to STREAMLINE_NO_FG and tears down the still-live FSR
        // overlay path.
        if (!ce::dx12_overlay_policy::ShouldPreserveHeuristicFSRDuringTransientHeuristicBlock(
                canUseFSRHeuristics, g_FGRuntimeOwnsSwapchain, g_HadFSRFGPhase)) {
            g_FGCompat.SetHeuristicFSRFGActive(false);
        }
    }
    // Interpolated (FG) frame detection: the game submits zero command lists
    // between consecutive Present calls for frames generated by the FG engine.
    bool isInterpolatedFrame = (count == 0);
    if (PresentDebugSample* activeDebugSample = PerfLogger::Get().GetActiveDebugSample();
        activeDebugSample && isInterpolatedFrame) {
        activeDebugSample->flags |= kPresentSampleFlagInterpolatedFrame;
    }

    UINT currentBackBufferIdx = sc3->GetCurrentBackBufferIndex();

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
    bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    ID3D12CommandQueue* currentSwapchainQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        currentSwapchainQueue = g_SwapchainQueue;
    }
    const bool postFSRNonFGRecovery = ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(
        g_HadFSRFGPhase, g_NeedOffscreenOverlayAfterPostFSRNonFG, IsActualFrameGenerationActive(),
        streamlineFGRunning, currentSwapchainQueue != nullptr);
    const bool recentStreamlineTeardown = g_SLOffHeuristicGrace.load(std::memory_order_acquire) > 0;
    const bool authoritativeFSRActive = g_FGCompat.IsFSRFGApiActive();
    int authoritativeFSRRealFrameOnlyStreak = 0;
    if (ce::dx12_overlay_policy::ShouldTrackAuthoritativeFSRRealFrameOnlyRun(
            streamlineFGRunning, g_FGRuntimeOwnsSwapchain, authoritativeFSRActive, isInterpolatedFrame,
            recentStreamlineTeardown)) {
        authoritativeFSRRealFrameOnlyStreak =
            g_AuthoritativeFSRRealFrameOnlyStreak.fetch_add(1, std::memory_order_acq_rel) + 1;
    } else {
        ResetAuthoritativeFSRRealFrameOnlyStreak();
    }

    if (ce::dx12_overlay_policy::ShouldClearAuthoritativeFSRAfterRealFrameOnlyRun(
            authoritativeFSRRealFrameOnlyStreak)) {
        if (authoritativeFSRRealFrameOnlyStreak == 120 || (authoritativeFSRRealFrameOnlyStreak % 600) == 0) {
            HookLogImportant(
                "DX12: Clearing stale authoritative FSR FG after %d consecutive real frames on runtime-owned "
                "swapchain (origGame=%p scQueue=%p slFG=%d recentSLTeardown=%d)",
                authoritativeFSRRealFrameOnlyStreak, g_OriginalGameQueue, g_SwapchainQueue,
                streamlineFGRunning ? 1 : 0, recentStreamlineTeardown ? 1 : 0);
        }
        g_FGCompat.SetFSRFGActive(false);
        g_FGCompat.SetFSRFGMultiplier(0);
        ResetAuthoritativeFSRRealFrameOnlyStreak();
    }

    if (ce::dx12_overlay_policy::ShouldSkipProcessFrameForZeroECLPresent(
            isInterpolatedFrame, hasDedicatedQueue, heuristicFSRFG, g_FGRuntimeOwnsSwapchain, streamlineFGRunning,
            recentStreamlineTeardown, postFSRNonFGRecovery)) {
        sc3->Release();
        return;
    }
    if (!isInterpolatedFrame &&
        ce::dx12_overlay_policy::ShouldSuppressLikelyDuplicateTopLevelPresent(g_FGRuntimeOwnsSwapchain,
                                                                              streamlineFGRunning) &&
        ShouldSuppressLikelyDuplicateTopLevelPresent(sc3, currentBackBufferIdx)) {
        sc3->Release();
        return;
    }
    bool processCapture = !isInterpolatedFrame;
    if (processCapture && ShouldSkipCaptureForTargetCadence()) {
        processCapture = false;
    }

    SharedMemoryLayout* screenshotShm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    OverlayConfig screenshotOverlayCfg = GetActiveDX12OverlayConfig(screenshotShm);
    const bool screenshotRequested =
        screenshotShm && screenshotShm->runtimeState.cmdTakeScreenshot.load(std::memory_order_acquire);
    const bool screenshotWantsOverlay =
        screenshotRequested && screenshotOverlayCfg.showOverlay && screenshotOverlayCfg.screenshotIncludeOverlay;
    const bool screenshotUsePostSL =
        screenshotWantsOverlay && ShouldUseConfirmedPostSLForOverlayIncludedWork(screenshotOverlayCfg);
    if (screenshotRequested && !screenshotWantsOverlay) {
        CaptureRequestedDX12Screenshot(sc3, screenshotShm);
    }

    // For interpolated frames, only render overlay (no capture processing) since
    // the backbuffer content is from the FG engine, not a real game frame.
    ProcessFrame(sc3, processCapture);

    if (screenshotWantsOverlay && !screenshotUsePostSL) {
        CaptureRequestedDX12Screenshot(sc3, screenshotShm);
    }

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
    const auto runtimeMode = g_FGCompat.GetRuntimeMode();
    // Check ShouldUseDedicatedOverlayQueue() (FG active) instead of just queue
    // existence, since the queue is now kept alive across FG mode switches.
    const bool usingDedicatedQueue = ShouldUseDedicatedOverlayQueue() && (g_State.overlayQueue != nullptr);
    if (!ce::dx12_overlay_policy::ShouldWaitForOverlayCompletion(g_State.fenceEvent != nullptr, usingDedicatedQueue,
                                                                 overlayModule != nullptr, runtimeMode))
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
    // Safety: during FG transitions, SL may call ECL on a queue that's being freed.
    // Freed COM objects have null vtable.  Forward directly to real ECL to avoid crash.
    if (!pThis || !*reinterpret_cast<void**>(pThis)) {
        ExecuteCommandListsPtr real = oExecuteCommandLists;
        if (real)
            real(pThis, NumCommandLists, ppCommandLists);
        return;
    }

    // ECL heartbeat counter — read by SL hook to verify ECL is still firing.
    static std::atomic<uint64_t> s_eclCallCounter{0};
    uint64_t eclCount = s_eclCallCounter.fetch_add(1, std::memory_order_relaxed);
    if ((eclCount & 0xFFF) == 0) {
        // Every 4096 ECL calls (~every few frames), log a heartbeat
        static std::atomic<uint32_t> s_eclHeartbeatLogCount{0};
        if (s_eclHeartbeatLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
            HookLogImportant("DX12: ECL heartbeat #%llu (queue=%p, tid=0x%04X)", (unsigned long long)eclCount, pThis,
                             GetCurrentThreadId());
        }
    }

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
        // During SL FG, use the real D3D12 ECL to bypass SL's vtable hook.
        ExecuteCommandListsPtr realECL = g_RealD3D12ECL.load(std::memory_order_acquire);
        if (realECL && IsStreamlineLoaded() && IsActualFrameGenerationActive()) {
            realECL(pThis, NumCommandLists, ppCommandLists);
        } else {
            ExecuteCommandListsPtr original = GetOriginalExecuteCommandLists(pThis);
            if (original)
                original(pThis, NumCommandLists, ppCommandLists);
        }
        return;
    }

    // Skip queue tracking for PostSL overlay virtual calls.  When PostSL submits
    // via queue->ExecuteCommandLists() (virtual call through SL's COM wrapper),
    // SL dispatches to the real D3D12 queue which re-enters this detour.
    // The real queue address differs from g_OriginalGameQueue (SL's wrapper),
    // so without this guard we'd corrupt queue tracking state.
    if (s_insidePostSLOverlayECL) {
        // REAL QUEUE CAPTURE from SL's COM wrapper dispatch:
        //
        // When PostSL submits via slQueue->ExecuteCommandLists() (bootstrap frame),
        // SL's COM wrapper dispatches to its internal real D3D12 queue, which
        // re-enters this ECL detour.  pThis here is the REAL D3D12 queue, not
        // SL's wrapper.
        //
        // We capture this queue into g_RealQueueBehindSLWrapper for subsequent
        // frames to use direct submission (bypassing SL's cumulative damage).
        //
        // The bootstrap happens ONCE per PostSL reactivation epoch.  All subsequent
        // PostSL frames use: g_RealD3D12ECL(g_RealQueueBehindSLWrapper, ...).
        //
        // NOTE: If SL recreates its internal queues during a session (e.g., after
        // DLSS mode switch), this captured pointer could become stale.  Currently
        // no known trigger for this.  If stale, PostSL would crash and we'd see
        // DEVICE_REMOVED in logs — at which point re-bootstrap can be triggered.
        static std::atomic<ID3D12CommandQueue*> s_realQueueBehindSL{nullptr};
        ID3D12CommandQueue* realQueue = (ID3D12CommandQueue*)pThis;
        ExecuteCommandListsPtr original = GetOriginalExecuteCommandLists(pThis);
        ExecuteCommandListsPtr real = g_RealD3D12ECL.load(std::memory_order_acquire);
        const bool queueLooksDirect = original && real && original == real;
        ID3D12CommandQueue* capturedSLWrapperQueue = g_SLWrapperQueue.load(std::memory_order_acquire);
        ID3D12CommandQueue* currentCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
        const bool usableDirectQueueCandidate = ce::dx12_overlay_policy::IsUsableValidatedPostSLDirectQueueCandidate(
            queueLooksDirect, realQueue == capturedSLWrapperQueue, realQueue == currentCommandQueue,
            realQueue == g_OriginalGameQueue, realQueue == g_SwapchainQueue);
        if (usableDirectQueueCandidate) {
            ID3D12CommandQueue* previousRealQueue = s_realQueueBehindSL.exchange(realQueue, std::memory_order_acq_rel);
            g_RealQueueBehindSLWrapper.store(realQueue, std::memory_order_release);
            if (previousRealQueue != realQueue) {
                HookLogImportant(
                    "DX12: ECL captured validated real queue behind SL wrapper %p during PostSL submit/probe",
                    realQueue);
            }
        } else {
            HookLogImportant(
                "DX12: ECL ignored PostSL direct-queue capture candidate %p (origECL=%p realECL=%p matchesWrapper=%d "
                "matchesCmdQ=%d matchesOrig=%d matchesScQ=%d)",
                realQueue, (void*)original, (void*)real, realQueue == capturedSLWrapperQueue ? 1 : 0,
                realQueue == currentCommandQueue ? 1 : 0, realQueue == g_OriginalGameQueue ? 1 : 0,
                realQueue == g_SwapchainQueue ? 1 : 0);
        }

        if (original)
            original(pThis, NumCommandLists, ppCommandLists);
        else {
            if (real)
                real(pThis, NumCommandLists, ppCommandLists);
        }
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

    // Count command lists only from the trusted frame-classification queue.
    // Once ProcessFrame has identified the original game queue, prefer it over
    // the "first direct queue seen" heuristic to avoid auxiliary queue bursts
    // being misclassified as real presents.
    ID3D12CommandQueue* primaryQ = g_PrimaryGameQueue.load(std::memory_order_acquire);
    ID3D12CommandQueue* classificationQueue = GetFrameClassificationQueue();
    if (!classificationQueue || pThis == classificationQueue) {
        g_CommandListsExecutedThisFrame.fetch_add(NumCommandLists, std::memory_order_relaxed);
    }

    // Register game's queue for overlay execution.
    // During FG, SL's worker threads may call ECL on transient internal queues
    // that can be freed at any time.  Skip registration for unknown queues to
    // avoid calling virtual methods (GetDesc/GetDevice) on freed objects.
    {
        const bool anyFGActive = IsActualFrameGenerationActive() || IsNvidiaSmoothMotionActiveRuntime() ||
                                 DXGIShared::g_StreamlineFGRunning.load(std::memory_order_relaxed);

        // Capture SL's wrapper queue: during SL FG, any DIRECT queue in ECL
        // that's NOT origGame/scQueue/primaryQ is likely SL's COM wrapper.
        // This wrapper routes ECL through SL's internal handler to the correct
        // queue.  We need it for PostSL overlay rendering after FSR→DLSS
        // transitions where origGame and scQueue both fail BB barriers.
        if (DXGIShared::g_StreamlineFGRunning.load(std::memory_order_relaxed) && pThis != g_OriginalGameQueue &&
            pThis != g_SwapchainQueue && pThis != primaryQ) {
            ID3D12CommandQueue* prevWrapper = g_SLWrapperQueue.load(std::memory_order_relaxed);
            if (prevWrapper != pThis) {
                // Must use mutex: PostSL reads g_SLWrapperQueue under this mutex
                // and calls AddRef. Without mutex, we could Release the old wrapper
                // while PostSL is between load() and AddRef() → use-after-free.
                std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
                prevWrapper = g_SLWrapperQueue.load(std::memory_order_relaxed);
                if (prevWrapper != pThis) {
                    pThis->AddRef();
                    g_SLWrapperQueue.store(pThis, std::memory_order_release);
                    if (prevWrapper)
                        prevWrapper->Release();
                    static int s_wrapperLog = 0;
                    if (s_wrapperLog++ < 10)
                        HookLogImportant("DX12: ECL captured SL wrapper queue %p (origGame=%p scQ=%p primaryQ=%p)",
                                         pThis, g_OriginalGameQueue, g_SwapchainQueue, primaryQ);
                }
            }
        }

        // PERF FIX: During FG (especially FSR FG), the ECL detour fires on every
        // command list submission.  FSR FG alternates queues each call (origGame
        // vs FSR queue), so DX12_SetCommandQueue's early-out (g_CommandQueue==pThis)
        // NEVER fires.  This means GetDesc(), g_CommandQueueMutex, GetDevice(),
        // and DX12_HookQueueVTable all execute on every single ECL call —
        // tens of thousands per second.  The cumulative overhead breaks FSR FG's
        // tight internal timing, causing its fence wait to never complete → freeze.
        //
        // Fix: During active FG, skip DX12_SetCommandQueue for queues we already
        // know (origGame, scQueue, primaryQ).  These are stable DIRECT queues that
        // don't need repeated registration.  Only register truly new/unknown queues.
        ID3D12CommandQueue* currentQ = g_CommandQueue.load(std::memory_order_acquire);
        bool isKnownQueue =
            (pThis == primaryQ || pThis == currentQ || pThis == g_OriginalGameQueue || pThis == g_SwapchainQueue);
        const bool recentStreamlineTeardown = g_SLOffHeuristicGrace.load(std::memory_order_acquire) > 0;
        if (ce::dx12_overlay_policy::ShouldIgnoreCommandQueueRegistrationAfterRecentStreamlineTeardown(
                recentStreamlineTeardown, pThis == primaryQ, pThis == g_OriginalGameQueue, pThis == g_SwapchainQueue)) {
            const bool postSLActive = g_PostSLOverlayActive.load(std::memory_order_acquire);
            const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
            if (ce::dx12_overlay_policy::ShouldRefreshRecentPostSLTeardownActivity(
                    recentStreamlineTeardown, g_PostSLLastWorkingQueue && pThis == g_PostSLLastWorkingQueue,
                    streamlineFGRunning, postSLActive)) {
                MarkPostSLRecentTeardownActivity("DX12: ECL recent PostSL teardown activity", pThis);
            }
            static std::atomic<int> s_recentSLTeardownQueueIgnoreLogCount{0};
            int logCount = s_recentSLTeardownQueueIgnoreLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 256) == 0) {
                HookLogImportant(
                    "DX12: Ignoring departed queue %p during recent Streamline teardown "
                    "(primary=%p orig=%p scQ=%p current=%p slOffGrace=%d)",
                    pThis, primaryQ, g_OriginalGameQueue, g_SwapchainQueue, currentQ,
                    g_SLOffHeuristicGrace.load(std::memory_order_acquire));
            }
            goto skip_command_queue_registration;
        }
        if (!anyFGActive) {
            // No FG: always register (same as before)
            DX12_SetCommandQueue(pThis);
        } else if (!primaryQ) {
            // FG active but primary queue not yet captured: register to capture it
            DX12_SetCommandQueue(pThis);
        } else if (!isKnownQueue) {
            // FG active, unknown queue: register it (new queue from FG runtime)
            DX12_SetCommandQueue(pThis);
        }
        // else: FG active, known queue — skip registration (fast path)
    }
skip_command_queue_registration:

    // Periodic device-removed check in ECL detour during FG —
    // helps pinpoint when GPU dies relative to our hook activity.
    if ((eclCount & 0x3FF) == 0x200 && g_FGCompat.IsFGActive()) {
        auto* eclDev = g_Device.load(std::memory_order_acquire);
        if (eclDev) {
            HRESULT eclDevHr = eclDev->GetDeviceRemovedReason();
            if (FAILED(eclDevHr)) {
                static std::atomic<int> s_eclDevRemovedLogs{0};
                if (s_eclDevRemovedLogs.fetch_add(1, std::memory_order_relaxed) < 5) {
                    HookLogImportant(
                        "DX12: DEVICE REMOVED in ECL detour (eclCount=%llu queue=%p devRemoved=0x%08X tid=0x%04X)",
                        (unsigned long long)eclCount, pThis, (unsigned)eclDevHr, GetCurrentThreadId());
                }
            }
        }
    }

    ExecuteCommandListsPtr original = GetOriginalExecuteCommandLists(pThis);

    if (original)
        original(pThis, NumCommandLists, ppCommandLists);
}

__attribute__((noinline)) void DX12_HookQueueVTable(ID3D12CommandQueue* queue) {
    if (!queue)
        return;

    // Safety: freed COM objects have null vtable — skip
    void** vtblCheck = *reinterpret_cast<void***>(queue);
    if (!vtblCheck)
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
    // Hook vtable only for game's original queue — skip FG runtime queues
    if (pDevice) {
        ID3D12CommandQueue* q = nullptr;
        if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&q)))) {
            if (q == g_OriginalGameQueue || !g_OriginalGameQueue) {
                DX12_HookQueueVTable(q);
            }
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
    // Hook vtable only for game's original queue — skip FG runtime queues
    if (pDevice) {
        ID3D12CommandQueue* q = nullptr;
        if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&q)))) {
            if (q == g_OriginalGameQueue || !g_OriginalGameQueue) {
                DX12_HookQueueVTable(q);
            } else {
                HookLog("DX12: DetourCreateSwapChainForHwnd — skipping vtable hook for non-origGame queue %p", q);
            }
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

    // Clean up drain fence/event (used for FSR→DLSS transition)
    if (g_DrainFence) {
        g_DrainFence->Release();
        g_DrainFence = nullptr;
    }
    if (g_DrainEvent) {
        CloseHandle(g_DrainEvent);
        g_DrainEvent = nullptr;
    }
    g_DrainFenceValue = 0;
    g_NeedOffscreenOverlayAfterPostFSRNonFG = false;

    // Clean up prerender fences/events
    for (auto* fence : g_PrerenderFences) {
        if (fence)
            fence->Release();
    }
    g_PrerenderFences.clear();
    for (auto event : g_PrerenderEvents) {
        if (event)
            CloseHandle(event);
    }
    g_PrerenderEvents.clear();

    // Clean up descriptor-free backend
    if (g_DescFreeBackend) {
        g_DescFreeBackend->Shutdown();
        delete g_DescFreeBackend;
        g_DescFreeBackend = nullptr;
    }

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
    // Disable post-SL overlay callback before tearing down D3D12 resources.
    SetPostSLCallbackInstalled(false, "DX12: Shutdown");
    WaitForInFlightPostSLCallbacks("DX12: Shutdown");
    g_PostSLDeferredQueueCleanupPending.store(false, std::memory_order_release);
    ClearPostSLQueues("DX12: Shutdown");
    ClearPostSLPinnedSLWrapperQueue("DX12: Shutdown");
    SetPostSLLastWorkingQueue(nullptr);
    if (auto* deferredLockedQueue = g_DeferredPostSLLockedQueueRelease.exchange(nullptr, std::memory_order_acq_rel)) {
        deferredLockedQueue->Release();
    }
    if (g_CommandQueue.load()) {
        g_CommandQueue.load()->Release();
        g_CommandQueue.store(nullptr);
    }
    if (auto* deferredCommandQueue = g_DeferredCommandQueueRelease.exchange(nullptr, std::memory_order_acq_rel)) {
        deferredCommandQueue->Release();
    }
    if (g_OriginalGameQueue) {
        g_OriginalGameQueue->Release();
        g_OriginalGameQueue = nullptr;
    }
    if (g_PreFGGameQueue) {
        g_PreFGGameQueue->Release();
        g_PreFGGameQueue = nullptr;
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
    if (g_SharedCaptureD3D12.IsActive()) {
        std::lock_guard<std::recursive_mutex> capLock(g_DX12CaptureMutex);
        g_SharedCaptureD3D12.Reset();
    }
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
