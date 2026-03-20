// Pseudo-overlay indicator for WGC capture (no injection required).
// Ported from OBSIndicator with permission (MIT license).
// Runs entirely in the captureengine controller process.

#include "pseudo_overlay.h"
#include "../common/logging.h"

#include <algorithm>
#include <sstream>
#include <string>

// ---- Palette (matching OBSIndicator exactly) ----
static constexpr COLORREF kColWarnText = RGB(255, 20, 20);

// ---- Static instance pointer for wndproc routing ----
PseudoOverlay* PseudoOverlay::instance_ = nullptr;

// ---- Constructor / Destructor ----

PseudoOverlay::PseudoOverlay() = default;

PseudoOverlay::~PseudoOverlay() {
    Shutdown();
}

// ---- Scale helper ----

int PseudoOverlay::S(int v) const {
    return static_cast<int>(static_cast<float>(v) * scale_);
}

// ---- Foreground process detection (ported from OBSIndicator) ----

bool PseudoOverlay::IsForegroundTarget() {
    if (config_.processList.empty())
        return false;

    HWND hFg = GetForegroundWindow();
    if (!hFg)
        return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(hFg, &pid);
    if (pid == 0)
        return false;

    // Cache results to avoid overhead while the same window is focused
    static DWORD lastPid = 0;
    static bool lastRes = false;
    static ULONGLONG lastCheckTime = 0;

    // Re-validate every 2 seconds in case config changed
    if (pid == lastPid && (GetTickCount64() - lastCheckTime < 2000))
        return lastRes;

    bool match = false;
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProcess) {
        char exePath[MAX_PATH];
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameA(hProcess, 0, exePath, &size)) {
            std::string exeName = exePath;
            size_t lastSlash = exeName.find_last_of("\\/");
            if (lastSlash != std::string::npos)
                exeName = exeName.substr(lastSlash + 1);
            std::transform(exeName.begin(), exeName.end(), exeName.begin(), ::tolower);

            std::stringstream ss(config_.processList);
            std::string item;
            while (std::getline(ss, item, '|')) {
                if (!item.empty()) {
                    std::string t = item;
                    std::transform(t.begin(), t.end(), t.begin(), ::tolower);
                    if (exeName == t) {
                        match = true;
                        break;
                    }
                }
            }
        }
        CloseHandle(hProcess);
    }

    lastPid = pid;
    lastRes = match;
    lastCheckTime = GetTickCount64();
    return match;
}

// ---- GDI helpers ----

