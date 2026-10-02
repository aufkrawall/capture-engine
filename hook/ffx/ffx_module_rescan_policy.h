#pragma once

#include <cstdint>

// When the FFX hook install pass must repeat its expensive half.
//
// The hook thread re-enters ffx_hook_InstallHooksForModule about once a second. Its cheap half
// (GetProcAddress of the three exports, confirming the entry breakpoint byte) is idempotent and
// stays per pass. Its expensive half is not: three IATHook::PatchIATAllModules walks over every
// loaded module plus ffx_cached_pointer_router::Refresh, which reads every pointer slot of every
// client module's writable data (GTA Enhanced: 39 MB of .data). Session 20260925_225006 measured
// that half at ~115 ms per second on the hook thread for the whole FSR FG session, sweeping the
// L3 cache under a GPU-bound game, and it found nothing new after the first pass.
//
// That work can only find something new when one of its inputs changed:
//   - a different FFX runtime image (first sight, or a reload at a new base);
//   - the loaded module set (a new client module with imports or cached slots to route);
//   - a client calling the export through a route CE does not intercept, which the create
//     entry breakpoint and the ffxConfigure VEH breakpoint observe directly.
// Without loader notifications the module set is not observable, so the pass stays periodic.

namespace ce::ffx_module_rescan {

enum class Reason : std::uint8_t {
    kNone = 0,
    kNewRuntimeModule,
    kLoaderNotificationsUnavailable,
    kModuleSetChanged,
    kUnroutedCallObserved,
};

struct Inputs {
    const void* runtimeModule = nullptr;
    bool loaderNotificationsLive = false;
    std::uint64_t moduleSetGeneration = 0;
    std::uint64_t unroutedCallEvidence = 0;
};

struct State {
    const void* runtimeModule = nullptr;
    std::uint64_t moduleSetGeneration = 0;
    std::uint64_t unroutedCallEvidence = 0;
    bool valid = false;
};

inline Reason Decide(const State& state, const Inputs& inputs) {
    if (!state.valid || state.runtimeModule != inputs.runtimeModule) {
        return Reason::kNewRuntimeModule;
    }
    if (!inputs.loaderNotificationsLive) {
        return Reason::kLoaderNotificationsUnavailable;
    }
    if (state.moduleSetGeneration != inputs.moduleSetGeneration) {
        return Reason::kModuleSetChanged;
    }
    if (state.unroutedCallEvidence != inputs.unroutedCallEvidence) {
        return Reason::kUnroutedCallObserved;
    }
    return Reason::kNone;
}

// Record the inputs a completed heavy pass covered. Take the inputs BEFORE the pass runs: a module
// load or trapped call during the pass then still differs afterwards and triggers the next one.
inline void Commit(State& state, const Inputs& inputs) {
    state.runtimeModule = inputs.runtimeModule;
    state.moduleSetGeneration = inputs.moduleSetGeneration;
    state.unroutedCallEvidence = inputs.unroutedCallEvidence;
    state.valid = true;
}

inline const char* ReasonName(Reason reason) {
    switch (reason) {
        case Reason::kNone:
            return "none";
        case Reason::kNewRuntimeModule:
            return "new-runtime-module";
        case Reason::kLoaderNotificationsUnavailable:
            return "loader-notifications-unavailable";
        case Reason::kModuleSetChanged:
            return "module-set-changed";
        case Reason::kUnroutedCallObserved:
            return "unrouted-call-observed";
    }
    return "unknown";
}

}  // namespace ce::ffx_module_rescan
