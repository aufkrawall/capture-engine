#pragma once

// clang-format off
#include <windows.h>

#include <shellapi.h>

#include <timeapi.h>

// clang-format on
#include <algorithm>

#include <atomic>

#include <filesystem>

#include <fstream>

#include <functional>

#include <mutex>

#include <string>

#include <string_view>

#include <vector>

#include "common/ipc/av_sync_latency_channel.h"

#include "common/config/config.h"

#include "common/crash/crash_handler.h"

#include "common/logging/logging.h"

#include "common/logging/log_privacy.h"

#include "common/platform/monitor_selection.h"

#include "common/ipc/process_ipc.h"

#include "common/ipc/shared_defs.h"

#include "common/platform/strict_integer_parse.h"

#include "common/graphics/vulkan_layer_registration.h"

#include "common/crash/wer_dump_adoption.h"

#include "captureengine/diagnostics/dump_helper.h"

#include "captureengine/display_timing/display_timing_policy.h"

#include "hotkey_input_hook.h"

#include "captureengine/injection/injection.h"

#include "main_vulkan_residency.h"

#include "captureengine/sensors/pawnio_setup.h"

#include "captureengine/media/process_loopback_worker_host.h"

#include "captureengine/pseudo_overlay/pseudo_overlay.h"

#include "captureengine/media/recording_manifest.h"

#include "captureengine/media/screenshot.h"

#include "captureengine/sensors/sensor_bridge_host.h"

#include "tray.h"
#include "controller_recording.h"
#include "host_children.h"

#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif

// Forward declarations for process entry points
extern int InjectProcessMain(const AppConfig& config);

extern int MediaProcessMain(const AppConfig& config);

extern int LoggerProcessMain(const AppConfig& config);

extern int SensorProcessMain(const AppConfig& config);

// Hotkey IDs live in common/overlay/hotkey_matcher.h so the RegisterHotKey
// registration and the low-level keyboard path cannot drift apart.

void LaunchGameSuspended(const std::string& path);

bool ConnectToChildProcesses(DWORD);

void SendCommandToAll(ProcessCommand cmd);

void PublishRecordingFailureOverlayNotification(const char* reason, bool streaming = false);

void CheckRecordingFailureState();

void ToggleRecording();

void ToggleAudioOnlyRecording();

void ToggleOverlay();

void ToggleBenchmark();

void DispatchHotkey(int hotkeyId);

void ShutdownChildProcesses();

void CheckChildProcessHealth();

bool CompleteControllerStartup();

BOOL WINAPI ControllerConsoleHandler(DWORD ctrlType);

int ControllerMain(HINSTANCE hInstance);

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow);

// Controller state
inline std::atomic<bool> main_g_Running{true};

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
inline AppConfig main_g_Config;

inline std::string main_g_ConfigPath;

// Auto-record feature for autonomous testing
inline bool main_g_AutoRecordEnabled = false;

inline DWORD main_g_AutoRecordDelayMs = 3000;      // Delay before starting

inline DWORD main_g_AutoRecordDurationMs = 10000;  // Recording duration

inline DWORD main_g_AutoRecordStartTime = 0;

// Deferred game launch (for --launch mode)
inline std::string main_g_DeferredLaunchPath;

// Child process handles

inline TrayIcon* main_g_Tray = nullptr;

inline std::unique_ptr<PseudoOverlay> main_g_PseudoOverlay;

// Hotkeys whose combination this process actually holds. Both delivery paths
// consult it, so neither serves a combination another application owns.
inline HotkeyOwnership main_g_HotkeyOwnership;

inline constexpr UINT main_kMsgCompleteControllerStartup = WM_APP + 1;

// A hotkey the low-level keyboard hook recognized and consumed. It is a message
// of its own rather than a synthesized WM_HOTKEY so the logs stay honest about
// which delivery path served a press.
inline constexpr UINT main_kMsgHotkeyFromInputHook = WM_APP + 2;

