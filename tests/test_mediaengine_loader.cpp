#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "captureengine/app/mediaengine_loader.h"
#include "common/logging/logging.h"
#include "common/platform/secure_dll_loading.h"

namespace loader_test {
void MediaEngine_Unload();
// NOLINTNEXTLINE(bugprone-suspicious-include) - exercise the actual private loader orchestration
#include "captureengine/app/mediaengine_loader.cpp"
}  // namespace loader_test

namespace {
INT_PTR WINAPI ExportStub() { return 0; }
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
    EXPECT_EQ(loader_test::MediaEngine_ProcessFrameD3D11.raw, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_StartRecording, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_StopRecording, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_GetLastOutputDegradedFlags, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_ReleaseEncoderTextures, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_GetD3D11Device, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_ReleaseSharedD3D11Device, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_CreateSharedCaptureTextures, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_GetLastFrameEncodeTimeUs, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_GetLastFrameFenceWaitUs, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_WasLastFrameDeferred, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_QueryInjectFrameCopyCompletion, nullptr);
    EXPECT_EQ(loader_test::MediaEngine_SetInjectTransportGeneration, nullptr);
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
    std::vector<std::string> names;
    auto resolve = [&](HMODULE, const char* name) {
        names.emplace_back(name);
        return reinterpret_cast<FARPROC>(&ExportStub);
    };
    EXPECT_TRUE(loader_test::ResolveMediaEngineExports(nullptr, resolve));
    for (const char* required : {"MediaEngine_SubmitFrameWithResultV1", "MediaEngine_SubmitFrameD3D11WithResultV1",
                                 "MediaEngine_RepeatLastFrameWithResultV1"})
        EXPECT_NE(std::find(names.begin(), names.end(), required), names.end());
    loader_test::MediaEngine_Unload();
    ExpectPointersCleared();
}

TEST(MediaEngineLoaderTest, EachMissingResultExportRejectsModuleAndClearsLegacyAndNewPointers) {
    for (const char* missing : {"MediaEngine_SubmitFrameWithResultV1", "MediaEngine_SubmitFrameD3D11WithResultV1",
                                "MediaEngine_RepeatLastFrameWithResultV1", "MediaEngine_SubmitFrame"}) {
        SCOPED_TRACE(missing);
        auto resolve = [&](HMODULE, const char* name) {
            return strcmp(name, missing) == 0 ? nullptr : reinterpret_cast<FARPROC>(&ExportStub);
        };
        EXPECT_FALSE(loader_test::ResolveMediaEngineExports(nullptr, resolve));
        ExpectPointersCleared();
    }
}
