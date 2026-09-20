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

// g_SharpenMutex is a plain std::mutex. SharpenDX12PresentedFrame holds it for
// the whole call, so its sharpen-off teardown has to use the unlocked body: the
// locking entry point re-acquires and, on libc++/SRWLOCK, never returns. That
// parked the RHI thread inside DetourPresent until UE5 declared "GameThread
// timed out waiting for RenderThread after 120.00 secs" and terminated Talos.
TEST(ConfigReloadReinitPolicyTest, Dx12SharpenOffPathReleasesThroughTheUnlockedBody) {
    const std::string source = ReadSource("hook/apis/dx12_hook_sharpen.cpp");

    const std::string presented =
        FunctionBody(source, "void SharpenDX12PresentedFrame(", "ID3D12Device* device = g_Device.load");
    ASSERT_FALSE(presented.empty());

    // It takes the lock for the whole call...
    EXPECT_NE(presented.find("std::unique_lock<std::mutex> lock(g_SharpenMutex, std::try_to_lock)"),
              std::string::npos);
    // ...so the off-branch must call the unlocked body...
    EXPECT_NE(presented.find("ReleaseSharpenResourcesLocked("), std::string::npos);
    // ...and must never re-enter the locking entry point.
    EXPECT_EQ(presented.find("ReleaseDX12SharpenResources("), std::string::npos);
}

TEST(ConfigReloadReinitPolicyTest, Dx12SharpenLockingReleaseIsOnlyAWrapper) {
    const std::string source = ReadSource("hook/apis/dx12_hook_sharpen.cpp");

    // The public entry point is for callers that do not hold the mutex
    // (DX12OverlayState::Cleanup). It must take the lock and do nothing else,
    // so there is exactly one place that touches g_SharpenPass teardown.
    const std::string release =
        FunctionBody(source, "void ReleaseDX12SharpenResources(", "void SharpenDX12PresentedFrame(");
    ASSERT_FALSE(release.empty());
    EXPECT_NE(release.find("std::lock_guard<std::mutex> lock(g_SharpenMutex)"), std::string::npos);
    EXPECT_NE(release.find("ReleaseSharpenResourcesLocked(releaseObjects)"), std::string::npos);
    EXPECT_EQ(release.find("g_SharpenPass."), std::string::npos);

    // The unlocked body owns the "nothing is allocated any more" bookkeeping,
    // so a caller cannot release the pass and leave the flag claiming otherwise.
    const std::string body =
        FunctionBody(source, "void ReleaseSharpenResourcesLocked(", "}  // namespace");
    ASSERT_FALSE(body.empty());
    EXPECT_NE(body.find("g_SharpenPass.Shutdown()"), std::string::npos);
    EXPECT_NE(body.find("g_SharpenPass.Abandon()"), std::string::npos);
    EXPECT_NE(body.find("g_SharpenEverRendered = false"), std::string::npos);
}

// The Vulkan layer had the same shape and got it right; keep it that way so the
// two sharpen backends cannot drift apart on this.
TEST(ConfigReloadReinitPolicyTest, VulkanSharpenOffPathReleasesThroughTheUnlockedBody) {
    const std::string source = ReadSource("hook/vulkan_layer/layer_sharpen.cpp");

    const size_t tryLock = source.find("std::unique_lock<std::mutex> lock(layer_sharpen_g_StateMutex, std::try_to_lock)");
    const size_t offBranch = source.find("request.mode == ce::sharpen::Mode::Off", tryLock);
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
    const std::string source = ReadSource("captureengine/inject_config_publication.cpp");

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
    const std::string source = ReadSource("captureengine/inject_config_publication.cpp");

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
    const std::string source = ReadSource("captureengine/inject_config_publication.cpp");

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
    const std::string publication = ReadSource("captureengine/inject_config_publication.cpp");
    const std::string stop =
        FunctionBody(publication, "void StopPublicationWarmup(", "void PublishResolvedConfig(");
    ASSERT_FALSE(stop.empty());
    EXPECT_NE(stop.find("publication.warmStop = true"), std::string::npos);
    EXPECT_NE(stop.find("publication.warmSignal.notify_all()"), std::string::npos);
    EXPECT_NE(stop.find("publication.warmWorker.join()"), std::string::npos);

    const std::string injectMain = ReadSource("captureengine/inject_main.cpp");
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
    const std::string source = ReadSource("captureengine/inject_config_publication.cpp");

    const std::string populate =
        FunctionBody(source, "void PopulateWhitelistCache(", "\n}\n");
    ASSERT_FALSE(populate.empty());
    EXPECT_NE(populate.find("config.gameWhitelist"), std::string::npos);
    EXPECT_NE(populate.find("config.overlayWhitelist"), std::string::npos);
    EXPECT_EQ(populate.find("resolvedTargetConfigs"), std::string::npos);
    EXPECT_EQ(populate.find("ResolveTargetConfig"), std::string::npos);
}
