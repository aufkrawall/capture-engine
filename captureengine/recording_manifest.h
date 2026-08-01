#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "../common/logging.h"
#include "../common/process_ipc.h"

inline void FinalizeRecordingManifest(const std::string& mediaLogPath, bool canceled, bool outputSaved,
                                      const char* healthStatus, const char* healthCause, uint32_t healthFlags,
                                      uint32_t currentDebtMs, uint32_t peakDebtMs,
                                      uint32_t capacityAttributedDebtMs) {
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
    manifest << "recording_health_flags=" << healthFlags << "\n";
    manifest << "final_timeline_debt_ms=" << currentDebtMs << "\n";
    manifest << "peak_timeline_debt_ms=" << peakDebtMs << "\n";
    manifest << "capacity_attributed_debt_ms=" << capacityAttributedDebtMs << "\n";
    manifest << "encoder_settings_changed=0\n";
    manifest << "output_saved=" << (outputSaved ? 1 : 0) << "\n";
    manifest << "finalization_complete=1\n";
}
