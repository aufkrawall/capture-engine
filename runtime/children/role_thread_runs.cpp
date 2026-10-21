// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "runtime/children/role_thread_runs.h"

#include "common/ipc/av_sync_latency_channel.h"
#include "common/ipc/role_channel_memory.h"
#include "common/logging/logging.h"

#include <windows.h>

#include <string>
#include <utility>

namespace ce::runtime::detail {
namespace {

// Closes its handle unless released; moves transfer the handle.
class OwnedHandle {
public:
    OwnedHandle() = default;
    explicit OwnedHandle(HANDLE handle) : handle_(handle) {}
    ~OwnedHandle() {
        if (handle_)
            CloseHandle(handle_);
    }
    OwnedHandle(OwnedHandle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    OwnedHandle& operator=(OwnedHandle&& other) noexcept {
        if (this != &other) {
            if (handle_)
                CloseHandle(handle_);
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    OwnedHandle(const OwnedHandle&) = delete;
    OwnedHandle& operator=(const OwnedHandle&) = delete;

    HANDLE Get() const {
        return handle_;
    }
    explicit operator bool() const {
        return handle_ != nullptr;
    }
    // Gives the handle away: its new owner closes it.
    void Release() {
        handle_ = nullptr;
    }

private:
    HANDLE handle_ = nullptr;
};

const char* RoleLabel(ChildRole role) {
    switch (role) {
        case ChildRole::Inject:
            return "inject";
        case ChildRole::Media:
            return "media";
        case ChildRole::Logger:
            return "logger";
        case ChildRole::Sensors:
            break;
    }
    return "sensor";
}

const wchar_t* ThreadName(ChildRole role) {
    switch (role) {
        case ChildRole::Inject:
            return L"CE inject role";
        case ChildRole::Media:
            return L"CE media role";
        case ChildRole::Logger:
            return L"CE logger role";
        case ChildRole::Sensors:
            break;
    }
    return L"CE role";
}

ProcessMode ModeOf(ChildRole role) {
    switch (role) {
        case ChildRole::Inject:
            return ProcessMode::Inject;
        case ChildRole::Media:
            return ProcessMode::Media;
        case ChildRole::Logger:
            return ProcessMode::Logger;
        case ChildRole::Sensors:
            break;
    }
    return ProcessMode::Sensors;
}

RoleEntry EntryOf(const RoleEntries& entries, ChildRole role) {
    switch (role) {
        case ChildRole::Inject:
            return entries.inject;
        case ChildRole::Media:
            return entries.media;
        case ChildRole::Logger:
            return entries.logger;
        case ChildRole::Sensors:
            break;
    }
    return nullptr;
}

// The role takes its own handle, as it would an inherited one; the spawner keeps the original.
HANDLE DuplicateForRole(HANDLE source) {
    HANDLE duplicate = nullptr;
    if (!source || !DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &duplicate, 0, FALSE,
                                    DUPLICATE_SAME_ACCESS)) {
        return nullptr;
    }
    return duplicate;
}

}  // namespace

// One role thread's state. The thread reads the context and the configuration for its whole life, so the run is
// heap-allocated and is released only after the thread has exited.
struct RoleThreadRuns::Run {
    ChildRole role = ChildRole::Logger;
    RoleEntry entry = nullptr;
    AppConfig config;
    std::unique_ptr<ce::role::RoleChannel> channel;  // the role's end; context.channel points here
    ce::role::RoleContext context;
    OwnedHandle stop;    // context.stopEvent: manual-reset, set to ask the role to stop
    OwnedHandle thread;  // the lifecycle token; signaled once the role has ended
    // The handles the role will own once the thread runs: until then this run still closes them.
    OwnedHandle resultSlotCopy;
    OwnedHandle latencyCopy;
};

DWORD WINAPI RoleThreadRuns::RunRoleThread(LPVOID parameter) {
    // Binds the role's log file to this thread for its whole life; threads the role starts inherit it (DR-40).
    auto& run = *static_cast<Run*>(parameter);
    ce::log::SinkScope sink(run.context.logSink);
    const int exitCode = run.entry(run.context, run.config);
    // The role's end goes with the role, as a helper's pipe goes with its process: the host sees the disconnect at once.
    run.context.channel = nullptr;
    run.channel.reset();
    return static_cast<DWORD>(exitCode);
}

RoleThreadRuns::RoleThreadRuns(std::string configPath, ThreadRoleSetup setup)
    : configPath_(std::move(configPath)), setup_(std::move(setup)) {}

RoleThreadRuns::~RoleThreadRuns() {
    // A live role thread references its run: such a run is kept rather than freed under the thread (DR-17, DR-44).
    for (auto& run : runs_) {
        if (run && WaitForSingleObject(run->thread.Get(), 0) != WAIT_OBJECT_0) {
            const ChildRole role = run->role;
            // Retained, never freed: its address is reported so a dump can find it.
            const Run* const retained = run.release();
            LogError("[HostChildren] %s role thread still runs at teardown; its state %p is retained", RoleLabel(role),
                     static_cast<const void*>(retained));
        }
    }
}

RoleThreadRuns::Spawned RoleThreadRuns::Spawn(ChildRole role, void* resultSlot) {
    Spawned spawned;
    const ProcessMode mode = ModeOf(role);
    const RoleEntry entry = EntryOf(setup_.entries, role);
    if (!entry || mode == ProcessMode::Sensors) {
        LogError("[HostChildren] %s has no thread entry; it cannot run on a runtime thread", RoleLabel(role));
        return spawned;
    }

    auto run = std::make_unique<Run>();
    run->role = role;
    run->entry = entry;
    run->config = setup_.currentConfig ? setup_.currentConfig() : AppConfig();

    ce::role::RoleContext& context = run->context;
    context.mode = mode;
    context.ownsProcess = false;
    context.processTuning = setup_.processTuning;
    context.controllerPid = GetCurrentProcessId();
    context.packageDir = setup_.packageDir;
    context.roleHostPath = setup_.roleHostPath;
    context.configPath = configPath_;
    context.logsRoot = setup_.logsRoot;
    // The recording serial a media role names its files with: the one a media helper process would receive.
    context.recordingId = mode == ProcessMode::Media ? g_RecordingId : std::string();

    if (mode == ProcessMode::Inject || mode == ProcessMode::Media) {
        ce::role::InMemoryChannelPair pair = ce::role::MakeInMemoryChannel(mode, context.controllerPid);
        if (!pair.host || !pair.role)
            return spawned;
        spawned.host = std::move(pair.host);
        run->channel = std::move(pair.role);
        context.channel = run->channel.get();
    }

    run->stop = OwnedHandle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!run->stop) {
        LogError("[HostChildren] %s stop event cannot be created (error=%lu)", RoleLabel(role), GetLastError());
        return spawned;
    }
    context.stopEvent = run->stop.Get();

