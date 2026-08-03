// Shared DX12 resources for frame-generation test apps.
// Provides realistic depth, motion-vector, HUD-less color, and UI surfaces.

#pragma once

#include <d3d12.h>
#include <dxgi.h>
#include <ffx_framegeneration.h>
#include <wrl/client.h>

#include "testapp_common.h"

namespace testapp::dx12fg {

using Microsoft::WRL::ComPtr;

constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
// High-precision linear color for the scene -> upscaler -> hudless chain. 8-bit UNORM inputs band
// visibly once a temporal upscaler accumulates/sharpens slow gradients (DLSS SR showed marching
// brightness bands on the animated cube faces); FP16 keeps the whole temporal chain band-free and
// the present blit dithers the final 8-bit quantization.
constexpr DXGI_FORMAT kHdrColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kMotionVectorFormat = DXGI_FORMAT_R16G16_FLOAT;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
constexpr DXGI_FORMAT kMaskFormat = DXGI_FORMAT_R8_UNORM;
constexpr D3D12_RESOURCE_STATES kColorReadState = static_cast<D3D12_RESOURCE_STATES>(
    static_cast<UINT>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) |
    static_cast<UINT>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
// Plain shader-read for the depth texture between frames. Deliberately WITHOUT
// D3D12_RESOURCE_STATE_DEPTH_READ: nothing rebinds it as a read-only DSV, and the pure SRV combo is
// exactly representable as the FFX-side FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ declared for FSR
// dispatch inputs (FFX state flags are NOT D3D12 state bits; mismatched declarations make the FFX
// backend emit wrong transition barriers).
constexpr D3D12_RESOURCE_STATES kDepthReadState = static_cast<D3D12_RESOURCE_STATES>(
    static_cast<UINT>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) |
    static_cast<UINT>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));

// Render-resolution inputs (sceneColor/motionVectors/depth/masks) feed the upscalers; the
// display-resolution hudlessColor is the upscaled pre-UI frame consumed by FG and present-compose.
// When no upscaling is active (render == display) sceneColor still exists but the scene renders
// straight into hudlessColor and the upscale stage is skipped.
struct AuxiliaryResources {
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    ComPtr<ID3D12Resource> hudlessColor;   // display res; upscaler output (UAV-capable)
    ComPtr<ID3D12Resource> uiColor;        // display res
    ComPtr<ID3D12Resource> sceneColor;     // render res; upscaler color input
    ComPtr<ID3D12Resource> motionVectors;  // render res
    ComPtr<ID3D12Resource> depth;          // render res
    // Render-res upscaler mask inputs (FSR reactive / transparency-and-composition); cleared every
    // frame for now, ready for future render elements (particles, animated screens, DOF, ...).
    ComPtr<ID3D12Resource> reactiveMask;
    ComPtr<ID3D12Resource> transparencyMask;
    UINT rtvStride = 0;
    UINT renderWidth = 0;
    UINT renderHeight = 0;
    UINT displayWidth = 0;
    UINT displayHeight = 0;
    DXGI_FORMAT colorFormat = kColorFormat;  // format of sceneColor + hudlessColor
    D3D12_RESOURCE_STATES hudlessState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_STATES uiState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_STATES sceneState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_STATES motionState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    D3D12_RESOURCE_STATES reactiveState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_STATES transparencyState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bool valid = false;

