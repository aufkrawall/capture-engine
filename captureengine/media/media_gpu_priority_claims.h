#pragma once

// Process-wide claims on the process GPU scheduling priority class (DR-39). Media sessions can overlap in one process
// (a finalizing recording and the next one), and they share that single class. Every session whose request is in
// effect holds a claim. The class the process had before the first claim is written back when the last claim is
// released, so a session that ends while another still holds one leaves the priority in place for it. When the class
// before the first claim could not be read, nothing is written back.
//
// Not thread-safe: the caller serializes Claim and Release (media_main_priority.cpp holds its priority mutex).

#include <functional>
#include <optional>
#include <vector>

namespace ce::media_gpu_priority {

// Writes the process GPU scheduling priority class; returns the call's status (negative on failure).
using SetProcessClassFn = std::function<long(int priorityClass)>;

struct ClaimResult {
    bool added = false;       // the claimant did not hold a claim before this call
    bool started = false;     // this claim opened a new set and recorded the class before the change
    bool priorKnown = false;  // the class before the change of the current set is known
    int priorClass = -1;      // valid when priorKnown
    int claimants = 0;        // claims held after this call
};

struct ReleaseResult {
    bool released = false;     // the claimant held a claim
    int claimants = 0;         // claims held after this call
    bool attempted = false;    // the last claim was released and the previous class was written back
    bool restored = false;     // that write succeeded
    long status = 0;           // status of that write (valid when attempted)
    int restoredClass = -1;    // valid when attempted
    bool priorUnknown = false; // the last claim was released, but the previous class was never read: nothing written
};

class GpuSchedulingPriorityClaims {
public:
    explicit GpuSchedulingPriorityClaims(SetProcessClassFn setClass);

    // `classBeforeChange` is the process class read before this request changes it; nullopt when that read failed.
    // Only the claim that opens a set uses it: a later claimant never sees the class the earlier one set.
    ClaimResult Claim(const void* claimant, std::optional<int> classBeforeChange);
    ReleaseResult Release(const void* claimant);

private:
    SetProcessClassFn setClass_;
    std::vector<const void*> claimants_;
    bool priorKnown_ = false;
    int priorClass_ = -1;
};

}  // namespace ce::media_gpu_priority

// Restores the process GPU scheduling priority this media session claimed (DR-39). Called at session Shutdown; defined
// in media_main_priority.cpp, which resolves the D3DKMT entry points.
void RestoreMediaGpuSchedulingPriority();
