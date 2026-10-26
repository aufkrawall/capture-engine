#include <gtest/gtest.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3dcompiler.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "mediaengine/video/face_camera_format.h"
#include "mediaengine/video/face_camera_shader.h"
#include "source_fragment_reader.h"

namespace {

std::string ReadFaceCameraSource(const char* filename) {
    const std::filesystem::path path =
        ce::test_source::FindSource("mediaengine", filename);
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream contents;
    contents << stream.rdbuf();
    return contents.str();
}

std::string ReadProjectSource(const std::filesystem::path& relativePath) {
    std::ifstream stream(std::filesystem::current_path() / relativePath, std::ios::binary);
    std::ostringstream contents;
    contents << stream.rdbuf();
    return contents.str();
}

}  // namespace

TEST(FaceCameraSourceTest, RuntimeShaderCompilesForBothStages) {
    HMODULE compilerModule = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    ASSERT_NE(compilerModule, nullptr);
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
                                       LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    auto compile = reinterpret_cast<CompileFn>(GetProcAddress(compilerModule, "D3DCompile"));
    ASSERT_NE(compile, nullptr);

    struct Profile {
        const char* entry;
        const char* target;
    };
    const Profile profiles[] = {{"VS_Main", "vs_4_0"}, {"PS_Main", "ps_4_0"}};
    for (const Profile& profile : profiles) {
        ID3DBlob* blob = nullptr;
        ID3DBlob* errors = nullptr;
        const HRESULT hr = compile(ce::face_camera::kShaderSource,
                                   sizeof(ce::face_camera::kShaderSource) - 1, "face_camera", nullptr, nullptr,
                                   profile.entry, profile.target, 0, 0, &blob, &errors);
        const std::string diagnostic =
            errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "";
        if (errors)
            errors->Release();
        if (blob)
            blob->Release();
        EXPECT_TRUE(SUCCEEDED(hr)) << profile.entry << "/" << profile.target << ": " << diagnostic;
    }
    FreeLibrary(compilerModule);
}

TEST(FaceCameraSourceTest, CapturePublishesOnlyTheLatestFrameWithoutAQueue) {
    const std::string capture = ReadFaceCameraSource("face_camera_capture.cpp");
    const std::string header = ReadFaceCameraSource("face_camera_capture.h");
    ASSERT_FALSE(capture.empty());
    ASSERT_FALSE(header.empty());
    EXPECT_NE(capture.find("reader->ReadSample"), std::string::npos);
    EXPECT_NE(capture.find("std::atomic_store_explicit(&latestFrame_"), std::string::npos);
    EXPECT_NE(header.find("std::shared_ptr<const FaceCameraFrame> latestFrame_"), std::string::npos);
    EXPECT_EQ(capture.find("Sleep("), std::string::npos);
    EXPECT_EQ(capture.find("WaitForSingleObject"), std::string::npos);
}

TEST(FaceCameraSourceTest, SourceReaderUsesGpuCompatibleProcessingWithOptionalCpuFallback) {
    const std::string capture = ReadFaceCameraSource("face_camera_capture.cpp");
    ASSERT_FALSE(capture.empty());
    EXPECT_NE(capture.find("readerAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, FALSE)"),
              std::string::npos);
    EXPECT_EQ(capture.find("readerAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE)"),
              std::string::npos);
    EXPECT_NE(capture.find("CandidateOutputSubtypes(mode.subtype)"), std::string::npos);
    EXPECT_NE(capture.find("[FaceCamera] Native YUV path: subtype=%s matrix=%s range=%s gpu=%d"), std::string::npos);
    EXPECT_NE(capture.find("MF_SOURCE_READER_D3D_MANAGER"), std::string::npos);
    EXPECT_NE(capture.find("MF_READWRITE_D3D_OPTIONAL"), std::string::npos);
    EXPECT_EQ(capture.find("MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING"), std::string::npos);
    EXPECT_NE(capture.find("immediateContext->QueryInterface"), std::string::npos);
}

