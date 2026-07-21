#include <gtest/gtest.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <mmsystem.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "../common/raii_helpers.h"
#include "../mediaengine/video_color_conversion_shader.h"

namespace {

std::string ReadVideoEncoderSource() {
    const std::filesystem::path source = std::filesystem::current_path() / "mediaengine" / "video_encoder.cpp";
    std::ifstream file(source, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

std::string ReadCursorRendererSource() {
    const std::filesystem::path source = std::filesystem::current_path() / "mediaengine" / "cursor_renderer.cpp";
    std::ifstream file(source, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

std::string ReadVideoColorShaderSource() {
    const std::filesystem::path source =
        std::filesystem::current_path() / "mediaengine" / "video_color_conversion_shader.h";
    std::ifstream file(source, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

std::string ReadVideoMetadataSource() {
    const std::filesystem::path source = std::filesystem::current_path() / "mediaengine" / "video_metadata.cpp";
    std::ifstream file(source, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

size_t CountOccurrences(const std::string& source, const std::string& needle) {
    size_t count = 0;
    size_t position = 0;
    while ((position = source.find(needle, position)) != std::string::npos) {
        count++;
        position += needle.size();
    }
    return count;
}

}  // namespace

TEST(VideoEncoderSourceTest, CursorShaderCompilesAndCompilerModuleOutlivesReturnedBlobs) {
    const std::string source = ReadCursorRendererSource();
    ASSERT_FALSE(source.empty());

    EXPECT_EQ(source.find("float3 linear ="), std::string::npos);
    EXPECT_NE(source.find("float3 linearRgb ="), std::string::npos);

    const size_t moduleGuard = source.find("ce::ModuleGuard d3dCompiler");
    const size_t vertexBlob = source.find("ce::ComGuard<ID3DBlob> vsBlob", moduleGuard);
    ASSERT_NE(moduleGuard, std::string::npos);
    ASSERT_NE(vertexBlob, std::string::npos);
    EXPECT_LT(moduleGuard, vertexBlob);

    const size_t createResources = source.find("bool CursorRenderer::CreateRenderingResources()");
    ASSERT_NE(createResources, std::string::npos);
    EXPECT_EQ(source.find("FreeLibrary(d3dCompiler)", createResources), std::string::npos);
}

TEST(VideoEncoderSourceTest, RgbConversionCompilerModuleOutlivesReturnedBlobs) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    const size_t function = source.find("bool VideoEncoder::EnsureSwapRBShader()");
    const size_t functionEnd = source.find("ID3D11Texture2D* VideoEncoder::RenderFullscreenCopy", function);
    ASSERT_NE(function, std::string::npos);
    ASSERT_NE(functionEnd, std::string::npos);
    const std::string body = source.substr(function, functionEnd - function);

    const size_t moduleGuard = body.find("ce::ModuleGuard d3dCompiler");
    const size_t vertexBlob = body.find("ce::ComGuard<ID3DBlob> vsBlob");
    const size_t createVertexShader = body.find("CreateVertexShader(vsBlob->GetBufferPointer()");
    ASSERT_NE(moduleGuard, std::string::npos);
    ASSERT_NE(vertexBlob, std::string::npos);
    ASSERT_NE(createVertexShader, std::string::npos);
    EXPECT_LT(moduleGuard, vertexBlob);
    EXPECT_LT(vertexBlob, createVertexShader);
    EXPECT_EQ(body.find("FreeLibrary(d3dCompiler)"), std::string::npos);
    EXPECT_NE(body.find("GetProcAddress(d3dCompiler.get(), \"D3DCompile\")"), std::string::npos);
}

TEST(VideoEncoderSourceTest, PerformanceLogsDoNotMislabelCpuSubmissionAsGpuDuration) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("[PERF] Frame %d:"), std::string::npos);
    EXPECT_NE(source.find("[PERF SUMMARY] Frames=%lld"), std::string::npos);
    EXPECT_NE(source.find("[Framegrab PERF] Frame %d:"), std::string::npos);
    EXPECT_NE(source.find("timing=cpu-wall-or-submit"), std::string::npos);
    EXPECT_EQ(source.find("timing=gpu"), std::string::npos);
}

TEST(VideoEncoderSourceTest, RealTimeGpuPathsNeverUseSleepRetries) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    EXPECT_EQ(source.find("Sleep(2)"), std::string::npos);
    EXPECT_NE(source.find("if (FAILED(hr) && hasSharedAlt)"), std::string::npos);
    EXPECT_NE(source.find("dupTexAlt.addressof()"), std::string::npos);
    EXPECT_EQ(source.find("Sleep(1)"), std::string::npos);
    EXPECT_NE(source.find("lastFrameDeferred.store(true"), std::string::npos);
    EXPECT_NE(source.find("Frame-ring publication uses release/acquire ordering"), std::string::npos);
}

TEST(VideoEncoderSourceTest, PostMuxProbeNeverOutlivesMediaEngineCode) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    EXPECT_EQ(source.find("std::thread probeThread"), std::string::npos);
    EXPECT_EQ(source.find("probeFuture.wait_for"), std::string::npos);
    EXPECT_NE(source.find("probeCtx->interrupt_callback.callback = PostMuxProbeInterrupt"), std::string::npos);
    EXPECT_NE(source.find("kPostMuxProbeMaxPackets"), std::string::npos);
    EXPECT_NE(source.find("Run on the already-owned writer/finalizer thread"), std::string::npos);
}

TEST(VideoEncoderSourceTest, EveryCfrSubmissionAdvancesOneContiguousPacketTick) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    const size_t function = source.find("int64_t ComputeTargetVideoPts");
    const size_t nextFunction = source.find("bool IsConfiguredNvencLookaheadActive", function);
    ASSERT_NE(function, std::string::npos);
    ASSERT_NE(nextFunction, std::string::npos);
    const std::string body = source.substr(function, nextFunction - function);
    EXPECT_NE(body.find("if (useExplicitCfrTimeline)"), std::string::npos);
    EXPECT_EQ(body.find("ComputeCfrFrameIndexForElapsedUs"), std::string::npos);
    const std::string contiguousCall = "ComputeNextCfrFrameIndex(lastAssignedVideoPts)";
    const size_t firstCall = body.find(contiguousCall);
    const size_t secondCall = body.find(contiguousCall, firstCall + 1);
    ASSERT_NE(firstCall, std::string::npos);
    ASSERT_NE(secondCall, std::string::npos);
    EXPECT_EQ(body.find(contiguousCall, secondCall + 1), std::string::npos);
}

TEST(VideoEncoderSourceTest, CaptureSourceTransitionDropsCachedVisualContent) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    const size_t reset = source.find("void VideoEncoder::ResetRepeatFrameCache()");
    ASSERT_NE(reset, std::string::npos);
    const size_t end = source.find("bool VideoEncoder::WasLastFrameDeferred", reset);
    ASSERT_NE(end, std::string::npos);
    const std::string body = source.substr(reset, end - reset);
    EXPECT_NE(body.find("repeatFrameTexture->Release()"), std::string::npos);
    EXPECT_NE(body.find("InvalidateRepeatSourceFrameTexture()"), std::string::npos);
    EXPECT_EQ(source.find("cachedRepeatPacket_"), std::string::npos);
    EXPECT_EQ(source.find("av_packet_ref(repeatPkt"), std::string::npos);
}

TEST(VideoEncoderSourceTest, Av1NvencSafetyOptionsOverrideCustomOptionsBeforeCodecOpen) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    const size_t configure = source.find("bool VideoEncoder::ConfigureAndOpenCodec()");
    const size_t customApply = source.find("for (const auto& option : optionPlan.customOptions)", configure);
    const size_t requiredApply = source.find("for (const auto& option : optionPlan.requiredOptions)", customApply);
    const size_t codecOpen = source.find("avcodec_open2(codecCtx, codec, &opts)", requiredApply);
    ASSERT_NE(customApply, std::string::npos);
    ASSERT_NE(requiredApply, std::string::npos);
    ASSERT_NE(codecOpen, std::string::npos);
    EXPECT_LT(customApply, requiredApply);
    EXPECT_LT(requiredApply, codecOpen);
    EXPECT_EQ(source.find("repeat_pps"), std::string::npos);
}

