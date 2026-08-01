// Extracted from dx12_av_sync_test.cpp to stay under the AGENTS.md
// size ceiling. Included at exactly the point these definitions used to sit,
// so declaration order is unchanged.

void ClearRect(D3D12_CPU_DESCRIPTOR_HANDLE rtv, LONG left, LONG top, LONG right, LONG bottom, const float color[4]) {
    D3D12_RECT rect = {left, top, right, bottom};
    g_CommandList->ClearRenderTargetView(rtv, color, 1, &rect);
}

void DrawMarkerTiles(D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint16_t marker, bool inverse) {
    const float one[] = {0.94f, 0.94f, 0.94f, 1.0f};
    const float zero[] = {0.02f, 0.02f, 0.02f, 1.0f};
    const LONG top = inverse ? kMarkerMargin + kMarkerTile + kMarkerGap : kMarkerMargin;
    for (int bit = 0; bit < testapp::avsync::kFrameMarkerBits; ++bit) {
        const bool set = testapp::avsync::FrameMarkerBit(marker, bit) ^ inverse;
        const LONG left = kMarkerMargin + bit * (kMarkerTile + kMarkerGap);
        ClearRect(rtv, left, top, left + kMarkerTile, top + kMarkerTile, set ? one : zero);
    }
}

void DrawMotionRuler(D3D12_CPU_DESCRIPTOR_HANDLE rtv, LONG laneLeft, LONG laneTop, LONG laneRight, LONG laneBottom) {
    const float tickColor[] = {0.24f, 0.24f, 0.24f, 1.0f};
    const LONG usable = std::max<LONG>(1, laneRight - laneLeft);
    for (int tick = 1; tick < 8; ++tick) {
        const LONG x = laneLeft + static_cast<LONG>((static_cast<double>(usable) * tick) / 8.0);
        ClearRect(rtv, x, laneTop + 3, x + 2, laneBottom - 3, tickColor);
    }
}

void DrawMotionLaneAt(D3D12_CPU_DESCRIPTOR_HANDLE rtv, double position, double topRatio, LONG barWidth,
                      LONG laneHeight) {
    const LONG laneLeft = kMarkerMargin;
    const LONG laneRight = g_WindowWidth - kMarkerMargin;
    const LONG laneTop = static_cast<LONG>(g_WindowHeight * topRatio);
    const LONG laneBottom = laneTop + laneHeight;
    const float laneColor[] = {0.015f, 0.015f, 0.015f, 1.0f};
    const float barColor[] = {1.0f, 1.0f, 1.0f, 1.0f};
    ClearRect(rtv, laneLeft, laneTop, laneRight, laneBottom, laneColor);
    DrawMotionRuler(rtv, laneLeft, laneTop, laneRight, laneBottom);
    const LONG usable = std::max<LONG>(1, laneRight - laneLeft - barWidth);
    const LONG x = laneLeft + static_cast<LONG>(position * static_cast<double>(usable));
    ClearRect(rtv, x, laneTop + 7, x + barWidth, laneBottom - 7, barColor);
}

void DrawMotionLane(D3D12_CPU_DESCRIPTOR_HANDLE rtv, double stimulusSeconds) {
    DrawMotionLaneAt(rtv, testapp::avsync::SmoothLanePosition(stimulusSeconds), 0.72, 96, 42);
    DrawMotionLaneAt(rtv, testapp::avsync::FastLanePosition(stimulusSeconds), 0.82, 48, 42);
}

void DrawCorruptionSentinels(D3D12_CPU_DESCRIPTOR_HANDLE rtv, const testapp::avsync::StimulusState& state,
                             uint16_t marker) {
    const float white[] = {0.96f, 0.96f, 0.96f, 1.0f};
    const float black[] = {0.02f, 0.02f, 0.02f, 1.0f};
    const LONG size = 28;
    const LONG startX = g_WindowWidth - kMarkerMargin - 8 * (size + 4);
    const LONG startY = g_WindowHeight - kMarkerMargin - size;
    const uint8_t checksum = testapp::avsync::FrameMarkerChecksum(marker, state.paletteIndex);
    for (int bit = 0; bit < 8; ++bit) {
        const bool set = ((checksum >> bit) & 1u) != 0;
        const LONG x = startX + bit * (size + 4);
        ClearRect(rtv, x, startY, x + size, startY + size, set ? white : black);
    }
    const LONG parityX = startX - size - 12;
    const bool parity = testapp::avsync::FrameMarkerParity(marker, state.paletteIndex);
    ClearRect(rtv, parityX, startY, parityX + size, startY + size, parity ? white : black);
}

