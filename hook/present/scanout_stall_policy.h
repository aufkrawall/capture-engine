#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

// When the screen stops changing while the game keeps presenting, and how long a new swapchain takes to reach it.
//
// GTA V Enhanced, session 20261110_182518 (0.1.7498): the DLSS-G swapchain presented ~131 times a second for 19.5 s
// right after its handoff and no frame reached the screen (sensors.log: `expire without a flip completion`,
// published=0, vblank at the VRR floor). CE's own cost was normal, and nothing in the hook log could say whether the
// window was covered, cloaked or occluded, whether DXGI dropped the presents, or whether the compositor simply never
// composed the new chain. This is the judgement half of the report that records those facts in one place; the
// collection half is scanout_stall_report.h. Pure: no Windows calls, unit-tested.
//
// Driven by presents only. The display stream is what the sensor service publishes: the writer's sequence and
// generation move whenever a transition (a flip, or a composition of the process) reaches the screen.

namespace ce::scanout_stall {

// What SharedDisplayTiming says at one present.
struct DisplayObservation {
    bool active = false;      // the stream is Active (published at least once, not failed)
    uint64_t generation = 0;  // publicationGeneration: a reset restarts the sequence
    uint64_t sequence = 0;    // writeSequence
    int64_t publishUs = 0;    // lastPublishQpcUs on the display-timing clock; 0 = nothing published yet

    bool SameAs(const DisplayObservation& other) const {
        return generation == other.generation && sequence == other.sequence;
    }
};

// A stall episode: the display has not changed for kStallAgeUs although the game kept presenting.
inline constexpr int64_t kStallAgeUs = 250'000;
inline constexpr uint32_t kStallMinPresents = 16;
// A snapshot of a running episode is repeated at most this often.
inline constexpr int64_t kStallDiffIntervalUs = 2'000'000;

enum class StallEvent : uint8_t {
    kNone,
    kStart,  // the episode began: take the full snapshot
    kDiff,   // the episode goes on: a compact update
    kEnd,    // the display changed again
};

struct StallStep {
    StallEvent event = StallEvent::kNone;
    int64_t ageUs = 0;                  // now - the last display change (kEnd: until the change was seen)
    uint32_t presentsSinceChange = 0;   // presents since the last display change
    uint64_t episode = 0;               // 1-based, counts every episode of the process
    int64_t episodeUs = 0;              // kEnd: from the last change before the episode to the first one after it
    uint32_t episodePresents = 0;       // kEnd: presents of the whole episode
    uint64_t sequenceAdvance = 0;       // kEnd: transitions published at once on the change (0 after a reset)
    bool generationChanged = false;     // kEnd: the stream was reset instead of advancing
};

class StallTracker {
public:
    StallStep OnPresent(int64_t nowUs, const DisplayObservation& observation) {
        StallStep step;
        step.episode = episode_;
        if (!observation.active) {
            // A stream that is not publishing says nothing about the screen.
            Forget();
            return step;
        }
        if (!haveBaseline_) {
            Rebase(nowUs, observation);
            return step;
        }
        if (!observation.SameAs(last_)) {
            const bool inEpisode = inEpisode_;
            step.ageUs = nowUs - lastChangeUs_;
            step.generationChanged = observation.generation != last_.generation;
            step.sequenceAdvance = (!step.generationChanged && observation.sequence > last_.sequence)
                                       ? observation.sequence - last_.sequence
                                       : 0;
            if (inEpisode) {
                step.event = StallEvent::kEnd;
                step.episodeUs = nowUs - lastChangeUs_;
                step.episodePresents = presentsSinceChange_ + 1;
            }
            step.presentsSinceChange = presentsSinceChange_;
            Rebase(nowUs, observation);
            step.episode = episode_;
            return step;
        }
        ++presentsSinceChange_;
        step.ageUs = nowUs - lastChangeUs_;
        step.presentsSinceChange = presentsSinceChange_;
        if (!inEpisode_) {
            if (step.ageUs >= kStallAgeUs && presentsSinceChange_ >= kStallMinPresents) {
                inEpisode_ = true;
                ++episode_;
                lastDiffUs_ = nowUs;
                step.event = StallEvent::kStart;
                step.episode = episode_;
            }
        } else if (nowUs - lastDiffUs_ >= kStallDiffIntervalUs) {
            lastDiffUs_ = nowUs;
            step.event = StallEvent::kDiff;
        }
        return step;
    }

    bool InEpisode() const { return inEpisode_; }
    uint64_t Episodes() const { return episode_; }

    // A new swapchain starts a new count of presents; the display stream itself carries on.
    void NoteNewSwapchain() { presentsSinceChange_ = 0; }

private:
    void Rebase(int64_t nowUs, const DisplayObservation& observation) {
        last_ = observation;
        haveBaseline_ = true;
        lastChangeUs_ = observation.publishUs > 0 ? observation.publishUs : nowUs;
        presentsSinceChange_ = 0;
        inEpisode_ = false;
    }
    void Forget() {
        haveBaseline_ = false;
        inEpisode_ = false;
        presentsSinceChange_ = 0;
    }

