#include <gtest/gtest.h>
#include <windows.h>

#include <filesystem>
#include <string>

#include "../common/config.h"
#include "../common/hotkey_matcher.h"
#include "source_fragment_reader.h"

namespace {

// Hotkeys stopped working in DOOM Eternal because the game registers its
// raw-input keyboard with RIDEV_NOHOTKEYS (usage page 1 / usage 6,
// dwFlags=0x200), which turns off application hotkey processing for every
// process while the game is foreground: the controller's WM_HOTKEY was never
// generated at all. A low-level keyboard hook runs ahead of that suppression,
// and these tests pin the semantics that hook has to reproduce so the two
// delivery paths stay interchangeable and can never both fire for one press.
constexpr int kVkNine = '9';
constexpr int kVkEight = '8';

HotkeyModifierState Mods(bool ctrl = false, bool shift = false, bool alt = false, bool win = false) {
    HotkeyModifierState modifiers;
    modifiers.ctrl = ctrl;
    modifiers.shift = shift;
    modifiers.alt = alt;
    modifiers.win = win;
    return modifiers;
}

HotkeyBinding Binding(int id, int vkey, bool ctrl = false, bool shift = false, bool alt = false, bool win = false) {
    HotkeyBinding binding;
    binding.id = id;
    binding.vkey = vkey;
    binding.ctrl = ctrl;
    binding.shift = shift;
    binding.alt = alt;
    binding.win = win;
    return binding;
}

class HotkeyMatcherTest : public ::testing::Test {
protected:
    HotkeyMatcher matcher;

    HotkeyMatchResult Down(const HotkeyBinding* bindings, size_t count, int vkey, const HotkeyModifierState& mods) {
        return matcher.Observe(bindings, count, vkey, HotkeyKeyAction::KeyDown, mods);
    }

