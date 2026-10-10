// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "main_startup_launcher.h"

#include "common/config/config.h"
#include "common/ipc/elevation_windows.h"
#include "common/ipc/shared_defs.h"
#include "common/platform/ansi_path.h"
#include "common/platform/startup_imports.h"
#include "common/platform/startup_launch_control.h"
#include "hook/hooking/inline_hook.h"
#include "hook/runtime/dll_utils.h"
#include "hook/runtime/hook_common.h"
#include "runtime_override_paths.h"

#include <atomic>
#include <algorithm>
#include <filesystem>
#include <mutex>
#include <memory>
#include <vector>
#include <winternl.h>

namespace {

// The Vista+ native creation boundary shared by the Win32 creation APIs. The
// opaque creation/attribute structures are forwarded unchanged; no private
// structure offsets, PEB edits or loader-graph manipulation are involved.
using CreateUserProcess = NTSTATUS(NTAPI*)(PHANDLE, PHANDLE, ACCESS_MASK, ACCESS_MASK,
                                          POBJECT_ATTRIBUTES, POBJECT_ATTRIBUTES, ULONG, ULONG,
                                          PRTL_USER_PROCESS_PARAMETERS, void*, void*);
using ResumeNativeThread = NTSTATUS(NTAPI*)(HANDLE, PULONG);
constexpr ULONG kThreadCreateSuspended = 1;
std::atomic<CreateUserProcess> g_CreateUserProcess{nullptr};
std::atomic<ResumeNativeThread> g_ResumeNativeThread{nullptr};
HMODULE g_LaunchModule = nullptr;
HANDLE g_LaunchRole = nullptr;
HANDLE g_LaunchReady = nullptr;
HANDLE g_LaunchGate = nullptr;
HANDLE g_LaunchActive = nullptr;

struct PendingLauncher {
    DWORD threadId = 0;
    std::string name;
    ce::HandleGuard process;
    ce::HandleGuard role;
    ce::HandleGuard ready;
    ce::HandleGuard active;
    ce::HandleGuard requested;
};
std::mutex g_PendingMutex;
std::vector<std::unique_ptr<PendingLauncher>> g_PendingLaunchers;

std::atomic<DWORD> g_HandshakeTimeoutMs{5000};
#ifdef CE_FLOW_TEST
std::string g_TestConfigPath;
std::atomic<bool> g_TestActive{false};
void (*g_TestCreationObserver)(DWORD, void*) = nullptr;
void* g_TestCreationContext = nullptr;
std::mutex g_PolicyMutex;
struct TestPolicyRecord {
    bool valid = false;
    DWORD pid = 0;
    int kind = -1;
    int forcedSuspension = 0;
    char name[64]{};
} g_PolicyRecord;

void NoteCreationPolicy(const std::string& name, ce::startup_launch::ChildKind kind, bool forcedSuspension, DWORD pid) {
    std::lock_guard<std::mutex> lock(g_PolicyMutex);
    g_PolicyRecord.valid = true;
    g_PolicyRecord.pid = pid;
    g_PolicyRecord.kind = static_cast<int>(kind);
    g_PolicyRecord.forcedSuspension = forcedSuspension ? 1 : 0;
    snprintf(g_PolicyRecord.name, sizeof(g_PolicyRecord.name), "%s", name.c_str());
}
#else
inline void NoteCreationPolicy(const std::string&, ce::startup_launch::ChildKind, bool, DWORD) {}
#endif

DWORD CurrentHostPid() {
    ce::HandleGuard mapping(OpenHostDiscoveryMapping());
    if (!mapping) return 0;
    auto* discovery = static_cast<DiscoveryInfo*>(MapViewOfFile(mapping.get(), FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo)));
    const DWORD pid = ValidateDiscoveryInfo(discovery) ? discovery->GetInjectPid() : 0;
    if (discovery) UnmapViewOfFile(discovery);
    return pid;
}

bool HostAvailable() {
#ifdef CE_FLOW_TEST
    if (!g_TestConfigPath.empty()) return g_TestActive.load(std::memory_order_acquire);
#endif
    if (g_LaunchRole && (!g_LaunchActive || WaitForSingleObject(g_LaunchActive, 0) != WAIT_OBJECT_0)) return false;
    const DWORD pid = CurrentHostPid();
    ce::HandleGuard host(pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr);
    return host && WaitForSingleObject(host.get(), 0) == WAIT_TIMEOUT;
}