TEST(FaceCameraSourceTest, CandidateOutputSubtypesPreferNativeUncompressedOrNv12BeforeRgb32) {
    using namespace ce::face_camera;

    const auto yuy2Candidates = CandidateOutputSubtypes(MFVideoFormat_YUY2);
    ASSERT_GE(yuy2Candidates.size(), 3u);
    EXPECT_EQ(yuy2Candidates[0], MFVideoFormat_YUY2);
    EXPECT_EQ(yuy2Candidates[1], MFVideoFormat_NV12);

    const auto rgbCandidates = CandidateOutputSubtypes(MFVideoFormat_RGB32);
    ASSERT_GE(rgbCandidates.size(), 3u);
    EXPECT_EQ(rgbCandidates[0], MFVideoFormat_RGB32);

    const auto mjpgCandidates = CandidateOutputSubtypes(MFVideoFormat_MJPG);
    ASSERT_GE(mjpgCandidates.size(), 3u);
    EXPECT_EQ(mjpgCandidates[0], MFVideoFormat_NV12);
    EXPECT_EQ(mjpgCandidates[1], MFVideoFormat_YUY2);
    EXPECT_EQ(mjpgCandidates[2], MFVideoFormat_RGB32);
}

TEST(FaceCameraSourceTest, YuvMatrixAndRangeDefaultsFollowMediaFoundationConventions) {
    using namespace ce::face_camera;

    EXPECT_EQ(ResolveYuvMatrix(MFVideoTransferMatrix_BT709, 640, 480), YuvMatrix::kBt709);
    EXPECT_EQ(ResolveYuvMatrix(MFVideoTransferMatrix_BT601, 1920, 1080), YuvMatrix::kBt601);
    EXPECT_EQ(ResolveYuvMatrix(4u, 1920, 1080), YuvMatrix::kBt2020);
    EXPECT_EQ(ResolveYuvMatrix(0u, 1280, 720), YuvMatrix::kBt709);
    EXPECT_EQ(ResolveYuvMatrix(0u, 640, 480), YuvMatrix::kBt601);

    EXPECT_EQ(ResolveYuvRange(0u), YuvRange::kLimited);
    EXPECT_EQ(ResolveYuvRange(MFNominalRange_16_235), YuvRange::kLimited);
    EXPECT_EQ(ResolveYuvRange(MFNominalRange_0_255), YuvRange::kFull);
}

TEST(FaceCameraSourceTest, ExtractCpuPlanesHandlesNv12Yuy2AndBottomUpBgra) {
    using namespace ce::face_camera;

    // 4x2 NV12 with stride=8 (4 active bytes + 4 padding bytes per row).
    std::vector<uint8_t> nv12Raw(8u * 3u, 0xCCu);
    for (uint32_t x = 0; x < 4u; ++x) {
        nv12Raw[0 * 8 + x] = static_cast<uint8_t>(10 + x);
        nv12Raw[1 * 8 + x] = static_cast<uint8_t>(20 + x);
        nv12Raw[2 * 8 + x] = static_cast<uint8_t>(100 + x);
    }
    std::vector<uint8_t> primary;
    std::vector<uint8_t> chroma;
    ASSERT_TRUE(ExtractCpuPlanes(PixelFormat::kNv12, nv12Raw.data(), nv12Raw.size(), 4, 2, 8, &primary, &chroma));
    ASSERT_EQ(primary.size(), 8u);
    ASSERT_EQ(chroma.size(), 4u);
    EXPECT_EQ(primary[0], 10u);
    EXPECT_EQ(primary[4], 20u);
    EXPECT_EQ(chroma[0], 100u);
    EXPECT_EQ(chroma[3], 103u);

    // 2x2 bottom-up BGRA8 (negative stride = -8).
    const uint8_t bgraBottomUp[16] = {
        1, 2, 3, 4,   5, 6, 7, 8,     // bottom row (row 1 in top-down)
        9, 10, 11, 12, 13, 14, 15, 16 // top row (row 0 in top-down)
    };
    ASSERT_TRUE(ExtractCpuPlanes(PixelFormat::kBgra8, bgraBottomUp, sizeof(bgraBottomUp), 2, 2, -8, &primary, &chroma));
    ASSERT_EQ(primary.size(), 16u);
    EXPECT_TRUE(chroma.empty());
    EXPECT_EQ(primary[0], 9u);
    EXPECT_EQ(primary[8], 1u);
}

