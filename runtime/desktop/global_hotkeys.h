#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>

#include "common/config/config.h"
#include "common/overlay/hotkey_delivery_target.h"
#include "common/overlay/hotkey_matcher.h"

// What a hotkey press asks the host to do. The component only decides which action a press means;
// recording, screenshot and overlay code stays with the caller.
enum class HotkeyAction { Record, AudioOnly, Screenshot, Overlay, Benchmark };

// The operating-system side of the hotkeys, behind one seam so tests never register a real hotkey or
// install a real keyboard hook. SystemHotkeys() is the production implementation.
class HotkeySystem {
public:
    virtual ~HotkeySystem() = default;

    // RegisterHotKey(NULL, id, ...) for the calling thread. True when the combination is now owned by
    // this process; false for an unset key or a combination another application registered first.
    virtual bool RegisterHotkey(int id, const AppConfig::HotkeyConfig& hotkey, const char* name) = 0;
    virtual void UnregisterHotkey(int id) = 0;

    // The low-level keyboard path for applications that suppress WM_HOTKEY (RIDEV_NOHOTKEYS).
    // StartHook is false when the hook cannot be installed; RegisterHotKey then stays the only path.
    virtual void PublishBindings(const AppConfig& config, const HotkeyOwnership& ownership) = 0;
    virtual bool StartHook(HotkeyDeliveryTarget target) = 0;
    virtual void StopHook() = 0;
    virtual bool HookActive() const = 0;
    virtual uint64_t HookDeliveredCount() const = 0;
    virtual void ReportHookDiagnostics() = 0;
};

// The real RegisterHotKey / low-level keyboard hook implementation (global_hotkeys_system.cpp).
HotkeySystem& SystemHotkeys();

// The five global hotkeys: their RegisterHotKey registrations, the ownership record, and the keyboard
// hook that backs them. Port of the registration block in CompleteControllerStartup, the reload block
// of the controller loop, DispatchHotkey and the shutdown unregistration.
//
// Thread affinity: RegisterHotKey(NULL) registrations belong to the thread that made them, and
// WM_HOTKEY plus the hook's delivery message are posted to that thread. Start, Apply and Stop must
// therefore run on the engine thread that pumps its message queue, and Dispatch gets the thread
// messages (hwnd == nullptr) that thread retrieves. Apply from another thread does nothing and logs
// an error; Stop from another thread still stops the hook but cannot unregister (the system drops
// the registrations when the engine thread ends). The component never pumps messages itself.
class GlobalHotkeys {
public:
    explicit GlobalHotkeys(HotkeySystem& system = SystemHotkeys()) : system_(system) {}
    ~GlobalHotkeys() {
        Stop();
    }
    GlobalHotkeys(const GlobalHotkeys&) = delete;
    GlobalHotkeys& operator=(const GlobalHotkeys&) = delete;

    // Registers every configured hotkey, records which ones this process owns, publishes them to the
    // keyboard hook and starts it. `engineThread` names this thread and the private message the hook
    // posts for a press (wParam = hotkey id, lParam = virtual key). Returns whether the keyboard hook
    // is active; a failed hook leaves RegisterHotKey as the only delivery path, as it always did.
    // A second Start without Stop is refused (returns the current hook state).
    bool Start(const AppConfig& config, HotkeyDeliveryTarget engineThread);

    // Config reload: re-registers exactly the hotkeys whose binding changed and always republishes to
    // the keyboard hook, so even a reload that only disabled a hotkey takes effect on both paths.
    // Does nothing before Start / after Stop.
    void Apply(const AppConfig& previous, const AppConfig& current);

    // Stops the keyboard hook first, then unregisters all five hotkeys. Idempotent.
    void Stop();

    // Logs what the keyboard hook counted since the previous call (cheap when nothing changed).
    void ReportDiagnostics();

    // Maps a retrieved thread message to the action it requests: WM_HOTKEY (RegisterHotKey path) and
    // the hook's delivery message. Anything else, an unknown id, or a message while stopped yields
    // nothing. A hook delivery also writes the "[Hotkey] Keyboard-hook delivery" debug line.
    std::optional<HotkeyAction> Dispatch(const MSG& message) const;

    const HotkeyOwnership& Ownership() const {
        return ownership_;
    }
    bool Started() const {
        return started_;
    }
    bool HookActive() const {
        return system_.HookActive();
    }

private:
    bool OnEngineThread(const char* what) const;

    HotkeySystem& system_;
    HotkeyOwnership ownership_;
    bool started_ = false;
    DWORD thread_ = 0;
    UINT hookMessage_ = 0;
};