void PseudoOverlay::InitGDI() {
    HDC hdcScreen = GetDC(NULL);
    hdcWarn_ = CreateCompatibleDC(hdcScreen);
    if (hdcWarn_)
        oldBmWarn_ = (HBITMAP)GetCurrentObject(hdcWarn_, OBJ_BITMAP);
    ReleaseDC(NULL, hdcScreen);

    fontWarn_ = CreateFontA(-S(40), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, "Arial");
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
    if (!initialized_)
        return;

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
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

    bool rec = isRecording_.load();
    bool showInd = false;
    if (rec && config_.mode != 2)  // MODE_WARN_ONLY
        showInd = true;

    bool ghostActive = config_.alwaysRender && (!config_.alwaysRenderOnlyWhenGame || IsForegroundTarget());

    // Determine alpha
    BYTE indAlpha = 0;
    if (ghostActive) {
        indAlpha = showInd ? 255 : 1;
    } else {
        indAlpha = showInd ? 255 : 0;
    }

    // Determine color
    COLORREF curCol = rec ? RGB(255, 0, 0) : RGB(0, 100, 255);

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
            SetWindowPos(hOv_, HWND_TOPMOST, winX, winY, fullS, fullS, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else {
            if (!config_.alwaysRender)
                SetWindowPos(hOv_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_HIDEWINDOW);
        }

        // Capture locals for the static draw callback
        int lIndX = indX, lIndY = indY, lS = s, lFullS = fullS;
        int lPixX = pixX, lPixY = pixY;
        COLORREF lCurCol = curCol;
        int lScale2 = S(2);
        int lScale1 = S(1);
        bool lGhostPixel = (indAlpha == 1);

        // Draw function using captured values via a lambda wrapped as function pointer
        // We use a different approach: pass context through a static and call it.
        // Actually, let's use a simpler approach - do the drawing inline.

        HDC hdcScreen = GetDC(NULL);
        if (hdcScreen) {
            HDC hdcMem = CreateCompatibleDC(hdcScreen);
            if (hdcMem) {
                HBITMAP hBm = CreateCompatibleBitmap(hdcScreen, fullS, fullS);
                if (hBm) {
                    HBITMAP hOldBm = (HBITMAP)SelectObject(hdcMem, hBm);

                    if (indAlpha == 1) {
                        // Ghost mode: draw single invisible pixel at furthest corner
                        HBRUSH hInv = CreateSolidBrush(RGB(0, 0, 1));
                        RECT rPixel = {pixX, pixY, pixX + 1, pixY + 1};
                        FillRect(hdcMem, &rPixel, hInv);
                        DeleteObject(hInv);
                    } else {
                        // Fill with black (color key = transparent)
                        HBRUSH hBlack = CreateSolidBrush(RGB(0, 0, 0));
                        RECT r = {0, 0, fullS, fullS};
                        FillRect(hdcMem, &r, hBlack);
                        DeleteObject(hBlack);

                        // Draw indicator circle
                        HPEN hPen = CreatePen(PS_SOLID, S(2), RGB(255, 255, 255));
                        HBRUSH hBrush = CreateSolidBrush(curCol);
                        HPEN hOldPen = (HPEN)SelectObject(hdcMem, hPen);
                        HBRUSH hOldBrush = (HBRUSH)SelectObject(hdcMem, hBrush);
                        Ellipse(hdcMem, indX + S(1), indY + S(1), indX + s - S(1), indY + s - S(1));
                        SelectObject(hdcMem, hOldPen);
                        SelectObject(hdcMem, hOldBrush);
                        DeleteObject(hBrush);
                        DeleteObject(hPen);
                    }

                    POINT ptDst = {winX, winY};
                    SIZE szWnd = {fullS, fullS};
                    POINT ptSrc = {0, 0};
                    BLENDFUNCTION blend = {AC_SRC_OVER, 0, indAlpha, 0};
                    DWORD flags = ULW_COLORKEY | ULW_ALPHA;
                    UpdateLayeredWindow(hOv_, hdcScreen, &ptDst, &szWnd, hdcMem, &ptSrc, RGB(0, 0, 0), &blend, flags);

                    SelectObject(hdcMem, hOldBm);
                    DeleteObject(hBm);
                }
                DeleteDC(hdcMem);
            }
            ReleaseDC(NULL, hdcScreen);
        }

        lastOv_ = {winX, winY, fullS, showInd, ghostActive};
        lastCol_ = curCol;
    }

    // ---- Warning overlay update ----
    bool showOverload = config_.showEncoderOverloadWarn && (GetTickCount64() < overloadWarnUntil_.load());
    bool showW = warnVisible_ || showOverload;
    BYTE warnAlpha = 0;
    bool doUpdateWarn = false;

    const char* msg = showOverload ? "Encoder overloaded!" : "NOT RECORDING";

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
            SetWindowPos(hWarn_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else {
            SetWindowPos(hWarn_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_HIDEWINDOW);
        }

        // Check if cached bitmap needs refresh
        bool cacheStale = (msg != lastWarnMsg_) || (bmWarn_ == NULL);

        if (cacheStale) {
            HDC hdcScreen = GetDC(NULL);
            if (!hdcScreen)
                return;

            HDC dc = CreateCompatibleDC(hdcScreen);
            if (!dc) {
                ReleaseDC(NULL, hdcScreen);
                return;
            }

            // Measure text
            HFONT oldFont = (HFONT)SelectObject(dc, fontWarn_);
            RECT rText = {0, 0, 0, 0};
            DrawTextA(dc, msg, -1, &rText, DT_CALCRECT);
            int wW = rText.right - rText.left + S(20);
            int wH = rText.bottom - rText.top + S(10);
            SelectObject(dc, oldFont);
            DeleteDC(dc);

            // Re-allocate bitmap
            if (bmWarn_) {
                if (oldBmWarn_)
                    SelectObject(hdcWarn_, oldBmWarn_);
                DeleteObject(bmWarn_);
                bmWarn_ = NULL;
            }
            bmWarn_ = CreateCompatibleBitmap(hdcScreen, wW, wH);
            if (!bmWarn_) {
                ReleaseDC(NULL, hdcScreen);
                return;
            }

            // Draw into cached bitmap
            HBITMAP oldWarnBm = (HBITMAP)SelectObject(hdcWarn_, bmWarn_);
            if (!oldBmWarn_)
                oldBmWarn_ = oldWarnBm;
            SelectObject(hdcWarn_, fontWarn_);
            SetTextColor(hdcWarn_, kColWarnText);
            SetBkMode(hdcWarn_, TRANSPARENT);

            // Fill black (color key)
            RECT rFill = {0, 0, wW, wH};
            HBRUSH hK = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(hdcWarn_, &rFill, hK);
            DeleteObject(hK);

            // Draw text
            RECT rT = {S(10), S(5), wW, wH};
            DrawTextA(hdcWarn_, msg, -1, &rT, DT_LEFT | DT_TOP | DT_NOCLIP);

            ReleaseDC(NULL, hdcScreen);

            sizeWarn_ = {wW, wH};
            lastWarnMsg_ = msg;
        }

        // Calculate position based on corner
        int wx = 0, wy = 0;
        int off = s + p + S(10);
        int wW = sizeWarn_.cx;
        int wH = sizeWarn_.cy;

        if (config_.pos == 3) {
            wx = off;
            wy = p;
        }  // TL
        else if (config_.pos == 2) {
            wx = sw - off - wW;
            wy = p;
        }  // TR
        else if (config_.pos == 1) {
            wx = off;
            wy = sh - p - wH;
        }  // BL
        else {
            wx = sw - off - wW;
            wy = sh - p - wH - S(40);
        }  // BR

        POINT ptDst = {wx, wy};
        SIZE szWnd = {wW, wH};
        POINT ptSrc = {0, 0};
        DWORD flags = ULW_COLORKEY | ULW_ALPHA;
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, warnAlpha, 0};
        UpdateLayeredWindow(hWarn_, NULL, &ptDst, &szWnd, hdcWarn_, &ptSrc, RGB(0, 0, 0), &blend, flags);

        lastWarnVis_ = warnAlpha > 0;
    }
}

