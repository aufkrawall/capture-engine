// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include <gtest/gtest.h>

#include "mediaengine/mux/resilient_output_sink.h"

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

extern "C" {
#include <libavformat/avio.h>
}

namespace {

class ResilientOutputSinkTest : public ::testing::Test {
protected:
    void SetUp() override {
        static std::atomic<unsigned> sequence{0};
        directory_ = std::filesystem::temp_directory_path() /
                     (L"ce_output_sink_test_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                      std::to_wstring(sequence.fetch_add(1)));
        std::filesystem::create_directories(directory_);
        staging_ = directory_ / L"capture_stage.mkv";
        std::ofstream(staging_, std::ios::binary).close();
        options_.spoolDirectory = directory_ / L"spool";
        options_.reconnectProbeIntervalMs = 0;
        LockTarget();
    }

    void LockTarget() {
        blocker_ = CreateFileW(staging_.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT_NE(blocker_, INVALID_HANDLE_VALUE);
        ASSERT_TRUE(LockFile(blocker_, 0, 0, 0xFFFFFFFFu, 0xFFFFFFFFu));
    }

    void TearDown() override {
        UnblockTarget();
        std::error_code ec;
        std::filesystem::remove_all(directory_, ec);
    }

    void UnblockTarget() {
        if (blocker_ != INVALID_HANDLE_VALUE) {
            UnlockFile(blocker_, 0, 0, 0xFFFFFFFFu, 0xFFFFFFFFu);
            CloseHandle(blocker_);
            blocker_ = INVALID_HANDLE_VALUE;
        }
    }

    void WriteAndClose(ce::mux::ResilientOutputSink& sink) {
        AVIOContext* io = nullptr;
        ASSERT_EQ(sink.Open(staging_, &io, options_), 0);
        const auto* bytes = reinterpret_cast<const unsigned char*>(kPayload);
        avio_write(io, bytes, sizeof(kPayload) - 1);
        avio_flush(io);
        ASSERT_EQ(io->error, 0);
        ASSERT_TRUE(sink.IsSpooling());
        ASSERT_EQ(sink.Close(&io), 0);
        ASSERT_TRUE(sink.HasPendingSpool());
        ASSERT_TRUE(ce::mux::HasSpoolForStaging(staging_, options_.spoolDirectory));
    }

    void ExpectRecoveryPayload() {
        UnblockTarget();
        const auto result = ce::mux::ReplaySpoolForStaging(staging_, options_.spoolDirectory);
        ASSERT_EQ(result.outcome, ce::mux::SpoolReplayOutcome::kReplayed) << result.detail;
        EXPECT_TRUE(result.finalized);
        EXPECT_EQ(result.finalFileSize, sizeof(kPayload) - 1);
        std::ifstream file(staging_, std::ios::binary);
        const std::string actual{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        EXPECT_EQ(actual, kPayload);
        EXPECT_FALSE(ce::mux::HasSpoolForStaging(staging_, options_.spoolDirectory));
    }

    static void Write(AVIOContext* io, const std::string& bytes) {
        avio_write(io, reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()));
        avio_flush(io);
        ASSERT_EQ(io->error, 0);
    }

    std::string TargetBytes() const {
        std::ifstream file(staging_, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    static constexpr char kPayload[] = "recording tail and trailer";
    std::filesystem::path directory_;
    std::filesystem::path staging_;
    ce::mux::ResilientOutputSink::Options options_;
    HANDLE blocker_ = INVALID_HANDLE_VALUE;
};

TEST_F(ResilientOutputSinkTest, RecoveryOwnsPendingSpoolAfterSinkDestruction) {
    {
        ce::mux::ResilientOutputSink sink;
        WriteAndClose(sink);
    }
    ExpectRecoveryPayload();
}

TEST_F(ResilientOutputSinkTest, OpeningNextRecordingPreservesPreviousRecoverySpool) {
    ce::mux::ResilientOutputSink sink;
    WriteAndClose(sink);
    const auto next = directory_ / L"next.mkv";
    std::ofstream(next, std::ios::binary).close();
    AVIOContext* io = nullptr;
    ASSERT_EQ(sink.Open(next, &io, options_), 0);
    EXPECT_TRUE(ce::mux::HasSpoolForStaging(staging_, options_.spoolDirectory));
    sink.Discard(&io);
    ExpectRecoveryPayload();
}

TEST_F(ResilientOutputSinkTest, RejectedNextOpenPreservesPreviousRecoverySpool) {
    ce::mux::ResilientOutputSink sink;
    WriteAndClose(sink);
    AVIOContext* io = nullptr;
    EXPECT_LT(sink.Open({}, &io, options_), 0);
    ExpectRecoveryPayload();
}

TEST_F(ResilientOutputSinkTest, ExplicitCancellationDiscardsPendingSpool) {
    ce::mux::ResilientOutputSink sink;
    WriteAndClose(sink);
    sink.Discard();
    EXPECT_FALSE(ce::mux::HasSpoolForStaging(staging_, options_.spoolDirectory));
}

TEST_F(ResilientOutputSinkTest, LiveReplayRetainsTheDestinationAndSupportsAnotherOutageAndTrailerPatch) {
    uint64_t now = 1000;
    options_.nowMs = [&] { return now; };
    options_.reconnectProbeIntervalMs = 100;
    ce::mux::ResilientOutputSink sink;
    AVIOContext* io = nullptr;
    ASSERT_EQ(sink.Open(staging_, &io, options_), 0);
    Write(io, "first");
    ASSERT_TRUE(sink.IsSpooling());

    UnblockTarget();
    now += 100;
    Write(io, "-second");
    ASSERT_FALSE(sink.IsSpooling());
    EXPECT_EQ(TargetBytes(), "first-second");
    EXPECT_FALSE(ce::mux::HasSpoolForStaging(staging_, options_.spoolDirectory));

    LockTarget();
    Write(io, "-third");
    ASSERT_TRUE(sink.IsSpooling());
    UnblockTarget();
    now += 100;
    Write(io, "-fourth");
    ASSERT_FALSE(sink.IsSpooling());

    ASSERT_EQ(avio_seek(io, 0, SEEK_SET), 0);
    Write(io, "FIRST");
    const int64_t end = avio_size(io);
    ASSERT_EQ(end, 25);
    ASSERT_EQ(avio_seek(io, end, SEEK_SET), end);
    Write(io, "-trailer");
    ASSERT_EQ(sink.Close(&io), 0);
    EXPECT_EQ(TargetBytes(), "FIRST-second-third-fourth-trailer");
    EXPECT_FALSE(sink.HasPendingSpool());
}

TEST_F(ResilientOutputSinkTest, FailedLiveReplayKeepsTheSpoolWritableUntilStorageReturns) {
    uint64_t now = 1000;
    options_.nowMs = [&] { return now; };
    options_.reconnectProbeIntervalMs = 100;
    ce::mux::ResilientOutputSink sink;
    AVIOContext* io = nullptr;
    ASSERT_EQ(sink.Open(staging_, &io, options_), 0);
    Write(io, "first");
    now += 100;
    Write(io, "-still-offline");
    ASSERT_TRUE(sink.IsSpooling());
    Write(io, "-queued");
    UnblockTarget();
    now += 100;
    Write(io, "-resumed");
    ASSERT_FALSE(sink.IsSpooling());
    Write(io, "-direct");
    ASSERT_EQ(sink.Close(&io), 0);
    EXPECT_EQ(TargetBytes(), "first-still-offline-queued-resumed-direct");
}

TEST_F(ResilientOutputSinkTest, ReplayRejectsMalformedRecordOffsetBeyondSignedMax) {
    const std::filesystem::path spoolPath = ce::mux::SpoolPathForStaging(staging_, options_.spoolDirectory);
    std::filesystem::create_directories(spoolPath.parent_path());
    std::ofstream out(spoolPath, std::ios::binary);
    struct Header {
        uint32_t magic = 0x4C505343u;
        uint32_t version = 1u;
        uint64_t baseFileSize = 0;
        uint64_t logicalFileSize = 100;
        uint32_t finalized = 1;
        uint32_t reserved = 0;
    } hdr;
    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    struct Record {
        uint32_t magic = 0x524C5053u;
        uint32_t payloadBytes = 10;
        uint64_t targetOffset = 0xFFFFFFFFFFFFFFFEull;
    } rec;
    out.write(reinterpret_cast<const char*>(&rec), sizeof(rec));
    char payload[10] = "123456789";
    out.write(payload, sizeof(payload));
    out.close();

    UnblockTarget();
    const ce::mux::SpoolReplayResult result = ce::mux::ReplaySpoolForStaging(staging_, options_.spoolDirectory);
    EXPECT_EQ(result.outcome, ce::mux::SpoolReplayOutcome::kCorruptSpool);
}

}  // namespace