TEST(FaceCameraSourceTest, StopCancelsPendingReadBeforeWorkerOwnedSourceShutdown) {
    const std::string capture = ReadFaceCameraSource("face_camera_capture.cpp");
    const std::string header = ReadFaceCameraSource("face_camera_capture.h");
    ASSERT_FALSE(capture.empty());
    ASSERT_FALSE(header.empty());

    const size_t stopBegin = capture.find("void FaceCameraCapture::Stop() {");
    const size_t stopEnd = capture.find("std::shared_ptr<const FaceCameraFrame> FaceCameraCapture::LatestFrame()",
                                        stopBegin);
    ASSERT_NE(stopBegin, std::string::npos);
    ASSERT_NE(stopEnd, std::string::npos);
    const std::string stop = capture.substr(stopBegin, stopEnd - stopBegin);
    const size_t flush = stop.find("reader->Flush(MF_SOURCE_READER_FIRST_VIDEO_STREAM)");
    const size_t join = stop.find("captureThread_.join()");
    ASSERT_NE(flush, std::string::npos);
    ASSERT_NE(join, std::string::npos);
    EXPECT_LT(flush, join);
    EXPECT_NE(stop.find("Keep publication locked through the synchronous flush"), std::string::npos);
    EXPECT_EQ(stop.find("source->Shutdown()"), std::string::npos);
    EXPECT_EQ(header.find("activeSource_"), std::string::npos);

    const size_t workerBegin = capture.find("void FaceCameraCapture::CaptureThreadMain() {");
    ASSERT_NE(workerBegin, std::string::npos);
    EXPECT_NE(capture.find("source->Shutdown()", workerBegin), std::string::npos);
}

TEST(FaceCameraSourceTest, RendererTransfersOnlyNewCameraFramesAndUsesOneGpuDraw) {
    const std::string renderer = ReadFaceCameraSource("face_camera_renderer.cpp");
    ASSERT_FALSE(renderer.empty());
    EXPECT_NE(renderer.find("frame->sequence == uploadedSequence_"), std::string::npos);
    // Mips once per uploaded camera frame, into that frame's history slot.
    EXPECT_NE(renderer.find("context_->GenerateMips(slot.srv)"), std::string::npos);
    EXPECT_NE(renderer.find("context_->Draw(4, 0)"), std::string::npos);
    EXPECT_NE(renderer.find("stale_timeout_ms=%u"), std::string::npos);
    EXPECT_NE(renderer.find("camera-free successor frame"), std::string::npos);
    EXPECT_EQ(renderer.find("Map(cameraTexture_"), std::string::npos);
}

TEST(FaceCameraSourceTest, SessionStartPrewarmsRendererBeforeTheFirstVideoFrame) {
    const std::string integration = ReadFaceCameraSource("video_encoder_face_camera.cpp");
    const std::string renderer = ReadFaceCameraSource("face_camera_renderer.cpp");
    const std::string start = ReadFaceCameraSource("video_encoder_start.cpp");
    ASSERT_FALSE(integration.empty());
    ASSERT_FALSE(renderer.empty());
    ASSERT_FALSE(start.empty());

    const size_t captureStart = integration.find("faceCameraRenderer->StartCapture(d3d11Device)");
    const size_t rendererPrewarm =
        integration.find("faceCameraRenderer->InitRenderer(d3d11Device, d3d11Context)");
    ASSERT_NE(captureStart, std::string::npos);
    ASSERT_NE(rendererPrewarm, std::string::npos);
    EXPECT_LT(captureStart, rendererPrewarm);
    EXPECT_NE(renderer.find("std::call_once(compileOnce"), std::string::npos);
    EXPECT_NE(renderer.find("D3DCOMPILE_OPTIMIZATION_LEVEL3"), std::string::npos);
    EXPECT_NE(renderer.find("(void)GetFaceCameraShaderBytecode();"), std::string::npos);

    const size_t beginDeferred = start.find("BeginDeferredRecording();");
    const size_t ensureCamera = start.find("EnsureFaceCameraStarted();");
    ASSERT_NE(beginDeferred, std::string::npos);
    ASSERT_NE(ensureCamera, std::string::npos);
    EXPECT_LT(beginDeferred, ensureCamera);
}