TEST(VideoEncoderSourceTest, VideoProcessorOutputsAreOwnedByFfmpegHardwareFrames) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("d11FramesHw->BindFlags |= D3D11_BIND_RENDER_TARGET"), std::string::npos);
    EXPECT_NE(source.find("av_hwframe_get_buffer(d3d11FramesCtx, outputFrame, 0)"), std::string::npos);
    EXPECT_NE(source.find("VideoProcessorBlt(videoProcessor, outputView, 0, 1, &stream)"), std::string::npos);
    EXPECT_NE(source.find("outputViewCache.push_back({outputTexture, outputArraySlice, outputView})"),
              std::string::npos);
    EXPECT_EQ(source.find("nv12StagingTextures"), std::string::npos);
    EXPECT_EQ(source.find("av_buffer_create(reinterpret_cast<uint8_t*>(nv12Tex)"), std::string::npos);
}

TEST(VideoEncoderSourceTest, QuickSyncDerivesOneVplSurfacesDirectlyFromCaptureD3D11Frames) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("av_hwdevice_ctx_create_derived(&hwDeviceCtx, AV_HWDEVICE_TYPE_QSV, d3d11DeviceCtx"),
              std::string::npos);
    EXPECT_NE(source.find("av_hwframe_ctx_create_derived(&hwFramesCtx, AV_PIX_FMT_QSV, hwDeviceCtx, d3d11FramesCtx"),
              std::string::npos);
    EXPECT_NE(source.find("av_hwframe_map(qsvFrame, d3d11Frame, AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT)"),
              std::string::npos);
    EXPECT_NE(source.find("resolved.codecPixFmt = UsesQsvHardwareFrames(config.encoder) ? AV_PIX_FMT_QSV"),
              std::string::npos);
    EXPECT_GE(CountOccurrences(source, "PrepareEncoderInputFrame(d3d11Frame)"), 3u);
    EXPECT_EQ(source.find("av_hwframe_transfer_data"), std::string::npos);
    EXPECT_NE(source.find("no CPU transfer"), std::string::npos);
}

TEST(VideoEncoderSourceTest, HdrFramesCarryBackendNeutralStaticMetadata) {
    const std::string source = ReadVideoEncoderSource();
    const std::string metadata = ReadVideoMetadataSource();
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(metadata.empty());

    const size_t configureMetadata = source.find("AddNominalHdrMetadataToCodecContext");
    const size_t openCodec = source.find("avcodec_open2(codecCtx", configureMetadata);
    ASSERT_NE(configureMetadata, std::string::npos);
    ASSERT_NE(openCodec, std::string::npos);
    EXPECT_LT(configureMetadata, openCodec);
    EXPECT_NE(source.find("AddNominalHdrMetadataToCodecParameters"), std::string::npos);
    EXPECT_NE(source.find("NormalizeHdrCodecExtradata"), std::string::npos);
    EXPECT_NE(source.find("NormalizeHdrPacketMetadata"), std::string::npos);
    EXPECT_NE(source.find("NormalizeHdrPacketIfNeeded"), std::string::npos);
    EXPECT_NE(metadata.find("av_packet_get_side_data(packet, AV_PKT_DATA_NEW_EXTRADATA"), std::string::npos);
    EXPECT_NE(metadata.find("av_mastering_display_metadata_create_side_data(frame)"), std::string::npos);
    EXPECT_NE(metadata.find("av_content_light_metadata_create_side_data(frame)"), std::string::npos);
    EXPECT_NE(metadata.find("mastering->display_primaries[0][0] = av_make_q(17, 25)"), std::string::npos);
    EXPECT_NE(metadata.find("mastering->has_primaries = 1"), std::string::npos);
    EXPECT_NE(metadata.find("mastering->has_luminance = 1"), std::string::npos);
    EXPECT_NE(metadata.find("light->MaxCLL = nominalPeakNits"), std::string::npos);
    EXPECT_NE(metadata.find("light->MaxFALL = nominalPeakNits"), std::string::npos);
    EXPECT_NE(metadata.find("AV_PKT_DATA_MASTERING_DISPLAY_METADATA"), std::string::npos);
    EXPECT_NE(metadata.find("AV_PKT_DATA_CONTENT_LIGHT_LEVEL"), std::string::npos);
    EXPECT_NE(source.find("av_frame_copy_props(qsvFrame, d3d11Frame)"), std::string::npos);
}

TEST(VideoEncoderSourceTest, MetadataDurationDiagnosticsRunAfterTrailerAndIgnoreUnavailableFields) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    const size_t syncFinalize = source.find("[VideoEncoder] Sync Stop: Finalizing file...");
    const size_t syncTrailer = source.find("av_write_trailer(fmtCtx)", syncFinalize);
    const size_t syncSummary = source.find("LogFinalDurationSummary(fmtCtx", syncTrailer);
    ASSERT_NE(syncFinalize, std::string::npos);
    ASSERT_NE(syncTrailer, std::string::npos);
    ASSERT_NE(syncSummary, std::string::npos);
    EXPECT_LT(syncTrailer, syncSummary);

    const size_t asyncFinalize = source.find("[VideoEncoder] Async Finalize: Writing Trailer...");
    const size_t asyncTrailer = source.find("av_write_trailer(fmtCtx)", asyncFinalize);
    const size_t asyncSummary = source.find("LogFinalDurationSummary(fmtCtx", asyncTrailer);
    ASSERT_NE(asyncFinalize, std::string::npos);
    ASSERT_NE(asyncTrailer, std::string::npos);
    ASSERT_NE(asyncSummary, std::string::npos);
    EXPECT_LT(asyncTrailer, asyncSummary);

    EXPECT_NE(source.find("const bool metadataComplete"), std::string::npos);
    EXPECT_NE(source.find("Final in-memory AVStream durations are incomplete"), std::string::npos);
}