    HotkeyMatchResult Up(const HotkeyBinding* bindings, size_t count, int vkey, const HotkeyModifierState& mods) {
        return matcher.Observe(bindings, count, vkey, HotkeyKeyAction::KeyUp, mods);
    }
};

TEST_F(HotkeyMatcherTest, ExactModifierMatchFiresAndConsumesTheKey) {
    const HotkeyBinding bindings[] = {Binding(HOTKEY_ID_RECORD, kVkNine, true)};

    const HotkeyMatchResult result = Down(bindings, 1, kVkNine, Mods(true));

    EXPECT_EQ(result.id, HOTKEY_ID_RECORD);
    // Consumption is what keeps the paths mutually exclusive: a key the hook
    // eats never reaches hotkey processing, so it can never also arrive as
    // WM_HOTKEY and toggle the same recording twice.
    EXPECT_TRUE(result.swallow);
}

TEST_F(HotkeyMatcherTest, ExtraModifierDoesNotMatch) {
    const HotkeyBinding bindings[] = {Binding(HOTKEY_ID_RECORD, kVkNine, true)};

    const HotkeyMatchResult result = Down(bindings, 1, kVkNine, Mods(true, true));

    EXPECT_EQ(result.id, 0);
    EXPECT_FALSE(result.swallow);
}

TEST_F(HotkeyMatcherTest, MissingModifierDoesNotMatch) {
    const HotkeyBinding bindings[] = {Binding(HOTKEY_ID_RECORD, kVkNine, true)};

    const HotkeyMatchResult result = Down(bindings, 1, kVkNine, Mods());

    EXPECT_EQ(result.id, 0);
    EXPECT_FALSE(result.swallow);
}

TEST_F(HotkeyMatcherTest, AutoRepeatDoesNotFireAgainButStaysConsumed) {
    const HotkeyBinding bindings[] = {Binding(HOTKEY_ID_RECORD, kVkNine, true)};

    ASSERT_EQ(Down(bindings, 1, kVkNine, Mods(true)).id, HOTKEY_ID_RECORD);

    // The registration carries MOD_NOREPEAT, so holding the key must not toggle
    // recording once per repeat. Leaking the repeats to the application would
    // also hand it half a keystroke.
    for (int repeat = 0; repeat < 5; ++repeat) {
        const HotkeyMatchResult result = Down(bindings, 1, kVkNine, Mods(true));
        EXPECT_EQ(result.id, 0);
        EXPECT_TRUE(result.swallow);
    }
}

TEST_F(HotkeyMatcherTest, ReleaseOfAConsumedPressIsConsumedOnce) {
    const HotkeyBinding bindings[] = {Binding(HOTKEY_ID_RECORD, kVkNine, true)};
    ASSERT_TRUE(Down(bindings, 1, kVkNine, Mods(true)).swallow);

    const HotkeyMatchResult release = Up(bindings, 1, kVkNine, Mods(true));
    EXPECT_EQ(release.id, 0);
    EXPECT_TRUE(release.swallow);

    // A second release belongs to no consumed press.
    EXPECT_FALSE(Up(bindings, 1, kVkNine, Mods(true)).swallow);
}

TEST_F(HotkeyMatcherTest, ReleaseIsConsumedEvenWhenTheModifierWentUpFirst) {
    const HotkeyBinding bindings[] = {Binding(HOTKEY_ID_RECORD, kVkNine, true)};
    ASSERT_TRUE(Down(bindings, 1, kVkNine, Mods(true)).swallow);

    // Releasing Ctrl before the digit is the common case, and the digit's
    // release still belongs to the keystroke that was consumed.
    EXPECT_TRUE(Up(bindings, 1, kVkNine, Mods()).swallow);
}

TEST_F(HotkeyMatcherTest, UnmatchedKeysPassThroughInBothDirections) {
    const HotkeyBinding bindings[] = {Binding(HOTKEY_ID_RECORD, kVkNine, true)};

    EXPECT_FALSE(Down(bindings, 1, kVkEight, Mods(true)).swallow);
    EXPECT_FALSE(Up(bindings, 1, kVkEight, Mods(true)).swallow);
}

TEST_F(HotkeyMatcherTest, PressAfterReleaseFiresAgain) {
    const HotkeyBinding bindings[] = {Binding(HOTKEY_ID_RECORD, kVkNine, true)};

    ASSERT_EQ(Down(bindings, 1, kVkNine, Mods(true)).id, HOTKEY_ID_RECORD);
    ASSERT_TRUE(Up(bindings, 1, kVkNine, Mods(true)).swallow);

    EXPECT_EQ(Down(bindings, 1, kVkNine, Mods(true)).id, HOTKEY_ID_RECORD);
}

TEST_F(HotkeyMatcherTest, ModifierKeysNeverMatchAndAreNeverConsumed) {
    // Consuming a modifier would break typing in every other application.
    const HotkeyBinding bindings[] = {Binding(HOTKEY_ID_RECORD, VK_CONTROL, false),
                                      Binding(HOTKEY_ID_SCREENSHOT, VK_LSHIFT, false)};

    for (const int vkey : {VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_MENU, VK_LMENU,
                           VK_RMENU, VK_LWIN, VK_RWIN}) {
        EXPECT_TRUE(IsHotkeyModifierKey(vkey)) << "vkey=" << vkey;
        const HotkeyMatchResult down = Down(bindings, 2, vkey, Mods());
        EXPECT_EQ(down.id, 0) << "vkey=" << vkey;
        EXPECT_FALSE(down.swallow) << "vkey=" << vkey;
        EXPECT_FALSE(Up(bindings, 2, vkey, Mods()).swallow) << "vkey=" << vkey;
    }
    EXPECT_FALSE(IsHotkeyModifierKey(kVkNine));
}

TEST_F(HotkeyMatcherTest, BindingsWithoutAKeyNeverMatch) {
    const HotkeyBinding bindings[] = {Binding(HOTKEY_ID_SCREENSHOT, 0, true)};

    EXPECT_EQ(Down(bindings, 1, 0, Mods(true)).id, 0);
    EXPECT_EQ(Down(bindings, 1, kVkNine, Mods(true)).id, 0);
}

TEST_F(HotkeyMatcherTest, SameKeyWithDifferentModifiersSelectsTheRightBinding) {
    const HotkeyBinding bindings[] = {Binding(HOTKEY_ID_RECORD, kVkNine, true),
                                      Binding(HOTKEY_ID_AUDIO_ONLY, kVkNine, true, true)};

    EXPECT_EQ(Down(bindings, 2, kVkNine, Mods(true, true)).id, HOTKEY_ID_AUDIO_ONLY);
    ASSERT_TRUE(Up(bindings, 2, kVkNine, Mods(true, true)).swallow);
    EXPECT_EQ(Down(bindings, 2, kVkNine, Mods(true)).id, HOTKEY_ID_RECORD);
}

TEST_F(HotkeyMatcherTest, EmptyTableStillTracksKeyStateSoLaterPressesFire) {
    // The hook refuses to wait on the binding table, so a press that lands while
    // the table is being republished sees no bindings. That press must not leave
    // the key marked down, or the next one would be misread as auto-repeat.
    const HotkeyBinding bindings[] = {Binding(HOTKEY_ID_RECORD, kVkNine, true)};

    EXPECT_FALSE(Down(nullptr, 0, kVkNine, Mods(true)).swallow);
    EXPECT_FALSE(Up(nullptr, 0, kVkNine, Mods(true)).swallow);

    EXPECT_EQ(Down(bindings, 1, kVkNine, Mods(true)).id, HOTKEY_ID_RECORD);
}

TEST_F(HotkeyMatcherTest, ClearSwallowLetsTheReleaseThroughAfterAFailedDelivery) {
    // Posting the match to the controller can fail. The key then has to reach
    // the application after all, release included.
    const HotkeyBinding bindings[] = {Binding(HOTKEY_ID_RECORD, kVkNine, true)};
    ASSERT_TRUE(Down(bindings, 1, kVkNine, Mods(true)).swallow);

    matcher.ClearSwallow(kVkNine);

    EXPECT_FALSE(Up(bindings, 1, kVkNine, Mods(true)).swallow);
}

TEST_F(HotkeyMatcherTest, ResetDropsKeyStateSoAReinstalledHookIsNotOutOfSync) {
    const HotkeyBinding bindings[] = {Binding(HOTKEY_ID_RECORD, kVkNine, true)};
    ASSERT_EQ(Down(bindings, 1, kVkNine, Mods(true)).id, HOTKEY_ID_RECORD);

    matcher.Reset();

    // Without the reset this would look like auto-repeat and never fire again.
    EXPECT_EQ(Down(bindings, 1, kVkNine, Mods(true)).id, HOTKEY_ID_RECORD);
}

TEST_F(HotkeyMatcherTest, OutOfRangeVirtualKeysAreIgnored) {
    const HotkeyBinding bindings[] = {Binding(HOTKEY_ID_RECORD, 4096, true)};

    const HotkeyMatchResult result = Down(bindings, 1, 4096, Mods(true));
    EXPECT_EQ(result.id, 0);
    EXPECT_FALSE(result.swallow);
    EXPECT_FALSE(Down(bindings, 1, -1, Mods(true)).swallow);
}

TEST_F(HotkeyMatcherTest, EveryMatchConsumesTheKey) {
    // The invariant that keeps a press from being handled twice: there is no
    // match that leaves the key available for hotkey processing.
    const HotkeyBinding bindings[] = {Binding(HOTKEY_ID_TOGGLE_OVERLAY, kVkEight, true)};

    const HotkeyMatchResult result = Down(bindings, 1, kVkEight, Mods(true));
    ASSERT_NE(result.id, 0);
    EXPECT_TRUE(result.swallow);
}

HotkeyOwnership OwnsEverything() {
    HotkeyOwnership ownership;
    ownership.record = true;
    ownership.screenshot = true;
    ownership.audioOnly = true;
    ownership.toggleOverlay = true;
    return ownership;
}

TEST(HotkeyBindingTableTest, BuildsOneBindingPerConfiguredHotkey) {
    AppConfig config;
    config.hotkeyStartStop = {kVkNine, true, false, false, false};
    config.hotkeyToggleOverlay = {kVkEight, true, false, false, false};
    config.hotkeyScreenshot = {'0', true, false, false, false};
    config.hotkeyAudioOnly = {VK_OEM_MINUS, true, false, false, false};

    HotkeyBinding bindings[kMaxHotkeyBindings] = {};
    const size_t count = BuildHotkeyBindings(config, OwnsEverything(), bindings, kMaxHotkeyBindings);

    ASSERT_EQ(count, 4u);
    EXPECT_EQ(bindings[0].id, HOTKEY_ID_RECORD);
    EXPECT_EQ(bindings[0].vkey, kVkNine);
    EXPECT_TRUE(bindings[0].ctrl);
    EXPECT_EQ(bindings[1].id, HOTKEY_ID_SCREENSHOT);
    EXPECT_EQ(bindings[1].vkey, '0');
    EXPECT_EQ(bindings[2].id, HOTKEY_ID_AUDIO_ONLY);
    EXPECT_EQ(bindings[2].vkey, VK_OEM_MINUS);
    EXPECT_EQ(bindings[3].id, HOTKEY_ID_TOGGLE_OVERLAY);
    EXPECT_EQ(bindings[3].vkey, kVkEight);
}

TEST(HotkeyBindingTableTest, DisabledHotkeysAreDroppedFromTheTable) {
    AppConfig config;
    config.hotkeyStartStop = {VK_F9, false, false, false, false};
    config.hotkeyToggleOverlay = {0, false, false, false, false};
    config.hotkeyScreenshot = {0, false, false, false, false};
    config.hotkeyAudioOnly = {0, false, false, false, false};

    HotkeyBinding bindings[kMaxHotkeyBindings] = {};
    const size_t count = BuildHotkeyBindings(config, OwnsEverything(), bindings, kMaxHotkeyBindings);

    ASSERT_EQ(count, 1u);
    EXPECT_EQ(bindings[0].id, HOTKEY_ID_RECORD);
    EXPECT_EQ(bindings[0].vkey, VK_F9);

    HotkeyMatcher matcher;
    EXPECT_EQ(matcher.Observe(bindings, count, 0, HotkeyKeyAction::KeyDown, Mods()).id, 0);
}

TEST(HotkeyBindingTableTest, HotkeysThisProcessDoesNotOwnAreDropped) {
    // RegisterHotKey is exclusive per combination. Losing it means another
    // application owns the combination, and the keyboard path must not take it
    // away from that application by consuming the key first.
    AppConfig config;
    config.hotkeyStartStop = {kVkNine, true, false, false, false};
    config.hotkeyToggleOverlay = {kVkEight, true, false, false, false};

    HotkeyOwnership ownership;
    ownership.toggleOverlay = true;

    HotkeyBinding bindings[kMaxHotkeyBindings] = {};
    const size_t count = BuildHotkeyBindings(config, ownership, bindings, kMaxHotkeyBindings);

    ASSERT_EQ(count, 1u);
    EXPECT_EQ(bindings[0].id, HOTKEY_ID_TOGGLE_OVERLAY);

    HotkeyMatcher matcher;
    const HotkeyMatchResult lost = matcher.Observe(bindings, count, kVkNine, HotkeyKeyAction::KeyDown, Mods(true));
    EXPECT_EQ(lost.id, 0);
    EXPECT_FALSE(lost.swallow);
}

size_t CountOccurrences(const std::string& haystack, const std::string& needle) {
    size_t count = 0;
    for (size_t at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + 1))
        ++count;
    return count;
}

