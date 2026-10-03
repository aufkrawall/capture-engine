// Attribution of AMD frame generation outputs to the game frame they show (dx12_overlay_policy/ffx_output_frames.h)
// and the source contracts that keep it exact. The end-to-end proof is the FG flow scenario FlowFSR without a present
// callback, whose fake runtime composes like FidelityFX SDK 1.1.4 and reports each output's true frame.

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "hook/d3d12/dx12_overlay_policy/ffx_output_frames.h"

#include "source_fragment_reader.h"

namespace {

using ce::dx12_overlay_policy::FFXComposingFrame;
using ce::dx12_overlay_policy::FFXFrameOverlayOwner;
using ce::dx12_overlay_policy::FFXFrameOwnerRecord;
using ce::dx12_overlay_policy::FFXFrameOwnerRing;
using ce::dx12_overlay_policy::JudgeFrameExactFFXOutput;
using ce::dx12_overlay_policy::ShouldDrawTopmostOnFFXOutput;

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

FFXFrameOwnerRecord Exact(FFXFrameOverlayOwner owner) {
    FFXFrameOwnerRecord record;
    record.owner = owner;
    record.frameExact = true;
    return record;
}

TEST(FFXOutputFramesPolicyTest, RingReturnsOnlyTheRecordedFrame) {
    FFXFrameOwnerRing ring;
    ring.Record(5, Exact(FFXFrameOverlayOwner::kTopmost));
    EXPECT_EQ(ring.Lookup(5).owner, FFXFrameOverlayOwner::kTopmost);
    EXPECT_TRUE(ring.Lookup(5).frameExact);
    // Another frame in the same slot, and a frame never recorded, are unknown.
    EXPECT_EQ(ring.Lookup(5 + 16).owner, FFXFrameOverlayOwner::kUnknown);
    EXPECT_EQ(ring.Lookup(6).owner, FFXFrameOverlayOwner::kUnknown);
    ring.Record(5 + 16, Exact(FFXFrameOverlayOwner::kBaseline));
    EXPECT_EQ(ring.Lookup(5).owner, FFXFrameOverlayOwner::kUnknown);
    EXPECT_EQ(ring.Lookup(5 + 16).owner, FFXFrameOverlayOwner::kBaseline);
    ring.Record(0, Exact(FFXFrameOverlayOwner::kTopmost));
    EXPECT_EQ(ring.Lookup(0).owner, FFXFrameOverlayOwner::kUnknown);
    ring.Clear();
    EXPECT_EQ(ring.Lookup(5 + 16).owner, FFXFrameOverlayOwner::kUnknown);
}

TEST(FFXOutputFramesPolicyTest, ComposingFrameOnlyAdvances) {
    FFXComposingFrame composing;
    EXPECT_TRUE(composing.AdvanceTo(3));
    EXPECT_FALSE(composing.AdvanceTo(3));  // the Present's return after its first submission already advanced it
    EXPECT_FALSE(composing.AdvanceTo(2));
    EXPECT_EQ(composing.Current(), 3u);
    EXPECT_TRUE(composing.AdvanceTo(4));
    EXPECT_EQ(composing.Current(), 4u);
}

// The enable handover as AMD orders it (session 20261003_120641, 12:07:04.937): prework N retires the UI baseline and
// grants the final-batch route while AMD still composes frame N-1, whose outputs already carry the baseline.
TEST(FFXOutputFramesPolicyTest, GrantDrawsOnlyOnFramesWhoseBaselineWasRetired) {
    const FFXFrameOwnerRecord previous = Exact(FFXFrameOverlayOwner::kBaseline);
    const FFXFrameOwnerRecord retired = Exact(FFXFrameOverlayOwner::kTopmost);
    EXPECT_FALSE(ShouldDrawTopmostOnFFXOutput(previous, /*ownershipGranted=*/true));
    EXPECT_TRUE(ShouldDrawTopmostOnFFXOutput(retired, true));
    // A routing change already revoked the grant: the frames the route owned keep their draw.
    EXPECT_TRUE(ShouldDrawTopmostOnFFXOutput(retired, false));

    // Without a frame record (prework did not run) or a UI resource AMD reads live, the grant decides.
    FFXFrameOwnerRecord live;
    live.owner = FFXFrameOverlayOwner::kBaseline;
    EXPECT_TRUE(ShouldDrawTopmostOnFFXOutput(live, true));
    EXPECT_FALSE(ShouldDrawTopmostOnFFXOutput(FFXFrameOwnerRecord{}, false));
    EXPECT_TRUE(ShouldDrawTopmostOnFFXOutput(FFXFrameOwnerRecord{}, true));
}

TEST(FFXOutputFramesPolicyTest, OutputIsCoveredByExactlyOneOwner) {
    EXPECT_TRUE(JudgeFrameExactFFXOutput(FFXFrameOverlayOwner::kBaseline, false).covered);
    EXPECT_FALSE(JudgeFrameExactFFXOutput(FFXFrameOverlayOwner::kBaseline, false).doubleDrawn);
    EXPECT_TRUE(JudgeFrameExactFFXOutput(FFXFrameOverlayOwner::kTopmost, true).covered);
    EXPECT_FALSE(JudgeFrameExactFFXOutput(FFXFrameOverlayOwner::kTopmost, true).doubleDrawn);
    // The old handover: a baseline frame's output drawn again by the route.
    EXPECT_TRUE(JudgeFrameExactFFXOutput(FFXFrameOverlayOwner::kBaseline, true).doubleDrawn);
    // A retired frame's output the route missed (the routing change or teardown cleared it too early).
    EXPECT_FALSE(JudgeFrameExactFFXOutput(FFXFrameOverlayOwner::kTopmost, false).covered);
    EXPECT_FALSE(JudgeFrameExactFFXOutput(FFXFrameOverlayOwner::kNone, false).covered);
}

TEST(FFXOutputFramesSourceTest, ProxyPresentBracketsAMDPresentAndRecordsTheFrameOwner) {
    const std::string proxy = ReadSource("hook/d3d12/dx12_hook_ffx_proxy_present.cpp");
    ASSERT_FALSE(proxy.empty());
    for (const char* detour : {"DX12_FFXProxyDetourPresent(IDXGISwapChain* self", "DX12_FFXProxyDetourPresent1("}) {
        const size_t begin = proxy.find(detour);
        ASSERT_NE(begin, std::string::npos) << detour;
        const size_t frame = proxy.find("DX12_BeginFFXProxyFrame()", begin);
        const size_t prework = proxy.find("DX12_RunFFXProxyPrePresentWork(self", begin);
        const size_t forward = proxy.find("DX12_BeginFFXProxyForward(frame)", begin);
        const size_t original = proxy.find("const HRESULT hr = original(self", begin);
        const size_t end = proxy.find("DX12_EndFFXProxyForward(frame)", begin);
        EXPECT_LT(frame, prework) << detour;
        EXPECT_LT(prework, forward) << detour;
        EXPECT_LT(forward, original) << detour;
        EXPECT_LT(original, end) << detour;
    }
    const size_t prework = proxy.find("static void DX12_RunFFXProxyPrePresentWork(");
    EXPECT_NE(proxy.find("DX12_RecordFFXFrameOverlayOwner(frame, owner);", prework), std::string::npos);
    EXPECT_NE(proxy.find("kUiCompositionEnableInternalDoubleBuffering", prework), std::string::npos);
    const size_t account = proxy.find("void DX12_AccountFFXRuntimeOutputForOverlayCoverage(");
    EXPECT_NE(proxy.find("AccountPresentForOverlayCoverageVerdict(", account), std::string::npos);
}

TEST(FFXOutputFramesSourceTest, OutputsTakeTheFrameAtTheirFinalBatch) {
    const std::string topmost = ReadSource("hook/d3d12/dx12_hook_ffx_topmost_batch.cpp");
    ASSERT_FALSE(topmost.empty());
    // AMD's first submission inside its Present advances the composing frame before that batch is recorded.
    const size_t record = topmost.find("void RecordECLBatch(");
    const size_t advance = topmost.find("AdvanceFFXComposingFrame(t_FFXForwardFrame", record);
    const size_t store = topmost.find("g_FFXComposingFrame.Current()}", record);
    ASSERT_NE(record, std::string::npos);
    EXPECT_LT(advance, store);
    // The draw decision and the presented output both read the final batch's frame.
    const size_t append = topmost.find("bool DX12_TryAppendNoCallbackFSRTopmostOverlayToECL(");
    EXPECT_NE(topmost.find("ShouldDrawTopmostOnFFXOutput(frameOwner, ownershipGranted)", append), std::string::npos);
    const size_t observe = topmost.find("void DX12_ObserveNoCallbackFSRTopmostPresent(");
    const size_t stash = topmost.find("StashPresentedOutput();", observe);
    const size_t reset = topmost.find("ResetPresenterFrameTrace();", observe);
    EXPECT_LT(stash, reset);
}

// The last FSR FG frame's outputs are composed after the game's configure for its next frame and after it destroys
// the FG context: neither may end the route those outputs need.
TEST(FFXOutputFramesSourceTest, RoutingChangesAndEffectContextTeardownSpareInFlightFrames) {
    const std::string core = ReadSource("hook/present/dxgi_shared_present_core.cpp");
    const std::string ffx = ReadSource("hook/d3d12/dx12_hook_ffx.cpp");
    const std::string context = ReadSource("hook/ffx/ffx_hook_context.cpp");
    ASSERT_FALSE(core.empty());
    ASSERT_FALSE(ffx.empty());
    ASSERT_FALSE(context.empty());
    EXPECT_NE(core.find("(amdActivelyInterpolatingOnFGQueue || DX12_IsFFXComposingFrameOwnedByTopmost())"),
              std::string::npos);
    const size_t configured = ffx.find("void DX12_OnNativeFSRPresentCallbackRoutingConfigured(");
    const size_t nextBrace = ffx.find("\n}", configured);
    const size_t immediate = ffx.find("DX12_ClearNoCallbackFSRTopmostBatch(", configured);
    EXPECT_TRUE(immediate == std::string::npos || immediate > nextBrace);
    const size_t destroy = context.find("ffxReturnCode_t Hooked_ffxDestroyContext(");
    const size_t gate = context.find("if (isFGSwapchainContext || !isKnownContext) {", destroy);
    const size_t unregister = context.find("DX12_UnregisterNativeFSRSwapchainPresentationQueue(contextHandle", destroy);
    ASSERT_NE(gate, std::string::npos);
    EXPECT_LT(gate, unregister);
}

}  // namespace
