#include <gtest/gtest.h>
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "../common/crash_handler.h"
#include "../common/keyboard_hook_policy.h"
#include "source_fragment_reader.h"

// CaptureEngine must never make keyboard input of other applications wait.
// The controller's WH_KEYBOARD_LL hook sits in front of every keystroke on the
// desktop, so anything that holds its thread - a log write, a busy game thread
// outranking it, a crash dump suspending the process - delays every key on the
// machine by up to the system hook timeout, and repeated timeouts make Windows
// remove the hook silently. These tests pin the rules that keep it responsive.

namespace {

namespace policy = ce::keyboard_hook;

std::string ReadSource(const char* relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

std::string Between(const std::string& text, const char* begin, const char* end) {
    const size_t from = text.find(begin);
    if (from == std::string::npos)
        return {};
    const size_t to = text.find(end, from + 1);
    return text.substr(from, to == std::string::npos ? std::string::npos : to - from);
}

int g_PreDumpCalls = 0;
void CountPreDump() {
    ++g_PreDumpCalls;
}

}  // namespace

TEST(KeyboardHookPolicyTest, TimeoutFollowsTheUserSettingWithinTheSystemCap) {
    EXPECT_EQ(policy::ResolveLowLevelHooksTimeoutMs(false, 0), policy::kDefaultLowLevelHooksTimeoutMs);
    EXPECT_EQ(policy::ResolveLowLevelHooksTimeoutMs(true, 0), policy::kDefaultLowLevelHooksTimeoutMs);
    EXPECT_EQ(policy::ResolveLowLevelHooksTimeoutMs(true, 200), 200u);
    EXPECT_EQ(policy::ResolveLowLevelHooksTimeoutMs(true, 10000), policy::kMaxLowLevelHooksTimeoutMs);
}

TEST(KeyboardHookPolicyTest, CallbackAgeSurvivesTickWrapAndFutureTimestamps) {
    EXPECT_EQ(policy::CallbackAgeMs(1000, 1016), 16u);
    EXPECT_EQ(policy::CallbackAgeMs(0xFFFFFFF0u, 0x10u), 0x20u);
    // A synthetic event may carry a caller-chosen time ahead of the clock.
    EXPECT_EQ(policy::CallbackAgeMs(5000, 4000), 0u);
}

TEST(KeyboardHookPolicyTest, LateAnswersRearmBeforeTheSystemCouldRemoveTheHook) {
    const DWORD timeout = 300;
    EXPECT_FALSE(policy::ShouldRearmAfterCallback(0, timeout, false));
    EXPECT_FALSE(policy::ShouldRearmAfterCallback(149, timeout, false));
    EXPECT_TRUE(policy::ShouldRearmAfterCallback(150, timeout, false));
    EXPECT_TRUE(policy::ShouldRearmAfterCallback(5000, timeout, false));
    // Injected events carry caller-chosen timestamps and prove nothing.
    EXPECT_FALSE(policy::ShouldRearmAfterCallback(5000, timeout, true));
}

TEST(KeyboardHookPolicyTest, AnEventPastTheTimeoutIsNoLongerOursToConsume) {
    EXPECT_FALSE(policy::IsPastSystemTimeout(299, 300));
    EXPECT_TRUE(policy::IsPastSystemTimeout(300, 300));
}

TEST(CrashPreDumpCallbackTest, RunsWhenRegisteredAndNotAfterUnregistering) {
    g_PreDumpCalls = 0;
    NotifyCrashPreDumpForTesting();
    EXPECT_EQ(g_PreDumpCalls, 0);

    RegisterCrashPreDumpCallback(CountPreDump);
    NotifyCrashPreDumpForTesting();
    EXPECT_EQ(g_PreDumpCalls, 1);

    RegisterCrashPreDumpCallback(nullptr);
    NotifyCrashPreDumpForTesting();
    EXPECT_EQ(g_PreDumpCalls, 1);
}

// The dump worker suspends the process; the release has to come first.
TEST(CrashPreDumpCallbackTest, FatalPathReleasesBeforeTheDumpWorkerStarts) {
    const std::string writer = ReadSource("common/crash_dump_writer.cpp");
    ASSERT_FALSE(writer.empty());
    const size_t release = writer.find("NotifyCrashPreDump();");
    const size_t worker = writer.find("CreateThread(NULL, 0, DumpWorker");
    ASSERT_NE(release, std::string::npos);
    ASSERT_NE(worker, std::string::npos);
    EXPECT_LT(release, worker);
}

TEST(KeyboardHookSourceTest, HookThreadNeverLogsAndOutranksNormalThreads) {
    const std::string hook = ReadSource("captureengine/hotkey_input_hook.cpp");
    ASSERT_FALSE(hook.empty());

    // The per-event path and the re-arm path run while the hook is installed.
    // Log() takes a process-wide mutex and flushes to disk on every line; the
    // hotkey that starts a recording used to wait on exactly that.
    const std::string callbackPath = Between(hook, "bool HandleKeyEvent(", "void HotkeyHookThreadMain(");
    ASSERT_FALSE(callbackPath.empty());
    for (const char* logCall : {"LogDebug(", "LogInfo(", "LogWarn(", "LogError(", "Log(", "fflush("})
        EXPECT_EQ(callbackPath.find(logCall), std::string::npos) << logCall;

    const std::string threadLoop = Between(hook, "void HotkeyHookThreadMain(", "while (true)");
    ASSERT_FALSE(threadLoop.empty());
    EXPECT_NE(threadLoop.find("THREAD_PRIORITY_TIME_CRITICAL"), std::string::npos);
    for (const char* logCall : {"LogDebug(", "LogInfo(", "LogWarn(", "LogError("})
        EXPECT_EQ(threadLoop.find(logCall), std::string::npos) << logCall;

    // An event the system already passed on is bookkept, never consumed.
    EXPECT_NE(callbackPath.find("if (pastTimeout)\n        return false;"), std::string::npos);

    EXPECT_NE(hook.find("RegisterCrashPreDumpCallback(ReleaseHotkeyInputHookForCrash)"), std::string::npos);
    EXPECT_NE(hook.find("RegisterCrashPreDumpCallback(nullptr)"), std::string::npos);

    // The controller reports what the hook thread counted.
    const std::string controller = ReadSource("captureengine/main.cpp");
    ASSERT_FALSE(controller.empty());
    EXPECT_NE(controller.find("ReportHotkeyInputHookDiagnostics();"), std::string::npos);
    EXPECT_NE(controller.find("[Hotkey] Keyboard-hook delivery"), std::string::npos);
}

// The injected runtime used to replace the game window's procedure with a
// forwarder that did nothing but take a mutex per message - every WM_INPUT of
// an 8 kHz mouse included - and log under that mutex. The overlay takes no
// window input, so no hook source may subclass a window.
TEST(KeyboardHookSourceTest, InjectedRuntimeNeverSubclassesTheGameWindow) {
    const std::filesystem::path root = std::filesystem::current_path() / "hook";
    size_t scanned = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        const std::string extension = entry.path().extension().string();
        if (!entry.is_regular_file() || (extension != ".cpp" && extension != ".h"))
            continue;
        std::ifstream stream(entry.path(), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        ++scanned;
        EXPECT_EQ(text.find("GWLP_WNDPROC"), std::string::npos) << entry.path().string();
        EXPECT_EQ(text.find("SetWindowSubclass("), std::string::npos) << entry.path().string();
    }
    EXPECT_GT(scanned, 50u);
    EXPECT_FALSE(std::filesystem::exists(root / "common" / "input_manager.cpp"));
}
