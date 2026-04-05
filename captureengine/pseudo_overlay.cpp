// Pseudo-overlay indicator for WGC capture (no injection required).
// Ported from OBSIndicator with permission (MIT license).
// Runs entirely in the captureengine controller process.

#include "pseudo_overlay.h"
#include "../common/logging.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <string>

// ---- Palette (matching OBSIndicator exactly) ----
static constexpr COLORREF kColWarnText = RGB(255, 20, 20);
static constexpr COLORREF kColScreenshotText = RGB(20, 255, 20);

namespace {
std::string NormalizeProcessName(std::string value) {
    static constexpr const char* kTrimChars = " \t\r\n\"";

    const size_t first = value.find_first_not_of(kTrimChars);
    if (first == std::string::npos)
        return {};

    const size_t last = value.find_last_not_of(kTrimChars);
    value = value.substr(first, last - first + 1);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool GetMonitorRectForWindow(HWND hwnd, RECT* rect) {
    if (!rect) {
        return false;
    }

    HMONITOR monitor = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
                            : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (!monitor) {
        return false;
    }

    MONITORINFO monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfo(monitor, &monitorInfo)) {
        return false;
    }

    *rect = monitorInfo.rcMonitor;
    return true;
}

bool GetMonitorRectForMonitor(HMONITOR monitor, RECT* rect) {
    if (!monitor || !rect) {
        return false;
    }

    MONITORINFO monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfo(monitor, &monitorInfo)) {
        return false;
    }

    *rect = monitorInfo.rcMonitor;
    return true;
}

UINT GetResolvedWindowDpi(HWND hwnd) {
    UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 0u;
    if (dpi == 0) {
        dpi = GetDpiForSystem();
    }
    return dpi == 0 ? 96u : dpi;
}

std::string FormatEncoderOverloadMessage(uint32_t sustainFpsX100, uint32_t targetFps) {
    const double sustainFps = static_cast<double>(sustainFpsX100) / 100.0;
    if (targetFps == 0 || sustainFpsX100 == 0) {
        return "Encoder overloaded!";
    }

    const double ratio = sustainFps / static_cast<double>(targetFps);
    char buffer[96];
    if (ratio >= 0.95) {
        std::snprintf(buffer, sizeof(buffer), "Encoder near limit (%.1f/%ufps)", sustainFps, targetFps);
    } else if (ratio >= 0.80) {
        std::snprintf(buffer, sizeof(buffer), "Encoder overloaded (%.1f/%ufps)", sustainFps, targetFps);
    } else {
        std::snprintf(buffer, sizeof(buffer), "Encoder severely overloaded (%.1f/%ufps)", sustainFps, targetFps);
    }
    return buffer;
}

bool GetWindowClientRectInScreen(HWND hwnd, RECT& rect) {
    RECT clientRect = {};
    if (!GetClientRect(hwnd, &clientRect)) {
        return false;
    }

    POINT topLeft = {clientRect.left, clientRect.top};
    POINT bottomRight = {clientRect.right, clientRect.bottom};
    if (!ClientToScreen(hwnd, &topLeft) || !ClientToScreen(hwnd, &bottomRight)) {
        return false;
    }

    rect.left = topLeft.x;
    rect.top = topLeft.y;
    rect.right = bottomRight.x;
    rect.bottom = bottomRight.y;
    return true;
}

bool RectNearlyMatches(const RECT& lhs, const RECT& rhs, LONG tolerance) {
    auto absDiff = [](LONG a, LONG b) -> LONG { return (a >= b) ? (a - b) : (b - a); };

    return absDiff(lhs.left, rhs.left) <= tolerance && absDiff(lhs.top, rhs.top) <= tolerance &&
           absDiff(lhs.right, rhs.right) <= tolerance && absDiff(lhs.bottom, rhs.bottom) <= tolerance;
}

// Keep the pseudo-overlay fullscreen-like heuristic aligned with media_main's
// window detection so controller-side overlay behavior tracks capture policy.
bool IsWindowFullscreenLike(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return false;
    }

    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!monitor) {
        return false;
    }

    MONITORINFO monitorInfo = {sizeof(monitorInfo)};
    if (!GetMonitorInfo(monitor, &monitorInfo)) {
        return false;
    }

    RECT windowRect = {};
    RECT clientRect = {};
    const bool haveWindowRect = GetWindowRect(hwnd, &windowRect) != FALSE;
    const bool haveClientRect = GetWindowClientRectInScreen(hwnd, clientRect);
    constexpr LONG kFullscreenTolerancePx = 8;

    if (!haveWindowRect && !haveClientRect) {
        return false;
    }

    const bool windowMatchesMonitor =
        haveWindowRect && RectNearlyMatches(windowRect, monitorInfo.rcMonitor, kFullscreenTolerancePx);
    const bool clientMatchesMonitor =
        haveClientRect && RectNearlyMatches(clientRect, monitorInfo.rcMonitor, kFullscreenTolerancePx);
    return windowMatchesMonitor || clientMatchesMonitor;
}

