#include <gtest/gtest.h>

#include <filesystem>
#include <regex>
#include <string>
#include <utility>

#include "source_fragment_reader.h"

// Source contract for the Present detour's stage accounting
// (hook/present/present_stage_cost.h). The accounting is only as good as the
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
    for (const char* file : {"hook/present/dxgi_shared_present.cpp", "hook/present/dxgi_shared_present_core.cpp",
                             "hook/present/dxgi_shared_present_routing.cpp", "hook/present/dxgi_shared_steam.cpp"}) {
        const std::string source = ReadProjectSource(file);
        ASSERT_FALSE(source.empty()) << file;
        std::smatch match;
        EXPECT_FALSE(std::regex_search(source, match, rawForward)) << file << ": raw forward " << match.str();
        EXPECT_NE(source.find("ForwardPresentThrough("), std::string::npos) << file;
    }

    const std::string internal = ReadProjectSource("hook/present/dxgi_shared_internal.h");
    const std::string helper = FunctionBody(internal, "inline HRESULT ForwardPresentThrough(");
    ASSERT_FALSE(helper.empty());
    const size_t scope = helper.find("StageScope forwardStage(ce::present_stage_cost::Stage::kForward);");
    const size_t call = helper.find("return present(pSwapChain, SyncInterval, Flags);");
    ASSERT_NE(scope, std::string::npos);
    ASSERT_NE(call, std::string::npos);
    EXPECT_LT(scope, call);

    const std::string original = ReadProjectSource("hook/present/dxgi_shared_original.cpp");
    const std::string callOriginal = FunctionBody(original, "HRESULT CallOriginalPresent(IDXGISwapChain*");
    ASSERT_FALSE(callOriginal.empty());
    const size_t originalScope = callOriginal.find("Stage::kForward");
    ASSERT_NE(originalScope, std::string::npos);
    EXPECT_LT(originalScope, callOriginal.find("return "));
    EXPECT_LT(originalScope, callOriginal.find("ScopedHookForwardedCall"));
}

// The same rule for Present1. It had no recorder until 20261104_144527, whose XeFG fence wait no stage showed.
TEST(PresentStageCostSourceTest, NoPresent1PathForwardsOutsideTheForwardStage) {
    const std::regex rawForward(R"(\b(present1Bypass|recursiveBypass1|dxgi_shared_oPresent1Trampoline|)"
                                R"(dxgi_shared_oPresent1Bypass|dxgi_shared_oPresent1)\(pSwapChain)");
    const std::string detour = ReadProjectSource("hook/present/dxgi_shared_present1.cpp");
    ASSERT_FALSE(detour.empty());
    std::smatch match;
    EXPECT_FALSE(std::regex_search(detour, match, rawForward)) << "raw forward " << match.str();
    EXPECT_NE(detour.find("ForwardPresent1Through("), std::string::npos);

    const std::string internal = ReadProjectSource("hook/present/dxgi_shared_internal.h");
    const std::string helper = FunctionBody(internal, "inline HRESULT ForwardPresent1Through(");
    ASSERT_FALSE(helper.empty());
    const size_t scope = helper.find("StageScope forwardStage(ce::present_stage_cost::Stage::kForward);");
    const size_t call = helper.find("return present(pSwapChain, SyncInterval, Flags, pParams);");
    ASSERT_NE(scope, std::string::npos);
    ASSERT_NE(call, std::string::npos);
    EXPECT_LT(scope, call);

    const std::string original = ReadProjectSource("hook/present/dxgi_shared_original_present1.cpp");
    const std::string callOriginal = FunctionBody(original, "HRESULT CallOriginalPresent1(IDXGISwapChain*");
    ASSERT_FALSE(callOriginal.empty());
    const size_t originalScope = callOriginal.find("Stage::kForward");
    ASSERT_NE(originalScope, std::string::npos);
    EXPECT_LT(originalScope, callOriginal.find("return "));
}

