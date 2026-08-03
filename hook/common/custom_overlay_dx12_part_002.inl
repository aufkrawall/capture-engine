
    HookLog("DX12 Overlay: Vertex buffer[%d] resized to %zu bytes", slot, newSize);
    return true;
}

bool DX12Backend::ResizeIndexBuffer(int slot, size_t requiredBytes) {
    DX12_DEBUG_STEP("ResizeIndexBuffer", "START - slot=%d, required=%zu, current=%zu", slot, requiredBytes,
                    indexBufferSize[slot]);

    if (!device) {
        DX12_DEBUG_STEP("ResizeIndexBuffer", "FAILED - no device");
        return false;
    }

    if (indexBuffer[slot] && indexBufferPtr[slot]) {
        DX12_DEBUG_STEP("ResizeIndexBuffer", "Unmapping old index buffer[%d]", slot);
        indexBuffer[slot]->Unmap(0, nullptr);
        indexBufferPtr[slot] = nullptr;
    }

    size_t newSize = indexBufferSize[slot] * 2;
    while (newSize < requiredBytes) {
        newSize *= 2;
    }
    DX12_DEBUG_STEP("ResizeIndexBuffer", "New size: %zu bytes (old=%zu, slot=%d)", newSize, indexBufferSize[slot],
                    slot);

    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = newSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> newBuffer;
    HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&newBuffer));
    DX12_DEBUG_STEP("ResizeIndexBuffer", "Create result: hr=0x%08X (%s)", hr, SUCCEEDED(hr) ? "OK" : "FAILED");
    if (FAILED(hr)) {
        HookLog(
            "DX12 Overlay: ResizeIndexBuffer - Failed to create new buffer "
            "(slot=%d, size=%zu), hr=0x%08X",
            slot, newSize, hr);
        return false;
    }

    indexBuffer[slot] = newBuffer;
    indexBufferSize[slot] = newSize;

    D3D12_RANGE readRange = {0, 0};
    indexBuffer[slot]->Map(0, &readRange, &indexBufferPtr[slot]);
    DX12_DEBUG_STEP("ResizeIndexBuffer", "SUCCESS - new buffer[%d] mapped at %p", slot, indexBufferPtr[slot]);

    HookLog("DX12 Overlay: Index buffer[%d] resized to %zu bytes", slot, newSize);
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

    if (hasTexturedCommands && !UploadFontTextureIfNeeded(currentCmdList)) {
        DX12_DEBUG_STEP("Render", "EARLY RETURN - deferred font upload failed");
        if (logThisRender) {
            HookLogImportant("DX12 Overlay: Backend render aborted because deferred font upload failed");
        }
        return;
    }

    size_t vbSize = vertices.size() * sizeof(DrawVertex);
    // FFX proxy rendering pins the upload slot to the current swapchain-buffer index. Reuse of that index is
    // AMD's proof that all prior work targeting the replacement buffer completed. Other routes keep the normal
    // atomic ring so concurrent calls receive distinct slots.
    const int forcedSlot = nextForcedUploadSlot.exchange(-1, std::memory_order_acq_rel);
    int slot = forcedSlot >= 0 ? forcedSlot : frameIdx.fetch_add(1, std::memory_order_relaxed) % kFramePoolSize;
    DX12_DEBUG_FRAME(s_RenderCounter, "Using buffer slot %d", slot);
    if (!WaitForSlotGpuComplete(slot)) {
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

    DX12_DEBUG_FRAME(s_RenderCounter, "Setting pipeline state");
    currentCmdList->SetGraphicsRootSignature(rootSignature.Get());

    D3D12_GPU_DESCRIPTOR_HANDLE fontSrvGpuHandle = {};
    if (hasTexturedCommands) {
        if (!srvHeap) {
            static std::atomic<int> s_missingSrvLog{0};
            const int logN = s_missingSrvLog.fetch_add(1, std::memory_order_relaxed);
            if (logN < 10 || (logN % 200) == 0) {
                HookLogImportant("DX12 Overlay: textured draw skipped because font SRV heap is unavailable");
            }
            return;
        }

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
