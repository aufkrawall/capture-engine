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
#include <type_traits>
#include <utility>

#include "../audio_capture.h"
#include "../process_loopback_protocol_types.h"

// Protocol constants, shared-ring layout, and the producer write path.

namespace ce::process_loopback {

constexpr uint32_t kProtocolMagic = 0x504C4345;  // "ECLP"
constexpr uint32_t kProtocolVersion = 4;
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
    std::atomic<uint32_t> transportFailureStage{static_cast<uint32_t>(TransportFailureStage::None)};
    std::atomic<uint32_t> transportStatus{static_cast<uint32_t>(TransportStatus::Healthy)};
    std::atomic<uint64_t> transportFailureSequence{0};
    std::atomic<uint64_t> failureReadSequence{0};
    std::atomic<uint64_t> failureWriteSequence{0};
    std::atomic<uint64_t> failureReadByteSequence{0};
    std::atomic<uint64_t> failureWriteByteSequence{0};
    std::atomic<uint64_t> failureCurrentEpoch{0};
    std::atomic<uint64_t> failureLastEpoch{0};
    std::atomic<uint64_t> failurePacketEpoch{0};
    std::atomic<uint64_t> failureCommittedSequence{0};
    std::atomic<uint32_t> failureRecordType{std::numeric_limits<uint32_t>::max()};
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
    layout = {sampleRate,         channels,      bitsPerSample, static_cast<uint32_t>(blockAlign),
              maximumPacketBytes, byteRingBytes, mappingBytes};
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

inline TransportFailureEvidence SnapshotTransportFailureEvidence(const SharedHeader& header,
                                                                  uint64_t currentEpoch = 0,
                                                                  uint64_t lastEpoch = 0,
                                                                  uint64_t packetEpoch = 0,
                                                                  uint32_t recordType =
                                                                      std::numeric_limits<uint32_t>::max(),
                                                                  uint64_t committedSequence = 0) {
    TransportFailureEvidence evidence;
    evidence.readSequence = header.readSequence.load(std::memory_order_acquire);
    evidence.writeSequence = header.writeSequence.load(std::memory_order_acquire);
    evidence.readByteSequence = header.readByteSequence.load(std::memory_order_acquire);
    evidence.writeByteSequence = header.writeByteSequence.load(std::memory_order_acquire);
    evidence.currentEpoch = currentEpoch;
    evidence.lastEpoch = lastEpoch;
    evidence.packetEpoch = packetEpoch;
    evidence.committedSequence = committedSequence;
    evidence.recordType = recordType;
    return evidence;
}

inline void LatchTransportFailure(SharedHeader* header, TransportStatus status, TransportFailureStage stage,
                                  uint64_t sequence, uint32_t error,
                                  const TransportFailureEvidence& evidence) {
    uint32_t expected = static_cast<uint32_t>(TransportStatus::Healthy);
    if (!header->transportStatus.compare_exchange_strong(
            expected, static_cast<uint32_t>(TransportStatus::PublishingFailure), std::memory_order_acq_rel)) {
        return;
    }
    header->transportFailureSequence.store(sequence, std::memory_order_relaxed);
    header->lastError.store(error, std::memory_order_relaxed);
    header->failureReadSequence.store(evidence.readSequence, std::memory_order_relaxed);
    header->failureWriteSequence.store(evidence.writeSequence, std::memory_order_relaxed);
    header->failureReadByteSequence.store(evidence.readByteSequence, std::memory_order_relaxed);
    header->failureWriteByteSequence.store(evidence.writeByteSequence, std::memory_order_relaxed);
    header->failureCurrentEpoch.store(evidence.currentEpoch, std::memory_order_relaxed);
    header->failureLastEpoch.store(evidence.lastEpoch, std::memory_order_relaxed);
    header->failurePacketEpoch.store(evidence.packetEpoch, std::memory_order_relaxed);
    header->failureCommittedSequence.store(evidence.committedSequence, std::memory_order_relaxed);
    header->failureRecordType.store(evidence.recordType, std::memory_order_relaxed);
    header->integrityFailureCount.fetch_add(1, std::memory_order_relaxed);
    header->transportFailureStage.store(static_cast<uint32_t>(stage), std::memory_order_release);
    // PublishingFailure is itself fatal if the publisher exits halfway through.
    // Replacing it with the final status is the evidence-complete publication edge.
    header->transportStatus.store(static_cast<uint32_t>(status), std::memory_order_release);
}

inline bool HasFatalTransportFailure(const void* mapping) {
    return mapping && static_cast<const SharedHeader*>(mapping)->transportStatus.load(std::memory_order_acquire) !=
                          static_cast<uint32_t>(TransportStatus::Healthy);
}

inline bool ComputeNextEpochState(AudioPacketRecordType recordType, uint64_t packetEpoch, uint64_t currentEpoch,
                                  uint64_t lastEpoch, uint64_t& nextCurrentEpoch, uint64_t& nextLastEpoch) {
    nextCurrentEpoch = currentEpoch;
    nextLastEpoch = lastEpoch;
    if (recordType == AudioPacketRecordType::EpochStart) {
        if (currentEpoch != 0 || packetEpoch <= lastEpoch) {
            return false;
        }
        nextCurrentEpoch = packetEpoch;
        nextLastEpoch = packetEpoch;
        return true;
    }
    if (recordType == AudioPacketRecordType::Data) {
        return currentEpoch != 0 && packetEpoch == currentEpoch;
    }
    if (recordType == AudioPacketRecordType::EndOfStream && currentEpoch != 0 && packetEpoch == currentEpoch) {
        nextCurrentEpoch = 0;
        return true;
    }
    return false;
}

inline bool BindAndValidateProducerState(SharedHeader& header, ProducerState& state) {
    if (state.workerGeneration == 0) {
        if (state.nextSequence != 0 || state.nextByteSequence != 0 || state.currentEpoch != 0 ||
            state.lastEpoch != 0) {
            return false;
        }
        state.workerGeneration = header.workerGeneration;
    }
    return state.workerGeneration == header.workerGeneration &&
           state.nextSequence == header.writeSequence.load(std::memory_order_relaxed) &&
           state.nextByteSequence == header.writeByteSequence.load(std::memory_order_relaxed);
}

inline bool BindAndValidateConsumerState(SharedHeader& header, ConsumerState& state) {
    if (state.workerGeneration == 0) {
        if (state.nextSequence != 0 || state.nextByteSequence != 0 || state.currentEpoch != 0 ||
            state.lastEpoch != 0 || header.readSequence.load(std::memory_order_relaxed) != 0 ||
            header.readByteSequence.load(std::memory_order_relaxed) != 0) {
            return false;
        }
        state.workerGeneration = header.workerGeneration;
    }
    return state.workerGeneration == header.workerGeneration &&
           state.nextSequence == header.readSequence.load(std::memory_order_relaxed) &&
           state.nextByteSequence == header.readByteSequence.load(std::memory_order_relaxed);
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
           packet.sampleRate == 0 && packet.bitsPerSample == 0 && packet.blockAlign == 0 &&
           packet.validBitsPerSample == 0 && packet.channelMask == 0 && !packet.isFloat && packet.devicePosition == 0 &&
           packet.qpcPosition == 0 && packet.rawQpcPosition == 0 && packet.streamLatency == 0 &&
           (packet.recordType == AudioPacketRecordType::EndOfStream) == packet.endOfStream;
}

// Process-loopback capture explicitly initializes WASAPI with the shared
// transport's format. Metadata copied back out of the mutable WAVEFORMATEX must
// therefore not redefine the packet contract after a process has joined late or
// the disposable worker has restarted. Canonicalize only non-payload metadata;
// a block-align or payload-shape mismatch remains fatal at the strict producer
// boundary below.
inline bool CanonicalizeProcessLoopbackDataPacket(const SharedHeader& header, uint32_t requestedChannelMask,
                                                  AudioPacket& packet, bool* changed = nullptr) {
    if (changed) {
        *changed = false;
    }
    if (packet.recordType != AudioPacketRecordType::Data) {
        return true;
    }
    if (header.requestedBitsPerSample != 32 || packet.channels != static_cast<int>(header.requestedChannels) ||
        packet.sampleRate != static_cast<int>(header.requestedSampleRate) || packet.bitsPerSample != 32 ||
        packet.data.empty()) {
        return false;
    }
    const uint32_t expectedBlockAlign = header.requestedChannels * (header.requestedBitsPerSample / 8u);
    if (expectedBlockAlign == 0 || packet.blockAlign != static_cast<int32_t>(expectedBlockAlign) ||
        packet.data.size() % expectedBlockAlign != 0 ||
        (requestedChannelMask != 0 && std::popcount(requestedChannelMask) != header.requestedChannels)) {
        return false;
    }
    const bool metadataChanged = packet.validBitsPerSample != 32 || packet.channelMask != requestedChannelMask ||
                                 !packet.isFloat;
    packet.validBitsPerSample = 32;
    packet.channelMask = requestedChannelMask;
    packet.isFloat = true;
    if (changed) {
        *changed = metadataChanged;
    }
    return true;
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

inline bool WritePacket(void* mapping, ProducerState& state, const AudioPacket& packet) {
    if (!mapping) {
        return false;
    }
    auto* header = static_cast<SharedHeader*>(mapping);
    if (!Validate(mapping, header->workerGeneration) || HasFatalTransportFailure(mapping)) {
        return false;
    }
    if (!BindAndValidateProducerState(*header, state)) {
        LatchTransportFailure(
            header, TransportStatus::ProducerPacketRejected, TransportFailureStage::ProducerStateValidation,
            header->writeSequence.load(std::memory_order_relaxed), ERROR_INVALID_STATE,
            SnapshotTransportFailureEvidence(*header, state.currentEpoch, state.lastEpoch, packet.captureEpoch,
                                             static_cast<uint32_t>(packet.recordType)));
        return false;
    }
    if (packet.data.size() > header->maximumPacketBytes) {
        header->oversizedPackets.fetch_add(1, std::memory_order_relaxed);
        LatchTransportFailure(header, TransportStatus::ByteRingExhausted,
                              TransportFailureStage::ProducerByteCapacity,
                              header->writeSequence.load(std::memory_order_relaxed), ERROR_BUFFER_OVERFLOW,
                              SnapshotTransportFailureEvidence(*header, state.currentEpoch, state.lastEpoch,
                                                               packet.captureEpoch,
                                                               static_cast<uint32_t>(packet.recordType)));
        return false;
    }
    if (!ValidatePacketForTransport(*header, packet)) {
        LatchTransportFailure(header, TransportStatus::ProducerPacketRejected,
                              TransportFailureStage::ProducerPacketValidation,
                              header->writeSequence.load(std::memory_order_relaxed), ERROR_INVALID_DATA,
                              SnapshotTransportFailureEvidence(*header, state.currentEpoch, state.lastEpoch,
                                                               packet.captureEpoch,
                                                               static_cast<uint32_t>(packet.recordType)));
        return false;
    }

    uint64_t nextCurrentEpoch = 0;
    uint64_t nextLastEpoch = 0;
    if (!ComputeNextEpochState(packet.recordType, packet.captureEpoch, state.currentEpoch, state.lastEpoch,
                               nextCurrentEpoch, nextLastEpoch)) {
        LatchTransportFailure(header, TransportStatus::ProducerPacketRejected,
                              TransportFailureStage::ProducerEpochValidation,
                              header->writeSequence.load(std::memory_order_relaxed), ERROR_INVALID_DATA,
                              SnapshotTransportFailureEvidence(*header, state.currentEpoch, state.lastEpoch,
                                                               packet.captureEpoch,
                                                               static_cast<uint32_t>(packet.recordType)));
        return false;
    }

    const uint64_t writeSequence = state.nextSequence;
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
        LatchTransportFailure(header, TransportStatus::DescriptorExhausted,
                              TransportFailureStage::ProducerDescriptorCapacity, writeSequence,
                              ERROR_BUFFER_OVERFLOW,
                              SnapshotTransportFailureEvidence(*header, state.currentEpoch, state.lastEpoch,
                                                               packet.captureEpoch,
                                                               static_cast<uint32_t>(packet.recordType)));
        return false;
    }

    const uint64_t writeByteSequence = state.nextByteSequence;
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
        LatchTransportFailure(header, TransportStatus::ByteRingExhausted,
                              TransportFailureStage::ProducerByteCapacity, writeSequence, ERROR_BUFFER_OVERFLOW,
                              SnapshotTransportFailureEvidence(*header, state.currentEpoch, state.lastEpoch,
                                                               packet.captureEpoch,
                                                               static_cast<uint32_t>(packet.recordType)));
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
    state.nextSequence = writeSequence + 1;
    state.nextByteSequence = writeByteSequence + packet.data.size();
    state.currentEpoch = nextCurrentEpoch;
    state.lastEpoch = nextLastEpoch;
    return true;
}

}  // namespace ce::process_loopback
