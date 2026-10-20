#include <gtest/gtest.h>

#include <set>
#include <string>
#include <thread>
#include <vector>

#include "runtime/desktop/global_hotkeys.h"

namespace {

constexpr UINT kHookMessage = WM_APP + 0x240;

// Records every call in order and answers registrations the way the real system does: an unset key
// or a combination in `taken` belongs to someone else.
class FakeHotkeySystem final : public HotkeySystem {
public:
    std::vector<std::string> events;
    std::set<int> taken;                      // ids another application already owns
    bool hookStarts = true;
    bool hookActive = false;
    std::vector<HotkeyBinding> lastBindings;  // what the keyboard hook would serve
    int deliveredCount = 7;

    bool RegisterHotkey(int id, const AppConfig::HotkeyConfig& hotkey, const char* name) override {
        const bool owned = hotkey.vkey != 0 && taken.count(id) == 0;
        events.push_back("register " + std::to_string(id) + " vk=" + std::to_string(hotkey.vkey) + " " + name +
                         (owned ? " owned" : " not-owned"));
        return owned;
    }
    void UnregisterHotkey(int id) override {
        events.push_back("unregister " + std::to_string(id));
    }
    void PublishBindings(const AppConfig& config, const HotkeyOwnership& ownership) override {
        HotkeyBinding bindings[kMaxHotkeyBindings] = {};
        const size_t count = BuildHotkeyBindings(config, ownership, bindings, kMaxHotkeyBindings);
        lastBindings.assign(bindings, bindings + count);
        std::string ids;
        for (const HotkeyBinding& binding : lastBindings)
            ids += " " + std::to_string(binding.id);
        events.push_back("publish" + ids);
    }
    bool StartHook(HotkeyDeliveryTarget target) override {
        events.push_back("hook-start msg=" + std::to_string(target.Message()));
        hookActive = hookStarts;
        return hookStarts;
    }
    void StopHook() override {
        events.push_back("hook-stop");
        hookActive = false;
    }
    bool HookActive() const override {
        return hookActive;
    }
    uint64_t HookDeliveredCount() const override {
        return static_cast<uint64_t>(deliveredCount);
    }
    void ReportHookDiagnostics() override {
        events.push_back("report");
    }

