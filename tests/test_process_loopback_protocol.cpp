#include <gtest/gtest.h>

#include "process_loopback_protocol.h"

#include <windows.h>

#include <cstdint>
#include <limits>
#include <string>
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
    uint64_t bytes = 0;
    void* data = nullptr;
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
    ASSERT_TRUE(ce::process_loopback::WritePacket(mapping.data, LifecyclePacket(AudioPacketRecordType::EpochStart, 9)));
    AudioPacket input = DataPacket(0x5a, 48);
    input.captureEpoch = 9;
    input.qpcPosition = 123456;
    input.channelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    ASSERT_TRUE(ce::process_loopback::WritePacket(mapping.data, input));
    ASSERT_TRUE(
        ce::process_loopback::WritePacket(mapping.data, LifecyclePacket(AudioPacketRecordType::EndOfStream, 9)));

    AudioPacket output;
    ASSERT_TRUE(ce::process_loopback::ReadPacket(mapping.data, output));
    EXPECT_EQ(output.recordType, AudioPacketRecordType::EpochStart);
    EXPECT_EQ(output.captureEpoch, 9u);
    ASSERT_TRUE(ce::process_loopback::ReadPacket(mapping.data, output));
    EXPECT_EQ(output.recordType, AudioPacketRecordType::Data);
    EXPECT_EQ(output.data, input.data);
    EXPECT_EQ(output.qpcPosition, 123456u);
    EXPECT_EQ(output.channelMask, input.channelMask);
    ASSERT_TRUE(ce::process_loopback::ReadPacket(mapping.data, output));
    EXPECT_EQ(output.recordType, AudioPacketRecordType::EndOfStream);
    EXPECT_TRUE(output.endOfStream);
    EXPECT_FALSE(ce::process_loopback::ReadPacket(mapping.data, output));
}

