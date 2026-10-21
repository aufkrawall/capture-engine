// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "captureengine/media/recording_recovery_role.h"

#include "captureengine/app/mediaengine_loader.h"
#include "common/logging/log_privacy.h"
#include "common/logging/logging.h"
#include "mediaengine/engine/mediaengine_abi.h"

#include <windows.h>

namespace ce::role {
namespace {

bool Canceled(const volatile long* cancel) {
    return cancel && *cancel != 0;
}

std::string Utf8(const std::wstring& wide) {
    if (wide.empty())
        return {};
    const int bytes =
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(bytes > 0 ? bytes : 0), '\0');
    if (bytes > 0)
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), utf8.data(), bytes, nullptr,
                            nullptr);
    return utf8;
}

}  // namespace

std::vector<RecoveredRecording> RecoverJournalEntries(const ce::capture_output::InFlightJournal& journal,
                                                      const RecoverOneFn& recoverOne, const volatile long* cancel,
                                                      RecoveryPassSummary* summary) {
    RecoveryPassSummary local;
    RecoveryPassSummary& counts = summary ? *summary : local;
    counts = {};
    std::vector<RecoveredRecording> recovered;
    const std::vector<std::filesystem::path> entries = journal.Entries();
    counts.entries = static_cast<uint32_t>(entries.size());
    for (const std::filesystem::path& staging : entries) {
        if (Canceled(cancel)) {
            ++counts.canceled;
            continue;
        }
        std::wstring output;
        int64_t endUs = 0;
        const auto outcome = static_cast<ce::media::RecoveryOutcome>(recoverOne(staging, &output, &endUs));
        switch (outcome) {
            case ce::media::RecoveryOutcome::kRecovered:
                ++counts.recovered;
                recovered.push_back(RecoveredRecording{output, endUs});
                break;
            case ce::media::RecoveryOutcome::kDiscardedEmpty:
                ++counts.discarded;
                break;
            case ce::media::RecoveryOutcome::kKeptIncomplete:
                ++counts.keptIncomplete;
                break;
            case ce::media::RecoveryOutcome::kWriterAlive:
                ++counts.writerAlive;
                break;
            case ce::media::RecoveryOutcome::kMissing:
                ++counts.missing;
                break;
            case ce::media::RecoveryOutcome::kFailed:
            default:
                ++counts.failed;
                break;
        }
        // A cancel inside a file reports kFailed: the entry stays, like any other retryable outcome.
        if (ce::media::RecoveryIsTerminal(outcome))
            journal.Remove(staging);
    }
    return recovered;
}

std::vector<RecoveredRecording> RecoverInterruptedRecordings(const std::wstring& packageDir,
                                                             const volatile long* cancel) {
    const ce::capture_output::InFlightJournal journal = ce::capture_output::InFlightJournal::ForThisUser();
    if (!journal || journal.Entries().empty())
        return {};
    // The recovery runs in the client's process (or the app's): mediaengine and FFmpeg load scoped to this load and
    // never change the process's DLL search order (DR-39). The loader is reference counted, so a media role that
    // records at the same time keeps its module.
    if (!MediaEngine_Load(packageDir, MediaEngineDllSearch::LoadScoped)) {
        LogWarn("[RecordingRecovery] mediaengine.dll could not be loaded; interrupted recordings stay for the next run");
        return {};
    }
    RecoveryPassSummary summary;
    const RecoverOneFn recoverOne = [cancel](const std::filesystem::path& staging, std::wstring* output,
                                             int64_t* endUs) {
        wchar_t buffer[32768];
        const int outcome = ce::mediaengine_api::MediaEngine_RecoverRecording(
            staging.c_str(), buffer, static_cast<uint32_t>(sizeof(buffer) / sizeof(buffer[0])), endUs, cancel);
        *output = buffer;
        return outcome;
    };
    std::vector<RecoveredRecording> recovered = RecoverJournalEntries(journal, recoverOne, cancel, &summary);
    MediaEngine_Unload();
    LogInfo("[RecordingRecovery] journal entries=%u recovered=%u discarded=%u kept_incomplete=%u writer_alive=%u "
            "missing=%u failed=%u canceled=%u",
            summary.entries, summary.recovered, summary.discarded, summary.keptIncomplete, summary.writerAlive,
            summary.missing, summary.failed, summary.canceled);
    for (const RecoveredRecording& recording : recovered)
        LogInfo("[RecordingRecovery] repaired %s (tracks end at %lld ms)",
                ce::privacy::CollapsePathForLog(Utf8(recording.output)).c_str(),
                static_cast<long long>(recording.endUs / 1000));
    return recovered;
}

}  // namespace ce::role