std::string ReadSource(const char* relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

// The bug this pins: hotkeys reached the controller only through RegisterHotKey,
// which a foreground application can switch off for the whole desktop with
// RIDEV_NOHOTKEYS. The wiring below is what keeps a press arriving anyway.
TEST(HotkeyDeliveryWiringTest, ControllerRunsBothDeliveryPathsAndTheHookNeverBlocks) {
    const std::string controllerSource = ReadSource("captureengine/main.cpp");
    const std::string hookSource = ReadSource("captureengine/hotkey_input_hook.cpp");
    ASSERT_FALSE(controllerSource.empty());
    ASSERT_FALSE(hookSource.empty());

    // Startup owns the combinations through RegisterHotKey, hands the same set
    // to the keyboard path, and starts that path on the controller thread -
    // the thread RegisterHotKey posts to.
    EXPECT_NE(controllerSource.find("RegisterConfiguredHotkey(HOTKEY_ID_RECORD"), std::string::npos);
    EXPECT_NE(controllerSource.find("PublishHotkeyBindings(main_g_Config, main_g_HotkeyOwnership)"),
              std::string::npos);
    EXPECT_NE(controllerSource.find("StartHotkeyInputHook(GetCurrentThreadId())"), std::string::npos);
    EXPECT_NE(controllerSource.find("StopHotkeyInputHook();"), std::string::npos);
    // Startup and config reload both republish, or a rebound hotkey would keep
    // working on one path only.
    EXPECT_EQ(CountOccurrences(controllerSource, "PublishHotkeyBindings(main_g_Config, main_g_HotkeyOwnership)"), 2u);

    // A matched key is consumed. That is what stops the same press from also
    // arriving as WM_HOTKEY and acting twice.
    EXPECT_NE(hookSource.find("WH_KEYBOARD_LL"), std::string::npos);
    EXPECT_NE(hookSource.find("main_kMsgHotkeyFromInputHook"), std::string::npos);
    EXPECT_NE(hookSource.find("return 1;"), std::string::npos);

    // The hook must never wait on the binding table: a low-level keyboard hook
    // that blocks stalls input for every process on the desktop, and the system
    // drops hooks that answer too slowly. Every shared acquisition is the Try
    // form, so the counts have to match.
    EXPECT_EQ(CountOccurrences(hookSource, "AcquireSRWLockShared("),
              CountOccurrences(hookSource, "TryAcquireSRWLockShared("));
    EXPECT_GT(CountOccurrences(hookSource, "TryAcquireSRWLockShared("), 0u);
}

TEST(HotkeyBindingTableTest, RejectsAnEmptyDestination) {
    AppConfig config;
    config.hotkeyStartStop = {VK_F9, false, false, false, false};

    HotkeyBinding bindings[kMaxHotkeyBindings] = {};
    EXPECT_EQ(BuildHotkeyBindings(config, OwnsEverything(), nullptr, kMaxHotkeyBindings), 0u);
    EXPECT_EQ(BuildHotkeyBindings(config, OwnsEverything(), bindings, 0), 0u);
}

}  // namespace
