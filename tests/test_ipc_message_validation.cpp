// Always-run regression floor for the IPC trust boundary.
//
// tests/fuzz/fuzz_ipc_deserialize.cpp explores ValidateProcessMessage under
// libFuzzer, but that stage only runs with `build.py --run-fuzz`. These tests
// replay the committed seed corpus and pin the exact accept/reject verdicts on
// every boundary, so the validator stays covered in the ordinary unit gate.

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "../common/process_ipc.h"

namespace {

// build.py runs unit_tests.exe from the repository root, but developers commonly
// run it straight out of tests/. Walk upwards so the corpus is found either way,
// and return an empty path when it genuinely does not exist so the test fails.
std::filesystem::path CorpusDir() {
    const std::filesystem::path relative = std::filesystem::path("tests") / "fuzz" / "corpus" / "ipc";
    std::filesystem::path base = std::filesystem::current_path();
    for (int depth = 0; depth < 4; ++depth) {
        std::filesystem::path candidate = base / relative;
        if (std::filesystem::is_directory(candidate)) {
            return candidate;
        }
        if (!base.has_parent_path() || base.parent_path() == base) {
            break;
        }
        base = base.parent_path();
    }
    return {};
}

ProcessMessage MakeValid(ProcessMessageKind kind, uint16_t opcode, uint64_t sequence, const char* payload = nullptr) {
    ProcessMessage message{};
    message.kind = kind;
    message.opcode = opcode;
    message.senderMode = ProcessMode::Media;
    message.sequence = sequence;
    message.senderPid = 4321;
    message.nonce.fill(0x5A);
    if (payload) {
        const size_t length = strlen(payload);
        memcpy(message.payload, payload, length);
        message.payloadSize = static_cast<uint32_t>(length + 1);
    }
    message.totalSize = message.headerSize + message.payloadSize;
    return message;
}

// Accept-path helper: the peer identity matches the message exactly.
bool Validate(const ProcessMessage& message, size_t bytesRead, uint64_t expectedSequence, bool requireExact) {
    return ValidateProcessMessage(message, bytesRead, message.kind, message.senderMode, message.senderPid,
                                  message.nonce, expectedSequence, requireExact);
}

}  // namespace

TEST(IpcMessageValidationTest, SeedCorpusIsPresentAndReplaysWithoutCrashing) {
    const std::filesystem::path dir = CorpusDir();
    ASSERT_FALSE(dir.empty()) << "Missing IPC fuzz corpus (tests/fuzz/corpus/ipc) at or above "
                              << std::filesystem::current_path().string();

    size_t replayed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::ifstream file(entry.path(), std::ios::binary);
        ASSERT_TRUE(file) << "Unreadable corpus seed " << entry.path().string();
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (bytes.size() < sizeof(ProcessMessage)) {
            continue;
        }

        ProcessMessage message;
        memcpy(&message, bytes.data(), sizeof(ProcessMessage));

        // Every seed must survive validation against both a matching and a
        // deliberately mismatched peer identity without crashing or reading past
        // the fixed payload array.
        (void)Validate(message, message.totalSize, 0, true);
        (void)ValidateProcessMessage(message, bytes.size(), ProcessMessageKind::Command, ProcessMode::Controller, 1,
                                     ProcessChannelNonce{}, 0, false);
        ++replayed;
    }
    // Fail closed: an empty corpus would make this test silently vacuous, which is
    // the exact failure mode that left the fuzz harnesses rotting unnoticed.
    EXPECT_GT(replayed, 0u) << "IPC fuzz corpus contains no replayable seeds";
}

