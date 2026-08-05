#pragma once

#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../common/config.h"
#include "../common/capture_policy/constants.h"
#include "../common/pseudo_overlay_focus_grace.h"
#include "../common/pseudo_overlay_visibility.h"
#include "../common/recording_indicator_policy.h"
#include "../common/shared_defs.h"

// Controller-side pseudo-overlay indicator for WGC capture.
// This uses layered top-level desktop windows and retains both the ghost
// keepalive path and animated warning text behavior for compatibility.
// Shows a colored circle in a screen corner:
//   Red = recording active, Blue = idle
// Blinking warning when a whitelisted game is focused but not recording.
// Runs entirely within the controller process (no extra DLL/EXE).
class PseudoOverlay {
public:
    PseudoOverlay();
    ~PseudoOverlay();

    // Create overlay windows. Returns false on failure.
    bool Init(HINSTANCE hInstance);

    // Destroy overlay windows and free resources.
    void Shutdown();

    // Update global configuration plus settings resolved for process-backed
    // application profiles (applied on the next overlay refresh).
    void UpdateConfig(const PseudoOverlayConfig& cfg,
                      const std::vector<PseudoOverlayApplicationConfig>& profiles = {});

    // Publish controller intent immediately, before child-process readiness waits.
    void SetRecordingStartIntent(RecordingStartIntent intent);

    // Ask the UI thread to re-read shared live/pending state.
    void RequestRefresh();

    // Notify encoder overload (warning shown for 5 seconds).
    void TriggerRecordingHealthWarning(uint32_t warningKind, uint32_t sustainFpsX100 = 0);

    // Show a brief screenshot result notification (2 seconds).
    void ShowScreenshotNotification(bool succeeded);

    // Show recording finalization state until media publishes the actual result.
    void ShowRecordingFinalizingNotification();

    // Temporarily hide overlay windows so they do not appear in a screenshot.
    // Must be paired with EndScreenshotCapture() after the capture completes.
    void BeginScreenshotCapture();
    void EndScreenshotCapture();

    bool IsInitialized() const {
        return initialized_.load(std::memory_order_acquire);
    }

#ifdef CE_UNIT_TESTS
    bool WaitForUiIdleForTesting(DWORD timeoutMs = 2000);
    ce::recording_indicator::State GetRecordingIndicatorStateForTesting() const {
        return publishedRecordingIndicatorState_.load(std::memory_order_acquire);
    }
    // Emulates the media-published capture-dark request so the notification and
    // acknowledgement path can be exercised without an inject shared-memory instance.
    void ForceStatusDarkForCaptureForTesting(bool dark) {
        forcedStatusDarkForCapture_.store(dark, std::memory_order_release);
    }
#endif

private:
    struct AnchorInfo {
        RECT monitorRect = {0, 0, 0, 0};
        HWND window = NULL;
        HMONITOR monitor = NULL;
        UINT dpi = 96;
        bool fullscreenLike = false;
    };

    // Overlay state tracking (to avoid redundant UpdateLayeredWindow calls)
    struct OvState {
        int x = -1, y = -1, s = -1;
        bool vis = false;
        bool ghost = false;
    };

    // Foreground process/profile detection
    void RefreshActiveProfileConfig();
    bool IsForegroundTarget();
    // Returns the raw foreground PID (0 if none) without consulting the whitelisted
    // process list. Used to feed the focus-grace policy helper.
    uint32_t GetForegroundTargetPid();

