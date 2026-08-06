#include "dx12_hook_internal.h"
#include "dx12_hook_process_session.h"

ProcessFrameFlow FrameProcessSession::DrawSubmitCoreTail() {
                        if (FAILED(closeHr)) {
                            HookLog("DX12: list->Close failed hr=0x%08X, forcing reinit", closeHr);
                            dx12_hook_g_State.syncInit = false;
                        } else {
                            // Choose submit queue: dedicated overlay queue when
                            // available (SL FG active), otherwise game queue.
                            bool useDedicated = dx12_hook_g_State.overlayQueue && ShouldUseDedicatedOverlayQueue();
                            ID3D12CommandQueue* eclQueue = useDedicated ? dx12_hook_g_State.overlayQueue : gameQueue;

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
                                            (unsigned)preEclDevHr, eclQueue, (void*)dx12_hook_g_RealD3D12ECL.load(),
                                            (void*)origECL, GetCurrentThreadId());
                                        dx12_hook_g_DeviceRemoved.store(true, std::memory_order_release);
return ProcessFrameFlow::kOverlayDone;
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
                            ExecuteCommandListsPtr realECL = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
                            bool usedRealECL = false;
                            const bool classifyFocusLossSubmitPath =
                                dx12_hook_s_WrappedPresentFocusLossContext.valid && !processHasForeground;
                            bool submitPathIsD3D12Module = false;
                            bool isSLWrapperECL =
                                dx12_hook_g_HadFSRFGPhase && slFGActive &&
                                eclQueue == g_CommandQueue.load(std::memory_order_acquire) &&
                                eclQueue != dx12_hook_g_OriginalGameQueue;

                            // Log ECL path decision for first N frames per reinit
                            {
                                static int s_eclPathLogCount = 0;
                                if (dx12_hook_g_ResetReinitSubmitCounter.load(std::memory_order_relaxed))
                                    s_eclPathLogCount = 0;
                                if (s_eclPathLogCount < 3) {
                                    s_eclPathLogCount++;
                                    const char* path = (!useDedicated && realECL && !isSLWrapperECL)
                                                           ? "realECL"
                                                       : origECL ? "origECL"
                                                                 : "vtable";
                                    const bool pathIsD3D12Module =
                                        (!useDedicated && realECL && !isSLWrapperECL)
                                            ? true
                                            : (origECL
                                                   ? IsD3D12ModuleAddress(reinterpret_cast<void*>(origECL))
                                                   : false);
                                    HookLogImportant(
                                        "DX12: ECL path=%s (eclQ=%p realECL=%p origECL=%p "
                                        "dedicated=%d slWrapper=%d directD3D12=%d scQ=%p origGame=%p)",
                                        path, eclQueue, (void*)realECL, (void*)origECL,
                                        useDedicated ? 1 : 0, isSLWrapperECL ? 1 : 0,
                                        pathIsD3D12Module ? 1 : 0, dx12_hook_g_SwapchainQueue, dx12_hook_g_OriginalGameQueue);
                                }
                            }

                            // Steam ECL deferred submit: when g_deferOverlaySubmitToSteamECL
                            // is true (non-SL Steam overlay path), skip the normal overlay ECL
                            // submission.  The overlay command list is closed and ready, but
                            // submission is deferred to DetourExecuteCommandLists which fires
                            // AFTER Steam's overlay handler submits its ECL.  This ensures CE
                            // overlay renders on top of Steam's cleared backbuffer.
                            if (dx12_hook_g_deferOverlaySubmitToSteamECL && !useDedicated) {
                                dx12_hook_g_steamDeferredOverlay.cmdList = list;
                                dx12_hook_g_steamDeferredOverlay.allocIdx = idx;
                                dx12_hook_g_steamDeferredOverlay.eclQueue = eclQueue;
                                dx12_hook_g_steamDeferredOverlay.pending = true;
                                static std::atomic<int> s_deferredSkipLogCount{0};
                                int deferredSkipNum =
                                    s_deferredSkipLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
                                if (deferredSkipNum <= 20 || (deferredSkipNum % 200) == 0) {
                                    HookLogImportant(
                                        "DX12: Deferring overlay ECL submit to Steam ECL hook #%d "
                                        "(eclQueue=%p, list=%p, allocIdx=%d, bb=%p, bufIdx=%u)",
                                        deferredSkipNum, eclQueue, list, idx, bb, bufferIdx);
                                }
                                // Skip the normal fence signal too - ECL hook will signal it.
return ProcessFrameFlow::kSkipSteamFence;
                            }

                            {
                                ScopedCEOverlayECLSubmission ceOverlayECLGuard("normal overlay submit");
                                if (!useDedicated && realECL && !isSLWrapperECL) {
                                    const bool fsrFGActiveForECL =
                                        g_FGCompat.GetRuntimeMode() == ce::fg_runtime::RuntimeMode::kFSRFG;
                                    if (fsrFGActiveForECL) {
                                        // FSR FG active: use origECL (vtable/hook-aware) instead of
                                        // realECL (raw D3D12 function).  FSR hooks the game queue's
                                        // ECL vtable entry to track command list submissions.  When
                                        // we bypass FSR's hook via realECL, FSR detects the raw ECL
                                        // on its queue as an unexpected submission and removes the
                                        // D3D12 device (ERR_GFX_STATE).  Using origECL lets FSR's
                                        // hook see and accept our overlay ECL.
                                        origECL(eclQueue, 1, lists);
                                        if (classifyFocusLossSubmitPath) {
                                            submitPathIsD3D12Module =
                                                IsD3D12ModuleAddress(reinterpret_cast<void*>(origECL));
                                        }
                                    } else {
                                        realECL(eclQueue, 1, lists);
                                        usedRealECL = true;
                                        submitPathIsD3D12Module = true;
                                    }
                                } else if (origECL) {
                                    origECL(eclQueue, 1, lists);
                                    if (classifyFocusLossSubmitPath) {
                                        submitPathIsD3D12Module =
                                            IsD3D12ModuleAddress(reinterpret_cast<void*>(origECL));
                                    }
                                } else {
                                    eclQueue->ExecuteCommandLists(1, lists);
                                    if (classifyFocusLossSubmitPath && eclQueue) {
                                        void** vtable = *reinterpret_cast<void***>(eclQueue);
                                        submitPathIsD3D12Module =
                                            vtable && IsD3D12ModuleAddress(vtable[10]);
                                    }
                                }
                            }
                            if (overlayDrawRecorded) {
                                NoteDX12OverlayRendered(DX12OverlayRenderRoute::kNormal);
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
                                if (dx12_hook_g_ResetReinitSubmitCounter.exchange(false, std::memory_order_acquire))
                                    s_reinitSubmitCount = 0;
                                if (s_reinitSubmitCount < 50) {
                                    s_reinitSubmitCount++;
                                    auto* diagDevR = g_Device.load(std::memory_order_acquire);
                                    HRESULT devHrR = diagDevR ? diagDevR->GetDeviceRemovedReason() : E_FAIL;
                                    HookLogImportant(
                                        "DX12: Reinit SUBMIT #%d (queue=%p descFree=%d realECL=%d "
                                        "directD3D12=%d offscreen=%d extOverlay=%d bb=%p bufIdx=%d "
                                        "devRemoved=0x%08X tid=0x%04X)",
                                        s_reinitSubmitCount, eclQueue, usedDescFree ? 1 : 0,
                                        usedRealECL ? 1 : 0, submitPathIsD3D12Module ? 1 : 0,
                                        offscreenCompositeRequired ? 1 : 0, startupOverlayPresent ? 1 : 0,
                                        bb, bufferIdx, (unsigned)devHrR, GetCurrentThreadId());
                                    if (FAILED(devHrR)) {
                                        HookLogImportant("DX12: DEVICE REMOVED after reinit submit #%d!",
                                                         s_reinitSubmitCount);
                                        dx12_hook_g_DeviceRemoved.store(true, std::memory_order_release);
                                    }
                                }
                            }

                            // Post-FG-transition diagnostic: log first 20 frames after any FG change.
                            // Catches DEVICE_REMOVED right after overlay resumes following FG switches.
                            {
                                static int s_postTransitionFrames = 0;
                                static int s_lastTransitionCooldown = -1;
                                int curCooldown = dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire);
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
                                        s_postTransitionFrames, eclQueue, dx12_hook_g_OriginalGameQueue,
                                        (void*)g_CommandQueue.load(), g_FGCompat.IsFGActive() ? 1 : 0,
                                        DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire)
                                            ? 1
                                            : 0,
                                        usedDescFree ? 1 : 0, usedRealECL ? 1 : 0, (unsigned)devHr3, bb,
                                        bufferIdx, GetCurrentThreadId());
                                }
                            }

                            if (dx12_hook_g_State.fence) {
                                UINT64 next = dx12_hook_g_State.currentFenceValue + 1;
                                bool anyFGForFence = slFGActive || g_FGCompat.IsFGActive();
                                const auto presentContext = dx12_hook_s_WrappedPresentFocusLossContext;
                                const bool runtimeOwnedPresentation =
                                    dx12_hook_g_FGRuntimeOwnsSwapchain || HookHasRuntimeOwnedNativeFGPresentPath() ||
                                    DXGIShared::DoesFGRuntimeOwnSwapchain();
                                bool deviceLostForFence = false;
                                {
                                    auto* fenceDev = g_Device.load(std::memory_order_acquire);
                                    if (fenceDev) {
                                        deviceLostForFence = FAILED(fenceDev->GetDeviceRemovedReason());
                                    }
                                }
                                const bool steamDeferredOverlaySubmit =
                                    dx12_hook_g_deferOverlaySubmitToSteamECL && !useDedicated;
                                const bool focusLossImmediateFence = ce::dx12_overlay_policy::
                                    ShouldSignalD3D12FocusLossOverlayFenceImmediately(
                                        presentContext.valid, !frameDesc.Windowed, processHasForeground,
                                        iconicWindow, zeroSizedSwapchain, true, deviceLostForFence,
                                        anyFGForFence, runtimeOwnedPresentation, useDedicated,
                                        steamDeferredOverlaySubmit, dx12_hook_g_State.fence != nullptr,
                                        dx12_hook_g_State.fenceEvent != nullptr, eclQueue != nullptr, next);

                                if (!focusLossImmediateFence && presentContext.valid &&
                                    !processHasForeground) {
                                    static std::atomic<int> s_focusImmediateSkipLogCount{0};
                                    const int logCount = s_focusImmediateSkipLogCount.fetch_add(
                                        1, std::memory_order_relaxed);
                                    if (logCount < 40 || (logCount % 300) == 0) {
                                        HookLog(
                                            "DX12: Focus-loss immediate overlay fence skipped (%s "
                                            "present=%s#%d queue=%p fg=%p/%lu game=%p/%lu sync=%u "
                                            "flags=0x%08X next=%llu event=%p fgActive=%d runtimeOwned=%d "
                                            "dedicated=%d steamDeferred=%d deviceLost=%d realECL=%d "
                                            "directD3D12=%d descFree=%d offscreen=%d)",
                                            DescribeFocusLossImmediateFenceSkip(
                                                presentContext.valid, !frameDesc.Windowed,
                                                processHasForeground, iconicWindow, zeroSizedSwapchain,
                                                true, deviceLostForFence, anyFGForFence,
                                                runtimeOwnedPresentation, useDedicated,
                                                steamDeferredOverlaySubmit, dx12_hook_g_State.fence != nullptr,
                                                dx12_hook_g_State.fenceEvent != nullptr, eclQueue != nullptr, next),
                                            presentContext.presentName ? presentContext.presentName
                                                                       : "Present",
                                            presentContext.callCount, eclQueue, foregroundWindow,
                                            foregroundPid, frameDesc.OutputWindow, currentProcessId,
                                            presentContext.syncInterval, presentContext.presentFlags,
                                            (unsigned long long)next, dx12_hook_g_State.fenceEvent,
                                            anyFGForFence ? 1 : 0, runtimeOwnedPresentation ? 1 : 0,
                                            useDedicated ? 1 : 0, steamDeferredOverlaySubmit ? 1 : 0,
                                            deviceLostForFence ? 1 : 0, usedRealECL ? 1 : 0,
                                            submitPathIsD3D12Module ? 1 : 0, usedDescFree ? 1 : 0,
                                            offscreenCompositeRequired ? 1 : 0);
                                    }
                                }

                                if (focusLossImmediateFence) {
                                    HRESULT sigHr = eclQueue->Signal(dx12_hook_g_State.fence, next);
                                    UINT64 completedValue = dx12_hook_g_State.fence->GetCompletedValue();
                                    if (SUCCEEDED(sigHr)) {
                                        dx12_hook_g_State.currentFenceValue = next;
                                        if (idx >= 0 && idx < (int)dx12_hook_g_State.fenceValues.size())
                                            dx12_hook_g_State.fenceValues[idx] = next;

                                        static std::atomic<int> s_focusImmediateSignalLogCount{0};
                                        const int logCount = s_focusImmediateSignalLogCount.fetch_add(
                                            1, std::memory_order_relaxed);
                                        if (logCount < 80 || (logCount % 300) == 0) {
                                            HookLogImportant(
                                                "DX12: Focus-loss immediate overlay fence signal "
                                                "(present=%s#%d queue=%p fence=%llu completed=%llu "
                                                "fg=%p/%lu game=%p/%lu sync=%u flags=0x%08X realECL=%d "
                                                "directD3D12=%d descFree=%d offscreen=%d); waiting before "
                                                "Present to sync same-frame work",
                                                presentContext.presentName ? presentContext.presentName
                                                                           : "Present",
                                                presentContext.callCount, eclQueue,
                                                (unsigned long long)next,
                                                (unsigned long long)completedValue, foregroundWindow,
                                                foregroundPid, frameDesc.OutputWindow, currentProcessId,
                                                presentContext.syncInterval, presentContext.presentFlags,
                                                usedRealECL ? 1 : 0, submitPathIsD3D12Module ? 1 : 0,
                                                usedDescFree ? 1 : 0, offscreenCompositeRequired ? 1 : 0);
                                        }
                                        WaitForFocusLossImmediateOverlayFenceBeforePresent(
                                            focusLossImmediateFence, true, dx12_hook_g_State.fence,
                                            dx12_hook_g_State.fenceEvent, eclQueue, next, presentContext,
                                            foregroundWindow, foregroundPid, frameDesc.OutputWindow,
                                            currentProcessId, usedRealECL, submitPathIsD3D12Module,
                                            usedDescFree, offscreenCompositeRequired);
                                    } else {
                                        static std::atomic<int> s_focusImmediateSignalFailLogCount{0};
                                        const int logCount = s_focusImmediateSignalFailLogCount.fetch_add(
                                            1, std::memory_order_relaxed);
                                        if (logCount < 20 || (logCount % 300) == 0) {
                                            HookLogImportant(
                                                "DX12: Focus-loss immediate overlay fence Signal failed "
                                                "hr=0x%08X (present=%s#%d queue=%p fence=%llu fg=%p/%lu "
                                                "game=%p/%lu sync=%u flags=0x%08X); falling back to "
                                                "post-Present deferred signal",
                                                (unsigned)sigHr,
                                                presentContext.presentName ? presentContext.presentName
                                                                           : "Present",
                                                presentContext.callCount, eclQueue,
                                                (unsigned long long)next, foregroundWindow, foregroundPid,
                                                frameDesc.OutputWindow, currentProcessId,
                                                presentContext.syncInterval, presentContext.presentFlags);
                                        }
                                        dx12_hook_g_deferredSignalQueue.store(eclQueue, std::memory_order_release);
                                        dx12_hook_g_deferredSignalValue.store(next, std::memory_order_release);
                                        dx12_hook_g_deferredSignalAllocIdx.store(idx, std::memory_order_release);
                                    }
                                } else if (anyFGForFence && !useDedicated) {
                                    // During ANY FG on the game queue, signal a separate
                                    // overlay completion fence via the raw D3D12 Signal
                                    // pointer (bypassing SL/FSR vtable hooks), then wait
                                    // on CPU.  This ensures all overlay GPU work
                                    // (including barriers, font upload, draw commands)
                                    // is complete before the FG runtime reads the
                                    // swapchain backbuffer in its Present hook.
                                    //
                                    // Without this wait, the overlay could still be
                                    // executing PRESENT->RT/RT->PRESENT barriers or
                                    // draw commands on the GPU when the FG runtime
                                    // processes the backbuffer, causing D3D device
                                    // removal (ERR_GFX_STATE).
                                    SignalPtr realSignal =
                                        dx12_hook_g_RealD3D12Signal.load(std::memory_order_acquire);
                                    ID3D12Fence* completionFence =
                                        dx12_hook_g_OverlayCompletionFence.load(std::memory_order_acquire);
                                    if (realSignal && completionFence) {
                                        static std::atomic<UINT64> s_overlayCompletionValue{0};
                                        UINT64 compVal = ++s_overlayCompletionValue;
                                        HRESULT compSigHr = realSignal(eclQueue, completionFence, compVal);
                                        if (SUCCEEDED(compSigHr)) {
                                            if (completionFence->GetCompletedValue() < compVal) {
                                                HANDLE compEvent =
                                                    CreateEventW(nullptr, FALSE, FALSE, nullptr);
                                                if (compEvent) {
                                                    completionFence->SetEventOnCompletion(compVal,
                                                                                          compEvent);
                                                    WaitForSingleObject(compEvent, 2000);
                                                    CloseHandle(compEvent);
                                                }
                                            }
                                        } else {
                                            static std::atomic<int> s_compSigFailLog{0};
                                            if (s_compSigFailLog.fetch_add(1) < 10) {
                                                HookLogImportant(
                                                    "DX12: Overlay completion fence Signal failed "
                                                    "hr=0x%08X (queue=%p)",
                                                    (unsigned)compSigHr, eclQueue);
                                            }
                                        }
                                    }
                                } else if (useDedicated) {
                                    // Signal immediately on dedicated queue (SL
                                    // doesn't see it).
                                    HRESULT sigHr = eclQueue->Signal(dx12_hook_g_State.fence, next);
                                    if (SUCCEEDED(sigHr)) {
                                        dx12_hook_g_State.currentFenceValue = next;
                                        if (idx >= 0 && idx < (int)dx12_hook_g_State.fenceValues.size())
                                            dx12_hook_g_State.fenceValues[idx] = next;
                                    }
                                } else {
                                    // Game queue: defer fence Signal to next frame
                                    // (avoids NVIDIA driver stall between Signal and
                                    // Present).
                                    dx12_hook_g_deferredSignalQueue.store(eclQueue, std::memory_order_release);
                                    dx12_hook_g_deferredSignalValue.store(next, std::memory_order_release);
                                    dx12_hook_g_deferredSignalAllocIdx.store(idx, std::memory_order_release);
                                }
                            } else if (dx12_hook_s_WrappedPresentFocusLossContext.valid && !processHasForeground) {
                                static std::atomic<int> s_focusImmediateNoFenceLogCount{0};
                                const int logCount =
                                    s_focusImmediateNoFenceLogCount.fetch_add(1, std::memory_order_relaxed);
                                if (logCount < 20 || (logCount % 300) == 0) {
                                    const auto presentContext = dx12_hook_s_WrappedPresentFocusLossContext;
                                    const bool anyFGForFence = slFGActive || g_FGCompat.IsFGActive();
                                    const bool runtimeOwnedPresentation =
                                        dx12_hook_g_FGRuntimeOwnsSwapchain ||
                                        HookHasRuntimeOwnedNativeFGPresentPath() ||
                                        DXGIShared::DoesFGRuntimeOwnSwapchain();
                                    HookLog(
                                        "DX12: Focus-loss immediate overlay fence skipped (%s "
                                        "present=%s#%d queue=%p fg=%p/%lu game=%p/%lu sync=%u "
                                        "flags=0x%08X fgActive=%d runtimeOwned=%d); no same-frame fence "
                                        "sync is possible",
                                        DescribeFocusLossImmediateFenceSkip(
                                            presentContext.valid, !frameDesc.Windowed, processHasForeground,
                                            iconicWindow, zeroSizedSwapchain, true, false, anyFGForFence,
                                            runtimeOwnedPresentation, useDedicated,
                                            dx12_hook_g_deferOverlaySubmitToSteamECL && !useDedicated, false,
                                            dx12_hook_g_State.fenceEvent != nullptr, eclQueue != nullptr, 0),
                                        presentContext.presentName ? presentContext.presentName : "Present",
                                        presentContext.callCount, eclQueue, foregroundWindow, foregroundPid,
                                        frameDesc.OutputWindow, currentProcessId,
                                        presentContext.syncInterval, presentContext.presentFlags,
                                        anyFGForFence ? 1 : 0, runtimeOwnedPresentation ? 1 : 0);
                                }
                            }
                        skip_steam_deferred_fence_signal:
                            cmdRecordOk = true;
                            QueryPerformanceCounter(&perfSubmit);
                        }

                        QueryPerformanceCounter(&perfEnd);
                        if (diagnostics && perfFreq.QuadPart > 0) {
                            const auto toUs = [&](LONGLONG ticks) {
                                return (ticks * 1000000) / perfFreq.QuadPart;
                            };
                            diagnostics->overlayAcquireUs =
                                toUs(perfGetBuf.QuadPart - perfQI.QuadPart);
                            diagnostics->overlayRecordUs =
                                toUs(perfRecord.QuadPart - perfGetBuf.QuadPart);
                            diagnostics->overlaySubmitUs =
                                toUs(perfSubmit.QuadPart - perfRecord.QuadPart);
                            diagnostics->overlayPostSubmitUs =
                                toUs(perfEnd.QuadPart - perfSubmit.QuadPart);
                            diagnostics->overlayBreakdownValid = true;
                        }
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

                            if (!dx12_hook_s_startupOverlayCompatSettled.exchange(true, std::memory_order_acq_rel)) {
                                if (shouldRunStartupOverlayDrawProbe &&
                                    dx12_hook_s_startupOverlayFirstDrawProbeStage ==
                                        StartupOverlayFirstDrawProbeStage::kActualRender) {
                                    HookLogImportant(
                                        "DX12: Startup overlay compat settled - future sync reinit will "
                                        "keep the full allocator pool");
                                } else {
                                    HookLogImportant(
                                        "DX12: Stable DX12 overlay rendering observed - later startup "
                                        "overlay popups will stay on the normal coexistence path");
                                }
                            }

                            // Clear probe state if we were in a probe sequence
                            if (shouldRunStartupOverlayDrawProbe &&
                                dx12_hook_s_startupOverlayFirstDrawProbeStage ==
                                    StartupOverlayFirstDrawProbeStage::kActualRender) {
                                HookLogImportant("DX12: Startup overlay probe complete - rendering stably");
                                dx12_hook_s_startupOverlayFirstDrawProbeStage =
                                    StartupOverlayFirstDrawProbeStage::kComplete;
                                dx12_hook_s_startupOverlayFirstDrawProbeMs = 0;
                            }
                        }
                        // FG-SAFE: Release per-frame backbuffer reference
                        if (bbNeedsRelease)
                            bb->Release();
    return ProcessFrameFlow::kContinue;
}