struct WindowSearch {
    DWORD pid = 0;
    HWND hwnd = NULL;
};

BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam) {
    auto* search = reinterpret_cast<WindowSearch*>(lParam);
    if (!search || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != 0) {
        return TRUE;
    }

    DWORD windowPid = 0;
    GetWindowThreadProcessId(hwnd, &windowPid);
    if (windowPid == search->pid) {
        LONG_PTR styles = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
        if (!(styles & WS_EX_TOOLWINDOW)) {
            search->hwnd = hwnd;
            return FALSE;
        }
    }

    return TRUE;
}

HWND GetMainWindowForProcess(DWORD pid) {
    if (pid == 0) {
        return NULL;
    }

    WindowSearch search = {pid, NULL};
    EnumWindows(EnumWindowsCallback, reinterpret_cast<LPARAM>(&search));
    return search.hwnd;
}
}  // namespace

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

void PseudoOverlay::UpdateScaleForDpi(UINT dpi) {
    const UINT resolvedDpi = dpi == 0 ? 96u : dpi;
    if (resolvedDpi == currentDpi_ && fontWarn_) {
        return;
    }

    currentDpi_ = resolvedDpi;
    scale_ = static_cast<float>(currentDpi_) / 96.0f;
    sizeWarn_ = {0, 0};
    lastWarnMsg_.clear();

    if (bmWarn_) {
        if (hdcWarn_ && oldBmWarn_) {
            SelectObject(hdcWarn_, oldBmWarn_);
        }
        DeleteObject(bmWarn_);
        bmWarn_ = NULL;
    }

    if (fontWarn_) {
        DeleteObject(fontWarn_);
        fontWarn_ = NULL;
    }

    fontWarn_ = CreateFontA(-S(40), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, "Segoe UI");
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
            exeName = NormalizeProcessName(exeName);

            std::stringstream ss(config_.processList);
            std::string item;
            while (std::getline(ss, item, '|')) {
                std::string normalizedItem = NormalizeProcessName(item);
                if (normalizedItem.empty())
                    continue;

                if (exeName == normalizedItem) {
                    match = true;
                    break;
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

// ---- Inject overlay detection via shared memory ----

bool PseudoOverlay::EnsureSharedMemoryMapping() {
    if (!hDiscoveryMap_) {
        hDiscoveryMap_ = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
        if (!hDiscoveryMap_) {
            return false;
        }
    }

    if (!pDiscovery_) {
        pDiscovery_ = (DiscoveryInfo*)MapViewOfFile(hDiscoveryMap_, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));
        if (!pDiscovery_) {
            CloseHandle(hDiscoveryMap_);
            hDiscoveryMap_ = NULL;
            return false;
        }
    }

    if (pDiscovery_->GetMagic() != DISCOVERY_MAGIC) {
        return false;
    }

    const uint32_t injectPid = pDiscovery_->GetInjectPid();
    if (injectPid == 0) {
        if (pSharedMem_) {
            UnmapViewOfFile(pSharedMem_);
            pSharedMem_ = nullptr;
        }
        if (hSharedMemMap_) {
            CloseHandle(hSharedMemMap_);
            hSharedMemMap_ = NULL;
        }
        mappedInjectPid_ = 0;
        return false;
    }

    if (pSharedMem_ && mappedInjectPid_ == injectPid) {
        return true;
    }

    if (pSharedMem_) {
        UnmapViewOfFile(pSharedMem_);
        pSharedMem_ = nullptr;
    }
    if (hSharedMemMap_) {
        CloseHandle(hSharedMemMap_);
        hSharedMemMap_ = NULL;
    }

    wchar_t sharedMemName[64];
    GenerateSharedMemName(sharedMemName, 64, injectPid);
    hSharedMemMap_ = OpenFileMappingW(FILE_MAP_READ, FALSE, sharedMemName);
    if (!hSharedMemMap_) {
        mappedInjectPid_ = 0;
        return false;
    }

    pSharedMem_ = (SharedMemoryLayout*)MapViewOfFile(hSharedMemMap_, FILE_MAP_READ, 0, 0, sizeof(SharedMemoryLayout));
    if (!pSharedMem_) {
        CloseHandle(hSharedMemMap_);
        hSharedMemMap_ = NULL;
        mappedInjectPid_ = 0;
        return false;
    }

    mappedInjectPid_ = injectPid;
    return true;
}

bool PseudoOverlay::IsInjectOverlayActive() {
    return EnsureSharedMemoryMapping() &&
           pSharedMem_->runtimeState.HasRuntimeFlag(kCaptureRuntimeFlagInjectOverlayActive);
}

bool PseudoOverlay::IsInjectOverlayPending() {
    return EnsureSharedMemoryMapping() &&
           pSharedMem_->runtimeState.HasRuntimeFlag(kCaptureRuntimeFlagInjectOverlayPending);
}

PseudoOverlay::AnchorInfo PseudoOverlay::ResolveAnchorInfo() {
    AnchorInfo anchor;

    HWND anchorWindow = GetForegroundWindow();
    if (anchorWindow && (!IsWindow(anchorWindow) || !IsWindowVisible(anchorWindow) || GetWindow(anchorWindow, GW_OWNER))) {
        anchorWindow = NULL;
    }

    if (!anchorWindow && EnsureSharedMemoryMapping() && pSharedMem_) {
        anchorWindow = GetMainWindowForProcess(pSharedMem_->GetSourcePid());
    }

    anchor.window = anchorWindow;
    anchor.monitor = anchorWindow ? MonitorFromWindow(anchorWindow, MONITOR_DEFAULTTONEAREST)
                                  : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (!GetMonitorRectForMonitor(anchor.monitor, &anchor.monitorRect)) {
        anchor.monitorRect.left = 0;
        anchor.monitorRect.top = 0;
        anchor.monitorRect.right = GetSystemMetrics(SM_CXSCREEN);
        anchor.monitorRect.bottom = GetSystemMetrics(SM_CYSCREEN);
    }

    anchor.dpi = GetResolvedWindowDpi(anchorWindow);
    anchor.fullscreenLike = IsWindowFullscreenLike(anchorWindow);
    return anchor;
}

// ---- GDI helpers ----

void PseudoOverlay::InitGDI() {
    HDC hdcScreen = GetDC(NULL);
    hdcWarn_ = CreateCompatibleDC(hdcScreen);
    if (hdcWarn_)
        oldBmWarn_ = (HBITMAP)GetCurrentObject(hdcWarn_, OBJ_BITMAP);
    ReleaseDC(NULL, hdcScreen);

    UpdateScaleForDpi(currentDpi_);
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

    if (!config_.enabled) {
        if (IsWindowVisible(hOv_))
            ShowWindow(hOv_, SW_HIDE);
        if (IsWindowVisible(hWarn_))
            ShowWindow(hWarn_, SW_HIDE);
        lastOv_ = {};
        lastWarnVis_ = false;
        lastOverlaySuppressed_ = false;
        lastFullscreenSuppressed_ = false;
        return;
    }

    // Suppress when inject overlay is active in a hooked game
    const bool suppressOverlay = IsInjectOverlayPending() || IsInjectOverlayActive();
    if (suppressOverlay) {
        if (!lastOverlaySuppressed_) {
            LogInfo("[PseudoOverlay] Suppressed while inject overlay handoff is active");
        }
        if (IsWindowVisible(hOv_))
            ShowWindow(hOv_, SW_HIDE);
        if (IsWindowVisible(hWarn_))
            ShowWindow(hWarn_, SW_HIDE);
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

    const AnchorInfo anchor = ResolveAnchorInfo();
    if (anchor.fullscreenLike) {
        if (!lastFullscreenSuppressed_) {
            LogInfo("[PseudoOverlay] Fullscreen-like anchor detected; using reduced-impact mode");
            lastFullscreenSuppressed_ = true;
        }
    } else if (lastFullscreenSuppressed_) {
        LogInfo("[PseudoOverlay] Fullscreen-like anchor cleared; restoring normal mode");
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

    bool rec = isRecording_.load();
    bool showInd = false;
    if (rec && config_.mode != 2)  // MODE_WARN_ONLY
        showInd = true;

    // Keep ghost mode disabled on layered pseudo-overlay windows. A hidden
    // window is safer for DirectFlip/MPO/VRR than a 1px keepalive surface.
    bool ghostActive = false;
    BYTE indAlpha = showInd ? 255 : 0;

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
            SetWindowPos(hOv_, NULL, winX, winY, fullS, fullS, SWP_NOACTIVATE | SWP_NOZORDER);
        } else {
            ShowWindow(hOv_, SW_HIDE);
        }

        if (indAlpha > 0) {
            HDC hdcScreen = GetDC(NULL);
            if (hdcScreen) {
                HDC hdcMem = CreateCompatibleDC(hdcScreen);
                if (hdcMem) {
                    HBITMAP hBm = CreateCompatibleBitmap(hdcScreen, fullS, fullS);
                    if (hBm) {
                        HBITMAP hOldBm = (HBITMAP)SelectObject(hdcMem, hBm);

                        HBRUSH hBlack = CreateSolidBrush(RGB(0, 0, 0));
                        RECT r = {0, 0, fullS, fullS};
                        FillRect(hdcMem, &r, hBlack);
                        DeleteObject(hBlack);

                        HPEN hPen = CreatePen(PS_SOLID, S(2), RGB(255, 255, 255));
                        HBRUSH hBrush = CreateSolidBrush(curCol);
                        HPEN hOldPen = (HPEN)SelectObject(hdcMem, hPen);
                        HBRUSH hOldBrush = (HBRUSH)SelectObject(hdcMem, hBrush);
                        Ellipse(hdcMem, indX + S(1), indY + S(1), indX + s - S(1), indY + s - S(1));
                        SelectObject(hdcMem, hOldPen);
                        SelectObject(hdcMem, hOldBrush);
                        DeleteObject(hBrush);
                        DeleteObject(hPen);

                        POINT ptDst = {winX, winY};
                        SIZE szWnd = {fullS, fullS};
                        POINT ptSrc = {0, 0};
                        BLENDFUNCTION blend = {AC_SRC_OVER, 0, indAlpha, 0};
                        DWORD flags = ULW_COLORKEY | ULW_ALPHA;
                        UpdateLayeredWindow(hOv_, hdcScreen, &ptDst, &szWnd, hdcMem, &ptSrc, RGB(0, 0, 0), &blend,
                                            flags);

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
    bool showScreenshot = now < screenshotNotifyUntil_.load();
    bool showOverload = !showScreenshot && config_.showEncoderOverloadWarn && (now < overloadWarnUntil_.load());
    bool showW = !anchor.fullscreenLike && (warnVisible_ || showOverload || showScreenshot);
    BYTE warnAlpha = 0;
    bool doUpdateWarn = false;

    const uint32_t overloadWarnSustainFpsX100 = this->overloadWarnSustainFpsX100_.load();
    uint32_t overloadTargetFps = 0;
    if (showOverload && EnsureSharedMemoryMapping() && pSharedMem_) {
        overloadTargetFps = pSharedMem_->runtimeState.wgcTargetFps.load(std::memory_order_relaxed);
    }
    const std::string overloadMsg = FormatEncoderOverloadMessage(overloadWarnSustainFpsX100, overloadTargetFps);
    const char* msg = showScreenshot ? "Screenshot saved!" : (showOverload ? overloadMsg.c_str() : "NOT RECORDING");
    bool isScreenshotMsg = showScreenshot;

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
            ShowWindow(hWarn_, SW_HIDE);
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
            SetTextColor(hdcWarn_, isScreenshotMsg ? kColScreenshotText : kColWarnText);
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
        DWORD flags = ULW_COLORKEY | ULW_ALPHA;
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, warnAlpha, 0};
        UpdateLayeredWindow(hWarn_, NULL, &ptDst, &szWnd, hdcWarn_, &ptSrc, RGB(0, 0, 0), &blend, flags);

        lastWarnVis_ = warnAlpha > 0;
    }
}

// ---- Window procedures ----

LRESULT CALLBACK PseudoOverlay::IndicatorWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) {
        return MA_NOACTIVATE;
    }
    if (m == WM_NCHITTEST) {
        return HTTRANSPARENT;
    }
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

        // Check screenshot notification expiry
        {
            static ULONGLONG lastScreenshotNotifyUntil = 0;
            ULONGLONG current = self->screenshotNotifyUntil_.load();
            if ((lastScreenshotNotifyUntil > 0 && current == 0) || (current > 0 && GetTickCount64() > current)) {
                self->UpdateOverlay();
            }
            lastScreenshotNotifyUntil = current;
        }

        if (self->config_.showEncoderOverloadWarn && self->EnsureSharedMemoryMapping() && self->pSharedMem_) {
            const uint32_t overloadFlags =
                self->pSharedMem_->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
            if (overloadFlags != 0) {
                ULONGLONG current = GetTickCount64();
                // Update if it's newly overloaded, or if our timer is halfway expired, or FPS changed significantly
                uint32_t currentFps =
                    self->pSharedMem_->runtimeState.encoderSustainFpsX100.load(std::memory_order_relaxed);
                uint32_t lastFps = self->overloadWarnSustainFpsX100_.load(std::memory_order_relaxed);
                bool fpsChanged =
                    (currentFps > lastFps ? currentFps - lastFps : lastFps - currentFps) > 100;  // 1 fps difference

                if (self->lastEncoderOverloadFlags_ == 0 || (current > self->overloadWarnUntil_.load() - 2500) ||
                    fpsChanged) {
                    self->TriggerEncoderOverloadWarning(currentFps);
                }
            }
            self->lastEncoderOverloadFlags_ = overloadFlags;
        }

        // Periodic refresh in info mode
        if (self->config_.mode == 0)
            self->UpdateOverlay();

        return 0;
    }
    return DefWindowProc(h, m, w, l);
}

LRESULT CALLBACK PseudoOverlay::WarningWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) {
        return MA_NOACTIVATE;
    }
    if (m == WM_NCHITTEST) {
        return HTTRANSPARENT;
    }
    return DefWindowProc(h, m, w, l);
}

