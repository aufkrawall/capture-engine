#include <gtest/gtest.h>

#include "process_loopback_protocol.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

class ProtocolMapping {
public:
    ProtocolMapping() {
        data = VirtualAlloc(nullptr, ce::process_loopback::MappingBytes(), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    ~ProtocolMapping() {
        if (data) {
            VirtualFree(data, 0, MEM_RELEASE);
        }
    }
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
    ASSERT_TRUE(ce::process_loopback::WritePacket(
        mapping.data, LifecyclePacket(AudioPacketRecordType::EpochStart, 9)));
    AudioPacket input = DataPacket(0x5a, 48);
    input.qpcPosition = 123456;
    input.channelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    ASSERT_TRUE(ce::process_loopback::WritePacket(mapping.data, input));
    ASSERT_TRUE(ce::process_loopback::WritePacket(
        mapping.data, LifecyclePacket(AudioPacketRecordType::EndOfStream, 9)));

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

TEST(ProcessLoopbackProtocolTest, FullDataRingRejectsNewDataWithoutMovingConsumerCursor) {
    ProtocolMapping mapping;
    ASSERT_NE(ce::process_loopback::Initialize(mapping.data, 1), nullptr);
    constexpr uint32_t extraPackets = 19;
    for (uint32_t sequence = 0; sequence < ce::process_loopback::kPacketSlotCount; ++sequence) {
        ASSERT_TRUE(ce::process_loopback::WritePacket(mapping.data, DataPacket(static_cast<uint8_t>(sequence))));
    }
    for (uint32_t sequence = 0; sequence < extraPackets; ++sequence) {
        EXPECT_FALSE(ce::process_loopback::WritePacket(mapping.data, DataPacket(0xff)));
    }
    auto* header = static_cast<ce::process_loopback::SharedHeader*>(mapping.data);
    EXPECT_EQ(header->overrunPackets.load(), extraPackets);
    EXPECT_EQ(ce::process_loopback::PendingPacketCount(mapping.data), ce::process_loopback::kPacketSlotCount);
    AudioPacket output;
    ASSERT_TRUE(ce::process_loopback::ReadPacket(mapping.data, output));
    EXPECT_EQ(output.data.front(), 0u);
}

TEST(ProcessLoopbackProtocolTest, ConsumedSlotsWrapWithoutOverrunOrReordering) {
    ProtocolMapping mapping;
    ASSERT_NE(ce::process_loopback::Initialize(mapping.data, 2), nullptr);
    AudioPacket output;
    for (uint32_t cycle = 0; cycle < 3; ++cycle) {
        for (uint32_t sequence = 0; sequence < ce::process_loopback::kPacketSlotCount; ++sequence) {
            const uint8_t value = static_cast<uint8_t>(cycle * 17 + sequence);
            ASSERT_TRUE(ce::process_loopback::WritePacket(mapping.data, DataPacket(value)));
        }
        for (uint32_t sequence = 0; sequence < ce::process_loopback::kPacketSlotCount; ++sequence) {
            const uint8_t value = static_cast<uint8_t>(cycle * 17 + sequence);
            ASSERT_TRUE(ce::process_loopback::ReadPacket(mapping.data, output));
            ASSERT_FALSE(output.data.empty());
            EXPECT_EQ(output.data.front(), value);
        }
    }
    auto* header = static_cast<ce::process_loopback::SharedHeader*>(mapping.data);
    EXPECT_EQ(header->overrunPackets.load(), 0u);
    EXPECT_EQ(header->producedPackets.load(), 3u * ce::process_loopback::kPacketSlotCount);
    EXPECT_EQ(header->consumedPackets.load(), header->producedPackets.load());
}

TEST(ProcessLoopbackProtocolTest, FullRingNeverDropsAnOrderedLifecycleHeadForNewData) {
    ProtocolMapping mapping;
    ASSERT_NE(ce::process_loopback::Initialize(mapping.data, 4), nullptr);
    ASSERT_TRUE(ce::process_loopback::WritePacket(
        mapping.data, LifecyclePacket(AudioPacketRecordType::EpochStart, 2)));
    for (uint32_t index = 1; index < ce::process_loopback::kPacketSlotCount; ++index) {
        ASSERT_TRUE(ce::process_loopback::WritePacket(mapping.data, DataPacket(static_cast<uint8_t>(index))));
    }
    EXPECT_FALSE(ce::process_loopback::WritePacket(mapping.data, DataPacket(0xff, 5)));
    auto* header = static_cast<ce::process_loopback::SharedHeader*>(mapping.data);
    EXPECT_EQ(header->overrunPackets.load(), 1u);
    EXPECT_EQ(header->overrunFrames.load(), 5u);
    EXPECT_EQ(header->lifecycleOverrunPackets.load(), 0u);
    AudioPacket output;
    ASSERT_TRUE(ce::process_loopback::ReadPacket(mapping.data, output));
    EXPECT_EQ(output.recordType, AudioPacketRecordType::EpochStart);
}

TEST(ProcessLoopbackProtocolTest, OversizedPacketIsRejectedAndCounted) {
    ProtocolMapping mapping;
    ASSERT_NE(ce::process_loopback::Initialize(mapping.data, 6), nullptr);
    AudioPacket packet = DataPacket(0);
    packet.data.resize(ce::process_loopback::kPacketPayloadBytes + 1);
    EXPECT_FALSE(ce::process_loopback::WritePacket(mapping.data, packet));
    EXPECT_EQ(static_cast<ce::process_loopback::SharedHeader*>(mapping.data)->oversizedPackets.load(), 1u);
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
    EXPECT_EQ(ClassifyWorkerExit(false, false, false), WorkerExitDisposition::Final);
    EXPECT_EQ(ce::process_loopback::ComputeWorkerRestartDelayMs(0), 100u);
    EXPECT_EQ(ce::process_loopback::ComputeWorkerRestartDelayMs(1), 200u);
    EXPECT_EQ(ce::process_loopback::ComputeWorkerRestartDelayMs(20), 5000u);
}
