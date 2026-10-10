// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "common/platform/startup_imports.h"
#include "common/platform/ansi_path.h"
#include "common/platform/startup_launch_control.h"
#include "tests/flow/flow_test_support.h"

#include <filesystem>
#include <fstream>
#include <atomic>
#include <thread>

namespace {

using WriteMemory = decltype(&WriteProcessMemory);
WriteMemory g_WriteMemory = nullptr;
unsigned g_WriteCalls = 0;
bool g_RefuseRollback = false;

BOOL WINAPI PartialWrite(HANDLE process, void* address, const void* source, SIZE_T size, SIZE_T* written) {
    ++g_WriteCalls;
    if (g_WriteCalls == 3) {
        // The descriptor's Name field changes before the failed partial write.
        g_WriteMemory(process, address, source, std::min<SIZE_T>(size, 16), written);
        SetLastError(ERROR_PARTIAL_COPY);
        return FALSE;
    }
    if (g_RefuseRollback && g_WriteCalls == 4) {
        if (written) *written = 0;
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return g_WriteMemory(process, address, source, size, written);
}

struct WriteFault {
    uintptr_t* slot = nullptr;
    DWORD protection = 0;
    explicit WriteFault(bool refuseRollback) {
        g_WriteMemory = reinterpret_cast<WriteMemory>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "WriteProcessMemory"));
        g_WriteCalls = 0;
        g_RefuseRollback = refuseRollback;
        auto* base = reinterpret_cast<std::byte*>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        const auto* header = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base +
            header->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
        for (; descriptor->Name && !slot; ++descriptor) {
            auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
            for (; thunk->u1.Function; ++thunk) {
                if (thunk->u1.Function == reinterpret_cast<uintptr_t>(g_WriteMemory)) {
                    slot = reinterpret_cast<uintptr_t*>(&thunk->u1.Function);
                    break;
                }
            }
        }
        if (slot && VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection))
            *slot = reinterpret_cast<uintptr_t>(&PartialWrite);
        else slot = nullptr;
    }
    ~WriteFault() {
        if (slot) {
            *slot = reinterpret_cast<uintptr_t>(g_WriteMemory);
            DWORD unused = 0;
            VirtualProtect(slot, sizeof(*slot), protection, &unused);
        }
    }
};

struct Child {
    PROCESS_INFORMATION process{};
    HANDLE output = nullptr;
    ~Child() {
        if (process.hProcess) {
            if (WaitForSingleObject(process.hProcess, 0) != WAIT_OBJECT_0) {
                TerminateProcess(process.hProcess, 99);
                WaitForSingleObject(process.hProcess, 5000);
            }
            CloseHandle(process.hProcess);
        }
        if (process.hThread) CloseHandle(process.hThread);
        if (output) CloseHandle(output);
    }
};