// ---- Init / Shutdown ----

bool PseudoOverlay::Init(HINSTANCE hInstance) {
    if (initialized_)
        return true;

    instance_ = this;
    currentDpi_ = 96;
    scale_ = 1.0f;
    UpdateScaleForDpi(GetDpiForSystem());

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
    hOv_ = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kIndicatorClass, "",
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
    hWarn_ = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kWarningClass, "",
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

    const AnchorInfo anchor = ResolveAnchorInfo();
    initialized_ = true;
    LogInfo("[PseudoOverlay] Initialized (scale=%.2f)", scale_);
    LogInfo("[PseudoOverlay] Initial anchor: monitor=%p window=%p dpi=%u fullscreenLike=%d", anchor.monitor,
            anchor.window, currentDpi_, anchor.fullscreenLike ? 1 : 0);

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

    // Release shared memory handles
    if (pSharedMem_) {
        UnmapViewOfFile(pSharedMem_);
        pSharedMem_ = nullptr;
    }
    if (pDiscovery_) {
        UnmapViewOfFile(pDiscovery_);
        pDiscovery_ = nullptr;
    }
    if (hSharedMemMap_) {
        CloseHandle(hSharedMemMap_);
        hSharedMemMap_ = NULL;
    }
    if (hDiscoveryMap_) {
        CloseHandle(hDiscoveryMap_);
        hDiscoveryMap_ = NULL;
    }

    // Reset tracking state
    lastOv_ = {};
    lastCol_ = 0;
    lastWarnVis_ = false;
    lastWarnMsg_.clear();
    warnActive_ = false;
    warnVisible_ = false;
    warnCycleStart_ = 0;
    mappedInjectPid_ = 0;
    lastEncoderOverloadFlags_ = 0;
    overloadWarnSustainFpsX100_.store(0, std::memory_order_relaxed);
    lastOverlaySuppressed_ = false;
    lastFullscreenSuppressed_ = false;

    initialized_ = false;
    instance_ = nullptr;

    LogInfo("[PseudoOverlay] Shutdown complete");
}

// ---- State updates ----

void PseudoOverlay::UpdateConfig(const PseudoOverlayConfig& cfg) {
    bool wasEnabled = config_.enabled;
    config_ = cfg;
    config_.alwaysRender = false;
    config_.alwaysRenderOnlyWhenGame = false;
    lastWarnMsg_.clear();
    sizeWarn_ = {0, 0};

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

void PseudoOverlay::TriggerEncoderOverloadWarning(uint32_t sustainFpsX100) {
    overloadWarnSustainFpsX100_.store(sustainFpsX100, std::memory_order_relaxed);
    overloadWarnUntil_.store(GetTickCount64() + 5000ULL);
    if (initialized_) {
        UpdateOverlay();
    }
}

void PseudoOverlay::ShowScreenshotNotification() {
    screenshotNotifyUntil_.store(GetTickCount64() + 2000ULL);
    if (initialized_) {
        UpdateOverlay();
    }
}
