                    UpdateOverlay();
                }
            }
        } else if (warnActive_ || warnVisible_) {
            if (warnActive_) {
                if (isRecording) {
                    LogInfo("[PseudoOverlay] NOT RECORDING warning deactivated: recording started");
                } else {
                    LogInfo("[PseudoOverlay] NOT RECORDING warning deactivated: foreground target lost");
                }
            }
            warnActive_ = false;
            warnVisible_ = false;
            UpdateOverlay();
        }
    } else if (warnActive_ || warnVisible_) {
        LogInfo("[PseudoOverlay] NOT RECORDING warning deactivated: mode changed to %d", config_.mode);
        warnActive_ = false;
        warnVisible_ = false;
        UpdateOverlay();
    }

    if (config_.showEncoderOverloadWarn) {
        static ULONGLONG lastOverloadWarnUntil = 0;
        ULONGLONG current = overloadWarnUntil_.load();
        if ((lastOverloadWarnUntil > 0 && current == 0) || (current > 0 && GetTickCount64() > current)) {
            UpdateOverlay();
        }
        lastOverloadWarnUntil = current;
    }

    static ULONGLONG lastScreenshotNotifyUntil = 0;
    ULONGLONG currentScreenshot = screenshotNotifyUntil_.load();
    if ((lastScreenshotNotifyUntil > 0 && currentScreenshot == 0) ||
        (currentScreenshot > 0 && GetTickCount64() > currentScreenshot)) {
        UpdateOverlay();
    }
    lastScreenshotNotifyUntil = currentScreenshot;

    static ULONGLONG lastRecordingNotifyUntil = 0;
    ULONGLONG currentRecordingNotification = recordingNotifyUntil_.load();
    if ((lastRecordingNotifyUntil > 0 && currentRecordingNotification == 0) ||
        (currentRecordingNotification > 0 && GetTickCount64() > currentRecordingNotification)) {
        UpdateOverlay();
    }
    lastRecordingNotifyUntil = currentRecordingNotification;

    if (EnsureSharedMemoryMapping() && pSharedMem_) {
        const uint64_t sharedNotificationExpiry =
            pSharedMem_->runtimeState.notificationExpiry.load(std::memory_order_acquire);
        const uint32_t sharedNotificationType =
            pSharedMem_->runtimeState.notificationType.load(std::memory_order_relaxed);
        const auto sharedRecordingNotification = ToPseudoRecordingNotification(sharedNotificationType);
        if (recordingIndicatorState_ == ce::recording_indicator::State::Idle &&
            sharedRecordingNotification != ce::pseudo_overlay::RecordingNotificationKind::None &&
            sharedNotificationExpiry > GetTickCount64() &&
            (recordingNotification_.load(std::memory_order_relaxed) != sharedRecordingNotification ||
             recordingNotifyUntil_.load(std::memory_order_relaxed) != sharedNotificationExpiry)) {
            recordingNotification_.store(sharedRecordingNotification, std::memory_order_relaxed);
            recordingNotifyUntil_.store(sharedNotificationExpiry, std::memory_order_relaxed);
            UpdateOverlay();
        }
    }

    if (config_.showEncoderOverloadWarn && ce::recording_indicator::IsRecording(recordingIndicatorState_) &&
        EnsureSharedMemoryMapping() && pSharedMem_) {
        const uint32_t overloadFlags = pSharedMem_->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
        const uint32_t captureHealthFlags =
            pSharedMem_->runtimeState.wgcCaptureHealthFlags.load(std::memory_order_relaxed);
        const uint32_t recordingHealthFlags =
            pSharedMem_->runtimeState.recordingHealthFlags.load(std::memory_order_relaxed);
        const uint32_t warningKind = ce::capture_policy::SelectWgcOverlayWarningKind(
            overloadFlags, captureHealthFlags, recordingHealthFlags);
        if (warningKind == ce::capture_policy::kOverlayWarningNone &&
            ce::capture_policy::IsWgcCaptureLimitedForOverlay(captureHealthFlags)) {
            const ULONGLONG previousWarnUntil = overloadWarnUntil_.exchange(0, std::memory_order_relaxed);
            if (previousWarnUntil != 0 && initialized_.load(std::memory_order_acquire)) {
                UpdateOverlay();
            }
        } else if (warningKind != ce::capture_policy::kOverlayWarningNone) {
            ULONGLONG current = GetTickCount64();
            uint32_t currentFps = pSharedMem_->runtimeState.encoderSustainFpsX100.load(std::memory_order_relaxed);
            uint32_t lastFps = overloadWarnSustainFpsX100_.load(std::memory_order_relaxed);
            bool fpsChanged = (currentFps > lastFps ? currentFps - lastFps : lastFps - currentFps) > 100;
            const bool encoderWarning = warningKind == ce::capture_policy::kOverlayWarningEncoderOverload;
            const bool encoderWarningStarted =
                encoderWarning &&
                (lastEncoderOverloadFlags_ & ce::capture_policy::kEncoderOverloadFlagEncoder) == 0;

            if (warningKind != overloadWarnKind_.load(std::memory_order_relaxed) ||
                encoderWarningStarted || (current > overloadWarnUntil_.load() - 2500) ||
                (encoderWarning && fpsChanged)) {
                TriggerRecordingHealthWarning(warningKind, currentFps);
            }
        }
        lastEncoderOverloadFlags_ = overloadFlags;
        lastCaptureHealthFlags_ = captureHealthFlags;
    }

    if (config_.mode == 0) {
        static ULONGLONG lastMode0Check = 0;
        if (now - lastMode0Check >= 500) {
            lastMode0Check = now;
            UpdateOverlay();
        }
    }

    const int64_t tickEndUs = Log_GetQpcUs();
    const int64_t tickElapsedUs = tickEndUs - tickStartUs;
    if (tickElapsedUs > 1000) {
        LogDebug("[PseudoOverlay] TimerTick took %lld us", (long long)tickElapsedUs);
    }
}

