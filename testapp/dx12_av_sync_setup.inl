// Extracted from dx12_av_sync_test.cpp to stay under the AGENTS.md
// size ceiling. Included at exactly the point these definitions used to sit,
// so declaration order is unchanged.

int ClampInt(int value, int lo, int hi) {
    return std::max(lo, std::min(value, hi));
}

void LoadConfig() {
    wchar_t modulePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::wstring configPath = modulePath;
    const size_t slash = configPath.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        configPath = configPath.substr(0, slash + 1) + L"testappconfig.ini";
    }

    g_WindowWidth = GetPrivateProfileIntW(L"Display", L"width", g_WindowWidth, configPath.c_str());
    g_WindowHeight = GetPrivateProfileIntW(L"Display", L"height", g_WindowHeight, configPath.c_str());
    g_Fullscreen = GetPrivateProfileIntW(L"Display", L"fullscreen", g_Fullscreen, configPath.c_str());
    g_WindowChrome = GetPrivateProfileIntW(L"Display", L"window_chrome", g_WindowChrome, configPath.c_str());
    g_Topmost = GetPrivateProfileIntW(L"Display", L"topmost", g_Topmost, configPath.c_str());
    g_VSync = GetPrivateProfileIntW(L"Rendering", L"vsync", g_VSync, configPath.c_str());
    g_TearingRequested =
        GetPrivateProfileIntW(L"Rendering", L"allow_tearing", g_TearingRequested ? 1 : 0, configPath.c_str()) != 0;
    g_GpuLoadPasses = GetPrivateProfileIntW(L"Performance", L"gpu_load", g_GpuLoadPasses, configPath.c_str());
    g_EncoderStressScene = GetPrivateProfileIntW(L"Performance", L"encoder_stress_scene", g_EncoderStressScene ? 1 : 0,
                                                 configPath.c_str()) != 0;
    g_TargetFps = ClampInt(GetPrivateProfileIntW(L"AVSync", L"fps", g_TargetFps, configPath.c_str()), 1, 480);
    g_DurationSeconds =
        ClampInt(GetPrivateProfileIntW(L"AVSync", L"duration_seconds", g_DurationSeconds, configPath.c_str()), 1, 3600);
    g_LogEveryFrame = GetPrivateProfileIntW(L"AVSync", L"log_every_frame", 0, configPath.c_str()) != 0;
    g_AudioEnabled = GetPrivateProfileIntW(L"AVSync", L"audio", 1, configPath.c_str()) != 0;
    g_AudioClockScheduling = GetPrivateProfileIntW(L"AVSync", L"audio_clock_scheduling", 0, configPath.c_str()) != 0;
    g_AudioBufferMs = testapp::avsync::ClampAudioBufferMs(
        GetPrivateProfileIntW(L"AVSync", L"audio_buffer_ms", g_AudioBufferMs, configPath.c_str()));
    wchar_t audioLeadText[64] = {};
    GetPrivateProfileStringW(L"AVSync", L"audio_lead_ms", L"", audioLeadText,
                             static_cast<DWORD>(sizeof(audioLeadText) / sizeof(audioLeadText[0])), configPath.c_str());
    if (audioLeadText[0] != L'\0') {
        wchar_t* end = nullptr;
        const double parsed = std::wcstod(audioLeadText, &end);
        if (end && end != audioLeadText && std::isfinite(parsed)) {
            g_AudioLeadMs = testapp::avsync::ClampAudioLeadMs(parsed);
        }
    }
    wchar_t analysisStartText[64] = {};
    GetPrivateProfileStringW(L"AVSync", L"analysis_start_seconds", L"", analysisStartText,
                             static_cast<DWORD>(sizeof(analysisStartText) / sizeof(analysisStartText[0])),
                             configPath.c_str());
    if (analysisStartText[0] != L'\0') {
        wchar_t* end = nullptr;
        const double parsed = std::wcstod(analysisStartText, &end);
        if (end && end != analysisStartText && std::isfinite(parsed)) {
            g_AnalysisStartSeconds = parsed;
        }
    }
}

