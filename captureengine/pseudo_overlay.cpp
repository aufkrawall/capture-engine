// Pseudo-overlay indicator for WGC capture (no injection required).
// Ported from OBSIndicator with permission (MIT license).
// Runs entirely in the captureengine controller process.

#include "pseudo_overlay.h"
#include "../common/capture_pipeline_policy.h"
#include "../common/inject_overlay_policy.h"
#include "../common/logging.h"
#include "../common/process_identity.h"
#include "../common/pseudo_overlay_profile_policy.h"
#include "../common/pseudo_overlay_visibility.h"
#include "../common/secure_dll_loading.h"

#include <dwmapi.h>

namespace {
typedef HRESULT(WINAPI* DwmSetWindowAttributeFn)(HWND, DWORD, LPCVOID, DWORD);
DwmSetWindowAttributeFn g_DwmSetWindowAttribute = nullptr;
bool g_DwmApiInitialized = false;

void EnsureDwmApi() {
    if (g_DwmApiInitialized)
        return;
    g_DwmApiInitialized = true;
    HMODULE mod = ce::security::LoadSystemLibrary(L"dwmapi.dll");
    if (mod) {
        g_DwmSetWindowAttribute = (DwmSetWindowAttributeFn)GetProcAddress(mod, "DwmSetWindowAttribute");
    }
}
}  // namespace

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

// ---- Palette (matching OBSIndicator exactly) ----
static constexpr COLORREF kColWarnText = RGB(255, 20, 20);
static constexpr COLORREF kColScreenshotText = RGB(20, 255, 20);
static constexpr COLORREF kColStarting = RGB(255, 191, 0);

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

std::string QueryProcessName(DWORD pid) {
    const ce::process::ProcessIdentityResult identity = ce::process::QueryProcessIdentity(pid);
    return NormalizeProcessName(identity.imageName);
}

bool PseudoOverlayConfigsEqual(const PseudoOverlayConfig& lhs, const PseudoOverlayConfig& rhs) {
    return lhs.enabled == rhs.enabled && lhs.size == rhs.size && lhs.pad == rhs.pad && lhs.pos == rhs.pos &&
           lhs.mode == rhs.mode && lhs.alwaysRender == rhs.alwaysRender &&
           lhs.alwaysRenderOnlyWhenGame == rhs.alwaysRenderOnlyWhenGame &&
           lhs.showEncoderOverloadWarn == rhs.showEncoderOverloadWarn &&
           lhs.foregroundAcquireGraceMs == rhs.foregroundAcquireGraceMs && lhs.processList == rhs.processList;
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

static HBITMAP CreateArgbDibSection(int width, int height, void** ppBits) {
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, ppBits, NULL, 0);
}