TEST(PresentStageCostSourceTest, DetourPresent1AdvancesThroughItsStagesInOrder) {
    const std::string source = ReadProjectSource("hook/present/dxgi_shared_present1.cpp");
    const std::string detour = FunctionBody(source, "HRESULT STDMETHODCALLTYPE DetourPresent1(");
    ASSERT_FALSE(detour.empty());
    const char* ordered[] = {
        "ce::present_stage_cost::DetourRecorder stageCost;",
        "EnterStage(CostStage::kEntry);",
        "ce::pacing_trace::PresentScope trace(",
        "EnterStage(CostStage::kKeepAlive);",
        "DX12_BeginOverlayPresentScope(pSwapChain);",
        "EnterStage(CostStage::kContext);",
        "ResolvePresent1DetourCaller(pSwapChain",
        "stageCost.SetRole(",
        "return ExecuteRuntimeOwnedOutputPresent(pSwapChain",
        "stageCost.SetRole(",
        "EnterStage(CostStage::kStartupRouting);",
        "EnterStage(CostStage::kCorePolicy);",
        "EnterStage(CostStage::kMetrics);",
        "UpdateDXGIPresentMetricsAndPublish(",
        "EnterStage(CostStage::kOverlay);",
        "HandleDX12ProcessFrame(",
        "HandleDX11ProcessFrame(",
        "EnterStage(CostStage::kLimiter);",
        "ApplyFpsLimiterStage(pSwapChain, true,",
        "EnterStage(CostStage::kOverlayWait);",
        "InvokeDX12WaitForOverlayCompletion(nullptr);",
        "EnterStage(CostStage::kCorePolicy);",
        "ForwardFinalPresent1(pSwapChain",
        "EnterStage(CostStage::kDeferredSignal);",
        "FlushDX12DeferredOverlaySignalAfterHookedPresent(",
        // CE's not-presentable hold runs on this result, and DetourPresent reported it while this detour did not. Flow
        // scenarios cannot reach it: they wrap the native swapchain, and the hold is off in an isolated hook.
        "EnterStage(CostStage::kPresentResult);",
        "NoteDX12PresentResultForVtablePath(pSwapChain, \"Present1\",",
        "EnterStage(CostStage::kLimiter);",
        "g_SharedFpsLimiter.ApplyPostPresent();",
        "EnterStage(CostStage::kPostPresent);",
        "NotePresentDeviceResult(hr, \"Present1\");",
    };
    size_t previous = 0;
    for (const char* marker : ordered) {
        const size_t position = detour.find(marker, previous);
        ASSERT_NE(position, std::string::npos) << marker;
        previous = position + 1;
    }
    // The overlay-scope teardown is post-present bookkeeping even on an early return.
    const size_t endScope = detour.find("DX12_EndOverlayPresentScope();");
    ASSERT_NE(endScope, std::string::npos);
    EXPECT_TRUE(PrecededWithin(detour, endScope, "Stage::kPostPresent", 1));
}

// The D3D12 preamble cost 9-16 us against 1.6 us on DXGI under Vulkan, and nothing said which part (20261104_144527).
TEST(PresentStageCostSourceTest, BothDetoursSplitTheirPreamble) {
    for (const auto& [file, signature] :
         {std::pair<const char*, const char*>{"hook/present/dxgi_shared_present.cpp",
                                              "HRESULT STDMETHODCALLTYPE DetourPresent("},
          std::pair<const char*, const char*>{"hook/present/dxgi_shared_present1.cpp",
                                              "HRESULT STDMETHODCALLTYPE DetourPresent1("}}) {
        const std::string detour = FunctionBody(ReadProjectSource(file), signature);
        ASSERT_FALSE(detour.empty()) << file;
        const size_t wrapper = detour.find("IsWrappedSwapChainObject(pSwapChain);");
        const size_t api = detour.find("DetectAPIType(pSwapChain);");
        const size_t window = detour.find("ShouldBypassDX12InvisibleWindowPresent(pSwapChain");
        ASSERT_NE(wrapper, std::string::npos) << file;
        ASSERT_NE(api, std::string::npos) << file;
        ASSERT_NE(window, std::string::npos) << file;
        EXPECT_TRUE(PrecededWithin(detour, wrapper, "EnterStage(CostStage::kEntryWrapper);", 1)) << file;
        EXPECT_TRUE(PrecededWithin(detour, api, "EnterStage(CostStage::kEntryApi);", 1)) << file;
        EXPECT_TRUE(PrecededWithin(detour, window, "EnterStage(CostStage::kEntryWindow);", 1)) << file;
    }
}