bool Whitelisted(const AppConfig& config, const std::string& name) {
    for (const auto& entry : config.gameWhitelist) if (MatchesProcessName(entry, name)) return true;
    for (const auto& entry : config.overlayWhitelist) if (MatchesProcessName(entry, name)) return true;
    return false;
}

// The child image name from the caller's own creation parameters, before the
// kernel commits the process. The parameters block lives in this (the creating)
// process, so its buffers are directly readable; an empty result sends the
// creation through the post-creation classification instead.
std::string ChildImageName(const void* processParameters) {
    const auto* parameters = static_cast<const RTL_USER_PROCESS_PARAMETERS*>(processParameters);
    if (!parameters || !parameters->ImagePathName.Buffer || !parameters->ImagePathName.Length) return {};
    const std::wstring image(parameters->ImagePathName.Buffer,
                             parameters->ImagePathName.Length / sizeof(wchar_t));
    std::string name;
    if (!ce::ansi_path::TryNarrowAcpExactly(std::filesystem::path(image).filename().wstring(), &name)) return {};
    return name;
}

// The INI of the current host's own installation; empty when its path cannot be
// represented losslessly. Shared by the pre-creation classification and the
// post-creation transaction so the two can never read different config.
std::string HostConfigPath() {
    auto directory = std::filesystem::path(ce::ansi_path::ModulePathW(g_LaunchModule)).parent_path();
    ce::HandleGuard host(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, CurrentHostPid()));
    std::wstring hostImage(32768, L'\0');
    DWORD hostLength = static_cast<DWORD>(hostImage.size());
    if (host && QueryFullProcessImageNameW(host.get(), 0, hostImage.data(), &hostLength)) {
        hostImage.resize(hostLength);
        directory = std::filesystem::path(hostImage).parent_path();
    }
    bool exact = false;
    auto configPath = ce::ansi_path::CompatiblePath((directory / "config.ini").wstring(), &exact);
#ifdef CE_FLOW_TEST
    if (!g_TestConfigPath.empty()) { configPath = g_TestConfigPath; exact = true; }
#endif
    return exact ? configPath : std::string{};
}

AppConfig LoadHostConfig(const std::string& name) {
    AppConfig config;
    const auto configPath = HostConfigPath();
    if (!configPath.empty()) LoadConfig(configPath, config, name);
    return config;
}

void PrepareLauncher(HANDLE process, HANDLE thread) {
    if (!g_ResumeNativeThread.load(std::memory_order_acquire)) return;
    std::wstring image(32768, L'\0');
    DWORD length = static_cast<DWORD>(image.size());
    if (!QueryFullProcessImageNameW(process, 0, image.data(), &length)) return;
    image.resize(length);
    std::string name;
    if (!ce::ansi_path::TryNarrowAcpExactly(std::filesystem::path(image).filename().wstring(), &name)) return;
    if (!ce::startup_launch::IsLaunchHost(name) || !ce::startup_launch::CanHostCreationHook(process)) return;
    const auto owner = ce::elevation::ProcessUserSid(GetCurrentProcess());
    if (owner.empty() || ce::elevation::ProcessUserSid(process) != owner) return;
    auto pending = std::make_unique<PendingLauncher>();
    pending->threadId = GetThreadId(thread);
    pending->name = name;
    const DWORD pid = GetProcessId(process);
    if (!pid || !pending->threadId ||
        !DuplicateHandle(GetCurrentProcess(), process, GetCurrentProcess(), pending->process.addressof(),
                          0, FALSE, DUPLICATE_SAME_ACCESS)) return;
    ce::elevation::Security security(L"D:P(A;;GA;;;SY)(A;;GA;;;" + owner + L")S:(ML;;NW;;;ME)");
    if (!security.Get()) return;
    const auto event = [&](ce::startup_launch::Object object, bool signaled) {
        return CreateEventW(security.Get(), TRUE, signaled,
                             ce::startup_launch::ObjectName(object, pid).c_str());
    };
    pending->role.reset(event(ce::startup_launch::Object::Role, false));
    pending->ready.reset(event(ce::startup_launch::Object::Ready, false));
    pending->active.reset(event(ce::startup_launch::Object::Active, true));
    pending->requested.reset(event(ce::startup_launch::Object::ResumeRequested, false));
    if (!pending->role || !pending->ready || !pending->active || !pending->requested) return;
    std::lock_guard<std::mutex> lock(g_PendingMutex);
    std::erase_if(g_PendingLaunchers, [](const auto& entry) {
        return WaitForSingleObject(entry->process.get(), 0) == WAIT_OBJECT_0;
    });
    g_PendingLaunchers.push_back(std::move(pending));
    EarlyLog("[StartupImport] New launcher %s PID=%lu will wait for creation-hook readiness before its first resume", name.c_str(), pid);
#ifdef CE_FLOW_TEST
    if (g_TestCreationObserver) g_TestCreationObserver(pid, g_TestCreationContext);
#endif
}

