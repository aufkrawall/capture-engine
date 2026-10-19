#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "common/config/config.h"
#include "common/logging/log_privacy.h"
#include "common/logging/logging.h"
#include "common/ipc/media_finalization.h"
#include "common/ipc/process_ipc.h"
#include "common/platform/build_identity.h"

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

inline void FinalizeRecordingManifest(const std::string& mediaLogPath, bool canceled, bool outputSaved,
                                      const char* healthStatus, const char* healthCause, const char* degradedScope,
                                      uint32_t healthFlags, uint32_t currentDebtMs, uint32_t peakDebtMs,
                                      uint32_t capacityAttributedDebtMs,
                                      const ce::media_result::Result& finalization) {
    if (g_RecordingId.empty() || mediaLogPath.empty()) {
        return;
    }

    const std::filesystem::path logPath(mediaLogPath);
    const std::filesystem::path manifestPath =
        logPath.parent_path() /
        ("recording_" + g_RecordingId + "_" + std::to_string(GetCurrentProcessId()) + ".manifest");
    std::ofstream manifest(manifestPath, std::ios::out | std::ios::app);
    if (!manifest.is_open()) {
        LogWarn("[RecordingManifest] Failed to append final state to %s", manifestPath.string().c_str());
        return;
    }

    manifest << "status="
             << (canceled ? "recording_canceled" : (outputSaved ? "media_finalized" : "recording_failed"))
             << "\n";
    manifest << "recording_health=" << (healthStatus ? healthStatus : "unknown") << "\n";
    manifest << "recording_health_cause=" << (healthCause ? healthCause : "unknown") << "\n";
    // none | video | audio | audio_and_video: which track(s) lost content.
    manifest << "recording_degraded=" << (degradedScope ? degradedScope : "unknown") << "\n";
    manifest << "recording_health_flags=" << healthFlags << "\n";
    manifest << "final_timeline_debt_ms=" << currentDebtMs << "\n";
    manifest << "peak_timeline_debt_ms=" << peakDebtMs << "\n";
    manifest << "capacity_attributed_debt_ms=" << capacityAttributedDebtMs << "\n";
    manifest << "encoder_settings_changed=0\n";
    manifest << "output_saved=" << (outputSaved ? 1 : 0) << "\n";
    // The same outcome the spawner receives through the exit code and the result slot, so a manifest
    // and a controller-side FINALIZED report can be compared. Paths are collapsed like every log line.
    manifest << "finalization_result=" << ce::media_result::StatusName(finalization.status) << "\n";
    manifest << "finalization_flags=" << finalization.flags << "\n";
    char exitCode[16]{};
    snprintf(exitCode, sizeof(exitCode), "0x%08X",
             ce::media_result::EncodeExitCode(finalization.status, finalization.flags));
    manifest << "media_exit_code=" << exitCode << "\n";
    manifest << "output_files=" << finalization.paths.size() << "\n";
    for (size_t i = 0; i < finalization.paths.size(); ++i) {
        manifest << "output_path" << (i == 0 ? std::string() : "_" + std::to_string(i + 1)) << "="
                 << ce::privacy::CollapsePathForLog(finalization.paths[i]) << "\n";
    }
    manifest << "finalization_complete=1\n";
}
