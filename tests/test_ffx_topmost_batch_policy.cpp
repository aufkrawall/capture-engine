#include <gtest/gtest.h>

#include <filesystem>
#include <limits>
#include <string>

#include "../hook/common/dx12_overlay_policy/ffx_topmost_batch.h"

#include "source_fragment_reader.h"

namespace {

using ce::dx12_overlay_policy::AdvanceFinalECLBatchSignatureStability;
using ce::dx12_overlay_policy::FinalECLBatchSignature;
using ce::dx12_overlay_policy::ShouldAppendTopmostOverlayToFinalECLBatch;

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

TEST(FFXTopmostBatchPolicyTest, RequiresTwoConsecutiveIdenticalFinalBatches) {
    const FinalECLBatchSignature target{0x1234, 0x9876, 2};

    EXPECT_EQ(AdvanceFinalECLBatchSignatureStability({}, 0, target), 1u);
    EXPECT_FALSE(ShouldAppendTopmostOverlayToFinalECLBatch(
        true, 1, target, target.callSite, target.queueIdentity, target.ordinal, 2, 129));
    EXPECT_EQ(AdvanceFinalECLBatchSignatureStability(target, 1, target), 2u);
    EXPECT_TRUE(ShouldAppendTopmostOverlayToFinalECLBatch(
        true, 2, target, target.callSite, target.queueIdentity, target.ordinal, 2, 129));
}

TEST(FFXTopmostBatchPolicyTest, SignatureChangeRestartsLearning) {
    const FinalECLBatchSignature previous{0x1234, 0x9876, 2};

    EXPECT_EQ(AdvanceFinalECLBatchSignatureStability(previous, 37, {0x5678, 0x9876, 2}), 1u);
    EXPECT_EQ(AdvanceFinalECLBatchSignatureStability(previous, 37, {0x1234, 0x6789, 2}), 1u);
    EXPECT_EQ(AdvanceFinalECLBatchSignatureStability(previous, 37, {0x1234, 0x9876, 3}), 1u);
    EXPECT_EQ(AdvanceFinalECLBatchSignatureStability(previous, 37, {}), 0u);
}

TEST(FFXTopmostBatchPolicyTest, StabilityCounterSaturates) {
    const FinalECLBatchSignature target{0x1234, 0x9876, 1};
    EXPECT_EQ(AdvanceFinalECLBatchSignatureStability(
                  target, std::numeric_limits<uint32_t>::max(), target),
              std::numeric_limits<uint32_t>::max());
}

TEST(FFXTopmostBatchPolicyTest, RequiresExactCallSiteAndOrdinal) {
    const FinalECLBatchSignature target{0x1234, 0x9876, 3};

    EXPECT_FALSE(ShouldAppendTopmostOverlayToFinalECLBatch(true, 2, target, 0x5678, 0x9876, 3, 1, 129));
    EXPECT_FALSE(ShouldAppendTopmostOverlayToFinalECLBatch(true, 2, target, 0x1234, 0x6789, 3, 1, 129));
    EXPECT_FALSE(ShouldAppendTopmostOverlayToFinalECLBatch(true, 2, target, 0x1234, 0x9876, 2, 1, 129));
    EXPECT_TRUE(ShouldAppendTopmostOverlayToFinalECLBatch(true, 2, target, 0x1234, 0x9876, 3, 1, 129));
}

TEST(FFXTopmostBatchPolicyTest, RefusesInvalidOrUnrepresentableBatch) {
    const FinalECLBatchSignature target{0x1234, 0x9876, 1};

    EXPECT_FALSE(ShouldAppendTopmostOverlayToFinalECLBatch(false, 2, target, 0x1234, 0x9876, 1, 1, 129));
    EXPECT_FALSE(ShouldAppendTopmostOverlayToFinalECLBatch(true, 2, {}, 0x1234, 0x9876, 1, 1, 129));
    EXPECT_FALSE(ShouldAppendTopmostOverlayToFinalECLBatch(true, 2, target, 0x1234, 0x9876, 1, 0, 129));
    EXPECT_FALSE(ShouldAppendTopmostOverlayToFinalECLBatch(true, 2, target, 0x1234, 0x9876, 1, 129, 129));
}

TEST(FFXTopmostBatchSourceTest, OverlayIsLastInOneExistingExecuteCommandListsCall) {
    const std::string source = ReadSource("hook/apis/dx12_hook_ffx_topmost_batch.cpp");
    ASSERT_FALSE(source.empty());

    const size_t append = source.find("combined[context->commandListCount] = overlayCommandList;");
    const size_t oneSubmit =
        source.find("context->original(queue, context->commandListCount + 1, combined.data());", append);
    ASSERT_NE(append, std::string::npos);
    ASSERT_NE(oneSubmit, std::string::npos);
    EXPECT_LT(append, oneSubmit);
    EXPECT_NE(source.find("request.inlineCompletionMarker = true;"), std::string::npos);
    EXPECT_EQ(source.find("Signal("), std::string::npos);
}

TEST(FFXTopmostBatchSourceTest, HotPathIsGenericAndExcludesCEOwnedSubmissions) {
    const std::string source = ReadSource("hook/apis/dx12_hook_ecl.cpp");
    ASSERT_FALSE(source.empty());

    const size_t exclusion = source.find("dx12_hook_s_insideCEOverlayECLDepth == 0");
    const size_t append = source.find("DX12_TryAppendNoCallbackFSRTopmostOverlayToECL", exclusion);
    ASSERT_NE(exclusion, std::string::npos);
    ASSERT_NE(append, std::string::npos);
    EXPECT_LT(exclusion, append);
}

TEST(FFXTopmostBatchSourceTest, FallbackAndInlineRenderersCoexistAndHandoffWithoutDoubleBlend) {
    const std::string renderer = ReadSource("hook/apis/dx12_ffx_suspend_overlay.cpp");
    const std::string proxy = ReadSource("hook/apis/dx12_hook_ffx_proxy_present.cpp");
    ASSERT_FALSE(renderer.empty());
    ASSERT_FALSE(proxy.empty());

    EXPECT_NE(renderer.find("request.inlineCompletionMarker ? g_InlineProxyStates : g_ProxyStates"),
              std::string::npos);
    EXPECT_NE(renderer.find("WriteBufferImmediate"), std::string::npos);
    EXPECT_NE(proxy.find("active-ui-resource-retire-ce-pixels"), std::string::npos);
    EXPECT_NE(proxy.find("request.renderOverlay = !clearOnly;"), std::string::npos);
}

}  // namespace
