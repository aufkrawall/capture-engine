#pragma once

#include <array>
#include <cstdint>

// When a stretch with nothing on screen deserves its raw trace (common/platform/display_flight_recorder.h).
//
// The same judgement as the hook's [SCANOUT STALL] (hook/present/scanout_stall_policy.h), made in the sensor from the
// two streams it already has: the tracked process's presents (DXGI PresentStart) and the screen changes the display
// timing service publishes for it (its own flips and, while the compositor is followed, its composed frames). A
// silence starts at the first present after the last screen change and ends at the next change.
//
// GTA V Enhanced loads into the game with DLSS-G already on and the screen stands still while it presents:
// 38.06 s (20261110_222057), 1.70 s (20261111_013758), 1.38 s (20261111_015744). Every other FG switch in those
// sessions stayed under 0.66 s. The first version of this trigger watched the submission-expiry episodes instead,
// which only start once a present is older than the completion bound (~1 s) and run every 250 ms: it counted the
// 1.70 s silence as 531 ms and never saw the 1.38 s one.
//
// A silence counts only while the process owns the foreground window (an alt-tabbed or minimized game's presents never
// show) and keeps presenting (a gap longer than kMaximumPresentGapUs starts it over: a loading hitch is no display
// stall). Pure; the display timing service feeds it.
namespace display_stall_trace {

inline constexpr int64_t kMinimumSilenceUs = 1'000'000;
inline constexpr int64_t kOngoingSealUs = 10'000'000;
inline constexpr uint32_t kMinimumPresents = 8;
inline constexpr int64_t kMaximumPresentGapUs = 500'000;
inline constexpr uint32_t kMaximumSealsPerSession = 6;
inline constexpr std::size_t kMaximumProcesses = 4;

enum class SealReason : uint8_t { kNone, kOngoing, kEnded };

inline const char* SealReasonName(SealReason reason) {
    switch (reason) {
        case SealReason::kOngoing:
            return "ongoing";
        case SealReason::kEnded:
            return "ended";
        case SealReason::kNone:
        default:
            return "none";
    }
}

struct SealRequest {
    SealReason reason = SealReason::kNone;
    uint32_t episode = 0;
    uint32_t processId = 0;
    uint64_t silenceMs = 0;
    int64_t silenceStartUs = 0;  // QPC microseconds of the first present that never reached the screen
    uint32_t presents = 0;       // presents during the silence
    explicit operator bool() const noexcept { return reason != SealReason::kNone; }
};

class Policy {
public:
    // A present of a tracked process, at its PresentStart (QPC microseconds).
    void OnPresent(uint32_t processId, int64_t us) {
        Process* process = Find(processId, true);
        if (!process)
            return;
        process->lastSeenUs = us;
        if (process->lastChangeUs == 0 || us <= process->lastChangeUs)
            return;  // nothing shown yet to measure from, or a present the published change already covers
        if (process->firstUs == 0 || us - process->lastPresentUs > kMaximumPresentGapUs)
            process->StartSilence(us);
        process->lastPresentUs = us;
        ++process->presents;
    }

    // A screen change published for a tracked process (its own flip or a composed frame), at its screen time.
    void OnScreenChange(uint32_t processId, int64_t us) {
        Process* process = Find(processId, true);
        if (!process)
            return;
        if (us > process->lastSeenUs)
            process->lastSeenUs = us;
        if (process->firstUs != 0 && us > process->firstUs) {
            const int64_t silenceUs = us - process->firstUs;
            if (process->Qualifies(silenceUs) && !pending_)
                pending_ = Take(*process, SealReason::kEnded, silenceUs);
            process->firstUs = 0;
        }
        if (us > process->lastChangeUs)
            process->lastChangeUs = us;
    }

    // Every prune: the foreground window's process. Returns at most one seal.
    SealRequest Observe(int64_t nowUs, uint32_t foregroundProcessId) {
        for (Process& process : processes_) {
            if (process.processId != 0 && process.processId != foregroundProcessId)
                process.background = true;
        }
        if (pending_) {
            const SealRequest request = pending_;
            pending_ = {};
            return request;
        }
        Process* process = Find(foregroundProcessId, false);
        if (!process || process->firstUs == 0 || process->ongoingSealed)
            return {};
        const int64_t silenceUs = nowUs - process->firstUs;
        if (silenceUs < kOngoingSealUs || nowUs - process->lastPresentUs > kMaximumPresentGapUs ||
            !process->Qualifies(silenceUs))
            return {};
        process->ongoingSealed = true;
        return Take(*process, SealReason::kOngoing, silenceUs);
    }

    uint32_t seals() const noexcept { return seals_; }
    uint32_t refusedForLimit() const noexcept { return refusedForLimit_; }

private:
    struct Process {
        uint32_t processId = 0;
        int64_t lastChangeUs = 0;
        int64_t firstUs = 0;  // 0: no silence running
        int64_t lastPresentUs = 0;
        int64_t lastSeenUs = 0;  // the latest present or change: the least recent process makes room for a new one
        uint32_t presents = 0;
        uint32_t episode = 0;
        bool background = false;
        bool ongoingSealed = false;

        void StartSilence(int64_t us) {
            firstUs = us;
            presents = 0;
            episode = 0;
            background = false;
            ongoingSealed = false;
        }
        bool Qualifies(int64_t silenceUs) const {
            return !background && silenceUs >= kMinimumSilenceUs && presents >= kMinimumPresents;
        }
    };

    Process* Find(uint32_t processId, bool create) {
        if (processId == 0)
            return nullptr;
        Process* oldest = &processes_[0];
        for (Process& process : processes_) {
            if (process.processId == processId)
                return &process;
            if (oldest->processId != 0 && (process.processId == 0 || process.lastSeenUs < oldest->lastSeenUs))
                oldest = &process;
        }
        if (!create)
            return nullptr;
        *oldest = Process{};
        oldest->processId = processId;
        return oldest;
    }

    SealRequest Take(Process& process, SealReason reason, int64_t silenceUs) {
        if (process.episode == 0)
            process.episode = ++episodes_;
        if (seals_ >= kMaximumSealsPerSession) {
            ++refusedForLimit_;
            return {};
        }
        ++seals_;
        return {reason,          process.episode, process.processId, static_cast<uint64_t>(silenceUs / 1000),
                process.firstUs, process.presents};
    }

    std::array<Process, kMaximumProcesses> processes_{};
    SealRequest pending_;
    uint32_t episodes_ = 0;
    uint32_t seals_ = 0;
    uint32_t refusedForLimit_ = 0;
};

}  // namespace display_stall_trace