// Under DLSS FG the PostSL draw runs inside the routing and read as `startup_routing` 89-165 us (20261104_144527).
TEST(PresentStageCostSourceTest, ThePostSLDrawIsItsOwnStage) {
    const std::string shared = ReadProjectSource("hook/present/dxgi_shared.cpp");
    const std::string invoke = FunctionBody(shared, "void InvokePostSLCallbackForFinalOutputPresent(");
    ASSERT_FALSE(invoke.empty());
    const size_t call = invoke.find("callback(swapChain);");
    ASSERT_NE(call, std::string::npos);
    EXPECT_TRUE(PrecededWithin(invoke, call, "Stage::kPostSLOverlay", 1));

    const std::string render = ReadProjectSource("hook/d3d12/dx12_hook_postsl_render.cpp");
    const size_t record = render.find("flow = RecordOverlayDraw();");
    const size_t submit = render.find("flow = SubmitAndConfirmOutput();");
    ASSERT_NE(record, std::string::npos);
    ASSERT_NE(submit, std::string::npos);
    EXPECT_TRUE(PrecededWithin(render, record, "Stage::kOverlayRecord", 1));
    EXPECT_TRUE(PrecededWithin(render, submit, "Stage::kOverlaySubmit", 1));
}

// DOOM Eternal presents on another queue family: the compute route's whole cost read as `overlay` 50-65 us, and
// `overlay_record` counted only the frames that took the direct route (20261104_144527).
TEST(PresentStageCostSourceTest, TheVulkanComputeRouteRecordsTheSameStagesAsTheDirectRoute) {
    const std::string compute = ReadProjectSource("hook/vulkan_layer/layer_overlay_compute.cpp");
    const std::string route = FunctionBody(compute, "bool RenderComputePresentOverlay(");
    ASSERT_FALSE(route.empty());
    const size_t record = route.find("Stage::kOverlayRecord");
    const size_t draw = route.find("RenderOverlay(state.extent.width");
    const size_t submit = route.find("Stage::kOverlaySubmit");
    const size_t firstQueueSubmit = route.find("fp_vkQueueSubmit(");
    ASSERT_NE(record, std::string::npos);
    ASSERT_NE(draw, std::string::npos);
    ASSERT_NE(submit, std::string::npos);
    ASSERT_NE(firstQueueSubmit, std::string::npos);
    EXPECT_LT(record, draw);
    EXPECT_LT(draw, submit);
    EXPECT_LT(submit, firstQueueSubmit);
}