    D3D12_CPU_DESCRIPTOR_HANDLE RtvAt(UINT index) const {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(rtvStride) * index;
        return handle;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE HudlessRtv() const {
        return RtvAt(0);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE UiRtv() const {
        return RtvAt(1);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE MotionRtv() const {
        return RtvAt(2);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE SceneRtv() const {
        return RtvAt(3);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE ReactiveRtv() const {
        return RtvAt(4);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE TransparencyRtv() const {
        return RtvAt(5);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE DepthDsv() const {
        return dsvHeap->GetCPUDescriptorHandleForHeapStart();
    }
};

inline D3D12_HEAP_PROPERTIES DefaultHeapProperties() {
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3D12_HEAP_PROPERTIES props = {};
    props.Type = D3D12_HEAP_TYPE_DEFAULT;
    props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    props.CreationNodeMask = 1;
    props.VisibleNodeMask = 1;
    return props;
}

inline D3D12_RESOURCE_DESC Texture2DDesc(DXGI_FORMAT format, UINT width, UINT height, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    return desc;
}

// renderWidth/renderHeight and sceneColorFormat default to the display size / 8-bit color so the
// single-runtime test apps (dx12_dlss_fg_test, dx12_fsr_fg_test) keep their existing behavior
// unchanged; the switch app passes the render resolution and kHdrColorFormat.
inline bool CreateAuxiliaryResources(ID3D12Device* device, UINT width, UINT height, AuxiliaryResources& out,
                                     UINT renderWidth = 0, UINT renderHeight = 0,
                                     DXGI_FORMAT sceneColorFormat = kColorFormat) {
    if (!device || width == 0 || height == 0) {
        return false;
    }
    if (renderWidth == 0 || renderHeight == 0) {
        renderWidth = width;
        renderHeight = height;
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 6;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (FAILED(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&out.rtvHeap)))) {
        testapp::Log("[FG-DIAG] Failed to create auxiliary RTV heap\n");
        return false;
    }
    out.rtvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if (FAILED(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&out.dsvHeap)))) {
        testapp::Log("[FG-DIAG] Failed to create auxiliary DSV heap\n");
        return false;
    }

    const D3D12_HEAP_PROPERTIES heapProps = DefaultHeapProperties();
    D3D12_CLEAR_VALUE colorClear = {};
    colorClear.Format = sceneColorFormat;
    colorClear.Color[3] = 1.0f;
    D3D12_CLEAR_VALUE uiClear = {};
    uiClear.Format = kColorFormat;
    D3D12_CLEAR_VALUE motionClear = {};
    motionClear.Format = kMotionVectorFormat;
    D3D12_CLEAR_VALUE depthClear = {};
    depthClear.Format = kDepthFormat;
    depthClear.DepthStencil.Depth = 1.0f;
    D3D12_CLEAR_VALUE maskClear = {};
    maskClear.Format = kMaskFormat;

    // hudlessColor is the upscaler output target: DLSS/FSR super resolution write it as a UAV.
    D3D12_RESOURCE_DESC hudlessDesc =
        Texture2DDesc(sceneColorFormat, width, height,
                      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12_RESOURCE_DESC uiDesc = Texture2DDesc(kColorFormat, width, height, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    D3D12_RESOURCE_DESC sceneDesc =
        Texture2DDesc(sceneColorFormat, renderWidth, renderHeight, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    D3D12_RESOURCE_DESC motionDesc =
        Texture2DDesc(kMotionVectorFormat, renderWidth, renderHeight, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    D3D12_RESOURCE_DESC depthDesc =
        Texture2DDesc(kDepthFormat, renderWidth, renderHeight, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    D3D12_RESOURCE_DESC maskDesc =
        Texture2DDesc(kMaskFormat, renderWidth, renderHeight, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

    if (FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &hudlessDesc,
                                               D3D12_RESOURCE_STATE_RENDER_TARGET, &colorClear,
                                               IID_PPV_ARGS(&out.hudlessColor))) ||
        FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &uiDesc,
                                               D3D12_RESOURCE_STATE_RENDER_TARGET, &uiClear,
                                               IID_PPV_ARGS(&out.uiColor))) ||
        FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &sceneDesc,
                                               D3D12_RESOURCE_STATE_RENDER_TARGET, &colorClear,
                                               IID_PPV_ARGS(&out.sceneColor))) ||
        FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &motionDesc,
                                               D3D12_RESOURCE_STATE_RENDER_TARGET, &motionClear,
                                               IID_PPV_ARGS(&out.motionVectors))) ||
        FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc,
                                               D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear,
                                               IID_PPV_ARGS(&out.depth))) ||
        FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &maskDesc,
                                               D3D12_RESOURCE_STATE_RENDER_TARGET, &maskClear,
                                               IID_PPV_ARGS(&out.reactiveMask))) ||
        FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &maskDesc,
                                               D3D12_RESOURCE_STATE_RENDER_TARGET, &maskClear,
                                               IID_PPV_ARGS(&out.transparencyMask)))) {
        testapp::Log("[FG-DIAG] Failed to create auxiliary FG input textures\n");
        return false;
    }

    device->CreateRenderTargetView(out.hudlessColor.Get(), nullptr, out.HudlessRtv());
    device->CreateRenderTargetView(out.uiColor.Get(), nullptr, out.UiRtv());
    device->CreateRenderTargetView(out.motionVectors.Get(), nullptr, out.MotionRtv());
    device->CreateRenderTargetView(out.sceneColor.Get(), nullptr, out.SceneRtv());
    device->CreateRenderTargetView(out.reactiveMask.Get(), nullptr, out.ReactiveRtv());
    device->CreateRenderTargetView(out.transparencyMask.Get(), nullptr, out.TransparencyRtv());
    device->CreateDepthStencilView(out.depth.Get(), nullptr, out.DepthDsv());
    out.renderWidth = renderWidth;
    out.renderHeight = renderHeight;
    out.displayWidth = width;
    out.displayHeight = height;
    out.colorFormat = sceneColorFormat;
    out.valid = true;
    testapp::Log(
        "[FG-DIAG] Auxiliary FG resources: hudless=%p ui=%p scene=%p mvec=%p depth=%p reactive=%p "
        "transparency=%p display=%ux%u render=%ux%u colorFormat=%d\n",
        out.hudlessColor.Get(), out.uiColor.Get(), out.sceneColor.Get(), out.motionVectors.Get(), out.depth.Get(),
        out.reactiveMask.Get(), out.transparencyMask.Get(), width, height, renderWidth, renderHeight,
        static_cast<int>(sceneColorFormat));
    return true;
}