TEST(VideoEncoderSourceTest, VideoProcessorUsesOneDeterministicPrecompositedRgbStream) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("VideoProcessorSetStreamFrameFormat(videoProcessor, 0, "
                          "D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE)"),
              std::string::npos);
    EXPECT_NE(source.find("VideoProcessorSetStreamAutoProcessingMode(videoProcessor, 0, FALSE)"), std::string::npos);
    EXPECT_NE(source.find("[VideoProcessor] Deterministic single-stream processing"), std::string::npos);
    EXPECT_EQ(source.find("VideoProcessorSetStreamFrameFormat(videoProcessor, 1"), std::string::npos);
    EXPECT_EQ(source.find("VideoProcessorSetStreamAutoProcessingMode(videoProcessor, 1"), std::string::npos);
    EXPECT_EQ(source.find("VideoProcessorSetStreamAlpha"), std::string::npos);
    EXPECT_EQ(source.find("streams[1]"), std::string::npos);
}

TEST(VideoEncoderSourceTest, CursorPrecompositionBacksUpOnlyTouchedRgbRegionOnNormalCaptureSurfaces) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("PrepareVideoProcessorCursorInput(bgraTexture, overlayCursor"), std::string::npos);
    EXPECT_NE(source.find("(sourceDesc.BindFlags & D3D11_BIND_RENDER_TARGET) != 0"), std::string::npos);
    EXPECT_NE(source.find("CopySubresourceRegion(cursorRestoreTexture, 0, 0, 0, 0, source"), std::string::npos);
    EXPECT_NE(source.find("CopySubresourceRegion(target, 0, destinationX, destinationY, 0, backup"), std::string::npos);
    EXPECT_NE(source.find("Point RGB precomposition before VP active"), std::string::npos);
    EXPECT_NE(source.find("VideoProcessorBlt(videoProcessor, outputView, 0, 1, &stream)"), std::string::npos);
}

TEST(VideoEncoderSourceTest, CursorPrecompositionFailureNeverFailsVideoConversion) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    const size_t prepare = source.find("bool VideoEncoder::PrepareVideoProcessorCursorInput");
    ASSERT_NE(prepare, std::string::npos);
    const size_t preparedSource = source.find("*preparedSource = source;", prepare);
    ASSERT_NE(preparedSource, std::string::npos);
    const size_t convert = source.find("bool VideoEncoder::ConvertBGRAtoNV12", preparedSource);
    ASSERT_NE(convert, std::string::npos);
    const std::string cursorBody = source.substr(preparedSource, convert - preparedSource);

    EXPECT_EQ(cursorBody.find("return false;"), std::string::npos);
    EXPECT_NE(cursorBody.find("video conversion continues without this"), std::string::npos);
}

TEST(VideoEncoderSourceTest, HdrScRgbIsConvertedDirectlyToDeterministicP010) {
    const std::string source = ReadVideoEncoderSource();
    const std::string shader = ReadVideoColorShaderSource();
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(shader.empty());

    EXPECT_NE(shader.find("float3 ScRgbToHdr10(float3 scRgb)"), std::string::npos);
    EXPECT_NE(shader.find("rec2020.r * 80.0"), std::string::npos);
    EXPECT_NE(shader.find("LinearNitsToPQ"), std::string::npos);
    EXPECT_NE(shader.find("colorTransform == 2"), std::string::npos);
    EXPECT_NE(shader.find("float3 PqP2020ToYcbcr(float3 rgb)"), std::string::npos);
    EXPECT_NE(shader.find("64.0 + 876.0 * y"), std::string::npos);
    EXPECT_NE(shader.find("512.0 + 896.0 * cb"), std::string::npos);
    EXPECT_NE(shader.find("512.0 + 896.0 * cr"), std::string::npos);
    EXPECT_NE(shader.find("float4 PS_P010Y"), std::string::npos);
    EXPECT_NE(shader.find("float4 PS_P010UV"), std::string::npos);

    const size_t conversion = source.find("bool VideoEncoder::ConvertBGRAtoNV12");
    const size_t normalize = source.find("const bool normalizeHdrForOutput", conversion);
    const size_t directP010 = source.find("ConvertHdrRgb10ToP010(vpInputTexture", normalize);
    const size_t inputView = source.find("CreateVideoProcessorInputView(vpInputTexture", normalize);
    ASSERT_NE(conversion, std::string::npos);
    ASSERT_NE(normalize, std::string::npos);
    ASSERT_NE(directP010, std::string::npos);
    ASSERT_NE(inputView, std::string::npos);
    EXPECT_LT(normalize, inputView);
    EXPECT_LT(directP010, inputView);
    EXPECT_NE(source.find("driverVP=0 cpuWait=0"), std::string::npos);
    EXPECT_NE(source.find("refusing the driver VideoProcessor PQ fallback"), std::string::npos);
    EXPECT_EQ(source.find("HDR conversion requires ID3D11VideoContext1 color-space control"), std::string::npos);
}

TEST(VideoEncoderSourceTest, HdrP010UsesRec2100TopLeftChromaWithoutAnSdrPhaseShift) {
    const std::string source = ReadVideoEncoderSource();
    const std::string shader = ReadVideoColorShaderSource();
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(shader.empty());

    EXPECT_NE(source.find("outputIsHDR ? AVCHROMA_LOC_TOPLEFT : AVCHROMA_LOC_LEFT"), std::string::npos);
    EXPECT_NE(shader.find("float2 topLeftPhase = input.uv - outputInvSize"), std::string::npos);
    EXPECT_NE(shader.find("topLeftPhase + float2(outputInvSize.x, 0.0)"), std::string::npos);
    EXPECT_NE(shader.find("topLeftPhase + float2(0.0, outputInvSize.y)"), std::string::npos);
    EXPECT_NE(shader.find("topLeftPhase + outputInvSize"), std::string::npos);
    EXPECT_EQ(shader.find("float2 halfPixel = outputInvSize * 0.5"), std::string::npos);
}

