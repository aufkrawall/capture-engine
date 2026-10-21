// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
// A recording that keeps finalizing while the next one starts must not write the inject session's shared memory
// (common/capture/detached_live_state.h). Before the detach, the finishing session's reset stored phase=idle over the
// newer session's warmup, whose warmup-to-live commit then failed until shutdown (SDK run 2026-10-21).
#include "common/capture/detached_live_state.h"
#include "common/capture/recording_lifecycle.h"
#include "common/ipc/shared_memory_ref.h"

#include <gtest/gtest.h>

#include <memory>

namespace {

namespace lifecycle = ce::recording_lifecycle;

uint32_t PhaseOf(const SharedMemoryLayout& layout) {
    return layout.runtimeState.capturePhase.load();
}

constexpr uint32_t Phase(CapturePipelinePhase phase) {
    return static_cast<uint32_t>(phase);
}

// The writes a session's stop path makes after its encoder joined (ResetRuntimeDiagnostics, the texture handshake).
void FinishingSessionResets(SharedMemoryLayout& view) {
    view.runtimeState.capturePhase.store(Phase(CapturePipelinePhase::kIdle));
    view.runtimeState.liveFramesEncoded.store(0);
    view.useEncoderTextures.store(false);
    view.encoderTextures.ready.store(false);
}

}  // namespace

TEST(DetachedLiveStateTest, AFinishingSessionNoLongerWritesTheNewerRecordingsState) {
    auto live = std::make_unique<SharedMemoryLayout>();
    std::atomic<bool> oldRequested{true};
    std::atomic<bool> newRequested{true};

    // r0001 is live with zero-copy textures.
    ce::SharedMemoryRef oldView;
    oldView = live.get();
    ASSERT_TRUE(lifecycle::TryArmWarmup(oldView->runtimeState.capturePhase, oldRequested));
    ASSERT_TRUE(lifecycle::TryCommitLive(oldView->runtimeState.capturePhase, oldRequested));
    oldView->runtimeState.liveFramesEncoded.store(430);
    oldView->encoderTextures.SetTextureHandle(0, 0x1234);
    oldView->encoderTextures.ready.store(true);

    // Its stop is acknowledged: it detaches first.
    auto detached = std::make_unique<SharedMemoryLayout>();
    ce::detached_live_state::CopyForDetachedSession(*live, *detached);
    oldView = detached.get();
    oldRequested.store(false);

    // r0002 starts on the live layout and arms its warmup.
    live->runtimeState.capturePhase.store(Phase(CapturePipelinePhase::kIdle));
    ASSERT_TRUE(lifecycle::TryArmWarmup(live->runtimeState.capturePhase, newRequested));
    live->encoderTextures.SetTextureHandle(0, 0x5678);
    live->encoderTextures.ready.store(true);

    // r0001 finishes its drain and resets what it sees.
    const CapturePipelinePhase oldStop =
        lifecycle::BeginStop(oldView->runtimeState.capturePhase, oldView->runtimeState.liveFramesEncoded.load());
    FinishingSessionResets(*oldView);

    EXPECT_EQ(oldStop, CapturePipelinePhase::kStopping) << "r0001 judges its own stop by its own live frames";
    EXPECT_EQ(PhaseOf(*live), Phase(CapturePipelinePhase::kWarmup)) << "r0001's reset stayed private";
    EXPECT_TRUE(live->encoderTextures.ready.load()) << "r0002's texture handshake is untouched";
    EXPECT_EQ(live->encoderTextures.GetTextureHandle(0), 0x5678u);
    EXPECT_TRUE(lifecycle::TryCommitLive(live->runtimeState.capturePhase, newRequested))
        << "r0002 goes live: the commit that failed 3,336 times in the SDK run";
}

// What the detached session keeps reading: its lifecycle, the encoder<->media channel, the texture handshake its drain
// encodes from, and the header that validation reads.
TEST(DetachedLiveStateTest, TheCopyCarriesWhatTheFinishingSessionStillReads) {
    auto live = std::make_unique<SharedMemoryLayout>();
    live->SetMagic(0xCAFEu);
    live->structSize.store(static_cast<uint32_t>(sizeof(SharedMemoryLayout)));
    live->abiSignature.store(0xABCDu);
    live->SetHostPID(4242);
    live->runtimeState.capturePhase.store(Phase(CapturePipelinePhase::kDrain));
    live->runtimeState.liveFramesEncoded.store(77);
    live->runtimeState.encoderOverloadFlags.store(0x5);
    live->runtimeState.muxQueueBytes.store(123456);
    live->runtimeState.recordingStartTime.store(987654321);
    live->encoderTextures.SetTextureHandle(ENCODER_TEXTURE_SLOT_COUNT - 1, 0x99);
    live->encoderTextures.SetKmtTextureHandle(0, 0x42);
    live->encoderTextures.SetFenceHandle(0x77);
    live->encoderTextures.SetWidth(3840);
    live->encoderTextures.kmtReady.store(true);
    live->useEncoderTextures.store(true);

    auto detached = std::make_unique<SharedMemoryLayout>();
    ce::detached_live_state::CopyForDetachedSession(*live, *detached);

    EXPECT_EQ(detached->GetMagic(), 0xCAFEu);
    EXPECT_EQ(detached->structSize.load(), static_cast<uint32_t>(sizeof(SharedMemoryLayout)));
    EXPECT_EQ(detached->abiSignature.load(), 0xABCDu);
    EXPECT_EQ(detached->GetHostPID(), 4242u);
    EXPECT_EQ(PhaseOf(*detached), Phase(CapturePipelinePhase::kDrain));
    EXPECT_EQ(detached->runtimeState.liveFramesEncoded.load(), 77u);
    EXPECT_EQ(detached->runtimeState.encoderOverloadFlags.load(), 0x5u);
    EXPECT_EQ(detached->runtimeState.muxQueueBytes.load(), 123456u);
    EXPECT_EQ(detached->runtimeState.recordingStartTime.load(), 987654321);
    EXPECT_EQ(detached->encoderTextures.GetTextureHandle(ENCODER_TEXTURE_SLOT_COUNT - 1), 0x99u);
    EXPECT_EQ(detached->encoderTextures.GetKmtTextureHandle(0), 0x42u);
    EXPECT_EQ(detached->encoderTextures.GetFenceHandle(), 0x77u);
    EXPECT_EQ(detached->encoderTextures.GetWidth(), 3840u);
    EXPECT_TRUE(detached->encoderTextures.kmtReady.load());
    EXPECT_TRUE(detached->useEncoderTextures.load());

    // The copy is the session's own from here: writing it leaves the live layout alone.
    detached->runtimeState.liveFramesEncoded.store(0);
    EXPECT_EQ(live->runtimeState.liveFramesEncoded.load(), 77u);
}

TEST(DetachedLiveStateTest, TheRefRetargetsBetweenLayoutsAndReadsAsAPointer) {
    auto first = std::make_unique<SharedMemoryLayout>();
    auto second = std::make_unique<SharedMemoryLayout>();
    ce::SharedMemoryRef ref;
    EXPECT_EQ(ref.get(), nullptr);
    EXPECT_FALSE(static_cast<SharedMemoryLayout*>(ref));
    ref = first.get();
    ref->runtimeState.framesEncoded.store(3);
    ref = second.get();
    ref->runtimeState.framesEncoded.store(9);
    EXPECT_EQ(first->runtimeState.framesEncoded.load(), 3u);
    EXPECT_EQ(second->runtimeState.framesEncoded.load(), 9u);
    SharedMemoryLayout* const raw = ref;
    EXPECT_EQ(raw, second.get());
}
