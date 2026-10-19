#include "global_hotkeys.h"

#include <iterator>

#include "common/logging/logging.h"

namespace {

// One row per hotkey, in the order the controller always registered them.
struct HotkeySlot {
    int id;
    const char* name;
    AppConfig::HotkeyConfig AppConfig::*binding;
    bool HotkeyOwnership::*owned;
    HotkeyAction action;
};

constexpr HotkeySlot kSlots[] = {
    {HOTKEY_ID_RECORD, "recording", &AppConfig::hotkeyStartStop, &HotkeyOwnership::record, HotkeyAction::Record},
    {HOTKEY_ID_SCREENSHOT, "screenshot", &AppConfig::hotkeyScreenshot, &HotkeyOwnership::screenshot,
     HotkeyAction::Screenshot},
    {HOTKEY_ID_AUDIO_ONLY, "audio-only", &AppConfig::hotkeyAudioOnly, &HotkeyOwnership::audioOnly,
     HotkeyAction::AudioOnly},
    {HOTKEY_ID_TOGGLE_OVERLAY, "overlay toggle", &AppConfig::hotkeyToggleOverlay, &HotkeyOwnership::toggleOverlay,
     HotkeyAction::Overlay},
    {HOTKEY_ID_BENCHMARK, "benchmark", &AppConfig::hotkeyBenchmark, &HotkeyOwnership::benchmark,
     HotkeyAction::Benchmark},
};

bool SameBinding(const AppConfig::HotkeyConfig& a, const AppConfig::HotkeyConfig& b) {
    return a.vkey == b.vkey && a.ctrl == b.ctrl && a.shift == b.shift && a.alt == b.alt && a.win == b.win;
}

int OwnedCount(const HotkeyOwnership& ownership) {
    int owned = 0;
    for (const HotkeySlot& slot : kSlots)
        owned += ownership.*slot.owned ? 1 : 0;
    return owned;
}

}  // namespace

bool GlobalHotkeys::OnEngineThread(const char* what) const {
    if (GetCurrentThreadId() == thread_)
        return true;
    LogError("[Hotkey] %s ignored: called on thread %lu, but the hotkeys belong to thread %lu", what,
             GetCurrentThreadId(), thread_);
    return false;
}

bool GlobalHotkeys::Start(const AppConfig& config, HotkeyDeliveryTarget engineThread) {
    if (started_) {
        LogWarn("[Hotkey] Start ignored: the global hotkeys are already running");
        return system_.HookActive();
    }
    // RegisterHotKey(NULL) posts WM_HOTKEY to the registering thread; the hook has to post to the same
    // one or the two delivery paths would reach different message loops.
    if (!engineThread.IsValid() || engineThread.Thread() != GetCurrentThreadId()) {
        LogError("[Hotkey] Start refused: the delivery target must name the calling thread and a WM_APP message");
        return false;
    }

    for (const HotkeySlot& slot : kSlots)
        ownership_.*slot.owned = system_.RegisterHotkey(slot.id, config.*slot.binding, slot.name);

    // The same set is recognized on a low-level keyboard hook, because a foreground application that
    // registers its raw-input keyboard with RIDEV_NOHOTKEYS stops WM_HOTKEY for everyone.
    system_.PublishBindings(config, ownership_);
    const bool hook = system_.StartHook(engineThread);

    started_ = true;
    thread_ = GetCurrentThreadId();
    hookMessage_ = engineThread.Message();
    LogInfo("[Hotkey] Global hotkeys started (owned=%d/%d record=%d screenshot=%d audio-only=%d overlay=%d "
            "benchmark=%d hook=%d)",
            OwnedCount(ownership_), static_cast<int>(std::size(kSlots)), ownership_.record ? 1 : 0,
            ownership_.screenshot ? 1 : 0, ownership_.audioOnly ? 1 : 0, ownership_.toggleOverlay ? 1 : 0,
            ownership_.benchmark ? 1 : 0, hook ? 1 : 0);
    return hook;
}

void GlobalHotkeys::Apply(const AppConfig& previous, const AppConfig& current) {
    if (!started_ || !OnEngineThread("Hotkey reload"))
        return;
    int reregistered = 0;
    for (const HotkeySlot& slot : kSlots) {
        if (SameBinding(previous.*slot.binding, current.*slot.binding))
            continue;
        // A registration that lost to another application is released like any other, so a rebound
        // combination gets a fresh chance to be owned.
        system_.UnregisterHotkey(slot.id);
        ownership_.*slot.owned = system_.RegisterHotkey(slot.id, current.*slot.binding, slot.name);
        ++reregistered;
    }
    // The keyboard-hook path recognizes the same hotkeys, so it follows every reload, including one
    // that only disabled a hotkey.
    system_.PublishBindings(current, ownership_);
    if (reregistered != 0) {
        LogInfo("[Hotkey] Config reload re-registered %d hotkey(s) (owned=%d/%d)", reregistered,
                OwnedCount(ownership_), static_cast<int>(std::size(kSlots)));
    }
}

void GlobalHotkeys::Stop() {
    if (!started_)
        return;
    // Unhook first so no press is consumed after its registration is gone.
    system_.StopHook();
    if (GetCurrentThreadId() == thread_) {
        for (const HotkeySlot& slot : kSlots)
            system_.UnregisterHotkey(slot.id);
    } else {
        LogError("[Hotkey] Stop on thread %lu cannot unregister the hotkeys of thread %lu; the system releases "
                 "them when that thread ends",
                 GetCurrentThreadId(), thread_);
    }
    ownership_ = {};
    started_ = false;
    hookMessage_ = 0;
    LogInfo("[Hotkey] Global hotkeys stopped");
}

void GlobalHotkeys::ReportDiagnostics() {
    system_.ReportHookDiagnostics();
}

std::optional<HotkeyAction> GlobalHotkeys::Dispatch(const MSG& message) const {
    if (!started_ || message.hwnd != nullptr)
        return std::nullopt;
    const bool fromHook = hookMessage_ != 0 && message.message == hookMessage_;
    if (message.message != WM_HOTKEY && !fromHook)
        return std::nullopt;
    if (fromHook) {
        LogDebug("[Hotkey] Keyboard-hook delivery id=%d vk=0x%02X total=%llu", static_cast<int>(message.wParam),
                 static_cast<unsigned>(message.lParam), static_cast<unsigned long long>(system_.HookDeliveredCount()));
    }
    for (const HotkeySlot& slot : kSlots) {
        if (static_cast<int>(message.wParam) == slot.id)
            return slot.action;
    }
    return std::nullopt;
}