TEST(ProcessLoopbackProtocolTest, DescriptorExhaustionLatchesFatalIntegrityFailure) {
    ProtocolMapping mapping;
    ASSERT_NE(ce::process_loopback::Initialize(mapping.data, 1), nullptr);
    for (uint32_t sequence = 0; sequence < ce::process_loopback::kDescriptorCount; ++sequence) {
        ASSERT_TRUE(ce::process_loopback::WritePacket(mapping.data, DataPacket(static_cast<uint8_t>(sequence))));
    }
    EXPECT_FALSE(ce::process_loopback::WritePacket(mapping.data, DataPacket(0xff)));
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
    ASSERT_TRUE(ce::process_loopback::WritePacket(mapping.data, LifecyclePacket(AudioPacketRecordType::EpochStart, 3)));
    ASSERT_TRUE(ce::process_loopback::ReadPacket(mapping.data, output));
    for (uint32_t cycle = 0; cycle < 3; ++cycle) {
        for (uint32_t sequence = 0; sequence < ce::process_loopback::kDescriptorCount; ++sequence) {
            const uint8_t value = static_cast<uint8_t>(cycle * 17 + sequence);
            ASSERT_TRUE(ce::process_loopback::WritePacket(mapping.data, DataPacket(value)));
        }
        for (uint32_t sequence = 0; sequence < ce::process_loopback::kDescriptorCount; ++sequence) {
            const uint8_t value = static_cast<uint8_t>(cycle * 17 + sequence);
            ASSERT_TRUE(ce::process_loopback::ReadPacket(mapping.data, output));
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
    ASSERT_TRUE(ce::process_loopback::WritePacket(mapping.data, LifecyclePacket(AudioPacketRecordType::EpochStart, 2)));
    for (uint32_t index = 1; index < ce::process_loopback::kDescriptorCount; ++index) {
        ASSERT_TRUE(ce::process_loopback::WritePacket(mapping.data, DataPacket(static_cast<uint8_t>(index))));
    }
    EXPECT_FALSE(ce::process_loopback::WritePacket(mapping.data, DataPacket(0xff, 5)));
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
    EXPECT_FALSE(ce::process_loopback::WritePacket(mapping.data, packet));
    EXPECT_EQ(static_cast<ce::process_loopback::SharedHeader*>(mapping.data)->oversizedPackets.load(), 1u);
}

TEST(ProcessLoopbackProtocolTest, ByteRingExhaustionLatchesFatalIntegrityFailure) {
    ProtocolMapping mapping;
    auto* header = ce::process_loopback::Initialize(mapping.data, 7);
    ASSERT_NE(header, nullptr);
    header->writeByteSequence.store(header->byteRingBytes, std::memory_order_relaxed);
    EXPECT_FALSE(ce::process_loopback::WritePacket(mapping.data, DataPacket(0x11)));
    EXPECT_EQ(header->transportStatus.load(),
              static_cast<uint32_t>(ce::process_loopback::TransportStatus::ByteRingExhausted));
}

TEST(ProcessLoopbackProtocolTest, SequenceOverflowIsFatalInsteadOfWrapping) {
    ProtocolMapping mapping;
    auto* header = ce::process_loopback::Initialize(mapping.data, 9);
    ASSERT_NE(header, nullptr);
    header->readSequence.store(std::numeric_limits<uint64_t>::max(), std::memory_order_relaxed);
    header->writeSequence.store(std::numeric_limits<uint64_t>::max(), std::memory_order_relaxed);
    EXPECT_FALSE(ce::process_loopback::WritePacket(mapping.data, DataPacket(0x22)));
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
    ASSERT_TRUE(ce::process_loopback::WritePacket(mapping.data, LifecyclePacket(AudioPacketRecordType::EpochStart, 1)));
    ASSERT_TRUE(ce::process_loopback::WritePacket(mapping.data, packet));
    AudioPacket decoded;
    ASSERT_TRUE(ce::process_loopback::ReadPacket(mapping.data, decoded));
    ASSERT_EQ(decoded.recordType, AudioPacketRecordType::EpochStart);
    ASSERT_TRUE(ce::process_loopback::ReadPacket(mapping.data, decoded));
    EXPECT_EQ(decoded.data, packet.data);
}

TEST(ProcessLoopbackProtocolTest, VariableByteRingWrapPreservesPayload) {
    ProtocolMapping mapping;
    ASSERT_NE(ce::process_loopback::Initialize(mapping.data, 11), nullptr);
    const auto* header = static_cast<ce::process_loopback::SharedHeader*>(mapping.data);
    const size_t packetBytes = 700u * 1024u;
    ASSERT_LT(packetBytes, header->maximumPacketBytes);
    AudioPacket decoded;
    ASSERT_TRUE(ce::process_loopback::WritePacket(mapping.data, LifecyclePacket(AudioPacketRecordType::EpochStart, 3)));
    ASSERT_TRUE(ce::process_loopback::ReadPacket(mapping.data, decoded));
    bool wrapped = false;
    uint32_t previousOffset = 0;
    for (uint32_t index = 0; index < 24; ++index) {
        AudioPacket packet = DataPacket(static_cast<uint8_t>(index), packetBytes / 8);
        ASSERT_TRUE(ce::process_loopback::WritePacket(mapping.data, packet));
        const auto& descriptor =
            ce::process_loopback::Descriptors(mapping.data)[index % ce::process_loopback::kDescriptorCount];
        if (index > 0 && descriptor.payloadOffset < previousOffset) {
            wrapped = true;
        }
        previousOffset = descriptor.payloadOffset;
        ASSERT_TRUE(ce::process_loopback::ReadPacket(mapping.data, decoded));
        ASSERT_EQ(decoded.data.size(), packetBytes);
        EXPECT_EQ(decoded.data.front(), static_cast<uint8_t>(index));
        EXPECT_EQ(decoded.data.back(), static_cast<uint8_t>(index));
    }
    EXPECT_TRUE(wrapped);
}

TEST(ProcessLoopbackProtocolTest, CorruptCommittedDescriptorIsFatal) {
    ProtocolMapping mapping;
    ASSERT_NE(ce::process_loopback::Initialize(mapping.data, 12), nullptr);
    ASSERT_TRUE(ce::process_loopback::WritePacket(mapping.data, DataPacket(1, 48)));
    ce::process_loopback::Descriptors(mapping.data)[0].payloadOffset++;
    AudioPacket decoded;
    EXPECT_FALSE(ce::process_loopback::ReadPacket(mapping.data, decoded));
    EXPECT_EQ(static_cast<ce::process_loopback::SharedHeader*>(mapping.data)->transportStatus.load(),
              static_cast<uint32_t>(ce::process_loopback::TransportStatus::CorruptCommittedMetadata));
}

TEST(ProcessLoopbackProtocolTest, ExtremeCorruptFormatFieldsAreRejectedBeforeArithmetic) {
    ProtocolMapping mapping;
    ASSERT_NE(ce::process_loopback::Initialize(mapping.data, 20), nullptr);
    ASSERT_TRUE(ce::process_loopback::WritePacket(mapping.data, DataPacket(1, 48)));
    auto& descriptor = ce::process_loopback::Descriptors(mapping.data)[0];
    descriptor.channels = std::numeric_limits<int32_t>::max();
    descriptor.bitsPerSample = std::numeric_limits<int32_t>::max();
    AudioPacket decoded;
    EXPECT_FALSE(ce::process_loopback::ReadPacket(mapping.data, decoded));
    EXPECT_EQ(static_cast<ce::process_loopback::SharedHeader*>(mapping.data)->transportStatus.load(),
              static_cast<uint32_t>(ce::process_loopback::TransportStatus::CorruptCommittedMetadata));
}

TEST(ProcessLoopbackProtocolTest, InvalidChannelMaskAndLifecycleMetadataAreFatal) {
    ProtocolMapping producerValidation;
    ASSERT_NE(ce::process_loopback::Initialize(producerValidation.data, 14), nullptr);
    AudioPacket invalidMask = DataPacket(1, 48);
    invalidMask.channelMask = SPEAKER_FRONT_LEFT;
    EXPECT_FALSE(ce::process_loopback::WritePacket(producerValidation.data, invalidMask));
    EXPECT_TRUE(ce::process_loopback::HasFatalTransportFailure(producerValidation.data));

    ProtocolMapping consumerValidation;
    ASSERT_NE(ce::process_loopback::Initialize(consumerValidation.data, 15), nullptr);
    ASSERT_TRUE(ce::process_loopback::WritePacket(consumerValidation.data,
                                                  LifecyclePacket(AudioPacketRecordType::EpochStart, 1)));
    ce::process_loopback::Descriptors(consumerValidation.data)[0].channelMask = SPEAKER_FRONT_LEFT;
    AudioPacket decoded;
    EXPECT_FALSE(ce::process_loopback::ReadPacket(consumerValidation.data, decoded));
    EXPECT_TRUE(ce::process_loopback::HasFatalTransportFailure(consumerValidation.data));
}

TEST(ProcessLoopbackProtocolTest, LifecycleEpochMismatchIsFatal) {
    ProtocolMapping mapping;
    ASSERT_NE(ce::process_loopback::Initialize(mapping.data, 13), nullptr);
    ASSERT_TRUE(ce::process_loopback::WritePacket(mapping.data, LifecyclePacket(AudioPacketRecordType::EpochStart, 1)));
    AudioPacket mismatched = DataPacket(2, 48);
    mismatched.captureEpoch = 2;
    ASSERT_TRUE(ce::process_loopback::WritePacket(mapping.data, mismatched));
    AudioPacket decoded;
    ASSERT_TRUE(ce::process_loopback::ReadPacket(mapping.data, decoded));
    EXPECT_FALSE(ce::process_loopback::ReadPacket(mapping.data, decoded));
    EXPECT_TRUE(ce::process_loopback::HasFatalTransportFailure(mapping.data));
}

TEST(ProcessLoopbackProtocolTest, DataAndEndOfStreamRequireAnOpenEpoch) {
    ProtocolMapping dataMapping;
    ASSERT_NE(ce::process_loopback::Initialize(dataMapping.data, 16), nullptr);
    ASSERT_TRUE(ce::process_loopback::WritePacket(dataMapping.data, DataPacket(3, 48)));
    AudioPacket decoded;
    EXPECT_FALSE(ce::process_loopback::ReadPacket(dataMapping.data, decoded));
    EXPECT_TRUE(ce::process_loopback::HasFatalTransportFailure(dataMapping.data));

    ProtocolMapping endMapping;
    ASSERT_NE(ce::process_loopback::Initialize(endMapping.data, 17), nullptr);
    ASSERT_TRUE(
        ce::process_loopback::WritePacket(endMapping.data, LifecyclePacket(AudioPacketRecordType::EndOfStream, 1)));
    EXPECT_FALSE(ce::process_loopback::ReadPacket(endMapping.data, decoded));
    EXPECT_TRUE(ce::process_loopback::HasFatalTransportFailure(endMapping.data));
}

TEST(ProcessLoopbackProtocolTest, CompletedEpochCannotBeReused) {
    ProtocolMapping mapping;
    ASSERT_NE(ce::process_loopback::Initialize(mapping.data, 18), nullptr);
    ASSERT_TRUE(ce::process_loopback::WritePacket(mapping.data, LifecyclePacket(AudioPacketRecordType::EpochStart, 4)));
    ASSERT_TRUE(
        ce::process_loopback::WritePacket(mapping.data, LifecyclePacket(AudioPacketRecordType::EndOfStream, 4)));
    ASSERT_TRUE(ce::process_loopback::WritePacket(mapping.data, LifecyclePacket(AudioPacketRecordType::EpochStart, 4)));
    AudioPacket decoded;
    ASSERT_TRUE(ce::process_loopback::ReadPacket(mapping.data, decoded));
    ASSERT_TRUE(ce::process_loopback::ReadPacket(mapping.data, decoded));
    EXPECT_FALSE(ce::process_loopback::ReadPacket(mapping.data, decoded));
    EXPECT_TRUE(ce::process_loopback::HasFatalTransportFailure(mapping.data));
}

TEST(ProcessLoopbackProtocolTest, DiscardPreservesLifecycleStateForContinuingCapture) {
    ProtocolMapping mapping;
    auto* header = ce::process_loopback::Initialize(mapping.data, 19);
    ASSERT_NE(header, nullptr);
    ASSERT_TRUE(ce::process_loopback::WritePacket(mapping.data, LifecyclePacket(AudioPacketRecordType::EpochStart, 7)));
    AudioPacket beforeDiscard = DataPacket(0x31);
    beforeDiscard.captureEpoch = 7;
    ASSERT_TRUE(ce::process_loopback::WritePacket(mapping.data, beforeDiscard));

    ce::process_loopback::DiscardPackets(mapping.data);
    EXPECT_FALSE(ce::process_loopback::HasFatalTransportFailure(mapping.data));
    EXPECT_EQ(header->consumerEpoch.load(), 7u);
    EXPECT_EQ(header->lastConsumerEpoch.load(), 7u);
    EXPECT_EQ(ce::process_loopback::PendingPacketCount(mapping.data), 0u);

    AudioPacket afterDiscard = DataPacket(0x62);
    afterDiscard.captureEpoch = 7;
    ASSERT_TRUE(ce::process_loopback::WritePacket(mapping.data, afterDiscard));
    AudioPacket decoded;
    ASSERT_TRUE(ce::process_loopback::ReadPacket(mapping.data, decoded));
    EXPECT_EQ(decoded.data, afterDiscard.data);

    ASSERT_TRUE(
        ce::process_loopback::WritePacket(mapping.data, LifecyclePacket(AudioPacketRecordType::EndOfStream, 7)));
    ce::process_loopback::DiscardPackets(mapping.data);
    EXPECT_EQ(header->consumerEpoch.load(), 0u);
    ASSERT_TRUE(ce::process_loopback::WritePacket(mapping.data, LifecyclePacket(AudioPacketRecordType::EpochStart, 8)));
    ASSERT_TRUE(ce::process_loopback::ReadPacket(mapping.data, decoded));
    EXPECT_EQ(decoded.captureEpoch, 8u);
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
