#include "process_loopback_capture.h"

#include "app_audio_capture.h"
#include "mediaengine.h"
#include "process_loopback_protocol.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

using ce::process_loopback::SharedHeader;
using ce::process_loopback::WorkerState;

namespace {

constexpr DWORD kWorkerShutdownDeadlineMs = 5000;
constexpr DWORD kWorkerForcedExitDeadlineMs = 1000;

}  // namespace

struct ProcessLoopbackCapture::WorkerInstance {
    uint64_t generation = 0;
    HANDLE mappingHandle = nullptr;
    void* mapping = nullptr;
    HANDLE packetEvent = nullptr;
    HANDLE stopEvent = nullptr;
    HANDLE processHandle = nullptr;
    DWORD processId = 0;
    bool stopRequested = false;
    mutable uint64_t lastDiagnosticOverruns = 0;

    ~WorkerInstance() {
        if (processHandle) {
            CloseHandle(processHandle);
        }
        if (mapping) {
            UnmapViewOfFile(mapping);
        }
        if (mappingHandle) {
            CloseHandle(mappingHandle);
        }
        if (packetEvent) {
            CloseHandle(packetEvent);
        }
        if (stopEvent) {
            CloseHandle(stopEvent);
        }
    }

    SharedHeader* Header() const {
        return static_cast<SharedHeader*>(mapping);
    }
};

ProcessLoopbackCapture::ProcessLoopbackCapture() = default;

ProcessLoopbackCapture::~ProcessLoopbackCapture() {
    Stop(true);
}

bool ProcessLoopbackCapture::IsSupported() {
    return AppAudioCapture::IsSupported();
}

void ProcessLoopbackCapture::SetRequestedFormat(int sampleRate, int channels, uint32_t channelMask) {
    std::lock_guard<std::mutex> lock(mutex_);
    requestedSampleRate_ = sampleRate > 0 ? sampleRate : 48000;
    requestedChannels_ = std::clamp(channels > 0 ? channels : 2, 1, 8);
    requestedChannelMask_ = channelMask;
    if (requestedChannelMask_ == 0) {
        requestedChannelMask_ = requestedChannels_ == 1
                                    ? SPEAKER_FRONT_CENTER
                                    : requestedChannels_ == 2
                                          ? SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT
                                          : 0;
    }
}

bool ProcessLoopbackCapture::StartByPID(DWORD processId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (desiredRunning_ || activeWorker_) {
        DLL_Log("[AppAudioWorker] StartByPID rejected: capture is already active");
        return false;
    }
    if (!IsSupported() || processId == 0) {
        DLL_Log("[AppAudioWorker] StartByPID rejected: supported=%d pid=%lu", IsSupported() ? 1 : 0, processId);
        return false;
    }
    targetByName_ = false;
    targetProcessId_ = processId;
    targetProcessName_.clear();
    desiredRunning_ = true;
    restartDesired_ = true;
    if (!StartWorkerLocked(false)) {
        desiredRunning_ = false;
        restartDesired_ = false;
        return false;
    }
    return true;
}

bool ProcessLoopbackCapture::StartByName(const std::string& processName) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (desiredRunning_ || activeWorker_ || processName.empty()) {
        DLL_Log("[AppAudioWorker] StartByName rejected: active=%d nameEmpty=%d", desiredRunning_ ? 1 : 0,
                processName.empty() ? 1 : 0);
        return false;
    }
    if (!IsSupported()) {
        DLL_Log("[AppAudioWorker] StartByName rejected: process loopback is unsupported");
        return false;
    }
    targetByName_ = true;
    targetProcessId_ = 0;
    targetProcessName_ = processName;
    desiredRunning_ = true;
    restartDesired_ = true;
    if (!StartWorkerLocked(false)) {
        desiredRunning_ = false;
        restartDesired_ = false;
        return false;
    }
    return true;
}

std::wstring ProcessLoopbackCapture::Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), static_cast<int>(value.size()),
                                          nullptr, 0);
    if (chars <= 0) {
        return std::wstring(value.begin(), value.end());
    }
    std::wstring result(static_cast<size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), static_cast<int>(value.size()), result.data(),
                        chars);
    return result;
}

std::wstring ProcessLoopbackCapture::QuoteCommandArgument(const std::wstring& value) {
    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(ch);
        } else {
            result.append(backslashes, L'\\');
            result.push_back(ch);
        }
        backslashes = 0;
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