    // Check if inject overlay is active in a hooked game (via shared memory)
    bool EnsureSharedMemoryMapping();
    bool IsInjectOverlayActive();
    bool IsInjectOverlayPending();
    bool EnsureOverlayWindows();
    void DestroyOverlayWindows();
    AnchorInfo ResolveAnchorInfo();
    void UpdateScaleForDpi(UINT dpi);
    void OnTimerTick();
    void UpdateOverlay();
    void ThreadMain();
    bool InitializeOnUiThread();
    void ShutdownOnUiThread();
    void ApplyPendingConfig();
    void ApplyEffectiveConfig(const PseudoOverlayConfig& cfg, const std::string& profileSection);
    bool RefreshRecordingState();
    void PostRefresh();
    // Media-signalled status synchronization. The watcher thread only waits and forwards;
    // every window/GDI touch stays on the UI thread that owns those resources.
    void StatusSyncWatcherMain();
    bool StartStatusSyncWatcher();
    void StopStatusSyncWatcher();
    void HandleStatusSyncOnUiThread();
    // Evaluate focus-acquire grace for the current tick, update tracking state, and
    // log transitions. Called once per timer tick.
    void UpdateForegroundGraceState(bool currentHadTarget, uint32_t currentPid);
    // Compute the focus-grace suppression decision using the latest tracked state plus
    // a fresh foreground snapshot. Returns the helper's decision; side-effect: may
    // update the tracking state via UpdateForegroundGraceState().
    ce::pseudo_overlay::FocusGraceDecision EvaluateForegroundGrace(bool currentHadTarget, uint32_t currentPid,
                                                                   ULONGLONG now);

    // GDI rendering helpers
    void InitGDI();
    void CleanupGDI();

    // Overlay state
    std::atomic<bool> initialized_{false};
    HINSTANCE hInstance_ = NULL;
    PseudoOverlayConfig config_;
    std::mutex pendingConfigMutex_;
    PseudoOverlayConfig pendingConfig_;
    std::vector<PseudoOverlayApplicationConfig> pendingProfileConfigs_;
    std::atomic<uint64_t> pendingConfigGeneration_{0};
    uint64_t appliedConfigGeneration_ = 0;
    PseudoOverlayConfig baseConfig_;
    std::vector<PseudoOverlayApplicationConfig> profileConfigs_;
    std::string activeProfileSection_;
    std::string pinnedProfileSection_;
    std::string foregroundProcessName_;
    uint32_t foregroundPid_ = 0;
    std::string sourceProcessName_;
    uint32_t sourceProfilePid_ = 0;
    bool foregroundIsTarget_ = false;
    std::thread uiThread_;
    std::atomic<DWORD> uiThreadId_{0};
    HANDLE uiReadyEvent_ = NULL;
    std::atomic<bool> uiInitSucceeded_{false};
    // Media -> controller status synchronization. The watcher owns only the wait; the
    // acknowledgement is set by the UI thread once the status windows are composited away.
    std::thread statusSyncThread_;
    HANDLE statusSyncEvent_ = NULL;
    HANDLE statusDarkAckEvent_ = NULL;
    HANDLE statusSyncStopEvent_ = NULL;
    float scale_ = 1.0f;

    // Scale helper
    int S(int v) const;

    // Cross-thread desired intent plus UI-thread-resolved live state.
    std::atomic<RecordingStartIntent> requestedStartIntent_{RecordingStartIntent::Idle};
    std::atomic<bool> isRecording_{false};
    ce::recording_indicator::State recordingIndicatorState_ = ce::recording_indicator::State::Idle;
    std::atomic<ce::recording_indicator::State> publishedRecordingIndicatorState_{
        ce::recording_indicator::State::Idle};
    std::atomic<ULONGLONG> overloadWarnUntil_{0};
    std::atomic<uint32_t> overloadWarnSustainFpsX100_{0};
    std::atomic<uint32_t> overloadWarnKind_{ce::capture_policy::kOverlayWarningNone};
    std::atomic<ULONGLONG> screenshotNotifyUntil_{0};
    std::atomic<bool> screenshotNotificationSucceeded_{true};
    std::atomic<bool> screenshotInProgress_{false};
    std::atomic<ULONGLONG> recordingNotifyUntil_{0};
    std::atomic<ce::pseudo_overlay::RecordingNotificationKind> recordingNotification_{
        ce::pseudo_overlay::RecordingNotificationKind::None};
    // Media-owned capture-dark request, resolved from shared memory on the UI thread.
    // While set, the pending startup status must not be composited: a screen-grab
    // recording captures whatever the compositor shows.
    bool statusDarkForCapture_ = false;
#ifdef CE_UNIT_TESTS
    std::atomic<bool> forcedStatusDarkForCapture_{false};
#endif
    // Warning blink state
    bool warnActive_ = false;
    bool warnVisible_ = false;
    ULONGLONG warnCycleStart_ = 0;

