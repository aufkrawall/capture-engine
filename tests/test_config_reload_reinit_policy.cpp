#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "source_fragment_reader.h"

// Saving config.ini with only a sharpen key changed must stay a republish. In
// session 20260920_192913 it was not: it respawned the inject child, blacked out
// the injected overlay for ~7 s behind the controller's desktop "NOT RECORDING"
// warning, re-ran a ~100 s UE5 console-registry sweep, and finally deadlocked
// the game's RHI thread. These are the two source-level invariants behind that,
// neither of which the runtime paths can be reached from a unit test (they need
// a live D3D12 present and a live inject/controller IPC pair), so they are
// asserted against the translation units that own them.

namespace {

std::string ReadSource(const std::filesystem::path& relativePath) {
    const std::string source = ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
    EXPECT_FALSE(source.empty()) << relativePath.string();
    return source;
}

std::string FunctionBody(const std::string& source, const std::string& signature, const std::string& endMarker) {
    const size_t begin = source.find(signature);
    EXPECT_NE(begin, std::string::npos) << signature;
    if (begin == std::string::npos)
        return {};
    const size_t end = source.find(endMarker, begin + signature.size());
    EXPECT_NE(end, std::string::npos) << endMarker;
    if (end == std::string::npos)
        return {};
    return source.substr(begin, end - begin);
}

}  // namespace

// Post-processing no longer shares one global try-lock/pass. Drivers run
// outside a short resource-registry lock; live disable only collects retired work.
TEST(ConfigReloadReinitPolicyTest, Dx12GammaOffDoesNotWaitOrReenterTeardown) {
    const std::string source = ReadSource("hook/d3d12/dx12_hook_sharpen.cpp");
    const std::string presented = FunctionBody(source, "SharpenDX12PresentedFrame(IDXGISwapChain* pSwapChain,",
                                              "Microsoft::WRL::ComPtr<ID3D12Device> device");
    ASSERT_FALSE(presented.empty());
    EXPECT_NE(presented.find("CollectRuntimePostProcess(true)"), std::string::npos);
    EXPECT_EQ(presented.find("ReleaseDX12SharpenResources("), std::string::npos);
    EXPECT_EQ(source.find("std::try_to_lock"), std::string::npos);
    EXPECT_NE(source.find("ScopedCEOverlayECLSubmission submission(\"post-process\")"), std::string::npos);
}

// The post-process pass submits CE work on the game's queue. During DLSS-G transitions (FG off settling,
// warmup, keep-alive) the routing sets skipOverlayDraw precisely because such pre-SL submissions on the game
// queue hang the device. A call placed ahead of that routing (session 20261008_172629: GPU crash on the first
// DLSS FG off) must not exist; every call goes through the policy in post_process_route_policy.h.
TEST(ConfigReloadReinitPolicyTest, Dx12PostProcessRoutesAreGatedByTheirPolicy) {
    const std::string session = ReadSource("hook/d3d12/dx12_hook_process_session.cpp");
    ASSERT_FALSE(session.empty());
    EXPECT_EQ(session.find("SharpenDX12PresentedFrame"), std::string::npos);
    // Only overlay-init returns may fall back to the overlay-independent call, and both do.
    size_t fallbacks = 0;
    for (size_t at = session.find("RunPostProcessWhileOverlayUnavailable();"); at != std::string::npos;
         at = session.find("RunPostProcessWhileOverlayUnavailable();", at + 1))
        ++fallbacks;
    EXPECT_EQ(fallbacks, 2u);
    const size_t init = session.find("flow = InitOverlayBackend();");
    const size_t sync = session.find("flow = InitOverlaySyncAndFocusHold();");
    const size_t transition = session.find("flow = HandleOuterFGTransition();");
    const size_t firstFallback = session.find("RunPostProcessWhileOverlayUnavailable();");
    ASSERT_NE(init, std::string::npos);
    ASSERT_NE(sync, std::string::npos);
    ASSERT_NE(transition, std::string::npos);
    EXPECT_GT(firstFallback, init);
    EXPECT_LT(session.rfind("RunPostProcessWhileOverlayUnavailable();"), transition);

    const std::string draw = ReadSource("hook/d3d12/dx12_hook_process_session_draw_main.cpp");
    EXPECT_EQ(draw.find("SharpenDX12PresentedFrame"), std::string::npos);
    EXPECT_NE(draw.find("RunPostProcessOnNormalRoute();"), std::string::npos);

    const std::string route = ReadSource("hook/d3d12/dx12_hook_process_session_postprocess.cpp");
    const size_t normal = route.find("void FrameProcessSession::RunPostProcessOnNormalRoute()");
    const size_t unavailable = route.find("void FrameProcessSession::RunPostProcessWhileOverlayUnavailable()");
    ASSERT_NE(normal, std::string::npos);
    ASSERT_NE(unavailable, std::string::npos);
    const size_t normalGate = route.find("NormalRouteMayPostProcess(", normal);
    const size_t normalCall = route.find("SharpenDX12PresentedFrame(", normal);
    EXPECT_LT(normalGate, normalCall);
    const size_t quietGate = route.find("MayPostProcessWithoutOverlay(", unavailable);
    const size_t quietCall = route.find("SharpenDX12PresentedFrame(", unavailable);
    EXPECT_LT(quietGate, quietCall);
}

