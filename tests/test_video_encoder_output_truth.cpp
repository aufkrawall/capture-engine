#include "test_video_encoder_source_shared.h"

// Regression pins for the output-truth contract: mux write failures, dropped
// encoded packets and dropped HDR metadata packets must surface as a degraded
// completion instead of a clean-save claim, and committed packets must survive
// every exit path (the behavioral matrix lives in test_mux_invariants.cpp).

TEST(VideoEncoderOutputTruthTest, HdrMetadataFailureDropsOnlyThePacket) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    const size_t function = source.find("bool VideoEncoder::NormalizeHdrPacketIfNeeded");
    const size_t functionEnd = source.find("void VideoEncoder::WriteFrame", function);
    ASSERT_NE(function, std::string::npos);
    ASSERT_NE(functionEnd, std::string::npos);
    const std::string body = source.substr(function, functionEnd - function);

    // The session-level discard flag used to be set here, deleting recordings
    // with committed frames over one unnormalizable metadata packet.
    EXPECT_EQ(body.find("discardOutputRequested.store"), std::string::npos);
    EXPECT_NE(body.find("hdrMetadataDropCount.fetch_add"), std::string::npos);
}

TEST(VideoEncoderOutputTruthTest, RecordingPacketsAreNeverDroppedSilently) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    const size_t function = source.find("void VideoEncoder::WriteFrame");
    const size_t functionEnd = source.find("void VideoEncoder::PublishRuntimeState", function);
    ASSERT_NE(function, std::string::npos);
    ASSERT_NE(functionEnd, std::string::npos);
    const std::string body = source.substr(function, functionEnd - function);

    // A failed av_packet_clone used to lose the packet with no log and no
    // counter in recording mode ("else if (liveOutput)" swallowed it).
    EXPECT_EQ(body.find("else if (liveOutput)"), std::string::npos);
    EXPECT_NE(body.find("RequestOutputFailure(\"clone_packet\", AVERROR(ENOMEM))"), std::string::npos);
}

TEST(VideoEncoderOutputTruthTest, MuxWriteFailuresRouteThroughTheOutputRouter) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("RequestOutputFailure(\"write_packet\", ret)"), std::string::npos);
    EXPECT_NE(source.find("RequestOutputFailure(\"flush_packet\", flushWriteResult)"), std::string::npos);
    EXPECT_EQ(source.find("RequestLiveOutputFailure(\"write_packet\""), std::string::npos);
    EXPECT_NE(source.find("void VideoEncoder::RequestLocalOutputFailure"), std::string::npos);
    EXPECT_NE(source.find("cmdStopRecording.store(true"), std::string::npos);
}

TEST(VideoEncoderOutputTruthTest, LocalOutputsUseTheBoundedIoDeadline) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    // The interrupt callback used to be wired for live outputs only, so a hung
    // local write wedged the writer thread and the recording was never
    // published. It is now registered and armed for both output kinds.
    EXPECT_EQ(source.find("liveOutput ? &fmtCtx->interrupt_callback : nullptr"), std::string::npos);
    EXPECT_NE(source.find("fmtCtx->interrupt_callback.callback = InterruptOutputIo"), std::string::npos);
    EXPECT_NE(source.find("kLocalOutputIoTimeoutMs"), std::string::npos);
}

TEST(VideoEncoderOutputTruthTest, CfrCoverageGapsMarkTheOutputDegraded) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    // The coverage verdict is recorded before the report line is written; the
    // consumer of the flag (MediaEngine::WasLastOutputDegraded, pinned below)
    // turns it into the "saved (degraded)" completion.
    const size_t coverage = source.find("CFR artifact failed packet-continuity validation");
    ASSERT_NE(coverage, std::string::npos);
    EXPECT_LT(source.find("cfrCoverageIncomplete.store(true"), coverage);

    const std::filesystem::path engineSource = std::filesystem::current_path() / "mediaengine" / "mediaengine.cpp";
    const std::string engine = ce::test_source::ReadLogicalSource(engineSource);
    ASSERT_FALSE(engine.empty());
    EXPECT_NE(engine.find("lastOutputDegraded = videoEnc->WasLastOutputDegraded()"), std::string::npos);
    EXPECT_NE(engine.find("MEDIAENGINE_API bool MediaEngine_WasLastOutputDegraded()"), std::string::npos);
}

// Regression: FFmpeg's interrupt callback only refuses the NEXT transfer, so a
// local write already blocked in the kernel (dead network share, dying disk)
// still wedged the writer thread despite the "bounded" deadline. The writer
// registers itself and an expired deadline is broken with CancelSynchronousIo
// from the producer path and from Stop's finalize wait.
TEST(VideoEncoderOutputTruthTest, ExpiredOutputIoIsCancelledNotOnlyRefusedNextTime) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    const size_t cancel = source.find("bool VideoEncoder::CancelExpiredOutputIo(");
    ASSERT_NE(cancel, std::string::npos);
    const size_t cancelEnd = source.find("\n}\n", cancel);
    const std::string cancelBody = source.substr(cancel, cancelEnd - cancel);
    EXPECT_NE(cancelBody.find("CancelSynchronousIo(outputIoThread)"), std::string::npos);
    EXPECT_NE(cancelBody.find("std::lock_guard<std::mutex> lock(outputIoCancelMutex);"), std::string::npos)
        << "the cancel must be ordered against deadline arm/clear so it only hits the expired operation";

    // Arm and clear take the same lock as the cancel.
    for (const char* fn : {"void VideoEncoder::ArmOutputIoDeadline()", "void VideoEncoder::ClearOutputIoDeadline()"}) {
        const size_t begin = source.find(fn);
        ASSERT_NE(begin, std::string::npos) << fn;
        const std::string body = source.substr(begin, source.find("\n}\n", begin) - begin);
        EXPECT_NE(body.find("outputIoCancelMutex"), std::string::npos) << fn;
    }

    const size_t writer = source.find("void VideoEncoder::AsyncWriteLoop()");
    ASSERT_NE(writer, std::string::npos);
    EXPECT_NE(source.find("RegisterOutputIoThread();", writer), std::string::npos);
    EXPECT_NE(source.find("CancelExpiredOutputIo(\"write_frame\")"), std::string::npos);
    EXPECT_NE(source.find("CancelExpiredOutputIo(\"stop\")"), std::string::npos);
}

// Regression: when Stop() gave up waiting for the writer, the degraded flag was
// read before the trailer, close and CFR coverage check had run, so a later
// failure could never reach the completion and a clean save was claimed.
TEST(VideoEncoderOutputTruthTest, FinalizeTimeoutReportsTheOutputDegraded) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    const size_t timeout = source.find("writerFinalizeTimedOut.store(true, std::memory_order_release);");
    ASSERT_NE(timeout, std::string::npos);
    const size_t latch = source.find("lastStopFinalizeTimedOut.store(true", timeout);
    ASSERT_NE(latch, std::string::npos);
    EXPECT_LT(latch - timeout, 400u) << "the latch belongs to the timeout branch";
    EXPECT_NE(source.find("lastStopFinalizeTimedOut.store(false"), std::string::npos)
        << "each recording starts without the previous stop's verdict";
}
