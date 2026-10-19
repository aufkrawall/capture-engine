#include <gtest/gtest.h>

#include <windows.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "runtime/inject/launch_command.h"
#include "runtime/inject/target_launcher.h"

// Launches go through a fake injector. The processes that do get created are copies of cmd.exe that
// exit immediately (`/c exit 0`), placed in a folder with a non-ASCII name; no game, hook or real
// injection is involved.

namespace {

namespace fs = std::filesystem;
using ce::runtime::LaunchInjection;
using ce::runtime::LaunchKind;
using ce::runtime::LaunchResult;
using ce::runtime::TargetInjector;
using ce::runtime::TargetLauncher;

class FakeInjector final : public TargetInjector {
public:
    explicit FakeInjector(bool* destroyed = nullptr) : destroyed_(destroyed) {}
    ~FakeInjector() override {
        if (destroyed_) {
            *destroyed_ = true;
        }
    }

    bool InjectEarly(uint32_t processId, void* suspendedMainThread) override {
        calls.push_back("early");
        earlyProcessIds.push_back(processId);
        // The process has never run: its initial thread carries exactly the creation suspend.
        const DWORD previous = SuspendThread(suspendedMainThread);
        suspendCountSeen.push_back(previous);
        if (previous != static_cast<DWORD>(-1)) {
            ResumeThread(suspendedMainThread);
        }
        return earlyResult;
    }

    bool InjectResumed(uint32_t processId, const std::string& fileName) override {
        calls.push_back("resumed");
        resumedProcessIds.push_back(processId);
        resumedFileNames.push_back(fileName);
        return resumedResult;
    }

    bool earlyResult = false;
    bool resumedResult = false;
    std::vector<std::string> calls;
    std::vector<uint32_t> earlyProcessIds;
    std::vector<DWORD> suspendCountSeen;
    std::vector<uint32_t> resumedProcessIds;
    std::vector<std::string> resumedFileNames;

private:
    bool* destroyed_;
};

class TargetLauncherTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() / (L"ce_target_launcher_" + std::to_wstring(GetCurrentProcessId()));
        // Outside every Western code page: U+00DC, then CJK ("game" in Chinese).
        directory_ = root_ / L"Spiele \u00DC\u6E38\u620F";
        fs::create_directories(directory_);
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    // A harmless program: cmd.exe under another name, in the non-ASCII folder.
    fs::path CopyProgram(const wchar_t* name) {
        wchar_t system[MAX_PATH] = {};
        GetSystemDirectoryW(system, MAX_PATH);
        const fs::path target = directory_ / name;
        std::error_code error;
        fs::copy_file(fs::path(system) / L"cmd.exe", target, fs::copy_options::overwrite_existing, error);
        EXPECT_FALSE(error) << error.message();
        return target;
    }

    static std::string CommandLine(const fs::path& program, const char* arguments = " /c exit 0") {
        std::string utf8;
        EXPECT_TRUE(ce::runtime::WideToUtf8(program.wstring(), &utf8));
        return "\"" + utf8 + "\"" + arguments;
    }

    // A launched program that was resumed ends on its own; one left suspended never would.
    static void ExpectExited(uint32_t processId) {
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
        if (!process) {
            return;  // already gone
        }
        EXPECT_EQ(WaitForSingleObject(process, 20000), WAIT_OBJECT_0) << "the launched process never ran";
        CloseHandle(process);
    }

    fs::path root_;
    fs::path directory_;
};

TEST_F(TargetLauncherTest, RejectsAnUnparsableCommandWithoutTouchingTheInjector) {
    auto fake = std::make_unique<FakeInjector>();
    FakeInjector* injector = fake.get();
    TargetLauncher launcher(std::move(fake));

    for (const char* command : {"", "   ", "\"unterminated"}) {
        const LaunchResult result = launcher.Launch(command);
        EXPECT_FALSE(result.started) << command;
        EXPECT_EQ(result.error, static_cast<uint32_t>(ERROR_INVALID_PARAMETER)) << command;
    }
    EXPECT_TRUE(injector->calls.empty());
}

TEST_F(TargetLauncherTest, RejectsInvalidUtf8InsteadOfLaunchingAnotherFile) {
    auto fake = std::make_unique<FakeInjector>();
    FakeInjector* injector = fake.get();
    TargetLauncher launcher(std::move(fake));

    const LaunchResult result = launcher.Launch("\"C:\\Games\\\xC3\x28\\game_dx11.exe\" -x");
    EXPECT_FALSE(result.started);
    EXPECT_EQ(result.error, static_cast<uint32_t>(ERROR_NO_UNICODE_TRANSLATION));
    EXPECT_TRUE(injector->calls.empty());
}

TEST_F(TargetLauncherTest, ReportsAMissingProgramAsAFailureWithItsWin32Error) {
    // The folder exists and has a non-ASCII name, so a path mangled by the ANSI code page would fail
    // earlier (measured: ERROR_DIRECTORY, the working directory does not exist); the real name
    // reaches CreateProcessW and only the file is missing.
    for (const wchar_t* name : {L"launcher.exe", L"missing_game_dx11.exe"}) {
        auto fake = std::make_unique<FakeInjector>();
        FakeInjector* injector = fake.get();
        TargetLauncher launcher(std::move(fake));

        const LaunchResult result = launcher.Launch(CommandLine(directory_ / name));
        EXPECT_FALSE(result.started);
        EXPECT_EQ(result.processId, 0u);
        EXPECT_EQ(result.error, static_cast<uint32_t>(ERROR_FILE_NOT_FOUND)) << "error " << result.error;
        EXPECT_EQ(result.injection, LaunchInjection::None);
        // No process exists, so there is nothing to inject into.
        EXPECT_TRUE(injector->calls.empty());
    }
}