void ParseArgs(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        auto readInt = [&](const char* separate, const char* prefix, int* out) {
            if (strcmp(argv[i], separate) == 0 && i + 1 < argc) {
                *out = atoi(argv[++i]);
                return true;
            }
            const size_t prefixLen = strlen(prefix);
            if (strncmp(argv[i], prefix, prefixLen) == 0) {
                *out = atoi(argv[i] + prefixLen);
                return true;
            }
            return false;
        };
        auto readDouble = [&](const char* separate, const char* prefix, double* out) {
            if (strcmp(argv[i], separate) == 0 && i + 1 < argc) {
                *out = std::strtod(argv[++i], nullptr);
                return true;
            }
            const size_t prefixLen = strlen(prefix);
            if (strncmp(argv[i], prefix, prefixLen) == 0) {
                *out = std::strtod(argv[i] + prefixLen, nullptr);
                return true;
            }
            return false;
        };

        if (readInt("--width", "--width=", &g_WindowWidth) || readInt("--height", "--height=", &g_WindowHeight) ||
            readInt("--fps", "--fps=", &g_TargetFps) || readInt("--duration", "--duration=", &g_DurationSeconds) ||
            readInt("--gpu-load", "--gpu-load=", &g_GpuLoadPasses) || readInt("--vsync", "--vsync=", &g_VSync) ||
            readInt("--fullscreen", "--fullscreen=", &g_Fullscreen) ||
            readInt("--window-chrome", "--window-chrome=", &g_WindowChrome) ||
            readInt("--topmost", "--topmost=", &g_Topmost) ||
            readInt("--audio-buffer-ms", "--audio-buffer-ms=", &g_AudioBufferMs)) {
            continue;
        }
        if (readDouble("--audio-lead-ms", "--audio-lead-ms=", &g_AudioLeadMs)) {
            continue;
        }
        if (readDouble("--analysis-start-sec", "--analysis-start-sec=", &g_AnalysisStartSeconds) ||
            readDouble("--analysis-start-seconds", "--analysis-start-seconds=", &g_AnalysisStartSeconds)) {
            continue;
        }
        if (strcmp(argv[i], "--log-every-frame") == 0) {
            g_LogEveryFrame = true;
        } else if (strcmp(argv[i], "--encoder-stress-scene") == 0) {
            g_EncoderStressScene = true;
        } else if (strcmp(argv[i], "--no-encoder-stress-scene") == 0) {
            g_EncoderStressScene = false;
        } else if (strcmp(argv[i], "--allow-tearing") == 0) {
            g_TearingRequested = true;
        } else if (strcmp(argv[i], "--no-allow-tearing") == 0) {
            g_TearingRequested = false;
        } else if (strcmp(argv[i], "--no-audio") == 0) {
            g_AudioEnabled = false;
        } else if (strcmp(argv[i], "--audio-clock-scheduling") == 0) {
            g_AudioClockScheduling = true;
        } else if (strcmp(argv[i], "--source-stall") == 0 && i + 1 < argc) {
            testapp::avsync::SourceStallSpec spec;
            if (testapp::avsync::ParseSourceStallSpec(argv[++i], &spec)) {
                g_SourceStalls.push_back({spec});
            } else {
                testapp::Log("AVSYNC WARNING invalid source stall spec=%s\n", argv[i]);
            }
        } else if (strncmp(argv[i], "--source-stall=", 15) == 0) {
            testapp::avsync::SourceStallSpec spec;
            if (testapp::avsync::ParseSourceStallSpec(argv[i] + 15, &spec)) {
                g_SourceStalls.push_back({spec});
            } else {
                testapp::Log("AVSYNC WARNING invalid source stall spec=%s\n", argv[i] + 15);
            }
        }
    }

    g_WindowWidth = ClampInt(g_WindowWidth, 320, 7680);
    g_WindowHeight = ClampInt(g_WindowHeight, 240, 4320);
    g_TargetFps = ClampInt(g_TargetFps, 1, 480);
    g_DurationSeconds = ClampInt(g_DurationSeconds, 1, 3600);
    g_GpuLoadPasses = ClampInt(g_GpuLoadPasses, 0, 10000);
    g_Fullscreen = g_Fullscreen ? 1 : 0;
    g_WindowChrome = g_WindowChrome ? 1 : 0;
    g_Topmost = g_Topmost ? 1 : 0;
    g_AudioBufferMs = testapp::avsync::ClampAudioBufferMs(g_AudioBufferMs);
    g_AudioLeadMs = testapp::avsync::ClampAudioLeadMs(g_AudioLeadMs);
    g_AnalysisStartSeconds =
        testapp::avsync::ClampAnalysisStartSeconds(g_AnalysisStartSeconds, static_cast<double>(g_DurationSeconds));
    std::sort(g_SourceStalls.begin(), g_SourceStalls.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.spec.startSeconds < rhs.spec.startSeconds; });
}

