#pragma once

#include <cstdint>
#include <limits>

namespace ce::process_loopback {

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
    PublishingFailure,
    DescriptorExhausted,
    ByteRingExhausted,
    CorruptCommittedMetadata,
    ConsumerAllocationFailed,
    ProducerPacketRejected,
};

enum class TransportFailureStage : uint32_t {
    None,
    ProducerStateValidation,
    ProducerPacketValidation,
    ProducerEpochValidation,
    ProducerDescriptorCapacity,
    ProducerByteCapacity,
    ConsumerStateValidation,
    ConsumerSequenceWindow,
    ConsumerDescriptorValidation,
    ConsumerEpochValidation,
    ConsumerAllocation,
    DiscardSequenceWindow,
    DiscardDescriptorValidation,
    DiscardEpochValidation,
};

inline const char* TransportStatusName(TransportStatus status) {
    switch (status) {
        case TransportStatus::Healthy:
            return "Healthy";
        case TransportStatus::PublishingFailure:
            return "PublishingFailure";
        case TransportStatus::DescriptorExhausted:
            return "DescriptorExhausted";
        case TransportStatus::ByteRingExhausted:
            return "ByteRingExhausted";
        case TransportStatus::CorruptCommittedMetadata:
            return "CorruptCommittedMetadata";
        case TransportStatus::ConsumerAllocationFailed:
            return "ConsumerAllocationFailed";
        case TransportStatus::ProducerPacketRejected:
            return "ProducerPacketRejected";
    }
    return "Unknown";
}

inline const char* TransportFailureStageName(TransportFailureStage stage) {
    switch (stage) {
        case TransportFailureStage::None:
            return "None";
        case TransportFailureStage::ProducerStateValidation:
            return "ProducerStateValidation";
        case TransportFailureStage::ProducerPacketValidation:
            return "ProducerPacketValidation";
        case TransportFailureStage::ProducerEpochValidation:
            return "ProducerEpochValidation";
        case TransportFailureStage::ProducerDescriptorCapacity:
            return "ProducerDescriptorCapacity";
        case TransportFailureStage::ProducerByteCapacity:
            return "ProducerByteCapacity";
        case TransportFailureStage::ConsumerStateValidation:
            return "ConsumerStateValidation";
        case TransportFailureStage::ConsumerSequenceWindow:
            return "ConsumerSequenceWindow";
        case TransportFailureStage::ConsumerDescriptorValidation:
            return "ConsumerDescriptorValidation";
        case TransportFailureStage::ConsumerEpochValidation:
            return "ConsumerEpochValidation";
        case TransportFailureStage::ConsumerAllocation:
            return "ConsumerAllocation";
        case TransportFailureStage::DiscardSequenceWindow:
            return "DiscardSequenceWindow";
        case TransportFailureStage::DiscardDescriptorValidation:
            return "DiscardDescriptorValidation";
        case TransportFailureStage::DiscardEpochValidation:
            return "DiscardEpochValidation";
    }
    return "Unknown";
}

struct ProducerState {
    uint64_t workerGeneration = 0;
    uint64_t nextSequence = 0;
    uint64_t nextByteSequence = 0;
    uint64_t currentEpoch = 0;
    uint64_t lastEpoch = 0;
};

struct ConsumerState {
    uint64_t workerGeneration = 0;
    uint64_t nextSequence = 0;
    uint64_t nextByteSequence = 0;
    uint64_t currentEpoch = 0;
    uint64_t lastEpoch = 0;
};

struct TransportFailureEvidence {
    uint64_t readSequence = 0;
    uint64_t writeSequence = 0;
    uint64_t readByteSequence = 0;
    uint64_t writeByteSequence = 0;
    uint64_t currentEpoch = 0;
    uint64_t lastEpoch = 0;
    uint64_t packetEpoch = 0;
    uint64_t committedSequence = 0;
    uint32_t recordType = std::numeric_limits<uint32_t>::max();
};

enum class WorkerExitDisposition : uint8_t {
    Final,
    Restart,
};

}  // namespace ce::process_loopback
