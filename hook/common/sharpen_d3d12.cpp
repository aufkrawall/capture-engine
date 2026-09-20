#include "sharpen_d3d12.h"

#include <atomic>

#include "sharpen_constants.h"
#include "sharpen_d3d11.h"
#include "sharpen_shader_bytecode.h"

using Microsoft::WRL::ComPtr;

namespace ce::sharpen {
namespace {

// b0 holds ShaderConstants as root constants: 12 DWORDs, no constant buffer to
// allocate, map or keep alive per frame.
constexpr UINT kRootConstantCount = sizeof(ShaderConstants) / sizeof(uint32_t);
static_assert(kRootConstantCount == 12, "Root constant count must match ShaderConstants");

constexpr UINT kRootParamConstants = 0;
constexpr UINT kRootParamSourceTable = 1;

D3D12_RESOURCE_BARRIER MakeTransition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                      D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

ComPtr<ID3D12RootSignature> CreateRootSignature(ID3D12Device* device) {
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER parameters[2] = {};
    parameters[kRootParamConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[kRootParamConstants].Constants.Num32BitValues = kRootConstantCount;
    parameters[kRootParamConstants].Constants.ShaderRegister = 0;
    parameters[kRootParamConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[kRootParamSourceTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[kRootParamSourceTable].DescriptorTable.NumDescriptorRanges = 1;
    parameters[kRootParamSourceTable].DescriptorTable.pDescriptorRanges = &srvRange;
    parameters[kRootParamSourceTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 2;
    desc.pParameters = parameters;
    // The fullscreen triangle is generated from SV_VertexID, so no input
    // assembler layout is needed and every unused stage is denied.
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors))) {
        HookLogImportant("Sharpen: DX12 root signature could not be serialized");
        return nullptr;
    }

    ComPtr<ID3D12RootSignature> rootSignature;
    if (FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                           IID_PPV_ARGS(&rootSignature)))) {
        HookLogImportant("Sharpen: DX12 root signature could not be created");
        return nullptr;
    }
    return rootSignature;
}

}  // namespace

bool D3D12Pass::EnsureDeviceObjects(ID3D12Device* device) {
    if (ownerDevice_ != device) {
        Shutdown();
        ownerDevice_ = device;
    }

    if (!rootSignature_) {
        rootSignature_ = CreateRootSignature(device);
        if (!rootSignature_)
            return false;
    }

    if (!srvHeap_) {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 1;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srvHeap_))))
            return false;
    }
    if (!rtvHeap_) {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.NumDescriptors = 1;
        if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap_))))
            return false;
    }

    for (UINT slot = 0; slot < kAllocatorSlots; ++slot) {
        if (!allocators_[slot] &&
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators_[slot])))) {
            return false;
        }
    }
    if (!commandList_) {
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0].Get(), nullptr,
                                             IID_PPV_ARGS(&commandList_)))) {
            return false;
        }
        commandList_->Close();
    }
    if (!fence_ && FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))))
        return false;

    return true;
}

bool D3D12Pass::EnsurePipelineState(ID3D12Device* device, Mode mode, DXGI_FORMAT viewFormat) {
    if (pipelineState_ && pipelineMode_ == mode && pipelineFormat_ == viewFormat)
        return true;

    pipelineState_.Reset();
    pipelineMode_ = Mode::Off;
    pipelineFormat_ = DXGI_FORMAT_UNKNOWN;

    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - the D3D descriptor is zero-initialized before its enum fields are assigned
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = rootSignature_.Get();
    desc.VS = {g_SharpenVS_5_0, sizeof(g_SharpenVS_5_0)};
    if (mode == Mode::Cas) {
        desc.PS = {g_SharpenPS_Cas_5_0, sizeof(g_SharpenPS_Cas_5_0)};
    } else {
        desc.PS = {g_SharpenPS_Rcas_5_0, sizeof(g_SharpenPS_Rcas_5_0)};
    }
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    // The filter replaces the frame; it never blends with what is there.
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        desc.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = viewFormat;
    desc.SampleDesc.Count = 1;

    if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipelineState_)))) {
        HookLogImportant("Sharpen: DX12 pipeline state for mode=%s fmt=%d could not be created", ModeName(mode),
                         static_cast<int>(viewFormat));
        return false;
    }
    pipelineMode_ = mode;
    pipelineFormat_ = viewFormat;
    return true;
}