testapp::avsync::AudioRenderer g_Audio(&g_QpcFreq, &g_StimulusStartQpc);

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                SetCursor(nullptr);
                return TRUE;
            }
            break;
        case WM_DESTROY:
            g_Running = false;
            PostQuitMessage(0);
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                g_Running = false;
                DestroyWindow(hwnd);
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void WaitForGpu() {
    std::lock_guard<std::mutex> lock(g_FrameSyncMutex);
    const UINT64 fenceValue = g_FenceValues[g_FrameIndex];
    g_CommandQueue->Signal(g_Fence.Get(), fenceValue);
    if (g_Fence->GetCompletedValue() < fenceValue) {
        g_Fence->SetEventOnCompletion(fenceValue, g_FenceEvent);
        WaitForSingleObject(g_FenceEvent, INFINITE);
    }
    ++g_FenceValues[g_FrameIndex];
}

void MoveToNextFrame() {
    std::lock_guard<std::mutex> lock(g_FrameSyncMutex);
    const UINT64 currentFenceValue = g_FenceValues[g_FrameIndex];
    g_CommandQueue->Signal(g_Fence.Get(), currentFenceValue);
    const UINT nextFrameIndex = g_SwapChain->GetCurrentBackBufferIndex();
    if (g_Fence->GetCompletedValue() < g_FenceValues[nextFrameIndex]) {
        g_Fence->SetEventOnCompletion(g_FenceValues[nextFrameIndex], g_FenceEvent);
        WaitForSingleObject(g_FenceEvent, INFINITE);
    }
    g_FrameIndex = nextFrameIndex;
    g_FenceValues[g_FrameIndex] = currentFenceValue + 1;
}

void RecordPresentTiming(const LARGE_INTEGER& presentQpc, int eventIndex) {
    if (g_LastPresentQpc.QuadPart != 0) {
        const double deltaMs = QpcToSeconds(presentQpc.QuadPart - g_LastPresentQpc.QuadPart) * 1000.0;
        g_LastPresentDeltaMs = deltaMs;
        g_MaxPresentDeltaMs = std::max(g_MaxPresentDeltaMs, deltaMs);
        g_PresentDeltaSumMs += deltaMs;
        ++g_PresentDeltaCount;

        const bool eventBoundary =
            g_LastPresentEventIndex != -9999 && eventIndex >= 0 && eventIndex != g_LastPresentEventIndex;
        const double thresholdMs = FramePacingSpikeThresholdMs();
        if (deltaMs > thresholdMs) {
            const double stimulusSeconds = SecondsSinceStimulusStart();
            const bool strictAnalysisWindow = stimulusSeconds >= g_AnalysisStartSeconds;
            if (g_PendingPlannedPresentGap) {
                ++g_PlannedPresentGapCount;
                testapp::Log(
                    "AVSYNC FRAME_TIMING planned_source_gap frameId=%llu deltaMs=%.3f thresholdMs=%.3f "
                    "eventBoundary=%d previousEvent=%d event=%d\n",
                    static_cast<unsigned long long>(g_FrameId), deltaMs, thresholdMs, eventBoundary ? 1 : 0,
                    g_LastPresentEventIndex, eventIndex);
            } else if (!strictAnalysisWindow) {
                ++g_WarmupPacingSpikeCount;
                testapp::Log(
                    "AVSYNC FRAME_TIMING warmup_spike frameId=%llu deltaMs=%.3f thresholdMs=%.3f "
                    "eventBoundary=%d previousEvent=%d event=%d stimulusSeconds=%.6f analysisStart=%.6f\n",
                    static_cast<unsigned long long>(g_FrameId), deltaMs, thresholdMs, eventBoundary ? 1 : 0,
                    g_LastPresentEventIndex, eventIndex, stimulusSeconds, g_AnalysisStartSeconds);
            } else {
                ++g_FramePacingSpikeCount;
                if (eventBoundary) {
                    ++g_EventBoundaryPacingSpikeCount;
                }
                testapp::Log(
                    "AVSYNC WARNING frame pacing spike frameId=%llu deltaMs=%.3f thresholdMs=%.3f "
                    "eventBoundary=%d previousEvent=%d event=%d stimulusSeconds=%.6f\n",
                    static_cast<unsigned long long>(g_FrameId), deltaMs, thresholdMs, eventBoundary ? 1 : 0,
                    g_LastPresentEventIndex, eventIndex, stimulusSeconds);
            }
        }
    }
    g_LastPresentQpc = presentQpc;
    g_LastPresentEventIndex = eventIndex;
    g_PendingPlannedPresentGap = false;
}

