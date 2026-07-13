#include "mediaengine.h"

#include "app_audio_capture.h"
#include "process_loopback_protocol.h"

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>

namespace {

struct WorkerLogRelay {
    void* mapping = nullptr;
    std::mutex mutex;
};

std::atomic<WorkerLogRelay*> g_WorkerLogRelay{nullptr};

void RelayWorkerLog(const char* message) {
    WorkerLogRelay* relay = g_WorkerLogRelay.load(std::memory_order_acquire);
    if (!relay || !relay->mapping || !message) {
        return;
    }
    std::lock_guard<std::mutex> lock(relay->mutex);
    ce::process_loopback::WriteDiagnostic(relay->mapping, message);
}

std::string WideToUtf8(const wchar_t* value) {
    if (!value || !*value) {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) {
        return {};
    }
    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, result.data(), bytes, nullptr, nullptr);
    result.resize(static_cast<size_t>(bytes - 1));
    return result;
}

}  // namespace

extern "C" MEDIAENGINE_API int MediaEngine_RunProcessLoopbackWorker(
    uint64_t mappingHandleValue, uint64_t packetEventValue, uint64_t stopEventValue, uint64_t workerGeneration,
    uint32_t targetPid, const wchar_t* targetProcessName, int sampleRate, int channels, uint32_t channelMask) {
    const HANDLE mappingHandle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(mappingHandleValue));
    const HANDLE packetEvent = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(packetEventValue));
    const HANDLE stopEvent = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(stopEventValue));
    void* mapping = MapViewOfFile(mappingHandle, FILE_MAP_ALL_ACCESS, 0, 0, ce::process_loopback::MappingBytes());
    if (!mapping || !ce::process_loopback::Validate(mapping, workerGeneration)) {
        if (mapping) {
            UnmapViewOfFile(mapping);
        }
        return ERROR_INVALID_DATA;
    }

    auto* header = static_cast<ce::process_loopback::SharedHeader*>(mapping);
    header->workerPid.store(GetCurrentProcessId(), std::memory_order_release);
    header->workerStartCount.fetch_add(1, std::memory_order_relaxed);
    header->workerState.store(static_cast<uint32_t>(ce::process_loopback::WorkerState::Starting),
                              std::memory_order_release);
    header->heartbeatTick.store(GetTickCount64(), std::memory_order_release);

    WorkerLogRelay relay{mapping};
    g_WorkerLogRelay.store(&relay, std::memory_order_release);
    MediaEngine_SetLogCallback(&RelayWorkerLog);

    int exitCode = ERROR_SUCCESS;
    {
        AppAudioCapture capture;
        capture.SetRequestedFormat(sampleRate, channels, channelMask);
        const std::string processName = WideToUtf8(targetProcessName);
        const bool byName = targetPid == 0 && !processName.empty();
        const bool started = byName ? capture.StartByName(processName) : capture.StartByPID(targetPid);
        if (!started || !capture.GetPacketReadyEvent()) {
            exitCode = !started ? ERROR_OPEN_FAILED : ERROR_INVALID_HANDLE;
            header->lastError.store(static_cast<uint32_t>(exitCode), std::memory_order_release);
            header->workerState.store(static_cast<uint32_t>(ce::process_loopback::WorkerState::Failed),
                                      std::memory_order_release);
            RelayWorkerLog("[Worker] Process-loopback capture failed to start or has no packet-ready event");
        } else {
            header->workerState.store(
                static_cast<uint32_t>(byName ? ce::process_loopback::WorkerState::Monitoring
                                             : ce::process_loopback::WorkerState::Starting),
                std::memory_order_release);
            HANDLE waitHandles[] = {stopEvent, capture.GetPacketReadyEvent()};
            bool stopping = false;
            bool sawEndOfStream = false;
            bool recycleWorker = false;
            while (true) {
                AudioPacket packet;
                while (capture.GetNextPacket(packet)) {
                    if (packet.recordType == AudioPacketRecordType::EpochStart && packet.captureEpoch > 1) {
                        recycleWorker = true;
                        RelayWorkerLog(
                            "[Worker] Recycling after process-loopback reactivation to bound abandoned AudioSes state");
                    }
                    if (packet.recordType == AudioPacketRecordType::EndOfStream || packet.endOfStream) {
                        sawEndOfStream = true;
                        recycleWorker = recycleWorker || byName;
                    }
                    if (!ce::process_loopback::WritePacket(mapping, packet)) {
                        header->lastError.store(ERROR_BUFFER_OVERFLOW, std::memory_order_release);
                        header->workerState.store(static_cast<uint32_t>(ce::process_loopback::WorkerState::Failed),
                                                  std::memory_order_release);
                        RelayWorkerLog(
                            "[Worker] Shared packet ring overrun; stopping instead of corrupting ordered audio");
                        exitCode = ERROR_BUFFER_OVERFLOW;
                        stopping = true;
                        break;
                    } else {
                        SetEvent(packetEvent);
                    }
                }

                if (recycleWorker) {
                    header->workerRecycleCount.fetch_add(1, std::memory_order_relaxed);
                    stopping = true;
                    header->workerState.store(static_cast<uint32_t>(ce::process_loopback::WorkerState::Stopping),
                                              std::memory_order_release);
                    capture.Stop(false);
                }

                header->activeTargetPid.store(capture.GetTargetPID(), std::memory_order_release);
                header->heartbeatTick.store(GetTickCount64(), std::memory_order_release);
                if (!stopping) {
                    header->workerState.store(
                        static_cast<uint32_t>(capture.IsCapturing()
                                                  ? ce::process_loopback::WorkerState::Capturing
                                                  : capture.IsMonitoring()
                                                        ? ce::process_loopback::WorkerState::Monitoring
                                                        : ce::process_loopback::WorkerState::Starting),
                        std::memory_order_release);
                }
                if (!byName && !sawEndOfStream && !capture.IsCapturing() && capture.GetTargetPID() == 0) {
                    exitCode = ERROR_OPEN_FAILED;
                    header->lastError.store(ERROR_OPEN_FAILED, std::memory_order_release);
                    header->workerState.store(static_cast<uint32_t>(ce::process_loopback::WorkerState::Failed),
                                              std::memory_order_release);
                    RelayWorkerLog("[Worker] PID capture activation completed without an active stream");
                    stopping = true;
                }
                if (stopping || (!byName && sawEndOfStream && !capture.IsCapturing())) {
                    break;
                }

                const DWORD wait = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
                if (wait == WAIT_OBJECT_0) {
                    stopping = true;
                    header->workerState.store(static_cast<uint32_t>(ce::process_loopback::WorkerState::Stopping),
                                              std::memory_order_release);
                    capture.Stop(false);
                } else if (wait != WAIT_OBJECT_0 + 1) {
                    exitCode = wait == WAIT_FAILED ? static_cast<int>(GetLastError()) : ERROR_INVALID_FUNCTION;
                    header->lastError.store(static_cast<uint32_t>(exitCode), std::memory_order_release);
                    header->workerState.store(static_cast<uint32_t>(ce::process_loopback::WorkerState::Failed),
                                              std::memory_order_release);
                    RelayWorkerLog("[Worker] WaitForMultipleObjects failed");
                    capture.Stop(false);
                    stopping = true;
                }
            }
            capture.Stop(false);
            AudioPacket tailPacket;
            while (capture.GetNextPacket(tailPacket)) {
                if (!ce::process_loopback::WritePacket(mapping, tailPacket)) {
                    exitCode = ERROR_BUFFER_OVERFLOW;
                    header->lastError.store(ERROR_BUFFER_OVERFLOW, std::memory_order_release);
                    break;
                }
                SetEvent(packetEvent);
            }
            if (exitCode == ERROR_SUCCESS) {
                header->workerCleanExitCount.fetch_add(1, std::memory_order_relaxed);
                header->workerState.store(static_cast<uint32_t>(ce::process_loopback::WorkerState::CleanExit),
                                          std::memory_order_release);
            }
        }
    }

    header->heartbeatTick.store(GetTickCount64(), std::memory_order_release);
    MediaEngine_SetLogCallback(nullptr);
    g_WorkerLogRelay.store(nullptr, std::memory_order_release);
    SetEvent(packetEvent);
    UnmapViewOfFile(mapping);
    return exitCode;
}
