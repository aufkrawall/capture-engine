#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <utility>

#include "audio_capture.h"

namespace ce::process_loopback {

constexpr uint32_t kProtocolMagic = 0x504C4345;  // "ECLP"
constexpr uint32_t kProtocolVersion = 1;
constexpr uint32_t kPacketSlotCount = 256;
constexpr uint32_t kPacketPayloadBytes = 256 * 1024;
constexpr uint32_t kDiagnosticSlotCount = 64;
constexpr uint32_t kDiagnosticPayloadBytes = 1024;

static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "The process-loopback shared-memory protocol requires lock-free 64-bit atomics");

enum class WorkerState : uint32_t {
    Empty,
    Starting,
    Monitoring,
    Capturing,
    Stopping,
    CleanExit,
    Failed,
};

enum class WorkerExitDisposition : uint8_t {
    Final,
    Restart,
};

inline uint64_t ComputeWorkerRestartDelayMs(uint32_t consecutiveFailures) {
    const uint32_t shift = std::min<uint32_t>(consecutiveFailures, 6);
    return std::min<uint64_t>(5000, 100ull << shift);
}

inline WorkerExitDisposition ClassifyWorkerExit(bool captureDesired, bool stopRequested, bool cleanExit,
                                                 bool recycleRequested = false) {
    return captureDesired && !stopRequested && (!cleanExit || recycleRequested) ? WorkerExitDisposition::Restart
                                                                                : WorkerExitDisposition::Final;
}

struct alignas(64) SharedHeader {
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t slotCount = 0;
    uint32_t payloadBytes = 0;
    uint64_t workerGeneration = 0;
    std::atomic<uint64_t> writeSequence{0};
    std::atomic<uint64_t> readSequence{0};
    std::atomic<uint64_t> producedPackets{0};
    std::atomic<uint64_t> consumedPackets{0};
    std::atomic<uint64_t> overrunPackets{0};
    std::atomic<uint64_t> overrunFrames{0};
    std::atomic<uint64_t> oversizedPackets{0};
    std::atomic<uint64_t> workerPid{0};
    std::atomic<uint64_t> workerStartCount{0};
    std::atomic<uint64_t> workerCleanExitCount{0};
    std::atomic<uint64_t> workerRecycleCount{0};
    std::atomic<uint64_t> lifecycleOverrunPackets{0};
    std::atomic<uint64_t> diagnosticWriteSequence{0};
    std::atomic<uint64_t> diagnosticReadSequence{0};
    std::atomic<uint64_t> diagnosticOverruns{0};
    std::atomic<uint64_t> heartbeatTick{0};
    std::atomic<uint64_t> activeTargetPid{0};
    std::atomic<uint32_t> workerState{static_cast<uint32_t>(WorkerState::Empty)};
    std::atomic<uint32_t> lastError{0};
};

struct alignas(64) PacketSlot {
    std::atomic<uint64_t> committedSequence{0};
    uint32_t dataSize = 0;
    int64_t timestamp = 0;
    int32_t channels = 0;
    int32_t sampleRate = 0;
    int32_t bitsPerSample = 0;
    int32_t blockAlign = 0;
    int32_t validBitsPerSample = 0;
    uint32_t channelMask = 0;
    uint8_t isFloat = 0;
    uint8_t recordType = 0;
    uint8_t endOfStream = 0;
    uint8_t reserved = 0;
    uint64_t devicePosition = 0;
    uint64_t qpcPosition = 0;
    uint64_t rawQpcPosition = 0;
    uint64_t streamLatency = 0;
    uint64_t captureEpoch = 0;
};

struct alignas(64) DiagnosticSlot {
    std::atomic<uint64_t> committedSequence{0};
    uint32_t size = 0;
    char text[kDiagnosticPayloadBytes]{};
};

constexpr size_t MappingBytes() {
    return sizeof(SharedHeader) + sizeof(PacketSlot) * kPacketSlotCount +
           static_cast<size_t>(kPacketPayloadBytes) * kPacketSlotCount +
           sizeof(DiagnosticSlot) * kDiagnosticSlotCount;
}

inline PacketSlot* Slots(void* mapping) {
    return reinterpret_cast<PacketSlot*>(static_cast<uint8_t*>(mapping) + sizeof(SharedHeader));
}

inline const PacketSlot* Slots(const void* mapping) {
    return reinterpret_cast<const PacketSlot*>(static_cast<const uint8_t*>(mapping) + sizeof(SharedHeader));
}

inline uint8_t* Payload(void* mapping, uint32_t slotIndex) {
    return static_cast<uint8_t*>(mapping) + sizeof(SharedHeader) + sizeof(PacketSlot) * kPacketSlotCount +
           static_cast<size_t>(slotIndex) * kPacketPayloadBytes;
}

inline const uint8_t* Payload(const void* mapping, uint32_t slotIndex) {
    return static_cast<const uint8_t*>(mapping) + sizeof(SharedHeader) + sizeof(PacketSlot) * kPacketSlotCount +
           static_cast<size_t>(slotIndex) * kPacketPayloadBytes;
}