TEST(IpcMessageValidationTest, AcceptsWellFormedMessages) {
    const ProcessMessage startup = MakeValid(ProcessMessageKind::Startup, 0, 0);
    EXPECT_TRUE(Validate(startup, startup.totalSize, 0, true));

    const ProcessMessage ping = MakeValid(ProcessMessageKind::Command, static_cast<uint16_t>(ProcessCommand::Ping), 7);
    EXPECT_TRUE(Validate(ping, ping.totalSize, 6, false));

    const ProcessMessage start =
        MakeValid(ProcessMessageKind::Command, static_cast<uint16_t>(ProcessCommand::StartRecording), 8, "audio_only");
    EXPECT_TRUE(Validate(start, start.totalSize, 7, false));

    const ProcessMessage toggleOverlay =
        MakeValid(ProcessMessageKind::Command, static_cast<uint16_t>(ProcessCommand::ToggleOverlay), 9);
    EXPECT_TRUE(Validate(toggleOverlay, toggleOverlay.totalSize, 8, false));

    const ProcessMessage ack = MakeValid(ProcessMessageKind::Response, static_cast<uint16_t>(ProcessResponse::Ack), 9);
    EXPECT_TRUE(Validate(ack, ack.totalSize, 8, false));

    const ProcessMessage error =
        MakeValid(ProcessMessageKind::Response, static_cast<uint16_t>(ProcessResponse::Error), 10, "detail");
    EXPECT_TRUE(Validate(error, error.totalSize, 9, false));
}

TEST(IpcMessageValidationTest, RejectsLengthInconsistencies) {
    const ProcessMessage ping = MakeValid(ProcessMessageKind::Command, static_cast<uint16_t>(ProcessCommand::Ping), 7);

    // Short read below the fixed header.
    EXPECT_FALSE(Validate(ping, offsetof(ProcessMessage, payload) - 1, 6, false));
    // Read longer than the wire struct.
    EXPECT_FALSE(Validate(ping, sizeof(ProcessMessage) + 1, 6, false));
    // totalSize disagreeing with the bytes actually received.
    EXPECT_FALSE(Validate(ping, ping.totalSize + 1, 6, false));

    ProcessMessage inconsistent = ping;
    inconsistent.payloadSize = 4;  // totalSize no longer equals headerSize + payloadSize
    EXPECT_FALSE(Validate(inconsistent, inconsistent.totalSize, 6, false));

    ProcessMessage oversized = ping;
    oversized.payloadSize = static_cast<uint32_t>(PROCESS_MAX_PAYLOAD) + 1;
    oversized.totalSize = oversized.headerSize + oversized.payloadSize;
    EXPECT_FALSE(Validate(oversized, oversized.totalSize, 6, false));
}

TEST(IpcMessageValidationTest, RejectsMalformedPayloadTermination) {
    ProcessMessage unterminated =
        MakeValid(ProcessMessageKind::Response, static_cast<uint16_t>(ProcessResponse::Error), 5, "detail");
    unterminated.payload[unterminated.payloadSize - 1] = 'x';  // strip the NUL terminator
    EXPECT_FALSE(Validate(unterminated, unterminated.totalSize, 4, false));

    ProcessMessage embeddedNul =
        MakeValid(ProcessMessageKind::Response, static_cast<uint16_t>(ProcessResponse::Error), 5, "detail");
    embeddedNul.payload[2] = '\0';  // short string inside a longer declared payload
    EXPECT_FALSE(Validate(embeddedNul, embeddedNul.totalSize, 4, false));
}

