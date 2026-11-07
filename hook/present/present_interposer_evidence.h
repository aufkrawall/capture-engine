#pragma once

#include <cstdint>

#include "hook/present/present_interposer_cadence.h"

// What CE knows about a present interposer's application frames, and how it decides on the status.
//
// The factor needs two streams, and only the output one is always there. The application-facing Present is CE's to
// see only where the game's create reached CE's factory wrapper (DX12); DX11 games usually reach the real factory
// through IDXGIDevice::GetAdapter, and in Vulkan CE's layer sits below VK_LAYER_NV_present. So:
//
// 1. The application's own presents, where CE has them. Exact.
// 2. Runs on the interposer's worker threads. NvPresent hands each application frame to one of its workers, which
//    presents all of that frame's outputs. Trine 5 DX11 (20261106_151156, 20261106_171629): 51574 presents
//    in 25711 runs, 25639 of length 2, 70 of length 4 or 6 and 2 of length 1, at 20 to 234 base fps, capped and
//    uncapped. It holds where timing says nothing: the same runs had gaps of 10000/3900 us, 6000/500 us and
//    13400/500 us. A worker can get two frames in a row, so the factor is the most common run length and a longer
//    run counts as that many frames.
// 3. The game's own draws, on a thread that always presents (fg_runtime_state.h): one-sided, because a game that is
//    still drawing while the interposer presents a generated frame reads as an application frame.
namespace ce::present_interposer {

enum class EvidenceStream { kNone, kWorkerRuns, kApplicationPresents, kApplicationSubmissions };

inline const char* EvidenceStreamName(EvidenceStream stream) {
    switch (stream) {
        case EvidenceStream::kWorkerRuns:
            return "worker-runs";
        case EvidenceStream::kApplicationPresents:
            return "application-presents";
        case EvidenceStream::kApplicationSubmissions:
            return "application-submissions";
        case EvidenceStream::kNone:
        default:
            return "none";
    }
}

// One output present. `measured` is false where nothing said which stream it belongs to.
struct OutputPresentClass {
    bool measured = false;
    bool applicationFrame = true;
};

struct WindowReport {
    CadenceVerdict verdict;
    EvidenceStream stream = EvidenceStream::kNone;
    // The worker runs behind the window, or the last ones seen beside an application stream: a cross-check.
    int runs = 0;
    int modalRun = 0;
};

// Splits the output stream into per-thread runs. Serialized by the owner.
class WorkerRunWindow {
public:
    // This long on one thread is a thread that presents everything, not a pool: the runs then say nothing. Repeats on
    // a pool end long before it (at most three frames of two in the sessions above).
    static constexpr int kDedicatedPresenterRun = 32;

    struct Result {
        OutputPresentClass present;
        bool windowClosed = false;
        WindowReport report;
    };

    Result Note(uint32_t threadId, int64_t nowUs) {
        Result result;
        // A stall (alt-tab, loading) is no frame rate, and what came before it says nothing about what follows.
        if (lastPresentUs_ != kNoTime && nowUs - lastPresentUs_ >= kWindowUs) {
            ClearWindow();
            runLength_ = 0;
            windowStartUs_ = kNoTime;
        }
        lastPresentUs_ = nowUs;
        if (windowStartUs_ == kNoTime) {
            windowStartUs_ = nowUs;
        }
        if (threadId != runThread_ || runLength_ == 0) {
            CloseRun();
            // Only here, between runs, so that a window counts every run whole.
            CloseWindowIfDue(nowUs, result);
            runThread_ = threadId;
            runLength_ = 0;
        }
        if (runLength_ < kDedicatedPresenterRun) {
            ++runLength_;
        }
        const bool dedicated = runLength_ >= kDedicatedPresenterRun;
        if (dedicated) {
            ClearWindow();
            windowStartUs_ = nowUs;
        }
        result.present.measured = !dedicated;
        result.present.applicationFrame = (runLength_ - 1) % (factor_ > 0 ? factor_ : kDedicatedPresenterRun) == 0;
        return result;
    }

    void Reset() {
        ClearWindow();
        runThread_ = 0;
        runLength_ = 0;
        factor_ = 0;
        windowStartUs_ = kNoTime;
        lastPresentUs_ = kNoTime;
    }

private:
    static constexpr int64_t kNoTime = -1;

    void CloseRun() {
        if (runLength_ > 0 && runLength_ < kDedicatedPresenterRun) {
            ++histogram_[runLength_];
            ++runs_;
            outputs_ += runLength_;
        }
    }

    void ClearWindow() {
        for (int& count : histogram_) {
            count = 0;
        }
        runs_ = 0;
        outputs_ = 0;
    }

    // The most common length, the shorter one on a tie: repeated frames make multiples of it, always less often.
    int ModalRun() const {
        int modal = 0;
        for (int length = 1; length < kDedicatedPresenterRun; ++length) {
            if (histogram_[length] > (modal > 0 ? histogram_[modal] : 0)) {
                modal = length;
            }
        }
        return modal;
    }

