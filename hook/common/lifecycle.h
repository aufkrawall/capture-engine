#pragma once

// Lifecycle State Machines for CaptureEngine
// Provides explicit state management with validated transitions for all subsystems.
// Invalid transitions are logged and rejected, preventing undefined behavior.

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>
#include <mutex>

#include "../../common/validation.h"

namespace ce {

// ============================================================================
// Hook Lifecycle State Machine
// ============================================================================
// Manages the overall lifecycle of the hook DLL from load to unload.

enum class HookState : uint8_t {
    Uninitialized = 0,  // DLL just loaded, no init done
    Connecting,         // Establishing IPC connection
    Connected,          // IPC ready, waiting for graphics API
    Attaching,          // Installing API hooks
    Active,             // Fully operational
    Detaching,          // Removing hooks
    Disconnecting,      // Closing IPC
    Detached,           // Ready for unload
    
    _Count
};

inline const char* HookStateName(HookState s) {
    static const char* names[] = {
        "Uninit", "Connecting", "Connected", "Attaching",
        "Active", "Detaching", "Disconnecting", "Detached"
    };
    auto idx = static_cast<size_t>(s);
    return idx < static_cast<size_t>(HookState::_Count) ? names[idx] : "?";
}

class HookLifecycle {
public:
    using StateChangeCallback = std::function<void(HookState oldState, HookState newState)>;
    
    HookLifecycle() = default;
    
    // Attempt state transition - returns false if invalid
    bool TransitionTo(HookState newState) {
        HookState current = state_.load(std::memory_order_acquire);
        
        if (!IsValidTransition(current, newState)) {
            CE_LOG_ERROR("HookLC", "invalid %s->%s", 
                         HookStateName(current), HookStateName(newState));
            CE_ASSERT(false);
            return false;
        }
        
        CE_LOG_DEBUG("HookLC", "%s->%s", HookStateName(current), HookStateName(newState));
        
        // Notify callbacks under lock
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            for (auto& cb : callbacks_) {
                if (cb) cb(current, newState);
            }
        }
        
        state_.store(newState, std::memory_order_release);
        return true;
    }
    
    HookState GetState() const { 
        return state_.load(std::memory_order_acquire); 
    }
    
    bool IsInState(HookState s) const { return GetState() == s; }
    bool IsActive() const { return IsInState(HookState::Active); }
    bool IsShuttingDown() const {
        auto s = GetState();
        return s == HookState::Detaching || 
               s == HookState::Disconnecting || 
               s == HookState::Detached;
    }
    
    void OnStateChange(StateChangeCallback cb) {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        callbacks_.push_back(std::move(cb));
    }
    
    // Check if transition is valid
    static bool IsValidTransition(HookState from, HookState to) {
        // Transition table: [from][to] = valid
        // Valid transitions form a directed graph
        static constexpr bool table[8][8] = {
            //              Uninit Conn'ing Conn'd Attach Active Detach Disconn Detached
            /* Uninit    */ {false, true,   false, false, false, false, false,  false},
            /* Conn'ing  */ {false, false,  true,  false, false, false, true,   false},
            /* Connected */ {false, false,  false, true,  false, false, true,   false},
            /* Attaching */ {false, false,  false, false, true,  true,  false,  false},
            /* Active    */ {false, false,  false, false, false, true,  false,  false},
            /* Detaching */ {false, false,  false, false, false, false, true,   false},
            /* Disconn   */ {false, false,  false, false, false, false, false,  true },
            /* Detached  */ {false, false,  false, false, false, false, false,  false},
        };
        auto f = static_cast<size_t>(from);
        auto t = static_cast<size_t>(to);
        if (f >= 8 || t >= 8) return false;
        return table[f][t];
    }
    
private:
    std::atomic<HookState> state_{HookState::Uninitialized};
    std::vector<StateChangeCallback> callbacks_;
    std::mutex callbackMutex_;
};

// ============================================================================
// Capture Subsystem State Machine
// ============================================================================
// Manages the capture pipeline for any graphics API.

enum class CaptureState : uint8_t {
    Uninitialized = 0,  // No capture resources
    Initializing,       // Creating textures/buffers
    Ready,              // Ready to capture (not recording)
    Capturing,          // Actively capturing frames
    Paused,             // Capture paused (no frame copy)
    Error,              // Recoverable error state
    CleaningUp,         // Releasing resources
    
    _Count
};