NTSTATUS NTAPI HookResumeNativeThread(HANDLE thread, PULONG previousCount) {
    const auto original = g_ResumeNativeThread.load(std::memory_order_acquire);
    if (!original) return static_cast<NTSTATUS>(0xc0000002u);
    const DWORD error = GetLastError();
    if (HookIsShuttingDown()) {
        SetLastError(error);
        return original(thread, previousCount);
    }
    // Patching transactions resume peer threads of this process. They must
    // never touch a mutex that one of those suspended peers could own.
    const DWORD processId = GetProcessIdOfThread(thread);
    const DWORD threadId = processId && processId != GetCurrentProcessId() ? GetThreadId(thread) : 0;
    std::unique_ptr<PendingLauncher> pending;
    if (threadId) {
        std::lock_guard<std::mutex> lock(g_PendingMutex);
        auto found = std::find_if(g_PendingLaunchers.begin(), g_PendingLaunchers.end(),
                                  [threadId](const auto& entry) { return entry->threadId == threadId; });
        if (found != g_PendingLaunchers.end()) {
            pending = std::move(*found);
            g_PendingLaunchers.erase(found);
        }
    }
    if (pending && !HookIsShuttingDown() && HostAvailable()) {
        SetEvent(pending->requested.get());
        wchar_t stoppingName[64]{};
        GenerateInjectHostStoppingEventName(stoppingName, _countof(stoppingName));
        ce::HandleGuard stopping(OpenEventW(SYNCHRONIZE, FALSE, stoppingName));
        ce::HandleGuard host(OpenProcess(SYNCHRONIZE, FALSE, CurrentHostPid()));
        HANDLE waits[] = {pending->ready.get(), pending->process.get(), nullptr, nullptr};
        DWORD count = 2;
        if (host) waits[count++] = host.get();
        if (stopping) waits[count++] = stopping.get();
        const DWORD wait = WaitForMultipleObjects(count, waits, FALSE,
                                                  g_HandshakeTimeoutMs.load(std::memory_order_relaxed));
        const bool hostStopped = wait >= WAIT_OBJECT_0 + 2 && wait < WAIT_OBJECT_0 + count;
        if (wait != WAIT_OBJECT_0 && !hostStopped && HostAvailable()) {
            // The handshake protects the launcher's game child, never the
            // launcher itself (PrepareLauncher registers non-targets only).
            // Missing it degrades that game child to the ordinary discovery
            // path; stopping the launcher would punish software that was never
            // an interception target.
            EarlyLog("[StartupImport] New launcher %s PID=%lu resumed unintercepted (wait=%lu)",
                     pending->name.c_str(), processId, wait);
        }
    }
    SetLastError(error);
    return original(thread, previousCount);
}

WORD FileMachine(const std::filesystem::path& path, bool requireDll = false) {
    ce::HandleGuard file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    IMAGE_DOS_HEADER dos{};
    DWORD bytes = 0;
    if (!file || !ReadFile(file.get(), &dos, sizeof(dos), &bytes, nullptr) || bytes != sizeof(dos) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < static_cast<LONG>(sizeof(dos)) || dos.e_lfanew > 1024 * 1024) return 0;
    LARGE_INTEGER offset{};
    offset.QuadPart = dos.e_lfanew;
    struct Prefix { DWORD signature; IMAGE_FILE_HEADER file; } header{};
    if (!SetFilePointerEx(file.get(), offset, nullptr, FILE_BEGIN) ||
        !ReadFile(file.get(), &header, sizeof(header), &bytes, nullptr) || bytes != sizeof(header) ||
        header.signature != IMAGE_NT_SIGNATURE || (requireDll && !(header.file.Characteristics & IMAGE_FILE_DLL))) return 0;
    return header.file.Machine;
}

