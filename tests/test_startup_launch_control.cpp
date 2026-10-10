// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "common/platform/startup_launch_control.h"
#include "common/config/config.h"
#include "common/platform/ansi_path.h"
#include "tests/source_fragment_reader.h"

#include <gtest/gtest.h>
#include <thread>

TEST(StartupLaunchControl, CreatorRolesDoNotIncludeGamesOrCriticalServices) {
    for (const auto& name : {"Steam.exe", "explorer.exe", "pwsh.exe", "vendor_launcher.exe"})
        EXPECT_TRUE(ce::startup_launch::IsLaunchHost(name));
    for (const auto& name : {"game.exe", "dwm.exe", "winlogon.exe", "captureengine.exe"})
        EXPECT_FALSE(ce::startup_launch::IsLaunchHost(name));
}

TEST(StartupLaunchControl, ChildrenAreClassifiedBeforeTheirCreationIsCommitted) {
    AppConfig config;
    config.gameWhitelist.push_back({"game.exe", "", MatchMode::kExact});
    config.overlayWhitelist.push_back({"overlay.exe", "", MatchMode::kExact});
    EXPECT_EQ(ce::startup_launch::ClassifyChild(config, "game.exe"), ce::startup_launch::ChildKind::GameTarget);
    EXPECT_EQ(ce::startup_launch::ClassifyChild(config, "overlay.exe"), ce::startup_launch::ChildKind::GameTarget);
    // A whitelisted target keeps its normal role even when its name would also
    // match the creation-only launcher set.
    config.gameWhitelist.push_back({"steam.exe", "", MatchMode::kExact});
    EXPECT_EQ(ce::startup_launch::ClassifyChild(config, "steam.exe"), ce::startup_launch::ChildKind::GameTarget);
    EXPECT_EQ(ce::startup_launch::ClassifyChild(config, "Steam.exe"), ce::startup_launch::ChildKind::GameTarget);
    EXPECT_EQ(ce::startup_launch::ClassifyChild(config, "vendor_launcher.exe"),
              ce::startup_launch::ChildKind::LaunchHost);
    // Everything else, anti-cheat titles and storefront tools alike, is never
    // touched: no gate, no forced suspension, no post-creation work.
    EXPECT_EQ(ce::startup_launch::ClassifyChild(config, "other.exe"), ce::startup_launch::ChildKind::Passthrough);
    EXPECT_EQ(ce::startup_launch::ClassifyChild(config, "protected_game.exe"),
              ce::startup_launch::ChildKind::Passthrough);
    EXPECT_EQ(ce::startup_launch::ClassifyChild(config, ""), ce::startup_launch::ChildKind::Passthrough);
}

TEST(StartupLaunchControl, OnlyProcessesThatCanLoadAndPatchTheCreationHookAreEligible) {
    EXPECT_TRUE(ce::startup_launch::CanHostCreationHook(GetCurrentProcess()));
    EXPECT_FALSE(ce::startup_launch::CanHostCreationHook(nullptr));
}

TEST(StartupLaunchControl, GateBlocksOtherThreadsAndReleasesDeterministically) {
    ce::HandleGuard mutex(CreateMutexW(nullptr, FALSE, nullptr));
    ASSERT_TRUE(mutex);
    bool acquired = true;
    {
        ce::startup_launch::Gate owner(mutex.get(), 0);
        ASSERT_TRUE(owner.Acquired());
        std::thread contender([&] { ce::startup_launch::Gate gate(mutex.get(), 0); acquired = gate.Acquired(); });
        contender.join();
        EXPECT_FALSE(acquired);
    }
    std::thread contender([&] { ce::startup_launch::Gate gate(mutex.get(), 0); acquired = gate.Acquired(); });
    contender.join();
    EXPECT_TRUE(acquired);
}

TEST(StartupLaunchControl, DiscoveryWaitsForTheActualCreatorsTransaction) {
    using ce::startup_launch::Object;
    const DWORD pid = GetCurrentProcessId();
    ce::HandleGuard role(CreateEventW(nullptr, TRUE, FALSE, ce::startup_launch::ObjectName(Object::Role, pid).c_str()));
    ce::HandleGuard ready(CreateEventW(nullptr, TRUE, TRUE, ce::startup_launch::ObjectName(Object::Ready, pid).c_str()));
    ce::HandleGuard mutex(CreateMutexW(nullptr, FALSE, ce::startup_launch::ObjectName(Object::Gate, pid).c_str()));
    ASSERT_TRUE(role && ready && mutex);
    const auto exe = ce::ansi_path::ModulePathW(nullptr);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    ASSERT_TRUE(CreateProcessW(exe.c_str(), nullptr, nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
                               nullptr, nullptr, &startup, &process));
    ce::HandleGuard child(process.hProcess);
    ce::HandleGuard primary(process.hThread);
    CE_SCOPE_EXIT({
        if (WaitForSingleObject(child.get(), 0) != WAIT_OBJECT_0) {
            TerminateProcess(child.get(), 0);
            WaitForSingleObject(child.get(), 5000);
        }
    });
    bool completed = true;
    {
        ce::startup_launch::Gate owner(mutex.get(), 0);
        ASSERT_TRUE(owner.Acquired());
        std::thread injector([&] { completed = ce::startup_launch::WaitForCreator(process.dwProcessId, 0); });
        injector.join();
        EXPECT_FALSE(completed);
    }
    std::thread injector([&] { completed = ce::startup_launch::WaitForCreator(process.dwProcessId, 0); });
    injector.join();
    EXPECT_TRUE(completed);
    EXPECT_TRUE(TerminateProcess(child.get(), 0));
    EXPECT_EQ(WaitForSingleObject(child.get(), 5000), WAIT_OBJECT_0);
}

TEST(StartupLaunchControl, RoleBootstrapPrecedesGraphicsAndCreatorWaitPrecedesRemoteInjection) {
    const auto dllmain = ce::test_source::ReadFile(ce::test_source::FindSource("hook", "main_dllmain.cpp"));
    ASSERT_FALSE(dllmain.empty());
    EXPECT_LT(dllmain.find("StartCreationOnlyLauncher(hinstDLL)"), dllmain.find("InstallCrashHandler()"));
    EXPECT_LT(dllmain.find("StartCreationOnlyLauncher(hinstDLL)"), dllmain.find("InitializeWrapperHooks()"));
    const auto injection = ce::test_source::ReadFile(ce::test_source::FindSource("captureengine", "injection_inject.cpp"));
    ASSERT_FALSE(injection.empty());
    EXPECT_LT(injection.find("WaitForCreator(pid)"), injection.find("injectCallback(pid, processName)"));
    EXPECT_LT(injection.find("WaitForCreator(pid)"), injection.find("CreateRemoteThread("));
    EXPECT_NE(injection.find("return !process.creationOnly"), std::string::npos);
    const auto child = ce::test_source::ReadFile(ce::test_source::FindSource("hook", "main_injection.cpp"));
    EXPECT_LT(child.find("WaitForCreator(GetProcessId(p->hProcess))"), child.find("CreateRemoteThread("));
    size_t start = 0;
    while ((start = child.find("ResumeThread(", start)) != std::string::npos) {
        const auto lineStart = child.rfind('\n', start);
        EXPECT_NE(child.substr(lineStart + 1, start - lineStart).find("if ("), std::string::npos);
        ++start;
    }
    EXPECT_NE(child.find("!(dwFlags & CREATE_SUSPENDED)"), std::string::npos);
}