// Every place that sets skipOverlayDraw must say why: the post-process routing tells frames another route
// corrects (PostSL) from frames that stay uncorrected by that cause.
TEST(ConfigReloadReinitPolicyTest, Dx12EverySkipOverlayDrawSiteNamesItsCause) {
    for (const char* unit : {"hook/d3d12/dx12_hook_process_session_draw_main.cpp",
                             "hook/d3d12/dx12_hook_process_session_draw_transition.cpp"}) {
        const std::string source = ReadSource(unit);
        size_t sites = 0;
        for (size_t at = source.find("skipOverlayDraw = true;"); at != std::string::npos;
             at = source.find("skipOverlayDraw = true;", at + 1)) {
            ++sites;
            const std::string tail = source.substr(at, 320);
            EXPECT_NE(tail.find("skipCause"), std::string::npos) << unit << " site " << sites;
        }
        EXPECT_GE(sites, 2u) << unit;
    }
}

// FG off: the overlay cleanup on the Streamline present thread ran its whole 200 ms wait on every menu open
// (session 20261008_181746), because it queued a fresh Signal on the game queue and so waited for everything
// ahead of it there, Streamline's work included. The overlay fence already says when its own submissions are
// done; the full-queue flush stays only for a signal that was deferred past the last submit.
TEST(ConfigReloadReinitPolicyTest, Dx12OverlayCleanupWaitsOnItsOwnFenceNotTheWholeGameQueue) {
    const std::string source = ReadSource("hook/d3d12/dx12_hook_overlay.cpp");
    const std::string body = FunctionBody(source, "void CleanupOverlay(bool preserveNativeFSRPresentCallbackBackend)",
                                          "void CleanupRTVs()");
    ASSERT_FALSE(body.empty());
    const size_t deferred = body.find("if (deferredSignalPending)");
    const size_t signal = body.find("queueToFlush->Signal(");
    ASSERT_NE(deferred, std::string::npos);
    ASSERT_NE(signal, std::string::npos);
    EXPECT_LT(deferred, signal);
    EXPECT_NE(body.find("UINT64 waitValue = dx12_hook_g_State.currentFenceValue;"), std::string::npos);
    EXPECT_EQ(body.find("queueToFlush->Signal(", signal + 1), std::string::npos);
    EXPECT_NE(body.find("CleanupOverlay waited"), std::string::npos) << "a wait that is not instant must be logged";
}

// The Vulkan layer had the same shape and got it right; keep it that way so the
// two sharpen backends cannot drift apart on this.
TEST(ConfigReloadReinitPolicyTest, VulkanSharpenOffPathReleasesThroughTheUnlockedBody) {
    const std::string source = ReadSource("hook/vulkan_layer/layer_sharpen.cpp");

    const size_t tryLock = source.find("std::unique_lock<std::mutex> lock(layer_sharpen_g_StateMutex, std::defer_lock)");
    const size_t offBranch = source.find("!ce::sharpen::Requested(request)", tryLock);
    const size_t destroy = source.find("DestroySharpenState(state, disp)", offBranch);
    ASSERT_NE(tryLock, std::string::npos);
    ASSERT_NE(offBranch, std::string::npos);
    ASSERT_NE(destroy, std::string::npos);
}

// Publishing a new base config must not resolve a per-target config for every
// configured game inline. Each resolve is a full ReadLiteralIniValue pass over
// config.ini; at 27 whitelisted games that was ~4.9 s, and this function is what
// the inject child's ReloadConfig handler calls. That handler has a 1 s reply
// window, so the controller read a healthy-but-busy child as a broken channel
// and respawned it. The prewarm itself is kept (see test_ngx_ota_policy.cpp) and
// moved onto its own thread.
TEST(ConfigReloadReinitPolicyTest, BaseConfigPublicationQueuesTheWhitelistSweepInsteadOfRunningIt) {
    const std::string source = ReadSource("captureengine/injection/inject_config_publication.cpp");

    const std::string setBase =
        FunctionBody(source, "void SetPublicationBaseConfig(", "void StopPublicationWarmup(");
    ASSERT_FALSE(setBase.empty());

    // The whitelists are queued, never resolved here.
    EXPECT_NE(setBase.find("QueueWarmTargetLocked(publication, entry.pattern)"), std::string::npos);
    EXPECT_EQ(setBase.find("ResolveTargetConfig("), std::string::npos);
    EXPECT_NE(setBase.find("publication.warmWorker = std::thread(PublicationWarmupLoop)"), std::string::npos);

    // Every cached resolve describes the file that was just replaced, and the
    // generation bump is what stops an in-flight resolve from reseeding it.
    EXPECT_NE(setBase.find("++publication.configGeneration"), std::string::npos);
    EXPECT_NE(setBase.find("publication.resolvedTargetConfigs.clear()"), std::string::npos);
}

