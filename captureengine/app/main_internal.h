#pragma once

// clang-format off
#include <windows.h>

#include <shellapi.h>

// clang-format on
#include <algorithm>

#include <atomic>

#include <filesystem>

#include <fstream>

#include <string>

#include <string_view>

#include <vector>

#include "common/config/config.h"

#include "common/crash/crash_handler.h"

#include "common/logging/logging.h"

#include "common/logging/log_privacy.h"

#include "common/platform/monitor_selection.h"

#include "common/ipc/process_ipc.h"

#include "common/ipc/shared_defs.h"

#include "common/platform/strict_integer_parse.h"

#include "common/crash/wer_dump_adoption.h"

#include "captureengine/diagnostics/dump_helper.h"

#include "captureengine/sensors/pawnio_setup.h"

#include "captureengine/media/process_loopback_worker_host.h"

#include "captureengine/media/recording_manifest.h"

#include "captureengine/sensors/sensor_bridge_host.h"

#include "runtime/core/service_policy.h"
#include "captureengine/app/role_configuration.h"
#include "common/platform/runtime_package_paths.h"

using ce::runtime::RuntimeConfiguration;

// Forward declarations for process entry points
extern int InjectProcessMain(const AppConfig& config);

extern int MediaProcessMain(const AppConfig& config);

extern int LoggerProcessMain(const AppConfig& config);

extern int SensorProcessMain(const AppConfig& config);

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow);

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
    manifest << "logger_enabled=" << (ce::engine::ShouldStartLoggerProcess(config) ? 1 : 0) << "\n";
    manifest << "sensor_enabled=" << (ce::engine::ShouldStartSensorProcess(config, false) ? 1 : 0) << "\n";
    manifest << "game_whitelist_entries=" << config.gameWhitelist.size() << "\n";
    manifest << "overlay_whitelist_entries=" << config.overlayWhitelist.size() << "\n";
    manifest << "logs=" << GetLogFileName(mode) << "\n";
    manifest << "media_logs=media_*.log\n";
    manifest << "recording_manifests=recording_*.manifest\n";
    manifest << "notes=Use this file as the compact session entrypoint before reading detailed logs.\n";
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
