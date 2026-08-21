#pragma once

#include "overlay_adapter.h"

#include "../../common/capture_pipeline_policy.h"

#include "custom_font.h"

#include "custom_overlay.h"

#include "fg_detection.h"

#include "graphics_api_identity.h"

#include "hook_common.h"

#include "overlay_layout_policy.h"

#include "perf_logger.h"

#include "../../common/pseudo_overlay_dpi_policy.h"

#include "../../common/secure_dll_loading.h"

#include <cfloat>  // FLT_MAX

// Include backends based on build context
// VK_LAYER_CE_OVERLAY is defined when building the Vulkan layer
#ifndef VK_LAYER_CE_OVERLAY
// Full backends for hook DLL
#include "custom_overlay_dx10.h"
#include "custom_overlay_dx11.h"
#include "custom_overlay_dx12.h"
#include "custom_overlay_dx8.h"
#include "custom_overlay_dx9.h"
#include "custom_overlay_gl.h"
#endif

#include "custom_overlay_vk.h"

#include <algorithm>

#include <cstdio>

#include <cstring>

#include <memory>

#include <vector>

using namespace ce::overlay_layout;

inline bool OverlayConfigEquals(const OverlayConfig& a, const OverlayConfig& b) {
    return a.showOverlay == b.showOverlay && a.captureIncludeOverlay == b.captureIncludeOverlay &&
           a.screenshotIncludeOverlay == b.screenshotIncludeOverlay && a.showFPS == b.showFPS &&
           a.showFrameTime == b.showFrameTime && a.showCPU == b.showCPU && a.showGPU == b.showGPU &&
           a.showRAM == b.showRAM && a.showVRAM == b.showVRAM && a.showRecording == b.showRecording &&
           a.showFG == b.showFG && a.position == b.position && a.padding == b.padding &&
           a.compactMode == b.compactMode && a.horizontalMode == b.horizontalMode && a.fontSize == b.fontSize &&
           a.roundedCorners == b.roundedCorners && a.bgColor == b.bgColor && a.bgAlpha == b.bgAlpha &&
           a.fpsColor == b.fpsColor && a.cpuColor == b.cpuColor && a.gpuColor == b.gpuColor &&
           a.ramColor == b.ramColor && a.vramColor == b.vramColor && a.frametimeColor == b.frametimeColor &&
           a.textColor == b.textColor && a.textOutline == b.textOutline && a.textOutlineColor == b.textOutlineColor &&
           a.textOutlineThickness == b.textOutlineThickness && a.loadColorLow == b.loadColorLow &&
           a.loadColorMed == b.loadColorMed && a.loadColorHigh == b.loadColorHigh &&
           a.textUpdateInterval == b.textUpdateInterval && a.hdrPaperWhite == b.hdrPaperWhite;
}

namespace {
class ScopedThreadDpiAwareness {
public:
    ScopedThreadDpiAwareness() {
        HMODULE user32 = GetModuleHandleA("user32.dll");
        if (!user32) {
            user32 = ce::security::LoadSystemLibrary(L"user32.dll");
        }

        setThreadDpiAwarenessContext_ =
            reinterpret_cast<SetThreadDpiAwarenessContextFn>(GetProcAddress(user32, "SetThreadDpiAwarenessContext"));
        if (setThreadDpiAwarenessContext_) {
            oldContext_ = setThreadDpiAwarenessContext_(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    ~ScopedThreadDpiAwareness() {
        if (setThreadDpiAwarenessContext_ && oldContext_) {
            setThreadDpiAwarenessContext_(oldContext_);
        }
    }

private:
    using SetThreadDpiAwarenessContextFn = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT value);

    SetThreadDpiAwarenessContextFn setThreadDpiAwarenessContext_ = nullptr;
    DPI_AWARENESS_CONTEXT oldContext_ = nullptr;
};
}

inline std::string FormatRecordingHealthLabel(uint32_t warningKind, uint32_t sustainFpsX100, uint32_t targetFps) {
    if (warningKind == ce::capture_policy::kOverlayWarningRecordingDegraded) {
        return "!VIDEO DEGRADED!";
    }
    if (warningKind == ce::capture_policy::kOverlayWarningRecordingRecovering) {
        return "!RECOVERING!";
    }

    const double sustainFps = static_cast<double>(sustainFpsX100) / 100.0;
    if (targetFps == 0 || sustainFpsX100 == 0) {
        return "!ENCODER OVERLOAD!";
    }

    char buffer[64];
    const double ratio = sustainFps / static_cast<double>(targetFps);
    if (ratio >= 0.95) {
        std::snprintf(buffer, sizeof(buffer), "!ENC LIMIT %.1f/%u!", sustainFps, targetFps);
    } else if (ratio >= 0.80) {
        std::snprintf(buffer, sizeof(buffer), "!ENC OVER %.1f/%u!", sustainFps, targetFps);
    } else {
        std::snprintf(buffer, sizeof(buffer), "!ENC SEVERE %.1f/%u!", sustainFps, targetFps);
    }
    return buffer;
}

// Helper to detect Windows DPI scaling
// A known-valid game window remembered from any adapter's SetHwnd, used as the DPI fallback before
// GetForegroundWindow(). During game startup the foreground window can be a 96-DPI launcher/splash, which
// made adapters that init without their own hwnd (the descriptor-free DX12 backend) render at 100% instead
// of the Windows scale.
inline std::atomic<HWND> overlay_adapter_g_SharedOverlayDpiHwnd{nullptr};

inline HWND ResolveOverlayReferenceHwnd(HWND targetHwnd) {
    HWND resolved = targetHwnd;
    if (!IsWindow(resolved))
        resolved = overlay_adapter_g_SharedOverlayDpiHwnd.load(std::memory_order_acquire);
    if (!IsWindow(resolved))
        resolved = GetForegroundWindow();
    if (!IsWindow(resolved))
        resolved = GetDesktopWindow();
    return resolved;
}

inline bool QueryWindowsSdrWhiteNits(HMONITOR monitor, float& nits, ULONG& rawLevel) {
    if (!monitor)
        return false;
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo))
        return false;

    for (int attempt = 0; attempt < 3; ++attempt) {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        LONG result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
        if (result != ERROR_SUCCESS || pathCount == 0)
            return false;
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        if (result == ERROR_INSUFFICIENT_BUFFER)
            continue;
        if (result != ERROR_SUCCESS)
            return false;
        paths.resize(pathCount);
        for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
            // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName{};
            sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            sourceName.header.size = sizeof(sourceName);
            sourceName.header.adapterId = path.sourceInfo.adapterId;
            sourceName.header.id = path.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS ||
                lstrcmpiW(sourceName.viewGdiDeviceName, monitorInfo.szDevice) != 0) {
                continue;
            }
            // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
            DISPLAYCONFIG_SDR_WHITE_LEVEL whiteLevel{};
            whiteLevel.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
            whiteLevel.header.size = sizeof(whiteLevel);
            whiteLevel.header.adapterId = path.targetInfo.adapterId;
            whiteLevel.header.id = path.targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&whiteLevel.header) != ERROR_SUCCESS || whiteLevel.SDRWhiteLevel == 0)
                return false;
            rawLevel = whiteLevel.SDRWhiteLevel;
            nits = std::clamp(static_cast<float>(rawLevel) * (80.0f / 1000.0f), 80.0f, 1000.0f);
            return true;
        }
        return false;
    }
    return false;
}
