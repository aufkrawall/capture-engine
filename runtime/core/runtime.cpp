// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "runtime/core/runtime_internal.h"
#include "common/platform/thread_label.h"

#include "common/logging/logging.h"

namespace ce::engine {
namespace {
uint64_t SystemNowMs() noexcept {
    return GetTickCount64();
}
int64_t SystemNowUs() noexcept {
    return Log_GetQpcUs();
}

bool IsRecordingCommand(CommandKind kind) {
    return kind == CommandKind::StartRecording || kind == CommandKind::ToggleRecording;
}

struct EngineThreadStart {
    RuntimeParts parts;
    EngineShared* shared = nullptr;
    HMODULE module = nullptr;
};

// The engine thread holds a reference on the module its code lives in and drops it as its very last action, so a
// client that unloads cengine.dll while the runtime still runs (a shutdown it stopped waiting for) cannot unmap the
// code under it. Inside captureengine.exe the reference is the executable's own and changes nothing.
DWORD WINAPI EngineThreadMain(void* parameter) {
    ce::platform::NameCurrentThread("CE runtime engine");
    HMODULE module = nullptr;
    {
        std::unique_ptr<EngineThreadStart> start(static_cast<EngineThreadStart*>(parameter));
        module = start->module;
        RunEngine(std::move(start->parts), *start->shared);
    }
    FreeLibraryAndExitThread(module, 0);
}
}  // namespace

Clock SystemClock() noexcept {
    return Clock{&SystemNowMs, &SystemNowUs};
}

Runtime::Impl::Impl(RuntimeParts parts, std::unique_ptr<EventQueue> events, ce_status_t* status)
    : settings(parts.store), session_(std::move(parts.session)) {
    shared.events = std::move(events);
    setup = parts.setup;
    shared.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    shared.stopped = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!shared.wake || !shared.stopped) {
        if (status)
            *status = CE_E_NO_MEMORY;
        return;
    }
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&EngineThreadMain),
                            &module)) {
        LogError("[Runtime] Cannot pin the runtime module for the engine thread (error=%lu)", GetLastError());
        if (status)
            *status = CE_E_NO_MEMORY;
        return;
    }
    std::unique_ptr<EngineThreadStart> start;
    try {
        start = std::make_unique<EngineThreadStart>(EngineThreadStart{std::move(parts), &shared, module});
    } catch (...) {
        // Nothing runs yet: a failed record allocation is the same out-of-memory result as a failed thread start.
        LogError("[Runtime] Cannot allocate the engine thread start record");
        FreeLibrary(module);
        if (status)
            *status = CE_E_NO_MEMORY;
        return;
    }
    EngineThreadStart* handoff = start.release();  // the engine thread owns the record from here
    thread_ = CreateThread(nullptr, 0, &EngineThreadMain, handoff, 0, nullptr);
    if (!thread_) {
        std::unique_ptr<EngineThreadStart> reclaimed(handoff);  // the thread never started: take the record back
        LogError("[Runtime] Cannot start the engine thread (error=%lu)", GetLastError());
        FreeLibrary(module);
        if (status)
            *status = CE_E_NO_MEMORY;
        return;
    }
}

Runtime::Impl::~Impl() {
    RequestShutdown();
    if (thread_) {
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
    }
    // Commands admitted after the engine stopped draining are owed nothing more: the queue is going away.
    shared.commands.clear();
    if (shared.wake)
        CloseHandle(shared.wake);
    if (shared.stopped)
        CloseHandle(shared.stopped);
    // The engine thread, its helpers and their role threads are gone; the session's log is still open.
    if (session_ && session_->residueBaseline) {
        const ce::runtime::ProcessResidueSample now = ce::runtime::SampleThisProcess();
        LogInfo("[RuntimeTeardown] residue %s", ce::runtime::DescribeResidue(*session_->residueBaseline, now).c_str());
        const std::string threads = ce::runtime::DescribeNewThreads(*session_->residueBaseline, now);
        if (!threads.empty())
            LogInfo("[RuntimeTeardown] residue threads: %s", threads.c_str());
    }
}