    std::vector<std::string> Take() {
        std::vector<std::string> taken_events;
        taken_events.swap(events);
        return taken_events;
    }
};

AppConfig Config() {
    AppConfig config;
    config.hotkeyStartStop = {VK_F9, false, false, false, false};
    config.hotkeyScreenshot = {VK_F10, false, false, false, false};
    config.hotkeyAudioOnly = {VK_F11, false, false, false, false};
    config.hotkeyToggleOverlay = {VK_F12, false, false, false, false};
    config.hotkeyBenchmark = {VK_F8, true, false, false, false};
    return config;
}

HotkeyDeliveryTarget Target() {
    return {GetCurrentThreadId(), kHookMessage};
}

MSG Message(UINT message, int id, int vkey = 0, HWND window = nullptr) {
    MSG msg{};
    msg.hwnd = window;
    msg.message = message;
    msg.wParam = static_cast<WPARAM>(id);
    msg.lParam = static_cast<LPARAM>(vkey);
    return msg;
}

using Events = std::vector<std::string>;

TEST(GlobalHotkeysTest, StartRegistersAllFiveThenPublishesThenStartsTheHook) {
    FakeHotkeySystem system;
    GlobalHotkeys hotkeys(system);
    EXPECT_TRUE(hotkeys.Start(Config(), Target()));
    EXPECT_EQ(system.Take(), (Events{
                                 "register 1 vk=120 recording owned",
                                 "register 2 vk=121 screenshot owned",
                                 "register 3 vk=122 audio-only owned",
                                 "register 4 vk=123 overlay toggle owned",
                                 "register 5 vk=119 benchmark owned",
                                 "publish 1 2 3 4 5",
                                 "hook-start msg=" + std::to_string(kHookMessage),
                             }));
    const HotkeyOwnership& owned = hotkeys.Ownership();
    EXPECT_TRUE(owned.record && owned.screenshot && owned.audioOnly && owned.toggleOverlay && owned.benchmark);
    EXPECT_TRUE(hotkeys.Started());
    EXPECT_TRUE(hotkeys.HookActive());
}

TEST(GlobalHotkeysTest, ACombinationAnotherApplicationOwnsIsNeverServedByTheHook) {
    FakeHotkeySystem system;
    system.taken = {HOTKEY_ID_SCREENSHOT};
    AppConfig config = Config();
    config.hotkeyAudioOnly = {0, false, false, false, false};  // unset: nothing to own
    GlobalHotkeys hotkeys(system);
    EXPECT_TRUE(hotkeys.Start(config, Target()));
    EXPECT_FALSE(hotkeys.Ownership().screenshot);
    EXPECT_FALSE(hotkeys.Ownership().audioOnly);
    EXPECT_TRUE(hotkeys.Ownership().record);
    ASSERT_EQ(system.lastBindings.size(), 3u);
    for (const HotkeyBinding& binding : system.lastBindings) {
        EXPECT_NE(binding.id, HOTKEY_ID_SCREENSHOT);
        EXPECT_NE(binding.id, HOTKEY_ID_AUDIO_ONLY);
    }
}

TEST(GlobalHotkeysTest, StartKeepsRegisteringWhenTheHookCannotBeInstalled) {
    FakeHotkeySystem system;
    system.hookStarts = false;
    GlobalHotkeys hotkeys(system);
    EXPECT_FALSE(hotkeys.Start(Config(), Target()));  // RegisterHotKey stays the only path
    EXPECT_TRUE(hotkeys.Started());
    EXPECT_TRUE(hotkeys.Ownership().record);
    ASSERT_TRUE(hotkeys.Dispatch(Message(WM_HOTKEY, HOTKEY_ID_RECORD)).has_value());
    EXPECT_EQ(hotkeys.Dispatch(Message(WM_HOTKEY, HOTKEY_ID_RECORD)).value(), HotkeyAction::Record);
}

TEST(GlobalHotkeysTest, StartRefusesATargetThatIsNotTheCallingThreadsQueue) {
    FakeHotkeySystem system;
    GlobalHotkeys hotkeys(system);
    EXPECT_FALSE(hotkeys.Start(Config(), HotkeyDeliveryTarget()));
    EXPECT_FALSE(hotkeys.Start(Config(), HotkeyDeliveryTarget(GetCurrentThreadId(), WM_USER)));  // below WM_APP
    EXPECT_FALSE(hotkeys.Start(Config(), HotkeyDeliveryTarget(GetCurrentThreadId() + 4, kHookMessage)));
    EXPECT_TRUE(system.events.empty());
    EXPECT_FALSE(hotkeys.Started());
}

TEST(GlobalHotkeysTest, ASecondStartDoesNotRegisterAgain) {
    FakeHotkeySystem system;
    GlobalHotkeys hotkeys(system);
    ASSERT_TRUE(hotkeys.Start(Config(), Target()));
    system.events.clear();
    EXPECT_TRUE(hotkeys.Start(Config(), Target()));  // reports the live hook, changes nothing
    EXPECT_TRUE(system.events.empty());
}

TEST(GlobalHotkeysTest, ReloadReRegistersOnlyChangedBindingsAndAlwaysRepublishes) {
    FakeHotkeySystem system;
    GlobalHotkeys hotkeys(system);
    const AppConfig before = Config();
    ASSERT_TRUE(hotkeys.Start(before, Target()));
    system.events.clear();

    AppConfig after = before;
    after.hotkeyStartStop = {VK_F5, false, false, false, false};  // key changed
    after.hotkeyBenchmark = {VK_F8, true, true, false, false};    // modifier changed
    hotkeys.Apply(before, after);
    EXPECT_EQ(system.Take(), (Events{
                                 "unregister 1",
                                 "register 1 vk=116 recording owned",
                                 "unregister 5",
                                 "register 5 vk=119 benchmark owned",
                                 "publish 1 2 3 4 5",
                             }));

    hotkeys.Apply(after, after);  // nothing changed: no registration traffic, but the hook is still told
    EXPECT_EQ(system.Take(), (Events{"publish 1 2 3 4 5"}));
}

TEST(GlobalHotkeysTest, ReloadThatOnlyDisablesAHotkeyStillTakesEffectOnTheHook) {
    FakeHotkeySystem system;
    GlobalHotkeys hotkeys(system);
    const AppConfig before = Config();
    ASSERT_TRUE(hotkeys.Start(before, Target()));
    system.events.clear();

    AppConfig after = before;
    after.hotkeyToggleOverlay = {0, false, false, false, false};
    hotkeys.Apply(before, after);
    EXPECT_EQ(system.Take(), (Events{
                                 "unregister 4",
                                 "register 4 vk=0 overlay toggle not-owned",
                                 "publish 1 2 3 5",
                             }));
    EXPECT_FALSE(hotkeys.Ownership().toggleOverlay);
    EXPECT_TRUE(hotkeys.Ownership().record);
}

TEST(GlobalHotkeysTest, ReboundCombinationLostToAnotherApplicationLeavesTheHookWithoutIt) {
    FakeHotkeySystem system;
    GlobalHotkeys hotkeys(system);
    const AppConfig before = Config();
    ASSERT_TRUE(hotkeys.Start(before, Target()));
    system.events.clear();

    system.taken = {HOTKEY_ID_RECORD};
    AppConfig after = before;
    after.hotkeyStartStop = {VK_F6, false, false, false, false};
    hotkeys.Apply(before, after);
    EXPECT_FALSE(hotkeys.Ownership().record);
    EXPECT_EQ(system.Take().back(), "publish 2 3 4 5");
    for (const HotkeyBinding& binding : system.lastBindings)
        EXPECT_NE(binding.id, HOTKEY_ID_RECORD);
}

TEST(GlobalHotkeysTest, ReloadBeforeStartAndAfterStopDoesNothing) {
    FakeHotkeySystem system;
    GlobalHotkeys hotkeys(system);
    AppConfig changed = Config();
    changed.hotkeyStartStop = {VK_F5, false, false, false, false};
    hotkeys.Apply(Config(), changed);
    EXPECT_TRUE(system.events.empty());

    ASSERT_TRUE(hotkeys.Start(Config(), Target()));
    hotkeys.Stop();
    system.events.clear();
    hotkeys.Apply(Config(), changed);  // a late reload must not bring a hotkey back
    EXPECT_TRUE(system.events.empty());
}

TEST(GlobalHotkeysTest, ReloadFromAnotherThreadIsIgnoredBecauseRegistrationsAreThreadAffine) {
    FakeHotkeySystem system;
    GlobalHotkeys hotkeys(system);
    const AppConfig before = Config();
    ASSERT_TRUE(hotkeys.Start(before, Target()));
    system.events.clear();
    AppConfig after = before;
    after.hotkeyStartStop = {VK_F5, false, false, false, false};
    std::thread other([&] { hotkeys.Apply(before, after); });
    other.join();
    EXPECT_TRUE(system.events.empty());
}

TEST(GlobalHotkeysTest, StopRemovesTheHookBeforeUnregisteringAndIsIdempotent) {
    FakeHotkeySystem system;
    GlobalHotkeys hotkeys(system);
    ASSERT_TRUE(hotkeys.Start(Config(), Target()));
    system.events.clear();

    hotkeys.Stop();
    EXPECT_EQ(system.Take(), (Events{"hook-stop", "unregister 1", "unregister 2", "unregister 3", "unregister 4",
                                     "unregister 5"}));
    EXPECT_FALSE(hotkeys.Started());
    EXPECT_FALSE(hotkeys.Ownership().record);

    hotkeys.Stop();
    EXPECT_TRUE(system.events.empty());
}

TEST(GlobalHotkeysTest, DestructionStopsARunningComponentAndANeverStartedOneIsQuiet) {
    FakeHotkeySystem system;
    {
        GlobalHotkeys idle(system);
    }
    EXPECT_TRUE(system.events.empty());
    {
        GlobalHotkeys running(system);
        ASSERT_TRUE(running.Start(Config(), Target()));
        system.events.clear();
    }
    ASSERT_FALSE(system.events.empty());
    EXPECT_EQ(system.events.front(), "hook-stop");
    EXPECT_EQ(system.events.size(), 6u);
}

TEST(GlobalHotkeysTest, StopFromAnotherThreadStillStopsTheHookButCannotUnregister) {
    FakeHotkeySystem system;
    GlobalHotkeys hotkeys(system);
    ASSERT_TRUE(hotkeys.Start(Config(), Target()));
    system.events.clear();
    std::thread other([&] { hotkeys.Stop(); });
    other.join();
    EXPECT_EQ(system.Take(), (Events{"hook-stop"}));
    EXPECT_FALSE(hotkeys.Started());
}

TEST(GlobalHotkeysTest, CanStartAgainAfterStop) {
    FakeHotkeySystem system;
    GlobalHotkeys hotkeys(system);
    ASSERT_TRUE(hotkeys.Start(Config(), Target()));
    hotkeys.Stop();
    system.events.clear();
    EXPECT_TRUE(hotkeys.Start(Config(), Target()));
    EXPECT_EQ(system.events.size(), 7u);  // five registrations, one publication, one hook start
}

TEST(GlobalHotkeysTest, DispatchMapsBothDeliveryPathsToTheSameActions) {
    FakeHotkeySystem system;
    GlobalHotkeys hotkeys(system);
    ASSERT_TRUE(hotkeys.Start(Config(), Target()));
    const struct {
        int id;
        HotkeyAction action;
    } cases[] = {{HOTKEY_ID_RECORD, HotkeyAction::Record},
                 {HOTKEY_ID_AUDIO_ONLY, HotkeyAction::AudioOnly},
                 {HOTKEY_ID_SCREENSHOT, HotkeyAction::Screenshot},
                 {HOTKEY_ID_TOGGLE_OVERLAY, HotkeyAction::Overlay},
                 {HOTKEY_ID_BENCHMARK, HotkeyAction::Benchmark}};
    for (const auto& entry : cases) {
        SCOPED_TRACE(entry.id);
        const auto viaRegisterHotKey = hotkeys.Dispatch(Message(WM_HOTKEY, entry.id, MAKELONG(0, VK_F9)));
        const auto viaHook = hotkeys.Dispatch(Message(kHookMessage, entry.id, VK_F9));
        ASSERT_TRUE(viaRegisterHotKey.has_value());
        ASSERT_TRUE(viaHook.has_value());
        EXPECT_EQ(viaRegisterHotKey.value(), entry.action);
        EXPECT_EQ(viaHook.value(), entry.action);
    }
}

TEST(GlobalHotkeysTest, DispatchIgnoresUnknownIdsOtherMessagesAndWindowMessages) {
    FakeHotkeySystem system;
    GlobalHotkeys hotkeys(system);
    ASSERT_TRUE(hotkeys.Start(Config(), Target()));
    for (const int id : {0, 6, 99, -1}) {
        EXPECT_FALSE(hotkeys.Dispatch(Message(WM_HOTKEY, id)).has_value()) << id;
        EXPECT_FALSE(hotkeys.Dispatch(Message(kHookMessage, id, VK_F9)).has_value()) << id;
    }
    EXPECT_FALSE(hotkeys.Dispatch(Message(WM_APP + 0x241, HOTKEY_ID_RECORD)).has_value());  // someone else's message
    EXPECT_FALSE(hotkeys.Dispatch(Message(WM_TIMER, HOTKEY_ID_RECORD)).has_value());
    // A message addressed to a window is never a thread-message hotkey delivery.
    const HWND window = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1234));
    EXPECT_FALSE(hotkeys.Dispatch(Message(WM_HOTKEY, HOTKEY_ID_RECORD, 0, window)).has_value());
    EXPECT_FALSE(hotkeys.Dispatch(Message(kHookMessage, HOTKEY_ID_RECORD, VK_F9, window)).has_value());
}

TEST(GlobalHotkeysTest, DispatchServesNothingBeforeStartOrAfterStop) {
    FakeHotkeySystem system;
    GlobalHotkeys hotkeys(system);
    EXPECT_FALSE(hotkeys.Dispatch(Message(WM_HOTKEY, HOTKEY_ID_RECORD)).has_value());
    ASSERT_TRUE(hotkeys.Start(Config(), Target()));
    hotkeys.Stop();
    // A press already queued when the hotkeys were torn down must not start a recording.
    EXPECT_FALSE(hotkeys.Dispatch(Message(WM_HOTKEY, HOTKEY_ID_RECORD)).has_value());
    EXPECT_FALSE(hotkeys.Dispatch(Message(kHookMessage, HOTKEY_ID_RECORD, VK_F9)).has_value());
}

TEST(GlobalHotkeysTest, DiagnosticsAreForwardedToTheHook) {
    FakeHotkeySystem system;
    GlobalHotkeys hotkeys(system);
    hotkeys.ReportDiagnostics();
    EXPECT_EQ(system.Take(), (Events{"report"}));
}

}  // namespace