bool ProcessLoopbackCapture::StartWorkerLocked(bool restart) {
    auto worker = std::make_unique<WorkerInstance>();
    worker->generation = nextWorkerGeneration_++;

    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;
    const uint64_t mappingBytes = ce::process_loopback::MappingBytes();
    worker->mappingHandle =
        CreateFileMappingW(INVALID_HANDLE_VALUE, &inheritable, PAGE_READWRITE, static_cast<DWORD>(mappingBytes >> 32),
                           static_cast<DWORD>(mappingBytes), nullptr);
    worker->packetEvent = CreateEventW(&inheritable, TRUE, FALSE, nullptr);
    worker->stopEvent = CreateEventW(&inheritable, TRUE, FALSE, nullptr);
    if (!worker->mappingHandle || !worker->packetEvent || !worker->stopEvent) {
        DLL_Log("[AppAudioWorker] Resource creation failed generation=%llu error=0x%lx",
                static_cast<unsigned long long>(worker->generation), GetLastError());
        return false;
    }
    worker->mapping = MapViewOfFile(worker->mappingHandle, FILE_MAP_ALL_ACCESS, 0, 0, mappingBytes);
    if (!worker->mapping || !ce::process_loopback::Initialize(worker->mapping, worker->generation)) {
        DLL_Log("[AppAudioWorker] Shared mapping failed generation=%llu bytes=%llu error=0x%lx",
                static_cast<unsigned long long>(worker->generation), static_cast<unsigned long long>(mappingBytes),
                GetLastError());
        return false;
    }

    wchar_t executablePath[MAX_PATH]{};
    const DWORD executableChars = GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
    if (executableChars == 0 || executableChars >= MAX_PATH) {
        DLL_Log("[AppAudioWorker] Cannot resolve executable directory: error=0x%lx", GetLastError());
        return false;
    }
    const std::filesystem::path helperPath =
        std::filesystem::path(executablePath).parent_path() / L"process_loopback_helper.exe";
    if (!std::filesystem::exists(helperPath)) {
        DLL_Log("[AppAudioWorker] Helper executable is missing: %ls", helperPath.c_str());
        return false;
    }

    const auto handleValue = [](HANDLE handle) { return std::to_wstring(reinterpret_cast<uintptr_t>(handle)); };
    std::wstring commandLine = QuoteCommandArgument(helperPath.wstring()) + L" " + handleValue(worker->mappingHandle) +
                               L" " + handleValue(worker->packetEvent) + L" " + handleValue(worker->stopEvent) +
                               L" " + std::to_wstring(worker->generation) + L" " +
                               std::to_wstring(targetProcessId_) + L" " + std::to_wstring(requestedSampleRate_) +
                               L" " + std::to_wstring(requestedChannels_) + L" " +
                               std::to_wstring(requestedChannelMask_) + L" " +
                               QuoteCommandArgument(Utf8ToWide(targetProcessName_));
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<uint8_t> attributeStorage(attributeBytes);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeBytes)) {
        DLL_Log("[AppAudioWorker] InitializeProcThreadAttributeList failed: 0x%lx", GetLastError());
        return false;
    }
    HANDLE inheritedHandles[] = {worker->mappingHandle, worker->packetEvent, worker->stopEvent};
    const bool handleListReady =
        UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritedHandles,
                                  sizeof(inheritedHandles), nullptr, nullptr) != FALSE;
    if (!handleListReady) {
        DLL_Log("[AppAudioWorker] UpdateProcThreadAttribute(handle-list) failed: 0x%lx", GetLastError());
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        return false;
    }

    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(helperPath.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                        helperPath.parent_path().c_str(), &startup.StartupInfo, &process);
    const DWORD createError = created ? ERROR_SUCCESS : GetLastError();
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!created) {
        DLL_Log("[AppAudioWorker] CreateProcess failed generation=%llu error=0x%lx",
                static_cast<unsigned long long>(worker->generation), createError);
        return false;
    }
    CloseHandle(process.hThread);
    worker->processHandle = process.hProcess;
    worker->processId = process.dwProcessId;
    activeWorker_ = std::move(worker);
    if (restart) {
        ++workerRestartCount_;
    }
    restartDesired_ = false;
    if (!restart) {
        consecutiveRestartFailures_ = 0;
    }
    DLL_Log(
        "[AppAudioWorker] Started generation=%llu helperPid=%lu targetMode=%s targetPid=%lu targetName=%s "
        "format=%dHz/%dch/0x%x mappingBytes=%llu restart=%d",
        static_cast<unsigned long long>(activeWorker_->generation), activeWorker_->processId,
        targetByName_ ? "name" : "pid", targetProcessId_, targetProcessName_.empty() ? "<none>" : targetProcessName_.c_str(),
        requestedSampleRate_, requestedChannels_, requestedChannelMask_, static_cast<unsigned long long>(mappingBytes),
        restart ? 1 : 0);
    return true;
}

