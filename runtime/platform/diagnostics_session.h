// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

// The runtime's diagnostics session (llm-wiki/library/04 runtime/platform), formerly the controller part of
// captureengine.exe's WinMain prelude: the session directory below the logs root, old-session cleanup, the session
// manifest, the runtime log, crash dump directories and, with CE_FEATURE_CRASH_HANDLER, the process crash handler.
// Helpers join the session through --session-dir. The role host keeps its own prelude for the helper roles.

#include "common/config/config.h"

#include <windows.h>

#include <cstddef>
#include <memory>
#include <string>

namespace ce::runtime {

// YYYYMMDD_HHMMSS: the session directory's name (lexicographic order is chronological).
std::string SessionDirectoryName(const SYSTEMTIME& localTime);

// Removes session directories beyond the newest `keep`, and flat .log/.csv files of pre-session builds.
void CleanupOldSessionDirs(const std::string& logsRoot, size_t keep = 20);

// session_manifest.txt, the compact entry point to a session's logs (tools/log_digest.py, the hook's FG state).
void WriteSessionManifest(const std::string& sessionDir, const AppConfig& config, bool loggerEnabled,
                          bool sensorEnabled);

// Where crash dumps go: the session directory, or a relative subfolder of it named by crash_dump_dir. Absolute
// paths and parent traversal are ignored, so dumps always stay below logs/.
std::string CrashDumpDirectory(const std::string& sessionDir, const std::string& configured);

struct DiagnosticsOptions {
    std::string logsRoot;       // <package>\logs (active code page, like every log path)
    bool crashHandler = false;  // CE_FEATURE_CRASH_HANDLER: install CE's crash handling in this process
    bool loggerEnabled = false;  // the manifest's service lines (runtime/core service policy)
    bool sensorEnabled = false;
};

class DiagnosticsSession {
public:
    // Opens a new session for `config`. With log_level=none nothing is created or installed (only the session
    // name exists, for the helpers' command lines).
    static std::unique_ptr<DiagnosticsSession> Open(const DiagnosticsOptions& options, const AppConfig& config);
    DiagnosticsSession(const DiagnosticsSession&) = delete;
    DiagnosticsSession& operator=(const DiagnosticsSession&) = delete;
    ~DiagnosticsSession();  // closes the runtime log it opened

    const std::string& SessionName() const { return name_; }
    const std::string& LogPath() const { return logPath_; }

private:
    DiagnosticsSession() = default;
    std::string name_;
    std::string logPath_;
    bool logOpened_ = false;
};

}  // namespace ce::runtime