// ---- Window procedures ----

LRESULT CALLBACK PseudoOverlay::IndicatorWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_TIMER && w == kTimerId && instance_ && instance_->initialized_) {
        PseudoOverlay* self = instance_;
        ULONGLONG now = GetTickCount64();

        if (self->config_.mode == 1 || self->config_.mode == 2) {
            // Warning mode active
            bool warnTargetFocused = self->IsForegroundTarget();
            bool condition = warnTargetFocused && !self->isRecording_.load();

            if (condition) {
                if (!self->warnActive_) {
                    self->warnActive_ = true;
                    self->warnCycleStart_ = now;
                    self->warnVisible_ = true;
                    self->UpdateOverlay();
                } else {
                    // Cycle: 2s ON, 1s OFF = 3s total
                    ULONGLONG elapsed = now - self->warnCycleStart_;
                    ULONGLONG cycleTime = elapsed % 3000;
                    bool shouldBeVisible = (cycleTime < 2000);
                    if (self->warnVisible_ != shouldBeVisible) {
                        self->warnVisible_ = shouldBeVisible;
                        self->UpdateOverlay();
                    }
                }
            } else {
                if (self->warnActive_ || self->warnVisible_) {
                    self->warnActive_ = false;
                    self->warnVisible_ = false;
                    self->UpdateOverlay();
                }
            }
        } else {
            if (self->warnActive_ || self->warnVisible_) {
                self->warnActive_ = false;
                self->warnVisible_ = false;
                self->UpdateOverlay();
            }
        }

        // Check encoder overload expiry
        if (self->config_.showEncoderOverloadWarn) {
            static ULONGLONG lastOverloadWarnUntil = 0;
            ULONGLONG current = self->overloadWarnUntil_.load();
            if ((lastOverloadWarnUntil > 0 && current == 0) || (current > 0 && GetTickCount64() > current)) {
                self->UpdateOverlay();
            }
            lastOverloadWarnUntil = current;
        }

        // Periodic refresh in info mode
        if (self->config_.mode == 0)
            self->UpdateOverlay();

        return 0;
    }
    return DefWindowProc(h, m, w, l);
}

