#include "dx11_hook_internal.h"

namespace {

// Applies the shared copy-query classification and reports the unusable-query
// case, which would otherwise silently look like permanent GPU busy-ness.
bool IsCaptureCopyQuerySlotReady(bool queryPresent, bool queryIssued, HRESULT queryHr, const char* apiTag,
                                 int32_t slot) {
    const CaptureCopyQuerySlotState state = ClassifyCaptureCopyQuerySlot(queryPresent, queryIssued, queryHr);
    if (state == CaptureCopyQuerySlotState::QueryUnusable) {
        static std::atomic<int> s_queryErrorLogCount{0};
        if (s_queryErrorLogCount.fetch_add(1, std::memory_order_relaxed) < 4) {
            HookLogImportant("%sCapture: Copy query for slot %d is unusable (hr=0x%08X); treating the slot as reusable",
                             apiTag, slot, queryHr);
        }
    }
    return state != CaptureCopyQuerySlotState::GpuBusy;
}

std::atomic<uint64_t> g_captureSlotStarvationStreak{0};

// Slot starvation used to fail silently, which made a permanently unavailable
// slot indistinguishable from "no frames yet" in the logs. Report the first
// occurrence and then only rare milestones so a real stall stays visible without
// adding hot-path noise.
void ReportCaptureSlotStarvation(const char* apiTag, uint32_t cpuBusySlots, uint32_t gpuBusySlots) {
    const uint64_t starved = g_captureSlotStarvationStreak.fetch_add(1, std::memory_order_relaxed) + 1;
    if (starved == 1 || starved == 60 || (starved % 600) == 0) {
        HookLogImportant("%sCapture: No capture texture slot available (consecutive=%llu cpuBusy=%u gpuBusy=%u)",
                         apiTag, static_cast<unsigned long long>(starved), cpuBusySlots, gpuBusySlots);
    }
}

void ResetCaptureSlotStarvation() {
    g_captureSlotStarvationStreak.store(0, std::memory_order_relaxed);
}

}  // namespace