    // Pump-health diagnostic: GetTickCount64() of the previous timer tick. A gap larger
    // than kPumpStallWarnMs means the overlay-owning thread stopped pumping messages for
    // that long (see OnTimerTick / Finding B).
    ULONGLONG lastTimerTickMs_ = 0;

    // Foreground-acquire grace state. After a whitelisted PID (re)acquires foreground
    // focus, the visible overlay is suppressed for `config_.foregroundAcquireGraceMs`
    // ms to avoid racing Windows MPO / fullscreen buffer rebinds on Alt+Tab-in. The
    // sticky anchor and warning blink phase are still advanced during grace so the
    // first post-grace frame is in-position and in-phase.
    ULONGLONG lastForegroundAcquireTick_ = 0;
    uint32_t lastForegroundAcquirePid_ = 0;
    bool hadForegroundTarget_ = false;
    ce::recording_indicator::State prevRecordingIndicatorState_ = ce::recording_indicator::State::Idle;
    bool prevGraceActive_ = false;
    bool foregroundGraceEverStarted_ = false;

    // Overlay change tracking
    OvState lastOv_;
    COLORREF lastCol_ = 0;
    bool lastWarnVis_ = false;
    std::string lastWarnMsg_;

    // Windows
    HWND hOv_ = NULL;    // Indicator overlay window
    HWND hWarn_ = NULL;  // Warning overlay window

    // GDI resources for warning overlay (cached)
    HDC hdcWarn_ = NULL;
    HBITMAP bmWarn_ = NULL;
    HBITMAP oldBmWarn_ = NULL;
    HFONT fontWarn_ = NULL;
    SIZE sizeWarn_ = {0, 0};
    UINT currentDpi_ = 96;
    UINT_PTR timerHandle_ = 0;
    uint32_t mappedInjectPid_ = 0;
    uint32_t lastEncoderOverloadFlags_ = 0;
    uint32_t lastCaptureHealthFlags_ = 0;
    bool lastOverlaySuppressed_ = false;
    bool lastFullscreenSuppressed_ = false;
    HWND stickyAnchorWindow_ = NULL;
    HMONITOR stickyAnchorMonitor_ = NULL;
    UINT stickyAnchorDpi_ = 96;

    // Timer ID
    static constexpr UINT_PTR kTimerId = 1001;
    static constexpr UINT kTimerInterval = 500;  // ms
    // Warn if the gap between timer ticks exceeds this (3x the interval): the topmost
    // overlay windows were unresponsive that long, which can wedge a game MPO transition.
    static constexpr ULONGLONG kPumpStallWarnMs = 1500;

    // Window class names
    static constexpr const char* kIndicatorClass = "CE_PseudoOv";
    static constexpr const char* kWarningClass = "CE_PseudoWarn";
    static constexpr UINT kMsgRefresh = WM_APP + 0x41;
    static constexpr UINT kMsgShutdown = WM_APP + 0x42;
    static constexpr UINT kMsgTestBarrier = WM_APP + 0x43;
    static constexpr UINT kMsgStatusSync = WM_APP + 0x44;

    // Window procedures
    static LRESULT CALLBACK IndicatorWndProc(HWND h, UINT m, WPARAM w, LPARAM l);
    static LRESULT CALLBACK WarningWndProc(HWND h, UINT m, WPARAM w, LPARAM l);
    static VOID CALLBACK TimerProc(HWND hwnd, UINT msg, UINT_PTR timerId, DWORD time);

    // Instance for static wndproc routing
    static PseudoOverlay* instance_;

    // Shared memory handles for inject overlay detection
    HANDLE hDiscoveryMap_ = NULL;
    HANDLE hSharedMemMap_ = NULL;
    struct DiscoveryInfo* pDiscovery_ = nullptr;
    struct SharedMemoryLayout* pSharedMem_ = nullptr;
};