TEST_F(TargetLauncherTest, StartsALauncherNormallyFromANonAsciiFolderWithoutInjecting) {
    const fs::path program = CopyProgram(L"launcher.exe");
    auto fake = std::make_unique<FakeInjector>();
    FakeInjector* injector = fake.get();
    TargetLauncher launcher(std::move(fake));

    const LaunchResult result = launcher.Launch(CommandLine(program));
    ASSERT_TRUE(result.started) << "error " << result.error;
    EXPECT_NE(result.processId, 0u);
    EXPECT_EQ(result.kind, LaunchKind::Launcher);
    EXPECT_EQ(result.injection, LaunchInjection::None);
    EXPECT_TRUE(injector->calls.empty());
    ExpectExited(result.processId);
}

TEST_F(TargetLauncherTest, GameIsInjectedEarlyWhileStillSuspended) {
    const fs::path program = CopyProgram(L"fake_game_dx11.exe");
    auto fake = std::make_unique<FakeInjector>();
    FakeInjector* injector = fake.get();
    injector->earlyResult = true;
    TargetLauncher launcher(std::move(fake));

    const LaunchResult result = launcher.Launch(CommandLine(program));
    ASSERT_TRUE(result.started) << "error " << result.error;
    EXPECT_EQ(result.kind, LaunchKind::Game);
    EXPECT_EQ(result.injection, LaunchInjection::EarlyApc);
    ASSERT_EQ(injector->calls.size(), 1u);
    EXPECT_EQ(injector->calls[0], "early");
    EXPECT_EQ(injector->earlyProcessIds[0], result.processId);
    ASSERT_EQ(injector->suspendCountSeen.size(), 1u);
    EXPECT_EQ(injector->suspendCountSeen[0], 1u) << "early injection must precede the first resume";
    ExpectExited(result.processId);  // and the launcher did resume it afterwards
}

TEST_F(TargetLauncherTest, FailedEarlyInjectionFallsBackAfterTheResume) {
    const fs::path program = CopyProgram(L"fake_game_dx11.exe");
    auto fake = std::make_unique<FakeInjector>();
    FakeInjector* injector = fake.get();
    injector->resumedResult = true;
    TargetLauncher launcher(std::move(fake));

    const LaunchResult result = launcher.Launch(CommandLine(program));
    ASSERT_TRUE(result.started) << "error " << result.error;
    EXPECT_EQ(result.injection, LaunchInjection::Fallback);
    ASSERT_EQ(injector->calls.size(), 2u);
    EXPECT_EQ(injector->calls[0], "early");
    EXPECT_EQ(injector->calls[1], "resumed");
    EXPECT_EQ(injector->resumedProcessIds[0], result.processId);
    EXPECT_EQ(injector->resumedFileNames[0], "fake_game_dx11.exe");
    ExpectExited(result.processId);
}

TEST_F(TargetLauncherTest, GameStaysRunningWhenBothInjectionRoutesFail) {
    const fs::path program = CopyProgram(L"fake_game_dx11.exe");
    auto fake = std::make_unique<FakeInjector>();
    FakeInjector* injector = fake.get();
    TargetLauncher launcher(std::move(fake));

    const LaunchResult result = launcher.Launch(CommandLine(program));
    ASSERT_TRUE(result.started) << "error " << result.error;
    EXPECT_EQ(result.injection, LaunchInjection::Failed);
    EXPECT_EQ(injector->calls.size(), 2u);
    ExpectExited(result.processId);  // never left suspended
}

TEST_F(TargetLauncherTest, GameWithoutAnInjectorIsStillResumed) {
    const fs::path program = CopyProgram(L"fake_game_dx11.exe");
    TargetLauncher launcher(std::unique_ptr<TargetInjector>{});

    const LaunchResult result = launcher.Launch(CommandLine(program));
    ASSERT_TRUE(result.started) << "error " << result.error;
    EXPECT_EQ(result.injection, LaunchInjection::Failed);
    ExpectExited(result.processId);
}

TEST_F(TargetLauncherTest, OneInjectorServesEveryLaunchAndDiesWithTheLauncher) {
    const fs::path program = CopyProgram(L"fake_game_dx11.exe");
    bool destroyed = false;
    auto fake = std::make_unique<FakeInjector>(&destroyed);
    FakeInjector* injector = fake.get();
    injector->earlyResult = true;
    {
        TargetLauncher launcher(std::move(fake));
        const LaunchResult first = launcher.Launch(CommandLine(program));
        const LaunchResult second = launcher.Launch(CommandLine(program));
        ASSERT_TRUE(first.started && second.started);
        // A second launch must not recreate (and so eject) the injector that hooked the first.
        EXPECT_EQ(injector->calls.size(), 2u);
        EXPECT_FALSE(destroyed);
        ExpectExited(first.processId);
        ExpectExited(second.processId);
    }
    EXPECT_TRUE(destroyed) << "the launcher's destruction, not static destruction, ends the injector";
}

}  // namespace