inline DiagnosticSlot* DiagnosticSlots(void* mapping) {
    return reinterpret_cast<DiagnosticSlot*>(static_cast<uint8_t*>(mapping) + sizeof(SharedHeader) +
                                             sizeof(PacketSlot) * kPacketSlotCount +
                                             static_cast<size_t>(kPacketPayloadBytes) * kPacketSlotCount);
}

inline const DiagnosticSlot* DiagnosticSlots(const void* mapping) {
    return reinterpret_cast<const DiagnosticSlot*>(static_cast<const uint8_t*>(mapping) + sizeof(SharedHeader) +
                                                   sizeof(PacketSlot) * kPacketSlotCount +
                                                   static_cast<size_t>(kPacketPayloadBytes) * kPacketSlotCount);
}

inline SharedHeader* Initialize(void* mapping, uint64_t workerGeneration) {
    if (!mapping) {
        return nullptr;
    }
    std::memset(mapping, 0, MappingBytes());
    auto* header = new (mapping) SharedHeader();
    header->magic = kProtocolMagic;
    header->version = kProtocolVersion;
    header->slotCount = kPacketSlotCount;
    header->payloadBytes = kPacketPayloadBytes;
    header->workerGeneration = workerGeneration;
    for (uint32_t index = 0; index < kPacketSlotCount; ++index) {
        new (&Slots(mapping)[index]) PacketSlot();
    }
    for (uint32_t index = 0; index < kDiagnosticSlotCount; ++index) {
        new (&DiagnosticSlots(mapping)[index]) DiagnosticSlot();
    }
    return header;
}

inline bool Validate(const void* mapping, uint64_t workerGeneration) {
    if (!mapping) {
        return false;
    }
    const auto* header = static_cast<const SharedHeader*>(mapping);
    return header->magic == kProtocolMagic && header->version == kProtocolVersion &&
           header->slotCount == kPacketSlotCount && header->payloadBytes == kPacketPayloadBytes &&
           header->workerGeneration == workerGeneration;
}