TEST(VideoEncoderSourceTest, ExplicitBt709ToneMapsHdrBeforeSdrVideoProcessorConversion) {
    const std::string source = ReadVideoEncoderSource();
    const std::string shader = ReadVideoColorShaderSource();
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(shader.empty());

    EXPECT_NE(source.find("const bool outputIsHDR = ShouldEncodeHdrOutput()"), std::string::npos);
    EXPECT_NE(source.find("color_space=bt709 explicitly requests SDR"), std::string::npos);
    EXPECT_NE(source.find("QuerySdrWhiteLevelNits"), std::string::npos);
    EXPECT_NE(source.find("DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL"), std::string::npos);
    EXPECT_NE(source.find("[HDR->SDR] Whole-frame shader tone map active"), std::string::npos);
    EXPECT_NE(shader.find("float3 AbsoluteNitsToSdr(float3 rec709Nits)"), std::string::npos);
    EXPECT_NE(shader.find("float3 CompressRec709Gamut(float3 rgb, float luminance)"), std::string::npos);
    EXPECT_NE(shader.find("lerp(neutral, rgb, saturate(chromaScale))"), std::string::npos);
    EXPECT_NE(shader.find("float3 ScRgbToSdr(float3 scRgb)"), std::string::npos);
    EXPECT_NE(shader.find("float3 Hdr10ToSdr(float3 pqRec2020)"), std::string::npos);
    EXPECT_NE(shader.find("colorTransform == 3"), std::string::npos);
    EXPECT_NE(shader.find("colorTransform == 4"), std::string::npos);
    EXPECT_NE(shader.find("c.a = 1.0"), std::string::npos);
    EXPECT_NE(source.find("passes=1 intermediate=RGB10 gamut=luminance-preserving"), std::string::npos);
    EXPECT_NE(source.find("frameAlpha=opaque"), std::string::npos);
}

TEST(VideoEncoderSourceTest, RgbColorConversionShaderCompilesForRuntimeProfiles) {
    HMODULE compilerModule = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    ASSERT_NE(compilerModule, nullptr);

    using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
                                          LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    auto compile = reinterpret_cast<D3DCompileFn>(GetProcAddress(compilerModule, "D3DCompile"));
    ASSERT_NE(compile, nullptr);

    struct ShaderProfile {
        const char* entry;
        const char* target;
    };
    const ShaderProfile profiles[] = {{"VS_Main", "vs_4_0"},       {"PS_Main", "ps_4_0"},
                                      {"PS_P010Y", "ps_4_0"},      {"PS_P010UV", "ps_4_0"}};
    for (const ShaderProfile& profile : profiles) {
        ID3DBlob* blob = nullptr;
        ID3DBlob* errors = nullptr;
        const HRESULT hr = compile(ce::video_color::kRgbColorConversionShaderSource,
                                   sizeof(ce::video_color::kRgbColorConversionShaderSource) - 1, nullptr, nullptr,
                                   nullptr, profile.entry, profile.target, 0, 0, &blob, &errors);
        const std::string diagnostic =
            errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "";
        if (errors) {
            errors->Release();
        }
        if (blob) {
            blob->Release();
        }
        EXPECT_TRUE(SUCCEEDED(hr)) << profile.entry << "/" << profile.target << ": " << diagnostic;
    }

    FreeLibrary(compilerModule);
}

TEST(VideoEncoderSourceTest, ForcedSdrToneMapPlacesConfiguredPaperWhiteAtSdrHeadroomAndForcesOpaqueOutput) {
    ce::ComGuard<ID3D11Device> device;
    ce::ComGuard<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel = {};
    const HRESULT deviceHr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                                device.addressof(), &featureLevel, context.addressof());
    ASSERT_TRUE(SUCCEEDED(deviceHr)) << std::hex << deviceHr;
    ASSERT_GE(featureLevel, D3D_FEATURE_LEVEL_11_0);

    constexpr float kSdrWhiteNits = 203.0f;
    const float sourcePixel[4] = {kSdrWhiteNits / 80.0f, kSdrWhiteNits / 80.0f, kSdrWhiteNits / 80.0f, 0.0f};
    D3D11_TEXTURE2D_DESC sourceDesc = {};
    sourceDesc.Width = 1;
    sourceDesc.Height = 1;
    sourceDesc.MipLevels = 1;
    sourceDesc.ArraySize = 1;
    sourceDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    sourceDesc.SampleDesc.Count = 1;
    sourceDesc.Usage = D3D11_USAGE_IMMUTABLE;
    sourceDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sourceData = {};
    sourceData.pSysMem = sourcePixel;
    sourceData.SysMemPitch = sizeof(sourcePixel);
    ce::ComGuard<ID3D11Texture2D> sourceTexture;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&sourceDesc, &sourceData, sourceTexture.addressof())));

    D3D11_TEXTURE2D_DESC outputDesc = sourceDesc;
    outputDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    outputDesc.Usage = D3D11_USAGE_DEFAULT;
    outputDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ce::ComGuard<ID3D11Texture2D> outputTexture;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&outputDesc, nullptr, outputTexture.addressof())));
    ce::ComGuard<ID3D11ShaderResourceView> sourceView;
    ce::ComGuard<ID3D11RenderTargetView> outputView;
    ASSERT_TRUE(SUCCEEDED(device->CreateShaderResourceView(sourceTexture.get(), nullptr, sourceView.addressof())));
    ASSERT_TRUE(SUCCEEDED(device->CreateRenderTargetView(outputTexture.get(), nullptr, outputView.addressof())));

    ce::ComGuard<ID3DBlob> vertexBlob;
    ce::ComGuard<ID3DBlob> pixelBlob;
    ce::ComGuard<ID3DBlob> errors;
    ASSERT_TRUE(SUCCEEDED(D3DCompile(ce::video_color::kRgbColorConversionShaderSource,
                                     sizeof(ce::video_color::kRgbColorConversionShaderSource) - 1, nullptr, nullptr,
                                     nullptr, "VS_Main", "vs_4_0", 0, 0, vertexBlob.addressof(), errors.addressof())));
    errors.reset();
    ASSERT_TRUE(SUCCEEDED(D3DCompile(ce::video_color::kRgbColorConversionShaderSource,
                                     sizeof(ce::video_color::kRgbColorConversionShaderSource) - 1, nullptr, nullptr,
                                     nullptr, "PS_Main", "ps_4_0", 0, 0, pixelBlob.addressof(), errors.addressof())));
    ce::ComGuard<ID3D11VertexShader> vertexShader;
    ce::ComGuard<ID3D11PixelShader> pixelShader;
    ASSERT_TRUE(SUCCEEDED(device->CreateVertexShader(vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(),
                                                     nullptr, vertexShader.addressof())));
    ASSERT_TRUE(SUCCEEDED(device->CreatePixelShader(pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize(), nullptr,
                                                    pixelShader.addressof())));

    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ce::ComGuard<ID3D11SamplerState> sampler;
    ASSERT_TRUE(SUCCEEDED(device->CreateSamplerState(&samplerDesc, sampler.addressof())));
    struct CopyConstants {
        uint32_t colorTransform;
        uint32_t padding;
        float outputInvWidth;
        float outputInvHeight;
        float lumaSharpenStrength;
        float sdrWhiteNits;
        float padding2[2];
    };
    const CopyConstants constants = {3, 0, 1.0f, 1.0f, 0.0f, kSdrWhiteNits, {0.0f, 0.0f}};
    D3D11_BUFFER_DESC constantsDesc = {};
    constantsDesc.ByteWidth = sizeof(constants);
    constantsDesc.Usage = D3D11_USAGE_IMMUTABLE;
    constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA constantsData = {};
    constantsData.pSysMem = &constants;
    ce::ComGuard<ID3D11Buffer> constantsBuffer;
    ASSERT_TRUE(SUCCEEDED(device->CreateBuffer(&constantsDesc, &constantsData, constantsBuffer.addressof())));

    D3D11_VIEWPORT viewport = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    ID3D11RenderTargetView* outputViewRaw = outputView.get();
    ID3D11ShaderResourceView* sourceViewRaw = sourceView.get();
    ID3D11SamplerState* samplerRaw = sampler.get();
    ID3D11Buffer* constantsRaw = constantsBuffer.get();
    context->RSSetViewports(1, &viewport);
    context->OMSetRenderTargets(1, &outputViewRaw, nullptr);
    context->VSSetShader(vertexShader.get(), nullptr, 0);
    context->PSSetShader(pixelShader.get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, &sourceViewRaw);
    context->PSSetSamplers(0, 1, &samplerRaw);
    context->PSSetConstantBuffers(0, 1, &constantsRaw);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->Draw(3, 0);
    ID3D11RenderTargetView* nullRtv = nullptr;
    ID3D11ShaderResourceView* nullSrv = nullptr;
    context->OMSetRenderTargets(1, &nullRtv, nullptr);
    context->PSSetShaderResources(0, 1, &nullSrv);

    D3D11_TEXTURE2D_DESC stagingDesc = outputDesc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ce::ComGuard<ID3D11Texture2D> stagingTexture;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&stagingDesc, nullptr, stagingTexture.addressof())));
    context->CopyResource(stagingTexture.get(), outputTexture.get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    ASSERT_TRUE(SUCCEEDED(context->Map(stagingTexture.get(), 0, D3D11_MAP_READ, 0, &mapped)));
    const uint32_t packed = *static_cast<const uint32_t*>(mapped.pData);
    context->Unmap(stagingTexture.get(), 0);

    const int red = packed & 0x3ff;
    const int green = (packed >> 10) & 0x3ff;
    const int blue = (packed >> 20) & 0x3ff;
    EXPECT_NEAR(red, 927, 1);
    EXPECT_NEAR(green, 927, 1);
    EXPECT_NEAR(blue, 927, 1);
    EXPECT_EQ(packed >> 30, 3u);
}