struct LauncherBroker {
    ce::HandleGuard created{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::atomic<DWORD> pid{0};
    std::atomic<bool> injected{false};
    std::thread worker;
    using ObserverSetter = void (*)(void (*)(DWORD, void*), void*);
    ObserverSetter setter = nullptr;
    LauncherBroker(HMODULE hook, std::filesystem::path dll) {
        setter = reinterpret_cast<ObserverSetter>(GetProcAddress(hook, "CEFlow_SetCreationObserver"));
        if (!setter || !created) return;
        setter([](DWORD child, void* context) {
            auto* broker = static_cast<LauncherBroker*>(context);
            broker->pid.store(child, std::memory_order_release);
            SetEvent(broker->created.get());
        }, this);
        worker = std::thread([this, dll = std::move(dll)] {
            if (WaitForSingleObject(created.get(), 5000) != WAIT_OBJECT_0) return;
            const DWORD child = pid.load(std::memory_order_acquire);
            std::wstring requestedName;
            std::wstring roleName;
            std::wstring path;
            try {
                requestedName = ce::startup_launch::ObjectName(
                    ce::startup_launch::Object::ResumeRequested, child);
                roleName = ce::startup_launch::ObjectName(ce::startup_launch::Object::Role, child);
                path = dll.wstring();
            } catch (...) {
                injected.store(false, std::memory_order_release);
                return;
            }
            ce::HandleGuard requested(OpenEventW(SYNCHRONIZE, FALSE, requestedName.c_str()));
            if (!requested || WaitForSingleObject(requested.get(), 5000) != WAIT_OBJECT_0) return;
            ce::HandleGuard process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                                                PROCESS_CREATE_THREAD | SYNCHRONIZE, FALSE, child));
            ce::HandleGuard role(OpenEventW(EVENT_MODIFY_STATE, FALSE, roleName.c_str()));
            if (!process || !role) return;
            const size_t bytes = (path.size() + 1) * sizeof(wchar_t);
            void* remote = VirtualAllocEx(process.get(), nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!remote) return;
            if (!WriteProcessMemory(process.get(), remote, path.c_str(), bytes, nullptr)) {
                VirtualFreeEx(process.get(), remote, 0, MEM_RELEASE);
                return;
            }
            SetEvent(role.get());
            const auto load = reinterpret_cast<LPTHREAD_START_ROUTINE>(
                GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
            ce::HandleGuard thread(CreateRemoteThread(process.get(), nullptr, 0, load, remote, 0, nullptr));
            if (!thread) {
                VirtualFreeEx(process.get(), remote, 0, MEM_RELEASE);
                return;
            }
            if (WaitForSingleObject(thread.get(), 5000) == WAIT_OBJECT_0) {
                DWORD result = 0;
                injected.store(GetExitCodeThread(thread.get(), &result) && result != 0, std::memory_order_release);
                VirtualFreeEx(process.get(), remote, 0, MEM_RELEASE);
            }
        });
    }
    ~LauncherBroker() {
        if (worker.joinable()) worker.join();
        if (setter) setter(nullptr, nullptr);
    }
};

enum class Mode { Baseline, Manual, Creator, Suspended, Dormant, Upgrade, Ota, Unlisted, Profile, File, Rollback,
                  FatalRollback, Launcher, LauncherTimeout };

