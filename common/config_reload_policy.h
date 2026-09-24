#pragma once

// When the controller reloads config.ini after it changed on disk.
//
// Editors save in steps (truncate then write, or write in chunks). A reload
// that lands inside that window reads a partial file: every missing key falls
// back to its default, and SendCommandToAll publishes that default configuration
// to the injected games and the media process for a second (profiles and
// injection targets vanish, overrides are removed and re-applied, hotkeys are
// re-registered). A change is now applied only once the file's identity (write
// time and size) has been seen unchanged on two consecutive checks, and an empty
// or missing file is never applied.

#include <cstdint>

namespace ce::config_reload {

struct FileIdentity {
    bool exists = false;
    uint64_t lastWriteTime = 0;
    uint64_t size = 0;

    bool operator==(const FileIdentity& other) const {
        return exists == other.exists && lastWriteTime == other.lastWriteTime && size == other.size;
    }
    bool operator!=(const FileIdentity& other) const {
        return !(*this == other);
    }
};

struct State {
    bool initialized = false;
    FileIdentity applied;
    bool pendingValid = false;
    FileIdentity pending;
};

enum class Decision : uint8_t {
    kNone,    // nothing changed
    kWait,    // a change is being written, or the file is empty/missing
    kReload,  // a changed file has been stable across two checks
};

// Checks run every kIdleCheckIntervalMs, and every kPendingCheckIntervalMs while
// a change is waiting to settle, so a save is still applied within ~1 s.
constexpr uint32_t kIdleCheckIntervalMs = 1000;
constexpr uint32_t kPendingCheckIntervalMs = 250;

inline uint32_t CheckIntervalMs(const State& state) {
    return state.pendingValid ? kPendingCheckIntervalMs : kIdleCheckIntervalMs;
}

inline Decision Observe(State& state, const FileIdentity& now) {
    if (!state.initialized) {
        state.initialized = true;
        state.applied = now;
        state.pendingValid = false;
        return Decision::kNone;
    }
    if (now == state.applied) {
        state.pendingValid = false;
        return Decision::kNone;
    }
    if (!now.exists || now.size == 0) {
        state.pendingValid = false;
        return Decision::kWait;
    }
    if (state.pendingValid && state.pending == now) {
        state.applied = now;
        state.pendingValid = false;
        return Decision::kReload;
    }
    state.pending = now;
    state.pendingValid = true;
    return Decision::kWait;
}

}  // namespace ce::config_reload
