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
        return reinterpret_cast<FARPROC>(reinterpret_cast<uintptr_t>(&FakeIdentityExport));  // GetProcAddress-style
    return reinterpret_cast<FARPROC>(&ExportStub);
}

void UseMatchingModuleIdentity() {
    g_moduleIdentity = bid::Current();
    g_identityExportAccepts = true;
}

void ExpectPointersCleared() {
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_SubmitFrameWithResultV1, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_SubmitFrameD3D11WithResultV1, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_RepeatLastFrameWithResultV1, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_SetLogCallback, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_Init, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_ReloadConfig, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_SetActiveScreenGrab, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_SetWgcStartupExtraDelayQpc, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_CanRepeatLastFrame, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_ResetRepeatFrameCache, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_PrepareFrameD3D11, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_PrepareInjectVideo, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_StartRecording, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_StopRecording, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_GetLastOutputDegradedFlags, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_GetLastOutputPath, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_ReleaseEncoderTextures, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_GetD3D11Device, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_ReleaseSharedD3D11Device, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_CreateSharedCaptureTextures, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_GetLastFrameEncodeTimeUs, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_GetLastFrameFenceWaitUs, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_GetMuxFlowSnapshotV1, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_QueryInjectFrameCopyCompletion, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_SetInjectTransportGeneration, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_TakeInjectSourceReadToken, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_IsInjectSourceReadComplete, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_SetInjectSourceReadWakeEvent, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_FinishInjectSourceReads, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_Create, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_Destroy, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_SetSharedMem, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_LockD3D11, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_UnlockD3D11, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_SetAudioOnly, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_SetSourcePrefers10Bit, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_SetCursorCompositionSuppressed, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_MeasureRenderEndpointLatency, nullptr);
    EXPECT_EQ(ce::mediaengine_api::MediaEngine_RecoverRecording, nullptr);
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
    // MediaEngine_Load leaves one reference on a module it resolved; the Unload below drops it.
    loader_test::g_MediaEngineReferences = 1;
    loader_test::MediaEngine_Unload();
    ExpectPointersCleared();
}

TEST(MediaEngineLoaderTest, EachMissingResultExportRejectsModuleAndClearsLegacyAndNewPointers) {
    for (const char* missing : {"MediaEngine_SubmitFrameWithResultV1", "MediaEngine_SubmitFrameD3D11WithResultV1",
                                "MediaEngine_RepeatLastFrameWithResultV1", "MediaEngine_CanRepeatLastFrame",
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

// Two pipelines of one process share the module: it is freed by the Unload that drops the last reference, and an
// unbalanced Unload changes nothing. A real module stands in for mediaengine.dll (LoadLibrary counts it, FreeLibrary
// releases it), so only the reference bookkeeping and the pointer clearing are under test.
TEST(MediaEngineLoaderTest, ModuleIsFreedByTheLastUnloadOnly) {
    HMODULE module = LoadLibraryW(L"version.dll");
    ASSERT_NE(module, nullptr);
    loader_test::g_MediaEngineModule = module;
    loader_test::g_MediaEngineReferences = 2;
    ce::mediaengine_api::MediaEngine_Init = reinterpret_cast<MediaEngine_Init_t>(&ExportStub);

    loader_test::MediaEngine_Unload();  // one pipeline's session ends; the other still uses the module
    EXPECT_EQ(loader_test::g_MediaEngineModule, module);
    EXPECT_EQ(loader_test::g_MediaEngineReferences, 1);
    EXPECT_NE(ce::mediaengine_api::MediaEngine_Init, nullptr);

    loader_test::MediaEngine_Unload();  // the last reference goes
    EXPECT_EQ(loader_test::g_MediaEngineModule, nullptr);
    EXPECT_EQ(loader_test::g_MediaEngineReferences, 0);
    ExpectPointersCleared();
}

TEST(MediaEngineLoaderTest, UnbalancedUnloadLeavesTheModuleInPlace) {
    HMODULE module = LoadLibraryW(L"version.dll");
    ASSERT_NE(module, nullptr);
    loader_test::g_MediaEngineModule = module;
    loader_test::g_MediaEngineReferences = 0;
    loader_test::MediaEngine_Unload();
    EXPECT_EQ(loader_test::g_MediaEngineModule, module);
    EXPECT_EQ(loader_test::g_MediaEngineReferences, 0);
    // Give back the LoadLibrary reference this test took.
    loader_test::g_MediaEngineModule = nullptr;
    FreeLibrary(module);
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

// The load-scoped mode (an in-process role) of a package without mediaengine.dll fails before it takes a reference, so
// the Unload of its session drops nothing that belongs to another user of the module.
TEST(MediaEngineLoaderTest, LoadScopedFromAPackageWithoutTheModuleTakesNoReference) {
    const std::filesystem::path missing = std::filesystem::temp_directory_path() / "ce_loader_missing_package";
    EXPECT_FALSE(loader_test::MediaEngine_Load(missing, MediaEngineDllSearch::LoadScoped));
    ExpectPointersCleared();
    loader_test::MediaEngine_Unload();  // unbalanced: a no-op
    ExpectPointersCleared();
}