void RunProbe(Mode mode) {
    const bool manual = mode == Mode::Manual || mode == Mode::Rollback || mode == Mode::FatalRollback;
    const bool redirect = mode == Mode::Manual || mode == Mode::Creator || mode == Mode::Suspended ||
        mode == Mode::Profile || mode == Mode::File || mode == Mode::Launcher;
    const auto directory = std::filesystem::path(ce::ansi_path::ModulePathW(nullptr)).parent_path();
    const auto root = directory / "logs" / ce::flow::CurrentTestName();
    const auto vendor = root / "vendor";
    const auto overrideDirectory = root / "override";
    std::filesystem::create_directories(vendor);
    std::filesystem::create_directories(overrideDirectory);
    const auto exe = vendor / "static_import_probe.exe";
    const auto launcher = vendor / "startup_launcher_probe.exe";
    for (const auto& pair : {std::pair(directory / "static_import_probe.exe", exe),
                             std::pair(directory / "sl.interposer.dll", vendor / "sl.interposer.dll"),
                             std::pair(directory / "sl.interposer.dll", overrideDirectory / "sl.interposer.dll")})
        std::filesystem::copy_file(pair.first, pair.second, std::filesystem::copy_options::overwrite_existing);
    std::unique_ptr<LauncherBroker> broker;
    CE_SCOPE_EXIT(SetEnvironmentVariableA("CE_FLOW_CREATION_CONFIG", nullptr));
    if (mode != Mode::Baseline && !manual) {
        const auto configPath = root / "creation.ini";
        bool exact = false;
        const auto overridePath = ce::ansi_path::CompatiblePath(
            (mode == Mode::File ? overrideDirectory / "sl.interposer.dll" : overrideDirectory).wstring(), &exact);
        ASSERT_TRUE(exact);
        {
            std::ofstream config(configPath);
            config << "[Injection]\nwhitelist=" << (mode == Mode::Unlisted ? "other.exe" : "static_import_probe.exe")
                   << "\n[Graphics]\nstreamline_dll_path=" << (mode == Mode::Profile ? "" : overridePath)
                   << "\nstreamline_upgrade=" << (mode == Mode::Upgrade ? "true" : "false")
                   << "\nngx_ota=" << (mode == Mode::Ota ? "on" : "default") << "\n";
            if (mode == Mode::Profile)
                config << "[Profile.Startup]\nProcess=static_import_probe.exe\ndll_injection=always\nstreamline_dll_path="
                       << overridePath << "\n";
        }
        HMODULE hook = LoadLibraryW((directory / "capture_hook_x64.dll").c_str());
        ASSERT_NE(hook, nullptr);
        const auto configure = reinterpret_cast<bool (*)(const char*, bool)>(GetProcAddress(hook, "CEFlow_ConfigureCreation"));
        ASSERT_NE(configure, nullptr);
        if (mode == Mode::LauncherTimeout) {
            // The handshake is bounded; a missed one must degrade, not stall.
            const auto timeout = reinterpret_cast<void (*)(DWORD)>(GetProcAddress(hook, "CEFlow_SetHandshakeTimeout"));
            ASSERT_NE(timeout, nullptr);
            timeout(100);
        }
        const auto path = ce::ansi_path::CompatiblePath(configPath.wstring(), &exact);
        ASSERT_TRUE(exact);
        ASSERT_TRUE(configure(path.c_str(), mode != Mode::Dormant));
        PROCESS_INFORMATION invalid{};
        STARTUPINFOW invalidStartup{};
        invalidStartup.cb = sizeof(invalidStartup);
        EXPECT_FALSE(CreateProcessW(L"Z:\\CE_nonexistent_startup_probe.exe", nullptr, nullptr, nullptr, FALSE,
                                    0, nullptr, nullptr, &invalidStartup, &invalid));
        EXPECT_EQ(GetLastError(), ERROR_FILE_NOT_FOUND);
        if (mode == Mode::Launcher || mode == Mode::LauncherTimeout)
            std::filesystem::copy_file(directory / "startup_launcher_probe.exe", launcher,
                                       std::filesystem::copy_options::overwrite_existing);
        if (mode == Mode::Launcher) {
            // LauncherTimeout deliberately runs without the broker: nothing ever
            // sets the launcher's Ready event, so the handshake must miss.
            ASSERT_TRUE(SetEnvironmentVariableA("CE_FLOW_CREATION_CONFIG", path.c_str()));
            broker = std::make_unique<LauncherBroker>(hook, directory / "capture_hook_x64.dll");
            ASSERT_NE(broker->setter, nullptr);
        }
    }
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Child child;
    HANDLE write = nullptr;
    ASSERT_TRUE(CreatePipe(&child.output, &write, &security, 0));
    ASSERT_TRUE(SetHandleInformation(child.output, HANDLE_FLAG_INHERIT, 0));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write;
    startup.hStdError = write;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    const bool suspended = manual || mode == Mode::Baseline || mode == Mode::Suspended;
    const BOOL created = CreateProcessW((mode == Mode::Launcher || mode == Mode::LauncherTimeout ? launcher : exe).c_str(),
                                        nullptr, nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | (suspended ? CREATE_SUSPENDED : 0),
                                        nullptr, vendor.c_str(), &startup, &child.process);
    CloseHandle(write);
    ASSERT_TRUE(created) << GetLastError();
    if (manual) {
        bool exact = false;
        const auto path = ce::ansi_path::CompatiblePath((overrideDirectory / "sl.interposer.dll").wstring(), &exact);
        ASSERT_TRUE(exact);
        if (mode == Mode::Manual) {
            const auto result = ce::startup_imports::Redirect(child.process.hProcess, "sl.interposer.dll", path);
            ASSERT_EQ(result.status, ce::startup_imports::Status::Redirected) << result.error;
            EXPECT_EQ(result.redirected, 1u);
        } else {
            WriteFault fault(mode == Mode::FatalRollback);
            ASSERT_NE(fault.slot, nullptr);
            const auto result = ce::startup_imports::Redirect(child.process.hProcess, "sl.interposer.dll", path);
            EXPECT_EQ(result.status, mode == Mode::FatalRollback ? ce::startup_imports::Status::RollbackFailed :
                                                                   ce::startup_imports::Status::Failed);
            EXPECT_EQ(result.error, ERROR_PARTIAL_COPY);
            EXPECT_EQ(result.redirected, 0u);
            if (mode == Mode::FatalRollback) return;  // Child destructor terminates; corrupted imports never execute.
        }
    }
    if (suspended) {
        EXPECT_EQ(WaitForSingleObject(child.process.hProcess, 0), WAIT_TIMEOUT);
        ASSERT_EQ(ResumeThread(child.process.hThread), 1u);
    }
    ASSERT_EQ(WaitForSingleObject(child.process.hProcess, 5000), WAIT_OBJECT_0);
    DWORD exitCode = 0;
    ASSERT_TRUE(GetExitCodeProcess(child.process.hProcess, &exitCode));
    ASSERT_EQ(exitCode, 0u);
    if (broker) EXPECT_TRUE(broker->injected.load(std::memory_order_acquire));
    char output[4096]{};
    DWORD read = 0;
    ASSERT_TRUE(ReadFile(child.output, output, sizeof(output) - 1, &read, nullptr));
    const std::string report(output, read);
    ASSERT_TRUE(report.starts_with("1\r\n") || report.starts_with("1\n")) << report;
    const auto first = report.find('\n') + 1;
    const auto end = report.find_first_of("\r\n", first);
    const auto pathText = report.substr(first, end - first);
    const auto loaded = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(pathText.data()), pathText.size()));
    EXPECT_TRUE(std::filesystem::equivalent(loaded, (redirect ? overrideDirectory : vendor) / "sl.interposer.dll"));
    if (mode == Mode::Unlisted) {
        // The unlisted child must have taken the caller's creation path exactly
        // as requested: classified as passthrough and never suspended by the hook.
        using PolicyGetter = bool (*)(DWORD*, char*, size_t, int*, int*);
        HMODULE hook = LoadLibraryW((directory / "capture_hook_x64.dll").c_str());
        ASSERT_NE(hook, nullptr);
        const auto policy = reinterpret_cast<PolicyGetter>(GetProcAddress(hook, "CEFlow_GetLastCreationPolicy"));
        ASSERT_NE(policy, nullptr);
        DWORD pid = 0;
        char name[64]{};
        int kind = -1;
        int forcedSuspension = -1;
        ASSERT_TRUE(policy(&pid, name, sizeof(name), &kind, &forcedSuspension));
        EXPECT_EQ(pid, child.process.dwProcessId);
        EXPECT_STREQ(name, "static_import_probe.exe");
        EXPECT_EQ(kind, 0);    // ChildKind::Passthrough
        EXPECT_EQ(forcedSuspension, 0);
    }
}

}  // namespace

