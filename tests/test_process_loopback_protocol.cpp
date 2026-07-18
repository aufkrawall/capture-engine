#include <gtest/gtest.h>

#include "process_loopback_protocol.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {

class ProtocolMapping {
public:
    explicit ProtocolMapping(uint32_t channels = 2) : bytes(ce::process_loopback::MappingBytes(48000, channels, 32)) {
        data = VirtualAlloc(nullptr, static_cast<SIZE_T>(bytes), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    ~ProtocolMapping() {
        if (data) {
            VirtualFree(data, 0, MEM_RELEASE);
        }
    }
    bool Write(const AudioPacket& packet) {
        return ce::process_loopback::WritePacket(data, producerState, packet);
    }
    bool Read(AudioPacket& packet) {
        return ce::process_loopback::ReadPacket(data, consumerState, packet);
    }
    void Discard() {
        ce::process_loopback::DiscardPackets(data, consumerState);
    }
    uint64_t bytes = 0;
    void* data = nullptr;
    ce::process_loopback::ProducerState producerState;
    ce::process_loopback::ConsumerState consumerState;
};

AudioPacket DataPacket(uint8_t value, size_t frames = 1) {
    AudioPacket packet;
    packet.recordType = AudioPacketRecordType::Data;
    packet.channels = 2;
    packet.sampleRate = 48000;
    packet.bitsPerSample = 32;
    packet.blockAlign = 8;
    packet.captureEpoch = 3;
    packet.data.assign(frames * static_cast<size_t>(packet.blockAlign), value);
    return packet;
}

AudioPacket LifecyclePacket(AudioPacketRecordType type, uint64_t epoch) {
    AudioPacket packet;
    packet.recordType = type;
    packet.captureEpoch = epoch;
    packet.endOfStream = type == AudioPacketRecordType::EndOfStream;
    return packet;
}

}  // namespace

TEST(ProcessLoopbackProtocolTest, OrderedRecordsRoundTripWithExactMetadata) {
    ProtocolMapping mapping;
    ASSERT_NE(mapping.data, nullptr);
    ASSERT_NE(ce::process_loopback::Initialize(mapping.data, 17), nullptr);
    ASSERT_TRUE(mapping.Write(LifecyclePacket(AudioPacketRecordType::EpochStart, 9)));
    AudioPacket input = DataPacket(0x5a, 48);
    input.captureEpoch = 9;
    input.qpcPosition = 123456;
    input.channelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    ASSERT_TRUE(mapping.Write(input));
    ASSERT_TRUE(mapping.Write(LifecyclePacket(AudioPacketRecordType::EndOfStream, 9)));

    AudioPacket output;
    ASSERT_TRUE(mapping.Read(output));
    EXPECT_EQ(output.recordType, AudioPacketRecordType::EpochStart);
    EXPECT_EQ(output.captureEpoch, 9u);
    ASSERT_TRUE(mapping.Read(output));
    EXPECT_EQ(output.recordType, AudioPacketRecordType::Data);
    EXPECT_EQ(output.data, input.data);
    EXPECT_EQ(output.qpcPosition, 123456u);
    EXPECT_EQ(output.channelMask, input.channelMask);
    ASSERT_TRUE(mapping.Read(output));
    EXPECT_EQ(output.recordType, AudioPacketRecordType::EndOfStream);
    EXPECT_TRUE(output.endOfStream);
    EXPECT_FALSE(mapping.Read(output));
}

TEST(ProcessLoopbackProtocolTest, LateJoinPacketUsesImmutableRequestedFormatMetadata) {
    ProtocolMapping mapping;
    auto* header = ce::process_loopback::Initialize(mapping.data, 21);
    ASSERT_NE(header, nullptr);

    AudioPacket lateJoin = DataPacket(0, 480);
    lateJoin.captureEpoch = 1;
    lateJoin.validBitsPerSample = 0;
    lateJoin.channelMask = SPEAKER_FRONT_LEFT;
    lateJoin.isFloat = false;
    bool changed = false;
    ASSERT_TRUE(ce::process_loopback::CanonicalizeProcessLoopbackDataPacket(
        *header, SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT, lateJoin, &changed));
    EXPECT_TRUE(changed);
    EXPECT_EQ(lateJoin.blockAlign, 8);
    EXPECT_EQ(lateJoin.validBitsPerSample, 32);
    EXPECT_EQ(lateJoin.channelMask, SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    EXPECT_TRUE(lateJoin.isFloat);

    ASSERT_TRUE(mapping.Write(LifecyclePacket(AudioPacketRecordType::EpochStart, lateJoin.captureEpoch)));
    ASSERT_TRUE(mapping.Write(lateJoin));
    AudioPacket decoded;
    ASSERT_TRUE(mapping.Read(decoded));
    EXPECT_EQ(decoded.recordType, AudioPacketRecordType::EpochStart);
    ASSERT_TRUE(mapping.Read(decoded));
    EXPECT_EQ(decoded.data, lateJoin.data);
    EXPECT_FALSE(ce::process_loopback::HasFatalTransportFailure(mapping.data));
}

TEST(ProcessLoopbackProtocolTest, PayloadLayoutMismatchIsNotCanonicalized) {
    ProtocolMapping mapping;
    auto* header = ce::process_loopback::Initialize(mapping.data, 22);
    ASSERT_NE(header, nullptr);

    AudioPacket malformed = DataPacket(0, 480);
    malformed.blockAlign = 1;
    bool changed = false;
    EXPECT_FALSE(ce::process_loopback::CanonicalizeProcessLoopbackDataPacket(
        *header, SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT, malformed, &changed));
    EXPECT_FALSE(changed);
    EXPECT_EQ(malformed.blockAlign, 1);
}

TEST(ProcessLoopbackProtocolTest, DescriptorExhaustionLatchesFatalIntegrityFailure) {
    ProtocolMapping mapping;
    ASSERT_NE(ce::process_loopback::Initialize(mapping.data, 1), nullptr);
    ASSERT_TRUE(mapping.Write(LifecyclePacket(AudioPacketRecordType::EpochStart, 3)));
    for (uint32_t sequence = 1; sequence < ce::process_loopback::kDescriptorCount; ++sequence) {
        ASSERT_TRUE(mapping.Write(DataPacket(static_cast<uint8_t>(sequence))));
    }
    EXPECT_FALSE(mapping.Write(DataPacket(0xff)));
    auto* header = static_cast<ce::process_loopback::SharedHeader*>(mapping.data);
    EXPECT_EQ(header->overrunPackets.load(), 1u);
    EXPECT_EQ(header->transportStatus.load(),
              static_cast<uint32_t>(ce::process_loopback::TransportStatus::DescriptorExhausted));
    EXPECT_TRUE(ce::process_loopback::HasFatalTransportFailure(mapping.data));
}

TEST(ProcessLoopbackProtocolTest, ConsumedSlotsWrapWithoutOverrunOrReordering) {
    ProtocolMapping mapping;
    ASSERT_NE(ce::process_loopback::Initialize(mapping.data, 2), nullptr);
    AudioPacket output;
    ASSERT_TRUE(mapping.Write(LifecyclePacket(AudioPacketRecordType::EpochStart, 3)));
    ASSERT_TRUE(mapping.Read(output));
    for (uint32_t cycle = 0; cycle < 3; ++cycle) {
        for (uint32_t sequence = 0; sequence < ce::process_loopback::kDescriptorCount; ++sequence) {
            const uint8_t value = static_cast<uint8_t>(cycle * 17 + sequence);
            ASSERT_TRUE(mapping.Write(DataPacket(value)));
        }
        for (uint32_t sequence = 0; sequence < ce::process_loopback::kDescriptorCount; ++sequence) {
            const uint8_t value = static_cast<uint8_t>(cycle * 17 + sequence);
            ASSERT_TRUE(mapping.Read(output));
            ASSERT_FALSE(output.data.empty());
            EXPECT_EQ(output.data.front(), value);
        }
    }
    auto* header = static_cast<ce::process_loopback::SharedHeader*>(mapping.data);
    EXPECT_EQ(header->overrunPackets.load(), 0u);
    EXPECT_EQ(header->producedPackets.load(), 1u + 3u * ce::process_loopback::kDescriptorCount);
    EXPECT_EQ(header->consumedPackets.load(), header->producedPackets.load());
}

TEST(ProcessLoopbackProtocolTest, FullRingNeverDropsAnOrderedLifecycleHeadForNewData) {
    ProtocolMapping mapping;
    ASSERT_NE(ce::process_loopback::Initialize(mapping.data, 4), nullptr);
    ASSERT_TRUE(mapping.Write(LifecyclePacket(AudioPacketRecordType::EpochStart, 3)));
    for (uint32_t index = 1; index < ce::process_loopback::kDescriptorCount; ++index) {
        ASSERT_TRUE(mapping.Write(DataPacket(static_cast<uint8_t>(index))));
    }
    EXPECT_FALSE(mapping.Write(DataPacket(0xff, 5)));
    auto* header = static_cast<ce::process_loopback::SharedHeader*>(mapping.data);
    EXPECT_EQ(header->overrunPackets.load(), 1u);
    EXPECT_EQ(header->overrunFrames.load(), 5u);
    EXPECT_EQ(header->lifecycleOverrunPackets.load(), 0u);
    EXPECT_EQ(header->transportStatus.load(),
              static_cast<uint32_t>(ce::process_loopback::TransportStatus::DescriptorExhausted));
}

TEST(ProcessLoopbackProtocolTest, OversizedPacketIsRejectedAndCounted) {
    ProtocolMapping mapping;
    ASSERT_NE(ce::process_loopback::Initialize(mapping.data, 6), nullptr);
    AudioPacket packet = DataPacket(0);
    const auto* header = static_cast<ce::process_loopback::SharedHeader*>(mapping.data);
    packet.data.resize(static_cast<size_t>(header->maximumPacketBytes) + packet.blockAlign);
    EXPECT_FALSE(mapping.Write(packet));
    EXPECT_EQ(static_cast<ce::process_loopback::SharedHeader*>(mapping.data)->oversizedPackets.load(), 1u);
}

TEST(ProcessLoopbackProtocolTest, ByteRingExhaustionLatchesFatalIntegrityFailure) {
    ProtocolMapping mapping;
    auto* header = ce::process_loopback::Initialize(mapping.data, 7);
    ASSERT_NE(header, nullptr);
    ASSERT_TRUE(mapping.Write(LifecyclePacket(AudioPacketRecordType::EpochStart, 3)));
    header->writeByteSequence.store(header->byteRingBytes, std::memory_order_relaxed);
    mapping.producerState.nextByteSequence = header->byteRingBytes;
    EXPECT_FALSE(mapping.Write(DataPacket(0x11)));
    EXPECT_EQ(header->transportStatus.load(),
              static_cast<uint32_t>(ce::process_loopback::TransportStatus::ByteRingExhausted));
}

TEST(ProcessLoopbackProtocolTest, SequenceOverflowIsFatalInsteadOfWrapping) {
    ProtocolMapping mapping;
    auto* header = ce::process_loopback::Initialize(mapping.data, 9);
    ASSERT_NE(header, nullptr);
    header->readSequence.store(std::numeric_limits<uint64_t>::max(), std::memory_order_relaxed);
    header->writeSequence.store(std::numeric_limits<uint64_t>::max(), std::memory_order_relaxed);
    mapping.producerState.workerGeneration = 9;
    mapping.producerState.nextSequence = std::numeric_limits<uint64_t>::max();
    mapping.producerState.currentEpoch = 3;
    mapping.producerState.lastEpoch = 3;
    EXPECT_FALSE(mapping.Write(DataPacket(0x22)));
    EXPECT_TRUE(ce::process_loopback::HasFatalTransportFailure(mapping.data));
}

TEST(ProcessLoopbackProtocolTest, DiagnosticRingIsIndependentAndReportsOverrun) {
    ProtocolMapping mapping;
    ASSERT_NE(ce::process_loopback::Initialize(mapping.data, 8), nullptr);
    for (uint32_t index = 0; index < ce::process_loopback::kDiagnosticSlotCount; ++index) {
        ASSERT_TRUE(ce::process_loopback::WriteDiagnostic(mapping.data, std::to_string(index).c_str()));
    }
    for (uint32_t index = 0; index < 3; ++index) {
        EXPECT_FALSE(ce::process_loopback::WriteDiagnostic(mapping.data, "overflow"));
    }
    auto* header = static_cast<ce::process_loopback::SharedHeader*>(mapping.data);
    EXPECT_EQ(header->diagnosticOverruns.load(), 3u);
    EXPECT_EQ(header->producedPackets.load(), 0u);
    std::string message;
    ASSERT_TRUE(ce::process_loopback::ReadDiagnostic(mapping.data, message));
    EXPECT_EQ(message, "0");
}

TEST(ProcessLoopbackProtocolTest, WorkerRestartPolicyIsBoundedAndOnlyRestartsUnexpectedExit) {
    using ce::process_loopback::ClassifyWorkerExit;
    using ce::process_loopback::WorkerExitDisposition;
    EXPECT_EQ(ClassifyWorkerExit(true, false, false), WorkerExitDisposition::Restart);
    EXPECT_EQ(ClassifyWorkerExit(true, true, false), WorkerExitDisposition::Final);
    EXPECT_EQ(ClassifyWorkerExit(true, false, true), WorkerExitDisposition::Final);
    EXPECT_EQ(ClassifyWorkerExit(true, false, true, true), WorkerExitDisposition::Restart);
    EXPECT_EQ(ClassifyWorkerExit(true, false, false, false, true), WorkerExitDisposition::Final);
    EXPECT_EQ(ClassifyWorkerExit(false, false, false), WorkerExitDisposition::Final);
    EXPECT_EQ(ce::process_loopback::ComputeWorkerRestartDelayMs(0), 100u);
    EXPECT_EQ(ce::process_loopback::ComputeWorkerRestartDelayMs(1), 200u);
    EXPECT_EQ(ce::process_loopback::ComputeWorkerRestartDelayMs(20), 5000u);
}

TEST(ProcessLoopbackProtocolTest, MaximumLegalEightChannelPacketRoundTrips) {
    ProtocolMapping mapping(8);
    ASSERT_NE(ce::process_loopback::Initialize(mapping.data, 10, 48000, 8, 32), nullptr);
    AudioPacket packet;
    packet.recordType = AudioPacketRecordType::Data;
    packet.channels = 8;
    packet.sampleRate = 48000;
    packet.bitsPerSample = 32;
    packet.validBitsPerSample = 32;
    packet.blockAlign = 32;
    packet.isFloat = true;
    packet.captureEpoch = 1;
    packet.data.assign(48000u * 2u * 32u, 0x6d);
    ASSERT_TRUE(mapping.Write(LifecyclePacket(AudioPacketRecordType::EpochStart, 1)));
    ASSERT_TRUE(mapping.Write(packet));
    AudioPacket decoded;
    ASSERT_TRUE(mapping.Read(decoded));
    ASSERT_EQ(decoded.recordType, AudioPacketRecordType::EpochStart);
    ASSERT_TRUE(mapping.Read(decoded));
    EXPECT_EQ(decoded.data, packet.data);
}

TEST(ProcessLoopbackProtocolTest, VariableByteRingWrapPreservesPayload) {
    ProtocolMapping mapping;
    ASSERT_NE(ce::process_loopback::Initialize(mapping.data, 11), nullptr);
    const auto* header = static_cast<ce::process_loopback::SharedHeader*>(mapping.data);
    const size_t packetBytes = 700u * 1024u;
    ASSERT_LT(packetBytes, header->maximumPacketBytes);
    AudioPacket decoded;
    ASSERT_TRUE(mapping.Write(LifecyclePacket(AudioPacketRecordType::EpochStart, 3)));
    ASSERT_TRUE(mapping.Read(decoded));
    bool wrapped = false;
    uint32_t previousOffset = 0;
    for (uint32_t index = 0; index < 24; ++index) {
        AudioPacket packet = DataPacket(static_cast<uint8_t>(index), packetBytes / 8);
        ASSERT_TRUE(mapping.Write(packet));
        const auto& descriptor =
            ce::process_loopback::Descriptors(mapping.data)[(index + 1) % ce::process_loopback::kDescriptorCount];
        if (index > 0 && descriptor.payloadOffset < previousOffset) {
            wrapped = true;
        }
        previousOffset = descriptor.payloadOffset;
        ASSERT_TRUE(mapping.Read(decoded));
        ASSERT_EQ(decoded.data.size(), packetBytes);
        EXPECT_EQ(decoded.data.front(), static_cast<uint8_t>(index));
        EXPECT_EQ(decoded.data.back(), static_cast<uint8_t>(index));
    }
    EXPECT_TRUE(wrapped);
}

TEST(ProcessLoopbackProtocolTest, CorruptCommittedDescriptorIsFatal) {
    ProtocolMapping mapping;
    ASSERT_NE(ce::process_loopback::Initialize(mapping.data, 12), nullptr);
    ASSERT_TRUE(mapping.Write(LifecyclePacket(AudioPacketRecordType::EpochStart, 3)));
    ASSERT_TRUE(mapping.Write(DataPacket(1, 48)));
    ce::process_loopback::Descriptors(mapping.data)[1].payloadOffset++;
    AudioPacket decoded;
    ASSERT_TRUE(mapping.Read(decoded));
    EXPECT_FALSE(mapping.Read(decoded));
    EXPECT_EQ(static_cast<ce::process_loopback::SharedHeader*>(mapping.data)->transportStatus.load(),
              static_cast<uint32_t>(ce::process_loopback::TransportStatus::CorruptCommittedMetadata));
    EXPECT_EQ(static_cast<ce::process_loopback::SharedHeader*>(mapping.data)->transportFailureStage.load(),
              static_cast<uint32_t>(ce::process_loopback::TransportFailureStage::ConsumerDescriptorValidation));
    EXPECT_EQ(static_cast<ce::process_loopback::SharedHeader*>(mapping.data)->lastError.load(), ERROR_INVALID_DATA);
}

TEST(ProcessLoopbackProtocolTest, ExtremeCorruptFormatFieldsAreRejectedBeforeArithmetic) {
    ProtocolMapping mapping;
    ASSERT_NE(ce::process_loopback::Initialize(mapping.data, 20), nullptr);
    ASSERT_TRUE(mapping.Write(LifecyclePacket(AudioPacketRecordType::EpochStart, 3)));
    ASSERT_TRUE(mapping.Write(DataPacket(1, 48)));
    auto& descriptor = ce::process_loopback::Descriptors(mapping.data)[1];
    descriptor.channels = std::numeric_limits<int32_t>::max();
    descriptor.bitsPerSample = std::numeric_limits<int32_t>::max();
    AudioPacket decoded;
    ASSERT_TRUE(mapping.Read(decoded));
    EXPECT_FALSE(mapping.Read(decoded));
    EXPECT_EQ(static_cast<ce::process_loopback::SharedHeader*>(mapping.data)->transportStatus.load(),
              static_cast<uint32_t>(ce::process_loopback::TransportStatus::CorruptCommittedMetadata));
}

TEST(ProcessLoopbackProtocolTest, InvalidChannelMaskAndLifecycleMetadataAreFatal) {
    ProtocolMapping producerValidation;
    ASSERT_NE(ce::process_loopback::Initialize(producerValidation.data, 14), nullptr);
    AudioPacket invalidMask = DataPacket(1, 48);
    invalidMask.channelMask = SPEAKER_FRONT_LEFT;
    EXPECT_FALSE(producerValidation.Write(invalidMask));
    EXPECT_TRUE(ce::process_loopback::HasFatalTransportFailure(producerValidation.data));
    EXPECT_EQ(static_cast<ce::process_loopback::SharedHeader*>(producerValidation.data)->transportStatus.load(),
              static_cast<uint32_t>(ce::process_loopback::TransportStatus::ProducerPacketRejected));
    EXPECT_EQ(static_cast<ce::process_loopback::SharedHeader*>(producerValidation.data)->transportFailureStage.load(),
              static_cast<uint32_t>(ce::process_loopback::TransportFailureStage::ProducerPacketValidation));

    ProtocolMapping consumerValidation;
    ASSERT_NE(ce::process_loopback::Initialize(consumerValidation.data, 15), nullptr);
    ASSERT_TRUE(consumerValidation.Write(LifecyclePacket(AudioPacketRecordType::EpochStart, 1)));
    ce::process_loopback::Descriptors(consumerValidation.data)[0].channelMask = SPEAKER_FRONT_LEFT;
    AudioPacket decoded;
    EXPECT_FALSE(consumerValidation.Read(decoded));
    EXPECT_TRUE(ce::process_loopback::HasFatalTransportFailure(consumerValidation.data));
}

TEST(ProcessLoopbackProtocolTest, ProducerAndConsumerLifecycleEpochMismatchAreFatal) {
    ProtocolMapping mapping;
    ASSERT_NE(ce::process_loopback::Initialize(mapping.data, 13), nullptr);
    ASSERT_TRUE(mapping.Write(LifecyclePacket(AudioPacketRecordType::EpochStart, 1)));
    AudioPacket mismatched = DataPacket(2, 48);
    mismatched.captureEpoch = 2;
    EXPECT_FALSE(mapping.Write(mismatched));
    EXPECT_EQ(static_cast<ce::process_loopback::SharedHeader*>(mapping.data)->transportFailureStage.load(),
              static_cast<uint32_t>(ce::process_loopback::TransportFailureStage::ProducerEpochValidation));
    auto* producerHeader = static_cast<ce::process_loopback::SharedHeader*>(mapping.data);
    EXPECT_EQ(producerHeader->failureReadSequence.load(), 0u);
    EXPECT_EQ(producerHeader->failureWriteSequence.load(), 1u);
    EXPECT_EQ(producerHeader->failureCurrentEpoch.load(), 1u);
    EXPECT_EQ(producerHeader->failureLastEpoch.load(), 1u);
    EXPECT_EQ(producerHeader->failurePacketEpoch.load(), 2u);
    EXPECT_EQ(producerHeader->failureRecordType.load(), static_cast<uint32_t>(AudioPacketRecordType::Data));

    ProtocolMapping committedCorruption;
    ASSERT_NE(ce::process_loopback::Initialize(committedCorruption.data, 23), nullptr);
    ASSERT_TRUE(committedCorruption.Write(LifecyclePacket(AudioPacketRecordType::EpochStart, 1)));
    AudioPacket valid = DataPacket(2, 48);
    valid.captureEpoch = 1;
    ASSERT_TRUE(committedCorruption.Write(valid));
    ce::process_loopback::Descriptors(committedCorruption.data)[1].captureEpoch = 2;
    AudioPacket decoded;
    ASSERT_TRUE(committedCorruption.Read(decoded));
    EXPECT_FALSE(committedCorruption.Read(decoded));
    EXPECT_TRUE(ce::process_loopback::HasFatalTransportFailure(committedCorruption.data));
    EXPECT_EQ(static_cast<ce::process_loopback::SharedHeader*>(committedCorruption.data)
                  ->transportFailureStage.load(),
              static_cast<uint32_t>(ce::process_loopback::TransportFailureStage::ConsumerEpochValidation));
    auto* consumerHeader = static_cast<ce::process_loopback::SharedHeader*>(committedCorruption.data);
    EXPECT_EQ(consumerHeader->failureReadSequence.load(), 1u);
    EXPECT_EQ(consumerHeader->failureWriteSequence.load(), 2u);
    EXPECT_EQ(consumerHeader->failureCurrentEpoch.load(), 1u);
    EXPECT_EQ(consumerHeader->failureLastEpoch.load(), 1u);
    EXPECT_EQ(consumerHeader->failurePacketEpoch.load(), 2u);
    EXPECT_EQ(consumerHeader->failureCommittedSequence.load(), 2u);
}

TEST(ProcessLoopbackProtocolTest, DataAndEndOfStreamRequireAnOpenEpoch) {
    ProtocolMapping dataMapping;
    ASSERT_NE(ce::process_loopback::Initialize(dataMapping.data, 16), nullptr);
    EXPECT_FALSE(dataMapping.Write(DataPacket(3, 48)));
    EXPECT_TRUE(ce::process_loopback::HasFatalTransportFailure(dataMapping.data));
    EXPECT_EQ(static_cast<ce::process_loopback::SharedHeader*>(dataMapping.data)->transportFailureStage.load(),
              static_cast<uint32_t>(ce::process_loopback::TransportFailureStage::ProducerEpochValidation));

    ProtocolMapping endMapping;
    ASSERT_NE(ce::process_loopback::Initialize(endMapping.data, 17), nullptr);
    EXPECT_FALSE(endMapping.Write(LifecyclePacket(AudioPacketRecordType::EndOfStream, 1)));
    EXPECT_TRUE(ce::process_loopback::HasFatalTransportFailure(endMapping.data));
}

TEST(ProcessLoopbackProtocolTest, CompletedEpochCannotBeReused) {
    ProtocolMapping mapping;
    ASSERT_NE(ce::process_loopback::Initialize(mapping.data, 18), nullptr);
    ASSERT_TRUE(mapping.Write(LifecyclePacket(AudioPacketRecordType::EpochStart, 4)));
    ASSERT_TRUE(mapping.Write(LifecyclePacket(AudioPacketRecordType::EndOfStream, 4)));
    EXPECT_FALSE(mapping.Write(LifecyclePacket(AudioPacketRecordType::EpochStart, 4)));
    EXPECT_TRUE(ce::process_loopback::HasFatalTransportFailure(mapping.data));
}

TEST(ProcessLoopbackProtocolTest, DiscardPreservesLifecycleStateForContinuingCapture) {
    ProtocolMapping mapping;
    auto* header = ce::process_loopback::Initialize(mapping.data, 19);
    ASSERT_NE(header, nullptr);
    ASSERT_TRUE(mapping.Write(LifecyclePacket(AudioPacketRecordType::EpochStart, 7)));
    AudioPacket beforeDiscard = DataPacket(0x31);
    beforeDiscard.captureEpoch = 7;
    ASSERT_TRUE(mapping.Write(beforeDiscard));

    mapping.Discard();
    EXPECT_FALSE(ce::process_loopback::HasFatalTransportFailure(mapping.data));
    EXPECT_EQ(mapping.consumerState.currentEpoch, 7u);
    EXPECT_EQ(mapping.consumerState.lastEpoch, 7u);
    EXPECT_EQ(ce::process_loopback::PendingPacketCount(mapping.data), 0u);

    AudioPacket afterDiscard = DataPacket(0x62);
    afterDiscard.captureEpoch = 7;
    ASSERT_TRUE(mapping.Write(afterDiscard));
    AudioPacket decoded;
    ASSERT_TRUE(mapping.Read(decoded));
    EXPECT_EQ(decoded.data, afterDiscard.data);

    ASSERT_TRUE(mapping.Write(LifecyclePacket(AudioPacketRecordType::EndOfStream, 7)));
    mapping.Discard();
    EXPECT_EQ(mapping.consumerState.currentEpoch, 0u);
    ASSERT_TRUE(mapping.Write(LifecyclePacket(AudioPacketRecordType::EpochStart, 8)));
    ASSERT_TRUE(mapping.Read(decoded));
    EXPECT_EQ(decoded.captureEpoch, 8u);
}

TEST(ProcessLoopbackProtocolTest, FirstEpochAdvancesPrivateLifecycleStateWithSharedCursor) {
    ProtocolMapping mapping;
    auto* header = ce::process_loopback::Initialize(mapping.data, 24);
    ASSERT_NE(header, nullptr);
    ASSERT_TRUE(mapping.Write(LifecyclePacket(AudioPacketRecordType::EpochStart, 1)));
    EXPECT_EQ(mapping.consumerState.currentEpoch, 0u);
    EXPECT_EQ(mapping.consumerState.nextSequence, 0u);
    EXPECT_EQ(header->readSequence.load(), 0u);

    AudioPacket packet;
    ASSERT_TRUE(mapping.Read(packet));
    EXPECT_EQ(packet.recordType, AudioPacketRecordType::EpochStart);
    EXPECT_EQ(mapping.consumerState.currentEpoch, 1u);
    EXPECT_EQ(mapping.consumerState.lastEpoch, 1u);
    EXPECT_EQ(mapping.consumerState.nextSequence, 1u);
    EXPECT_EQ(header->readSequence.load(), 1u);
    EXPECT_EQ(header->consumedPackets.load(), 1u);
    EXPECT_FALSE(mapping.Read(packet));
    EXPECT_FALSE(ce::process_loopback::HasFatalTransportFailure(mapping.data));
}

TEST(ProcessLoopbackProtocolTest, DiscardRollsBackPrivateStateWhenLaterDescriptorIsCorrupt) {
    ProtocolMapping mapping;
    auto* header = ce::process_loopback::Initialize(mapping.data, 25);
    ASSERT_NE(header, nullptr);
    ASSERT_TRUE(mapping.Write(LifecyclePacket(AudioPacketRecordType::EpochStart, 1)));
    AudioPacket packet = DataPacket(0x42, 48);
    packet.captureEpoch = 1;
    ASSERT_TRUE(mapping.Write(packet));
    ce::process_loopback::Descriptors(mapping.data)[1].captureEpoch = 2;

    mapping.Discard();
    EXPECT_TRUE(ce::process_loopback::HasFatalTransportFailure(mapping.data));
    EXPECT_EQ(header->transportFailureStage.load(),
              static_cast<uint32_t>(ce::process_loopback::TransportFailureStage::DiscardEpochValidation));
    EXPECT_EQ(header->readSequence.load(), 0u);
    EXPECT_EQ(header->readByteSequence.load(), 0u);
    EXPECT_EQ(mapping.consumerState.nextSequence, 0u);
    EXPECT_EQ(mapping.consumerState.nextByteSequence, 0u);
    EXPECT_EQ(mapping.consumerState.currentEpoch, 0u);
    EXPECT_EQ(mapping.consumerState.lastEpoch, 0u);
    EXPECT_EQ(header->failureCurrentEpoch.load(), 1u);
    EXPECT_EQ(header->failurePacketEpoch.load(), 2u);
}

TEST(ProcessLoopbackProtocolTest, EndpointStateCannotBeReusedAcrossWorkersOrCursors) {
    ProtocolMapping producerMismatch;
    auto* producerHeader = ce::process_loopback::Initialize(producerMismatch.data, 26);
    ASSERT_NE(producerHeader, nullptr);
    producerMismatch.producerState.workerGeneration = 99;
    EXPECT_FALSE(producerMismatch.Write(LifecyclePacket(AudioPacketRecordType::EpochStart, 1)));
    EXPECT_EQ(producerHeader->transportFailureStage.load(),
              static_cast<uint32_t>(ce::process_loopback::TransportFailureStage::ProducerStateValidation));

    ProtocolMapping consumerMismatch;
    auto* consumerHeader = ce::process_loopback::Initialize(consumerMismatch.data, 27);
    ASSERT_NE(consumerHeader, nullptr);
    ASSERT_TRUE(consumerMismatch.Write(LifecyclePacket(AudioPacketRecordType::EpochStart, 1)));
    consumerMismatch.consumerState.workerGeneration = 99;
    AudioPacket packet;
    EXPECT_FALSE(consumerMismatch.Read(packet));
    EXPECT_EQ(consumerHeader->transportFailureStage.load(),
              static_cast<uint32_t>(ce::process_loopback::TransportFailureStage::ConsumerStateValidation));
}

TEST(ProcessLoopbackProtocolTest, ConcurrentSingleProducerSingleConsumerPreservesEpochAndOrder) {
    ProtocolMapping mapping;
    auto* header = ce::process_loopback::Initialize(mapping.data, 28);
    ASSERT_NE(header, nullptr);
    constexpr uint32_t kPacketCount = 4096;
    std::atomic<bool> producerDone{false};
    std::atomic<bool> producerSucceeded{true};
    std::atomic<bool> consumerSucceeded{true};
    std::atomic<uint32_t> consumedDataPackets{0};

    std::thread producer([&]() {
        if (!mapping.Write(LifecyclePacket(AudioPacketRecordType::EpochStart, 3))) {
            producerSucceeded.store(false, std::memory_order_release);
        }
        for (uint32_t index = 0; index < kPacketCount && producerSucceeded.load(std::memory_order_acquire); ++index) {
            if (!mapping.Write(DataPacket(static_cast<uint8_t>(index)))) {
                producerSucceeded.store(false, std::memory_order_release);
            }
        }
        if (producerSucceeded.load(std::memory_order_acquire) &&
            !mapping.Write(LifecyclePacket(AudioPacketRecordType::EndOfStream, 3))) {
            producerSucceeded.store(false, std::memory_order_release);
        }
        producerDone.store(true, std::memory_order_release);
    });
    std::thread consumer([&]() {
        bool sawEpochStart = false;
        bool sawEndOfStream = false;
        uint32_t nextDataPacket = 0;
        while (!producerDone.load(std::memory_order_acquire) ||
               ce::process_loopback::PendingPacketCount(mapping.data) != 0) {
            AudioPacket packet;
            if (!mapping.Read(packet)) {
                if (ce::process_loopback::HasFatalTransportFailure(mapping.data)) {
                    consumerSucceeded.store(false, std::memory_order_release);
                    break;
                }
                std::this_thread::yield();
                continue;
            }
            if (packet.recordType == AudioPacketRecordType::EpochStart) {
                if (sawEpochStart || nextDataPacket != 0 || packet.captureEpoch != 3) {
                    consumerSucceeded.store(false, std::memory_order_release);
                }
                sawEpochStart = true;
            } else if (packet.recordType == AudioPacketRecordType::Data) {
                if (!sawEpochStart || sawEndOfStream || packet.data.empty() ||
                    packet.data.front() != static_cast<uint8_t>(nextDataPacket)) {
                    consumerSucceeded.store(false, std::memory_order_release);
                }
                ++nextDataPacket;
                consumedDataPackets.fetch_add(1, std::memory_order_relaxed);
            } else {
                if (!sawEpochStart || sawEndOfStream || nextDataPacket != kPacketCount || packet.captureEpoch != 3) {
                    consumerSucceeded.store(false, std::memory_order_release);
                }
                sawEndOfStream = true;
            }
        }
        if (!sawEpochStart || !sawEndOfStream || nextDataPacket != kPacketCount) {
            consumerSucceeded.store(false, std::memory_order_release);
        }
    });
    producer.join();
    consumer.join();

    EXPECT_TRUE(producerSucceeded.load());
    EXPECT_TRUE(consumerSucceeded.load());
    EXPECT_EQ(consumedDataPackets.load(), kPacketCount);
    EXPECT_FALSE(ce::process_loopback::HasFatalTransportFailure(mapping.data));
    EXPECT_EQ(header->producedPackets.load(), kPacketCount + 2u);
    EXPECT_EQ(header->consumedPackets.load(), header->producedPackets.load());
    EXPECT_EQ(mapping.consumerState.currentEpoch, 0u);
    EXPECT_EQ(mapping.consumerState.lastEpoch, 3u);
}

TEST(ProcessLoopbackProtocolTest, IncompleteFailurePublicationStillFailsClosed) {
    ProtocolMapping mapping;
    auto* header = ce::process_loopback::Initialize(mapping.data, 29);
    ASSERT_NE(header, nullptr);
    header->transportStatus.store(static_cast<uint32_t>(ce::process_loopback::TransportStatus::PublishingFailure),
                                  std::memory_order_release);
    EXPECT_TRUE(ce::process_loopback::HasFatalTransportFailure(mapping.data));
    EXPECT_STREQ(ce::process_loopback::TransportStatusName(ce::process_loopback::TransportStatus::PublishingFailure),
                 "PublishingFailure");
}

TEST(ProcessLoopbackProtocolTest, FormatSizedMappingsStayWithinEightSourceBound) {
    ce::process_loopback::TransportLayout stereo{};
    ce::process_loopback::TransportLayout eightChannel{};
    ASSERT_TRUE(ce::process_loopback::ComputeTransportLayout(48000, 2, 32, stereo));
    ASSERT_TRUE(ce::process_loopback::ComputeTransportLayout(48000, 8, 32, eightChannel));
    EXPECT_LT(stereo.mappingBytes, eightChannel.mappingBytes);
    EXPECT_EQ(eightChannel.maximumPacketBytes, 48000u * 8u * 4u * 2u);
    EXPECT_LT(eightChannel.mappingBytes * 8u, 512ull * 1024ull * 1024ull);
}