bool RedirectChild(HANDLE process, bool& gameTarget) {
    std::wstring image(32768, L'\0');
    DWORD length = static_cast<DWORD>(image.size());
    if (!QueryFullProcessImageNameW(process, 0, image.data(), &length)) return true;
    image.resize(length);
    const auto executable = std::filesystem::path(image);
    std::string name;
    if (!ce::ansi_path::TryNarrowAcpExactly(executable.filename().wstring(), &name)) return true;
    bool exact = false;
    const auto configPath = HostConfigPath();
    if (configPath.empty()) return true;
    AppConfig config;
    LoadConfig(configPath, config, name);
    gameTarget = Whitelisted(config, name);
    if (!gameTarget || config.graphics.streamlineDllPath.empty() || config.graphics.streamlineUpgrade ||
        ParseNgxOtaMode(config.graphics.ngxOta) == kNgxOtaModeOn)
        return true;
    const auto replacement = std::filesystem::path(DllPathToWide(
        BuildOverridePath(config.graphics.streamlineDllPath, "sl.interposer.dll").c_str()));
    const auto original = executable.parent_path() / L"sl.interposer.dll";
    uint32_t originalMajor = 0;
    uint32_t replacementMajor = 0;
    VersionResourceFileVersionParts(DllReadVersionResourceW(original.c_str()), &originalMajor, nullptr, nullptr);
    VersionResourceFileVersionParts(DllReadVersionResourceW(replacement.c_str()), &replacementMajor, nullptr, nullptr);
    const WORD machine = FileMachine(executable);
    // Same-generation override only. The opt-in 1.x->2.x bridge retains its
    // existing two-runtime contract, and mismatched architectures never start.
    if ((originalMajor != 1 && originalMajor != 2) || originalMajor != replacementMajor ||
        !machine || FileMachine(replacement, true) != machine) {
        EarlyLog("[StartupImport] Refused incompatible interposer for %s (originalMajor=%u replacementMajor=%u machine=%04X)",
                 name.c_str(), originalMajor, replacementMajor, machine);
        return true;
    }
    const auto path = ce::ansi_path::CompatiblePath(std::filesystem::absolute(replacement).wstring(), &exact);
    if (!exact) {
        EarlyLog("[StartupImport] Override path cannot be represented losslessly for %s", name.c_str());
        return true;
    }
    const auto result = ce::startup_imports::Redirect(process, "sl.interposer.dll", path);
    if (result.status != ce::startup_imports::Status::Unchanged) {
        EarlyLog("[StartupImport] Child %s PID=%lu status=%u redirected=%u error=%lu", name.c_str(),
                 static_cast<unsigned long>(GetProcessId(process)), static_cast<unsigned>(result.status),
                 result.redirected, static_cast<unsigned long>(result.error));
    }
    return result.status != ce::startup_imports::Status::RollbackFailed;
}