TEST(FlowStartupImports, StaticImportBaselineLoadsGameDirectory) { RunProbe(Mode::Baseline); }
TEST(FlowStartupImports, StaticImportRedirectLoadsOnlyOverrideInterposer) { RunProbe(Mode::Manual); }
TEST(FlowStartupImports, OrdinaryLaunchRedirectsBeforeWindowsImports) { RunProbe(Mode::Creator); }
TEST(FlowStartupImports, CallerRequestedSuspensionIsPreserved) { RunProbe(Mode::Suspended); }
TEST(FlowStartupImports, HostUnavailableIsExactPassThrough) { RunProbe(Mode::Dormant); }
TEST(FlowStartupImports, LegacyUpgradeRetainsExistingBridgeContract) { RunProbe(Mode::Upgrade); }
TEST(FlowStartupImports, ExplicitOtaOnRetainsDriverRuntimeContract) { RunProbe(Mode::Ota); }
TEST(FlowStartupImports, UnlistedChildIsUnchanged) { RunProbe(Mode::Unlisted); }
TEST(FlowStartupImports, ChildProfileSelectsOverrideDirectory) { RunProbe(Mode::Profile); }
TEST(FlowStartupImports, ConfiguredDllFileRetainsPathContract) { RunProbe(Mode::File); }
TEST(FlowStartupImports, PartialDescriptorWriteRollsBackToGameInterposer) { RunProbe(Mode::Rollback); }
TEST(FlowStartupImports, UnrecoverableWriteIsReportedAndChildNeverRuns) { RunProbe(Mode::FatalRollback); }
TEST(FlowStartupImports, NewlyCreatedLauncherIsReadyBeforeItsGameLaunch) { RunProbe(Mode::Launcher); }
TEST(FlowStartupImports, MissedLauncherHandshakeResumesInsteadOfStoppingIt) { RunProbe(Mode::LauncherTimeout); }