ProcessFrameFlow FrameProcessSession::DrawSc3Else() {
    HookLog("DX12: ProcessFrame - failed to get SwapChain3 interface");
    return ProcessFrameFlow::kContinue;
}

ProcessFrameFlow FrameProcessSession::DrawSubmitElse() {
    HookLog("DX12: ProcessFrame - list->Reset failed hr=0x%08X, forcing reinit", listResetHr);
    dx12_hook_g_State.syncInit = false;
    return ProcessFrameFlow::kContinue;
}

ProcessFrameFlow FrameProcessSession::DrawResetElse() {
    HookLog("DX12: ProcessFrame - alloc->Reset failed hr=0x%08X, forcing reinit", allocResetHr);
    dx12_hook_g_State.syncInit = false;
    return ProcessFrameFlow::kContinue;
}

ProcessFrameFlow FrameProcessSession::DrawNullList() {
    HookLog("DX12: ProcessFrame - null list or alloc");
    return ProcessFrameFlow::kContinue;
}

ProcessFrameFlow FrameProcessSession::pw5_c3() {
    return ProcessFrameFlow::kContinue;
}

ProcessFrameFlow FrameProcessSession::Phase6Tail() {
if (captureAfterOverlay) {

    int64_t captureStartUs = PerfLogger::GetQpcUs();
    PublishDX12CapturedFrame(pSwapChain, captureShm, gameQueue, hasCurrentBackBufferIdx, currentBackBufferIdx);
    const int64_t captureUs = PerfLogger::GetQpcUs() - captureStartUs;
    perfMetrics.captureUs = static_cast<int32_t>(captureUs);
    if (diagnostics) {
        diagnostics->captureUs += captureUs;
    }
}
    return ProcessFrameFlow::kContinue;
}