inline const char* CaptureStateName(CaptureState s) {
    static const char* names[] = {
        "Uninit", "Init'ing", "Ready", "Capturing", "Paused", "Error", "Cleanup"
    };
    auto idx = static_cast<size_t>(s);
    return idx < static_cast<size_t>(CaptureState::_Count) ? names[idx] : "?";
}

class CaptureLifecycle {
public:
    using StateChangeCallback = std::function<void(CaptureState oldState, CaptureState newState)>;
    
    bool TransitionTo(CaptureState newState) {
        CaptureState current = state_.load(std::memory_order_acquire);
        
        if (!IsValidTransition(current, newState)) {
            CE_LOG_ERROR("CapLC", "invalid %s->%s",
                         CaptureStateName(current), CaptureStateName(newState));
            CE_ASSERT(false);
            return false;
        }
        
        CE_LOG_DEBUG("CapLC", "%s->%s", CaptureStateName(current), CaptureStateName(newState));
        
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            for (auto& cb : callbacks_) {
                if (cb) cb(current, newState);
            }
        }
        
        state_.store(newState, std::memory_order_release);
        return true;
    }
    
    CaptureState GetState() const { return state_.load(std::memory_order_acquire); }
    bool IsInState(CaptureState s) const { return GetState() == s; }
    bool IsCapturing() const { return IsInState(CaptureState::Capturing); }
    bool CanCapture() const {
        auto s = GetState();
        return s == CaptureState::Ready || s == CaptureState::Capturing;
    }
    
    void OnStateChange(StateChangeCallback cb) {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        callbacks_.push_back(std::move(cb));
    }
    
    static bool IsValidTransition(CaptureState from, CaptureState to) {
        static constexpr bool table[7][7] = {
            //              Uninit Init'ing Ready  Capt   Paused Error  Cleanup
            /* Uninit    */ {false, true,   false, false, false, false, false},
            /* Init'ing  */ {false, false,  true,  false, false, true,  true },
            /* Ready     */ {false, false,  false, true,  false, true,  true },
            /* Capturing */ {false, false,  true,  false, true,  true,  true },
            /* Paused    */ {false, false,  true,  true,  false, true,  true },
            /* Error     */ {false, false,  false, false, false, false, true },
            /* Cleanup   */ {true,  false,  false, false, false, false, false},
        };
        auto f = static_cast<size_t>(from);
        auto t = static_cast<size_t>(to);
        if (f >= 7 || t >= 7) return false;
        return table[f][t];
    }
    
private:
    std::atomic<CaptureState> state_{CaptureState::Uninitialized};
    std::vector<StateChangeCallback> callbacks_;
    std::mutex callbackMutex_;
};

// ============================================================================
// Overlay Subsystem State Machine
// ============================================================================

enum class OverlayState : uint8_t {
    Uninitialized = 0,  // No overlay resources
    Initializing,       // Creating ImGui context, fonts
    Ready,              // Can render but not currently
    Rendering,          // In middle of frame render
    Hidden,             // Initialized but hidden by user
    Resizing,           // Handling swapchain resize
    CleaningUp,         // Releasing resources
    
    _Count
};

inline const char* OverlayStateName(OverlayState s) {
    static const char* names[] = {
        "Uninit", "Init'ing", "Ready", "Rendering", "Hidden", "Resizing", "Cleanup"
    };
    auto idx = static_cast<size_t>(s);
    return idx < static_cast<size_t>(OverlayState::_Count) ? names[idx] : "?";
}

class OverlayLifecycle {
public:
    using StateChangeCallback = std::function<void(OverlayState oldState, OverlayState newState)>;
    
    bool TransitionTo(OverlayState newState) {
        OverlayState current = state_.load(std::memory_order_acquire);
        
        if (!IsValidTransition(current, newState)) {
            CE_LOG_ERROR("OvlLC", "invalid %s->%s",
                         OverlayStateName(current), OverlayStateName(newState));
            CE_ASSERT(false);
            return false;
        }
        
        CE_LOG_DEBUG("OvlLC", "%s->%s", OverlayStateName(current), OverlayStateName(newState));
        
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            for (auto& cb : callbacks_) {
                if (cb) cb(current, newState);
            }
        }
        
