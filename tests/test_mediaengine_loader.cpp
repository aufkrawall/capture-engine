#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "captureengine/app/mediaengine_loader.h"
#include "common/logging/logging.h"
#include "common/platform/build_identity_check.h"
#include "common/platform/secure_dll_loading.h"

namespace loader_test {
void MediaEngine_Unload();
// NOLINTNEXTLINE(bugprone-suspicious-include) - exercise the actual private loader orchestration
#include "captureengine/app/mediaengine_loader.cpp"
}  // namespace loader_test

namespace {
namespace bid = ce::build_identity;

INT_PTR WINAPI ExportStub() { return 0; }

// What the fake mediaengine.dll answers through its identity export.
bid::Record g_moduleIdentity;
bool g_identityExportAccepts = true;
bool FakeIdentityExport(bid::Record* out) {
    if (!g_identityExportAccepts)
        return false;
    *out = g_moduleIdentity;
    return true;
}

// A DLL of this very build: every export is a stub, the identity export answers this binary's identity.
FARPROC ResolveFromMatchingModule(const char* name) {
    if (strcmp(name, bid::kMediaEngineExportName) == 0)
        return reinterpret_cast<FARPROC>(&FakeIdentityExport);
    return reinterpret_cast<FARPROC>(&ExportStub);
}

void UseMatchingModuleIdentity() {
    g_moduleIdentity = bid::Current();
    g_identityExportAccepts = true;
}

void ExpectPointersCleared() {
    EXPECT_EQ(loader_test::MediaEngine_SubmitFrameWithResultV1, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_SubmitFrameD3D11WithResultV1, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_RepeatLastFrameWithResultV1, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_SetLogCallback, nullptr);
    EXPECT_EQ(loader_test::DLL_Log, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_Init, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_ReloadConfig, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_SetActiveScreenGrab, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_SetWgcStartupExtraDelayQpc, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_ProcessFrame, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_RepeatLastFrame, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_RepeatLastFrameWithTimeline, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_CanRepeatLastFrame, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_ResetRepeatFrameCache, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_PrepareFrameD3D11, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_PrepareInjectVideo, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_ProcessFrameD3D11.raw, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_StartRecording, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_StopRecording, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_GetLastOutputDegradedFlags, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_GetLastOutputPath, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_ReleaseEncoderTextures, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_GetD3D11Device, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_ReleaseSharedD3D11Device, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_CreateSharedCaptureTextures, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_GetLastFrameEncodeTimeUs, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_GetLastFrameFenceWaitUs, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_WasLastFrameDeferred, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_GetMuxFlowSnapshotV1, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_QueryInjectFrameCopyCompletion, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_SetInjectTransportGeneration, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_TakeInjectSourceReadToken, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_IsInjectSourceReadComplete, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_SetInjectSourceReadWakeEvent, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_FinishInjectSourceReads, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_Shutdown, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_SetSharedMem, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_LockD3D11, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_UnlockD3D11, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_SetAudioOnly, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_SetSourcePrefers10Bit, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_SetCursorCompositionSuppressed, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_MeasureRenderEndpointLatency, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_SetRenderLatencyChannel, nullptr);
}
}  // namespace

TEST(MediaEngineLoaderTest, AllRequiredExportsResolveAndUnloadClearsEveryPointer) {
    UseMatchingModuleIdentity();
    std::vector<std::string> names;
    auto resolve = [&](HMODULE, const char* name) {
        names.emplace_back(name);
        return ResolveFromMatchingModule(name);
    };
    EXPECT_TRUE(loader_test::ResolveMediaEngineExports(nullptr, resolve));
    ASSERT_FALSE(names.empty());
    EXPECT_EQ(names.front(), bid::kMediaEngineExportName) << "the identity is checked before anything else";
    for (const char* required : {"MediaEngine_SubmitFrameWithResultV1", "MediaEngine_SubmitFrameD3D11WithResultV1",
                                 "MediaEngine_RepeatLastFrameWithResultV1", "MediaEngine_GetMuxFlowSnapshotV1",
                                 "MediaEngine_TakeInjectSourceReadToken", "MediaEngine_IsInjectSourceReadComplete",
                                 "MediaEngine_SetInjectSourceReadWakeEvent", "MediaEngine_FinishInjectSourceReads",
                                 "MediaEngine_PrepareInjectVideo", "MediaEngine_GetLastOutputPath"})
        EXPECT_NE(std::find(names.begin(), names.end(), required), names.end());
    loader_test::MediaEngine_Unload();
    ExpectPointersCleared();
}

TEST(MediaEngineLoaderTest, EachMissingResultExportRejectsModuleAndClearsLegacyAndNewPointers) {
    for (const char* missing : {"MediaEngine_SubmitFrameWithResultV1", "MediaEngine_SubmitFrameD3D11WithResultV1",
                                "MediaEngine_RepeatLastFrameWithResultV1", "MediaEngine_SubmitFrame",
                                "MediaEngine_GetMuxFlowSnapshotV1", "MediaEngine_TakeInjectSourceReadToken",
                                "MediaEngine_IsInjectSourceReadComplete", "MediaEngine_FinishInjectSourceReads",
                                "MediaEngine_GetLastOutputPath"}) {
        SCOPED_TRACE(missing);
        UseMatchingModuleIdentity();
        auto resolve = [&](HMODULE, const char* name) {
            return strcmp(name, missing) == 0 ? nullptr : ResolveFromMatchingModule(name);
        };
        EXPECT_FALSE(loader_test::ResolveMediaEngineExports(nullptr, resolve));
        ExpectPointersCleared();
    }
}

// A mediaengine.dll of another build has other layouts and call semantics behind the same export names;
// it is refused before any export besides the identity one is resolved, and no pointer survives.
TEST(MediaEngineLoaderTest, ModuleOfAnotherBuildIsRefusedBeforeAnyOtherExportIsResolved) {
    struct Case {
        const char* what;
        void (*configure)();
    };
    const Case cases[] = {
        {"another build number", [] { g_moduleIdentity.buildNumber += 1; }},
        {"another version text", [] { g_moduleIdentity.version[0] = 'X'; }},
        {"another build timestamp", [] { g_moduleIdentity.timestamp[0] = 'X'; }},
        {"another record size", [] { g_moduleIdentity.size += 8; }},
        {"an identity export that rejects the request", [] { g_identityExportAccepts = false; }},
    };
    for (const Case& testCase : cases) {
        SCOPED_TRACE(testCase.what);
        UseMatchingModuleIdentity();
        testCase.configure();
        std::vector<std::string> names;
        auto resolve = [&](HMODULE, const char* name) {
            names.emplace_back(name);
            return ResolveFromMatchingModule(name);
        };
        EXPECT_FALSE(loader_test::ResolveMediaEngineExports(nullptr, resolve));
        ASSERT_EQ(names.size(), 1u) << "nothing but the identity export may be touched";
        EXPECT_EQ(names.front(), bid::kMediaEngineExportName);
        ExpectPointersCleared();
    }
}

TEST(MediaEngineLoaderTest, ModuleWithoutAnIdentityExportIsRefused) {
    std::vector<std::string> names;
    auto resolve = [&](HMODULE, const char* name) {
        names.emplace_back(name);
        return strcmp(name, bid::kMediaEngineExportName) == 0 ? nullptr : reinterpret_cast<FARPROC>(&ExportStub);
    };
    EXPECT_FALSE(loader_test::ResolveMediaEngineExports(nullptr, resolve));
    ASSERT_EQ(names.size(), 1u);
    ExpectPointersCleared();
}