TEST(VideoEncoderSourceTest, ForcedSdrPackedHdr10OverlayGreenRemainsColoredAndOpaque) {
    ce::ComGuard<ID3D11Device> device;
    ce::ComGuard<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel = {};
    const HRESULT deviceHr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                                device.addressof(), &featureLevel, context.addressof());
    ASSERT_TRUE(SUCCEEDED(deviceHr)) << std::hex << deviceHr;
    ASSERT_GE(featureLevel, D3D_FEATURE_LEVEL_11_0);

    constexpr float kSdrWhiteNits = 320.0f;
    constexpr uint32_t kHdr10OverlayGreen = 0x18c9ea0du;
    D3D11_TEXTURE2D_DESC sourceDesc = {};
    sourceDesc.Width = 1;
    sourceDesc.Height = 1;
    sourceDesc.MipLevels = 1;
    sourceDesc.ArraySize = 1;
    sourceDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    sourceDesc.SampleDesc.Count = 1;
    sourceDesc.Usage = D3D11_USAGE_IMMUTABLE;
    sourceDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sourceData = {};
    sourceData.pSysMem = &kHdr10OverlayGreen;
    sourceData.SysMemPitch = sizeof(kHdr10OverlayGreen);
    ce::ComGuard<ID3D11Texture2D> sourceTexture;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&sourceDesc, &sourceData, sourceTexture.addressof())));

    D3D11_TEXTURE2D_DESC outputDesc = sourceDesc;
    outputDesc.Usage = D3D11_USAGE_DEFAULT;
    outputDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ce::ComGuard<ID3D11Texture2D> outputTexture;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&outputDesc, nullptr, outputTexture.addressof())));
    ce::ComGuard<ID3D11ShaderResourceView> sourceView;
    ce::ComGuard<ID3D11RenderTargetView> outputView;
    ASSERT_TRUE(SUCCEEDED(device->CreateShaderResourceView(sourceTexture.get(), nullptr, sourceView.addressof())));
    ASSERT_TRUE(SUCCEEDED(device->CreateRenderTargetView(outputTexture.get(), nullptr, outputView.addressof())));

    ce::ComGuard<ID3DBlob> vertexBlob;
    ce::ComGuard<ID3DBlob> pixelBlob;
    ce::ComGuard<ID3DBlob> errors;
    ASSERT_TRUE(SUCCEEDED(D3DCompile(ce::video_color::kRgbColorConversionShaderSource,
                                     sizeof(ce::video_color::kRgbColorConversionShaderSource) - 1, nullptr, nullptr,
                                     nullptr, "VS_Main", "vs_4_0", 0, 0, vertexBlob.addressof(), errors.addressof())));
    errors.reset();
    ASSERT_TRUE(SUCCEEDED(D3DCompile(ce::video_color::kRgbColorConversionShaderSource,
                                     sizeof(ce::video_color::kRgbColorConversionShaderSource) - 1, nullptr, nullptr,
                                     nullptr, "PS_Main", "ps_4_0", 0, 0, pixelBlob.addressof(), errors.addressof())));
    ce::ComGuard<ID3D11VertexShader> vertexShader;
    ce::ComGuard<ID3D11PixelShader> pixelShader;
    ASSERT_TRUE(SUCCEEDED(device->CreateVertexShader(vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(),
                                                     nullptr, vertexShader.addressof())));
    ASSERT_TRUE(SUCCEEDED(device->CreatePixelShader(pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize(), nullptr,
                                                    pixelShader.addressof())));

    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ce::ComGuard<ID3D11SamplerState> sampler;
    ASSERT_TRUE(SUCCEEDED(device->CreateSamplerState(&samplerDesc, sampler.addressof())));
    struct CopyConstants {
        uint32_t colorTransform;
        uint32_t padding;
        float outputInvWidth;
        float outputInvHeight;
        float lumaSharpenStrength;
        float sdrWhiteNits;
        float padding2[2];
    };
    const CopyConstants constants = {4, 0, 1.0f, 1.0f, 0.0f, kSdrWhiteNits, {0.0f, 0.0f}};
    D3D11_BUFFER_DESC constantsDesc = {};
    constantsDesc.ByteWidth = sizeof(constants);
    constantsDesc.Usage = D3D11_USAGE_IMMUTABLE;
    constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA constantsData = {};
    constantsData.pSysMem = &constants;
    ce::ComGuard<ID3D11Buffer> constantsBuffer;
    ASSERT_TRUE(SUCCEEDED(device->CreateBuffer(&constantsDesc, &constantsData, constantsBuffer.addressof())));

    D3D11_VIEWPORT viewport = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    ID3D11RenderTargetView* outputViewRaw = outputView.get();
    ID3D11ShaderResourceView* sourceViewRaw = sourceView.get();
    ID3D11SamplerState* samplerRaw = sampler.get();
    ID3D11Buffer* constantsRaw = constantsBuffer.get();
    context->RSSetViewports(1, &viewport);
    context->OMSetRenderTargets(1, &outputViewRaw, nullptr);
    context->VSSetShader(vertexShader.get(), nullptr, 0);
    context->PSSetShader(pixelShader.get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, &sourceViewRaw);
    context->PSSetSamplers(0, 1, &samplerRaw);
    context->PSSetConstantBuffers(0, 1, &constantsRaw);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->Draw(3, 0);
    ID3D11RenderTargetView* nullRtv = nullptr;
    ID3D11ShaderResourceView* nullSrv = nullptr;
    context->OMSetRenderTargets(1, &nullRtv, nullptr);
    context->PSSetShaderResources(0, 1, &nullSrv);

    D3D11_TEXTURE2D_DESC stagingDesc = outputDesc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ce::ComGuard<ID3D11Texture2D> stagingTexture;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&stagingDesc, nullptr, stagingTexture.addressof())));
    context->CopyResource(stagingTexture.get(), outputTexture.get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    ASSERT_TRUE(SUCCEEDED(context->Map(stagingTexture.get(), 0, D3D11_MAP_READ, 0, &mapped)));
    const uint32_t packed = *static_cast<const uint32_t*>(mapped.pData);
    context->Unmap(stagingTexture.get(), 0);

    EXPECT_NEAR(static_cast<int>(packed & 0x3ff), 0, 2);
    EXPECT_NEAR(static_cast<int>((packed >> 10) & 0x3ff), 927, 2);
    EXPECT_NEAR(static_cast<int>((packed >> 20) & 0x3ff), 4, 2);
    EXPECT_EQ(packed >> 30, 3u);
}