void PseudoOverlay::CleanupGDI() {
    if (hdcWarn_ && oldBmWarn_)
        SelectObject(hdcWarn_, oldBmWarn_);
    if (bmWarn_) {
        DeleteObject(bmWarn_);
        bmWarn_ = NULL;
    }
    if (hdcWarn_) {
        DeleteDC(hdcWarn_);
        hdcWarn_ = NULL;
    }
    if (fontWarn_) {
        DeleteObject(fontWarn_);
        fontWarn_ = NULL;
    }
}

// ---- Main overlay update (ported from OBSIndicator UpdateOv) ----

void PseudoOverlay::UpdateOverlay() {
    if (!initialized_.load(std::memory_order_acquire))
        return;

    const auto recordingState = recordingIndicatorState_;
    const bool isRecording = ce::recording_indicator::IsRecording(recordingState);
    const bool isStarting = ce::recording_indicator::IsStarting(recordingState);
    const bool ghostActive = config_.alwaysRender && (!config_.alwaysRenderOnlyWhenGame || IsForegroundTarget());

    const bool shouldHaveVisibleOverlay = ShouldOverlayBeVisible(
        config_, recordingState, warnVisible_, overloadWarnUntil_.load(), screenshotNotifyUntil_.load(),
        recordingNotifyUntil_.load(), recordingNotification_.load(std::memory_order_relaxed), ghostActive);

    LogDebug(
        "[PseudoOverlay] UpdateOverlay: mode=%d recordingState=%u isRecording=%d warnVisible=%d ghost=%d "
        "shouldHaveVisible=%d",
        config_.mode, static_cast<unsigned>(recordingState), isRecording ? 1 : 0, warnVisible_ ? 1 : 0,
        ghostActive ? 1 : 0, shouldHaveVisibleOverlay ? 1 : 0);

    if (!config_.enabled) {
        DestroyOverlayWindows();
        lastOv_ = {};
        lastWarnVis_ = false;
        lastOverlaySuppressed_ = false;
        lastFullscreenSuppressed_ = false;
        return;
    }

    // Suppress when inject overlay is active in a hooked game
    const bool injectPending = IsInjectOverlayPending();
    const bool injectActive = IsInjectOverlayActive();
    const bool suppressOverlay = ShouldSuppressPseudoOverlayForInjectOverlayHandoff(injectPending, injectActive);
    if (suppressOverlay) {
        if (!lastOverlaySuppressed_) {
            LogInfo("[PseudoOverlay] Suppressed while inject overlay handoff is active (pending=%d active=%d)",
                    injectPending ? 1 : 0, injectActive ? 1 : 0);
        }
        DestroyOverlayWindows();
        lastOv_ = {};
        lastWarnVis_ = false;
        lastOverlaySuppressed_ = true;
        lastFullscreenSuppressed_ = false;
        return;
    }

    if (lastOverlaySuppressed_) {
        LogInfo("[PseudoOverlay] Resuming after inject overlay suppression");
        lastOverlaySuppressed_ = false;
    }

    if (!shouldHaveVisibleOverlay) {
        if (hOv_ || hWarn_) {
            LogInfo("[PseudoOverlay] Destroying idle overlay windows (isRecording=%d warnVis=%d ghost=%d)",
                    isRecording ? 1 : 0, warnVisible_ ? 1 : 0, ghostActive ? 1 : 0);
            DestroyOverlayWindows();
        }
        lastOv_ = {};
        lastWarnVis_ = false;
        return;
    }

    // Foreground-acquire grace: while the whitelisted PID is still inside the
    // post-focus window, refresh the sticky anchor (so the first post-grace frame is
    // in-position) but skip the four OS-touching calls (EnsureOverlayWindows /
    // ShowWindow / SetWindowPos / UpdateLayeredWindow) so we do not race Windows MPO
    // / fullscreen buffer rebinds on Alt+Tab-in. The warning blink phase is already
    // advanced in OnTimerTick, so the first post-grace frame is also in-phase.
    const uint32_t rawFgPid = GetForegroundTargetPid();
    const bool currentHadTarget = IsForegroundTarget();
    const ULONGLONG nowForGrace = GetTickCount64();
    const auto graceDecision = EvaluateForegroundGrace(currentHadTarget, currentHadTarget ? rawFgPid : 0u, nowForGrace);
    if (graceDecision.suppressVisibleOverlay) {
        // Keep the anchor fresh even though we are not touching the OS yet. The
        // sticky fields are read by ResolveAnchorInfo() on the next call and by the
        // diff logic in lastOv_ below.
        (void)ResolveAnchorInfo();
        return;
    }

    if (!EnsureOverlayWindows()) {
        LogWarn("[PseudoOverlay] EnsureOverlayWindows failed");
        return;
    }

    if (screenshotInProgress_.load(std::memory_order_acquire)) {
        if (hOv_)
            ShowWindow(hOv_, SW_HIDE);
        if (hWarn_)
            ShowWindow(hWarn_, SW_HIDE);
        return;
    }

    LogDebug("[PseudoOverlay] Overlay windows present: hOv=%p hWarn=%p", (void*)hOv_, (void*)hWarn_);

    const AnchorInfo anchor = ResolveAnchorInfo();
    if (anchor.fullscreenLike) {
        if (!lastFullscreenSuppressed_) {
            LogInfo("[PseudoOverlay] Fullscreen-like anchor detected");
            lastFullscreenSuppressed_ = true;
        }
    } else if (lastFullscreenSuppressed_) {
        LogInfo("[PseudoOverlay] Fullscreen-like anchor cleared");
        lastFullscreenSuppressed_ = false;
    }

    UpdateScaleForDpi(anchor.dpi);

    const int monitorLeft = anchor.monitorRect.left;
    const int monitorTop = anchor.monitorRect.top;
    const int sw = anchor.monitorRect.right - anchor.monitorRect.left;
    const int sh = anchor.monitorRect.bottom - anchor.monitorRect.top;
    int p = config_.pad;
    int s = config_.size;
    int fullS = s + p;

    // Window positions (anchored to screen corners)
    int winX = 0, winY = 0;
    if (config_.pos == 3) {
        winX = 0;
        winY = 0;
    }  // TL
    else if (config_.pos == 2) {
        winX = sw - fullS;
        winY = 0;
    }  // TR
    else if (config_.pos == 1) {
        winX = 0;
        winY = sh - fullS;
    }  // BL
    else {
        winX = sw - fullS;
        winY = sh - fullS;
    }  // BR
    winX += monitorLeft;
    winY += monitorTop;

    // Indicator offsets (relative to window)
    int indX = 0, indY = 0;
    if (config_.pos == 3) {
        indX = p;
        indY = p;
    }  // TL
    else if (config_.pos == 2) {
        indX = 0;
        indY = p;
    }  // TR
    else if (config_.pos == 1) {
        indX = p;
        indY = 0;
    }  // BL
    else {
        indX = 0;
        indY = 0;
    }  // BR

    // Ghost pixel offsets (relative to window, furthest corner)
    int pixX = 0, pixY = 0;
    if (config_.pos == 3) {
        pixX = 0;
        pixY = 0;
    }  // TL -> 0,0
    else if (config_.pos == 2) {
        pixX = fullS - 1;
        pixY = 0;
    }  // TR
    else if (config_.pos == 1) {
        pixX = 0;
        pixY = fullS - 1;
    }  // BL
    else {
        pixX = fullS - 1;
        pixY = fullS - 1;
    }  // BR

    bool showInd = false;
    if (ce::recording_indicator::IsVisible(recordingState) && config_.mode != 2)  // MODE_WARN_ONLY
        showInd = true;

    BYTE indAlpha = 0;
    if (ghostActive) {
        indAlpha = showInd ? 255 : 1;
    } else {
        indAlpha = showInd ? 255 : 0;
    }

    // Determine color
    COLORREF curCol = isStarting ? kColStarting : (isRecording ? RGB(255, 0, 0) : RGB(0, 100, 255));

    // Change detection
    bool doUpdateInd = false;
    if (indAlpha != (lastOv_.vis ? 255 : (lastOv_.ghost ? 1 : 0)) || curCol != lastCol_ || lastOv_.x != winX ||
        lastOv_.y != winY || lastOv_.s != fullS) {
        doUpdateInd = true;
    }

    if (doUpdateInd) {
        if (indAlpha > 0) {
            if (!IsWindowVisible(hOv_))
                ShowWindow(hOv_, SW_SHOWNA);
            SetWindowPos(hOv_, NULL, winX, winY, fullS, fullS, SWP_NOACTIVATE | SWP_NOZORDER);
        } else {
            ShowWindow(hOv_, SW_HIDE);
        }

        if (indAlpha > 1) {
            HDC hdcScreen = GetDC(NULL);
            if (hdcScreen) {
                HDC hdcMem = CreateCompatibleDC(hdcScreen);
                if (hdcMem) {
                    void* pBits = nullptr;
                    HBITMAP hBm = CreateArgbDibSection(fullS, fullS, &pBits);
                    if (hBm && pBits) {
                        HBITMAP hOldBm = (HBITMAP)SelectObject(hdcMem, hBm);

                        memset(pBits, 0, fullS * fullS * 4);

                        HPEN hPen = CreatePen(PS_SOLID, S(2), RGB(255, 255, 255));
                        HBRUSH hBrush = CreateSolidBrush(curCol);
                        HPEN hOldPen = (HPEN)SelectObject(hdcMem, hPen);
                        HBRUSH hOldBrush = (HBRUSH)SelectObject(hdcMem, hBrush);
                        Ellipse(hdcMem, indX + S(1), indY + S(1), indX + s - S(1), indY + s - S(1));
                        SelectObject(hdcMem, hOldPen);
                        SelectObject(hdcMem, hOldBrush);
                        DeleteObject(hBrush);
                        DeleteObject(hPen);

                        ApplyPremultipliedAlpha(pBits, fullS, fullS);

                        POINT ptDst = {winX, winY};
                        SIZE szWnd = {fullS, fullS};
                        POINT ptSrc = {0, 0};
                        BLENDFUNCTION blend = {AC_SRC_OVER, 0, indAlpha, AC_SRC_ALPHA};
                        UpdateLayeredWindow(hOv_, hdcScreen, &ptDst, &szWnd, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);

                        SelectObject(hdcMem, hOldBm);
                        DeleteObject(hBm);
                    }
                    DeleteDC(hdcMem);
                }
                ReleaseDC(NULL, hdcScreen);
            }
        } else if (indAlpha == 1) {
            HDC hdcScreen = GetDC(NULL);
            if (hdcScreen) {
                HDC hdcMem = CreateCompatibleDC(hdcScreen);
                if (hdcMem) {
                    void* pBits = nullptr;
                    HBITMAP hBm = CreateArgbDibSection(fullS, fullS, &pBits);
                    if (hBm && pBits) {
                        HBITMAP hOldBm = (HBITMAP)SelectObject(hdcMem, hBm);
                        memset(pBits, 0, fullS * fullS * 4);
                        static_cast<DWORD*>(pBits)[pixY * fullS + pixX] = 0xFFFFFFFFu;

                        POINT ptDst = {winX, winY};
                        SIZE szWnd = {fullS, fullS};
                        POINT ptSrc = {0, 0};
                        BLENDFUNCTION blend = {AC_SRC_OVER, 0, 1, AC_SRC_ALPHA};
                        UpdateLayeredWindow(hOv_, hdcScreen, &ptDst, &szWnd, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);

                        SelectObject(hdcMem, hOldBm);
                        DeleteObject(hBm);
                    }
                    DeleteDC(hdcMem);
                }
                ReleaseDC(NULL, hdcScreen);
            }
        }

        lastOv_ = {winX, winY, fullS, showInd, ghostActive};
        lastCol_ = curCol;
    }

    // ---- Warning overlay update ----
    ULONGLONG now = GetTickCount64();
    ce::pseudo_overlay::OverlayVisibilityInputs textInputs;
    textInputs.mode = config_.mode;
    textInputs.recordingState = recordingState;
    textInputs.warnVisible = warnVisible_;
    textInputs.showEncoderOverloadWarn = config_.showEncoderOverloadWarn;
    textInputs.overloadWarnUntilMs = overloadWarnUntil_.load();
    textInputs.screenshotNotifyUntilMs = screenshotNotifyUntil_.load();
    textInputs.recordingNotifyUntilMs = recordingNotifyUntil_.load();
    textInputs.recordingNotification = recordingNotification_.load(std::memory_order_relaxed);
    textInputs.nowMs = now;
    const auto textKind = ce::pseudo_overlay::SelectPseudoOverlayText(textInputs);
    const bool showStarting = textKind == ce::pseudo_overlay::OverlayTextKind::Starting;
    const bool showScreenshot = textKind == ce::pseudo_overlay::OverlayTextKind::Screenshot;
    const bool screenshotSucceeded = screenshotNotificationSucceeded_.load(std::memory_order_relaxed);
    const bool showOverload = textKind == ce::pseudo_overlay::OverlayTextKind::EncoderOverload;
    const bool showW = textKind != ce::pseudo_overlay::OverlayTextKind::None;
    BYTE warnAlpha = 0;
    bool doUpdateWarn = false;

    const uint32_t overloadWarnSustainFpsX100 = this->overloadWarnSustainFpsX100_.load();
    uint32_t overloadTargetFps = 0;
    if (showOverload && EnsureSharedMemoryMapping() && pSharedMem_) {
        overloadTargetFps = pSharedMem_->runtimeState.wgcTargetFps.load(std::memory_order_relaxed);
    }
    const std::string overloadMsg = FormatRecordingHealthMessage(
        overloadWarnKind_.load(std::memory_order_relaxed), overloadWarnSustainFpsX100, overloadTargetFps);
    const bool showRecordingFinalizing = textKind == ce::pseudo_overlay::OverlayTextKind::RecordingFinalizing;
    const bool showRecordingSaved = textKind == ce::pseudo_overlay::OverlayTextKind::RecordingSaved;
    const bool showRecordingSavedDegraded =
        textKind == ce::pseudo_overlay::OverlayTextKind::RecordingSavedDegraded;
    const bool showRecordingCanceled = textKind == ce::pseudo_overlay::OverlayTextKind::RecordingCanceled;
    const bool showRecordingFailed = textKind == ce::pseudo_overlay::OverlayTextKind::RecordingFailed;
    const char* msg = showStarting ? ce::recording_indicator::GetStartingText(recordingState)
                       : showScreenshot ? (screenshotSucceeded ? "Screenshot saved!" : "Screenshot failed!")
                       : showRecordingFinalizing ? "Finalizing recording..."
                       : showRecordingSaved ? "Recording saved"
                       : showRecordingSavedDegraded ? "Recording saved - video degraded"
                       : showRecordingCanceled ? "Recording canceled"
                       : showRecordingFailed ? "Recording failed"
                      : showOverload   ? overloadMsg.c_str()
                                       : "NOT RECORDING";
    if (ghostActive) {
        warnAlpha = showW ? 255 : 0;
        if ((warnAlpha > 0) != lastWarnVis_ || ghostActive != lastOv_.ghost || msg != lastWarnMsg_)
            doUpdateWarn = true;
    } else {
        if (showW) {
            warnAlpha = 255;
            if (!lastWarnVis_ || msg != lastWarnMsg_ || lastOv_.ghost)
                doUpdateWarn = true;
        } else {
            if (lastWarnVis_ || lastOv_.ghost) {
                warnAlpha = 0;
                doUpdateWarn = true;
            }
        }
    }

    if (doUpdateWarn) {
        if (!hdcWarn_)
            return;

        if (warnAlpha > 0) {
            if (!IsWindowVisible(hWarn_))
                ShowWindow(hWarn_, SW_SHOWNA);
            SetWindowPos(hWarn_, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
        } else {
            if (hWarn_) {
                ShowWindow(hWarn_, SW_HIDE);
            }
        }

        bool cacheStale = (msg != lastWarnMsg_) || (bmWarn_ == NULL);

        int warnW = 0;
        int warnH = 0;

        if (cacheStale) {
            HDC hdcScreen = GetDC(NULL);
            if (!hdcScreen)
                return;

            HDC dc = CreateCompatibleDC(hdcScreen);
            if (!dc) {
                ReleaseDC(NULL, hdcScreen);
                return;
            }

            HFONT oldFont = (HFONT)SelectObject(dc, fontWarn_);
            RECT rText = {0, 0, 0, 0};
            DrawTextA(dc, msg, -1, &rText, DT_CALCRECT);
            warnW = rText.right - rText.left + S(20);
            warnH = rText.bottom - rText.top + S(10);
            SelectObject(dc, oldFont);
            DeleteDC(dc);

            if (bmWarn_) {
                SelectObject(hdcWarn_, oldBmWarn_);
                DeleteObject(bmWarn_);
                bmWarn_ = NULL;
                oldBmWarn_ = NULL;
            }

            bmWarn_ = CreateCompatibleBitmap(hdcScreen, warnW, warnH);
            if (!bmWarn_) {
                ReleaseDC(NULL, hdcScreen);
                return;
            }

            HBITMAP prevBm = (HBITMAP)SelectObject(hdcWarn_, bmWarn_);
            if (oldBmWarn_ == NULL)
                oldBmWarn_ = prevBm;

            HBRUSH hK = CreateSolidBrush(RGB(0, 0, 0));
            RECT rFill = {0, 0, warnW, warnH};
            FillRect(hdcWarn_, &rFill, hK);
            DeleteObject(hK);

            SelectObject(hdcWarn_, fontWarn_);
            SetTextColor(hdcWarn_, showStarting   ? kColStarting
                                   : showScreenshot ? (screenshotSucceeded ? kColScreenshotText
                                                                            : kColScreenshotFailureText)
                                    : (showRecordingSavedDegraded || showRecordingFailed) ? kColWarnText
                                    : (showRecordingFinalizing || showRecordingSaved || showRecordingCanceled)
                                        ? kColScreenshotText
                                        : kColWarnText);
            SetBkMode(hdcWarn_, TRANSPARENT);

            RECT rT = {S(10), S(5), warnW, warnH};
            DrawTextA(hdcWarn_, msg, -1, &rT, DT_LEFT | DT_TOP | DT_NOCLIP);

            ReleaseDC(NULL, hdcScreen);

            sizeWarn_ = {warnW, warnH};
            lastWarnMsg_ = msg;
        }

        int wx = 0, wy = 0;
        int off = s + p + S(10);
        int wW = sizeWarn_.cx;
        int wH = sizeWarn_.cy;

        if (config_.pos == 3) {
            wx = monitorLeft + off;
            wy = monitorTop + p;
        }  // TL
        else if (config_.pos == 2) {
            wx = monitorLeft + sw - off - wW;
            wy = monitorTop + p;
        }  // TR
        else if (config_.pos == 1) {
            wx = monitorLeft + off;
            wy = monitorTop + sh - p - wH;
        }  // BL
        else {
            wx = monitorLeft + sw - off - wW;
            wy = monitorTop + sh - p - wH - S(40);
        }  // BR

        POINT ptDst = {wx, wy};
        SIZE szWnd = {wW, wH};
        POINT ptSrc = {0, 0};
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, warnAlpha, 0};
        const BOOL ulwOk = UpdateLayeredWindow(hWarn_, NULL, &ptDst, &szWnd, hdcWarn_, &ptSrc, RGB(0, 0, 0), &blend,
                                               ULW_COLORKEY | ULW_ALPHA);

        LogDebug("[PseudoOverlay] Warning overlay: msg='%s' alpha=%d wx=%d wy=%d w=%d h=%d ulw=%d", msg, warnAlpha, wx,
                 wy, wW, wH, ulwOk ? 1 : 0);

        lastWarnVis_ = warnAlpha > 0;
    }
}

