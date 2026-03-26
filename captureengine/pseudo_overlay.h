#pragma once

#include <windows.h>
#include <atomic>
#include <string>

#include "../common/config.h"

// Pseudo-overlay indicator for WGC capture.
// Shows a colored circle in a screen corner:
//   Red = recording active, Blue = connected/idle, Gray = disconnected
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

    // Update configuration (applied on next overlay refresh).
    void UpdateConfig(const PseudoOverlayConfig& cfg);

    // Notify recording state change.
    void SetRecordingState(bool recording);

    // Notify encoder overload (warning shown for 5 seconds).
    void TriggerEncoderOverloadWarning();

    // Show brief screenshot notification (2 seconds).
    void ShowScreenshotNotification();

    // Force an immediate overlay update (called from timer or state changes).
    void UpdateOverlay();

    bool IsInitialized() const {
        return initialized_;
    }

private:
    struct AnchorInfo {
        RECT monitorRect = {0, 0, 0, 0};
        HWND window = NULL;
        UINT dpi = 96;
    };

    // Overlay state tracking (to avoid redundant UpdateLayeredWindow calls)
    struct OvState {
        int x = -1, y = -1, s = -1;
        bool vis = false;
        bool ghost = false;
    };

    // Foreground process detection
    bool IsForegroundTarget();

    // Check if inject overlay is active in a hooked game (via shared memory)
    bool EnsureSharedMemoryMapping();
    bool IsInjectOverlayActive();
    AnchorInfo ResolveAnchorInfo();
    void UpdateScaleForDpi(UINT dpi);

    // GDI rendering helpers
    void InitGDI();
    void CleanupGDI();

    // Overlay state
    bool initialized_ = false;
    PseudoOverlayConfig config_;
    float scale_ = 1.0f;

    // Scale helper
    int S(int v) const;

    // Atomic recording state
    std::atomic<bool> isRecording_{false};
    std::atomic<ULONGLONG> overloadWarnUntil_{0};
    std::atomic<ULONGLONG> screenshotNotifyUntil_{0};

    // Warning blink state
    bool warnActive_ = false;
    bool warnVisible_ = false;
    ULONGLONG warnCycleStart_ = 0;

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
    uint32_t mappedInjectPid_ = 0;
    uint32_t lastEncoderOverloadFlags_ = 0;

    // Timer ID
    static constexpr UINT_PTR kTimerId = 1001;
    static constexpr UINT kTimerInterval = 100;  // ms

    // Window class names
    static constexpr const char* kIndicatorClass = "CE_PseudoOv";
    static constexpr const char* kWarningClass = "CE_PseudoWarn";

    // Window procedures
    static LRESULT CALLBACK IndicatorWndProc(HWND h, UINT m, WPARAM w, LPARAM l);
    static LRESULT CALLBACK WarningWndProc(HWND h, UINT m, WPARAM w, LPARAM l);

    // Instance for static wndproc routing
    static PseudoOverlay* instance_;

    // Shared memory handles for inject overlay detection
    HANDLE hDiscoveryMap_ = NULL;
    HANDLE hSharedMemMap_ = NULL;
    struct DiscoveryInfo* pDiscovery_ = nullptr;
    struct SharedMemoryLayout* pSharedMem_ = nullptr;
};
