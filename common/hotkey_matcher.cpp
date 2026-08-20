#include "hotkey_matcher.h"

bool IsHotkeyModifierKey(int vkey) {
    switch (vkey) {
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT:
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL:
        case VK_MENU:
        case VK_LMENU:
        case VK_RMENU:
        case VK_LWIN:
        case VK_RWIN:
            return true;
        default:
            return false;
    }
}

namespace {

void AppendBinding(HotkeyBinding* bindings, size_t& count, size_t capacity, int id,
                   const AppConfig::HotkeyConfig& hotkey, bool owned) {
    if (!owned || hotkey.vkey == 0 || count >= capacity)
        return;
    HotkeyBinding& binding = bindings[count++];
    binding.id = id;
    binding.vkey = hotkey.vkey;
    binding.ctrl = hotkey.ctrl;
    binding.shift = hotkey.shift;
    binding.alt = hotkey.alt;
    binding.win = hotkey.win;
}

bool ModifiersMatch(const HotkeyBinding& binding, const HotkeyModifierState& modifiers) {
    // RegisterHotKey matches the modifier set exactly: Ctrl+Shift+9 does not
    // trigger a hotkey registered as Ctrl+9. Reproducing that here keeps the
    // two delivery paths interchangeable.
    return binding.ctrl == modifiers.ctrl && binding.shift == modifiers.shift && binding.alt == modifiers.alt &&
           binding.win == modifiers.win;
}

}  // namespace

size_t BuildHotkeyBindings(const AppConfig& config, const HotkeyOwnership& ownership, HotkeyBinding* bindings,
                           size_t capacity) {
    if (!bindings || capacity == 0)
        return 0;
    size_t count = 0;
    AppendBinding(bindings, count, capacity, HOTKEY_ID_RECORD, config.hotkeyStartStop, ownership.record);
    AppendBinding(bindings, count, capacity, HOTKEY_ID_SCREENSHOT, config.hotkeyScreenshot, ownership.screenshot);
    AppendBinding(bindings, count, capacity, HOTKEY_ID_AUDIO_ONLY, config.hotkeyAudioOnly, ownership.audioOnly);
    AppendBinding(bindings, count, capacity, HOTKEY_ID_TOGGLE_OVERLAY, config.hotkeyToggleOverlay,
                  ownership.toggleOverlay);
    return count;
}

HotkeyMatchResult HotkeyMatcher::Observe(const HotkeyBinding* bindings, size_t count, int vkey,
                                         HotkeyKeyAction action, const HotkeyModifierState& modifiers) {
    HotkeyMatchResult result;
    if (!IsTracked(vkey) || IsHotkeyModifierKey(vkey))
        return result;

    const size_t index = static_cast<size_t>(vkey);

    if (action == HotkeyKeyAction::KeyUp) {
        down_[index] = false;
        // The press was consumed, so the release belongs to the same consumed
        // keystroke. Letting it through would hand the application a key-up it
        // never saw a key-down for.
        if (swallowedDown_[index]) {
            swallowedDown_[index] = false;
            result.swallow = true;
        }
        return result;
    }

    if (down_[index]) {
        // Auto-repeat. MOD_NOREPEAT means no second activation, but a repeat of
        // a consumed press stays consumed.
        result.swallow = swallowedDown_[index];
        return result;
    }

    down_[index] = true;
    if (!bindings)
        return result;
    for (size_t i = 0; i < count && i < kMaxHotkeyBindings; ++i) {
        const HotkeyBinding& binding = bindings[i];
        if (binding.id == 0 || binding.vkey == 0 || binding.vkey != vkey)
            continue;
        if (!ModifiersMatch(binding, modifiers))
            continue;
        result.id = binding.id;
        result.swallow = true;
        swallowedDown_[index] = true;
        return result;
    }
    return result;
}

void HotkeyMatcher::ClearSwallow(int vkey) {
    if (!IsTracked(vkey))
        return;
    swallowedDown_[static_cast<size_t>(vkey)] = false;
}

void HotkeyMatcher::Reset() {
    for (size_t i = 0; i < kVirtualKeyCount; ++i) {
        down_[i] = false;
        swallowedDown_[i] = false;
    }
}
