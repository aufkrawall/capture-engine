#include "custom_overlay_dx12.h"

#include <algorithm>
#include <cstring>
#include "../apis/dx12_hook.h"
#include "dx12_overlay_policy.h"
#include "hook_common.h"

namespace CustomOverlay {

static thread_local uint64_t s_FrameCounter = 0;
static thread_local uint64_t s_RenderCounter = 0;

void DX12Backend::SetRenderTarget(ID3D12GraphicsCommandList* cmdList, D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle) {
    s_FrameCounter++;
    DX12_DEBUG_FRAME(s_FrameCounter, "SetRenderTarget: cmdList=%p, rtvHandle.ptr=%llx", cmdList, rtvHandle.ptr);

    currentCmdList = cmdList;
    currentRTV = rtvHandle;
}

void DX12Backend::SetUploadSlotFence(ID3D12Fence* fence, uint64_t guardValue) {
    // The binding pins the fence with an owning reference so a replacement
    // fence can never reuse the released old fence's address (ABA reuse).
    // Without the pin, pointer comparison can miss the lifetime change and
    // stale guards wedge every present for the full wait timeout
    // (session 20260813_173453).
    if (slotGuardBinding.RebindIfNeeded(fence)) {
        for (int i = 0; i < kFramePoolSize; ++i) {
            slotFenceValue[i] = 0;
        }
    }
    nextSlotFenceValue = guardValue;
}

void DX12Backend::SetNextUploadSlot(int slot) {
    nextForcedUploadSlot.store(slot >= 0 && slot < kFramePoolSize ? slot : -1, std::memory_order_release);
}

bool DX12Backend::PrimeResources(ID3D12GraphicsCommandList* cmdList) {
    DX12_DEBUG_STEP("PrimeResources", "START - initialized=%d, cmdList=%p, pending=%d", initialized ? 1 : 0, cmdList,
                    HasPendingResources() ? 1 : 0);

    if (!initialized || !cmdList) {
        DX12_DEBUG_STEP("PrimeResources", "FAILED - initialized=%d, cmdList=%p", initialized ? 1 : 0, cmdList);
        return false;
    }

    return UploadFontTextureIfNeeded(cmdList);
}

bool DX12Backend::UploadFontTextureIfNeeded(ID3D12GraphicsCommandList* cmdList) {
    if (fontUploaded.load(std::memory_order_acquire) || !uploadBuffer || !fontTexture) {
        return true;
    }
    if (!cmdList) {
        DX12_DEBUG_STEP("UploadFontTextureIfNeeded", "FAILED - no command list");
        return false;
    }

    DX12_DEBUG_STEP("UploadFontTextureIfNeeded", "Font upload: Copying texture to default heap");

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = uploadBuffer.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = fontTextureFootprint;

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = fontTexture.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    DX12_DEBUG_STEP("UploadFontTextureIfNeeded", "Font upload: Calling CopyTextureRegion");
    cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    DX12_DEBUG_STEP("UploadFontTextureIfNeeded", "Font upload: CopyTextureRegion complete");

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = fontTexture.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    DX12_DEBUG_STEP("UploadFontTextureIfNeeded", "Font upload: Transitioning COPY_DEST -> PIXEL_SHADER_RESOURCE");
    cmdList->ResourceBarrier(1, &barrier);
    DX12_DEBUG_STEP("UploadFontTextureIfNeeded", "Font upload: Barrier submitted");

    fontUploaded.store(true, std::memory_order_release);
    DX12_DEBUG_STEP("UploadFontTextureIfNeeded", "Font upload: COMPLETE - fontUploaded=true");
    return true;
}

void DX12Backend::Render(const std::vector<DrawVertex>& vertices, const std::vector<uint16_t>& indices,
                         const std::vector<DrawCommand>& commands, int viewportWidth, int viewportHeight) {
    s_RenderCounter++;
    DX12_DEBUG_FRAME(s_RenderCounter, "Render: vertices=%zu, indices=%zu, commands=%zu, viewport=%dx%d",
                     vertices.size(), indices.size(), commands.size(), viewportWidth, viewportHeight);
    static std::atomic<int> s_renderImportantLogCount{0};
    const bool logThisRender = s_renderImportantLogCount.fetch_add(1, std::memory_order_relaxed) < 10;
    if (logThisRender) {
        HookLogImportant("DX12 Overlay: Backend render begin (verts=%zu, indices=%zu, commands=%zu, fontUploaded=%d)",
                         vertices.size(), indices.size(), commands.size(),
                         fontUploaded.load(std::memory_order_acquire) ? 1 : 0);
    }

    if (!initialized || !currentCmdList || vertices.empty()) {
        DX12_DEBUG_STEP("Render", "EARLY RETURN - initialized=%d, cmdList=%p, verts=%zu", initialized, currentCmdList,
                        vertices.size());
        if (logThisRender) {
            HookLogImportant("DX12 Overlay: Backend render skipped (initialized=%d, cmdList=%p, verts=%zu)",
                             initialized ? 1 : 0, currentCmdList, vertices.size());
        }
        return;
    }

    const bool hasTexturedCommands =
        std::any_of(commands.begin(), commands.end(), [](const DrawCommand& cmd) { return cmd.useTexture; });

    size_t vbSize = vertices.size() * sizeof(DrawVertex);
    // FFX owner-queue renderers pin the upload slot to the allocator/RTV slot whose own fence or inline marker
    // proved it reusable. The UI-resource baseline normally maps that to a replacement-buffer index; the embedded
    // final-batch route may select any completed slot while queue order serializes repeated target writes. Other
    // routes keep the normal atomic ring so concurrent calls receive distinct slots.
    const int forcedSlot = nextForcedUploadSlot.exchange(-1, std::memory_order_acq_rel);
    const bool needsInlineCompletion = forcedSlot < 0 && nextSlotFenceValue == 0;
    ComPtr<ID3D12GraphicsCommandList2> inlineList;
    if (needsInlineCompletion && FAILED(currentCmdList->QueryInterface(IID_PPV_ARGS(&inlineList)))) {
        static std::atomic<uint32_t> unsupported{0};
        if (unsupported.fetch_add(1, std::memory_order_relaxed) < 3)
            HookLogImportant("DX12 Overlay: callback command list lacks WriteBufferImmediate completion support");
        return;
    }
    const int slot = needsInlineCompletion ? AcquireInlineUploadSlot()
                     : forcedSlot >= 0 ? forcedSlot
                     : frameIdx.fetch_add(1, std::memory_order_relaxed) % kFramePoolSize;
    if (slot < 0)
        return;
    DX12_DEBUG_FRAME(s_RenderCounter, "Using buffer slot %d", slot);
    if (!needsInlineCompletion && !WaitForSlotGpuComplete(slot)) {
        return;
    }
    if (vbSize > vertexBufferSize[slot]) {
        DX12_DEBUG_STEP("Render", "Vertex buffer resize needed: %zu > %zu (slot=%d)", vbSize, vertexBufferSize[slot],
                        slot);
        if (!ResizeVertexBuffer(slot, vbSize)) {
            HookLog(
                "DX12 Overlay: Render - Failed to resize vertex buffer (needed "
                "%zu bytes)",
                vbSize);
            return;
        }
    }
    if (vertexBufferPtr[slot]) {
        memcpy(vertexBufferPtr[slot], vertices.data(), vbSize);
        DX12_DEBUG_FRAME(s_RenderCounter, "Vertex data copied: %zu bytes", vbSize);
    }

    size_t ibSize = indices.size() * sizeof(uint16_t);
    if (ibSize > indexBufferSize[slot]) {
        DX12_DEBUG_STEP("Render", "Index buffer resize needed: %zu > %zu (slot=%d)", ibSize, indexBufferSize[slot],
                        slot);
        if (!ResizeIndexBuffer(slot, ibSize)) {
            HookLog(
                "DX12 Overlay: Render - Failed to resize index buffer (needed "
                "%zu bytes)",
                ibSize);
            return;
        }
    }
    if (indexBufferPtr[slot]) {
        memcpy(indexBufferPtr[slot], indices.data(), ibSize);
        DX12_DEBUG_FRAME(s_RenderCounter, "Index data copied: %zu bytes", ibSize);
    }

    if (hasTexturedCommands) {
        if (!srvHeap) {
            static std::atomic<int> s_missingSrvLog{0};
            const int logN = s_missingSrvLog.fetch_add(1, std::memory_order_relaxed);
            if (logN < 10 || (logN % 200) == 0) {
                HookLogImportant("DX12 Overlay: textured draw skipped because font SRV heap is unavailable");
            }
            return;
        }

    }

    // All early allocation failures precede GPU references to backend resources.
    if (hasTexturedCommands && !UploadFontTextureIfNeeded(currentCmdList))
        return;

    DX12_DEBUG_FRAME(s_RenderCounter, "Setting pipeline state");
    currentCmdList->SetGraphicsRootSignature(rootSignature.Get());

    D3D12_GPU_DESCRIPTOR_HANDLE fontSrvGpuHandle = {};
    if (hasTexturedCommands) {
        ID3D12DescriptorHeap* heaps[] = {srvHeap.Get()};
        currentCmdList->SetDescriptorHeaps(1, heaps);
        fontSrvGpuHandle = srvHeap->GetGPUDescriptorHandleForHeapStart();
        currentCmdList->SetGraphicsRootDescriptorTable(1, fontSrvGpuHandle);
    }

    float constants[4] = {(float)viewportWidth, (float)viewportHeight, (float)hdrMode, paperWhiteNits};
    currentCmdList->SetGraphicsRoot32BitConstants(0, 4, constants, 0);

    currentCmdList->OMSetRenderTargets(1, &currentRTV, FALSE, nullptr);

    D3D12_VIEWPORT vp = {0, 0, (float)viewportWidth, (float)viewportHeight, 0, 1};
    D3D12_RECT scissor = {0, 0, (LONG)viewportWidth, (LONG)viewportHeight};
    currentCmdList->RSSetViewports(1, &vp);
    currentCmdList->RSSetScissorRects(1, &scissor);

    D3D12_VERTEX_BUFFER_VIEW vbv = {};
    vbv.BufferLocation = vertexBuffer[slot]->GetGPUVirtualAddress();
    vbv.SizeInBytes = (UINT)vbSize;
    vbv.StrideInBytes = sizeof(DrawVertex);
    currentCmdList->IASetVertexBuffers(0, 1, &vbv);

    D3D12_INDEX_BUFFER_VIEW ibv = {};
    ibv.BufferLocation = indexBuffer[slot]->GetGPUVirtualAddress();
    ibv.SizeInBytes = (UINT)ibSize;
    ibv.Format = DXGI_FORMAT_R16_UINT;
    currentCmdList->IASetIndexBuffer(&ibv);

    currentCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D12PipelineState* lastPSO = nullptr;
    int psoBindCount = 0;
    const DX12RenderProbeMode probeMode = GetDX12RenderProbeMode();
    int drawCallCount = 0;
    const bool useHdrTextShader = hdrMode > 0;
    {
        static std::atomic<int> s_commandDetailLog{0};
        const int logFrame = s_commandDetailLog.fetch_add(1, std::memory_order_relaxed);
        if (logFrame < 6) {
            HookLogImportant(
                "DX12 DIAG: Texture2D backend bindings frame=%d slot=%d srvGpu=0x%llX vbGpu=0x%llX ibGpu=0x%llX "
                "hdr=%d viewport=%dx%d",
                logFrame, slot, (unsigned long long)fontSrvGpuHandle.ptr, (unsigned long long)vbv.BufferLocation,
                (unsigned long long)ibv.BufferLocation, hdrMode, viewportWidth, viewportHeight);
            for (size_t cmdIndex = 0; cmdIndex < commands.size(); ++cmdIndex) {
                const auto& cmd = commands[cmdIndex];
                HookLogImportant(
                    "DX12 DIAG: Texture2D command frame=%d cmd=%zu textured=%d vtxOff=%u vtxCount=%u idxOff=%u "
                    "idxCount=%u",
                    logFrame, cmdIndex, cmd.useTexture ? 1 : 0, cmd.vertexOffset, cmd.vertexCount, cmd.indexOffset,
                    cmd.indexCount);
            }
        }
    }
    for (const auto& cmd : commands) {
        ID3D12PipelineState* pso = cmd.useTexture
                                       ? (useHdrTextShader ? pipelineState.Get() : pipelineStateTexturedSdr.Get())
                                       : pipelineStateSolid.Get();
        if (pso != lastPSO) {
            currentCmdList->SetPipelineState(pso);
            lastPSO = pso;
            ++psoBindCount;
        }
        if (probeMode == DX12RenderProbeMode::kStateSetupOnly) {
            continue;
        }
        currentCmdList->DrawIndexedInstanced(cmd.indexCount, 1, cmd.indexOffset, 0, 0);
        drawCallCount++;
    }
    if (needsInlineCompletion)
        MarkInlineUploadComplete(inlineList.Get(), slot);
    else
        slotFenceValue[slot] = nextSlotFenceValue;
    DX12_DEBUG_FRAME(s_RenderCounter, "Render complete: %d draw calls", drawCallCount);
    if (logThisRender) {
        if (probeMode == DX12RenderProbeMode::kStateSetupOnly) {
            HookLogImportant("DX12 Overlay: Backend state probe complete (drawCalls skipped, psoBinds=%d, slot=%d)",
                             psoBindCount, slot);
        } else {
            HookLogImportant("DX12 Overlay: Backend render complete (drawCalls=%d, slot=%d)", drawCallCount, slot);
        }
    }
}

}  // namespace CustomOverlay