bool DX11Capture::CaptureFrame(IDXGISwapChain* swapChain) {


        std::unique_lock<std::recursive_mutex> captureLock(captureMutex, std::try_to_lock);
        if (!captureLock.owns_lock()) {
            if (g_IPC && g_IPC->GetSharedMem()) {
                g_IPC->GetSharedMem()->runtimeState.injectProducerCaptureLockDrops.fetch_add(1,
                                                                                             std::memory_order_relaxed);
            }
            static std::atomic<int> s_contentionLogCount{0};
            if (s_contentionLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
                HookLog("DX11Capture: Skipping concurrent capture while another Present/cleanup owns resources");
            }
            return false;
        }
        if (HookIsShuttingDown())
            return false;

        static std::atomic<int> s_captureFrameCount{0};
        int frameNum = s_captureFrameCount.fetch_add(1, std::memory_order_relaxed) + 1;

        if (!swapChain) {
            HookLog("DX11Capture: [%d] swapChain is null", frameNum);
            return false;
        }

        // Check if we should throttle capture (encoder is falling behind)
        if (g_IPC && g_IPC->GetSharedMem()) {
            if (g_IPC->GetSharedMem()->throttleCapture.load(std::memory_order_acquire)) {
                return false;
            }
        }

        // Initialize capture if needed (GetDevice only called during init to avoid per-frame COM overhead)
        if (!initialized) {
            HookLog("DX11Capture: [%d] Not initialized, initializing...", frameNum);
            if (generationResetPending) {
                SharedMemoryLayout* sharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
                if (HasOutstandingCaptureFrameLeases(sharedMem)) {
                    static std::atomic<int> s_generationLeaseLogCount{0};
                    if (s_generationLeaseLogCount.fetch_add(1, std::memory_order_relaxed) < 12) {
                        HookLog("DX11Capture: [%d] Waiting for old frame leases before rebuilding resources", frameNum);
                    }
                    return false;
                }
                Cleanup();
            }
            const DXGIShared::APIType swapChainApi = DetectSwapChainAPITypeForDX11Hook(swapChain);
            if (swapChainApi == DXGIShared::APIType::D3D10) {
                if (!InitDX10(swapChain)) {
                    HookLog("DX11Capture: [%d] InitDX10 failed", frameNum);
                    return false;
                }
                Init(nullptr, swapChain);
            } else if (swapChainApi == DXGIShared::APIType::D3D11) {
                ID3D11Device* device = nullptr;
                HRESULT initHr = swapChain->GetDevice(IID_PPV_ARGS(&device));
                if (FAILED(initHr) || !device) {
                    HookLog("DX11Capture: [%d] GetDevice failed hr=0x%08X", frameNum, initHr);
                    return false;
                }
                Init(device, swapChain);
                device->Release();
            } else {
                HookLog("DX11Capture: [%d] Unsupported swapchain API %s during init", frameNum,
                        GetDX11HookBaseAPIName(swapChainApi));
                return false;
            }
        }

        if (!initialized) {
            HookLog("DX11Capture: [%d] Still not initialized after Init", frameNum);
            return false;
        }

        if (isDX10Mode) {
            if (!cachedDevice10) {
                HookLog("DX10Capture: [%d] cachedDevice10 is null", frameNum);
                return false;
            }

            ID3D10Texture2D* backbuffer10 = nullptr;
            UINT bufferIndex = ResolveDX11BackBufferIndex(swapChain);
            HRESULT hr = swapChain->GetBuffer(bufferIndex, __uuidof(ID3D10Texture2D), (void**)&backbuffer10);
            if (FAILED(hr) || !backbuffer10) {
                HookLog("DX10Capture: [%d] GetBuffer(%u) failed hr=0x%08X", frameNum, bufferIndex, hr);
                return false;
            }

            int writeIdx = writeIndex % CAPTURE_TEXTURE_COUNT;
            SharedMemoryLayout* captureSharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
            uint32_t cpuBusySlots = 0;
            uint32_t gpuBusySlots = 0;
            writeIdx = FindAvailableCaptureTextureSlotIf(
                captureSharedMem, writeIdx, CAPTURE_TEXTURE_COUNT,
                [&](int32_t candidate) {
                    ID3D10Query* query = copyQueries10[candidate];
                    const bool issued = query != nullptr && copyQueryIssued[candidate];
                    HRESULT queryHr = S_OK;
                    if (issued) {
                        BOOL complete = FALSE;
                        queryHr = query->GetData(&complete, sizeof(complete), D3D10_ASYNC_GETDATA_DONOTFLUSH);
                    }
                    return IsCaptureCopyQuerySlotReady(query != nullptr, issued, queryHr, "DX10", candidate);
                },
                &cpuBusySlots, &gpuBusySlots);
            if (writeIdx < 0) {
                ReportCaptureSlotStarvation("DX10", cpuBusySlots, gpuBusySlots);
                if (captureSharedMem) {
                    if (cpuBusySlots != 0)
                        captureSharedMem->runtimeState.injectProducerCpuLeaseBusyDrops.fetch_add(
                            1, std::memory_order_relaxed);
                    if (gpuBusySlots != 0)
                        captureSharedMem->runtimeState.injectProducerGpuBusyDrops.fetch_add(1,
                                                                                            std::memory_order_relaxed);
                }
                droppedFrames.fetch_add(1, std::memory_order_relaxed);
                backbuffer10->Release();
                return false;
            }
            ResetCaptureSlotStarvation();
            writeIndex.store(writeIdx, std::memory_order_relaxed);
            if (frameNum <= 20 || frameNum % 60 == 0) {
                HookLog("DX10Capture: [%d] Copying to texture %d (writeIndex=%d)", frameNum, writeIdx,
                        writeIndex.load());
            }

            LARGE_INTEGER qpc;
            QueryPerformanceCounter(&qpc);
            int64_t timestamp = qpc.QuadPart;

            cachedDevice10->CopyResource(sharedTextures10[writeIdx], backbuffer10);
            backbuffer10->Release();

            if (copyQueries10[writeIdx]) {
                copyQueries10[writeIdx]->End();
                copyQueryIssued[writeIdx] = true;
            }

            // D3D10->D3D11 shared-texture interop requires a producer-side Flush()
            // so the media process sees the latest contents of the shared surface.
            cachedDevice10->Flush();

            if (g_IPC) {
                SignalFrameReady(g_IPC, writeIdx, timestamp, 0);
                if (frameNum <= 10) {
                    HookLog("DX10Capture: [%d] Signaled frame ready via IPC (texture=%d)", frameNum, writeIdx);
                }
            } else {
                HookLog("DX10Capture: [%d] g_IPC is NULL - cannot signal frame", frameNum);
            }

            AdvanceWriteIndex();
            return true;
        }

        // Get immediate context for copy
        ID3D11DeviceContext* context = GetCaptureContext();
        if (!context) {
            HookLog("DX11Capture: [%d] GetCaptureContext returned null", frameNum);
            return false;
        }

        ID3D11Texture2D* backbuffer = nullptr;

        // When capture is intentionally ordered after overlay, prefer the RTV
        // resource that overlay rendered to on this frame.
        if (!isDX10Mode && dx11_hook_g_CaptureUsesOverlayRTV && dx11_hook_g_mainRenderTargetView) {
            ID3D11Resource* rtResource = nullptr;
            dx11_hook_g_mainRenderTargetView->GetResource(&rtResource);
            if (rtResource) {
                rtResource->QueryInterface(IID_PPV_ARGS(&backbuffer));
                rtResource->Release();
            }
        }

        // Fallback: resolve backbuffer index directly from swapchain.
        UINT bufferIndex = 0;
        if (!backbuffer) {
            bufferIndex = ResolveDX11BackBufferIndex(swapChain);
            HRESULT hr = swapChain->GetBuffer(bufferIndex, IID_PPV_ARGS(&backbuffer));
            if (FAILED(hr) || !backbuffer) {
                HookLog("DX11Capture: [%d] GetBuffer(%u) failed hr=0x%08X", frameNum, bufferIndex, hr);
                return false;
            }
        }

        // Determine which texture slot to write to
        int writeIdx = writeIndex % CAPTURE_TEXTURE_COUNT;
        SharedMemoryLayout* captureSharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        const UINT64 completedFenceValue = (useFences && fence) ? fence->GetCompletedValue() : 0;
        if (completedFenceValue == UINT64_MAX) {
            HookLog("DX11Capture: Producer fence reported device removal");
            backbuffer->Release();
            return false;
        }
        uint32_t cpuBusySlots = 0;
        uint32_t gpuBusySlots = 0;
        writeIdx = FindAvailableCaptureTextureSlotIf(
            captureSharedMem, writeIdx, CAPTURE_TEXTURE_COUNT,
            [&](int32_t candidate) {
                if (useFences && fence) {
                    const UINT64 requiredValue = slotFenceValues[candidate];
                    return requiredValue == 0 || completedFenceValue >= requiredValue;
                }
                ID3D11Query* query = copyQueries[candidate];
                const bool issued = query != nullptr && copyQueryIssued[candidate];
                HRESULT queryHr = S_OK;
                if (issued) {
                    BOOL complete = FALSE;
                    queryHr = context->GetData(query, &complete, sizeof(complete), D3D11_ASYNC_GETDATA_DONOTFLUSH);
                }
                return IsCaptureCopyQuerySlotReady(query != nullptr, issued, queryHr, "DX11", candidate);
            },
            &cpuBusySlots, &gpuBusySlots);
        if (writeIdx < 0) {
            ReportCaptureSlotStarvation("DX11", cpuBusySlots, gpuBusySlots);
            if (captureSharedMem) {
                if (cpuBusySlots != 0)
                    captureSharedMem->runtimeState.injectProducerCpuLeaseBusyDrops.fetch_add(1,
                                                                                             std::memory_order_relaxed);
                if (gpuBusySlots != 0)
                    captureSharedMem->runtimeState.injectProducerGpuBusyDrops.fetch_add(1, std::memory_order_relaxed);
            }
            droppedFrames.fetch_add(1, std::memory_order_relaxed);
            backbuffer->Release();
            return false;
        }
        ResetCaptureSlotStarvation();
        writeIndex.store(writeIdx, std::memory_order_relaxed);

        if (frameNum <= 20 || frameNum % 60 == 0) {
            HookLog("DX11Capture: [%d] Copying to texture %d (writeIndex=%d)", frameNum, writeIdx, writeIndex.load());
        }

        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        int64_t timestamp = qpc.QuadPart;

        // Perform GPU copy: backbuffer -> shared texture
        // For DXVK: copy into the DXVK-imported texture (system D3D11-owned,
        // imported into DXVK's device). The encoder opens the system D3D11 NT handle.
        ID3D11Texture2D* copyTarget =
            (isDXVKMode && dxvkImportedTextures[writeIdx]) ? dxvkImportedTextures[writeIdx] : sharedTextures[writeIdx];
        context->CopyResource(copyTarget, backbuffer);
        backbuffer->Release();

        // Issue query for GPU completion tracking
        if (copyQueries[writeIdx]) {
            context->End(copyQueries[writeIdx]);
            copyQueryIssued[writeIdx] = true;
        }

        // Signal fence if using D3D11.3 fences
        uint64_t currentFenceValue = 0;
        if (useFences && fence && context4) {
            currentFenceValue = ++fenceValue;
            const HRESULT signalHr = context4->Signal(fence, currentFenceValue);
            if (FAILED(signalHr)) {
                HookLog(
                    "DX11Capture: Fence Signal failed value=%llu hr=0x%08X; falling back to implicit shared "
                    "synchronization",
                    static_cast<unsigned long long>(currentFenceValue), signalHr);
                useFences = false;
                currentFenceValue = 0;
                context->Flush();
                if (cachedDevice && FAILED(cachedDevice->GetDeviceRemovedReason())) {
                    return false;
                }
            } else {
                slotFenceValues[writeIdx] = currentFenceValue;
            }
        } else {
            // A legacy shared texture has no explicit cross-process completion
            // primitive. Submit the copy before publishing its ring entry so the
            // media device cannot indefinitely observe an older texture version.
            context->Flush();
        }

        // Signal frame ready to media process via IPC
        // Note: EnqueueFrame to internal pendingRing is skipped for inject mode
        // since SignalFrameReady writes directly to the shared memory ring buffer
        if (g_IPC) {
            SignalFrameReady(g_IPC, writeIdx, timestamp, currentFenceValue);
            if (frameNum <= 10) {
                HookLog("DX11Capture: [%d] Signaled frame ready via IPC (texture=%d)", frameNum, writeIdx);
            }
        } else {
            HookLog("DX11Capture: [%d] g_IPC is NULL - cannot signal frame", frameNum);
        }

        // Advance write index
        AdvanceWriteIndex();

        return true;

}