LRESULT CALLBACK PseudoOverlay::WarningWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProc(h, m, w, l);
}

// ---- Init / Shutdown ----

bool PseudoOverlay::Init(HINSTANCE hInstance) {
    if (initialized_)
        return true;

    instance_ = this;
    scale_ = static_cast<float>(GetDpiForSystem()) / 96.0f;

    InitGDI();

    // Register indicator window class
    WNDCLASSA wcInd = {};
    wcInd.lpfnWndProc = IndicatorWndProc;
    wcInd.hInstance = hInstance;
    wcInd.lpszClassName = kIndicatorClass;
    if (!RegisterClassA(&wcInd) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        LogError("[PseudoOverlay] Failed to register indicator window class");
        CleanupGDI();
        return false;
    }

    // Create indicator overlay window
    hOv_ = CreateWindowExA(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW, kIndicatorClass, "",
                           WS_POPUP, 0, 0, 0, 0, NULL, NULL, hInstance, 0);
    if (!hOv_) {
        LogError("[PseudoOverlay] Failed to create indicator overlay window");
        CleanupGDI();
        return false;
    }

    // Register warning window class
    WNDCLASSA wcWarn = {};
    wcWarn.lpfnWndProc = WarningWndProc;
    wcWarn.hInstance = hInstance;
    wcWarn.lpszClassName = kWarningClass;
    if (!RegisterClassA(&wcWarn) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        LogError("[PseudoOverlay] Failed to register warning window class");
        DestroyWindow(hOv_);
        hOv_ = NULL;
        CleanupGDI();
        return false;
    }

    // Create warning overlay window
    hWarn_ = CreateWindowExA(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW, kWarningClass, "",
                             WS_POPUP, 0, 0, 0, 0, NULL, NULL, hInstance, 0);
    if (!hWarn_) {
        LogError("[PseudoOverlay] Failed to create warning overlay window");
        DestroyWindow(hOv_);
        hOv_ = NULL;
        CleanupGDI();
        return false;
    }

    // Start timer for blink cycle and periodic refresh on indicator window
    SetTimer(hOv_, kTimerId, kTimerInterval, NULL);

    initialized_ = true;
    LogInfo("[PseudoOverlay] Initialized (scale=%.2f)", scale_);

    // Initial render
    UpdateOverlay();

    return true;
}

void PseudoOverlay::Shutdown() {
    if (!initialized_)
        return;

    if (hOv_) {
        KillTimer(hOv_, kTimerId);
        DestroyWindow(hOv_);
        hOv_ = NULL;
    }
    if (hWarn_) {
        DestroyWindow(hWarn_);
        hWarn_ = NULL;
    }

    CleanupGDI();

    // Reset tracking state
    lastOv_ = {};
    lastCol_ = 0;
    lastWarnVis_ = false;
    lastWarnMsg_.clear();
    warnActive_ = false;
    warnVisible_ = false;
    warnCycleStart_ = 0;

    initialized_ = false;
    instance_ = nullptr;

    LogInfo("[PseudoOverlay] Shutdown complete");
}

// ---- State updates ----

void PseudoOverlay::UpdateConfig(const PseudoOverlayConfig& cfg) {
    bool wasEnabled = config_.enabled;
    config_ = cfg;

    if (wasEnabled && !cfg.enabled) {
        // Disable overlay: hide windows
        if (hOv_)
            ShowWindow(hOv_, SW_HIDE);
        if (hWarn_)
            ShowWindow(hWarn_, SW_HIDE);
        return;
    }

    if (cfg.enabled && initialized_) {
        UpdateOverlay();
    }
}

void PseudoOverlay::SetRecordingState(bool recording) {
    isRecording_.store(recording);
    if (initialized_) {
        UpdateOverlay();
        InvalidateRect(hOv_, NULL, FALSE);
    }
}

void PseudoOverlay::TriggerEncoderOverloadWarning() {
    overloadWarnUntil_.store(GetTickCount64() + 5000ULL);
    if (initialized_) {
        UpdateOverlay();
    }
}
