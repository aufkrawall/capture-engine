// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "runtime/platform/diagnostics_session.h"

#include "common/crash/crash_handler.h"
#include "common/crash/wer_dump_adoption.h"
#include "common/ipc/process_ipc.h"
#include "common/logging/log_privacy.h"
#include "common/logging/logging.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace ce::runtime {

std::string SessionDirectoryName(const SYSTEMTIME& localTime) {
    char name[32];
    snprintf(name, sizeof(name), "%04d%02d%02d_%02d%02d%02d", localTime.wYear, localTime.wMonth, localTime.wDay,
             localTime.wHour, localTime.wMinute, localTime.wSecond);
    return name;
}

void CleanupOldSessionDirs(const std::string& logsRoot, size_t keep) {
    namespace fs = std::filesystem;
    std::error_code ec;

    // Session subdirectories are named YYYYMMDD_HHMMSS, so name order is age order.
    std::vector<fs::path> sessions;
    for (const auto& entry : fs::directory_iterator(logsRoot, ec)) {
        if (!entry.is_directory(ec))
            continue;
        const std::string name = entry.path().filename().string();
        if (name.size() == 15 && name[8] == '_')
            sessions.push_back(entry.path());
    }
    std::sort(sessions.begin(), sessions.end(),
              [](const fs::path& a, const fs::path& b) { return a.filename() < b.filename(); });
    if (sessions.size() > keep) {
        for (size_t i = 0; i < sessions.size() - keep; ++i)
            fs::remove_all(sessions[i], ec);
    }

    // Flat files of the builds before per-session directories.
    for (const char* pattern : {"\\*.log", "\\*.csv"}) {
        WIN32_FIND_DATAA found;
        const HANDLE search = FindFirstFileA((logsRoot + pattern).c_str(), &found);
        if (search == INVALID_HANDLE_VALUE)
            continue;
        do {
            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                DeleteFileA((logsRoot + "\\" + found.cFileName).c_str());
        } while (FindNextFileA(search, &found));
        FindClose(search);
    }
}

void WriteSessionManifest(const std::string& sessionDir, const AppConfig& config, bool loggerEnabled,
                          bool sensorEnabled) {
    std::ofstream manifest(sessionDir + "\\session_manifest.txt", std::ios::out | std::ios::trunc);
    if (!manifest.is_open())
        return;
    manifest << "build_version=" << GetCaptureVersion() << "\n";
    manifest << "build_timestamp=" << GetBuildTimestamp() << "\n";
    manifest << "session_dir=" << ce::privacy::RedactUserAccountComponents(sessionDir) << "\n";
    manifest << "process_mode=Controller\n";
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
    manifest << "logger_enabled=" << (loggerEnabled ? 1 : 0) << "\n";
    manifest << "sensor_enabled=" << (sensorEnabled ? 1 : 0) << "\n";
    manifest << "game_whitelist_entries=" << config.gameWhitelist.size() << "\n";
    manifest << "overlay_whitelist_entries=" << config.overlayWhitelist.size() << "\n";
    manifest << "logs=" << GetLogFileName(ProcessMode::Controller) << "\n";
    manifest << "media_logs=media_*.log\n";
    manifest << "recording_manifests=recording_*.manifest\n";
    manifest << "notes=Use this file as the compact session entrypoint before reading detailed logs.\n";
}

std::string CrashDumpDirectory(const std::string& sessionDir, const std::string& configured) {
    if (configured.empty())
        return sessionDir;
    const std::filesystem::path normalized = std::filesystem::path(configured).lexically_normal();
    const bool traversal = std::any_of(normalized.begin(), normalized.end(),
                                       [](const std::filesystem::path& part) { return part == ".."; });
    if (normalized.empty() || normalized == "." || normalized.has_root_name() || normalized.has_root_directory() ||
        traversal)
        return sessionDir;
    return (std::filesystem::path(sessionDir) / normalized).string();
}

std::unique_ptr<DiagnosticsSession> DiagnosticsSession::Open(const DiagnosticsOptions& options,
                                                             const AppConfig& config) {
    std::unique_ptr<DiagnosticsSession> session(new DiagnosticsSession());
    SYSTEMTIME now;
    GetLocalTime(&now);
    session->name_ = SessionDirectoryName(now);
    // Every helper spawned from here on joins this session (--session-dir).
    g_SessionDirName = session->name_;
    const std::string sessionDir = options.logsRoot + "\\" + session->name_;
    session->logPath_ =
        sessionDir + "\\" + GetProcessLogFileName(ProcessMode::Controller, std::string(), GetCurrentProcessId());
    if (!IsAnyLoggingEnabled(config.logLevel)) {
        OutputDebugStringA("[CaptureEngine] log_level=none: no log directory, crash handler or debug machinery\n");
        return session;
    }

    // Session symbol archives are hard links into one store below the logs root (crash_symbol_store.h).
    SetCrashSymbolStoreRoot(options.logsRoot);
    CleanupOldSessionDirs(options.logsRoot);
    CreateDirectoryA(options.logsRoot.c_str(), nullptr);
    CreateDirectoryA(sessionDir.c_str(), nullptr);
    SetCrashDumpDirectory(sessionDir);
    if (options.crashHandler) {
        InstallCrashHandler();
        // Earlier builds wrote WER LocalDumps values under HKCU, which WER never reads (it reads HKLM only); they
        // only accumulated one stale subkey per game, each with this user's session paths.
        ce::wer_dump_adoption::PurgeInertCaptureEngineLocalDumpsRegistration(options.logsRoot);
    }
    WriteSessionManifest(sessionDir, config, options.loggerEnabled, options.sensorEnabled);
    Log_Init(session->logPath_, config.logLevel);
    session->logOpened_ = true;
    LogInfo("CaptureEngine Starting... Version: %s (Built: %s)", GetCaptureVersion(), GetBuildTimestamp());
    LogInfo("Process Mode: Controller (runtime, crash handler %s)", options.crashHandler ? "installed" : "not requested");
    SetCrashDumpDirectory(CrashDumpDirectory(sessionDir, config.crashDumpDir));
    // After this session linked its symbols and old sessions were pruned, a store file nothing links is unused.
    PruneCrashSymbolStore();
    return session;
}

DiagnosticsSession::~DiagnosticsSession() {
    if (logOpened_)
        Log_Shutdown();
}

}  // namespace ce::runtime