// Deliberately not in an unnamed namespace: the type's linkage decides the
// variable's. An internal-linkage type gives every translation unit its own
// main_g_ControllerStartupTiming, so main_entry.cpp's measurements never reach
// the [StartupPerf] line in main_recording.cpp and it reports zeros plus an
// absolute QPC reading as TotalToReady.
struct ControllerStartupTimingState {
    int64_t controllerStartUs = 0;
    int64_t vulkanRegUs = 0;
    int64_t trayCreateUs = 0;
    bool complete = false;
};

inline ControllerStartupTimingState main_g_ControllerStartupTiming;

inline double QpcDeltaToMs(int64_t deltaUs) {
    return static_cast<double>(deltaUs) / 1000.0;
}

inline void PumpStartupMessages() {
    MSG msg = {};
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

inline void PrimeStartupCursor() {
    MSG msg = {};
    PeekMessage(&msg, NULL, 0, 0, PM_NOREMOVE);
    HCURSOR arrow = LoadCursor(nullptr, IDC_ARROW);
    if (arrow) {
        SetCursor(arrow);
    }
}

inline bool HasExactCommandLineArgument(const wchar_t* expected) {
    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (!arguments)
        return false;
    bool found = false;
    for (int index = 1; index < argumentCount; ++index) {
        if (_wcsicmp(arguments[index], expected) == 0) {
            found = true;
            break;
        }
    }
    LocalFree(reinterpret_cast<HLOCAL>(arguments));
    return found;
}

inline bool TryParseAutoRecordValue(std::string_view value, DWORD& result) {
    uint32_t parsed = 0;
    if (!ce::TryParseUInt32(value, parsed))
        return false;
    result = parsed;
    return true;
}

inline bool IsProcessRunning(HANDLE hProcess) {
    if (!hProcess) {
        return false;
    }
    DWORD exitCode = 0;
    return GetExitCodeProcess(hProcess, &exitCode) && exitCode == STILL_ACTIVE;
}

namespace {
// Measures how long the controller's main thread spends inside a blocking section. The
// pseudo-overlay owns a dedicated message thread, but the tray and global hotkeys still
// depend on this controller thread and remain useful diagnostics when it is starved.
struct MainThreadBlockTimer {
    const char* label_;
    ULONGLONG startMs_;
    explicit MainThreadBlockTimer(const char* label) : label_(label), startMs_(GetTickCount64()) {}
    ~MainThreadBlockTimer() {
        const ULONGLONG elapsedMs = GetTickCount64() - startMs_;
        if (elapsedMs >= 250) {
            LogWarn(
                "[Controller] Main-thread blocked %llums in %s — tray + global hotkeys were unresponsive this long",
                static_cast<unsigned long long>(elapsedMs), label_);
        }
    }
};
}

inline bool ShouldStartMediaProcessAtStartup() {
    return main_g_AutoRecordEnabled;
}

inline bool ShouldStartLoggerProcess(const AppConfig& config) {
    return IsAnyLoggingEnabled(config.logLevel);
}

inline bool ShouldStartSensorProcess(const AppConfig& config) {
    return config.overlay.showCPU || config.overlay.showGPU || config.overlay.showRAM || config.overlay.showVRAM ||
           ShouldStartOverlayDisplayTiming(config.overlay.showOverlay, config.overlay.showSystemLatency) ||
           ControllerRecordingSnapshot().requested;
}

inline bool HardwareSensorServiceConfigEquals(const AppConfig& lhs, const AppConfig& rhs) {
    const HardwareSensorsConfig& left = lhs.hardwareSensors;
    const HardwareSensorsConfig& right = rhs.hardwareSensors;
    return lhs.overlay.showCPU == rhs.overlay.showCPU && lhs.overlay.showGPU == rhs.overlay.showGPU &&
           left.enabled == right.enabled && left.pollIntervalMs == right.pollIntervalMs &&
           left.cpuTemperature == right.cpuTemperature && left.gpuTemperature == right.gpuTemperature &&
           left.cpuPackagePower == right.cpuPackagePower && left.gpuPackagePower == right.gpuPackagePower &&
           left.gpuFan == right.gpuFan && left.cpuCoreClock == right.cpuCoreClock &&
           left.gpuCoreClock == right.gpuCoreClock && left.gpuMemoryClock == right.gpuMemoryClock &&
           left.gpuVoltage == right.gpuVoltage;
}

void SyncPseudoOverlayConfiguration(const char* reason);

inline void WriteSessionManifest(const std::string& logsDir, const AppConfig& config, ProcessMode mode) {
    std::ofstream manifest(logsDir + "\\session_manifest.txt", std::ios::out | std::ios::trunc);
    if (!manifest.is_open()) {
        return;
    }

    manifest << "build_version=" << GetCaptureVersion() << "\n";
    manifest << "build_timestamp=" << GetBuildTimestamp() << "\n";
    manifest << "session_dir=" << ce::privacy::RedactUserAccountComponents(logsDir) << "\n";
    manifest << "process_mode="
             << (mode == ProcessMode::Controller ? "Controller"
                 : mode == ProcessMode::Inject   ? "Inject"
                 : mode == ProcessMode::Media    ? "Media"
                 : mode == ProcessMode::Logger   ? "Logger"
                 : mode == ProcessMode::Sensors  ? "Sensors"
                                                 : "Unknown")
             << "\n";
    manifest << "log_level=" << LogLevelToConfigString(config.logLevel) << "\n";
    manifest << "capture_method=" << config.captureMethod << "\n";
    manifest << "capture_monitor=" << config.captureMonitor << "\n";
    manifest << "overlay_enabled=" << (config.overlay.showOverlay ? 1 : 0) << "\n";
    manifest << "overlay_observer_only=" << (config.overlay.observerOnly ? 1 : 0) << "\n";
    manifest << "overlay_observer_policy_only=" << (config.overlay.observerPolicyOnly ? 1 : 0) << "\n";
    manifest << "overlay_observer_startup_present_only=" << (config.overlay.observerStartupPresentOnly ? 1 : 0) << "\n";
    manifest << "steam_overlay_loaded=0\n";
    manifest << "streamline_loaded=0\n";
    manifest << "ffx_loaded=0\n";
    manifest << "fg_shadow_state_enabled=1\n";
    manifest << "fg_state_schema_version=1\n";
    manifest << "logger_enabled=" << (ShouldStartLoggerProcess(config) ? 1 : 0) << "\n";
    manifest << "sensor_enabled=" << (ShouldStartSensorProcess(config) ? 1 : 0) << "\n";
    manifest << "game_whitelist_entries=" << config.gameWhitelist.size() << "\n";
    manifest << "overlay_whitelist_entries=" << config.overlayWhitelist.size() << "\n";
    manifest << "logs=" << GetLogFileName(mode) << "\n";
    manifest << "media_logs=media_*.log\n";
    manifest << "recording_manifests=recording_*.manifest\n";
    manifest << "notes=Use this file as the compact session entrypoint before reading detailed logs.\n";
}

inline void WriteRecordingManifest(const std::string& logsDir, const AppConfig& config, const std::string& mediaLog) {
    if (g_RecordingId.empty())
        return;

    const DWORD processId = GetCurrentProcessId();
    const std::string path = logsDir + "\\recording_" + g_RecordingId + "_" + std::to_string(processId) +
                             ".manifest";
    std::ofstream manifest(path, std::ios::out | std::ios::trunc);
    if (!manifest.is_open())
        return;

    manifest << "build_version=" << GetCaptureVersion() << "\n";
    manifest << "build_timestamp=" << GetBuildTimestamp() << "\n";
    manifest << "recording_id=" << g_RecordingId << "\n";
    manifest << "media_pid=" << processId << "\n";
    manifest << "media_log=" << mediaLog << "\n";
    manifest << "base_capture_method=" << config.captureMethod << "\n";
    manifest << "base_capture_monitor=" << config.captureMonitor << "\n";
    manifest << "status=media_process_started\n";
    manifest << "notes=Recording-specific evidence; correlate by recording_id and media_pid.\n";
}

inline DWORD GetControllerLoopWaitMs(DWORD lastConfigCheck, DWORD configCheckIntervalMs = 1000) {
    DWORD waitMs = 2000;
    DWORD now = GetTickCount();

    DWORD configElapsed = now - lastConfigCheck;
    if (configElapsed >= configCheckIntervalMs) {
        return 0;
    }
    DWORD configWaitMs = configCheckIntervalMs - configElapsed;
    if (configWaitMs < waitMs) {
        waitMs = configWaitMs;
    }

    if (main_g_AutoRecordEnabled && main_g_AutoRecordStartTime > 0) {
        DWORD elapsed = now - main_g_AutoRecordStartTime;
        DWORD nextAutoActionMs = !ControllerRecordingSnapshot().requested ? main_g_AutoRecordDelayMs : (main_g_AutoRecordDelayMs + main_g_AutoRecordDurationMs);
        if (elapsed >= nextAutoActionMs) {
            return 0;
        }
        DWORD autoWaitMs = nextAutoActionMs - elapsed;
        if (autoWaitMs < waitMs) {
            waitMs = autoWaitMs;
        }
    }

    return waitMs;
}

inline bool HotkeyConfigEquals(const AppConfig::HotkeyConfig& a, const AppConfig::HotkeyConfig& b) {
    return a.vkey == b.vkey && a.ctrl == b.ctrl && a.shift == b.shift && a.alt == b.alt && a.win == b.win;
}

inline bool EnsureMediaProcessReady(DWORD timeoutMs) {
    return ce::runtime::EnsureHostChild(ce::runtime::HostChild::Media, timeoutMs);
}

inline bool EnsureSensorProcessReady() {
    return ce::runtime::EnsureHostChild(ce::runtime::HostChild::Sensors);
}

inline void SyncLoggerAndSensorProcesses(const AppConfig& config, const AppConfig* previousConfig = nullptr) {
    ce::runtime::ReconfigureHostServices({ShouldStartLoggerProcess(config), ShouldStartSensorProcess(config),
        previousConfig && !HardwareSensorServiceConfigEquals(*previousConfig, config)});
}

// Remove old session directories from logs/, keeping the most recent maxKeep.
// Also cleans up any stale flat .log/.csv files from pre-session-dir versions.
inline void CleanupOldSessionDirs(const std::string& logsDir, size_t maxKeep = 20) {
    namespace fs = std::filesystem;
    std::error_code ec;

    // Collect session subdirectories (names are YYYYMMDD_HHMMSS, so lexicographic sort = chronological)
    std::vector<fs::directory_entry> sessions;
    for (auto& entry : fs::directory_iterator(logsDir, ec)) {
        if (!entry.is_directory(ec))
            continue;
        auto name = entry.path().filename().string();
        // Validate timestamp format: 15 chars, YYYYMMDD_HHMMSS
        if (name.size() == 15 && name[8] == '_')
            sessions.push_back(entry);
    }

    // Sort oldest-first by name
    std::sort(sessions.begin(), sessions.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) {
        return a.path().filename() < b.path().filename();
    });

    // Remove oldest sessions beyond maxKeep
    if (sessions.size() > maxKeep) {
        size_t toRemove = sessions.size() - maxKeep;
        for (size_t i = 0; i < toRemove; i++) {
            fs::remove_all(sessions[i].path(), ec);
        }
    }

    // Clean up stale flat files from pre-session-dir versions
    const char* patterns[] = {"\\*.log", "\\*.csv"};
    for (int p = 0; p < 2; p++) {
        std::string pattern = logsDir + patterns[p];
        WIN32_FIND_DATAA ffd;
        HANDLE hFind = FindFirstFileA(pattern.c_str(), &ffd);
        if (hFind == INVALID_HANDLE_VALUE)
            continue;
        do {
            if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            std::string filePath = logsDir + "\\" + ffd.cFileName;
            DeleteFileA(filePath.c_str());
        } while (FindNextFileA(hFind, &ffd));
        FindClose(hFind);
    }
}

namespace {
struct DeferredLaunchCommand {
    std::string rawCommandLine;
    std::string executablePath;
    std::string workingDirectory;
    std::string fileName;
};
}