inline void ReleaseAuxiliaryResources(AuxiliaryResources& resources) {
    resources.hudlessColor.Reset();
    resources.uiColor.Reset();
    resources.sceneColor.Reset();
    resources.motionVectors.Reset();
    resources.depth.Reset();
    resources.reactiveMask.Reset();
    resources.transparencyMask.Reset();
    resources.rtvHeap.Reset();
    resources.dsvHeap.Reset();
    resources.rtvStride = 0;
    resources.renderWidth = 0;
    resources.renderHeight = 0;
    resources.displayWidth = 0;
    resources.displayHeight = 0;
    resources.colorFormat = kColorFormat;
    resources.hudlessState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    resources.uiState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    resources.sceneState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    resources.motionState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    resources.depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    resources.reactiveState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    resources.transparencyState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    resources.valid = false;
}

inline void Transition(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource,
                       D3D12_RESOURCE_STATES& currentState, D3D12_RESOURCE_STATES nextState) {
    if (!resource || currentState == nextState) {
        return;
    }
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = currentState;
    barrier.Transition.StateAfter = nextState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &barrier);
    currentState = nextState;
}

inline void RenderAuxiliaryInputs(ID3D12GraphicsCommandList* commandList, AuxiliaryResources& resources, int width,
                                  int height, float barPosition) {
    if (!resources.valid) {
        return;
    }

    Transition(commandList, resources.hudlessColor.Get(), resources.hudlessState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Transition(commandList, resources.uiColor.Get(), resources.uiState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Transition(commandList, resources.motionVectors.Get(), resources.motionState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Transition(commandList, resources.depth.Get(), resources.depthState, D3D12_RESOURCE_STATE_DEPTH_WRITE);

    const float sceneColor[] = {0.1f, 0.1f, 0.1f, 1.0f};
    const float uiClear[] = {0.0f, 0.0f, 0.0f, 0.0f};
    const float motionColor[] = {-0.015f, 0.0f, 0.0f, 0.0f};
    commandList->ClearRenderTargetView(resources.HudlessRtv(), sceneColor, 0, nullptr);
    commandList->ClearRenderTargetView(resources.UiRtv(), uiClear, 0, nullptr);
    commandList->ClearRenderTargetView(resources.MotionRtv(), motionColor, 0, nullptr);
    commandList->ClearDepthStencilView(resources.DepthDsv(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    D3D12_RECT sceneRect = {
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        static_cast<LONG>(barPosition * (width - 100)),
        height / 2 - 50,
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        static_cast<LONG>(barPosition * (width - 100) + 100),
        height / 2 + 50,
    };
    const float barColor[] = {1.0f, 1.0f, 1.0f, 1.0f};
    commandList->ClearRenderTargetView(resources.HudlessRtv(), barColor, 1, &sceneRect);

    D3D12_RECT uiRect = {24, 24, 260, 84};
    const float uiColor[] = {0.0f, 0.8f, 0.2f, 0.85f};
    commandList->ClearRenderTargetView(resources.UiRtv(), uiColor, 1, &uiRect);

    Transition(commandList, resources.hudlessColor.Get(), resources.hudlessState, kColorReadState);
    Transition(commandList, resources.uiColor.Get(), resources.uiState, kColorReadState);
    Transition(commandList, resources.motionVectors.Get(), resources.motionState, kColorReadState);
    Transition(commandList, resources.depth.Get(), resources.depthState, kDepthReadState);
}

// FFX present-callback helpers (mirrors capture-engine CopyFFXPresentSourceToOutput).
inline D3D12_RESOURCE_STATES GetDX12StateFromFfxResourceState(uint32_t state) {
    D3D12_RESOURCE_STATES dx12State = D3D12_RESOURCE_STATE_COMMON;
    if (state & FFX_API_RESOURCE_STATE_UNORDERED_ACCESS) {
        dx12State = static_cast<D3D12_RESOURCE_STATES>(dx12State | D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    if (state & FFX_API_RESOURCE_STATE_COMPUTE_READ) {
        dx12State = static_cast<D3D12_RESOURCE_STATES>(dx12State | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (state & FFX_API_RESOURCE_STATE_PIXEL_READ) {
        dx12State = static_cast<D3D12_RESOURCE_STATES>(dx12State | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    if (state & FFX_API_RESOURCE_STATE_COPY_SRC) {
        dx12State = static_cast<D3D12_RESOURCE_STATES>(dx12State | D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    if (state & FFX_API_RESOURCE_STATE_COPY_DEST) {
        dx12State = static_cast<D3D12_RESOURCE_STATES>(dx12State | D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if (state & FFX_API_RESOURCE_STATE_PRESENT) {
        dx12State = static_cast<D3D12_RESOURCE_STATES>(dx12State | D3D12_RESOURCE_STATE_PRESENT);
    }
    if (state & FFX_API_RESOURCE_STATE_RENDER_TARGET) {
        dx12State = static_cast<D3D12_RESOURCE_STATES>(dx12State | D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    return dx12State;
}

inline void CopyFfxPresentSourceToOutput(ID3D12GraphicsCommandList* commandList,
                                         ffxCallbackDescFrameGenerationPresent* desc) {
    if (!commandList || !desc || !desc->currentBackBuffer.resource || !desc->outputSwapChainBuffer.resource) {
        return;
    }
    if (desc->currentBackBuffer.resource == desc->outputSwapChainBuffer.resource) {
        return;
    }

    auto* source = static_cast<ID3D12Resource*>(desc->currentBackBuffer.resource);
    auto* output = static_cast<ID3D12Resource*>(desc->outputSwapChainBuffer.resource);
    D3D12_RESOURCE_STATES sourceState = GetDX12StateFromFfxResourceState(desc->currentBackBuffer.state);
    D3D12_RESOURCE_STATES outputState = GetDX12StateFromFfxResourceState(desc->outputSwapChainBuffer.state);
    const D3D12_RESOURCE_STATES savedSourceState = sourceState;
    const D3D12_RESOURCE_STATES savedOutputState = outputState;

    Transition(commandList, source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(commandList, output, outputState, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->CopyResource(output, source);
    Transition(commandList, output, outputState, savedOutputState);
    Transition(commandList, source, sourceState, savedSourceState);
}

}  // namespace testapp::dx12fg