void ProcessLoopbackCapture::DrainWorkerDiagnosticsLocked(WorkerInstance& worker) const {
    std::string message;
    while (ce::process_loopback::ReadDiagnostic(worker.mapping, message)) {
        DLL_Log("[AppAudioWorker g=%llu pid=%lu] %s", static_cast<unsigned long long>(worker.generation),
                worker.processId, message.c_str());
    }
    const uint64_t diagnosticOverruns = worker.Header()->diagnosticOverruns.load(std::memory_order_relaxed);
    if (diagnosticOverruns != worker.lastDiagnosticOverruns) {
        DLL_Log("[AppAudioWorker] WARNING: diagnostic relay overrun generation=%llu dropped=%llu total=%llu",
                static_cast<unsigned long long>(worker.generation),
                static_cast<unsigned long long>(diagnosticOverruns - worker.lastDiagnosticOverruns),
                static_cast<unsigned long long>(diagnosticOverruns));
        worker.lastDiagnosticOverruns = diagnosticOverruns;
    }
}

void ProcessLoopbackCapture::RetireActiveWorkerLocked(bool unexpectedExit, DWORD exitCode) {
    if (!activeWorker_) {
        return;
    }
    DrainWorkerDiagnosticsLocked(*activeWorker_);
    const SharedHeader* header = activeWorker_->Header();
    const uint64_t pending = ce::process_loopback::PendingPacketCount(activeWorker_->mapping);
    DLL_Log(
        "[AppAudioWorker] Exit generation=%llu helperPid=%lu exitCode=0x%lx unexpected=%d state=%u "
        "pending=%llu produced=%llu consumed=%llu overrun=%llu/%llu lifecycleOverrun=%llu clean=%llu recycle=%llu",
        static_cast<unsigned long long>(activeWorker_->generation), activeWorker_->processId, exitCode,
        unexpectedExit ? 1 : 0, header->workerState.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(pending),
        static_cast<unsigned long long>(header->producedPackets.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(header->consumedPackets.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(header->overrunPackets.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(header->overrunFrames.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(header->lifecycleOverrunPackets.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(header->workerCleanExitCount.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(header->workerRecycleCount.load(std::memory_order_relaxed)));
    retiredWorkers_.push_back(std::move(activeWorker_));
    if (unexpectedExit && desiredRunning_) {
        restartDesired_ = true;
        ++consecutiveRestartFailures_;
        nextRestartTick_ = GetTickCount64() +
                           ce::process_loopback::ComputeWorkerRestartDelayMs(consecutiveRestartFailures_);
    }
}

void ProcessLoopbackCapture::RefreshWorkerLocked() {
    for (auto& worker : retiredWorkers_) {
        DrainWorkerDiagnosticsLocked(*worker);
    }
    if (activeWorker_) {
        DrainWorkerDiagnosticsLocked(*activeWorker_);
        const DWORD wait = WaitForSingleObject(activeWorker_->processHandle, 0);
        if (wait == WAIT_OBJECT_0) {
            DWORD exitCode = ERROR_PROCESS_ABORTED;
            GetExitCodeProcess(activeWorker_->processHandle, &exitCode);
            const SharedHeader* header = activeWorker_->Header();
            const bool clean = header->workerCleanExitCount.load(std::memory_order_acquire) != 0;
            const bool recycle = header->workerRecycleCount.load(std::memory_order_acquire) != 0;
            const bool restartAfterExit = ce::process_loopback::ClassifyWorkerExit(
                                              desiredRunning_, activeWorker_->stopRequested, clean, recycle) ==
                                          ce::process_loopback::WorkerExitDisposition::Restart;
            RetireActiveWorkerLocked(restartAfterExit && !recycle, exitCode);
            if (recycle && desiredRunning_) {
                // A clean recycle is the process-lifetime containment policy, not a crash. Restart
                // immediately so app stop/start churn does not inherit failure backoff latency.
                consecutiveRestartFailures_ = 0;
                nextRestartTick_ = 0;
                restartDesired_ = true;
                DLL_Log("[AppAudioWorker] Clean lifecycle recycle will restart immediately");
            }
        } else if (wait == WAIT_FAILED) {
            const DWORD waitError = GetLastError();
            DLL_Log("[AppAudioWorker] WARNING: process-state wait failed generation=%llu error=0x%lx",
                    static_cast<unsigned long long>(activeWorker_->generation), waitError);
        }
    }
    if (!activeWorker_ && desiredRunning_ && restartDesired_ && GetTickCount64() >= nextRestartTick_) {
        if (!StartWorkerLocked(true)) {
            ++consecutiveRestartFailures_;
            nextRestartTick_ = GetTickCount64() +
                               ce::process_loopback::ComputeWorkerRestartDelayMs(consecutiveRestartFailures_);
            DLL_Log("[AppAudioWorker] WARNING: restart failed attempt=%u nextAttemptInMs=%llu",
                    consecutiveRestartFailures_,
                    static_cast<unsigned long long>(
                        ce::process_loopback::ComputeWorkerRestartDelayMs(consecutiveRestartFailures_)));
        }
    }
    CleanupDrainedWorkersLocked();
}

void ProcessLoopbackCapture::AccumulateAndDestroyWorkerLocked(std::unique_ptr<WorkerInstance> worker) {
    if (!worker) {
        return;
    }
    DrainWorkerDiagnosticsLocked(*worker);
    const SharedHeader* header = worker->Header();
    accumulatedOverrunPackets_ += header->overrunPackets.load(std::memory_order_relaxed);
    accumulatedOverrunFrames_ += header->overrunFrames.load(std::memory_order_relaxed);
    accumulatedDiagnosticOverruns_ += header->diagnosticOverruns.load(std::memory_order_relaxed);
}

void ProcessLoopbackCapture::CleanupDrainedWorkersLocked() {
    while (!retiredWorkers_.empty() &&
           ce::process_loopback::PendingPacketCount(retiredWorkers_.front()->mapping) == 0) {
        AccumulateAndDestroyWorkerLocked(std::move(retiredWorkers_.front()));
        retiredWorkers_.pop_front();
    }
}

bool ProcessLoopbackCapture::GetNextPacket(AudioPacket& packet) {
    std::lock_guard<std::mutex> lock(mutex_);
    RefreshWorkerLocked();
    WorkerInstance* selected = !retiredWorkers_.empty() ? retiredWorkers_.front().get() : activeWorker_.get();
    if (!selected || !ce::process_loopback::ReadPacket(selected->mapping, packet)) {
        return false;
    }
    if (packet.captureEpoch != 0) {
        packet.captureEpoch = (selected->generation << 32) | (packet.captureEpoch & 0xffffffffull);
    }
    if (packet.recordType == AudioPacketRecordType::Data && !packet.data.empty()) {
        consecutiveRestartFailures_ = 0;
    }
    if (ce::process_loopback::PendingPacketCount(selected->mapping) == 0 && selected->packetEvent) {
        ResetEvent(selected->packetEvent);
        if (ce::process_loopback::PendingPacketCount(selected->mapping) != 0) {
            SetEvent(selected->packetEvent);
        }
    }
    CleanupDrainedWorkersLocked();
    return true;
}

size_t ProcessLoopbackCapture::PendingPacketCount() {
    std::lock_guard<std::mutex> lock(mutex_);
    RefreshWorkerLocked();
    size_t pending = activeWorker_ ? ce::process_loopback::PendingPacketCount(activeWorker_->mapping) : 0;
    for (const auto& worker : retiredWorkers_) {
        pending += ce::process_loopback::PendingPacketCount(worker->mapping);
    }
    return pending;
}

void ProcessLoopbackCapture::DiscardPendingPackets() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto discard = [](WorkerInstance& worker) {
        SharedHeader* header = worker.Header();
        header->readSequence.store(header->writeSequence.load(std::memory_order_acquire), std::memory_order_release);
        if (worker.packetEvent) {
            ResetEvent(worker.packetEvent);
        }
    };
    if (activeWorker_) {
        discard(*activeWorker_);
    }
    for (auto& worker : retiredWorkers_) {
        discard(*worker);
    }
    CleanupDrainedWorkersLocked();
}

void ProcessLoopbackCapture::Stop(bool discardPendingPackets) {
    std::lock_guard<std::mutex> lock(mutex_);
    desiredRunning_ = false;
    restartDesired_ = false;
    if (activeWorker_) {
        activeWorker_->stopRequested = true;
        activeWorker_->Header()->workerState.store(static_cast<uint32_t>(WorkerState::Stopping),
                                                   std::memory_order_release);
        SetEvent(activeWorker_->stopEvent);
        DWORD wait = WaitForSingleObject(activeWorker_->processHandle, kWorkerShutdownDeadlineMs);
        if (wait == WAIT_TIMEOUT) {
            DLL_Log("[AppAudioWorker] ERROR: shutdown deadline exceeded generation=%llu helperPid=%lu; terminating",
                    static_cast<unsigned long long>(activeWorker_->generation), activeWorker_->processId);
            TerminateProcess(activeWorker_->processHandle, ERROR_TIMEOUT);
            WaitForSingleObject(activeWorker_->processHandle, kWorkerForcedExitDeadlineMs);
        } else if (wait == WAIT_FAILED) {
            DLL_Log("[AppAudioWorker] ERROR: shutdown wait failed generation=%llu error=0x%lx",
                    static_cast<unsigned long long>(activeWorker_->generation), GetLastError());
        }
        DWORD exitCode = ERROR_PROCESS_ABORTED;
        GetExitCodeProcess(activeWorker_->processHandle, &exitCode);
        RetireActiveWorkerLocked(false, exitCode);
    }
    if (discardPendingPackets) {
        for (auto& worker : retiredWorkers_) {
            SharedHeader* header = worker->Header();
            header->readSequence.store(header->writeSequence.load(std::memory_order_acquire),
                                       std::memory_order_release);
        }
    }
    CleanupDrainedWorkersLocked();
    if (discardPendingPackets) {
        while (!retiredWorkers_.empty()) {
            AccumulateAndDestroyWorkerLocked(std::move(retiredWorkers_.front()));
            retiredWorkers_.pop_front();
        }
    }
    DLL_Log(
        "[AppAudioWorker] Stop complete discard=%d restarts=%llu accumulatedOverrun=%llu/%llu "
        "diagnosticOverrun=%llu retainedWorkers=%zu retainedPackets=%zu",
        discardPendingPackets ? 1 : 0, static_cast<unsigned long long>(workerRestartCount_),
        static_cast<unsigned long long>(accumulatedOverrunPackets_),
        static_cast<unsigned long long>(accumulatedOverrunFrames_),
        static_cast<unsigned long long>(accumulatedDiagnosticOverruns_), retiredWorkers_.size(),
        [&]() {
            size_t pending = 0;
            for (const auto& worker : retiredWorkers_) {
                pending += ce::process_loopback::PendingPacketCount(worker->mapping);
            }
            return pending;
        }());
}

uint64_t ProcessLoopbackCapture::GetQueueOverrunPacketCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t total = accumulatedOverrunPackets_;
    if (activeWorker_) {
        total += activeWorker_->Header()->overrunPackets.load(std::memory_order_relaxed);
    }
    for (const auto& worker : retiredWorkers_) {
        total += worker->Header()->overrunPackets.load(std::memory_order_relaxed);
    }
    return total;
}

uint64_t ProcessLoopbackCapture::GetQueueOverrunFrameCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t total = accumulatedOverrunFrames_;
    if (activeWorker_) {
        total += activeWorker_->Header()->overrunFrames.load(std::memory_order_relaxed);
    }
    for (const auto& worker : retiredWorkers_) {
        total += worker->Header()->overrunFrames.load(std::memory_order_relaxed);
    }
    return total;
}

uint64_t ProcessLoopbackCapture::GetWorkerRestartCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return workerRestartCount_;
}

bool ProcessLoopbackCapture::IsCapturing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return activeWorker_ && activeWorker_->Header()->workerState.load(std::memory_order_acquire) ==
                                static_cast<uint32_t>(WorkerState::Capturing);
}

bool ProcessLoopbackCapture::IsMonitoring() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!activeWorker_) {
        return false;
    }
    const uint32_t state = activeWorker_->Header()->workerState.load(std::memory_order_acquire);
    return state == static_cast<uint32_t>(WorkerState::Monitoring) ||
           state == static_cast<uint32_t>(WorkerState::Capturing);
}

DWORD ProcessLoopbackCapture::GetTargetPID() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (activeWorker_) {
        return static_cast<DWORD>(activeWorker_->Header()->activeTargetPid.load(std::memory_order_acquire));
    }
    return targetProcessId_;
}