// The recorder closes last, and the detour's own sequence advances through the
// named stages in order around the calls they are named for.
TEST(PresentStageCostSourceTest, DetourPresentAdvancesThroughItsStagesInOrder) {
    const std::string source = ReadProjectSource("hook/present/dxgi_shared_present.cpp");
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
    const std::string core = ReadProjectSource("hook/present/dxgi_shared_present_core.cpp");
    ASSERT_FALSE(core.empty());
    const char* file = "dxgi_shared_present_core.cpp";
    ExpectEveryCallScoped(core, "UpdateDXGIPresentMetricsAndPublish(", "CostStage::kMetrics", 2, file);
    ExpectEveryCallScoped(core, "DX12_ObserveNoCallbackFSRTopmostPresent(", "CostStage::kFsrTopmost", 1, file);
    ExpectEveryCallScoped(core, "DX12_CompositeOverlayOntoCachedFFXUiResource();", "CostStage::kOverlay", 1, file);
    ExpectEveryCallScoped(core, "DX12_ProcessFrameMinimal(", "CostStage::kOverlay", 1, file);
    ExpectEveryCallScoped(core, "HandleDX12ProcessFrame(", "CostStage::kOverlay", 1, file);
    ExpectEveryCallScoped(core, "HandleDX11ProcessFrame(", "CostStage::kOverlay", 1, file);
    ExpectEveryCallScoped(core, "InvokeDX12WaitForOverlayCompletion(nullptr);", "CostStage::kOverlayWait", 1, file);
    ExpectEveryCallScoped(core, "ResolveDxgiPresentSite(", "CostStage::kLimiter", 4, file);
    // Streamline's external-overlay transport forwards outside the normal route: its deferred Signal read as
    // `core_policy` with `deferred_signal` n=0 (20261104_195245). The file's only other call is the normal route's flush
    // helper, whose call site opens the scope.
    const std::string presentCore = FunctionBody(core, "HRESULT ExecutePresentCore(");
    ASSERT_FALSE(presentCore.empty());
    ExpectEveryCallScoped(presentCore, "InvokeDX12FlushDeferredSignal();", "CostStage::kDeferredSignal", 1, file);
    size_t transportStage = presentCore.find("const auto finishTransportPresent =");
    ASSERT_NE(transportStage, std::string::npos);
    for (const char* marker : {"EnterStage(CostStage::kPostPresent);", "CostStage::kDeferredSignal",
                               "InvokeDX12FlushDeferredSignal();", "return FinishForwardedPresent(pSwapChain,"}) {
        transportStage = presentCore.find(marker, transportStage);
        ASSERT_NE(transportStage, std::string::npos) << marker;
    }
    const std::string transportReturn = "return finishTransportPresent(";
    const size_t firstTransportReturn = presentCore.find(transportReturn);
    ASSERT_NE(firstTransportReturn, std::string::npos);
    EXPECT_NE(presentCore.find(transportReturn, firstTransportReturn + 1), std::string::npos)
        << "both transports, the guarded Steam call and the bypass, finish the same way";

    // Post-present bookkeeping starts right after the final forward, before the signal flush.
    const size_t finalForward = core.rfind("hr = CallOriginalPresent(pSwapChain, SyncInterval, Flags);");
    const size_t postPresent = core.find("EnterStage(CostStage::kPostPresent);", finalForward);
    const size_t flush = core.find("FlushDX12DeferredOverlaySignalAfterHookedPresent(ctx.api", finalForward);
    ASSERT_NE(finalForward, std::string::npos);
    ASSERT_NE(postPresent, std::string::npos);
    ASSERT_NE(flush, std::string::npos);
    EXPECT_LT(postPresent, flush);
    EXPECT_NE(core.find("return FinishForwardedPresent(pSwapChain,", flush), std::string::npos);

    // Every forward ends through one function. CE's not-presentable hold and device-loss latch run on its reports, and
    // Streamline's Steam transport left them out (`present_result` n=0, 20261104_195245). Flow scenarios cannot reach
    // that route, which needs the Steam overlay, or the hold, which is off in an isolated hook.
    const std::string finish = FunctionBody(core, "HRESULT FinishForwardedPresent(");
    ASSERT_FALSE(finish.empty());
    size_t finishStep = 0;
    for (const char* marker : {"CostStage::kPresentResult", "NoteDX12PresentResultForVtablePath(pSwapChain, presentName,",
                               // The limiter's post-present wait is the limiter's, not bookkeeping.
                               "CostStage::kLimiter", "g_SharedFpsLimiter.ApplyPostPresent();",
                               "NotePresentDeviceResult(hr, presentName);"}) {
        finishStep = finish.find(marker, finishStep);
        ASSERT_NE(finishStep, std::string::npos) << marker;
    }
}

// CE's swapchain wrappers process the frame themselves when no foreign chain is loaded, and the detour their real
// Present re-enters only nests inside it: without a recorder that whole route was invisible (dx12_test, 20261104_133008:
// overlay n=0, own=42 us).
TEST(PresentStageCostSourceTest, TheSwapchainWrappersRecordTheFrameTheyProcess) {
    const std::string wrapper = ReadProjectSource("hook/wrappers/dxgi_swapchain_wrap_present.cpp");
    ASSERT_FALSE(wrapper.empty());
    for (const auto& [signature, forward] :
         {std::pair<const char*, const char*>{"HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::Present(UINT",
                                              "HRESULT hr = pRealCached->Present(SyncInterval, presentFlags);"},
          std::pair<const char*, const char*>{"HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::Present1(UINT",
                                              "HRESULT hr = pReal1Cached->Present1(SyncInterval, PresentFlags"}}) {
        const std::string body = FunctionBody(wrapper, signature);
        ASSERT_FALSE(body.empty()) << signature;
        const char* ordered[] = {"ce::present_stage_cost::DetourRecorder stageCost;", "g_InWrapperPresent = true;",
                                 "EnterStage(CostStage::kOverlay);", "EnterStage(CostStage::kLimiter);",
                                 "CostScope overlayWaitStage(CostStage::kOverlayWait);",
                                 "EnterStage(CostStage::kForward);", forward, "EnterStage(CostStage::kPostPresent);",
                                 "CostScope deferredSignalStage(CostStage::kDeferredSignal);"};
        size_t previous = 0;
        for (const char* marker : ordered) {
            const size_t position = body.find(marker, previous);
            ASSERT_NE(position, std::string::npos) << signature << ": " << marker;
            previous = position;
        }
    }
}