bool D3D12Pass::EnsureSourceCopy(ID3D12Device* device, ID3D12Resource* target, DXGI_FORMAT viewFormat) {
    const D3D12_RESOURCE_DESC targetDesc = target->GetDesc();
    const bool matches = sourceCopy_ && copyWidth_ == targetDesc.Width && copyHeight_ == targetDesc.Height &&
                         copyFormat_ == targetDesc.Format && copyViewFormat_ == viewFormat;
    if (matches)
        return true;

    sourceCopy_.Reset();
    copyWidth_ = 0;
    copyHeight_ = 0;
    copyFormat_ = DXGI_FORMAT_UNKNOWN;
    copyViewFormat_ = DXGI_FORMAT_UNKNOWN;

    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - the D3D descriptor is zero-initialized before its enum fields are assigned
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    // CopyResource needs identical formats or the same typeless group, so the
    // copy keeps the target's storage format and only the view reinterprets it.
    D3D12_RESOURCE_DESC copyDesc = targetDesc;
    copyDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    copyDesc.MipLevels = 1;
    copyDesc.DepthOrArraySize = 1;
    copyDesc.SampleDesc.Count = 1;
    copyDesc.SampleDesc.Quality = 0;

    if (FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &copyDesc,
                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                               IID_PPV_ARGS(&sourceCopy_)))) {
        HookLogImportant("Sharpen: DX12 source copy %llux%u fmt=%d could not be created",
                         static_cast<unsigned long long>(targetDesc.Width), targetDesc.Height,
                         static_cast<int>(targetDesc.Format));
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
    viewDesc.Format = viewFormat;
    viewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    viewDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    viewDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(sourceCopy_.Get(), &viewDesc, srvHeap_->GetCPUDescriptorHandleForHeapStart());

    copyWidth_ = targetDesc.Width;
    copyHeight_ = targetDesc.Height;
    copyFormat_ = targetDesc.Format;
    copyViewFormat_ = viewFormat;
    HookLogImportant("Sharpen: DX12 source copy ready %llux%u storage=%d view=%d",
                     static_cast<unsigned long long>(copyWidth_), copyHeight_, static_cast<int>(copyFormat_),
                     static_cast<int>(copyViewFormat_));
    return true;
}

bool D3D12Pass::EnsureTargetView(ID3D12Device* device, ID3D12Resource* target, DXGI_FORMAT viewFormat) {
    if (viewedTarget_ == target && viewedFormat_ == viewFormat)
        return true;

    D3D12_RENDER_TARGET_VIEW_DESC viewDesc = {};
    viewDesc.Format = viewFormat;
    viewDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(target, &viewDesc, rtvHeap_->GetCPUDescriptorHandleForHeapStart());
    viewedTarget_ = target;
    viewedFormat_ = viewFormat;
    return true;
}

int D3D12Pass::AcquireAllocatorSlot() {
    const UINT64 completed = fence_->GetCompletedValue();
    if (completed == UINT64_MAX)
        return -1;  // Device removal.
    for (UINT slot = 0; slot < kAllocatorSlots; ++slot) {
        if (allocatorFenceValues_[slot] == 0 || completed >= allocatorFenceValues_[slot])
            return static_cast<int>(slot);
    }
    return -1;
}

