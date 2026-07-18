#include "process_loopback_capture.h"

#include "../common/restricted_child_process.h"
#include "app_audio_capture.h"
#include "mediaengine.h"
#include "process_loopback_protocol.h"

#include <algorithm>
#include <bit>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>
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
    ce::process_loopback::ConsumerState consumerState;

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
    ce::process_loopback::TransportLayout layout;
    if (sampleRate < 0 || channels < 0 ||
        !ce::process_loopback::ComputeTransportLayout(static_cast<uint32_t>(sampleRate),
                                                      static_cast<uint32_t>(channels), 32, layout) ||
        (channelMask != 0 && std::popcount(channelMask) != channels)) {
        requestedSampleRate_ = 0;
        requestedChannels_ = 0;
        requestedChannelMask_ = 0;
        DLL_Log("[AppAudioWorker] Invalid requested format rejected: %dHz/%dch/0x%x", sampleRate, channels,
                channelMask);
        return;
    }
    requestedSampleRate_ = sampleRate;
    requestedChannels_ = channels;
    requestedChannelMask_ = channelMask != 0 ? channelMask
                            : channels == 1  ? SPEAKER_FRONT_CENTER
                            : channels == 2  ? SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT
                                             : 0;
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
    integrityFailure_ = false;
    fatalTransportStatus_ = 0;
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
    integrityFailure_ = false;
    fatalTransportStatus_ = 0;
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
    const int chars =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
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
    const uint64_t mappingBytes = ce::process_loopback::MappingBytes(static_cast<uint32_t>(requestedSampleRate_),
                                                                     static_cast<uint32_t>(requestedChannels_), 32);
    if (mappingBytes == 0 || mappingBytes > std::numeric_limits<SIZE_T>::max()) {
        DLL_Log("[AppAudioWorker] Invalid shared mapping size for %dHz/%dch", requestedSampleRate_, requestedChannels_);
        return false;
    }
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
    if (!worker->mapping || !ce::process_loopback::Initialize(worker->mapping, worker->generation,
                                                              static_cast<uint32_t>(requestedSampleRate_),
                                                              static_cast<uint32_t>(requestedChannels_), 32)) {
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
    const std::filesystem::path workerExecutablePath = std::filesystem::absolute(executablePath);

    const auto handleValue = [](HANDLE handle) { return std::to_wstring(reinterpret_cast<uintptr_t>(handle)); };
    std::wstring commandLine = QuoteCommandArgument(workerExecutablePath.wstring()) +
                               L" --process-loopback-worker " + handleValue(worker->mappingHandle) + L" " +
                               handleValue(worker->packetEvent) + L" " + handleValue(worker->stopEvent) + L" " +
                               std::to_wstring(worker->generation) + L" " + std::to_wstring(targetProcessId_) + L" " +
                               std::to_wstring(requestedSampleRate_) + L" " + std::to_wstring(requestedChannels_) +
                               L" " + std::to_wstring(requestedChannelMask_) + L" " +
                               QuoteCommandArgument(Utf8ToWide(targetProcessName_));
    ce::process::RestrictedChildProcess process;
    DWORD createError = ERROR_SUCCESS;
    // The shared launcher installs PROC_THREAD_ATTRIBUTE_HANDLE_LIST and combines
    // CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT so no unrelated parent handle is inherited.
    if (!ce::process::LaunchRestrictedChildProcess(
            workerExecutablePath.wstring(), commandLine, workerExecutablePath.parent_path().wstring(),
            {worker->mappingHandle, worker->packetEvent, worker->stopEvent}, CREATE_NO_WINDOW, process, createError)) {
        DLL_Log("[AppAudioWorker] CreateProcess failed generation=%llu error=0x%lx",
                static_cast<unsigned long long>(worker->generation), createError);
        return false;
    }
    worker->processHandle = std::exchange(process.processHandle, nullptr);
    worker->processId = process.processId;
    activeWorker_ = std::move(worker);
    if (restart) {
        ++workerRestartCount_;
    }
    restartDesired_ = false;
    if (!restart) {
        consecutiveRestartFailures_ = 0;
    }
    DLL_Log(
        "[AppAudioWorker] Started generation=%llu workerPid=%lu targetMode=%s targetPid=%lu targetName=%s "
        "format=%dHz/%dch/0x%x mappingBytes=%llu restart=%d",
        static_cast<unsigned long long>(activeWorker_->generation), activeWorker_->processId,
        targetByName_ ? "name" : "pid", targetProcessId_,
        targetProcessName_.empty() ? "<none>" : targetProcessName_.c_str(), requestedSampleRate_, requestedChannels_,
        requestedChannelMask_, static_cast<unsigned long long>(mappingBytes), restart ? 1 : 0);
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
    const auto transportStatus =
        static_cast<ce::process_loopback::TransportStatus>(header->transportStatus.load(std::memory_order_acquire));
    const auto failureStage = static_cast<ce::process_loopback::TransportFailureStage>(
        header->transportFailureStage.load(std::memory_order_acquire));
    DLL_Log(
        "[AppAudioWorker] Exit generation=%llu workerPid=%lu exitCode=0x%lx unexpected=%d state=%u "
        "pending=%llu produced=%llu consumed=%llu overrun=%llu/%llu lifecycleOverrun=%llu transport=%u "
        "statusName=%s stage=%u stageName=%s failureSeq=%llu lastError=0x%x "
        "failureCursors=%llu/%llu failureBytes=%llu/%llu failureEpoch=%llu/%llu packetEpoch=%llu "
        "record=%u committed=%llu integrityFailures=%llu clean=%llu recycle=%llu",
        static_cast<unsigned long long>(activeWorker_->generation), activeWorker_->processId, exitCode,
        unexpectedExit ? 1 : 0, header->workerState.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(pending),
        static_cast<unsigned long long>(header->producedPackets.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(header->consumedPackets.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(header->overrunPackets.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(header->overrunFrames.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(header->lifecycleOverrunPackets.load(std::memory_order_relaxed)),
        static_cast<uint32_t>(transportStatus), ce::process_loopback::TransportStatusName(transportStatus),
        static_cast<uint32_t>(failureStage), ce::process_loopback::TransportFailureStageName(failureStage),
        static_cast<unsigned long long>(header->transportFailureSequence.load(std::memory_order_relaxed)),
        header->lastError.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(header->failureReadSequence.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(header->failureWriteSequence.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(header->failureReadByteSequence.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(header->failureWriteByteSequence.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(header->failureCurrentEpoch.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(header->failureLastEpoch.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(header->failurePacketEpoch.load(std::memory_order_relaxed)),
        header->failureRecordType.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(header->failureCommittedSequence.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(header->integrityFailureCount.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(header->workerCleanExitCount.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(header->workerRecycleCount.load(std::memory_order_relaxed)));
    retiredWorkers_.push_back(std::move(activeWorker_));
    if (unexpectedExit && desiredRunning_) {
        restartDesired_ = true;
        ++consecutiveRestartFailures_;
        nextRestartTick_ =
            GetTickCount64() + ce::process_loopback::ComputeWorkerRestartDelayMs(consecutiveRestartFailures_);
    }
}

void ProcessLoopbackCapture::HandleIntegrityFailureLocked(WorkerInstance& worker) {
    if (!ce::process_loopback::HasFatalTransportFailure(worker.mapping)) {
        return;
    }
    const uint32_t status = worker.Header()->transportStatus.load(std::memory_order_acquire);
    const uint32_t stage = worker.Header()->transportFailureStage.load(std::memory_order_acquire);
    if (!integrityFailure_) {
        DLL_Log(
            "[AppAudioWorker] FATAL: transport integrity failed generation=%llu workerPid=%lu status=%u/%s "
            "stage=%u/%s sequence=%llu lastError=0x%x cursors=%llu/%llu bytes=%llu/%llu "
            "lifecycle=%llu/%llu packetEpoch=%llu record=%u committed=%llu; "
            "disabling worker restart and requesting recording failure",
            static_cast<unsigned long long>(worker.generation), worker.processId, status,
            ce::process_loopback::TransportStatusName(static_cast<ce::process_loopback::TransportStatus>(status)),
            stage,
            ce::process_loopback::TransportFailureStageName(
                static_cast<ce::process_loopback::TransportFailureStage>(stage)),
            static_cast<unsigned long long>(worker.Header()->transportFailureSequence.load(std::memory_order_acquire)),
            worker.Header()->lastError.load(std::memory_order_acquire),
            static_cast<unsigned long long>(worker.Header()->failureReadSequence.load(std::memory_order_acquire)),
            static_cast<unsigned long long>(worker.Header()->failureWriteSequence.load(std::memory_order_acquire)),
            static_cast<unsigned long long>(worker.Header()->failureReadByteSequence.load(std::memory_order_acquire)),
            static_cast<unsigned long long>(worker.Header()->failureWriteByteSequence.load(std::memory_order_acquire)),
            static_cast<unsigned long long>(worker.Header()->failureCurrentEpoch.load(std::memory_order_acquire)),
            static_cast<unsigned long long>(worker.Header()->failureLastEpoch.load(std::memory_order_acquire)),
            static_cast<unsigned long long>(worker.Header()->failurePacketEpoch.load(std::memory_order_acquire)),
            worker.Header()->failureRecordType.load(std::memory_order_acquire),
            static_cast<unsigned long long>(
                worker.Header()->failureCommittedSequence.load(std::memory_order_acquire)));
    }
    integrityFailure_ = true;
    fatalTransportStatus_ = status;
    desiredRunning_ = false;
    restartDesired_ = false;
    worker.stopRequested = true;
    if (worker.stopEvent) {
        SetEvent(worker.stopEvent);
    }
}

void ProcessLoopbackCapture::RefreshWorkerLocked() {
    for (auto& worker : retiredWorkers_) {
        DrainWorkerDiagnosticsLocked(*worker);
    }
    if (activeWorker_) {
        DrainWorkerDiagnosticsLocked(*activeWorker_);
        HandleIntegrityFailureLocked(*activeWorker_);
        const DWORD wait = WaitForSingleObject(activeWorker_->processHandle, 0);
        if (wait == WAIT_OBJECT_0) {
            DWORD exitCode = ERROR_PROCESS_ABORTED;
            GetExitCodeProcess(activeWorker_->processHandle, &exitCode);
            const SharedHeader* header = activeWorker_->Header();
            const bool clean = header->workerCleanExitCount.load(std::memory_order_acquire) != 0;
            const bool recycle = header->workerRecycleCount.load(std::memory_order_acquire) != 0;
            const bool transportFailure = ce::process_loopback::HasFatalTransportFailure(activeWorker_->mapping);
            const bool restartAfterExit = ce::process_loopback::ClassifyWorkerExit(
                                              desiredRunning_, activeWorker_->stopRequested, clean, recycle,
                                              transportFailure) == ce::process_loopback::WorkerExitDisposition::Restart;
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
            nextRestartTick_ =
                GetTickCount64() + ce::process_loopback::ComputeWorkerRestartDelayMs(consecutiveRestartFailures_);
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
    if (!selected || !ce::process_loopback::ReadPacket(selected->mapping, selected->consumerState, packet)) {
        if (selected) {
            HandleIntegrityFailureLocked(*selected);
        }
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
        ce::process_loopback::DiscardPackets(worker.mapping, worker.consumerState);
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
            DLL_Log("[AppAudioWorker] ERROR: shutdown deadline exceeded generation=%llu workerPid=%lu; terminating",
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
            ce::process_loopback::DiscardPackets(worker->mapping, worker->consumerState);
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
        static_cast<unsigned long long>(accumulatedDiagnosticOverruns_), retiredWorkers_.size(), [&]() {
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

bool ProcessLoopbackCapture::HasIntegrityFailure() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return integrityFailure_;
}

uint32_t ProcessLoopbackCapture::GetTransportStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fatalTransportStatus_;
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
