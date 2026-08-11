#pragma once

#include "pseudo_overlay.h"

#include "../common/capture_pipeline_policy.h"

#include "../common/inject_overlay_policy.h"

#include "../common/logging.h"

#include "../common/process_identity.h"

#include "../common/pseudo_overlay_dpi_policy.h"

#include "../common/pseudo_overlay_profile_policy.h"

#include "../common/pseudo_overlay_visibility.h"

#include "../common/screen_grab_privacy.h"

#include "../common/secure_dll_loading.h"

#include <dwmapi.h>

typedef HRESULT(WINAPI* DwmSetWindowAttributeFn)(HWND, DWORD, LPCVOID, DWORD);

typedef HRESULT(WINAPI* DwmFlushFn)(void);

typedef HRESULT(WINAPI* GetDpiForMonitorFn)(HMONITOR, int, UINT*, UINT*);

#include <algorithm>

#include <cctype>

#include <cstring>

#include <string>

using ce::screen_grab_privacy::IsWindowFullscreenLike;

inline DwmSetWindowAttributeFn pseudo_overlay_g_DwmSetWindowAttribute = nullptr;

inline DwmFlushFn pseudo_overlay_g_DwmFlush = nullptr;

inline bool pseudo_overlay_g_DwmApiInitialized = false;

inline GetDpiForMonitorFn pseudo_overlay_g_GetDpiForMonitor = nullptr;

inline bool pseudo_overlay_g_GetDpiForMonitorInitialized = false;

inline void EnsureDwmApi() {
    if (pseudo_overlay_g_DwmApiInitialized)
        return;
    pseudo_overlay_g_DwmApiInitialized = true;
    HMODULE mod = ce::security::LoadSystemLibrary(L"dwmapi.dll");
    if (mod) {
        pseudo_overlay_g_DwmSetWindowAttribute = (DwmSetWindowAttributeFn)GetProcAddress(mod, "DwmSetWindowAttribute");
        pseudo_overlay_g_DwmFlush = (DwmFlushFn)GetProcAddress(mod, "DwmFlush");
    }
}

inline void EnsureGetDpiForMonitorApi() {
    if (pseudo_overlay_g_GetDpiForMonitorInitialized)
        return;
    pseudo_overlay_g_GetDpiForMonitorInitialized = true;
    HMODULE mod = GetModuleHandleW(L"shcore.dll");
    if (!mod) {
        mod = ce::security::LoadSystemLibrary(L"shcore.dll");
    }
    if (mod) {
        pseudo_overlay_g_GetDpiForMonitor = (GetDpiForMonitorFn)GetProcAddress(mod, "GetDpiForMonitor");
    }
}

// Block until DWM has finished composing a frame that no longer contains whatever this
// process just hid. Hiding a layered window is asynchronous to composition, so the first
// flush can still return for the pass that was already in flight with the window in it;
// the second returns only for a pass that started after the hide was submitted. Bounded
// by two refresh intervals and paid once per recording start.
inline void DwmFlushComposition() {
    EnsureDwmApi();
    if (!pseudo_overlay_g_DwmFlush) {
        return;
    }
    pseudo_overlay_g_DwmFlush();
    pseudo_overlay_g_DwmFlush();
}

// ---- Palette (matching OBSIndicator exactly) ----
inline constexpr COLORREF pseudo_overlay_kColWarnText = RGB(255, 20, 20);

inline constexpr COLORREF pseudo_overlay_kColScreenshotText = RGB(20, 255, 20);

inline constexpr COLORREF pseudo_overlay_kColScreenshotFailureText = RGB(255, 64, 64);

inline constexpr COLORREF pseudo_overlay_kColStarting = RGB(255, 191, 0);

inline std::string NormalizeProcessName(std::string value) {
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

inline std::string QueryProcessName(DWORD pid) {
    const ce::process::ProcessIdentityResult identity = ce::process::QueryProcessIdentity(pid);
    return NormalizeProcessName(identity.imageName);
}

inline bool PseudoOverlayConfigsEqual(const PseudoOverlayConfig& lhs, const PseudoOverlayConfig& rhs) {
    return lhs.enabled == rhs.enabled && lhs.size == rhs.size && lhs.pad == rhs.pad && lhs.pos == rhs.pos &&
           lhs.mode == rhs.mode && lhs.alwaysRender == rhs.alwaysRender &&
           lhs.alwaysRenderOnlyWhenGame == rhs.alwaysRenderOnlyWhenGame &&
           lhs.showEncoderOverloadWarn == rhs.showEncoderOverloadWarn &&
           lhs.foregroundAcquireGraceMs == rhs.foregroundAcquireGraceMs && lhs.processList == rhs.processList;
}

inline bool GetMonitorRectForMonitor(HMONITOR monitor, RECT* rect) {
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

inline UINT GetMonitorEffectiveDpi(HMONITOR monitor) {
    // The overlay is presented in physical pixels on the anchor monitor, so the scale
    // must follow the monitor's effective DPI (see common/pseudo_overlay_dpi_policy.h).
    // GetDpiForWindow() would leak the anchor window's DPI-awareness context into the
    // font size (unaware apps always report 96, system-aware apps the system DPI), which
    // made the text change size on the same monitor depending on the foreground app.
    UINT monitorDpi = 0;
    if (monitor) {
        EnsureGetDpiForMonitorApi();
        UINT dpiX = 0;
        UINT dpiY = 0;
        if (pseudo_overlay_g_GetDpiForMonitor &&
            SUCCEEDED(pseudo_overlay_g_GetDpiForMonitor(monitor, 0 /* MDT_EFFECTIVE_DPI */, &dpiX, &dpiY))) {
            monitorDpi = dpiX;
        }
    }
    const UINT systemDpi = GetDpiForSystem();
    return ce::pseudo_overlay::ResolveOverlayDpi(monitorDpi, systemDpi);
}

inline std::string FormatRecordingHealthMessage(uint32_t warningKind, uint32_t sustainFpsX100, uint32_t targetFps) {
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

inline ce::pseudo_overlay::RecordingNotificationKind ToPseudoRecordingNotification(uint32_t notificationType) {
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

inline HBITMAP CreateArgbDibSection(int width, int height, void** ppBits) {
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, ppBits, NULL, 0);
}

inline void ApplyPremultipliedAlpha(void* pBits, int width, int height) {
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

namespace {
struct WindowSearch {
    DWORD pid = 0;
    HWND hwnd = NULL;
};
}

inline BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam) {
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

inline HWND GetMainWindowForProcess(DWORD pid) {
    if (pid == 0) {
        return NULL;
    }

    WindowSearch search = {pid, NULL};
    EnumWindows(EnumWindowsCallback, reinterpret_cast<LPARAM>(&search));
    return search.hwnd;
}

inline bool ShouldOverlayBeVisible(const PseudoOverlayConfig& config, ce::recording_indicator::State recordingState,
                             bool warnVisible,
                             ULONGLONG overloadWarnUntil, ULONGLONG screenshotNotifyUntil,
                             ULONGLONG recordingNotifyUntil,
                             ce::pseudo_overlay::RecordingNotificationKind recordingNotification,
                             bool ghostActive, bool statusDarkForCapture) {
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
    in.statusDarkForCapture = statusDarkForCapture;
    return ce::pseudo_overlay::ShouldPseudoOverlayBeVisible(in);
}