bool D3D12Pass::Render(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* target,
                       DXGI_FORMAT viewFormat, D3D12_RESOURCE_STATES targetStateBefore, const Request& request,
                       Route route, TargetEncoding encoding) {
    if (HookIsShuttingDown() || !device || !queue || !target)
        return false;

    if (request.mode == Mode::Off) {
        if (logGate_.ShouldLog(false, "disabled"))
            HookLog("Sharpen: DX12 pass idle (disabled)");
        return false;
    }

    const D3D12_RESOURCE_DESC targetDesc = target->GetDesc();
    const DXGI_FORMAT resolvedViewFormat = viewFormat != DXGI_FORMAT_UNKNOWN ? viewFormat : targetDesc.Format;

    Target targetInfo;
    targetInfo.route = route;
    targetInfo.encoding = encoding;
    targetInfo.width = static_cast<uint32_t>(targetDesc.Width);
    targetInfo.height = targetDesc.Height;
    targetInfo.viewAppliesSrgbConversion = FormatAppliesSrgbConversion(resolvedViewFormat);
    // A multisampled frame would need a resolve rather than a copy, and a
    // DENY_SHADER_RESOURCE target cannot be read at all.
    targetInfo.readable = targetDesc.SampleDesc.Count == 1 &&
                          (targetDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0;
    targetInfo.writable = (targetDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;

    const Decision decision = Decide(request, targetInfo);
    if (logGate_.ShouldLog(decision.run, decision.reason)) {
        HookLogImportant("Sharpen: DX12 %s reason=%s %ux%u view=%d srgbView=%d samples=%u route=%d param=%.3f",
                         decision.run ? "running" : "idle", decision.reason, targetInfo.width, targetInfo.height,
                         static_cast<int>(resolvedViewFormat), targetInfo.viewAppliesSrgbConversion ? 1 : 0,
                         targetDesc.SampleDesc.Count, static_cast<int>(route),
                         static_cast<double>(decision.effectParameter));
    }
    if (!decision.run)
        return false;

    if (!EnsureDeviceObjects(device))
        return false;
    if (!EnsurePipelineState(device, request.mode, resolvedViewFormat))
        return false;
    if (!EnsureSourceCopy(device, target, resolvedViewFormat))
        return false;
    if (!EnsureTargetView(device, target, resolvedViewFormat))
        return false;

    const int slot = AcquireAllocatorSlot();
    if (slot < 0) {
        // Never wait on the present thread. A skipped frame is one unfiltered
        // frame; a wait here would be a stall in the game's Present.
        static std::atomic<int> s_busyLogCount{0};
        const int logCount = s_busyLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 600) == 0)
            HookLogImportant("Sharpen: DX12 skipped a frame - every command allocator is still in flight (#%d)",
                             logCount + 1);
        return false;
    }

    ID3D12CommandAllocator* allocator = allocators_[static_cast<UINT>(slot)].Get();
    if (FAILED(allocator->Reset()))
        return false;
    if (FAILED(commandList_->Reset(allocator, pipelineState_.Get())))
        return false;

    ID3D12GraphicsCommandList* list = commandList_.Get();

    // A transition whose before and after states are equal is invalid, so the
    // frame's own barrier is emitted only when its incoming state differs.
    D3D12_RESOURCE_BARRIER preCopy[2] = {};
    UINT preCopyCount = 0;
    if (targetStateBefore != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        preCopy[preCopyCount++] = MakeTransition(target, targetStateBefore, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    preCopy[preCopyCount++] = MakeTransition(sourceCopy_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                             D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(preCopyCount, preCopy);

    list->CopyResource(sourceCopy_.Get(), target);

    const D3D12_RESOURCE_BARRIER preDraw[2] = {
        MakeTransition(target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        MakeTransition(sourceCopy_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
    };
    list->ResourceBarrier(2, preDraw);

    const ShaderConstants constants =
        BuildShaderConstants(request.mode, decision, targetInfo.width, targetInfo.height);
    ID3D12DescriptorHeap* heaps[] = {srvHeap_.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(rootSignature_.Get());
    list->SetGraphicsRoot32BitConstants(kRootParamConstants, kRootConstantCount, &constants, 0);
    list->SetGraphicsRootDescriptorTable(kRootParamSourceTable, srvHeap_->GetGPUDescriptorHandleForHeapStart());

    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(targetInfo.width);
    viewport.Height = static_cast<float>(targetInfo.height);
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor = {0, 0, static_cast<LONG>(targetInfo.width), static_cast<LONG>(targetInfo.height)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);

    const D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    list->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);

    // The frame is handed back in exactly the state it arrived in, so whatever
    // records after this pass - the overlay, the capture copy, the present -
    // sees the resource state it expects.
    if (targetStateBefore != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        const D3D12_RESOURCE_BARRIER restore =
            MakeTransition(target, D3D12_RESOURCE_STATE_RENDER_TARGET, targetStateBefore);
        list->ResourceBarrier(1, &restore);
    }

    if (FAILED(list->Close()))
        return false;

    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);

    const UINT64 fenceValue = ++fenceValue_;
    if (FAILED(queue->Signal(fence_.Get(), fenceValue))) {
        // The slot cannot be proven retired without the signal, so it is left
        // marked busy rather than recycled into a list the GPU may still read.
        HookLogImportant("Sharpen: DX12 queue signal failed; allocator slot %d retired conservatively", slot);
        allocatorFenceValues_[static_cast<UINT>(slot)] = UINT64_MAX;
        return false;
    }
    allocatorFenceValues_[static_cast<UINT>(slot)] = fenceValue;
    return true;
}

void D3D12Pass::Shutdown() {
    sourceCopy_.Reset();
    rtvHeap_.Reset();
    srvHeap_.Reset();
    fence_.Reset();
    commandList_.Reset();
    for (UINT slot = 0; slot < kAllocatorSlots; ++slot) {
        allocators_[slot].Reset();
        allocatorFenceValues_[slot] = 0;
    }
    pipelineState_.Reset();
    rootSignature_.Reset();
    pipelineMode_ = Mode::Off;
    pipelineFormat_ = DXGI_FORMAT_UNKNOWN;
    copyWidth_ = 0;
    copyHeight_ = 0;
    copyFormat_ = DXGI_FORMAT_UNKNOWN;
    copyViewFormat_ = DXGI_FORMAT_UNKNOWN;
    viewedTarget_ = nullptr;
    viewedFormat_ = DXGI_FORMAT_UNKNOWN;
    fenceValue_ = 0;
    ownerDevice_ = nullptr;
}

void D3D12Pass::Abandon() {
    sourceCopy_.Detach();
    rtvHeap_.Detach();
    srvHeap_.Detach();
    fence_.Detach();
    commandList_.Detach();
    for (UINT slot = 0; slot < kAllocatorSlots; ++slot) {
        allocators_[slot].Detach();
        allocatorFenceValues_[slot] = 0;
    }
    pipelineState_.Detach();
    rootSignature_.Detach();
    pipelineMode_ = Mode::Off;
    pipelineFormat_ = DXGI_FORMAT_UNKNOWN;
    copyWidth_ = 0;
    copyHeight_ = 0;
    copyFormat_ = DXGI_FORMAT_UNKNOWN;
    copyViewFormat_ = DXGI_FORMAT_UNKNOWN;
    viewedTarget_ = nullptr;
    viewedFormat_ = DXGI_FORMAT_UNKNOWN;
    fenceValue_ = 0;
    ownerDevice_ = nullptr;
}

}  // namespace ce::sharpen