// vkQueuePresentKHR has no DXGI detour around it: 200 us per present showed up in no CSV column
// (Strange Brigade Vulkan, 20261104_131318).
TEST(PresentStageCostSourceTest, TheVulkanPresentRecordsItsStagesAndItsHostThreadReports) {
    const std::string present = ReadProjectSource("hook/vulkan_layer/vulkan_layer_present.cpp");
    const std::string detour = FunctionBody(present, "VKAPI_ATTR VkResult VKAPI_CALL Capture_vkQueuePresentKHR(");
    ASSERT_FALSE(detour.empty());
    const char* ordered[] = {"ce::present_stage_cost::DetourRecorder stageCost;", "EnterStage(CostStage::kEntry);",
                             "CostScope limiterStage(CostStage::kLimiter);", "CostScope overlayStage(CostStage::kOverlay);",
                             "EnterStage(CostStage::kForward);", "fp_vkQueuePresentKHR(queue,",
                             "EnterStage(CostStage::kPostPresent);", "CostScope perfRowStage(CostStage::kPerfRow);"};
    size_t previous = 0;
    for (const char* marker : ordered) {
        const size_t position = detour.find(marker, previous);
        ASSERT_NE(position, std::string::npos) << marker;
        previous = position;
    }
    const std::string internal = ReadProjectSource("hook/vulkan_layer/vulkan_layer_internal.h");
    const size_t limit = internal.find("inline uint32_t ApplyPrerenderLimitVulkan(");
    ASSERT_NE(limit, std::string::npos);
    const size_t wait = internal.find("Stage::kPrerenderWait", limit);
    const size_t marker = internal.find("Stage::kPrerenderMarker", limit);
    EXPECT_NE(wait, std::string::npos);
    EXPECT_NE(marker, std::string::npos);
    const std::string ipc = ReadProjectSource("hook/vulkan_layer/layer_ipc.cpp");
    EXPECT_NE(ipc.find("ce::present_stage_cost::ReportPresentStageCostIfDue();"), std::string::npos);
    const std::string build = ReadProjectSource("tools/build/build_vulkan_layer.py");
    EXPECT_NE(build.find("\"present_stage_cost_report.cpp\""), std::string::npos);
}

// A Signal before the overlay costs a few microseconds (20261104_132859): the split that shows it keeps it apart from
// the wait on the same line.
TEST(PresentStageCostSourceTest, TheD3D12PrerenderLimitSeparatesItsMarkerFromItsWait) {
    const std::string prerender = ReadProjectSource("hook/d3d12/dx12_hook_prerender.cpp");
    ASSERT_FALSE(prerender.empty());
    const size_t marker = prerender.find("Stage::kPrerenderMarker");
    const size_t signal = prerender.find("signalHr = ctx.queue->Signal(fence, signalValue);", marker);
    const size_t wait = prerender.find("Stage::kPrerenderWait", signal);
    const size_t waitCall = prerender.find("waitForFence(waitFence, waitEvent, waitValue);", wait);
    ASSERT_NE(marker, std::string::npos);
    ASSERT_NE(signal, std::string::npos);
    ASSERT_NE(wait, std::string::npos);
    ASSERT_NE(waitCall, std::string::npos);
}

// The present threads never format or log: the hook service loop reports.
TEST(PresentStageCostSourceTest, TheReportRunsOnTheHookServiceThreadOnly) {
    const std::string hookThread = ReadProjectSource("hook/runtime/main_hookthread.cpp");
    const size_t service = hookThread.find("ce::pacing_trace::Service();");
    const size_t report = hookThread.find("ce::present_stage_cost::ReportPresentStageCostIfDue();");
    ASSERT_NE(service, std::string::npos);
    ASSERT_NE(report, std::string::npos);
    EXPECT_LT(service, report);
    for (const char* file : {"hook/present/dxgi_shared_present.cpp", "hook/present/dxgi_shared_present_core.cpp",
                             "hook/present/dxgi_shared_present_routing.cpp"}) {
        EXPECT_EQ(ReadProjectSource(file).find("ReportPresentStageCostIfDue"), std::string::npos) << file;
    }
}