static void ApplyPremultipliedAlpha(void* pBits, int width, int height) {
    DWORD* px = static_cast<DWORD*>(pBits);
    for (int i = 0, n = width * height; i < n; ++i) {
        DWORD v = px[i];
        BYTE r = static_cast<BYTE>(v & 0xFF);
        BYTE g = static_cast<BYTE>((v >> 8) & 0xFF);
        BYTE b = static_cast<BYTE>((v >> 16) & 0xFF);
        BYTE a = r;
        if (g > a)
            a = g;
        if (b > a)
            a = b;
        px[i] = (a << 24) | (v & 0x00FFFFFF);
    }
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

    MONITORINFO monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
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

bool ShouldOverlayBeVisible(const PseudoOverlayConfig& config, ce::recording_indicator::State recordingState,
                            bool warnVisible,
                            ULONGLONG overloadWarnUntil, ULONGLONG screenshotNotifyUntil, bool ghostActive) {
    // Delegate to the pure, unit-tested policy helper. The helper gates the NOT-RECORDING
    // warning on the resolved idle state so it can never leak into pending or active recording (see
    // common/pseudo_overlay_visibility.h and tests/test_pseudo_overlay_visibility.cpp).
    ce::pseudo_overlay::OverlayVisibilityInputs in;
    in.mode = config.mode;
    in.recordingState = recordingState;
    in.warnVisible = warnVisible;
    in.showEncoderOverloadWarn = config.showEncoderOverloadWarn;
    in.ghostActive = ghostActive;
    in.nowMs = GetTickCount64();
    in.overloadWarnUntilMs = overloadWarnUntil;
    in.screenshotNotifyUntilMs = screenshotNotifyUntil;
    return ce::pseudo_overlay::ShouldPseudoOverlayBeVisible(in);
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

    fontWarn_ =
        CreateFontA(-S(40), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, ANTIALIASED_QUALITY, 0, 0, 0, "Segoe UI");
}

// ---- Foreground process/profile detection ----

void PseudoOverlay::RefreshActiveProfileConfig() {
    HWND foregroundWindow = GetForegroundWindow();
    DWORD foregroundPid = 0;
    if (foregroundWindow)
        GetWindowThreadProcessId(foregroundWindow, &foregroundPid);

    if (foregroundPid != foregroundPid_ || (foregroundPid != 0 && foregroundProcessName_.empty())) {
        foregroundPid_ = foregroundPid;
        foregroundProcessName_ = QueryProcessName(foregroundPid);
        LogDebug("[PseudoOverlay] Foreground process changed: pid=%lu process=%s", foregroundPid,
                 foregroundProcessName_.empty() ? "unknown" : foregroundProcessName_.c_str());
    }

    const PseudoOverlayApplicationConfig* foregroundProfile =
        ce::pseudo_overlay::FindApplicationConfig(profileConfigs_, foregroundProcessName_);
    foregroundIsTarget_ = ce::pseudo_overlay::IsForegroundWarningTarget(
        foregroundProfile, baseConfig_.processList, foregroundProcessName_);

    const bool recordingActive = ce::recording_indicator::IsVisible(recordingIndicatorState_);
    if (!recordingActive) {
        pinnedProfileSection_.clear();
    } else {
        // Once a hook has published the actual source PID, it is stronger evidence
        // than whichever process happened to own foreground at the hotkey edge.
        const PseudoOverlayApplicationConfig* sourceProfile = nullptr;
        if (EnsureSharedMemoryMapping() && pSharedMem_) {
            const uint32_t sourcePid = pSharedMem_->GetSourcePid();
            if (sourcePid != 0) {
                if (sourcePid != sourceProfilePid_ || sourceProcessName_.empty()) {
                    sourceProfilePid_ = sourcePid;
                    sourceProcessName_ = QueryProcessName(sourcePid);
                }
                sourceProfile = ce::pseudo_overlay::FindApplicationConfig(profileConfigs_, sourceProcessName_);
                if (sourceProfile && !sourceProfile->captureUsesInjection)
                    sourceProfile = nullptr;
            }
        }
        if (sourceProfile && (!foregroundProfile || foregroundProfile->captureUsesInjection)) {
            pinnedProfileSection_ = sourceProfile->section;
        } else if (pinnedProfileSection_.empty() && foregroundProfile) {
            // WGC/DXGI routes have no injected source PID. Pin the foreground
            // profile selected at recording start so Alt+Tab does not change the
            // recording indicator's appearance midway through the session.
            pinnedProfileSection_ = foregroundProfile->section;
        }
    }

    const PseudoOverlayApplicationConfig* activeProfile = nullptr;
    if (!pinnedProfileSection_.empty()) {
        const auto pinned = std::find_if(
            profileConfigs_.begin(), profileConfigs_.end(), [&](const PseudoOverlayApplicationConfig& profile) {
                return _stricmp(profile.section.c_str(), pinnedProfileSection_.c_str()) == 0;
            });
        if (pinned != profileConfigs_.end())
            activeProfile = &*pinned;
        else
            pinnedProfileSection_.clear();
    }
    if (!activeProfile && !recordingActive)
        activeProfile = foregroundProfile;

    ApplyEffectiveConfig(activeProfile ? activeProfile->settings : baseConfig_,
                         activeProfile ? activeProfile->section : std::string{});
}

bool PseudoOverlay::IsForegroundTarget() {
    return foregroundIsTarget_;
}

uint32_t PseudoOverlay::GetForegroundTargetPid() {
    return foregroundPid_;
}

void PseudoOverlay::UpdateForegroundGraceState(bool currentHadTarget, uint32_t currentPid) {
    // Detection: a foreground-acquire transition is either (a) the first time we ever
    // see a whitelisted PID, (b) the PID changed while still whitelisted, or (c) we
    // had no target last tick but have one now. The grace tracking state only
    // advances on these transitions so the grace timer measures from the latest
    // acquire edge, not from init.
    const bool pidChanged = lastForegroundAcquirePid_ != 0 && lastForegroundAcquirePid_ != currentPid;
    const bool firstDetection = lastForegroundAcquireTick_ == 0;
    const bool isTransition = firstDetection || pidChanged || (!hadForegroundTarget_ && currentHadTarget);

    if (currentHadTarget && isTransition) {
        const ULONGLONG now = GetTickCount64();
        lastForegroundAcquireTick_ = now;
        lastForegroundAcquirePid_ = currentPid;
        if (config_.foregroundAcquireGraceMs > 0) {
            foregroundGraceEverStarted_ = true;
            LogInfo("[PseudoOverlay] Foreground grace started pid=%lu grace=%ums (was: hadTarget=%d, prevPid=%lu)",
                    static_cast<unsigned long>(currentPid), config_.foregroundAcquireGraceMs,
                    hadForegroundTarget_ ? 1 : 0,
                    static_cast<unsigned long>(lastForegroundAcquirePid_));
        } else {
            LogDebug("[PseudoOverlay] Foreground grace skipped: grace_ms=0 (pid=%lu)",
                     static_cast<unsigned long>(currentPid));
        }
    } else if (!currentHadTarget) {
        if (hadForegroundTarget_) {
            LogInfo("[PseudoOverlay] Foreground grace aborted: focus_lost (was pid=%lu)",
                    static_cast<unsigned long>(lastForegroundAcquirePid_));
        }
        // Clear the tracking state so the next acquire is treated as a fresh transition.
        lastForegroundAcquireTick_ = 0;
        lastForegroundAcquirePid_ = 0;
    }

    hadForegroundTarget_ = currentHadTarget;
}

ce::pseudo_overlay::FocusGraceDecision PseudoOverlay::EvaluateForegroundGrace(bool currentHadTarget,
                                                                              uint32_t currentPid, ULONGLONG now) {
    const bool recordingChanged = prevRecordingIndicatorState_ != recordingIndicatorState_;
    prevRecordingIndicatorState_ = recordingIndicatorState_;

    auto decision = ce::pseudo_overlay::ComputeFocusGraceDecision(
        now, lastForegroundAcquireTick_, lastForegroundAcquirePid_, currentPid, hadForegroundTarget_, currentHadTarget,
        prevGraceActive_, static_cast<uint32_t>(config_.foregroundAcquireGraceMs), recordingChanged);

    if (decision.justEndedGrace && foregroundGraceEverStarted_) {
        ULONGLONG waited = 0;
        if (lastForegroundAcquireTick_ != 0 && now >= lastForegroundAcquireTick_) {
            waited = now - lastForegroundAcquireTick_;
        }
        LogInfo("[PseudoOverlay] Foreground grace elapsed pid=%lu waited=%lums",
                static_cast<unsigned long>(currentPid),
                static_cast<unsigned long>(waited));
        foregroundGraceEverStarted_ = false;
    }
    if (decision.justStartedGrace && config_.foregroundAcquireGraceMs > 0) {
        // UpdateForegroundGraceState() already logged the start; nothing to do here.
    }

    prevGraceActive_ = decision.graceActive;
    return decision;
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

    if (!ValidateDiscoveryInfo(pDiscovery_)) {
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
    if (!ValidateSharedMemory(pSharedMem_)) {
        LogError("[PseudoOverlay] Rejected incompatible inject shared memory ABI (version=%u size=%u abi=0x%08X)",
                 pSharedMem_->GetVersion(), pSharedMem_->structSize.load(std::memory_order_acquire),
                 pSharedMem_->abiSignature.load(std::memory_order_acquire));
        UnmapViewOfFile(pSharedMem_);
        pSharedMem_ = nullptr;
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

bool PseudoOverlay::RefreshRecordingState() {
    bool recordingActive = false;
    bool recordingAudioOnly = false;
    RecordingStartIntent sharedIntent = RecordingStartIntent::Idle;
    const bool haveSharedState = EnsureSharedMemoryMapping() && pSharedMem_;
    if (haveSharedState) {
        recordingActive = pSharedMem_->runtimeState.isRecording.load(std::memory_order_acquire);
        recordingAudioOnly = pSharedMem_->runtimeState.audioOnly.load(std::memory_order_acquire);
        sharedIntent = pSharedMem_->runtimeState.GetRecordingStartIntent();
    }

    RecordingStartIntent startIntent = sharedIntent;
    if (startIntent == RecordingStartIntent::Idle) {
        startIntent = requestedStartIntent_.load(std::memory_order_acquire);
    }
    const auto nextState =
        ce::recording_indicator::SelectState(recordingActive, recordingAudioOnly, startIntent);
    if (recordingActive) {
        requestedStartIntent_.store(RecordingStartIntent::Idle, std::memory_order_release);
    }

    if (nextState == recordingIndicatorState_) {
        return false;
    }

    LogInfo("[PseudoOverlay] Recording indicator state %u -> %u",
            static_cast<unsigned>(recordingIndicatorState_), static_cast<unsigned>(nextState));
    recordingIndicatorState_ = nextState;
    publishedRecordingIndicatorState_.store(nextState, std::memory_order_release);
    isRecording_.store(ce::recording_indicator::IsRecording(nextState), std::memory_order_release);
    if (nextState != ce::recording_indicator::State::Idle) {
        warnActive_ = false;
        warnVisible_ = false;
    }
    return true;
}

PseudoOverlay::AnchorInfo PseudoOverlay::ResolveAnchorInfo() {
    AnchorInfo anchor;

    auto isUsableAnchorWindow = [](HWND hwnd) -> bool {
        return hwnd && IsWindow(hwnd) && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == 0;
    };

    HWND sourceWindow = NULL;
    if (EnsureSharedMemoryMapping() && pSharedMem_) {
        sourceWindow = GetMainWindowForProcess(pSharedMem_->GetSourcePid());
        if (!isUsableAnchorWindow(sourceWindow)) {
            sourceWindow = NULL;
        }
    }

    HWND anchorWindow = sourceWindow;
    if (!anchorWindow && isUsableAnchorWindow(this->stickyAnchorWindow_)) {
        anchorWindow = this->stickyAnchorWindow_;
    }

    HMONITOR anchorMonitor = NULL;
    if (anchorWindow) {
        anchorMonitor = MonitorFromWindow(anchorWindow, MONITOR_DEFAULTTONEAREST);
    }

    if (!anchorMonitor && this->stickyAnchorMonitor_) {
        RECT stickyRect = {};
        if (GetMonitorRectForMonitor(this->stickyAnchorMonitor_, &stickyRect)) {
            anchorMonitor = this->stickyAnchorMonitor_;
        }
    }

    if (!anchorMonitor) {
        const HWND foreground = GetForegroundWindow();
        if (isUsableAnchorWindow(foreground)) {
            anchorWindow = foreground;
            anchorMonitor = MonitorFromWindow(anchorWindow, MONITOR_DEFAULTTONEAREST);
        }
    }

    if (!anchorMonitor) {
        anchorMonitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    }

    anchor.window = anchorWindow;
    anchor.monitor = anchorMonitor;
    if (!GetMonitorRectForMonitor(anchor.monitor, &anchor.monitorRect)) {
        anchor.monitorRect.left = 0;
        anchor.monitorRect.top = 0;
        anchor.monitorRect.right = GetSystemMetrics(SM_CXSCREEN);
        anchor.monitorRect.bottom = GetSystemMetrics(SM_CYSCREEN);
    }

    anchor.dpi = anchorWindow ? GetResolvedWindowDpi(anchorWindow)
                              : (this->stickyAnchorDpi_ ? this->stickyAnchorDpi_ : GetDpiForSystem());
    anchor.fullscreenLike = IsWindowFullscreenLike(anchorWindow);

    if (anchorWindow) {
        if (this->stickyAnchorWindow_ != anchorWindow || this->stickyAnchorMonitor_ != anchor.monitor ||
            this->stickyAnchorDpi_ != anchor.dpi) {
            LogInfo("[PseudoOverlay] Sticky anchor updated: window=%p monitor=%p dpi=%u fullscreenLike=%d",
                    anchorWindow, anchor.monitor, anchor.dpi, anchor.fullscreenLike ? 1 : 0);
        }
        this->stickyAnchorWindow_ = anchorWindow;
        this->stickyAnchorMonitor_ = anchor.monitor;
        this->stickyAnchorDpi_ = anchor.dpi;
    } else if (this->stickyAnchorMonitor_ != anchor.monitor || this->stickyAnchorDpi_ != anchor.dpi) {
        LogInfo("[PseudoOverlay] Sticky anchor monitor fallback: monitor=%p dpi=%u", anchor.monitor, anchor.dpi);
        this->stickyAnchorMonitor_ = anchor.monitor;
        this->stickyAnchorDpi_ = anchor.dpi;
    }

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

void PseudoOverlay::OnTimerTick() {
    if (!initialized_.load(std::memory_order_acquire)) {
        return;
    }

    const int64_t tickStartUs = Log_GetQpcUs();
    ULONGLONG now = GetTickCount64();
    ApplyPendingConfig();
    const bool recordingStateChanged = RefreshRecordingState();
    RefreshActiveProfileConfig();

    // The pseudo-overlay owns this dedicated message thread. A large timer gap now
    // diagnoses work on this thread itself; controller readiness waits cannot starve it.
    if (lastTimerTickMs_ != 0 && now > lastTimerTickMs_) {
        const ULONGLONG tickGapMs = now - lastTimerTickMs_;
        if (tickGapMs >= kPumpStallWarnMs) {
            LogWarn(
                "[PseudoOverlay] UI-thread stall: %llums between timer ticks (interval=%ums)",
                static_cast<unsigned long long>(tickGapMs), kTimerInterval);
        }
    }
    lastTimerTickMs_ = now;

    // Foreground-acquire grace tracking. The decision (suppress or render) is evaluated
    // inside UpdateOverlay() so every entry path that calls UpdateOverlay() benefits
    // from the same gate, but the tracking state only advances on the timer tick to
    // avoid double-counting a transition.
    const uint32_t rawFgPid = GetForegroundTargetPid();
    const bool currentHadTarget = IsForegroundTarget();
    UpdateForegroundGraceState(currentHadTarget, currentHadTarget ? rawFgPid : 0u);
    (void)now;

    if (recordingStateChanged) {
        UpdateOverlay();
    }

    if (config_.mode == 1 || config_.mode == 2) {
        bool warnTargetFocused = IsForegroundTarget();
        const bool isRecording = ce::recording_indicator::IsRecording(recordingIndicatorState_);
        const bool isStarting = ce::recording_indicator::IsStarting(recordingIndicatorState_);
        bool condition = warnTargetFocused && !isRecording && !isStarting;

        if (condition) {
            if (!warnActive_) {
                LogInfo("[PseudoOverlay] NOT RECORDING warning activated (foreground target focused, not recording)");
                warnActive_ = true;
                warnCycleStart_ = now;
                warnVisible_ = true;
                UpdateOverlay();
            } else {
                ULONGLONG elapsed = now - warnCycleStart_;
                ULONGLONG cycleTime = elapsed % 3000;
                bool shouldBeVisible = (cycleTime < 2000);
                if (warnVisible_ != shouldBeVisible) {
                    warnVisible_ = shouldBeVisible;
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

    if (config_.showEncoderOverloadWarn && ce::recording_indicator::IsRecording(recordingIndicatorState_) &&
        EnsureSharedMemoryMapping() && pSharedMem_) {
        const uint32_t overloadFlags = pSharedMem_->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
        const uint32_t captureHealthFlags =
            pSharedMem_->runtimeState.wgcCaptureHealthFlags.load(std::memory_order_relaxed);
        const uint32_t warningKind = ce::capture_policy::SelectWgcOverlayWarningKind(overloadFlags, captureHealthFlags);
        if (ce::capture_policy::IsWgcCaptureLimitedForOverlay(captureHealthFlags)) {
            const ULONGLONG previousWarnUntil = overloadWarnUntil_.exchange(0, std::memory_order_relaxed);
            if (previousWarnUntil != 0 && initialized_.load(std::memory_order_acquire)) {
                UpdateOverlay();
            }
        } else if (warningKind == ce::capture_policy::kOverlayWarningEncoderOverload) {
            ULONGLONG current = GetTickCount64();
            uint32_t currentFps = pSharedMem_->runtimeState.encoderSustainFpsX100.load(std::memory_order_relaxed);
            uint32_t lastFps = overloadWarnSustainFpsX100_.load(std::memory_order_relaxed);
            bool fpsChanged = (currentFps > lastFps ? currentFps - lastFps : lastFps - currentFps) > 100;

            if ((lastEncoderOverloadFlags_ & ce::capture_policy::kEncoderOverloadFlagEncoder) == 0 ||
                (current > overloadWarnUntil_.load() - 2500) || fpsChanged) {
                TriggerEncoderOverloadWarning(currentFps);
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
        config_, recordingState, warnVisible_, overloadWarnUntil_.load(), screenshotNotifyUntil_.load(), ghostActive);

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
    textInputs.nowMs = now;
    const auto textKind = ce::pseudo_overlay::SelectPseudoOverlayText(textInputs);
    const bool showStarting = textKind == ce::pseudo_overlay::OverlayTextKind::Starting;
    const bool showScreenshot = textKind == ce::pseudo_overlay::OverlayTextKind::Screenshot;
    const bool showOverload = textKind == ce::pseudo_overlay::OverlayTextKind::EncoderOverload;
    const bool showW = textKind != ce::pseudo_overlay::OverlayTextKind::None;
    BYTE warnAlpha = 0;
    bool doUpdateWarn = false;

    const uint32_t overloadWarnSustainFpsX100 = this->overloadWarnSustainFpsX100_.load();
    uint32_t overloadTargetFps = 0;
    if (showOverload && EnsureSharedMemoryMapping() && pSharedMem_) {
        overloadTargetFps = pSharedMem_->runtimeState.wgcTargetFps.load(std::memory_order_relaxed);
    }
    const std::string overloadMsg = FormatEncoderOverloadMessage(overloadWarnSustainFpsX100, overloadTargetFps);
    const char* msg = showStarting ? ce::recording_indicator::GetStartingText(recordingState)
                      : showScreenshot ? "Screenshot saved!"
                      : showOverload   ? overloadMsg.c_str()
                                       : "NOT RECORDING";
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
            SetTextColor(hdcWarn_, showStarting ? kColStarting : (isScreenshotMsg ? kColScreenshotText : kColWarnText));
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
    }

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == kMsgShutdown) {
            break;
        }
        if (msg.message == kMsgRefresh) {
            ApplyPendingConfig();
            RefreshRecordingState();
            RefreshActiveProfileConfig();
            UpdateOverlay();
            continue;
        }
#ifdef CE_UNIT_TESTS
        if (msg.message == kMsgTestBarrier) {
            SetEvent(reinterpret_cast<HANDLE>(msg.wParam));
            continue;
        }
#endif
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ShutdownOnUiThread();
    uiThreadId_.store(0, std::memory_order_release);
}

bool PseudoOverlay::InitializeOnUiThread() {
    instance_ = this;

    currentDpi_ = 96;
    scale_ = 1.0f;
    UpdateScaleForDpi(GetDpiForSystem());

    InitGDI();

    // Register indicator window class
    WNDCLASSA wcInd = {};
    wcInd.lpfnWndProc = IndicatorWndProc;
    wcInd.hInstance = hInstance_;
    wcInd.lpszClassName = kIndicatorClass;
    if (!RegisterClassA(&wcInd) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        LogError("[PseudoOverlay] Failed to register indicator window class");
        CleanupGDI();
        return false;
    }

    // Register warning window class
    WNDCLASSA wcWarn = {};
    wcWarn.lpfnWndProc = WarningWndProc;
    wcWarn.hInstance = hInstance_;
    wcWarn.lpszClassName = kWarningClass;
    if (!RegisterClassA(&wcWarn) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        LogError("[PseudoOverlay] Failed to register warning window class");
        CleanupGDI();
        return false;
    }

    timerHandle_ = SetTimer(NULL, kTimerId, kTimerInterval, TimerProc);
    if (timerHandle_ == 0) {
        LogError("[PseudoOverlay] Failed to start timer");
        CleanupGDI();
        return false;
    }

    const AnchorInfo anchor = ResolveAnchorInfo();
    initialized_.store(true, std::memory_order_release);
    ApplyPendingConfig();
    RefreshRecordingState();
    RefreshActiveProfileConfig();
    LogInfo("[PseudoOverlay] Initialized (scale=%.2f)", scale_);
    LogInfo("[PseudoOverlay] Initial anchor: monitor=%p window=%p dpi=%u fullscreenLike=%d", anchor.monitor,
            anchor.window, currentDpi_, anchor.fullscreenLike ? 1 : 0);

    // Initial render
    UpdateOverlay();

    return true;
}

void PseudoOverlay::Shutdown() {
    if (!uiThread_.joinable())
        return;

    const DWORD threadId = uiThreadId_.load(std::memory_order_acquire);
    if (threadId != 0) {
        PostThreadMessageW(threadId, kMsgShutdown, 0, 0);
    }
    uiThread_.join();
    uiThreadId_.store(0, std::memory_order_release);
    hInstance_ = NULL;
}

void PseudoOverlay::ShutdownOnUiThread() {
    if (!initialized_.load(std::memory_order_acquire))
        return;

    if (timerHandle_ != 0) {
        KillTimer(NULL, timerHandle_);
        timerHandle_ = 0;
    }

    DestroyOverlayWindows();

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
    lastTimerTickMs_ = 0;
    mappedInjectPid_ = 0;
    lastEncoderOverloadFlags_ = 0;
    lastCaptureHealthFlags_ = 0;
    overloadWarnSustainFpsX100_.store(0, std::memory_order_relaxed);
    lastOverlaySuppressed_ = false;
    lastFullscreenSuppressed_ = false;
    stickyAnchorWindow_ = NULL;
    stickyAnchorMonitor_ = NULL;
    stickyAnchorDpi_ = 96;
    lastForegroundAcquireTick_ = 0;
    lastForegroundAcquirePid_ = 0;
    hadForegroundTarget_ = false;
    activeProfileSection_.clear();
    pinnedProfileSection_.clear();
    foregroundProcessName_.clear();
    foregroundPid_ = 0;
    sourceProcessName_.clear();
    sourceProfilePid_ = 0;
    foregroundIsTarget_ = false;
    prevRecordingIndicatorState_ = ce::recording_indicator::State::Idle;
    prevGraceActive_ = false;
    foregroundGraceEverStarted_ = false;
    recordingIndicatorState_ = ce::recording_indicator::State::Idle;
    publishedRecordingIndicatorState_.store(ce::recording_indicator::State::Idle, std::memory_order_release);
    isRecording_.store(false, std::memory_order_release);
    initialized_.store(false, std::memory_order_release);
    instance_ = nullptr;

    LogInfo("[PseudoOverlay] Shutdown complete");
}

// ---- State updates ----

void PseudoOverlay::UpdateConfig(const PseudoOverlayConfig& cfg,
                                 const std::vector<PseudoOverlayApplicationConfig>& profiles) {
    {
        std::lock_guard<std::mutex> lock(pendingConfigMutex_);
        pendingConfig_ = cfg;
        pendingProfileConfigs_ = profiles;
        pendingConfigGeneration_.fetch_add(1, std::memory_order_release);
    }
    PostRefresh();
}

void PseudoOverlay::ApplyPendingConfig() {
    const uint64_t generation = pendingConfigGeneration_.load(std::memory_order_acquire);
    if (generation == appliedConfigGeneration_) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(pendingConfigMutex_);
        baseConfig_ = pendingConfig_;
        profileConfigs_ = pendingProfileConfigs_;
    }
    appliedConfigGeneration_ = generation;
}

void PseudoOverlay::ApplyEffectiveConfig(const PseudoOverlayConfig& cfg, const std::string& profileSection) {
    if (PseudoOverlayConfigsEqual(config_, cfg) && activeProfileSection_ == profileSection)
        return;

    bool wasEnabled = config_.enabled;
    const uint32_t prevGraceMs = static_cast<uint32_t>(config_.foregroundAcquireGraceMs);
    const std::string previousProfile = activeProfileSection_;
    config_ = cfg;
    activeProfileSection_ = profileSection;
    lastWarnMsg_.clear();
    sizeWarn_ = {0, 0};

    if (_stricmp(previousProfile.c_str(), activeProfileSection_.c_str()) != 0) {
        LogInfo("[PseudoOverlay] Active DesktopOverlay settings: %s",
                activeProfileSection_.empty() ? "global" : activeProfileSection_.c_str());
    }

    if (wasEnabled && !cfg.enabled) {
        // Disable overlay: hide windows
        DestroyOverlayWindows();
    }

    // If the grace length changed, drop any in-flight grace so the new value is used
    // for the next foreground acquire. Otherwise the helper would happily keep
    // suppressing with the old length.
    if (prevGraceMs != static_cast<uint32_t>(cfg.foregroundAcquireGraceMs)) {
        lastForegroundAcquireTick_ = 0;
        lastForegroundAcquirePid_ = 0;
        hadForegroundTarget_ = false;
        prevGraceActive_ = false;
        foregroundGraceEverStarted_ = false;
        LogInfo("[PseudoOverlay] Foreground grace reset: grace_ms changed %u -> %u", prevGraceMs,
                static_cast<uint32_t>(cfg.foregroundAcquireGraceMs));
    }
}

void PseudoOverlay::SetRecordingStartIntent(RecordingStartIntent intent) {
    requestedStartIntent_.store(intent, std::memory_order_release);
    PostRefresh();
}

void PseudoOverlay::RequestRefresh() {
    PostRefresh();
}

#ifdef CE_UNIT_TESTS
bool PseudoOverlay::WaitForUiIdleForTesting(DWORD timeoutMs) {
    const DWORD threadId = uiThreadId_.load(std::memory_order_acquire);
    if (threadId == 0) {
        return false;
    }
    HANDLE barrier = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!barrier) {
        return false;
    }
    const bool posted = PostThreadMessageW(threadId, kMsgTestBarrier, reinterpret_cast<WPARAM>(barrier), 0) != FALSE;
    const bool signaled = posted && WaitForSingleObject(barrier, timeoutMs) == WAIT_OBJECT_0;
    CloseHandle(barrier);
    return signaled;
}
#endif

void PseudoOverlay::PostRefresh() {
    const DWORD threadId = uiThreadId_.load(std::memory_order_acquire);
    if (threadId != 0) {
        PostThreadMessageW(threadId, kMsgRefresh, 0, 0);
    }
}

void PseudoOverlay::TriggerEncoderOverloadWarning(uint32_t sustainFpsX100) {
    overloadWarnSustainFpsX100_.store(sustainFpsX100, std::memory_order_relaxed);
    overloadWarnUntil_.store(GetTickCount64() + 5000ULL);
    PostRefresh();
}

void PseudoOverlay::ShowScreenshotNotification() {
    screenshotNotifyUntil_.store(GetTickCount64() + 2000ULL);
    PostRefresh();
}
bool PseudoOverlay::EnsureOverlayWindows() {
    if (hOv_ && hWarn_) {
        return true;
    }

    if (!hInstance_) {
        LogError("[PseudoOverlay] Missing HINSTANCE for overlay window creation");
        return false;
    }

    if (!hOv_) {
        hOv_ = CreateWindowExA(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                               kIndicatorClass, "", WS_POPUP, 0, 0, 0, 0, NULL, NULL, hInstance_, 0);
        if (!hOv_) {
            LogError("[PseudoOverlay] Failed to create indicator overlay window");
            return false;
        }
    }

    if (!hWarn_) {
        hWarn_ =
            CreateWindowExA(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            kWarningClass, "", WS_POPUP, 0, 0, 0, 0, NULL, NULL, hInstance_, 0);
        if (!hWarn_) {
            LogError("[PseudoOverlay] Failed to create warning overlay window");
            DestroyWindow(hOv_);
            hOv_ = NULL;
            return false;
        }
    }

    EnsureDwmApi();
    if (g_DwmSetWindowAttribute) {
        BOOL peekExclude = TRUE;
        g_DwmSetWindowAttribute(hOv_, DWMWA_EXCLUDED_FROM_PEEK, &peekExclude, sizeof(peekExclude));
        g_DwmSetWindowAttribute(hWarn_, DWMWA_EXCLUDED_FROM_PEEK, &peekExclude, sizeof(peekExclude));
    }

    return true;
}

void PseudoOverlay::DestroyOverlayWindows() {
    if (hWarn_) {
        DestroyWindow(hWarn_);
        hWarn_ = NULL;
    }
    if (hOv_) {
        DestroyWindow(hOv_);
        hOv_ = NULL;
    }
}