TEST(FlowStartupImports, CreationHooksDetachCompletelyWhenTheInjectorReleasesThem) {
    const auto directory = std::filesystem::path(ce::ansi_path::ModulePathW(nullptr)).parent_path();
    const auto root = directory / "logs" / ce::flow::CurrentTestName();
    std::filesystem::create_directories(root);
    const auto configPath = root / "creation.ini";
    {
        std::ofstream config(configPath);
        config << "[Injection]\nwhitelist=other.exe\n";
    }
    HMODULE hook = LoadLibraryW((directory / "capture_hook_x64.dll").c_str());
    ASSERT_NE(hook, nullptr);
    const auto configure =
        reinterpret_cast<bool (*)(const char*, bool)>(GetProcAddress(hook, "CEFlow_ConfigureCreation"));
    const auto disable = reinterpret_cast<bool (*)()>(GetProcAddress(hook, "CEFlow_DisableCreation"));
    const auto state = reinterpret_cast<int (*)()>(GetProcAddress(hook, "CEFlow_GetCreationHookState"));
    const auto policy =
        reinterpret_cast<bool (*)(DWORD*, char*, size_t, int*, int*)>(GetProcAddress(hook, "CEFlow_GetLastCreationPolicy"));
    ASSERT_NE(configure, nullptr);
    ASSERT_NE(disable, nullptr);
    ASSERT_NE(state, nullptr);
    ASSERT_NE(policy, nullptr);
    bool exact = false;
    const auto path = ce::ansi_path::CompatiblePath(configPath.wstring(), &exact);
    ASSERT_TRUE(exact);
    ASSERT_TRUE(configure(path.c_str(), true));
    EXPECT_EQ(state(), 3);

    const auto createChild = [&directory](DWORD& pid) {
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW((directory / "static_import_probe.exe").c_str(), nullptr, nullptr, nullptr, FALSE,
                            CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &process))
            return false;
        pid = process.dwProcessId;
        CloseHandle(process.hThread);
        TerminateProcess(process.hProcess, 0);
        WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hProcess);
        return true;
    };
    DWORD first = 0;
    ASSERT_TRUE(createChild(first));
    DWORD recordedPid = 0;
    char name[64]{};
    int kind = -1;
    int forcedSuspension = -1;
    ASSERT_TRUE(policy(&recordedPid, name, sizeof(name), &kind, &forcedSuspension));
    EXPECT_EQ(recordedPid, first);  // the live creation hook recorded this creation

    ASSERT_TRUE(disable());
    EXPECT_EQ(state(), 0);  // neither entry patch belongs to CE any more

    DWORD second = 0;
    ASSERT_TRUE(createChild(second));
    ASSERT_TRUE(policy(&recordedPid, name, sizeof(name), &kind, &forcedSuspension));
    EXPECT_EQ(recordedPid, first);  // unchanged: no hook runs in this process any more
}