bool InitDX12(HWND hwnd) {
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        testapp::Log("AVSYNC WARNING CreateDXGIFactory1 failed\n");
        return false;
    }

    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory.As(&factory5))) {
        BOOL allowTearing = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing,
                                                    sizeof(allowTearing)))) {
            g_TearingSupported = allowTearing != FALSE;
        }
    }
    g_TearingActive = testapp::avsync::ShouldUseDxgiTearing(g_TearingRequested, g_TearingSupported, g_VSync != 0);

    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_Device)))) {
        testapp::Log("AVSYNC WARNING D3D12CreateDevice failed\n");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g_Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_CommandQueue)))) {
        testapp::Log("AVSYNC WARNING CreateCommandQueue failed\n");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.BufferCount = kFrameCount;
    scDesc.Width = static_cast<UINT>(g_WindowWidth);
    scDesc.Height = static_cast<UINT>(g_WindowHeight);
    scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.SampleDesc.Count = 1;
    scDesc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (g_TearingActive) {
        scDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    ComPtr<IDXGISwapChain1> swapChain1;
    if (FAILED(factory->CreateSwapChainForHwnd(g_CommandQueue.Get(), hwnd, &scDesc, nullptr, nullptr, &swapChain1))) {
        testapp::Log("AVSYNC WARNING CreateSwapChainForHwnd failed\n");
        return false;
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    swapChain1.As(&g_SwapChain);
    g_FrameIndex = g_SwapChain->GetCurrentBackBufferIndex();

    ComPtr<IDXGISwapChain2> swapChain2;
    if (SUCCEEDED(swapChain1.As(&swapChain2))) {
        swapChain2->SetMaximumFrameLatency(1);
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = kFrameCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    g_Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_RtvHeap));
    g_RtvDescriptorSize = g_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; ++i) {
        g_SwapChain->GetBuffer(i, IID_PPV_ARGS(&g_RenderTargets[i]));
        g_Device->CreateRenderTargetView(g_RenderTargets[i].Get(), nullptr, rtv);
        rtv.ptr += g_RtvDescriptorSize;
        g_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_CommandAllocators[i]));
    }

    g_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_CommandAllocators[g_FrameIndex].Get(), nullptr,
                                IID_PPV_ARGS(&g_CommandList));
    g_CommandList->Close();
    g_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_Fence));
    ++g_FenceValues[g_FrameIndex];
    g_FenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    testapp::Log(
        "AVSYNC START dx12 width=%d height=%d vsync=%d tearingRequested=%d tearingSupported=%d tearingActive=%d\n",
        g_WindowWidth, g_WindowHeight, g_VSync, g_TearingRequested ? 1 : 0, g_TearingSupported ? 1 : 0,
        g_TearingActive ? 1 : 0);
    return true;
}

