#pragma once

// Out-of-line FpsLimiter members for a present interposer's outputs (PresentSite::kInterposerOutputPresent). Included by
// fps_limiter.h after the class body. See ce::interposer_output_pacing for why each output of a capped frame gets a slot.

#include "hook/pacing/fps_limiter.h"

// The group's first output went through the cadence: its later outputs get slots if it waited for its own.
inline void FpsLimiter::NoteInterposerGroupCadence(const LocalCadenceResult& cadence, int multiplier) {
    if (!cadence.waited) {
        return;  // below the cap: the schedule stays clear and the driver spaces this frame
    }
    interposerSchedule_.Arm(GetCurrentThreadId(), cadence.deadlineQpc, cadence.nextDeadlineQpc, multiplier);
    if (!interposerSpacingLogged_ && multiplier > 1) {
        interposerSpacingLogged_ = true;
        TraceLog("Apply: INTERPOSER outputs on the output grid multiplier=%d groupIntervalUs=%lld", multiplier,
                 (cadence.nextDeadlineQpc - cadence.deadlineQpc) * 1000000 / qpcFrequency);
        HookLog(
            "FPS Limiter: pacing the present interposer's outputs, each on its own slot of the output grid "
            "(multiplier=%d); a frame that misses its slot is left to the driver's own spacing",
            multiplier);
    }
}

// A later output of the group this thread opened. Under the cadence lock, like every other wait: the first output of the
// next group waits behind it, and its deadline is later anyway.
inline void FpsLimiter::SpaceInterposerOutput(uint32_t outputIndex) {
    std::lock_guard<std::mutex> cadenceLock(cadenceMutex_);
    lastActualWaitUs_ = 0;
    const int64_t deadline = interposerSchedule_.OutputDeadline(GetCurrentThreadId(), outputIndex);
    const int64_t now = ReadClock();
    ++interposerOutputWindow_.outputs;
    if (deadline == 0) {
        ++interposerOutputWindow_.unscheduled;
    } else if (deadline <= now) {
        const int64_t lateUs = (now - deadline) * 1000000 / qpcFrequency;
        ++interposerOutputWindow_.late;
        interposerOutputWindow_.lateUsSum += lateUs;
        interposerOutputWindow_.lateUsMax = (std::max)(interposerOutputWindow_.lateUsMax, lateUs);
    } else {
        // This output's overlay reaches the GPU before CE blocks, as for any other wait.
        RunPreWaitSubmission();
        SmartWait(deadline);
        lastActualWaitUs_ = (ReadClock() - now) * 1000000 / qpcFrequency;
        ++interposerOutputWindow_.spaced;
        interposerOutputWindow_.waitUsSum += lastActualWaitUs_;
    }
    if (interposerOutputWindow_.outputs < kInterposerOutputWindowOutputs) {
        return;
    }
    const auto& window = interposerOutputWindow_;
    HookLog(
        "FPS Limiter: present interposer output spacing (%u outputs): spaced=%u waitMeanUs=%lld late=%u lateMeanUs=%lld "
        "lateMaxUs=%lld unscheduled=%u",
        window.outputs, window.spaced, window.spaced != 0 ? window.waitUsSum / window.spaced : 0, window.late,
        window.late != 0 ? window.lateUsSum / window.late : 0, window.lateUsMax, window.unscheduled);
    interposerOutputWindow_ = {};
}
