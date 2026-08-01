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
#include "../common/screen_grab_privacy.h"
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

using ce::screen_grab_privacy::IsWindowFullscreenLike;

// ---- Palette (matching OBSIndicator exactly) ----
static constexpr COLORREF kColWarnText = RGB(255, 20, 20);
static constexpr COLORREF kColScreenshotText = RGB(20, 255, 20);
static constexpr COLORREF kColScreenshotFailureText = RGB(255, 64, 64);
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

std::string FormatRecordingHealthMessage(uint32_t warningKind, uint32_t sustainFpsX100, uint32_t targetFps) {
    if (warningKind == ce::capture_policy::kOverlayWarningRecordingDegraded) {
        return "Recording video degraded!";
    }
    if (warningKind == ce::capture_policy::kOverlayWarningRecordingRecovering) {
        return "Recording recovering...";
    }

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

ce::pseudo_overlay::RecordingNotificationKind ToPseudoRecordingNotification(uint32_t notificationType) {
    switch (static_cast<OverlayNotificationType>(notificationType)) {
        case OverlayNotificationType::RecordingFinalizing:
            return ce::pseudo_overlay::RecordingNotificationKind::Finalizing;
        case OverlayNotificationType::RecordingSaved:
            return ce::pseudo_overlay::RecordingNotificationKind::Saved;
        case OverlayNotificationType::RecordingSavedDegraded:
            return ce::pseudo_overlay::RecordingNotificationKind::SavedDegraded;
        case OverlayNotificationType::RecordingCanceled:
            return ce::pseudo_overlay::RecordingNotificationKind::Canceled;
        case OverlayNotificationType::RecordingFailed:
            return ce::pseudo_overlay::RecordingNotificationKind::Failed;
        default:
            return ce::pseudo_overlay::RecordingNotificationKind::None;
    }
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
                             ULONGLONG overloadWarnUntil, ULONGLONG screenshotNotifyUntil,
                             ULONGLONG recordingNotifyUntil,
                             ce::pseudo_overlay::RecordingNotificationKind recordingNotification,
                             bool ghostActive) {
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
    in.recordingNotifyUntilMs = recordingNotifyUntil;
    in.recordingNotification = recordingNotification;
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
