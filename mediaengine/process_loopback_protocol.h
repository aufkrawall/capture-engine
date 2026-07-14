#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <utility>

#include "audio_capture.h"

namespace ce::process_loopback {

constexpr uint32_t kProtocolMagic = 0x504C4345;  // "ECLP"
constexpr uint32_t kProtocolVersion = 2;
constexpr uint32_t kDescriptorCount = 32768;  // 30 seconds at a 1 ms packet cadence plus lifecycle records.
constexpr uint32_t kRetentionSeconds = 30;
constexpr uint32_t kMaximumPacketSeconds = 2;
constexpr uint32_t kMinimumSampleRate = 8000;
constexpr uint32_t kMaximumSampleRate = 384000;
constexpr uint32_t kMaximumChannels = 8;
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

enum class TransportStatus : uint32_t {
    Healthy,
    DescriptorExhausted,
    ByteRingExhausted,
    CorruptCommittedMetadata,
    ConsumerAllocationFailed,
};

enum class WorkerExitDisposition : uint8_t {
    Final,
    Restart,
};

struct TransportLayout {
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    uint32_t bitsPerSample = 0;
    uint32_t blockAlign = 0;
    uint64_t maximumPacketBytes = 0;
    uint64_t byteRingBytes = 0;
    uint64_t mappingBytes = 0;
};

inline bool CheckedAdd(uint64_t left, uint64_t right, uint64_t& result) {
    if (left > std::numeric_limits<uint64_t>::max() - right) {
        return false;
    }
    result = left + right;
    return true;
}

inline bool CheckedMultiply(uint64_t left, uint64_t right, uint64_t& result) {
    if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

inline bool ComputeTransportLayout(uint32_t sampleRate, uint32_t channels, uint32_t bitsPerSample,
                                   TransportLayout& layout);

inline uint64_t ComputeWorkerRestartDelayMs(uint32_t consecutiveFailures) {
    const uint32_t shift = std::min<uint32_t>(consecutiveFailures, 6);
    return std::min<uint64_t>(5000, 100ull << shift);
}

inline WorkerExitDisposition ClassifyWorkerExit(bool captureDesired, bool stopRequested, bool cleanExit,
                                                 bool recycleRequested = false, bool integrityFailure = false) {
    if (integrityFailure) {
        return WorkerExitDisposition::Final;
    }
    return captureDesired && !stopRequested && (!cleanExit || recycleRequested) ? WorkerExitDisposition::Restart
                                                                                : WorkerExitDisposition::Final;
}

struct alignas(64) SharedHeader {
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t descriptorCount = 0;
    uint32_t byteRingBytes = 0;
    uint32_t maximumPacketBytes = 0;
    uint32_t requestedSampleRate = 0;
    uint32_t requestedChannels = 0;
    uint32_t requestedBitsPerSample = 0;
    uint64_t mappingBytes = 0;
    uint64_t workerGeneration = 0;
    std::atomic<uint64_t> writeSequence{0};
    std::atomic<uint64_t> readSequence{0};
    std::atomic<uint64_t> writeByteSequence{0};
    std::atomic<uint64_t> readByteSequence{0};
    std::atomic<uint64_t> consumerEpoch{0};
    std::atomic<uint64_t> lastConsumerEpoch{0};
    std::atomic<uint64_t> producedPackets{0};
    std::atomic<uint64_t> consumedPackets{0};
    std::atomic<uint64_t> overrunPackets{0};
    std::atomic<uint64_t> overrunFrames{0};
    std::atomic<uint64_t> oversizedPackets{0};
    std::atomic<uint64_t> integrityFailureCount{0};
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
    std::atomic<uint32_t> transportStatus{static_cast<uint32_t>(TransportStatus::Healthy)};
};

struct alignas(64) PacketDescriptor {
    std::atomic<uint64_t> committedSequence{0};
    uint64_t payloadSequence = 0;
    uint32_t payloadOffset = 0;
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

inline bool ComputeTransportLayout(uint32_t sampleRate, uint32_t channels, uint32_t bitsPerSample,
                                   TransportLayout& layout) {
    if (sampleRate < kMinimumSampleRate || sampleRate > kMaximumSampleRate || channels == 0 ||
        channels > kMaximumChannels || (bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32)) {
        return false;
    }
    uint64_t blockAlign = 0;
    uint64_t bytesPerSecond = 0;
    uint64_t retentionBytes = 0;
    uint64_t maximumPacketBytes = 0;
    uint64_t byteRingBytes = 0;
    uint64_t mappingBytes = sizeof(SharedHeader);
    if (!CheckedMultiply(channels, bitsPerSample / 8, blockAlign) ||
        !CheckedMultiply(sampleRate, blockAlign, bytesPerSecond) ||
        !CheckedMultiply(bytesPerSecond, kRetentionSeconds, retentionBytes) ||
        !CheckedMultiply(bytesPerSecond, kMaximumPacketSeconds, maximumPacketBytes) ||
        !CheckedAdd(retentionBytes, maximumPacketBytes, byteRingBytes) ||
        !CheckedAdd(mappingBytes, sizeof(PacketDescriptor) * static_cast<uint64_t>(kDescriptorCount), mappingBytes) ||
        !CheckedAdd(mappingBytes, byteRingBytes, mappingBytes) ||
        !CheckedAdd(mappingBytes, sizeof(DiagnosticSlot) * static_cast<uint64_t>(kDiagnosticSlotCount), mappingBytes) ||
        blockAlign > std::numeric_limits<uint32_t>::max() ||
        maximumPacketBytes > std::numeric_limits<uint32_t>::max() ||
        byteRingBytes > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    layout = {sampleRate, channels, bitsPerSample, static_cast<uint32_t>(blockAlign), maximumPacketBytes,
              byteRingBytes, mappingBytes};
    return true;
}

inline uint64_t MappingBytes(uint32_t sampleRate = 48000, uint32_t channels = 2, uint32_t bitsPerSample = 32) {
    TransportLayout layout;
    return ComputeTransportLayout(sampleRate, channels, bitsPerSample, layout) ? layout.mappingBytes : 0;
}

inline PacketDescriptor* Descriptors(void* mapping) {
    return reinterpret_cast<PacketDescriptor*>(static_cast<uint8_t*>(mapping) + sizeof(SharedHeader));
}

inline const PacketDescriptor* Descriptors(const void* mapping) {
    return reinterpret_cast<const PacketDescriptor*>(static_cast<const uint8_t*>(mapping) + sizeof(SharedHeader));
}

inline uint8_t* ByteRing(void* mapping) {
    return static_cast<uint8_t*>(mapping) + sizeof(SharedHeader) + sizeof(PacketDescriptor) * kDescriptorCount;
}

inline const uint8_t* ByteRing(const void* mapping) {
    return static_cast<const uint8_t*>(mapping) + sizeof(SharedHeader) + sizeof(PacketDescriptor) * kDescriptorCount;
}

inline DiagnosticSlot* DiagnosticSlots(void* mapping) {
    auto* header = static_cast<SharedHeader*>(mapping);
    return reinterpret_cast<DiagnosticSlot*>(ByteRing(mapping) + header->byteRingBytes);
}

inline const DiagnosticSlot* DiagnosticSlots(const void* mapping) {
    const auto* header = static_cast<const SharedHeader*>(mapping);
    return reinterpret_cast<const DiagnosticSlot*>(ByteRing(mapping) + header->byteRingBytes);
}

inline SharedHeader* Initialize(void* mapping, uint64_t workerGeneration, uint32_t sampleRate = 48000,
                                uint32_t channels = 2, uint32_t bitsPerSample = 32) {
    TransportLayout layout;
    if (!mapping || !ComputeTransportLayout(sampleRate, channels, bitsPerSample, layout)) {
        return nullptr;
    }
    std::memset(mapping, 0, static_cast<size_t>(layout.mappingBytes));
    auto* header = new (mapping) SharedHeader();
    header->magic = kProtocolMagic;
    header->version = kProtocolVersion;
    header->descriptorCount = kDescriptorCount;
    header->byteRingBytes = static_cast<uint32_t>(layout.byteRingBytes);
    header->maximumPacketBytes = static_cast<uint32_t>(layout.maximumPacketBytes);
    header->requestedSampleRate = sampleRate;
    header->requestedChannels = channels;
    header->requestedBitsPerSample = bitsPerSample;
    header->mappingBytes = layout.mappingBytes;
    header->workerGeneration = workerGeneration;
    for (uint32_t index = 0; index < kDescriptorCount; ++index) {
        new (&Descriptors(mapping)[index]) PacketDescriptor();
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
    TransportLayout layout;
    return header->magic == kProtocolMagic && header->version == kProtocolVersion &&
           header->descriptorCount == kDescriptorCount &&
           ComputeTransportLayout(header->requestedSampleRate, header->requestedChannels,
                                  header->requestedBitsPerSample, layout) &&
           header->byteRingBytes == layout.byteRingBytes && header->maximumPacketBytes == layout.maximumPacketBytes &&
           header->mappingBytes == layout.mappingBytes && header->workerGeneration == workerGeneration;
}

inline void LatchTransportFailure(SharedHeader* header, TransportStatus status) {
    uint32_t expected = static_cast<uint32_t>(TransportStatus::Healthy);
    if (header->transportStatus.compare_exchange_strong(expected, static_cast<uint32_t>(status),
                                                        std::memory_order_acq_rel)) {
        header->integrityFailureCount.fetch_add(1, std::memory_order_relaxed);
    }
}

inline bool HasFatalTransportFailure(const void* mapping) {
    return mapping && static_cast<const SharedHeader*>(mapping)->transportStatus.load(std::memory_order_acquire) !=
                          static_cast<uint32_t>(TransportStatus::Healthy);
}

inline bool ValidateDataFormat(const SharedHeader& header, const AudioPacket& packet) {
    if (packet.channels < 1 || packet.channels > static_cast<int>(kMaximumChannels) ||
        packet.channels != static_cast<int>(header.requestedChannels) ||
        packet.sampleRate != static_cast<int>(header.requestedSampleRate) ||
        (packet.bitsPerSample != 16 && packet.bitsPerSample != 24 && packet.bitsPerSample != 32) ||
        packet.bitsPerSample != static_cast<int>(header.requestedBitsPerSample)) {
        return false;
    }
    const uint32_t expectedBlockAlign =
        static_cast<uint32_t>(packet.channels) * (static_cast<uint32_t>(packet.bitsPerSample) / 8u);
    return packet.blockAlign == static_cast<int32_t>(expectedBlockAlign) && packet.blockAlign > 0 &&
           packet.validBitsPerSample >= 0 && packet.validBitsPerSample <= packet.bitsPerSample &&
           (!packet.isFloat || packet.bitsPerSample == 32) && !packet.data.empty() &&
           packet.data.size() % static_cast<size_t>(packet.blockAlign) == 0 && packet.captureEpoch != 0 &&
           (packet.channelMask == 0 || std::popcount(packet.channelMask) == packet.channels);
}

inline bool ValidatePacketForTransport(const SharedHeader& header, const AudioPacket& packet) {
    if (packet.recordType == AudioPacketRecordType::Data) {
        return !packet.endOfStream && ValidateDataFormat(header, packet);
    }
    if (packet.recordType != AudioPacketRecordType::EpochStart &&
        packet.recordType != AudioPacketRecordType::EndOfStream) {
        return false;
    }
    return packet.data.empty() && packet.captureEpoch != 0 && packet.timestamp == 0 && packet.channels == 0 &&
           packet.sampleRate == 0 &&
           packet.bitsPerSample == 0 && packet.blockAlign == 0 && packet.validBitsPerSample == 0 &&
           packet.channelMask == 0 && !packet.isFloat && packet.devicePosition == 0 && packet.qpcPosition == 0 &&
           packet.rawQpcPosition == 0 && packet.streamLatency == 0 &&
           (packet.recordType == AudioPacketRecordType::EndOfStream) == packet.endOfStream;
}

inline void CopyIntoByteRing(void* mapping, uint32_t offset, const uint8_t* data, uint32_t size) {
    const uint32_t ringBytes = static_cast<SharedHeader*>(mapping)->byteRingBytes;
    const uint32_t first = std::min<uint32_t>(size, ringBytes - offset);
    std::memcpy(ByteRing(mapping) + offset, data, first);
    if (first < size) {
        std::memcpy(ByteRing(mapping), data + first, size - first);
    }
}

inline void CopyFromByteRing(const void* mapping, uint32_t offset, uint8_t* data, uint32_t size) {
    const uint32_t ringBytes = static_cast<const SharedHeader*>(mapping)->byteRingBytes;
    const uint32_t first = std::min<uint32_t>(size, ringBytes - offset);
    std::memcpy(data, ByteRing(mapping) + offset, first);
    if (first < size) {
        std::memcpy(data + first, ByteRing(mapping), size - first);
    }
}

inline bool WritePacket(void* mapping, const AudioPacket& packet) {
    if (!mapping) {
        return false;
    }
    auto* header = static_cast<SharedHeader*>(mapping);
    if (!Validate(mapping, header->workerGeneration) || HasFatalTransportFailure(mapping)) {
        return false;
    }
    if (packet.data.size() > header->maximumPacketBytes) {
        header->oversizedPackets.fetch_add(1, std::memory_order_relaxed);
        LatchTransportFailure(header, TransportStatus::ByteRingExhausted);
        return false;
    }
    if (!ValidatePacketForTransport(*header, packet)) {
        LatchTransportFailure(header, TransportStatus::CorruptCommittedMetadata);
        header->lastError.store(ERROR_INVALID_DATA, std::memory_order_release);
        return false;
    }

    const uint64_t writeSequence = header->writeSequence.load(std::memory_order_relaxed);
    const uint64_t readSequence = header->readSequence.load(std::memory_order_acquire);
    if (writeSequence == std::numeric_limits<uint64_t>::max() || writeSequence < readSequence ||
        writeSequence - readSequence >= kDescriptorCount) {
        header->overrunPackets.fetch_add(1, std::memory_order_relaxed);
        if (packet.recordType != AudioPacketRecordType::Data) {
            header->lifecycleOverrunPackets.fetch_add(1, std::memory_order_relaxed);
        } else if (packet.blockAlign > 0) {
            header->overrunFrames.fetch_add(packet.data.size() / static_cast<size_t>(packet.blockAlign),
                                            std::memory_order_relaxed);
        }
        LatchTransportFailure(header, TransportStatus::DescriptorExhausted);
        return false;
    }

    const uint64_t writeByteSequence = header->writeByteSequence.load(std::memory_order_relaxed);
    const uint64_t readByteSequence = header->readByteSequence.load(std::memory_order_acquire);
    const uint64_t occupiedBytes =
        writeByteSequence >= readByteSequence ? writeByteSequence - readByteSequence : header->byteRingBytes + 1ULL;
    if (writeByteSequence < readByteSequence || occupiedBytes > header->byteRingBytes ||
        packet.data.size() > header->byteRingBytes - occupiedBytes ||
        packet.data.size() > std::numeric_limits<uint64_t>::max() - writeByteSequence) {
        header->overrunPackets.fetch_add(1, std::memory_order_relaxed);
        if (packet.recordType == AudioPacketRecordType::Data && packet.blockAlign > 0) {
            header->overrunFrames.fetch_add(packet.data.size() / static_cast<size_t>(packet.blockAlign),
                                            std::memory_order_relaxed);
        } else {
            header->lifecycleOverrunPackets.fetch_add(1, std::memory_order_relaxed);
        }
        LatchTransportFailure(header, TransportStatus::ByteRingExhausted);
        return false;
    }

    const uint32_t descriptorIndex = static_cast<uint32_t>(writeSequence % kDescriptorCount);
    PacketDescriptor& descriptor = Descriptors(mapping)[descriptorIndex];
    descriptor.payloadSequence = writeByteSequence;
    descriptor.payloadOffset = static_cast<uint32_t>(writeByteSequence % header->byteRingBytes);
    descriptor.dataSize = static_cast<uint32_t>(packet.data.size());
    descriptor.timestamp = packet.timestamp;
    descriptor.channels = packet.channels;
    descriptor.sampleRate = packet.sampleRate;
    descriptor.bitsPerSample = packet.bitsPerSample;
    descriptor.blockAlign = packet.blockAlign;
    descriptor.validBitsPerSample = packet.validBitsPerSample;
    descriptor.channelMask = packet.channelMask;
    descriptor.isFloat = packet.isFloat ? 1 : 0;
    descriptor.recordType = static_cast<uint8_t>(packet.recordType);
    descriptor.endOfStream = packet.endOfStream ? 1 : 0;
    descriptor.reserved = 0;
    descriptor.devicePosition = packet.devicePosition;
    descriptor.qpcPosition = packet.qpcPosition;
    descriptor.rawQpcPosition = packet.rawQpcPosition;
    descriptor.streamLatency = packet.streamLatency;
    descriptor.captureEpoch = packet.captureEpoch;
    if (!packet.data.empty()) {
        CopyIntoByteRing(mapping, descriptor.payloadOffset, packet.data.data(), descriptor.dataSize);
    }
    header->writeByteSequence.store(writeByteSequence + packet.data.size(), std::memory_order_release);
    descriptor.committedSequence.store(writeSequence + 1, std::memory_order_release);
    header->writeSequence.store(writeSequence + 1, std::memory_order_release);
    header->producedPackets.fetch_add(1, std::memory_order_relaxed);
    return true;
}

inline bool ValidateDescriptor(const SharedHeader& header, const PacketDescriptor& descriptor,
                               uint64_t expectedByteSequence, uint64_t writeByteSequence) {
    if (descriptor.reserved != 0 || descriptor.isFloat > 1 || descriptor.endOfStream > 1 ||
        descriptor.dataSize > header.maximumPacketBytes || descriptor.dataSize > header.byteRingBytes ||
        descriptor.payloadSequence != expectedByteSequence ||
        descriptor.payloadOffset != expectedByteSequence % header.byteRingBytes ||
        descriptor.payloadSequence > writeByteSequence ||
        descriptor.dataSize > writeByteSequence - descriptor.payloadSequence) {
        return false;
    }
    const auto recordType = static_cast<AudioPacketRecordType>(descriptor.recordType);
    if (recordType == AudioPacketRecordType::Data) {
        if (descriptor.channels < 1 || descriptor.channels > static_cast<int>(kMaximumChannels) ||
            descriptor.channels != static_cast<int>(header.requestedChannels) ||
            descriptor.sampleRate != static_cast<int>(header.requestedSampleRate) ||
            (descriptor.bitsPerSample != 16 && descriptor.bitsPerSample != 24 &&
             descriptor.bitsPerSample != 32) ||
            descriptor.bitsPerSample != static_cast<int>(header.requestedBitsPerSample)) {
            return false;
        }
        const uint32_t expectedBlockAlign =
            static_cast<uint32_t>(descriptor.channels) * (static_cast<uint32_t>(descriptor.bitsPerSample) / 8u);
        return descriptor.blockAlign == static_cast<int32_t>(expectedBlockAlign) && descriptor.blockAlign > 0 &&
               descriptor.validBitsPerSample >= 0 && descriptor.validBitsPerSample <= descriptor.bitsPerSample &&
               (!descriptor.isFloat || descriptor.bitsPerSample == 32) && descriptor.dataSize != 0 &&
               descriptor.dataSize % static_cast<uint32_t>(descriptor.blockAlign) == 0 &&
               descriptor.captureEpoch != 0 && descriptor.endOfStream == 0 &&
               (descriptor.channelMask == 0 || std::popcount(descriptor.channelMask) == descriptor.channels);
    }
    if (recordType != AudioPacketRecordType::EpochStart && recordType != AudioPacketRecordType::EndOfStream) {
        return false;
    }
    return descriptor.dataSize == 0 && descriptor.timestamp == 0 && descriptor.channels == 0 &&
           descriptor.sampleRate == 0 && descriptor.bitsPerSample == 0 && descriptor.blockAlign == 0 &&
           descriptor.validBitsPerSample == 0 && descriptor.channelMask == 0 && descriptor.isFloat == 0 &&
           descriptor.devicePosition == 0 && descriptor.qpcPosition == 0 && descriptor.rawQpcPosition == 0 &&
           descriptor.streamLatency == 0 && descriptor.captureEpoch != 0 &&
           (recordType == AudioPacketRecordType::EndOfStream) == (descriptor.endOfStream != 0);
}

inline bool ReadPacket(void* mapping, AudioPacket& packet) {
    if (!mapping) {
        return false;
    }
    auto* header = static_cast<SharedHeader*>(mapping);
    if (!Validate(mapping, header->workerGeneration) || HasFatalTransportFailure(mapping)) {
        return false;
    }
    const uint64_t readSequence = header->readSequence.load(std::memory_order_relaxed);
    const uint64_t writeSequence = header->writeSequence.load(std::memory_order_acquire);
    if (readSequence >= writeSequence) {
        return false;
    }
    if (writeSequence - readSequence > kDescriptorCount) {
        LatchTransportFailure(header, TransportStatus::CorruptCommittedMetadata);
        return false;
    }

    const PacketDescriptor& descriptor = Descriptors(mapping)[readSequence % kDescriptorCount];
    const uint64_t readByteSequence = header->readByteSequence.load(std::memory_order_relaxed);
    const uint64_t writeByteSequence = header->writeByteSequence.load(std::memory_order_acquire);
    if (descriptor.committedSequence.load(std::memory_order_acquire) != readSequence + 1 ||
        !ValidateDescriptor(*header, descriptor, readByteSequence, writeByteSequence)) {
        LatchTransportFailure(header, TransportStatus::CorruptCommittedMetadata);
        header->lastError.store(ERROR_INVALID_DATA, std::memory_order_release);
        return false;
    }

    AudioPacket decoded;
    decoded.timestamp = descriptor.timestamp;
    decoded.channels = descriptor.channels;
    decoded.sampleRate = descriptor.sampleRate;
    decoded.bitsPerSample = descriptor.bitsPerSample;
    decoded.blockAlign = descriptor.blockAlign;
    decoded.validBitsPerSample = descriptor.validBitsPerSample;
    decoded.channelMask = descriptor.channelMask;
    decoded.isFloat = descriptor.isFloat != 0;
    decoded.recordType = static_cast<AudioPacketRecordType>(descriptor.recordType);
    decoded.endOfStream = descriptor.endOfStream != 0;
    decoded.devicePosition = descriptor.devicePosition;
    decoded.qpcPosition = descriptor.qpcPosition;
    decoded.rawQpcPosition = descriptor.rawQpcPosition;
    decoded.streamLatency = descriptor.streamLatency;
    decoded.captureEpoch = descriptor.captureEpoch;
    try {
        decoded.data.resize(descriptor.dataSize);
    } catch (...) {
        LatchTransportFailure(header, TransportStatus::ConsumerAllocationFailed);
        header->lastError.store(ERROR_NOT_ENOUGH_MEMORY, std::memory_order_release);
        return false;
    }
    if (descriptor.dataSize > 0) {
        CopyFromByteRing(mapping, descriptor.payloadOffset, decoded.data.data(), descriptor.dataSize);
    }

    const uint64_t currentEpoch = header->consumerEpoch.load(std::memory_order_relaxed);
    bool epochValid = true;
    const uint64_t lastEpoch = header->lastConsumerEpoch.load(std::memory_order_relaxed);
    if (decoded.recordType == AudioPacketRecordType::EpochStart) {
        epochValid = currentEpoch == 0 && decoded.captureEpoch > lastEpoch;
        if (epochValid) {
            header->consumerEpoch.store(decoded.captureEpoch, std::memory_order_relaxed);
            header->lastConsumerEpoch.store(decoded.captureEpoch, std::memory_order_relaxed);
        }
    } else if (decoded.recordType == AudioPacketRecordType::Data) {
        epochValid = currentEpoch != 0 && decoded.captureEpoch == currentEpoch;
    } else {
        epochValid = currentEpoch != 0 && decoded.captureEpoch == currentEpoch;
        if (epochValid)
            header->consumerEpoch.store(0, std::memory_order_relaxed);
    }
    if (!epochValid) {
        LatchTransportFailure(header, TransportStatus::CorruptCommittedMetadata);
        header->lastError.store(ERROR_INVALID_DATA, std::memory_order_release);
        return false;
    }

    packet = std::move(decoded);
    header->readByteSequence.store(readByteSequence + descriptor.dataSize, std::memory_order_release);
    header->readSequence.store(readSequence + 1, std::memory_order_release);
    header->consumedPackets.fetch_add(1, std::memory_order_relaxed);
    return true;
}

inline size_t PendingPacketCount(const void* mapping) {
    if (!mapping || HasFatalTransportFailure(mapping)) {
        return 0;
    }
    const auto* header = static_cast<const SharedHeader*>(mapping);
    const uint64_t writeSequence = header->writeSequence.load(std::memory_order_acquire);
    const uint64_t readSequence = header->readSequence.load(std::memory_order_acquire);
    if (readSequence >= writeSequence) {
        return 0;
    }
    return static_cast<size_t>(std::min<uint64_t>(writeSequence - readSequence, kDescriptorCount));
}

inline void DiscardPackets(void* mapping) {
    if (!mapping) {
        return;
    }
    auto* header = static_cast<SharedHeader*>(mapping);
    if (!Validate(mapping, header->workerGeneration) || HasFatalTransportFailure(mapping)) {
        return;
    }

    const uint64_t readSequence = header->readSequence.load(std::memory_order_relaxed);
    const uint64_t writeSequence = header->writeSequence.load(std::memory_order_acquire);
    const uint64_t writeByteSequence = header->writeByteSequence.load(std::memory_order_acquire);
    uint64_t readByteSequence = header->readByteSequence.load(std::memory_order_relaxed);
    uint64_t currentEpoch = header->consumerEpoch.load(std::memory_order_relaxed);
    uint64_t lastEpoch = header->lastConsumerEpoch.load(std::memory_order_relaxed);
    if (writeSequence < readSequence || writeSequence - readSequence > kDescriptorCount) {
        LatchTransportFailure(header, TransportStatus::CorruptCommittedMetadata);
        header->lastError.store(ERROR_INVALID_DATA, std::memory_order_release);
        return;
    }

    for (uint64_t sequence = readSequence; sequence < writeSequence; ++sequence) {
        const PacketDescriptor& descriptor = Descriptors(mapping)[sequence % kDescriptorCount];
        if (descriptor.committedSequence.load(std::memory_order_acquire) != sequence + 1 ||
            !ValidateDescriptor(*header, descriptor, readByteSequence, writeByteSequence)) {
            LatchTransportFailure(header, TransportStatus::CorruptCommittedMetadata);
            header->lastError.store(ERROR_INVALID_DATA, std::memory_order_release);
            return;
        }

        const auto recordType = static_cast<AudioPacketRecordType>(descriptor.recordType);
        bool epochValid = true;
        if (recordType == AudioPacketRecordType::EpochStart) {
            epochValid = currentEpoch == 0 && descriptor.captureEpoch > lastEpoch;
            if (epochValid) {
                currentEpoch = descriptor.captureEpoch;
                lastEpoch = descriptor.captureEpoch;
            }
        } else if (recordType == AudioPacketRecordType::Data) {
            epochValid = currentEpoch != 0 && descriptor.captureEpoch == currentEpoch;
        } else {
            epochValid = currentEpoch != 0 && descriptor.captureEpoch == currentEpoch;
            if (epochValid) {
                currentEpoch = 0;
            }
        }
        if (!epochValid) {
            LatchTransportFailure(header, TransportStatus::CorruptCommittedMetadata);
            header->lastError.store(ERROR_INVALID_DATA, std::memory_order_release);
            return;
        }
        readByteSequence += descriptor.dataSize;
    }

    header->consumerEpoch.store(currentEpoch, std::memory_order_relaxed);
    header->lastConsumerEpoch.store(lastEpoch, std::memory_order_relaxed);
    header->readByteSequence.store(readByteSequence, std::memory_order_release);
    header->readSequence.store(writeSequence, std::memory_order_release);
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
    if (writeSequence < readSequence || writeSequence - readSequence >= kDiagnosticSlotCount) {
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
    const uint64_t readSequence = header->diagnosticReadSequence.load(std::memory_order_relaxed);
    const uint64_t writeSequence = header->diagnosticWriteSequence.load(std::memory_order_acquire);
    if (readSequence >= writeSequence) {
        return false;
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