    if (mode == ProcessMode::Media) {
        if (resultSlot) {
            run->resultSlotCopy = OwnedHandle(DuplicateForRole(resultSlot));
            if (!run->resultSlotCopy)
                LogWarn("[HostChildren] media result slot cannot be duplicated; the role reports through its exit code");
            context.resultSlotHandle = run->resultSlotCopy.Get();
        }
        // The controller-lifetime latency channel: the role maps its copy and closes it, as a helper does.
        void* latency = ce::av_sync::GetSessionLatencyChannelChildHandle();
        if (latency) {
            run->latencyCopy = OwnedHandle(DuplicateForRole(static_cast<HANDLE>(latency)));
            context.latencyChannelHandle = run->latencyCopy.Get();
        }
    }

    // A session directory means logging is on: the role writes its own file, named as its helper process would.
    if (!setup_.sessionDir.empty()) {
        const std::string path =
            setup_.sessionDir + "\\" + GetProcessLogFileName(mode, context.recordingId, context.controllerPid);
        context.logSink = ce::log::OpenSink(path);
        if (!context.logSink)
            LogWarn("[HostChildren] %s role log cannot be opened; the role writes the process log", RoleLabel(role));
    }

    // Reserved before the thread exists: the run must reach the list without an allocation that could fail.
    runs_.reserve(runs_.size() + 1);
    DWORD threadId = 0;
    run->thread = OwnedHandle(CreateThread(nullptr, 0, RunRoleThread, run.get(), 0, &threadId));
    if (!run->thread) {
        LogError("[HostChildren] %s role thread cannot be created (error=%lu)", RoleLabel(role), GetLastError());
        return {};
    }
    // The media role's name carries its recording id: overlapping recordings run one role thread each.
    std::wstring threadName = ThreadName(role);
    if (mode == ProcessMode::Media && !context.recordingId.empty())
        threadName += L" " + std::wstring(context.recordingId.begin(), context.recordingId.end());
    SetThreadDescription(run->thread.Get(), threadName.c_str());
    run->resultSlotCopy.Release();  // the role owns both handles from here on
    run->latencyCopy.Release();

    spawned.token = reinterpret_cast<ChildToken>(run->thread.Get());
    LogInfo("[HostChildren] %s running on a runtime thread (thread=%lu)", RoleLabel(role),
            static_cast<unsigned long>(threadId));
    runs_.push_back(std::move(run));
    return spawned;
}

bool RoleThreadRuns::Owns(ChildToken token) const {
    return Find(token) != nullptr;
}

void RoleThreadRuns::RequestStop(ChildToken token) const {
    if (const Run* run = Find(token))
        SetEvent(run->stop.Get());
}

uint32_t RoleThreadRuns::ExitCode(ChildToken token) const {
    const Run* run = Find(token);
    DWORD exitCode = STILL_ACTIVE;
    if (!run || !GetExitCodeThread(run->thread.Get(), &exitCode))
        LogWarn("[HostChildren] role thread exit code unavailable (error=%lu)", GetLastError());
    return exitCode;
}

void RoleThreadRuns::Release(ChildToken token) {
    for (auto it = runs_.begin(); it != runs_.end(); ++it) {
        if (reinterpret_cast<ChildToken>((*it)->thread.Get()) != token)
            continue;
        if (WaitForSingleObject((*it)->thread.Get(), 0) != WAIT_OBJECT_0) {
            LogError("[HostChildren] %s role thread has not exited; its state is kept", RoleLabel((*it)->role));
            return;
        }
        runs_.erase(it);
        return;
    }
}

RoleThreadRuns::Run* RoleThreadRuns::Find(ChildToken token) const {
    for (const auto& run : runs_) {
        if (reinterpret_cast<ChildToken>(run->thread.Get()) == token)
            return run.get();
    }
    return nullptr;
}

}  // namespace ce::runtime::detail