// ---- Window procedures ----

LRESULT CALLBACK PseudoOverlay::IndicatorWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) {
        return MA_NOACTIVATE;
    }
    if (m == WM_ACTIVATE || m == WM_ACTIVATEAPP) {
        return 0;
    }
    if (m == WM_SETFOCUS) {
        SetFocus(NULL);
        return 0;
    }
    if (m == WM_NCACTIVATE) {
        return FALSE;
    }
    if (m == WM_NCHITTEST) {
        return HTTRANSPARENT;
    }
    if (m == WM_TIMER && w == kTimerId && instance_ &&
        instance_->initialized_.load(std::memory_order_acquire)) {
        instance_->OnTimerTick();
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}

LRESULT CALLBACK PseudoOverlay::WarningWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) {
        return MA_NOACTIVATE;
    }
    if (m == WM_ACTIVATE || m == WM_ACTIVATEAPP) {
        return 0;
    }
    if (m == WM_SETFOCUS) {
        SetFocus(NULL);
        return 0;
    }
    if (m == WM_NCACTIVATE) {
        return FALSE;
    }
    if (m == WM_NCHITTEST) {
        return HTTRANSPARENT;
    }
    return DefWindowProc(h, m, w, l);
}

VOID CALLBACK PseudoOverlay::TimerProc(HWND, UINT, UINT_PTR timerId, DWORD) {
    if (timerId != 0 && instance_ && instance_->initialized_.load(std::memory_order_acquire)) {
        instance_->OnTimerTick();
    }
}