TEST(ConfigReloadReinitPolicyTest, PrewarmNeverHoldsThePublicationMutexAcrossAResolve) {
    const std::string source = ReadSource("captureengine/injection/inject_config_publication.cpp");

    const std::string loop =
        FunctionBody(source, "void PublicationWarmupLoop(", "void QueueWarmTargetLocked(");
    ASSERT_FALSE(loop.empty());

    // A resolve is ~180 ms of INI parsing. Holding the mutex across it would put
    // the injector's pre-LoadLibrary publish behind the whole sweep, which is
    // the latency the prewarm exists to remove in the first place.
    const size_t unlock = loop.find("lock.unlock()");
    const size_t resolve = loop.find("ResolveTargetConfig(configPath, baseConfig, target)", unlock);
    const size_t relock = loop.find("lock.lock()", resolve);
    ASSERT_NE(unlock, std::string::npos);
    ASSERT_NE(resolve, std::string::npos);
    ASSERT_NE(relock, std::string::npos);

    // A result that finished after a newer config arrived is dropped, not cached.
    const size_t generationCheck = loop.find("publication.configGeneration != generation", relock);
    const size_t insert = loop.find("publication.resolvedTargetConfigs.emplace(", generationCheck);
    ASSERT_NE(generationCheck, std::string::npos);
    ASSERT_NE(insert, std::string::npos);
    EXPECT_LT(generationCheck, insert);
}

TEST(ConfigReloadReinitPolicyTest, PrewarmIsAnOptimizationTheOnDemandPathDoesNotDependOn) {
    const std::string source = ReadSource("captureengine/injection/inject_config_publication.cpp");

    // A target the sweep has not reached yet must still resolve correctly, or
    // the prewarm stops being an optimization and becomes a correctness input.
    const std::string resolveActive =
        FunctionBody(source, "AppConfig ResolveActiveConfigLocked(", "void PublishConfigLocked(");
    ASSERT_FALSE(resolveActive.empty());
    EXPECT_NE(resolveActive.find("ResolveTargetConfig(publication.configPath, publication.baseConfig, targetProcessOut)"),
              std::string::npos);
    EXPECT_NE(resolveActive.find("ResolvedTargetConfig{targetProcessOut, resolved}"), std::string::npos);
}

// A thread that outlives the config it reads, or the mapping it publishes into,
// is a shutdown crash waiting for a slow disk.
TEST(ConfigReloadReinitPolicyTest, PrewarmWorkerIsJoinedBeforeInjectTearsDownItsState) {
    const std::string publication = ReadSource("captureengine/injection/inject_config_publication.cpp");
    const std::string stop =
        FunctionBody(publication, "void StopPublicationWarmup(", "void PublishResolvedConfig(");
    ASSERT_FALSE(stop.empty());
    EXPECT_NE(stop.find("publication.warmStop = true"), std::string::npos);
    EXPECT_NE(stop.find("publication.warmSignal.notify_all()"), std::string::npos);
    EXPECT_NE(stop.find("publication.warmWorker.join()"), std::string::npos);

    const std::string injectMain = ReadSource("captureengine/injection/inject_main.cpp");
    const size_t cleanup = injectMain.find("LogInfo(\"[Inject] Cleaning up...\")");
    const size_t join = injectMain.find("StopPublicationWarmup()", cleanup);
    const size_t unmap = injectMain.find("UnmapViewOfFile(pSharedMem)", cleanup);
    ASSERT_NE(cleanup, std::string::npos);
    ASSERT_NE(join, std::string::npos);
    ASSERT_NE(unmap, std::string::npos);
    EXPECT_LT(join, unmap);
}

// Process whitelisting must keep reading the discovery cache, never the resolved
// config map: the map is lazy now, so an injection decision that consulted it
// would depend on whether some earlier publish happened to warm the entry.
TEST(ConfigReloadReinitPolicyTest, WhitelistCacheIsIndependentOfResolvedTargetConfigs) {
    const std::string source = ReadSource("captureengine/injection/inject_config_publication.cpp");

    const std::string populate =
        FunctionBody(source, "void PopulateWhitelistCache(", "\n}\n");
    ASSERT_FALSE(populate.empty());
    EXPECT_NE(populate.find("config.gameWhitelist"), std::string::npos);
    EXPECT_NE(populate.find("config.overlayWhitelist"), std::string::npos);
    EXPECT_EQ(populate.find("resolvedTargetConfigs"), std::string::npos);
    EXPECT_EQ(populate.find("ResolveTargetConfig"), std::string::npos);
}