TEST(VideoEncoderSourceTest, DirectHdrP010ShaderWritesCanonicalRedCodes) {
    ce::ComGuard<ID3D11Device> baseDevice;
    ce::ComGuard<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel = {};
    const HRESULT deviceHr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                                baseDevice.addressof(), &featureLevel, context.addressof());
    ASSERT_TRUE(SUCCEEDED(deviceHr)) << std::hex << deviceHr;
    ASSERT_GE(featureLevel, D3D_FEATURE_LEVEL_11_0);

    ce::ComGuard<ID3D11Device3> device;
    ASSERT_TRUE(SUCCEEDED(baseDevice->QueryInterface(IID_PPV_ARGS(device.addressof()))));

    constexpr UINT kWidth = 2;
    constexpr UINT kHeight = 2;
    constexpr uint32_t kOpaqueRedRgb10 = 0xc00003ffu;
    const uint32_t sourcePixels[kWidth * kHeight] = {kOpaqueRedRgb10, kOpaqueRedRgb10, kOpaqueRedRgb10,
                                                     kOpaqueRedRgb10};
    D3D11_TEXTURE2D_DESC sourceDesc = {};
    sourceDesc.Width = kWidth;
    sourceDesc.Height = kHeight;
    sourceDesc.MipLevels = 1;
    sourceDesc.ArraySize = 1;
    sourceDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    sourceDesc.SampleDesc.Count = 1;
    sourceDesc.Usage = D3D11_USAGE_IMMUTABLE;
    sourceDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sourceData = {};
    sourceData.pSysMem = sourcePixels;
    sourceData.SysMemPitch = kWidth * sizeof(uint32_t);
    ce::ComGuard<ID3D11Texture2D> sourceTexture;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&sourceDesc, &sourceData, sourceTexture.addressof())));

    D3D11_TEXTURE2D_DESC outputDesc = {};
    outputDesc.Width = kWidth;
    outputDesc.Height = kHeight;
    outputDesc.MipLevels = 1;
    outputDesc.ArraySize = 1;
    outputDesc.Format = DXGI_FORMAT_P010;
    outputDesc.SampleDesc.Count = 1;
    outputDesc.Usage = D3D11_USAGE_DEFAULT;
    outputDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ce::ComGuard<ID3D11Texture2D> outputTexture;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&outputDesc, nullptr, outputTexture.addressof())));

    auto createPlaneView = [&](DXGI_FORMAT format, UINT plane, ce::ComGuard<ID3D11RenderTargetView1>& view) {
        D3D11_RENDER_TARGET_VIEW_DESC1 desc = {};
        desc.Format = format;
        desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipSlice = 0;
        desc.Texture2D.PlaneSlice = plane;
        return device->CreateRenderTargetView1(outputTexture.get(), &desc, view.addressof());
    };
    ce::ComGuard<ID3D11RenderTargetView1> lumaView;
    ce::ComGuard<ID3D11RenderTargetView1> chromaView;
    ASSERT_TRUE(SUCCEEDED(createPlaneView(DXGI_FORMAT_R16_UNORM, 0, lumaView)));
    ASSERT_TRUE(SUCCEEDED(createPlaneView(DXGI_FORMAT_R16G16_UNORM, 1, chromaView)));

    ce::ComGuard<ID3D11ShaderResourceView> sourceView;
    ASSERT_TRUE(SUCCEEDED(device->CreateShaderResourceView(sourceTexture.get(), nullptr, sourceView.addressof())));

    ce::ComGuard<ID3DBlob> vertexBlob;
    ce::ComGuard<ID3DBlob> lumaBlob;
    ce::ComGuard<ID3DBlob> chromaBlob;
    auto compile = [&](const char* entry, const char* target, ce::ComGuard<ID3DBlob>& blob) {
        ce::ComGuard<ID3DBlob> errors;
        const HRESULT hr = D3DCompile(ce::video_color::kRgbColorConversionShaderSource,
                                      sizeof(ce::video_color::kRgbColorConversionShaderSource) - 1, nullptr, nullptr,
                                      nullptr, entry, target, 0, 0, blob.addressof(), errors.addressof());
        const std::string diagnostic =
            errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "";
        EXPECT_TRUE(SUCCEEDED(hr)) << entry << "/" << target << ": " << diagnostic;
        return hr;
    };
    ASSERT_TRUE(SUCCEEDED(compile("VS_Main", "vs_4_0", vertexBlob)));
    ASSERT_TRUE(SUCCEEDED(compile("PS_P010Y", "ps_4_0", lumaBlob)));
    ASSERT_TRUE(SUCCEEDED(compile("PS_P010UV", "ps_4_0", chromaBlob)));

    ce::ComGuard<ID3D11VertexShader> vertexShader;
    ce::ComGuard<ID3D11PixelShader> lumaShader;
    ce::ComGuard<ID3D11PixelShader> chromaShader;
    ASSERT_TRUE(SUCCEEDED(device->CreateVertexShader(vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(),
                                                     nullptr, vertexShader.addressof())));
    ASSERT_TRUE(SUCCEEDED(device->CreatePixelShader(lumaBlob->GetBufferPointer(), lumaBlob->GetBufferSize(), nullptr,
                                                    lumaShader.addressof())));
    ASSERT_TRUE(SUCCEEDED(device->CreatePixelShader(chromaBlob->GetBufferPointer(), chromaBlob->GetBufferSize(),
                                                    nullptr, chromaShader.addressof())));

    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ce::ComGuard<ID3D11SamplerState> sampler;
    ASSERT_TRUE(SUCCEEDED(device->CreateSamplerState(&samplerDesc, sampler.addressof())));

    struct CopyConstants {
        uint32_t colorTransform;
        uint32_t padding;
        float outputInvWidth;
        float outputInvHeight;
        float lumaSharpenStrength;
        float sdrWhiteNits;
        float padding2[2];
    };
    // sdrWhiteNits=0 selects limited range (default for P010); sdrWhiteNits>=1 selects full range.
    const CopyConstants constants = {0, 0, 1.0f / kWidth, 1.0f / kHeight, 0.0f, 0.0f, {0.0f, 0.0f}};
    D3D11_BUFFER_DESC constantsDesc = {};
    constantsDesc.ByteWidth = sizeof(constants);
    constantsDesc.Usage = D3D11_USAGE_IMMUTABLE;
    constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA constantsData = {};
    constantsData.pSysMem = &constants;
    ce::ComGuard<ID3D11Buffer> constantsBuffer;
    ASSERT_TRUE(SUCCEEDED(device->CreateBuffer(&constantsDesc, &constantsData, constantsBuffer.addressof())));

    ID3D11ShaderResourceView* sourceViewRaw = sourceView.get();
    ID3D11SamplerState* samplerRaw = sampler.get();
    ID3D11Buffer* constantsRaw = constantsBuffer.get();
    context->VSSetShader(vertexShader.get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, &sourceViewRaw);
    context->PSSetSamplers(0, 1, &samplerRaw);
    context->PSSetConstantBuffers(0, 1, &constantsRaw);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D11_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f};
    ID3D11RenderTargetView* lumaViewRaw = lumaView.get();
    context->RSSetViewports(1, &viewport);
    context->OMSetRenderTargets(1, &lumaViewRaw, nullptr);
    context->PSSetShader(lumaShader.get(), nullptr, 0);
    context->Draw(3, 0);

    viewport.Width = static_cast<float>(kWidth / 2);
    viewport.Height = static_cast<float>(kHeight / 2);
    ID3D11RenderTargetView* chromaViewRaw = chromaView.get();
    context->RSSetViewports(1, &viewport);
    context->OMSetRenderTargets(1, &chromaViewRaw, nullptr);
    context->PSSetShader(chromaShader.get(), nullptr, 0);
    context->Draw(3, 0);
    ID3D11RenderTargetView* nullRtv = nullptr;
    ID3D11ShaderResourceView* nullSrv = nullptr;
    context->OMSetRenderTargets(1, &nullRtv, nullptr);
    context->PSSetShaderResources(0, 1, &nullSrv);

    D3D11_TEXTURE2D_DESC stagingDesc = outputDesc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ce::ComGuard<ID3D11Texture2D> stagingTexture;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&stagingDesc, nullptr, stagingTexture.addressof())));
    context->CopyResource(stagingTexture.get(), outputTexture.get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    ASSERT_TRUE(SUCCEEDED(context->Map(stagingTexture.get(), 0, D3D11_MAP_READ, 0, &mapped)));

    constexpr uint16_t kExpectedY = 294u << 6;
    constexpr uint16_t kExpectedCb = 387u << 6;
    constexpr uint16_t kExpectedCr = 960u << 6;
    const auto* bytes = static_cast<const uint8_t*>(mapped.pData);
    EXPECT_EQ(*reinterpret_cast<const uint16_t*>(bytes), kExpectedY);
    EXPECT_EQ(*reinterpret_cast<const uint16_t*>(bytes + sizeof(uint16_t)), kExpectedY);
    EXPECT_EQ(*reinterpret_cast<const uint16_t*>(bytes + mapped.RowPitch), kExpectedY);
    EXPECT_EQ(*reinterpret_cast<const uint16_t*>(bytes + mapped.RowPitch + sizeof(uint16_t)), kExpectedY);
    const auto* chroma = reinterpret_cast<const uint16_t*>(bytes + mapped.RowPitch * kHeight);
    EXPECT_EQ(chroma[0], kExpectedCb);
    EXPECT_EQ(chroma[1], kExpectedCr);
    context->Unmap(stagingTexture.get(), 0);
}