// ---- Init / Shutdown ----

bool PseudoOverlay::Init(HINSTANCE hInstance) {
    if (initialized_.load(std::memory_order_acquire))
        return true;

    if (uiThread_.joinable()) {
        return false;
    }

    hInstance_ = hInstance;
    uiReadyEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!uiReadyEvent_) {
        LogError("[PseudoOverlay] Failed to create UI-thread readiness event");
        return false;
    }

    uiInitSucceeded_.store(false, std::memory_order_release);
    uiThread_ = std::thread([this]() { ThreadMain(); });
    const DWORD readyResult = WaitForSingleObject(uiReadyEvent_, 5000);
    const bool ready = readyResult == WAIT_OBJECT_0 && uiInitSucceeded_.load(std::memory_order_acquire);
    if (!ready) {
        LogError("[PseudoOverlay] UI thread failed to initialize (wait=%lu)", readyResult);
        const DWORD threadId = uiThreadId_.load(std::memory_order_acquire);
        if (threadId != 0) {
            PostThreadMessageW(threadId, kMsgShutdown, 0, 0);
        }
        if (uiThread_.joinable()) {
            uiThread_.join();
        }
        CloseHandle(uiReadyEvent_);
        uiReadyEvent_ = NULL;
        uiThreadId_.store(0, std::memory_order_release);
        hInstance_ = NULL;
        return false;
    }

    CloseHandle(uiReadyEvent_);
    uiReadyEvent_ = NULL;
    return true;
}

void PseudoOverlay::ThreadMain() {
    uiThreadId_.store(GetCurrentThreadId(), std::memory_order_release);
    MSG msg = {};
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    const bool initialized = InitializeOnUiThread();
    uiInitSucceeded_.store(initialized, std::memory_order_release);
    if (uiReadyEvent_) {
        SetEvent(uiReadyEvent_);
    }
    if (!initialized) {
        instance_ = nullptr;
        uiThreadId_.store(0, std::memory_order_release);
        return;
