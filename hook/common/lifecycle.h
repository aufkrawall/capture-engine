#pragma once

// Lifecycle State Machines for CaptureEngine
// Provides explicit state management with validated transitions for all
// subsystems. Invalid transitions are logged and rejected, preventing undefined
// behavior.

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "../../common/validation.h"

namespace ce {

// ============================================================================
// Generic Lifecycle Template
// ============================================================================

template <typename StateEnum, size_t N, const char* LogTag, const char* (*NameFn)(StateEnum)>
class Lifecycle {
public:
    using TransitionTable = bool[N][N];
    using StateChangeCallback = std::function<void(StateEnum oldState, StateEnum newState)>;

    Lifecycle() = default;

    bool TransitionTo(StateEnum newState) {
        StateEnum current = state_.load(std::memory_order_acquire);

        if (!IsValidTransition(current, newState)) {
            CE_LOG_ERROR(LogTag, "invalid %s->%s", NameFn(current), NameFn(newState));
            CE_ASSERT(false);
            return false;
        }

        // CAS loop to prevent TOCTOU: validate+store must be atomic
        while (!state_.compare_exchange_weak(current, newState, std::memory_order_acq_rel, std::memory_order_acquire)) {
            if (!IsValidTransition(current, newState)) {
                CE_LOG_ERROR(LogTag, "raced invalid %s->%s", NameFn(current), NameFn(newState));
                CE_ASSERT(false);
                return false;
            }
        }

        CE_LOG_DEBUG(LogTag, "%s->%s", NameFn(current), NameFn(newState));

        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            for (auto& cb : callbacks_) {
                if (cb) {
                    try {
                        cb(current, newState);
                    } catch (...) {
                        CE_LOG_ERROR(LogTag, "callback threw during %s->%s", NameFn(current), NameFn(newState));
                    }
                }
            }
        }

        return true;
    }

    StateEnum GetState() const {
        return state_.load(std::memory_order_acquire);
    }
    bool IsInState(StateEnum s) const {
        return GetState() == s;
    }

    void OnStateChange(StateChangeCallback cb) {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        callbacks_.push_back(std::move(cb));
    }

    static bool IsValidTransition(StateEnum from, StateEnum to) {
        auto f = static_cast<size_t>(from);
        auto t = static_cast<size_t>(to);
        if (f >= N || t >= N)
            return false;
        return GetTransitionTable()[f][t];
    }

protected:
    // Subclasses must define this via explicit specialization or static member
    static const TransitionTable& GetTransitionTable();

private:
    std::atomic<StateEnum> state_{static_cast<StateEnum>(0)};
    std::vector<StateChangeCallback> callbacks_;
    std::mutex callbackMutex_;
};

// ============================================================================
// Hook Lifecycle
// ============================================================================

enum class HookState : uint8_t {
    Uninitialized = 0,
    Connecting,
    Connected,
    Attaching,
    Active,
    Detaching,
    Disconnecting,
    Detached,

    _Count
};

inline const char* HookStateName(HookState s) {
    static const char* names[] = {"Uninit", "Connecting", "Connected",     "Attaching",
                                  "Active", "Detaching",  "Disconnecting", "Detached"};
    auto idx = static_cast<size_t>(s);
    return idx < static_cast<size_t>(HookState::_Count) ? names[idx] : "?";
}

inline constexpr char kHookLCTag[] = "HookLC";

class HookLifecycle : public Lifecycle<HookState, 8, kHookLCTag, HookStateName> {
public:
    bool IsActive() const {
        return IsInState(HookState::Active);
    }
    bool IsShuttingDown() const {
        auto s = GetState();
        return s == HookState::Detaching || s == HookState::Disconnecting || s == HookState::Detached;
    }
};

template <>
inline const Lifecycle<HookState, 8, kHookLCTag, HookStateName>::TransitionTable&
Lifecycle<HookState, 8, kHookLCTag, HookStateName>::GetTransitionTable() {
    static constexpr bool table[8][8] = {
        //              Uninit Conn'ing Conn'd Attach Active Detach Disconn
        //              Detached
        /* Uninit    */ {false, true, false, false, false, false, false, false},
        /* Conn'ing  */ {false, false, true, false, false, false, true, false},
        /* Connected */ {false, false, false, true, false, false, true, false},
        /* Attaching */ {false, false, false, false, true, true, false, false},
        /* Active    */ {false, false, false, false, false, true, false, false},
        /* Detaching */ {false, false, false, false, false, false, true, false},
        /* Disconn   */ {false, false, false, false, false, false, false, true},
        /* Detached  */ {false, false, false, false, false, false, false, false},
    };
    return table;
}

// ============================================================================
// Capture Lifecycle
// ============================================================================

enum class CaptureState : uint8_t {
    Uninitialized = 0,
    Initializing,
    Ready,
    Capturing,
    Paused,
    Error,
    CleaningUp,

    _Count
};