    DisplayObservation last_;
    bool haveBaseline_ = false;
    bool inEpisode_ = false;
    int64_t lastChangeUs_ = 0;
    int64_t lastDiffUs_ = 0;
    uint32_t presentsSinceChange_ = 0;
    uint64_t episode_ = 0;
};

// The first change of the display after a swapchain handoff: how long the new chain took to reach the screen, and how
// many of its presents it made before that. Armed once per handoff.
struct FirstChange {
    bool reached = false;
    uint64_t handoff = 0;
    int64_t sinceHandoffUs = 0;
    uint32_t presentsBefore = 0;
    bool generationChanged = false;
};

class FirstChangeWatch {
public:
    void Arm(uint64_t handoff, uint64_t chain, int64_t armUs, const DisplayObservation& baseline) {
        armed_ = true;
        handoff_ = handoff;
        chain_ = chain;
        armUs_ = armUs;
        baseline_ = baseline;
        presents_ = 0;
    }

    // `chain` is the swapchain this present belongs to; only the armed chain's presents are counted.
    FirstChange OnPresent(uint64_t chain, int64_t nowUs, const DisplayObservation& observation) {
        FirstChange result;
        if (!armed_) {
            return result;
        }
        if (observation.active && !observation.SameAs(baseline_)) {
            result.reached = true;
            result.handoff = handoff_;
            result.sinceHandoffUs = nowUs - armUs_;
            result.presentsBefore = presents_;
            result.generationChanged = observation.generation != baseline_.generation;
            armed_ = false;
            return result;
        }
        if (chain == chain_) {
            ++presents_;
        }
        return result;
    }

    bool Armed() const { return armed_; }

private:
    bool armed_ = false;
    uint64_t handoff_ = 0;
    uint64_t chain_ = 0;
    int64_t armUs_ = 0;
    DisplayObservation baseline_;
    uint32_t presents_ = 0;
};

// How a swapchain's Presents ended: DXGI drops the presents of an occluded window and says so in the result, so a
// histogram of results tells "dropped" from "shown". Distinct values, most frequent first on output.
class PresentResultTally {
public:
    static constexpr size_t kSlots = 6;

    void Reset() {
        for (auto& slot : slots_) {
            slot = {};
        }
        other_ = 0;
        total_ = 0;
    }

    void Note(int32_t result) {
        ++total_;
        for (auto& slot : slots_) {
            if (slot.count != 0 && slot.result == result) {
                ++slot.count;
                return;
            }
        }
        for (auto& slot : slots_) {
            if (slot.count == 0) {
                slot = {result, 1};
                return;
            }
        }
        ++other_;
    }

    uint64_t total() const { return total_; }
    uint64_t other() const { return other_; }

    // "0x00000000=1520 0x087A0001=3 other=1", most frequent first; an empty tally: "none". Always terminates a
    // non-empty buffer and returns the number of characters written.
    size_t Describe(char* out, size_t capacity) const {
        if (!out || capacity == 0) {
            return 0;
        }
        out[0] = '\0';
        Slot sorted[kSlots] = {};
        size_t count = 0;
        for (const auto& slot : slots_) {
            if (slot.count != 0) {
                sorted[count++] = slot;
            }
        }
        for (size_t i = 1; i < count; ++i) {
            const Slot value = sorted[i];
            size_t j = i;
            for (; j > 0 && sorted[j - 1].count < value.count; --j) {
                sorted[j] = sorted[j - 1];
            }
            sorted[j] = value;
        }
        size_t written = 0;
        for (size_t i = 0; i < count; ++i) {
            written = Advance(capacity, written,
                              std::snprintf(out + written, capacity - written, "%s0x%08X=%llu", i ? " " : "",
                                            static_cast<unsigned>(sorted[i].result),
                                            static_cast<unsigned long long>(sorted[i].count)));
        }
        if (other_ != 0) {
            written = Advance(capacity, written,
                              std::snprintf(out + written, capacity - written, "%sother=%llu", count ? " " : "",
                                            static_cast<unsigned long long>(other_)));
        }
        if (count == 0 && other_ == 0) {
            written = Advance(capacity, written, std::snprintf(out, capacity, "none"));
        }
        return written;
    }

private:
    struct Slot {
        int32_t result = 0;
        uint64_t count = 0;
    };

    // snprintf reports what it wanted to write: a truncated line stays terminated inside the buffer.
    static size_t Advance(size_t capacity, size_t written, int printed) {
        if (printed < 0) {
            return written;
        }
        return static_cast<size_t>(printed) >= capacity - written ? capacity - 1 : written + static_cast<size_t>(printed);
    }

    Slot slots_[kSlots] = {};
    uint64_t other_ = 0;
    uint64_t total_ = 0;
};

}  // namespace ce::scanout_stall