NTSTATUS NTAPI HookCreateUserProcess(PHANDLE process, PHANDLE thread, ACCESS_MASK processAccess,
                                     ACCESS_MASK threadAccess, POBJECT_ATTRIBUTES processAttributes,
                                     POBJECT_ATTRIBUTES threadAttributes, ULONG processFlags, ULONG threadFlags,
                                     PRTL_USER_PROCESS_PARAMETERS parameters, void* createInfo, void* attributes) {
    const auto original = g_CreateUserProcess.load(std::memory_order_acquire);
    if (!original) return static_cast<NTSTATUS>(0xc0000002u);
    const DWORD incomingError = GetLastError();
    if (HookIsShuttingDown() || !HostAvailable()) {
        SetLastError(incomingError);
        return original(process, thread, processAccess, threadAccess, processAttributes, threadAttributes,
                        processFlags, threadFlags, parameters, createInfo, attributes);
    }
    ce::startup_launch::Gate gate(g_LaunchGate);
    if (!gate.Acquired()) {
        // Nothing has been written to the child yet, so a creation without
        // interception is always safe; failing the caller's process start would
        // punish software that merely shares a creator with a target.
        EarlyLog("[StartupImport] Creation gate unavailable; creating without interception");
        SetLastError(incomingError);
        return original(process, thread, processAccess, threadAccess, processAttributes, threadAttributes,
                        processFlags, threadFlags, parameters, createInfo, attributes);
    }
    // Classify before the kernel commits anything: software that is neither a
    // target nor a launch host must get the caller's creation path exactly as
    // requested, with no forced suspension and no post-creation work. The
    // classification stays under the gate because it reads the same config the
    // post-creation transaction will.
    const std::string childName = ChildImageName(parameters);
    ce::startup_launch::ChildKind kind = ce::startup_launch::ChildKind::GameTarget;
    if (!childName.empty()) kind = ce::startup_launch::ClassifyChild(LoadHostConfig(childName), childName);
    if (kind == ce::startup_launch::ChildKind::Passthrough) {
        gate.Release();
        SetLastError(incomingError);
        const NTSTATUS passThrough = original(process, thread, processAccess, threadAccess, processAttributes,
                                              threadAttributes, processFlags, threadFlags, parameters, createInfo,
                                              attributes);
        if (passThrough >= 0 && process && *process)
            NoteCreationPolicy(childName, kind, false, GetProcessId(*process));
        return passThrough;
    }
    SetLastError(incomingError);
    const NTSTATUS status = original(process, thread, processAccess, threadAccess, processAttributes, threadAttributes,
                                     processFlags, threadFlags | kThreadCreateSuspended, parameters, createInfo, attributes);
    const DWORD error = GetLastError();
    if (status >= 0 && process && thread && *process && *thread) {
        bool safe = true;
        bool gameTarget = true;
        try {
            safe = RedirectChild(*process, gameTarget);
            if (safe && !gameTarget) PrepareLauncher(*process, *thread);
        } catch (...) {
            // Allocation/config errors before the transaction leave the child
            // intact. Redirect itself performs a complete rollback on failure.
            EarlyLog("[StartupImport] Child policy evaluation failed (PID=%lu)",
                     static_cast<unsigned long>(GetProcessId(*process)));
        }
        NoteCreationPolicy(childName,
                           gameTarget ? ce::startup_launch::ChildKind::GameTarget
                                      : ce::startup_launch::ChildKind::LaunchHost,
                           true, GetProcessId(*process));
        gate.Release();
        if (!safe || (!(threadFlags & kThreadCreateSuspended) && ResumeThread(*thread) == MAXDWORD)) {
            TerminateProcess(*process, ERROR_BAD_EXE_FORMAT);
            // Kernel creation already committed its opaque success records.
            // Preserve that result and handle ownership for the Win32 caller's
            // normal cleanup; never fabricate an inconsistent native failure.
            EarlyLog("[StartupImport] Child terminated before execution (PID=%lu unsafeRollback=%d)",
                     static_cast<unsigned long>(GetProcessId(*process)), !safe);
        }
    }
    SetLastError(error);
    return status;
}

void PublishCreate(void* trampoline, void*) {
    g_CreateUserProcess.store(reinterpret_cast<CreateUserProcess>(trampoline), std::memory_order_release);
}

void PublishResume(void* trampoline, void*) {
    g_ResumeNativeThread.store(reinterpret_cast<ResumeNativeThread>(trampoline), std::memory_order_release);
}

DWORD WINAPI CreationThread(void*) {
    // Host activation is an explicit event, including after replacement-host
    // adoption. A failed patch transaction is retried only on a new request.
    while (WaitForSingleObject(g_LaunchRole, INFINITE) == WAIT_OBJECT_0) {
        ResetEvent(g_LaunchRole);
        InstallStartupCreationHook();
    }
    return 0;
}

}  // namespace

