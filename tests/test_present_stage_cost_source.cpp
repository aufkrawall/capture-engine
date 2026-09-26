#include <gtest/gtest.h>

#include <filesystem>
#include <regex>
#include <string>

#include "source_fragment_reader.h"

// Source contract for the Present detour's stage accounting
// (hook/common/present_stage_cost.h). The accounting is only as good as the
// placement of its scopes: a forward that escapes kForward bills the runtime's
// Present to CE, and a region that loses its scope silently moves into
// unattributed. These tests pin the placement.

namespace {
std::string ReadProjectSource(const char* relativePath) {
    const std::filesystem::path source = std::filesystem::current_path() / relativePath;
    return std::filesystem::exists(source) ? ce::test_source::ReadFile(source) : std::string{};
}

// The function body starting at `signature`, up to the next top-level closing brace.
std::string FunctionBody(const std::string& source, const std::string& signature) {
    const size_t begin = source.find(signature);
    if (begin == std::string::npos) {
        return {};
    }
    const size_t end = source.find("\n}\n", begin);
    return source.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

// True when `marker` appears in one of the `lines` lines before `position`.
bool PrecededWithin(const std::string& source, size_t position, const std::string& marker, int lines) {
    size_t windowBegin = source.rfind('\n', position);
    for (int i = 0; i < lines && windowBegin != std::string::npos && windowBegin > 0; ++i) {
        windowBegin = source.rfind('\n', windowBegin - 1);
    }
    const size_t from = windowBegin == std::string::npos ? 0 : windowBegin;
    const size_t found = source.find(marker, from);
    return found != std::string::npos && found < position;
}

void ExpectEveryCallScoped(const std::string& source, const std::string& call, const std::string& marker,
                           int lines, const char* file) {
    size_t count = 0;
    for (size_t pos = source.find(call); pos != std::string::npos; pos = source.find(call, pos + 1)) {
        ++count;
        EXPECT_TRUE(PrecededWithin(source, pos, marker, lines)) << file << ": " << call << " not inside " << marker;
    }
    EXPECT_GT(count, 0u) << file << ": " << call;
}
}  // namespace

// Every Present the detour path hands to the runtime must be inside kForward.
// Raw calls through the saved pointers are the way that breaks, so none may remain:
// they go through ForwardPresentThrough or CallOriginalPresent.
TEST(PresentStageCostSourceTest, NoDetourPathForwardsPresentOutsideTheForwardStage) {
    const std::regex rawForward(
        R"(\b(presentBypass|recursiveBypass|bypass|dxgi_shared_oPresentTrampoline|dxgi_shared_oPresentBypass|)"
        R"(dxgi_shared_oPresent|externalPresent)\(pSwapChain)");
    for (const char* file : {"hook/common/dxgi_shared_present.cpp", "hook/common/dxgi_shared_present_core.cpp",
                             "hook/common/dxgi_shared_present_routing.cpp", "hook/common/dxgi_shared_steam.cpp"}) {
        const std::string source = ReadProjectSource(file);
        ASSERT_FALSE(source.empty()) << file;
        std::smatch match;
        EXPECT_FALSE(std::regex_search(source, match, rawForward)) << file << ": raw forward " << match.str();
        EXPECT_NE(source.find("ForwardPresentThrough("), std::string::npos) << file;
    }

    const std::string internal = ReadProjectSource("hook/common/dxgi_shared_internal.h");
    const std::string helper = FunctionBody(internal, "inline HRESULT ForwardPresentThrough(");
    ASSERT_FALSE(helper.empty());
    const size_t scope = helper.find("StageScope forwardStage(ce::present_stage_cost::Stage::kForward);");
    const size_t call = helper.find("return present(pSwapChain, SyncInterval, Flags);");
    ASSERT_NE(scope, std::string::npos);
    ASSERT_NE(call, std::string::npos);
    EXPECT_LT(scope, call);

    const std::string original = ReadProjectSource("hook/common/dxgi_shared_original.cpp");
    const std::string callOriginal = FunctionBody(original, "HRESULT CallOriginalPresent(IDXGISwapChain*");
    ASSERT_FALSE(callOriginal.empty());
    const size_t originalScope = callOriginal.find("Stage::kForward");
    ASSERT_NE(originalScope, std::string::npos);
    EXPECT_LT(originalScope, callOriginal.find("return "));
    EXPECT_LT(originalScope, callOriginal.find("ScopedHookForwardedCall"));
}

// The recorder closes last, and the detour's own sequence advances through the
// named stages in order around the calls they are named for.
TEST(PresentStageCostSourceTest, DetourPresentAdvancesThroughItsStagesInOrder) {
    const std::string source = ReadProjectSource("hook/common/dxgi_shared_present.cpp");
    const std::string detour = FunctionBody(source, "HRESULT STDMETHODCALLTYPE DetourPresent(");
    ASSERT_FALSE(detour.empty());
    const char* ordered[] = {
        "ce::present_stage_cost::DetourRecorder stageCost;",
        "EnterStage(CostStage::kEntry);",
        "ce::pacing_trace::PresentScope trace(",
        "ScopedHookCpuCost presentCpuCost(",
        "DX12_BeginOverlayPresentScope(pSwapChain);",
        "EnterStage(CostStage::kKeepAlive);",
        "DX12_TryRenderExactPostSLOffKeepAliveBeforePresent(",
        "EnterStage(CostStage::kContext);",
        "CapturePresentCallContext(pSwapChain",
        "stageCost.SetRole(",
        "EnterStage(CostStage::kStartupRouting);",
        "ExecuteStartupRouting(pSwapChain",
        "EnterStage(CostStage::kPostPresent);",
        "EnterStage(CostStage::kCorePolicy);",
        "return ExecutePresentCore(pSwapChain",
    };
    size_t previous = 0;
    for (const char* marker : ordered) {
        const size_t position = detour.find(marker, previous);
        ASSERT_NE(position, std::string::npos) << marker;
        previous = position;
    }
    // The overlay-scope teardown is post-present bookkeeping even on an early return.
    const size_t endScope = detour.find("DX12_EndOverlayPresentScope();");
    ASSERT_NE(endScope, std::string::npos);
    EXPECT_TRUE(PrecededWithin(detour, endScope, "Stage::kPostPresent", 1));
}

TEST(PresentStageCostSourceTest, PresentCoreWrapsItsCostlyRegionsInNamedStages) {
    const std::string core = ReadProjectSource("hook/common/dxgi_shared_present_core.cpp");
    ASSERT_FALSE(core.empty());
    const char* file = "dxgi_shared_present_core.cpp";
    ExpectEveryCallScoped(core, "UpdateDXGIPresentMetricsAndPublish(", "CostStage::kMetrics", 2, file);
    ExpectEveryCallScoped(core, "DX12_ObserveNoCallbackFSRTopmostPresent(", "CostStage::kFsrTopmost", 1, file);
    ExpectEveryCallScoped(core, "DX12_CompositeOverlayOntoCachedFFXUiResource();", "CostStage::kOverlay", 1, file);
    ExpectEveryCallScoped(core, "DX12_ProcessFrameMinimal(", "CostStage::kOverlay", 1, file);
    ExpectEveryCallScoped(core, "HandleDX12ProcessFrame(", "CostStage::kOverlay", 1, file);
    ExpectEveryCallScoped(core, "HandleDX11ProcessFrame(", "CostStage::kOverlay", 1, file);
    ExpectEveryCallScoped(core, "InvokeDX12WaitForOverlayCompletion(nullptr);", "CostStage::kOverlayWait", 1, file);
    ExpectEveryCallScoped(core, "PresentSite::kUniqueApplicationPresent", "CostStage::kLimiter", 3, file);

    // Post-present bookkeeping starts right after the final forward, before the signal flush.
    const size_t finalForward = core.rfind("hr = CallOriginalPresent(pSwapChain, SyncInterval, Flags);");
    const size_t postPresent = core.find("EnterStage(CostStage::kPostPresent);", finalForward);
    const size_t flush = core.find("FlushDX12DeferredOverlaySignalAfterHookedPresent(ctx.api", finalForward);
    ASSERT_NE(finalForward, std::string::npos);
    ASSERT_NE(postPresent, std::string::npos);
    ASSERT_NE(flush, std::string::npos);
    EXPECT_LT(postPresent, flush);
    // The limiter's post-present wait is the limiter's, not bookkeeping.
    const size_t postLimiter = core.find("g_SharedFpsLimiter.ApplyPostPresent();", flush);
    ASSERT_NE(postLimiter, std::string::npos);
    EXPECT_TRUE(PrecededWithin(core, postLimiter, "CostStage::kLimiter", 1));
}

// The present threads never format or log: the hook service loop reports.
TEST(PresentStageCostSourceTest, TheReportRunsOnTheHookServiceThreadOnly) {
    const std::string hookThread = ReadProjectSource("hook/main_hookthread.cpp");
    const size_t service = hookThread.find("ce::pacing_trace::Service();");
    const size_t report = hookThread.find("ce::present_stage_cost::ReportPresentStageCostIfDue();");
    ASSERT_NE(service, std::string::npos);
    ASSERT_NE(report, std::string::npos);
    EXPECT_LT(service, report);
    for (const char* file : {"hook/common/dxgi_shared_present.cpp", "hook/common/dxgi_shared_present_core.cpp",
                             "hook/common/dxgi_shared_present_routing.cpp"}) {
        EXPECT_EQ(ReadProjectSource(file).find("ReportPresentStageCostIfDue"), std::string::npos) << file;
    }
}