void DrawEncoderStressScene(D3D12_CPU_DESCRIPTOR_HANDLE rtv, const testapp::avsync::StimulusState& state,
                            uint64_t frameId) {
    if (!g_EncoderStressScene || state.eventIndex < 0) {
        return;
    }

    const auto layout = testapp::avsync::ComputeEncoderStressLayout(g_WindowWidth, g_WindowHeight, kMarkerMargin,
                                                                    kMarkerTile, kMarkerGap);
    if (!layout.valid) {
        return;
    }

    for (LONG y = layout.top; y < layout.bottom; y += layout.tile) {
        for (LONG x = layout.left; x < layout.right; x += layout.tile) {
            if (x < layout.reserveRight && x + layout.tile > layout.reserveLeft && y < layout.reserveBottom &&
                y + layout.tile > layout.reserveTop) {
                continue;
            }
            const uint32_t tileX = static_cast<uint32_t>((x - layout.left) / layout.tile);
            const uint32_t tileY = static_cast<uint32_t>((y - layout.top) / layout.tile);
            const uint32_t hash = testapp::avsync::EncoderStressTileHash(tileX, tileY, frameId, state.paletteIndex);
            const float color[] = {
                0.08f + static_cast<float>(hash & 0xffu) * (0.84f / 255.0f),
                0.08f + static_cast<float>((hash >> 8) & 0xffu) * (0.84f / 255.0f),
                0.08f + static_cast<float>((hash >> 16) & 0xffu) * (0.84f / 255.0f),
                1.0f,
            };
            ClearRect(rtv, x, y, x + layout.tile - 2, y + layout.tile - 2, color);
        }
    }
}

void RenderFrame() {
    const double stimulusSeconds = SecondsSinceStimulusStart();
    const auto state = testapp::avsync::StateAt(stimulusSeconds);
    const uint16_t marker = testapp::avsync::EncodeFrameMarker(g_FrameId);

    UINT frameIndex = 0;
    {
        std::lock_guard<std::mutex> lock(g_FrameSyncMutex);
        frameIndex = g_FrameIndex;
    }

    g_CommandAllocators[frameIndex]->Reset();
    g_CommandList->Reset(g_CommandAllocators[frameIndex].Get(), nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = g_RenderTargets[frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_CommandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += frameIndex * g_RtvDescriptorSize;

    const float bg[] = {state.color.r / 255.0f, state.color.g / 255.0f, state.color.b / 255.0f, 1.0f};
    const float preStart[] = {0.0f, 0.0f, 0.0f, 1.0f};
    for (int pass = 0; pass < g_GpuLoadPasses; ++pass) {
        const float load[] = {static_cast<float>((pass & 7) + 1) / 64.0f, 0.02f, 0.03f, 1.0f};
        g_CommandList->ClearRenderTargetView(rtv, load, 0, nullptr);
    }
    g_CommandList->ClearRenderTargetView(rtv, state.eventIndex < 0 ? preStart : bg, 0, nullptr);
    DrawEncoderStressScene(rtv, state, g_FrameId);
    DrawMarkerTiles(rtv, marker, false);
    DrawMarkerTiles(rtv, marker, true);
    DrawMotionLane(rtv, stimulusSeconds);
    DrawCorruptionSentinels(rtv, state, marker);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_CommandList->ResourceBarrier(1, &barrier);
    g_CommandList->Close();

    ID3D12CommandList* lists[] = {g_CommandList.Get()};
    g_CommandQueue->ExecuteCommandLists(1, lists);
    const UINT sync = g_VSync ? 1u : 0u;
    const UINT flags = g_TearingActive ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    const HRESULT presentHr = g_SwapChain->Present(sync, flags);
    if (FAILED(presentHr)) {
        testapp::Log("AVSYNC WARNING Present failed frameId=%llu hr=0x%08lx\n",
                     static_cast<unsigned long long>(g_FrameId), static_cast<unsigned long>(presentHr));
        g_Running = false;
    }
    const LARGE_INTEGER presentQpc = QueryQpc();
    RecordPresentTiming(presentQpc, state.eventIndex);
    MoveToNextFrame();

    if (g_LogEveryFrame || (g_FrameId % static_cast<uint64_t>(std::max(1, g_TargetFps)) == 0)) {
        testapp::Log(
            "AVSYNC FRAME frameId=%llu marker=%u event=%d palette=%d stimulusSeconds=%.6f "
            "motion=%.6f expectedMotion=%.6f fastMotion=%.6f frameIndex=%u presentDeltaMs=%.3f "
            "maxPresentDeltaMs=%.3f\n",
            static_cast<unsigned long long>(g_FrameId), marker, state.eventIndex, state.paletteIndex, stimulusSeconds,
            testapp::avsync::SmoothLanePosition(stimulusSeconds),
            testapp::avsync::ExpectedMotionPosition(stimulusSeconds),
            testapp::avsync::FastLanePosition(stimulusSeconds), frameIndex, g_LastPresentDeltaMs, g_MaxPresentDeltaMs);
    }
    ++g_FrameId;
}
