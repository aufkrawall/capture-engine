#pragma once

#include <windows.h>

#include <cstddef>

#include "config.h"

// Global hotkey recognition for the low-level keyboard path.
//
// RegisterHotKey is not a dependable delivery mechanism: an application that
// registers its raw-input keyboard with RIDEV_NOHOTKEYS suppresses every
// application-defined hotkey while its window is in the foreground, so the
// controller's WM_HOTKEY simply never arrives (DOOM Eternal registers usage
// page 1 / usage 6 with dwFlags=0x200). The controller therefore also observes
// keys through a low-level keyboard hook, which sits ahead of that suppression.
//
// This matcher is the pure decision half of that path: it owns no Windows hook
// state, only the key bookkeeping needed to reproduce RegisterHotKey semantics
// exactly - exact modifier match, MOD_NOREPEAT (no auto-repeat refire), and the
// keystroke being consumed rather than delivered to the foreground application.
// Hotkey identifiers. The RegisterHotKey registration, the low-level keyboard
// path and the controller's dispatch all name a hotkey by these, so a press
// means the same thing whichever path delivered it.
inline constexpr int HOTKEY_ID_RECORD = 1;
inline constexpr int HOTKEY_ID_SCREENSHOT = 2;
inline constexpr int HOTKEY_ID_AUDIO_ONLY = 3;
inline constexpr int HOTKEY_ID_TOGGLE_OVERLAY = 4;

struct HotkeyBinding {
    int id = 0;    // HOTKEY_ID_* the controller acts on; 0 means unused
    int vkey = 0;  // 0 disables the binding
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    bool win = false;
};

// Modifiers held while a key event was observed.
struct HotkeyModifierState {
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    bool win = false;
};

enum class HotkeyKeyAction { KeyDown, KeyUp };

struct HotkeyMatchResult {
    int id = 0;            // non-zero when a binding fired
    bool swallow = false;  // the key must not reach the foreground application
};

// Modifier virtual keys, including the side-specific ones. A modifier is never
// a hotkey's own key, and consuming one would break every other application.
bool IsHotkeyModifierKey(int vkey);

// Maximum bindings the controller publishes (record, screenshot, audio-only,
// overlay toggle). Kept as a compile-time bound so the hook path needs no
// allocation while it holds a lock.
constexpr size_t kMaxHotkeyBindings = 8;

// Which hotkeys the controller actually owns. RegisterHotKey is exclusive per
// combination, so a registration that lost to another application has to stay
// lost: the keyboard path recognizes only hotkeys the controller owns and never
// takes a combination away from whoever registered it first.
struct HotkeyOwnership {
    bool record = false;
    bool screenshot = false;
    bool audioOnly = false;
    bool toggleOverlay = false;
};

// Fills the binding table from the configured hotkeys. A hotkey without a key,
// or one the controller does not own, is dropped rather than stored disabled,
// so the table only ever holds bindings that may match. Returns how many
// entries were written.
size_t BuildHotkeyBindings(const AppConfig& config, const HotkeyOwnership& ownership, HotkeyBinding* bindings,
                           size_t capacity);

// Key bookkeeping for one observer. Not thread-safe by design: exactly one
// thread - the hook thread - drives it, while the binding table it is handed is
// owned and synchronized by the caller.
class HotkeyMatcher {
public:
    // Decides what a single observed key event means. A binding only fires on
    // the transition into "down" (MOD_NOREPEAT parity), and a key whose press
    // was consumed also has its release consumed so the application never sees
    // an unpaired key-up.
    HotkeyMatchResult Observe(const HotkeyBinding* bindings, size_t count, int vkey, HotkeyKeyAction action,
                              const HotkeyModifierState& modifiers);

    // Withdraws a consumption the caller could not honour. Delivery of a match
    // can still fail after Observe decided to consume the key; the key then has
    // to reach the application after all, release included.
    void ClearSwallow(int vkey);

    // Drops all key bookkeeping. Used whenever the observer (re)starts, because
    // key state observed before that point is no longer trustworthy.
    void Reset();

private:
    static constexpr size_t kVirtualKeyCount = 256;

    bool IsTracked(int vkey) const {
        return vkey >= 0 && static_cast<size_t>(vkey) < kVirtualKeyCount;
    }

    bool down_[kVirtualKeyCount] = {};
    bool swallowedDown_[kVirtualKeyCount] = {};
};