bool InstallStartupCreationHook() {
    const DWORD pid = GetCurrentProcessId();
    if (!g_LaunchModule)
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&InstallStartupCreationHook), &g_LaunchModule);
    if (!g_LaunchGate)
        g_LaunchGate = CreateMutexW(nullptr, FALSE, ce::startup_launch::ObjectName(ce::startup_launch::Object::Gate, pid).c_str());
    if (!g_LaunchReady)
        g_LaunchReady = CreateEventW(nullptr, TRUE, FALSE, ce::startup_launch::ObjectName(ce::startup_launch::Object::Ready, pid).c_str());
    if (!g_LaunchGate || !g_LaunchReady) return false;
    auto* ntdll = GetModuleHandleW(L"ntdll.dll");
    void* createTrampoline = nullptr;
    void* resumeTrampoline = nullptr;
    InlineHook::PublishedHookSpec hooks[2]{};
    size_t count = 0;
    if (!g_ResumeNativeThread.load(std::memory_order_acquire))
        hooks[count++] = {reinterpret_cast<void*>(GetProcAddress(ntdll, "NtResumeThread")),
                          reinterpret_cast<void*>(&HookResumeNativeThread), &resumeTrampoline, PublishResume};
    if (!g_CreateUserProcess.load(std::memory_order_acquire))
        hooks[count++] = {reinterpret_cast<void*>(GetProcAddress(ntdll, "NtCreateUserProcess")),
                          reinterpret_cast<void*>(&HookCreateUserProcess), &createTrampoline, PublishCreate};
    if (count) InlineHook::InstallPublishedBatch(hooks, count);
    const bool installed = g_CreateUserProcess.load(std::memory_order_acquire) &&
        g_ResumeNativeThread.load(std::memory_order_acquire);
    if (installed) SetEvent(g_LaunchReady);
    EarlyLog("[StartupImport] Creation-only hook PID=%lu installed=%d", static_cast<unsigned long>(pid), installed);
    return installed;
}

bool StartCreationOnlyLauncher(HMODULE module) {
    g_LaunchRole = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE,
                              ce::startup_launch::ObjectName(ce::startup_launch::Object::Role, GetCurrentProcessId()).c_str());
    if (!g_LaunchRole) return false;
    g_LaunchModule = module;
#ifdef CE_FLOW_TEST
    char testConfig[32768]{};
    if (GetEnvironmentVariableA("CE_FLOW_CREATION_CONFIG", testConfig, sizeof(testConfig))) {
        g_TestConfigPath = testConfig;
        g_TestActive.store(true, std::memory_order_release);
    }
#endif
    g_LaunchActive = OpenEventW(SYNCHRONIZE, FALSE,
        ce::startup_launch::ObjectName(ce::startup_launch::Object::Active, GetCurrentProcessId()).c_str());
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(module), &pinned);
    if (HANDLE worker = CreateThread(nullptr, 0, CreationThread, nullptr, 0, nullptr)) CloseHandle(worker);
    return true;
}

#ifdef CE_FLOW_TEST
extern "C" __declspec(dllexport) void CEFlow_SetHandshakeTimeout(DWORD timeoutMs) {
    g_HandshakeTimeoutMs.store(timeoutMs, std::memory_order_release);
}

extern "C" __declspec(dllexport) bool CEFlow_GetLastCreationPolicy(DWORD* pid, char* name, size_t nameSize,
                                                                   int* kind, int* forcedSuspension) {
    std::lock_guard<std::mutex> lock(g_PolicyMutex);
    if (!g_PolicyRecord.valid) return false;
    if (pid) *pid = g_PolicyRecord.pid;
    if (kind) *kind = g_PolicyRecord.kind;
    if (forcedSuspension) *forcedSuspension = g_PolicyRecord.forcedSuspension;
    if (name && nameSize) snprintf(name, nameSize, "%s", g_PolicyRecord.name);
    return true;
}

extern "C" __declspec(dllexport) bool CEFlow_ConfigureCreation(const char* configPath, bool active) {
    if (g_TestConfigPath.empty() && configPath) g_TestConfigPath = configPath;
    g_TestActive.store(active, std::memory_order_release);
    return InstallStartupCreationHook();
}

extern "C" __declspec(dllexport) void CEFlow_SetCreationObserver(void (*observer)(DWORD, void*), void* context) {
    g_TestCreationObserver = observer;
    g_TestCreationContext = context;
}
#endif