        state_.store(newState, std::memory_order_release);
        return true;
    }
    
    OverlayState GetState() const { return state_.load(std::memory_order_acquire); }
    bool IsInState(OverlayState s) const { return GetState() == s; }
    bool CanRender() const {
        auto s = GetState();
        return s == OverlayState::Ready || s == OverlayState::Rendering;
    }
    
    void OnStateChange(StateChangeCallback cb) {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        callbacks_.push_back(std::move(cb));
    }
    
    static bool IsValidTransition(OverlayState from, OverlayState to) {
        static constexpr bool table[7][7] = {
            //              Uninit Init'ing Ready  Render Hidden Resize Cleanup
            /* Uninit    */ {false, true,   false, false, false, false, false},
            /* Init'ing  */ {false, false,  true,  false, false, false, true },
            /* Ready     */ {false, false,  false, true,  true,  true,  true },
            /* Rendering */ {false, false,  true,  false, false, false, true },
            /* Hidden    */ {false, false,  true,  false, false, false, true },
            /* Resizing  */ {false, false,  true,  false, false, false, true },
            /* Cleanup   */ {true,  false,  false, false, false, false, false},
        };
        auto f = static_cast<size_t>(from);
        auto t = static_cast<size_t>(to);
        if (f >= 7 || t >= 7) return false;
        return table[f][t];
    }
    
private:
    std::atomic<OverlayState> state_{OverlayState::Uninitialized};
    std::vector<StateChangeCallback> callbacks_;
    std::mutex callbackMutex_;
};

// ============================================================================
// Encoder Subsystem State Machine
// ============================================================================

enum class EncoderState : uint8_t {
    Uninitialized = 0,  // No encoder resources
    Initializing,       // Creating FFmpeg contexts
    Ready,              // Encoder ready, not recording
    Recording,          // Actively encoding frames
    Stopping,           // Flushing and finalizing
    Error,              // Encoder error (file I/O, etc.)
    CleaningUp,         // Releasing FFmpeg resources
    
    _Count
};

inline const char* EncoderStateName(EncoderState s) {
    static const char* names[] = {
        "Uninit", "Init'ing", "Ready", "Recording", "Stopping", "Error", "Cleanup"
    };
    auto idx = static_cast<size_t>(s);
    return idx < static_cast<size_t>(EncoderState::_Count) ? names[idx] : "?";
}

class EncoderLifecycle {
public:
    using StateChangeCallback = std::function<void(EncoderState oldState, EncoderState newState)>;
    
    bool TransitionTo(EncoderState newState) {
        EncoderState current = state_.load(std::memory_order_acquire);
        
        if (!IsValidTransition(current, newState)) {
            CE_LOG_ERROR("EncLC", "invalid %s->%s",
                         EncoderStateName(current), EncoderStateName(newState));
            CE_ASSERT(false);
            return false;
        }
        
        CE_LOG_DEBUG("EncLC", "%s->%s", EncoderStateName(current), EncoderStateName(newState));
        
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            for (auto& cb : callbacks_) {
                if (cb) cb(current, newState);
            }
        }
        
        state_.store(newState, std::memory_order_release);
        return true;
    }
    
    EncoderState GetState() const { return state_.load(std::memory_order_acquire); }
    bool IsInState(EncoderState s) const { return GetState() == s; }
    bool IsRecording() const { return IsInState(EncoderState::Recording); }
    bool CanEncode() const {
        auto s = GetState();
        return s == EncoderState::Ready || s == EncoderState::Recording;
    }
    
    void OnStateChange(StateChangeCallback cb) {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        callbacks_.push_back(std::move(cb));
    }
    
    static bool IsValidTransition(EncoderState from, EncoderState to) {
        static constexpr bool table[7][7] = {
            //              Uninit Init'ing Ready  Record Stopp  Error  Cleanup
            /* Uninit    */ {false, true,   false, false, false, false, false},
            /* Init'ing  */ {false, false,  true,  false, false, true,  true },
            /* Ready     */ {false, false,  false, true,  false, true,  true },
            /* Recording */ {false, false,  false, false, true,  true,  false},
            /* Stopping  */ {false, false,  true,  false, false, true,  true },
            /* Error     */ {false, false,  false, false, false, false, true },
            /* Cleanup   */ {true,  false,  false, false, false, false, false},
        };
        auto f = static_cast<size_t>(from);
        auto t = static_cast<size_t>(to);
        if (f >= 7 || t >= 7) return false;
        return table[f][t];
    }
    
private:
    std::atomic<EncoderState> state_{EncoderState::Uninitialized};
    std::vector<StateChangeCallback> callbacks_;
    std::mutex callbackMutex_;
};

} // namespace ce