    int ApplicationFrames(int modalRun) const {
        int frames = 0;
        for (int length = 1; length < kDedicatedPresenterRun; ++length) {
            const int framesInRun = (length + modalRun / 2) / modalRun;
            frames += histogram_[length] * (framesInRun > 0 ? framesInRun : 1);
        }
        return frames;
    }

    void CloseWindowIfDue(int64_t nowUs, Result& result) {
        const int64_t elapsedUs = nowUs - windowStartUs_;
        if (runs_ == 0 || elapsedUs < kWindowUs) {
            return;
        }
        const int modalRun = ModalRun();
        const int frames = ApplicationFrames(modalRun);
        if (!HasEnoughApplicationFrames(frames, elapsedUs) && elapsedUs < kLongWindowUs) {
            return;
        }
        result.windowClosed = true;
        result.report.verdict = ClassifyInterposerCadence(frames, outputs_, elapsedUs);
        result.report.stream = EvidenceStream::kWorkerRuns;
        result.report.runs = runs_;
        result.report.modalRun = modalRun;
        if (result.report.verdict.decided) {
            factor_ = modalRun;
        }
        ClearWindow();
        windowStartUs_ = nowUs;
    }

    int histogram_[kDedicatedPresenterRun] = {};
    int runs_ = 0;
    int outputs_ = 0;
    uint32_t runThread_ = 0;
    int runLength_ = 0;
    // The last decided window's modal run. Unknown at first: then only a run's first present is a frame.
    int factor_ = 0;
    int64_t windowStartUs_ = kNoTime;
    int64_t lastPresentUs_ = kNoTime;
};

// The published status. A decided window that says "not generating" takes a few to believe: a scene change or a hitch
// can read 1x for a window, and a status that flips with it is the one thing the overlay must not do. An undecided
// window says nothing at all.
class GenerationLatch {
public:
    static constexpr int kNotGeneratingWindowsToClear = 3;

    bool Observe(const CadenceVerdict& verdict) {
        if (!verdict.decided) {
            return active_;
        }
        if (verdict.generating) {
            active_ = true;
            notGeneratingWindows_ = 0;
        } else if (active_ && ++notGeneratingWindows_ >= kNotGeneratingWindowsToClear) {
            active_ = false;
            notGeneratingWindows_ = 0;
        }
        return active_;
    }

    void Reset() {
        active_ = false;
        notGeneratingWindows_ = 0;
    }

private:
    bool active_ = false;
    int notGeneratingWindows_ = 0;
};

// Picks the best evidence there is. Serialized by the owner.
class InterposerEvidence {
public:
    // Returns true and fills `report` when this output present closed a window of the stream in charge.
    bool NoteOutput(uint32_t threadId, int64_t nowUs, OutputPresentClass* present, WindowReport* report) {
        application_.NoteOutputPresent();
        const WorkerRunWindow::Result runs = runs_.Note(threadId, nowUs);
        *present = runs.present;
        if (runs.present.measured && applicationSource_ == EvidenceStream::kApplicationSubmissions) {
            ResetApplication();
        }
        if (!runs.windowClosed) {
            return false;
        }
        lastRuns_ = runs.report;
        if (lastApplicationUs_ != kNoTime && nowUs - lastApplicationUs_ < kLongWindowUs) {
            return false;
        }
        *report = runs.report;
        return true;
    }

    bool HasDirectApplicationStream(int64_t nowUs) const {
        return applicationSource_ == EvidenceStream::kApplicationPresents &&
               nowUs - lastApplicationUs_ < kApplicationStoppedWindowUs;
    }

    // Direct and inferred frames cannot share a measurement window.
    bool NoteApplication(int64_t nowUs, WindowReport* report,
                         EvidenceStream source = EvidenceStream::kApplicationPresents) {
        if (source != applicationSource_ || nowUs - lastApplicationUs_ >= kApplicationStoppedWindowUs) {
            ResetApplication();
        }
        applicationSource_ = source;
        lastApplicationUs_ = nowUs;
        CadenceVerdict verdict;
        if (!application_.NoteApplicationPresent(nowUs, &verdict)) {
            return false;
        }
        report->verdict = verdict;
        report->stream = source;
        report->runs = lastRuns_.runs;
        report->modalRun = lastRuns_.modalRun;
        return true;
    }

    void Reset() {
        ResetApplication();
        runs_.Reset();
        lastRuns_ = WindowReport{};
    }

private:
    static constexpr int64_t kNoTime = -1;

    void ResetApplication() {
        application_.Reset();
        applicationSource_ = EvidenceStream::kNone;
        lastApplicationUs_ = kNoTime;
    }

    CadenceTracker application_;
    WorkerRunWindow runs_;
    WindowReport lastRuns_;
    EvidenceStream applicationSource_ = EvidenceStream::kNone;
    int64_t lastApplicationUs_ = kNoTime;
};

}  // namespace ce::present_interposer