TEST(IpcMessageValidationTest, RejectsHeaderAndIdentityMismatches) {
    const ProcessMessage ping = MakeValid(ProcessMessageKind::Command, static_cast<uint16_t>(ProcessCommand::Ping), 7);

    ProcessMessage badMagic = ping;
    badMagic.magic = PROCESS_MSG_MAGIC ^ 0xFFu;
    EXPECT_FALSE(Validate(badMagic, badMagic.totalSize, 6, false));

    ProcessMessage badVersion = ping;
    badVersion.version = PROCESS_PROTOCOL_VERSION + 1;
    EXPECT_FALSE(Validate(badVersion, badVersion.totalSize, 6, false));

    ProcessMessage badHeaderSize = ping;
    badHeaderSize.headerSize = 48;
    EXPECT_FALSE(Validate(badHeaderSize, badHeaderSize.totalSize, 6, false));

    ProcessMessage badMode = ping;
    badMode.senderMode = static_cast<ProcessMode>(99);
    EXPECT_FALSE(Validate(badMode, badMode.totalSize, 6, false));

    // Foreign peer identity: wrong PID, wrong nonce, wrong kind.
    EXPECT_FALSE(ValidateProcessMessage(ping, ping.totalSize, ping.kind, ping.senderMode, ping.senderPid + 1,
                                        ping.nonce, 6, false));
    ProcessChannelNonce foreign = ping.nonce;
    foreign[0] = static_cast<uint8_t>(foreign[0] + 1u);
    EXPECT_FALSE(ValidateProcessMessage(ping, ping.totalSize, ping.kind, ping.senderMode, ping.senderPid, foreign, 6,
                                        false));
    EXPECT_FALSE(ValidateProcessMessage(ping, ping.totalSize, ProcessMessageKind::Response, ping.senderMode,
                                        ping.senderPid, ping.nonce, 6, false));
}

TEST(IpcMessageValidationTest, RejectsOutOfRangeOpcodesAndUnexpectedPayloads) {
    ProcessMessage badCommand = MakeValid(ProcessMessageKind::Command, 99, 7);
    EXPECT_FALSE(Validate(badCommand, badCommand.totalSize, 6, false));

    ProcessMessage badResponse = MakeValid(ProcessMessageKind::Response, 99, 7);
    EXPECT_FALSE(Validate(badResponse, badResponse.totalSize, 6, false));

    ProcessMessage badKind = MakeValid(static_cast<ProcessMessageKind>(9), 1, 7);
    EXPECT_FALSE(Validate(badKind, badKind.totalSize, 6, false));

    // Only StartRecording carries a payload, and only the exact "audio_only" token.
    ProcessMessage pingWithPayload =
        MakeValid(ProcessMessageKind::Command, static_cast<uint16_t>(ProcessCommand::Ping), 7, "audio_only");
    EXPECT_FALSE(Validate(pingWithPayload, pingWithPayload.totalSize, 6, false));

    ProcessMessage toggleWithPayload =
        MakeValid(ProcessMessageKind::Command, static_cast<uint16_t>(ProcessCommand::ToggleOverlay), 7, "audio_only");
    EXPECT_FALSE(Validate(toggleWithPayload, toggleWithPayload.totalSize, 6, false));

    ProcessMessage startWrongToken =
        MakeValid(ProcessMessageKind::Command, static_cast<uint16_t>(ProcessCommand::StartRecording), 7, "video_only");
    EXPECT_FALSE(Validate(startWrongToken, startWrongToken.totalSize, 6, false));

    // Startup carries no opcode, no sequence, and no payload.
    ProcessMessage startupWithPayload = MakeValid(ProcessMessageKind::Startup, 0, 0, "x");
    EXPECT_FALSE(Validate(startupWithPayload, startupWithPayload.totalSize, 0, true));

    ProcessMessage startupWithOpcode = MakeValid(ProcessMessageKind::Startup, 3, 0);
    EXPECT_FALSE(Validate(startupWithOpcode, startupWithOpcode.totalSize, 0, true));
}

TEST(IpcMessageValidationTest, EnforcesSequencePolicy) {
    const ProcessMessage ping = MakeValid(ProcessMessageKind::Command, static_cast<uint16_t>(ProcessCommand::Ping), 7);

    // Exact-sequence channels accept only the awaited value.
    EXPECT_TRUE(Validate(ping, ping.totalSize, 7, true));
    EXPECT_FALSE(Validate(ping, ping.totalSize, 6, true));

    // Monotonic channels reject replays and stale sequences.
    EXPECT_TRUE(Validate(ping, ping.totalSize, 6, false));
    EXPECT_FALSE(Validate(ping, ping.totalSize, 7, false));
    EXPECT_FALSE(Validate(ping, ping.totalSize, 8, false));
}