TEST(VideoEncoderSourceTest, DirectHdrP010ShaderProducesNeutralChromaForWhite) {
    ce::ComGuard<ID3D11Device> baseDevice;
    ce::ComGuard<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel = {};
    const HRESULT deviceHr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                                baseDevice.addressof(), &featureLevel, context.addressof());
    ASSERT_TRUE(SUCCEEDED(deviceHr)) << std::hex << deviceHr;
    ASSERT_GE(featureLevel, D3D_FEATURE_LEVEL_11_0);

    ce::ComGuard<ID3D11Device3> device;
    ASSERT_TRUE(SUCCEEDED(baseDevice->QueryInterface(IID_PPV_ARGS(device.addressof()))));

    constexpr UINT kWidth = 2;
    constexpr UINT kHeight = 2;
    constexpr uint32_t kNeutralRgb10 = [] {
        constexpr uint32_t kCode = 625u;  // PQ(203) * 1023 ≈ 625
        return (3u << 30) | (kCode << 20) | (kCode << 10) | kCode;
    }();
    const uint32_t sourcePixels[kWidth * kHeight] = {kNeutralRgb10, kNeutralRgb10, kNeutralRgb10, kNeutralRgb10};
    D3D11_TEXTURE2D_DESC sourceDesc = {};
    sourceDesc.Width = kWidth;
    sourceDesc.Height = kHeight;
    sourceDesc.MipLevels = 1;
    sourceDesc.ArraySize = 1;
    sourceDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    sourceDesc.SampleDesc.Count = 1;
    sourceDesc.Usage = D3D11_USAGE_IMMUTABLE;
    sourceDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sourceData = {};
    sourceData.pSysMem = sourcePixels;
    sourceData.SysMemPitch = kWidth * sizeof(uint32_t);
    ce::ComGuard<ID3D11Texture2D> sourceTexture;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&sourceDesc, &sourceData, sourceTexture.addressof())));

    D3D11_TEXTURE2D_DESC outputDesc = {};
    outputDesc.Width = kWidth;
    outputDesc.Height = kHeight;
    outputDesc.MipLevels = 1;
    outputDesc.ArraySize = 1;
    outputDesc.Format = DXGI_FORMAT_P010;
    outputDesc.SampleDesc.Count = 1;
    outputDesc.Usage = D3D11_USAGE_DEFAULT;
    outputDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ce::ComGuard<ID3D11Texture2D> outputTexture;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&outputDesc, nullptr, outputTexture.addressof())));

    auto createPlaneView = [&](DXGI_FORMAT format, UINT plane, ce::ComGuard<ID3D11RenderTargetView1>& view) {
        D3D11_RENDER_TARGET_VIEW_DESC1 desc = {};
        desc.Format = format;
        desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipSlice = 0;
        desc.Texture2D.PlaneSlice = plane;
        return device->CreateRenderTargetView1(outputTexture.get(), &desc, view.addressof());
    };
    ce::ComGuard<ID3D11RenderTargetView1> lumaView;
    ce::ComGuard<ID3D11RenderTargetView1> chromaView;
    ASSERT_TRUE(SUCCEEDED(createPlaneView(DXGI_FORMAT_R16_UNORM, 0, lumaView)));
    ASSERT_TRUE(SUCCEEDED(createPlaneView(DXGI_FORMAT_R16G16_UNORM, 1, chromaView)));

    ce::ComGuard<ID3D11ShaderResourceView> sourceView;
    ASSERT_TRUE(SUCCEEDED(device->CreateShaderResourceView(sourceTexture.get(), nullptr, sourceView.addressof())));

    ce::ComGuard<ID3DBlob> vertexBlob;
    ce::ComGuard<ID3DBlob> lumaBlob;
    ce::ComGuard<ID3DBlob> chromaBlob;
    auto compile = [&](const char* entry, const char* target, ce::ComGuard<ID3DBlob>& blob) {
        ce::ComGuard<ID3DBlob> errors;
        const HRESULT hr = D3DCompile(ce::video_color::kRgbColorConversionShaderSource,
                                      sizeof(ce::video_color::kRgbColorConversionShaderSource) - 1, nullptr, nullptr,
                                      nullptr, entry, target, 0, 0, blob.addressof(), errors.addressof());
        const std::string diagnostic =
            errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "";
        EXPECT_TRUE(SUCCEEDED(hr)) << entry << "/" << target << ": " << diagnostic;
        return hr;
    };
    ASSERT_TRUE(SUCCEEDED(compile("VS_Main", "vs_4_0", vertexBlob)));
    ASSERT_TRUE(SUCCEEDED(compile("PS_P010Y", "ps_4_0", lumaBlob)));
    ASSERT_TRUE(SUCCEEDED(compile("PS_P010UV", "ps_4_0", chromaBlob)));

    ce::ComGuard<ID3D11VertexShader> vertexShader;
    ce::ComGuard<ID3D11PixelShader> lumaShader;
    ce::ComGuard<ID3D11PixelShader> chromaShader;
    ASSERT_TRUE(SUCCEEDED(device->CreateVertexShader(vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(),
                                                     nullptr, vertexShader.addressof())));
    ASSERT_TRUE(SUCCEEDED(device->CreatePixelShader(lumaBlob->GetBufferPointer(), lumaBlob->GetBufferSize(), nullptr,
                                                    lumaShader.addressof())));
    ASSERT_TRUE(SUCCEEDED(device->CreatePixelShader(chromaBlob->GetBufferPointer(), chromaBlob->GetBufferSize(), nullptr,
                                                    chromaShader.addressof())));

    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ce::ComGuard<ID3D11SamplerState> sampler;
    ASSERT_TRUE(SUCCEEDED(device->CreateSamplerState(&samplerDesc, sampler.addressof())));

    struct CopyConstants {
        uint32_t colorTransform;
        uint32_t padding;
        float outputInvWidth;
        float outputInvHeight;
        float lumaSharpenStrength;
        float sdrWhiteNits;
        float padding2[2];
    };
    const CopyConstants constants = {0, 0, 1.0f / kWidth, 1.0f / kHeight, 0.0f, 0.0f, {0.0f, 0.0f}};
    D3D11_BUFFER_DESC constantsDesc = {};
    constantsDesc.ByteWidth = sizeof(constants);
    constantsDesc.Usage = D3D11_USAGE_IMMUTABLE;
    constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA constantsData = {};
    constantsData.pSysMem = &constants;
    ce::ComGuard<ID3D11Buffer> constantsBuffer;
    ASSERT_TRUE(SUCCEEDED(device->CreateBuffer(&constantsDesc, &constantsData, constantsBuffer.addressof())));

    ID3D11ShaderResourceView* sourceViewRaw = sourceView.get();
    ID3D11SamplerState* samplerRaw = sampler.get();
    ID3D11Buffer* constantsRaw = constantsBuffer.get();
    context->VSSetShader(vertexShader.get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, &sourceViewRaw);
    context->PSSetSamplers(0, 1, &samplerRaw);
    context->PSSetConstantBuffers(0, 1, &constantsRaw);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D11_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f};
    ID3D11RenderTargetView* lumaViewRaw = lumaView.get();
    context->RSSetViewports(1, &viewport);
    context->OMSetRenderTargets(1, &lumaViewRaw, nullptr);
    context->PSSetShader(lumaShader.get(), nullptr, 0);
    context->Draw(3, 0);

    viewport.Width = static_cast<float>(kWidth / 2);
    viewport.Height = static_cast<float>(kHeight / 2);
    ID3D11RenderTargetView* chromaViewRaw = chromaView.get();
    context->RSSetViewports(1, &viewport);
    context->OMSetRenderTargets(1, &chromaViewRaw, nullptr);
    context->PSSetShader(chromaShader.get(), nullptr, 0);
    context->Draw(3, 0);
    ID3D11RenderTargetView* nullRtv = nullptr;
    ID3D11ShaderResourceView* nullSrv = nullptr;
    context->OMSetRenderTargets(1, &nullRtv, nullptr);
    context->PSSetShaderResources(0, 1, &nullSrv);

    D3D11_TEXTURE2D_DESC stagingDesc = outputDesc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ce::ComGuard<ID3D11Texture2D> stagingTexture;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&stagingDesc, nullptr, stagingTexture.addressof())));
    context->CopyResource(stagingTexture.get(), outputTexture.get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    ASSERT_TRUE(SUCCEEDED(context->Map(stagingTexture.get(), 0, D3D11_MAP_READ, 0, &mapped)));

    constexpr uint16_t kNeutralCb = 512u << 6;
    constexpr uint16_t kNeutralCr = 512u << 6;
    const auto* chromaOutput = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mapped.pData) + mapped.RowPitch * kHeight);
    EXPECT_EQ(chromaOutput[0], kNeutralCb);
    EXPECT_EQ(chromaOutput[1], kNeutralCr);
    context->Unmap(stagingTexture.get(), 0);
}
