#pragma once

// Why a proactive Streamline feature-function query (DLSS-G, Reflex, PCL) was
// not attempted. Session 20261014_204616 logged "runtime is unloading" for
// sl.reflex.dll and sl.pcl.dll at 38.275 s while both plugins only loaded at
// 38.75 s: every refusal of ScopedStreamlineFeatureQueryGuard was reported as a
// teardown. Each reason now names what actually happened.
namespace ce::streamline_runtime_policy {

enum class FeatureQuerySkip {
    None,              // The guard holds every sl.* module; the query may run.
    FeatureNotLoaded,  // The queried plugin is not mapped yet (or any more).
    NoInterposer,      // No active sl.interposer.dll to query through.
    TeardownInFlight,  // A tracked Streamline teardown is in progress.
    UnloadRaced,       // A tracked sl.* unload started while the modules were being pinned.
    PinFailed,         // A listed sl.* module vanished before it could be pinned.
    NotListed,         // The interposer or plugin was missing from the module snapshot.
    PinCapacity,       // More sl.* modules than the guard can pin.
    SnapshotFailed,    // The loaded-module snapshot could not be taken.
};

inline const char* FeatureQuerySkipName(FeatureQuerySkip skip) {
    switch (skip) {
        case FeatureQuerySkip::None: return "none";
        case FeatureQuerySkip::FeatureNotLoaded: return "plugin not loaded";
        case FeatureQuerySkip::NoInterposer: return "no active interposer";
        case FeatureQuerySkip::TeardownInFlight: return "teardown in flight";
        case FeatureQuerySkip::UnloadRaced: return "unload raced the module pins";
        case FeatureQuerySkip::PinFailed: return "module vanished before its pin";
        case FeatureQuerySkip::NotListed: return "module missing from snapshot";
        case FeatureQuerySkip::PinCapacity: return "too many sl.* modules to pin";
        case FeatureQuerySkip::SnapshotFailed: return "module snapshot failed";
    }
    return "unknown";
}

// Only these mean the Streamline runtime is (being) torn down.
inline bool IsTeardownFeatureQuerySkip(FeatureQuerySkip skip) {
    return skip == FeatureQuerySkip::TeardownInFlight || skip == FeatureQuerySkip::UnloadRaced ||
           skip == FeatureQuerySkip::PinFailed;
}

// A plugin or interposer that has not loaded yet is the ordinary state before
// slInit has finished; the next module load retries, so it is not logged.
inline bool ShouldLogFeatureQuerySkip(FeatureQuerySkip skip) {
    return skip != FeatureQuerySkip::None && skip != FeatureQuerySkip::FeatureNotLoaded &&
           skip != FeatureQuerySkip::NoInterposer;
}

}  // namespace ce::streamline_runtime_policy
