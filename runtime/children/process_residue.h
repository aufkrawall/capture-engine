#pragma once

// What a finished piece of work left behind in this process. A media helper process returns everything to Windows
// when it exits; on a runtime thread (SDK clients, or the app with isolated_helpers=off) the media session must
// release it explicitly (09, M17). The meter takes a baseline when the first tracked session starts after idle and
// reports the difference when the last one ends, so a leak shows up as a growing residue across recordings in every
// session log, not only in a dedicated test (11, part 2).

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace ce::runtime {

struct ProcessResidueSample {
    uint32_t handles = 0;
    uint32_t threads = 0;
    uint32_t gdiObjects = 0;
    uint32_t userObjects = 0;
    uint64_t privateBytes = 0;
    uint64_t videoMemoryBytes = 0;  // this process's local + non-local usage summed over the adapters
    bool videoMemoryKnown = false;
};

// Reads this process's counters (handles, threads, GDI/USER objects, private bytes, video memory).
ProcessResidueSample SampleThisProcess();

// "handles=+2 threads=+0 gdi=+0 user=+0 private=+1.5MiB vram=+0.0MiB" (vram=n/a when either sample lacks it).
std::string DescribeResidue(const ProcessResidueSample& before, const ProcessResidueSample& after);

// Tracks overlapping sessions (recordings) of one kind. Not thread-safe: one owner thread.
class ResidueMeter {
public:
    explicit ResidueMeter(std::function<ProcessResidueSample()> sample);

    // A session is about to start. The first one after idle takes the baseline, before the session exists.
    void Starting();
    // A session that Starting() announced never started; forgets it without a report.
    void Abandoned();
    // A session ended and released what it owns. Returns "sessions=K <residue>" when it was the last live one.
    std::optional<std::string> Ended();

    uint32_t Live() const { return live_; }

private:
    std::function<ProcessResidueSample()> sample_;
    ProcessResidueSample baseline_;
    uint32_t live_ = 0;
    uint32_t sessionsSinceBaseline_ = 0;
};

}  // namespace ce::runtime