inline const char* CaptureStateName(CaptureState s) {
    static const char* names[] = {"Uninit", "Init'ing", "Ready", "Capturing", "Paused", "Error", "Cleanup"};
    auto idx = static_cast<size_t>(s);
    return idx < static_cast<size_t>(CaptureState::_Count) ? names[idx] : "?";
}

inline constexpr char kCapLCTag[] = "CapLC";

class CaptureLifecycle : public Lifecycle<CaptureState, 7, kCapLCTag, CaptureStateName> {
public:
    bool IsCapturing() const {
        return IsInState(CaptureState::Capturing);
    }
    bool CanCapture() const {
        auto s = GetState();
        return s == CaptureState::Ready || s == CaptureState::Capturing;
    }
};

template <>
inline const Lifecycle<CaptureState, 7, kCapLCTag, CaptureStateName>::TransitionTable&
Lifecycle<CaptureState, 7, kCapLCTag, CaptureStateName>::GetTransitionTable() {
    static constexpr bool table[7][7] = {
        //              Uninit Init'ing Ready  Capt   Paused Error  Cleanup
        /* Uninit    */ {false, true, false, false, false, false, false},
        /* Init'ing  */ {false, false, true, false, false, true, true},
        /* Ready     */ {false, false, false, true, false, true, true},
        /* Capturing */ {false, false, true, false, true, true, true},
        /* Paused    */ {false, false, true, true, false, true, true},
        /* Error     */ {false, false, false, false, false, false, true},
        /* Cleanup   */ {true, false, false, false, false, false, false},
    };
    return table;
}

// ============================================================================
// Overlay Lifecycle
// ============================================================================

enum class OverlayState : uint8_t {
    Uninitialized = 0,
    Initializing,
    Ready,
    Rendering,
    Hidden,
    Resizing,
    CleaningUp,

    _Count
};

inline const char* OverlayStateName(OverlayState s) {
    static const char* names[] = {"Uninit", "Init'ing", "Ready", "Rendering", "Hidden", "Resizing", "Cleanup"};
    auto idx = static_cast<size_t>(s);
    return idx < static_cast<size_t>(OverlayState::_Count) ? names[idx] : "?";
}

inline constexpr char kOvlLCTag[] = "OvlLC";

class OverlayLifecycle : public Lifecycle<OverlayState, 7, kOvlLCTag, OverlayStateName> {
public:
    bool CanRender() const {
        auto s = GetState();
        return s == OverlayState::Ready || s == OverlayState::Rendering;
    }
};

template <>
inline const Lifecycle<OverlayState, 7, kOvlLCTag, OverlayStateName>::TransitionTable&
Lifecycle<OverlayState, 7, kOvlLCTag, OverlayStateName>::GetTransitionTable() {
    static constexpr bool table[7][7] = {
        //              Uninit Init'ing Ready  Render Hidden Resize Cleanup
        /* Uninit    */ {false, true, false, false, false, false, false},
        /* Init'ing  */ {false, false, true, false, false, false, true},
        /* Ready     */ {false, false, false, true, true, true, true},
        /* Rendering */ {false, false, true, false, false, false, true},
        /* Hidden    */ {false, false, true, false, false, false, true},
        /* Resizing  */ {false, false, true, false, false, false, true},
        /* Cleanup   */ {true, false, false, false, false, false, false},
    };
    return table;
}

// ============================================================================
// Encoder Lifecycle
// ============================================================================

enum class EncoderState : uint8_t {
    Uninitialized = 0,
    Initializing,
    Ready,
    Recording,
    Stopping,
    Error,
    CleaningUp,

    _Count
};

inline const char* EncoderStateName(EncoderState s) {
    static const char* names[] = {"Uninit", "Init'ing", "Ready", "Recording", "Stopping", "Error", "Cleanup"};
    auto idx = static_cast<size_t>(s);
    return idx < static_cast<size_t>(EncoderState::_Count) ? names[idx] : "?";
}

inline constexpr char kEncLCTag[] = "EncLC";

class EncoderLifecycle : public Lifecycle<EncoderState, 7, kEncLCTag, EncoderStateName> {
public:
    bool IsRecording() const {
        return IsInState(EncoderState::Recording);
    }
    bool CanEncode() const {
        auto s = GetState();
        return s == EncoderState::Ready || s == EncoderState::Recording;
    }
};

template <>
inline const Lifecycle<EncoderState, 7, kEncLCTag, EncoderStateName>::TransitionTable&
Lifecycle<EncoderState, 7, kEncLCTag, EncoderStateName>::GetTransitionTable() {
    static constexpr bool table[7][7] = {
        //              Uninit Init'ing Ready  Record Stopp  Error  Cleanup
        /* Uninit    */ {false, true, false, false, false, false, false},
        /* Init'ing  */ {false, false, true, false, false, true, true},
        /* Ready     */ {false, false, false, true, false, true, true},
        /* Recording */ {false, false, false, false, true, true, false},
        /* Stopping  */ {false, false, true, false, false, true, true},
        /* Error     */ {false, false, false, false, false, false, true},
        /* Cleanup   */ {true, false, false, false, false, false, false},
    };
    return table;
}

}  // namespace ce