TEST(FaceCameraSourceTest, CameraCompositesBeforeCursorAndColorConversion) {
    const std::string conversion = ReadFaceCameraSource("video_encoder_convert_bgra.cpp");
    const std::string direct = ReadFaceCameraSource("video_encoder_textures.cpp");
    ASSERT_FALSE(conversion.empty());
    ASSERT_FALSE(direct.empty());

    const size_t cameraPrepare = conversion.find("PrepareVideoProcessorFaceCameraInput");
    const size_t cursorPrepare = conversion.find("PrepareVideoProcessorCursorInput");
    const size_t processorBlt = conversion.find("VideoProcessorBlt");
    ASSERT_NE(cameraPrepare, std::string::npos);
    ASSERT_NE(cursorPrepare, std::string::npos);
    ASSERT_NE(processorBlt, std::string::npos);
    EXPECT_LT(cameraPrepare, cursorPrepare);
    EXPECT_LT(cursorPrepare, processorBlt);

    const size_t directCamera = direct.find("CompositeFaceCameraOntoRgb");
    const size_t directCursor = direct.find("cursorRenderer->CompositeOntoFrame");
    ASSERT_NE(directCamera, std::string::npos);
    ASSERT_NE(directCursor, std::string::npos);
    EXPECT_LT(directCamera, directCursor);
}

TEST(FaceCameraSourceTest, CfrRepeatSourceIsCommittedOnlyAfterAcceptedInjectFrame) {
    const std::string encode = ReadFaceCameraSource("video_encoder_encode.cpp");
    ASSERT_FALSE(encode.empty());
    const size_t stage = encode.find("StageRepeatSourceFrameTexture");
    const size_t submit = encode.find("SubmitFrameForEncode");
    const size_t commit = encode.find("CommitStagedRepeatSourceFrameTexture");
    ASSERT_NE(stage, std::string::npos);
    ASSERT_NE(submit, std::string::npos);
    ASSERT_NE(commit, std::string::npos);
    EXPECT_LT(stage, submit);
    EXPECT_LT(submit, commit);
    EXPECT_NE(encode.find("DiscardStagedRepeatSourceFrameTexture"), std::string::npos);
    EXPECT_NE(encode.find("if (!repeatFrameTexture)"), std::string::npos);
    EXPECT_NE(encode.find("otherwise it would linger until a later CFR"), std::string::npos);

    const std::string integration = ReadFaceCameraSource("video_encoder_face_camera.cpp");
    const std::string repeat = ReadFaceCameraSource("video_encoder_framegrab.cpp");
    const std::string repeatTextures = ReadFaceCameraSource("video_encoder_textures.cpp");
    ASSERT_FALSE(integration.empty());
    ASSERT_FALSE(repeat.empty());
    ASSERT_FALSE(repeatTextures.empty());
    EXPECT_NE(integration.find("NeedsDynamicRepeatSource"), std::string::npos);
    EXPECT_NE(repeat.find("if (!DynamicOverlayRecompositionActive())"), std::string::npos);
    EXPECT_NE(repeat.find("freezeDynamicOverlaysForRepeat"), std::string::npos);
    EXPECT_NE(repeat.find("UpdateCfrDynamicOverlayRepeatState"), std::string::npos);
    EXPECT_NE(repeat.find("CacheRepeatFrameTexture(acceptedConvertedFrame)"), std::string::npos);
    EXPECT_NE(repeat.find("temporal regression"), std::string::npos);
    EXPECT_NE(encode.find("ObserveFreshFrameDynamicOverlayPressure"), std::string::npos);
    EXPECT_NE(repeatTextures.find("small transactional restores"), std::string::npos);
}

TEST(FaceCameraSourceTest, ConfigReloadComparisonIncludesCameraSettings) {
    const std::string reload =
        ReadProjectSource(std::filesystem::path("captureengine") / "media" / "media_main_recording.cpp");
    ASSERT_FALSE(reload.empty());
    EXPECT_NE(reload.find("lhs.faceCamera == rhs.faceCamera"), std::string::npos);
}