Admission Runtime::Impl::Submit(Command command) {
    Admission admission;
    if (IsRecordingCommand(command.kind) && command.mode != CE_MODE_VIDEO && command.mode != CE_MODE_AUDIO_ONLY) {
        admission.status = CE_E_INVALID_ARGUMENT;
        return admission;
    }
    if (command.kind == CommandKind::LaunchTarget && command.text.empty()) {
        admission.status = CE_E_INVALID_ARGUMENT;
        return admission;
    }
    if (command.kind == CommandKind::Setup && !ce::runtime::IsKnownSetupAction(command.setupAction)) {
        admission.status = CE_E_INVALID_ARGUMENT;
        return admission;
    }
    std::lock_guard<std::mutex> lock(shared.mutex);
    if (shared.shutdownRequested || shared.status.runtimeState != CE_RUNTIME_READY) {
        admission.status = CE_E_INVALID_STATE;
        return admission;
    }
    PendingCommand pending;
    pending.completion = shared.events->Reserve();
    if (!pending.completion) {
        admission.status = CE_E_NO_MEMORY;
        return admission;
    }
    if (command.kind == CommandKind::TakeScreenshot) {
        // Combined HDR+SDR screenshots publish up to two files.
        pending.screenshot = shared.events->Reserve(EventQueue::kDefaultReservedTextBytes + 2 * MAX_PATH * 3, 2);
        if (!pending.screenshot) {
            admission.status = CE_E_NO_MEMORY;
            return admission;
        }
    }
    pending.request = ++shared.nextRequest;
    pending.command = std::move(command);
    admission.request = pending.request;
    shared.commands.push_back(std::move(pending));
    SetEvent(shared.wake);
    return admission;
}

StatusSnapshot Runtime::Impl::Status() const {
    std::lock_guard<std::mutex> lock(shared.mutex);
    return shared.status;
}

void Runtime::Impl::RequestShutdown() {
    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.shutdownRequested = true;
    }
    if (shared.wake)
        SetEvent(shared.wake);
}

bool Runtime::Impl::WaitStopped(uint32_t timeoutMs) {
    return shared.stopped && WaitForSingleObject(shared.stopped, timeoutMs) == WAIT_OBJECT_0;
}

std::unique_ptr<Runtime> RuntimeFactory::Assemble(RuntimeParts parts, ce_status_t* status) {
    if (status)
        *status = CE_OK;
    if (!parts.clock.nowMs || !parts.clock.nowUs)
        parts.clock = SystemClock();
    ce_status_t queueStatus = CE_OK;
    std::unique_ptr<EventQueue> events = EventQueue::Create(EventQueue::Options{}, &queueStatus);
    if (!events) {
        if (status)
            *status = queueStatus;
        return nullptr;
    }
    ce_status_t startStatus = CE_OK;
    auto impl = std::make_unique<Runtime::Impl>(std::move(parts), std::move(events), &startStatus);
    if (startStatus != CE_OK || !impl->Started()) {
        if (status)
            *status = startStatus != CE_OK ? startStatus : CE_E_INTERNAL;
        return nullptr;
    }
    return std::unique_ptr<Runtime>(new Runtime(std::move(impl)));
}

Runtime::Runtime(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Runtime::~Runtime() = default;

Admission Runtime::Submit(Command command) {
    return impl_->Submit(std::move(command));
}

StatusSnapshot Runtime::Status() const {
    return impl_->Status();
}

bool Runtime::SetupState(ce::runtime::SetupStatus& out) const {
    if (!impl_->setup)
        return false;
    HardwareSensorsConfig sensors;
    {
        std::lock_guard<std::mutex> lock(impl_->shared.mutex);
        sensors = impl_->shared.hardwareSensors;
    }
    out = ce::runtime::ReadSetupStatus(*impl_->setup, sensors);
    return true;
}

EventQueue& Runtime::Events() {
    return *impl_->shared.events;
}

void Runtime::RequestShutdown() {
    impl_->RequestShutdown();
}

bool Runtime::WaitStopped(uint32_t timeoutMs) {
    return impl_->WaitStopped(timeoutMs);
}

const SettingsReader& Runtime::Settings() const {
    return impl_->settings;
}

}  // namespace ce::engine
