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

#include "layout_and_producer.h"

// Descriptor validation, the consumer read path, and diagnostics.

namespace ce::process_loopback {

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
            (descriptor.bitsPerSample != 16 && descriptor.bitsPerSample != 24 && descriptor.bitsPerSample != 32) ||
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

inline bool ReadPacket(void* mapping, ConsumerState& state, AudioPacket& packet) {
    if (!mapping) {
        return false;
    }
    auto* header = static_cast<SharedHeader*>(mapping);
    if (!Validate(mapping, header->workerGeneration) || HasFatalTransportFailure(mapping)) {
        return false;
    }
    if (!BindAndValidateConsumerState(*header, state)) {
        LatchTransportFailure(
            header, TransportStatus::CorruptCommittedMetadata, TransportFailureStage::ConsumerStateValidation,
            header->readSequence.load(std::memory_order_relaxed), ERROR_INVALID_STATE,
            SnapshotTransportFailureEvidence(*header, state.currentEpoch, state.lastEpoch));
        return false;
    }
    const uint64_t readSequence = state.nextSequence;
    const uint64_t writeSequence = header->writeSequence.load(std::memory_order_acquire);
    if (readSequence >= writeSequence) {
        return false;
    }
    if (writeSequence - readSequence > kDescriptorCount) {
        LatchTransportFailure(header, TransportStatus::CorruptCommittedMetadata,
                              TransportFailureStage::ConsumerSequenceWindow, readSequence, ERROR_INVALID_DATA,
                              SnapshotTransportFailureEvidence(*header, state.currentEpoch, state.lastEpoch));
        return false;
    }

    const PacketDescriptor& descriptor = Descriptors(mapping)[readSequence % kDescriptorCount];
    const uint64_t readByteSequence = state.nextByteSequence;
    const uint64_t writeByteSequence = header->writeByteSequence.load(std::memory_order_acquire);
    const uint64_t committedSequence = descriptor.committedSequence.load(std::memory_order_acquire);
    if (committedSequence != readSequence + 1 ||
        !ValidateDescriptor(*header, descriptor, readByteSequence, writeByteSequence)) {
        LatchTransportFailure(header, TransportStatus::CorruptCommittedMetadata,
                              TransportFailureStage::ConsumerDescriptorValidation, readSequence, ERROR_INVALID_DATA,
                              SnapshotTransportFailureEvidence(*header, state.currentEpoch, state.lastEpoch,
                                                               descriptor.captureEpoch, descriptor.recordType,
                                                               committedSequence));
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
        LatchTransportFailure(header, TransportStatus::ConsumerAllocationFailed,
                              TransportFailureStage::ConsumerAllocation, readSequence, ERROR_NOT_ENOUGH_MEMORY,
                              SnapshotTransportFailureEvidence(*header, state.currentEpoch, state.lastEpoch,
                                                               descriptor.captureEpoch, descriptor.recordType,
                                                               committedSequence));
        return false;
    }
    if (descriptor.dataSize > 0) {
        CopyFromByteRing(mapping, descriptor.payloadOffset, decoded.data.data(), descriptor.dataSize);
    }

    uint64_t nextCurrentEpoch = 0;
    uint64_t nextLastEpoch = 0;
    if (!ComputeNextEpochState(decoded.recordType, decoded.captureEpoch, state.currentEpoch, state.lastEpoch,
                               nextCurrentEpoch, nextLastEpoch)) {
        LatchTransportFailure(header, TransportStatus::CorruptCommittedMetadata,
                              TransportFailureStage::ConsumerEpochValidation, readSequence, ERROR_INVALID_DATA,
                              SnapshotTransportFailureEvidence(*header, state.currentEpoch, state.lastEpoch,
                                                               decoded.captureEpoch,
                                                               static_cast<uint32_t>(decoded.recordType),
                                                               committedSequence));
        return false;
    }

    static_assert(std::is_nothrow_move_assignable_v<AudioPacket>);
    packet = std::move(decoded);
    const uint64_t nextReadByteSequence = readByteSequence + descriptor.dataSize;
    // Publish ring capacity only after the output packet is fully materialized.
    // Lifecycle state is process-local and advances after both shared cursors, so
    // no other process can observe an applied epoch paired with the old cursor.
    header->readByteSequence.store(nextReadByteSequence, std::memory_order_release);
    header->readSequence.store(readSequence + 1, std::memory_order_release);
    header->consumedPackets.fetch_add(1, std::memory_order_relaxed);
    state.nextSequence = readSequence + 1;
    state.nextByteSequence = nextReadByteSequence;
    state.currentEpoch = nextCurrentEpoch;
    state.lastEpoch = nextLastEpoch;
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

inline void DiscardPackets(void* mapping, ConsumerState& state) {
    if (!mapping) {
        return;
    }
    auto* header = static_cast<SharedHeader*>(mapping);
    if (!Validate(mapping, header->workerGeneration) || HasFatalTransportFailure(mapping)) {
        return;
    }
    if (!BindAndValidateConsumerState(*header, state)) {
        LatchTransportFailure(
            header, TransportStatus::CorruptCommittedMetadata, TransportFailureStage::ConsumerStateValidation,
            header->readSequence.load(std::memory_order_relaxed), ERROR_INVALID_STATE,
            SnapshotTransportFailureEvidence(*header, state.currentEpoch, state.lastEpoch));
        return;
    }

    const uint64_t readSequence = state.nextSequence;
    const uint64_t writeSequence = header->writeSequence.load(std::memory_order_acquire);
    const uint64_t writeByteSequence = header->writeByteSequence.load(std::memory_order_acquire);
    uint64_t readByteSequence = state.nextByteSequence;
    uint64_t currentEpoch = state.currentEpoch;
    uint64_t lastEpoch = state.lastEpoch;
    if (writeSequence < readSequence || writeSequence - readSequence > kDescriptorCount) {
        LatchTransportFailure(header, TransportStatus::CorruptCommittedMetadata,
                              TransportFailureStage::DiscardSequenceWindow, readSequence, ERROR_INVALID_DATA,
                              SnapshotTransportFailureEvidence(*header, currentEpoch, lastEpoch));
        return;
    }

    for (uint64_t sequence = readSequence; sequence < writeSequence; ++sequence) {
        const PacketDescriptor& descriptor = Descriptors(mapping)[sequence % kDescriptorCount];
        const uint64_t committedSequence = descriptor.committedSequence.load(std::memory_order_acquire);
        if (committedSequence != sequence + 1 ||
            !ValidateDescriptor(*header, descriptor, readByteSequence, writeByteSequence)) {
            LatchTransportFailure(header, TransportStatus::CorruptCommittedMetadata,
                                  TransportFailureStage::DiscardDescriptorValidation, sequence, ERROR_INVALID_DATA,
                                  SnapshotTransportFailureEvidence(*header, currentEpoch, lastEpoch,
                                                                   descriptor.captureEpoch, descriptor.recordType,
                                                                   committedSequence));
            return;
        }

        const auto recordType = static_cast<AudioPacketRecordType>(descriptor.recordType);
        uint64_t nextCurrentEpoch = 0;
        uint64_t nextLastEpoch = 0;
        if (!ComputeNextEpochState(recordType, descriptor.captureEpoch, currentEpoch, lastEpoch, nextCurrentEpoch,
                                   nextLastEpoch)) {
            LatchTransportFailure(header, TransportStatus::CorruptCommittedMetadata,
                                  TransportFailureStage::DiscardEpochValidation, sequence, ERROR_INVALID_DATA,
                                  SnapshotTransportFailureEvidence(*header, currentEpoch, lastEpoch,
                                                                   descriptor.captureEpoch, descriptor.recordType,
                                                                   committedSequence));
            return;
        }
        currentEpoch = nextCurrentEpoch;
        lastEpoch = nextLastEpoch;
        readByteSequence += descriptor.dataSize;
    }

    header->readByteSequence.store(readByteSequence, std::memory_order_release);
    header->readSequence.store(writeSequence, std::memory_order_release);
    state.nextSequence = writeSequence;
    state.nextByteSequence = readByteSequence;
    state.currentEpoch = currentEpoch;
    state.lastEpoch = lastEpoch;
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