inline bool WritePacket(void* mapping, const AudioPacket& packet) {
    if (!Validate(mapping, static_cast<SharedHeader*>(mapping)->workerGeneration)) {
        return false;
    }
    auto* header = static_cast<SharedHeader*>(mapping);
    if (packet.data.size() > kPacketPayloadBytes) {
        header->oversizedPackets.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const uint64_t writeSequence = header->writeSequence.load(std::memory_order_relaxed);
    const uint64_t readSequence = header->readSequence.load(std::memory_order_acquire);
    if (writeSequence >= readSequence && writeSequence - readSequence >= kPacketSlotCount) {
        // The producer must never advance the consumer cursor: doing so can race an in-progress
        // payload copy and either overwrite it or move readSequence backwards when that copy commits.
        // Reject the new record, preserve every already-ordered record (especially EpochStart/EOS),
        // and let the worker fail loudly/restart rather than silently corrupting audio.
        header->overrunPackets.fetch_add(1, std::memory_order_relaxed);
        if (packet.recordType != AudioPacketRecordType::Data) {
            header->lifecycleOverrunPackets.fetch_add(1, std::memory_order_relaxed);
        } else if (packet.blockAlign > 0) {
            header->overrunFrames.fetch_add(packet.data.size() / static_cast<size_t>(packet.blockAlign),
                                            std::memory_order_relaxed);
        }
        return false;
    }

    const uint32_t slotIndex = static_cast<uint32_t>(writeSequence % kPacketSlotCount);
    PacketSlot& slot = Slots(mapping)[slotIndex];
    slot.dataSize = static_cast<uint32_t>(packet.data.size());
    slot.timestamp = packet.timestamp;
    slot.channels = packet.channels;
    slot.sampleRate = packet.sampleRate;
    slot.bitsPerSample = packet.bitsPerSample;
    slot.blockAlign = packet.blockAlign;
    slot.validBitsPerSample = packet.validBitsPerSample;
    slot.channelMask = packet.channelMask;
    slot.isFloat = packet.isFloat ? 1 : 0;
    slot.recordType = static_cast<uint8_t>(packet.recordType);
    slot.endOfStream = packet.endOfStream ? 1 : 0;
    slot.devicePosition = packet.devicePosition;
    slot.qpcPosition = packet.qpcPosition;
    slot.rawQpcPosition = packet.rawQpcPosition;
    slot.streamLatency = packet.streamLatency;
    slot.captureEpoch = packet.captureEpoch;
    if (!packet.data.empty()) {
        std::memcpy(Payload(mapping, slotIndex), packet.data.data(), packet.data.size());
    }
    slot.committedSequence.store(writeSequence + 1, std::memory_order_release);
    header->writeSequence.store(writeSequence + 1, std::memory_order_release);
    header->producedPackets.fetch_add(1, std::memory_order_relaxed);
    return true;
}

inline bool ReadPacket(void* mapping, AudioPacket& packet) {
    if (!mapping) {
        return false;
    }
    auto* header = static_cast<SharedHeader*>(mapping);
    if (!Validate(mapping, header->workerGeneration)) {
        return false;
    }
    uint64_t readSequence = header->readSequence.load(std::memory_order_relaxed);
    const uint64_t writeSequence = header->writeSequence.load(std::memory_order_acquire);
    if (readSequence >= writeSequence) {
        return false;
    }
    if (writeSequence - readSequence > kPacketSlotCount) {
        readSequence = writeSequence - kPacketSlotCount;
        header->readSequence.store(readSequence, std::memory_order_release);
    }

    const uint32_t slotIndex = static_cast<uint32_t>(readSequence % kPacketSlotCount);
    const PacketSlot& slot = Slots(mapping)[slotIndex];
    if (slot.committedSequence.load(std::memory_order_acquire) != readSequence + 1 ||
        slot.dataSize > kPacketPayloadBytes) {
        return false;
    }

    AudioPacket decoded;
    decoded.timestamp = slot.timestamp;
    decoded.channels = slot.channels;
    decoded.sampleRate = slot.sampleRate;
    decoded.bitsPerSample = slot.bitsPerSample;
    decoded.blockAlign = slot.blockAlign;
    decoded.validBitsPerSample = slot.validBitsPerSample;
    decoded.channelMask = slot.channelMask;
    decoded.isFloat = slot.isFloat != 0;
    decoded.recordType = static_cast<AudioPacketRecordType>(slot.recordType);
    if (decoded.recordType != AudioPacketRecordType::Data &&
        decoded.recordType != AudioPacketRecordType::EpochStart &&
        decoded.recordType != AudioPacketRecordType::EndOfStream) {
        return false;
    }
    decoded.endOfStream = slot.endOfStream != 0;
    decoded.devicePosition = slot.devicePosition;
    decoded.qpcPosition = slot.qpcPosition;
    decoded.rawQpcPosition = slot.rawQpcPosition;
    decoded.streamLatency = slot.streamLatency;
    decoded.captureEpoch = slot.captureEpoch;
    decoded.data.resize(slot.dataSize);
    if (slot.dataSize > 0) {
        std::memcpy(decoded.data.data(), Payload(mapping, slotIndex), slot.dataSize);
    }
    packet = std::move(decoded);
    header->readSequence.store(readSequence + 1, std::memory_order_release);
    header->consumedPackets.fetch_add(1, std::memory_order_relaxed);
    return true;
}

inline size_t PendingPacketCount(const void* mapping) {
    if (!mapping) {
        return 0;
    }
    const auto* header = static_cast<const SharedHeader*>(mapping);
    const uint64_t writeSequence = header->writeSequence.load(std::memory_order_acquire);
    const uint64_t readSequence = header->readSequence.load(std::memory_order_acquire);
    if (readSequence >= writeSequence) {
        return 0;
    }
    return static_cast<size_t>(std::min<uint64_t>(writeSequence - readSequence, kPacketSlotCount));
}

inline bool WriteDiagnostic(void* mapping, const char* message) {
    if (!mapping || !message) {
        return false;
    }
    auto* header = static_cast<SharedHeader*>(mapping);
    if (!Validate(mapping, header->workerGeneration)) {
        return false;
    }
    const uint64_t writeSequence = header->diagnosticWriteSequence.load(std::memory_order_relaxed);
    const uint64_t readSequence = header->diagnosticReadSequence.load(std::memory_order_acquire);
    if (writeSequence >= readSequence && writeSequence - readSequence >= kDiagnosticSlotCount) {
        header->diagnosticOverruns.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    DiagnosticSlot& slot = DiagnosticSlots(mapping)[writeSequence % kDiagnosticSlotCount];
    const size_t size = std::min<size_t>(std::strlen(message), kDiagnosticPayloadBytes - 1);
    std::memcpy(slot.text, message, size);
    slot.text[size] = '\0';
    slot.size = static_cast<uint32_t>(size);
    slot.committedSequence.store(writeSequence + 1, std::memory_order_release);
    header->diagnosticWriteSequence.store(writeSequence + 1, std::memory_order_release);
    return true;
}

inline bool ReadDiagnostic(void* mapping, std::string& message) {
    if (!mapping) {
        return false;
    }
    auto* header = static_cast<SharedHeader*>(mapping);
    if (!Validate(mapping, header->workerGeneration)) {
        return false;
    }
    uint64_t readSequence = header->diagnosticReadSequence.load(std::memory_order_relaxed);
    const uint64_t writeSequence = header->diagnosticWriteSequence.load(std::memory_order_acquire);
    if (readSequence >= writeSequence) {
        return false;
    }
    if (writeSequence - readSequence > kDiagnosticSlotCount) {
        readSequence = writeSequence - kDiagnosticSlotCount;
        header->diagnosticReadSequence.store(readSequence, std::memory_order_release);
    }
    const DiagnosticSlot& slot = DiagnosticSlots(mapping)[readSequence % kDiagnosticSlotCount];
    if (slot.committedSequence.load(std::memory_order_acquire) != readSequence + 1 ||
        slot.size >= kDiagnosticPayloadBytes) {
        return false;
    }
    message.assign(slot.text, slot.size);
    header->diagnosticReadSequence.store(readSequence + 1, std::memory_order_release);
    return true;
}

}  // namespace ce::process_loopback
