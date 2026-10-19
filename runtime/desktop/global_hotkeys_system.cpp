#include "global_hotkeys.h"
#include "hotkey_input_hook.h"

namespace {

// RegisterHotKey(NULL, ...) plus the process-wide low-level keyboard hook.
class Win32HotkeySystem final : public HotkeySystem {
public:
    bool RegisterHotkey(int id, const AppConfig::HotkeyConfig& hotkey, const char* name) override {
        return RegisterConfiguredHotkey(id, hotkey, name);
    }
    void UnregisterHotkey(int id) override {
        UnregisterHotKey(nullptr, id);
    }
    void PublishBindings(const AppConfig& config, const HotkeyOwnership& ownership) override {
        PublishHotkeyBindings(config, ownership);
    }
    bool StartHook(HotkeyDeliveryTarget target) override {
        return StartHotkeyInputHook(target);
    }
    void StopHook() override {
        StopHotkeyInputHook();
    }
    bool HookActive() const override {
        return IsHotkeyInputHookActive();
    }
    uint64_t HookDeliveredCount() const override {
        return GetHotkeyInputHookDeliveredCount();
    }
    void ReportHookDiagnostics() override {
        ReportHotkeyInputHookDiagnostics();
    }
};

}  // namespace

HotkeySystem& SystemHotkeys() {
    static Win32HotkeySystem system;
    return system;
}
