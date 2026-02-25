#include "video_encoder.h"
#include "../common/raii_helpers.h"
#include "../common/shared_defs.h"
#include "mediaengine.h"

extern "C" {
#include <libavutil/pixfmt.h>
}
#include <chrono>
#include <d3d11_4.h>
#include <dxgi1_5.h>
#include <functional>
#include <unordered_map>

#include "cursor_renderer.h"
#include <filesystem>
namespace fs = std::filesystem;

#ifndef D3D11_FORMAT_SUPPORT_SHAREABLE
#define D3D11_FORMAT_SUPPORT_SHAREABLE 0x2000
#endif

static HANDLE NormalizeSourceHandleForWow64(HANDLE handle, uint32_t sourcePid) {
#if defined(_WIN64)
  if (!handle || sourcePid == 0) {
    return handle;
  }

  static std::mutex s_bitnessMutex;
  static std::unordered_map<uint32_t, bool> s_isWow64ByPid;

  bool isWow64Source = false;
  {
    std::lock_guard<std::mutex> lock(s_bitnessMutex);
    auto it = s_isWow64ByPid.find(sourcePid);
    if (it != s_isWow64ByPid.end()) {
      isWow64Source = it->second;
    } else {
      ce::HandleGuard hProcess(
          OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, sourcePid));
      if (hProcess) {
        BOOL wow64 = FALSE;
        if (IsWow64Process(hProcess.get(), &wow64)) {
          isWow64Source = (wow64 == TRUE);
        }
      }
      s_isWow64ByPid[sourcePid] = isWow64Source;
    }
  }

  if (!isWow64Source) {
    return handle;
  }

  const uint64_t rawHandle =
      static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
  const int64_t signExtended = static_cast<int64_t>(
      static_cast<int32_t>(static_cast<uint32_t>(rawHandle)));
  if (signExtended != static_cast<int64_t>(rawHandle)) {
    static std::atomic<int> s_normalizeLogCount{0};
    if (s_normalizeLogCount.fetch_add(1, std::memory_order_relaxed) < 6) {
      DLL_Log("[VideoEncoder] WOW64 handle normalized for PID %u: %p -> %p",
              sourcePid, (HANDLE)(uintptr_t)rawHandle,
              (HANDLE)(uint64_t)signExtended);
    }
  }
  return reinterpret_cast<HANDLE>(static_cast<uint64_t>(signExtended));
#else
  (void)sourcePid;
  return handle;
#endif
}

// Helper to generate robust output filename
static std::string GenerateOutputFilename(const VideoConfig &config) {
  // Default to "captures" subdirectory if outputDir is not specified
  fs::path outDir = config.outputDir;
  if (outDir.empty()) {
    outDir = "captures";
  }

  // Create directory if it doesn't exist
  std::error_code ec;
  if (!fs::exists(outDir, ec)) {
    if (fs::create_directories(outDir, ec)) {
      DLL_Log("[VideoEncoder] Created output directory: %s",
              outDir.string().c_str());
    } else {
      DLL_Log("[VideoEncoder] Failed to create output directory: %s (Error: "
              "%d). Fallback to current dir.",
              outDir.string().c_str(), ec.value());
      outDir = ".";
    }
  } else {
    // DLL_Log("[VideoEncoder] Output directory exists: %s",
    // outDir.string().c_str());
  }

  std::string filenameOnly =
      "capture_" + std::to_string(GetTickCount64()) + "." + config.container;
  fs::path fullPath = outDir / filenameOnly;

  // Handle Windows backslashes for FFmpeg
  std::filesystem::path absPath = std::filesystem::absolute(fullPath, ec);
  if (!ec) {
    return absPath.string();
  }
  return fullPath.string();
}

// RAII Wrapper for MediaEngine D3D11 Guard
class D3D11ScopedLock {
public:
  D3D11ScopedLock() { MediaEngine_LockD3D11(); }
  ~D3D11ScopedLock() { MediaEngine_UnlockD3D11(); }
};

// Performance timing helper for pipeline analysis
class PerfTimer {
public:
  using Clock = std::chrono::high_resolution_clock;
  using TimePoint = Clock::time_point;

  static TimePoint now() { return Clock::now(); }

  static double elapsed_ms(const TimePoint &start, const TimePoint &end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
  }
};

// Frame statistics for performance monitoring
struct FrameStats {
  int64_t frameNumber = 0;
  int64_t ptsMs = 0;
  double fenceWaitMs = 0;
  double textureOpenMs = 0;
  double colorConvertMs = 0;
  double encodeMs = 0;
  double totalMs = 0;
  int packetsProduced = 0;
  int64_t expectedPtsDiff = 0; // Expected ms between frames
  int64_t actualPtsDiff = 0;   // Actual ms between frames
};

// Global stats for frame analysis
static int64_t g_lastFramePts = -1;
static int64_t g_framesEncoded = 0;
// static int64_t g_framesDropped = 0;
static double g_totalFenceWait = 0;
static double g_totalColorConvert = 0;
static double g_totalEncode = 0;
static double g_maxFrameTime = 0;
static int g_slowFrameCount = 0; // Frames taking > 2x expected time

// Helper to release D3D11 Texture when AVFrame is freed
static void FreeD3D11Tex(void *opaque, uint8_t *data) {
  ID3D11Texture2D *tex = (ID3D11Texture2D *)data;
  if (tex)
    tex->Release();
}

VideoEncoder::VideoEncoder()
    : fmtCtx(nullptr), codecCtx(nullptr), stream(nullptr), hwDeviceCtx(nullptr),
      hwFramesCtx(nullptr), cudaDeviceCtx(nullptr), cudaFramesCtx(nullptr),
      d3d11DeviceCtx(nullptr), d3d11FramesCtx(nullptr), d3d11Device(nullptr),
      d3d11Context(nullptr), luidLow(0), luidHigh(0), initDone(false),
      currentIsHDR(false), fileOpened(false), recordingRequested(false),
      isStopping(false), flushRequested(false), codecOpenFailed(false),
      startPts(-1), width(0), height(0), cachedSourcePid(0),
      lastEncodeTimeUs(0), fenceEvent(nullptr), videoDevice(nullptr),
      videoContext(nullptr), videoProcessor(nullptr),
      videoProcessorEnum(nullptr), currentNV12Buffer(0), inputView(nullptr),
      videoProcessorInit(false) {}

VideoEncoder::~VideoEncoder() {
  Stop(); // Triiger async stop

  // Destructor MUST be synchronous to ensure no threads are running
  // and all resources are safely released.
  if (writerThread.joinable()) {
    DLL_Log("[VideoEncoder] Destructor: Waiting for async writer to finish...");
    writerThread.join();
  }

  if (fenceEvent) {
    CloseHandle(fenceEvent);
    fenceEvent = nullptr;
  }
}

bool VideoEncoder::Init(const VideoConfig &config, int width, int height,
                        int fps,
                        std::function<void(AVPacket *)> packetCallback) {
  DLL_Log("[VideoEncoder] Init Entry - config.encoder=%s w=%d h=%d fps=%d",
          config.encoder.c_str(), width, height, fps);

  // Disable buffering to see logs immediately
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);

  DLL_Log("[VideoEncoder] Step 1: Setting member variables");
  this->width = width;
  this->height = height;
  this->captureCursor = config.captureCursor;
  this->gpuPriority = config.gpuPriority;
  this->onPacket = packetCallback;

  // Initialize cursor renderer if cursor capture enabled
  if (captureCursor) {
    cursorRenderer = std::make_unique<CursorRenderer>();
    DLL_Log("[VideoEncoder] Cursor capture enabled (renderer created)");
  }

  DLL_Log("[VideoEncoder] Step 2: Setting av_log level");
  av_log_set_level(AV_LOG_DEBUG);

  DLL_Log("[VideoEncoder] Step 3: Creating output filename");
  outputFilename = GenerateOutputFilename(config);
  DLL_Log("[VideoEncoder] Output file: %s", outputFilename.c_str());

  DLL_Log("[VideoEncoder] Step 4: Calling avformat_alloc_output_context2");
  if (avformat_alloc_output_context2(&fmtCtx, nullptr, nullptr,
                                     outputFilename.c_str()) < 0) {
    DLL_Log("[VideoEncoder] Failed to alloc output context");
    return false;
  }
  DLL_Log("[VideoEncoder] Step 4 done, fmtCtx=%p", (void *)fmtCtx);

  DLL_Log("[VideoEncoder] Step 5: Finding encoder: %s", config.encoder.c_str());
  const AVCodec *codec = avcodec_find_encoder_by_name(config.encoder.c_str());
  if (!codec) {
    DLL_Log("[VideoEncoder] Codec not found: %s", config.encoder.c_str());
    return false;
  }
  DLL_Log("[VideoEncoder] Step 5 done, codec=%p name=%s", (void *)codec,
          codec->name);

  DLL_Log("[VideoEncoder] Step 6: Allocating codec context");
  codecCtx = avcodec_alloc_context3(codec);
  if (!codecCtx) {
    DLL_Log("[VideoEncoder] Failed to alloc codec context");
    return false;
  }
  DLL_Log("[VideoEncoder] Step 6 done, codecCtx=%p", (void *)codecCtx);

  // Store config for use in EnsureDevice()
  savedConfig = config;

  DLL_Log("[VideoEncoder] Init Complete - returning true");
  // Defer device creation to EnsureDevice()
  return true;
}

void VideoEncoder::SetAdapterLUID(int32_t low, int32_t high) {
  this->luidLow = low;
  this->luidHigh = high;
}

void VideoEncoder::SetDimensions(uint32_t w, uint32_t h) {
  if (w > 0 && h > 0) {
    this->width = w;
    this->height = h;
    DLL_Log("[VideoEncoder] SetDimensions: %dx%d", w, h);
  }
}

bool VideoEncoder::CreateSharedCaptureTextures(uint32_t w, uint32_t h,
                                               uint32_t fmt,
                                               SharedMemoryLayout *sharedMem) {
  if (sharedCaptureTexturesCreated) {
    return true; // Already created
  }

  if (!d3d11Device) {
    DLL_Log("[VideoEncoder] CreateSharedCaptureTextures: No D3D11 device");
    return false;
  }

  DLL_Log("[VideoEncoder] Creating shared capture textures: %dx%d format=%d", w,
          h, fmt);

  // Create 4 shared textures with NT handles
  for (int i = 0; i < 4; i++) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = (DXGI_FORMAT)fmt;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags =
        D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

    HRESULT hr =
        d3d11Device->CreateTexture2D(&desc, nullptr, &sharedCaptureTextures[i]);
    if (FAILED(hr)) {
      DLL_Log("[VideoEncoder] Failed to create shared texture %d: HR=%x", i,
              hr);
      return false;
    }

    // Get IDXGIResource1 and create shared handle
    IDXGIResource1 *dxgiRes = nullptr;
    hr = sharedCaptureTextures[i]->QueryInterface(IID_PPV_ARGS(&dxgiRes));
    if (FAILED(hr)) {
      DLL_Log(
          "[VideoEncoder] Failed to get IDXGIResource1 for texture %d: HR=%x",
          i, hr);
      return false;
    }

    hr = dxgiRes->CreateSharedHandle(nullptr, // Use default security
                                     DXGI_SHARED_RESOURCE_READ |
                                         DXGI_SHARED_RESOURCE_WRITE,
                                     nullptr, // No name
                                     &sharedCaptureHandles[i]);
    dxgiRes->Release();

    if (FAILED(hr)) {
      DLL_Log(
          "[VideoEncoder] Failed to create shared handle for texture %d: HR=%x",
          i, hr);
      return false;
    }

    DLL_Log("[VideoEncoder] Created shared texture %d, handle=%p", i,
            sharedCaptureHandles[i]);
  }

  // Create event for CPU-side fence waiting
  fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

  // Create shared fence
  HRESULT hr = d3d11Device->CreateFence(0, D3D11_FENCE_FLAG_SHARED,
                                        IID_PPV_ARGS(&sharedCaptureFence));
  if (FAILED(hr)) {
    DLL_Log("[VideoEncoder] Failed to create shared fence: HR=%x", hr);
    return false;
  }

  // Export fence handle - CreateSharedHandle is on the fence object, not the
  // device
  hr = sharedCaptureFence->CreateSharedHandle(nullptr, // Security attributes
                                              GENERIC_ALL, // Access rights
                                              nullptr,     // Name (optional)
                                              &sharedCaptureFenceHandle);
  if (FAILED(hr)) {
    DLL_Log("[VideoEncoder] Failed to export fence handle: HR=%x", hr);
    return false;
  }

  DLL_Log("[VideoEncoder] Created shared fence, handle=%p",
          sharedCaptureFenceHandle);

  // Publish to shared memory
  if (sharedMem) {
    this->pSharedMem = sharedMem;
    for (int i = 0; i < 4; i++) {
      sharedMem->encoderTextures.SetTextureHandle(
          i, (uint64_t)sharedCaptureHandles[i]);
    }
    sharedMem->encoderTextures.SetFenceHandle(
        (uint64_t)sharedCaptureFenceHandle);
    sharedMem->encoderTextures.SetWidth(w);
    sharedMem->encoderTextures.SetHeight(h);
    sharedMem->encoderTextures.SetFormat(fmt);
    sharedMem->encoderTextures.ready.store(true, std::memory_order_release);
    DLL_Log("[VideoEncoder] Published encoder textures to shared memory");
  }

  sharedCaptureTexturesCreated = true;
  return true;
}

bool VideoEncoder::ConfigureAndOpenCodec() {
  if (!codecCtx || !fmtCtx) {
    DLL_Log("[VideoEncoder] ConfigureAndOpenCodec: Missing context(s)");
    return false;
  }

  const AVCodec *codec = codecCtx->codec;
  if (!codec) {
    codec = avcodec_find_encoder_by_name(savedConfig.encoder.c_str());
    if (!codec) {
      DLL_Log("[VideoEncoder] ConfigureAndOpenCodec: Codec not found");
      return false;
    }
  }

  // Build encoder options from savedConfig
  AVDictionary *opts = nullptr;

  // Log all config settings for debugging
  DLL_Log("[VideoEncoder] ===== ENCODER SETTINGS FROM CONFIG =====");
  DLL_Log("[VideoEncoder] encoder=%s", savedConfig.encoder.c_str());
  DLL_Log("[VideoEncoder] fps=%d", savedConfig.fps);
  DLL_Log("[VideoEncoder] preset=%s", savedConfig.preset.c_str());
  DLL_Log("[VideoEncoder] tuning=%s", savedConfig.tuning.c_str());
  DLL_Log("[VideoEncoder] rate_control=%s", savedConfig.rateControl.c_str());
  DLL_Log("[VideoEncoder] bitrate=%s", savedConfig.bitrate.c_str());
  DLL_Log("[VideoEncoder] max_bitrate=%s", savedConfig.maxBitrate.c_str());
  DLL_Log("[VideoEncoder] profile=%s", savedConfig.profile.c_str());
  DLL_Log("[VideoEncoder] lookahead=%s",
          savedConfig.lookahead ? "true" : "false");
  DLL_Log("[VideoEncoder] aq=%s", savedConfig.aq ? "true" : "false");
  DLL_Log("[VideoEncoder] b_frames=%d", savedConfig.bFrames);
  DLL_Log("[VideoEncoder] multipass=%s", savedConfig.multipass.c_str());
  DLL_Log("[VideoEncoder] keyframe_interval=%d", savedConfig.keyframeInterval);
  DLL_Log("[VideoEncoder] qp=%d", savedConfig.qp);
  DLL_Log("[VideoEncoder] bit_depth=%s color_space=%s color_range=%s chroma=%s",
          savedConfig.bitDepth.c_str(), savedConfig.colorSpace.c_str(),
          savedConfig.colorRange.c_str(), savedConfig.chromaSubsampling.c_str());
  DLL_Log("[VideoEncoder] ==============================================");

  // Check encoder type for option compatibility
  bool isAv1 = (savedConfig.encoder.find("av1") != std::string::npos);
  bool isMF = (savedConfig.encoder.find("_mf") != std::string::npos);
  bool isNVENC = (savedConfig.encoder.find("_nvenc") != std::string::npos);

  // Apply preset (p1-p7 for NVENC, speed/quality for AMF/QSV - NOT for MF)
  if (!isMF && !savedConfig.preset.empty()) {
    av_dict_set(&opts, "preset", savedConfig.preset.c_str(), 0);
  }

  // Apply tuning (NVENC only)
  if (isNVENC && !savedConfig.tuning.empty()) {
    av_dict_set(&opts, "tune", savedConfig.tuning.c_str(), 0);
  }

  // Set color properties from config (with auto-detection defaults)
  // Color space
  std::string cs = savedConfig.colorSpace;
  if (cs == "auto" || cs.empty()) {
    cs = currentIsHDR ? "bt2020" : "bt709";
  }
  if (cs == "bt2020") {
    codecCtx->color_primaries = AVCOL_PRI_BT2020;
    codecCtx->color_trc =
        currentIsHDR ? AVCOL_TRC_SMPTE2084 : AVCOL_TRC_BT2020_10;
    codecCtx->colorspace = AVCOL_SPC_BT2020_NCL;
  } else {
    codecCtx->color_primaries = AVCOL_PRI_BT709;
    codecCtx->color_trc = AVCOL_TRC_BT709;
    codecCtx->colorspace = AVCOL_SPC_BT709;
  }

  // Color range
  std::string cr = savedConfig.colorRange;
  if (cr == "auto" || cr.empty()) {
    codecCtx->color_range = AVCOL_RANGE_MPEG; // TV/limited is standard
  } else if (cr == "full") {
    codecCtx->color_range = AVCOL_RANGE_JPEG;
  } else {
    codecCtx->color_range = AVCOL_RANGE_MPEG;
  }

  // Bit depth and chroma subsampling → pixel format
  std::string bd = savedConfig.bitDepth;
  if (bd == "auto" || bd.empty()) {
    bd = currentIsHDR ? "10" : "8";
  }
  std::string chroma = savedConfig.chromaSubsampling;
  if (chroma == "auto" || chroma.empty()) {
    chroma = "420";
  }

  bool use10bit = (bd == "10");
  if (chroma == "444") {
    codecCtx->pix_fmt = use10bit ? AV_PIX_FMT_YUV444P10LE : AV_PIX_FMT_YUV444P;
  } else if (chroma == "422") {
    codecCtx->pix_fmt = use10bit ? AV_PIX_FMT_YUV422P10LE : AV_PIX_FMT_YUV422P;
  } else {
    // 4:2:0 (default) - use HW-accelerated formats when possible
    if (use10bit) {
      codecCtx->pix_fmt = AV_PIX_FMT_P010;
    } else {
      codecCtx->pix_fmt = AV_PIX_FMT_D3D11; // NV12 via D3D11 HW path
    }
  }

  DLL_Log("[VideoEncoder] Color config: space=%s range=%s bitDepth=%s chroma=%s "
          "pixFmt=%d hdr=%d",
          cs.c_str(), cr.c_str(), bd.c_str(), chroma.c_str(),
          codecCtx->pix_fmt, currentIsHDR);

  // Apply rate control mode
  if (!isMF && !savedConfig.rateControl.empty()) {
    std::string rc = savedConfig.rateControl;
    if (rc == "VBR")
      rc = "vbr";
    else if (rc == "CBR")
      rc = "cbr";
    else if (rc == "CQ" || rc == "CQP" || rc == "constqp")
      rc = "constqp";
    av_dict_set(&opts, "rc", rc.c_str(), 0);

    if (isNVENC &&
        (savedConfig.rateControl == "CQ" || savedConfig.rateControl == "CQP")) {
      av_dict_set_int(&opts, "qp", savedConfig.qp, 0);
      DLL_Log("[VideoEncoder] Applied NVENC qp=%d for CQ mode", savedConfig.qp);
    }
  }

  // Bitrate and max bitrate
  if (!savedConfig.bitrate.empty()) {
    std::string br = savedConfig.bitrate;
    int64_t bitrate_val = 0;
    if (br.find("Mbps") != std::string::npos)
      bitrate_val = std::stoll(br.substr(0, br.find("Mbps"))) * 1000000;
    else if (br.find("Kbps") != std::string::npos)
      bitrate_val = std::stoll(br.substr(0, br.find("Kbps"))) * 1000;
    else
      try {
        bitrate_val = std::stoll(br);
      } catch (...) {
      }
    if (bitrate_val > 0)
      codecCtx->bit_rate = bitrate_val;
  }

  if (!savedConfig.maxBitrate.empty()) {
    std::string maxbr = savedConfig.maxBitrate;
    int64_t maxbitrate_val = 0;
    if (maxbr.find("Mbps") != std::string::npos)
      maxbitrate_val =
          std::stoll(maxbr.substr(0, maxbr.find("Mbps"))) * 1000000;
    else if (maxbr.find("Kbps") != std::string::npos)
      maxbitrate_val = std::stoll(maxbr.substr(0, maxbr.find("Kbps"))) * 1000;
    else
      try {
        maxbitrate_val = std::stoll(maxbr);
      } catch (...) {
      }
    if (maxbitrate_val > 0)
      codecCtx->rc_max_rate = maxbitrate_val;
  }

  if (isNVENC) {
    av_dict_set(&opts, "rc-lookahead", savedConfig.lookahead ? "32" : "0", 0);
    if (!isAv1 && savedConfig.aq) {
      av_dict_set(&opts, "spatial-aq", "1", 0);
      av_dict_set(&opts, "temporal-aq", "1", 0);
    }
    if (!savedConfig.multipass.empty() && savedConfig.multipass != "disabled") {
      av_dict_set(&opts, "multipass", savedConfig.multipass.c_str(), 0);
    }
  }

  codecCtx->max_b_frames =
      (luidLow == 0 && luidHigh == 0) ? 0 : savedConfig.bFrames;
  if (savedConfig.keyframeInterval > 0) {
    codecCtx->gop_size = savedConfig.fps * savedConfig.keyframeInterval;
  }

  if (!isMF) {
    std::string profileToUse = savedConfig.profile;
    if (profileToUse == "auto" || profileToUse.empty()) {
      bool isH264 = savedConfig.encoder.find("264") != std::string::npos;
      bool isHEVC = savedConfig.encoder.find("hevc") != std::string::npos ||
                    savedConfig.encoder.find("265") != std::string::npos;
      if (isH264)
        profileToUse = use10bit ? "high10" : "high";
      else if (isHEVC)
        profileToUse = use10bit ? "main10" : "main";
    }
    if (!profileToUse.empty() && !isAv1)
      av_dict_set(&opts, "profile", profileToUse.c_str(), 0);
  }

  if (isMF) {
    if (!savedConfig.mfRateControl.empty())
      av_dict_set(&opts, "rate_control", savedConfig.mfRateControl.c_str(), 0);
    if (savedConfig.mfQuality >= 0 && savedConfig.mfQuality <= 100)
      av_dict_set_int(&opts, "quality", savedConfig.mfQuality, 0);
    if (!savedConfig.mfScenario.empty())
      av_dict_set(&opts, "scenario", savedConfig.mfScenario.c_str(), 0);
    av_dict_set_int(&opts, "hw_encoding", savedConfig.mfHwEncoding ? 1 : 0, 0);
  }

  if (savedConfig.useVFR) {
    codecCtx->time_base = {1, 1000000};
    codecCtx->framerate = {savedConfig.fps, 1};
  } else {
    codecCtx->time_base = {1, savedConfig.fps};
    codecCtx->framerate = {savedConfig.fps, 1};
  }

  codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  DLL_Log("[VideoEncoder] Opening Codec with options...");
  int ret = avcodec_open2(codecCtx, codec, &opts);
  if (opts)
    av_dict_free(&opts);

  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
    DLL_Log("[VideoEncoder] Failed to open codec: %d. Error details: %s", ret,
            errbuf);
    codecOpenFailed = true;
    return false;
  }

  DLL_Log("[VideoEncoder] Codec Opened Successfully.");
  stream = avformat_new_stream(fmtCtx, codec);
  avcodec_parameters_from_context(stream->codecpar, codecCtx);
  stream->time_base = codecCtx->time_base;
  stream->avg_frame_rate = codecCtx->framerate;
  stream->r_frame_rate = codecCtx->framerate;

  for (auto &actx : audioContexts) {
    if (actx.codecCtx) {
      actx.streamIndex = AddAudioStream(actx.config, actx.codecCtx);
      if (actx.streamIndex >= 0 && audioStreamIndex < 0)
        audioStreamIndex = actx.streamIndex;
    }
  }

  initDone = true;
  return true;
}

bool VideoEncoder::EnsureDevice() {
  if (initDone)
    return true;

  // Don't retry if codec already failed - prevents infinite loop and device
  // leak
  if (codecOpenFailed) {
    return false;
  }

  DLL_Log("[VideoEncoder] EnsureDevice with LUID: %08x %08x", luidLow,
          luidHigh);

  if (luidLow == 0 && luidHigh == 0) {
    DLL_Log(
        "[VideoEncoder] WARNING: EnsureDevice called with default LUID (0:0). "
        "In inject mode, this usually indicates a propagation failure.");
  }

  // D3D11 Video Processor is the only supported color conversion path
  // (D3D12 does not have an equivalent VideoProcessorBlt API)

  // 1. Find Adapter by LUID
  IDXGIAdapter *targetAdapter = nullptr;
  if (luidLow != 0 || luidHigh != 0) {
    LUID searchLuid;
    searchLuid.LowPart = (DWORD)luidLow;
    searchLuid.HighPart = (LONG)luidHigh;

    DLL_Log("[VideoEncoder] Searching for Adapter with LUID: %08x-%08x",
            searchLuid.HighPart, searchLuid.LowPart);

    IDXGIFactory4 *factory4 = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory4)))) {
      if (SUCCEEDED(factory4->EnumAdapterByLuid(
              searchLuid, IID_PPV_ARGS(&targetAdapter)))) {
        DLL_Log("[VideoEncoder] Found Adapter matching LUID via IDXGIFactory4");
      }
      factory4->Release();
    }

    if (!targetAdapter) {
      IDXGIFactory1 *factory = nullptr;
      if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        IDXGIAdapter *adapter = nullptr;
        for (UINT i = 0;
             factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
          DXGI_ADAPTER_DESC desc;
          adapter->GetDesc(&desc);
          if (desc.AdapterLuid.LowPart == searchLuid.LowPart &&
              desc.AdapterLuid.HighPart == searchLuid.HighPart) {
            targetAdapter = adapter;
            DLL_Log(
                "[VideoEncoder] Found Adapter matching LUID via manual scan");
            break;
          }
          adapter->Release();
        }
        factory->Release();
      }
    }
  }

  // 1. Create D3D11 Device Manually
  // For screengrab mode (LUID=0), we use the shared device created in
  // MediaEngine_GetD3D11Device This ensures ScreenCapture and VideoEncoder
  // share the same device for CopyResource compatibility

  // Declare these for extern access to shared device from mediaengine.cpp
  extern ID3D11Device *g_SharedD3D11Device;
  extern ID3D11DeviceContext *g_SharedD3D11Context;

  if ((luidLow == 0 && luidHigh == 0) && g_SharedD3D11Device) {
    // Framegrab mode - use the shared device that ScreenCapture also uses
    DLL_Log("[VideoEncoder] Framegrab using dimensions: %dx%d", width, height);
    g_SharedD3D11Device->QueryInterface(IID_PPV_ARGS(&d3d11Device));
    g_SharedD3D11Context->QueryInterface(IID_PPV_ARGS(&d3d11Context));
    DLL_Log("[VideoEncoder] Using shared D3D11 device for framegrab");
  } else {
    // Inject mode - create device on specific adapter
    DLL_Log("[VideoEncoder] Creating D3D11 Device (Flags: BGRA + VIDEO)...");

    UINT createDeviceFlags =
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
// createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG; // Optional
#endif

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL featureLevel;
    ID3D11Device *baseDevice = nullptr;
    ID3D11DeviceContext *baseContext = nullptr;

    HRESULT hr = D3D11CreateDevice(
        targetAdapter,
        targetAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, 0,
        createDeviceFlags, featureLevels, ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION, &baseDevice, &featureLevel, &baseContext);

    if (targetAdapter)
      targetAdapter->Release();

    if (FAILED(hr)) {
      DLL_Log("[VideoEncoder] D3D11CreateDevice Failed: 0x%x (Target: %p)", hr,
              targetAdapter);
      return false;
    }
    DLL_Log("[VideoEncoder] D3D11 Device Created (Feature Level: 0x%x)",
            featureLevel);

    // Use RAII to prevent leaks on error paths
    ce::ComGuard<ID3D11Device> baseDeviceGuard(baseDevice);
    ce::ComGuard<ID3D11DeviceContext> baseContextGuard(baseContext);

    // QI for Interfaces
    if (FAILED(baseDevice->QueryInterface(IID_PPV_ARGS(&d3d11Device)))) {
      return false;
    }

    if (FAILED(baseContext->QueryInterface(IID_PPV_ARGS(&d3d11Context)))) {
      return false;
    }

    // 2. Wrap in AVHWDeviceContext
    d3d11DeviceCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!d3d11DeviceCtx)
      return false;

    AVHWDeviceContext *deviceCtx = (AVHWDeviceContext *)d3d11DeviceCtx->data;
    AVD3D11VADeviceContext *d3d11Ctx =
        (AVD3D11VADeviceContext *)deviceCtx->hwctx;
    d3d11Ctx->device = baseDevice;
    baseDevice->AddRef();

    if (av_hwdevice_ctx_init(d3d11DeviceCtx) < 0)
      return false;

    // baseDeviceGuard and baseContextGuard will auto-release on scope exit
  } // End of else block (inject mode device creation)

  // Apply GPU priority from config to ensure encoder/game balance
  if (d3d11Device) {
    IDXGIDevice *dxgiDevice = nullptr;
    if (SUCCEEDED(d3d11Device->QueryInterface(__uuidof(IDXGIDevice),
                                              (void **)&dxgiDevice))) {
      // Range -7 to 7 (DXGI_GPU_THREAD_PRIORITY_MAX/MIN)
      // Positive = higher than game, negative = lower than game
      // Value comes from config.ini [Performance] gpu_priority
      int priority = gpuPriority; // Passed to Init() from config

      // Clamp to valid range
      if (priority < -7)
        priority = -7;
      if (priority > 7)
        priority = 7;

      HRESULT phr = dxgiDevice->SetGPUThreadPriority(priority);
      if (SUCCEEDED(phr)) {
        DLL_Log("[VideoEncoder] Set GPU Thread Priority to %d", priority);
      } else {
        DLL_Log("[VideoEncoder] Failed to set GPU Thread Priority: HR=%x", phr);
      }
      dxgiDevice->Release();
    }
  }

  // Set up FFmpeg HW device context with our D3D11 device (shared for both
  // paths)
  if (!d3d11DeviceCtx) {
    // 2. Wrap in AVHWDeviceContext - for screengrab mode using shared device
    d3d11DeviceCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!d3d11DeviceCtx)
      return false;

    AVHWDeviceContext *deviceCtx = (AVHWDeviceContext *)d3d11DeviceCtx->data;
    AVD3D11VADeviceContext *d3d11Ctx =
        (AVD3D11VADeviceContext *)deviceCtx->hwctx;

    // Get base device from our QI'd interface
    ce::ComGuard<ID3D11Device> baseDevice;
    if (FAILED(d3d11Device->QueryInterface(__uuidof(ID3D11Device),
                                           (void **)baseDevice.addressof()))) {
      return false;
    }

    d3d11Ctx->device = baseDevice.get();
    // FFmpeg expects to own a reference, so we AddRef.
    // The ComPtr will release our local reference when it goes out of scope.
    baseDevice->AddRef();

    if (av_hwdevice_ctx_init(d3d11DeviceCtx) < 0) {
      // If init fails, we rely on ComPtr to release baseDevice.
      // We also need to clean up the partially created context.
      av_buffer_unref(&d3d11DeviceCtx);
      return false;
    }
    // baseDevice releases its ref here, but FFmpeg holds one via AddRef above.
  }

  codecCtx->hw_device_ctx = av_buffer_ref(d3d11DeviceCtx);

  // 3. D3D11 Frames Context
  d3d11FramesCtx = av_hwframe_ctx_alloc(d3d11DeviceCtx);
  AVHWFramesContext *d11Frames = (AVHWFramesContext *)d3d11FramesCtx->data;
  d11Frames->format = AV_PIX_FMT_D3D11;
  d11Frames->sw_format = AV_PIX_FMT_NV12;

  int framesWidth = width;
  int framesHeight = height;
  if (savedConfig.scaling.enabled && savedConfig.scaling.outputWidth > 0 &&
      savedConfig.scaling.outputHeight > 0) {
    framesWidth = savedConfig.scaling.outputWidth;
    framesHeight = savedConfig.scaling.outputHeight;
  }

  d11Frames->width = framesWidth;
  d11Frames->height = framesHeight;
  d11Frames->initial_pool_size = 0;

  if (av_hwframe_ctx_init(d3d11FramesCtx) < 0) {
    DLL_Log("[VideoEncoder] Failed to init D3D11 frames context");
    return false;
  }
  codecCtx->hw_frames_ctx = av_buffer_ref(d3d11FramesCtx);
  codecCtx->extra_hw_frames = 5;
  codecCtx->width = framesWidth;
  codecCtx->height = framesHeight;

  return ConfigureAndOpenCodec();
}

int VideoEncoder::AddAudioStream(const AudioConfig &config,
                                 AVCodecContext *audioCtx) {
  if (!fmtCtx)
    return -1;

  const AVCodec *codec = nullptr;
  if (audioCtx) {
    codec = audioCtx->codec;
  } else {
    codec = avcodec_find_encoder_by_name(
        config.codec.empty() ? "aac" : config.codec.c_str());
  }

  if (!codec)
    return -1;
  AVStream *st = avformat_new_stream(fmtCtx, codec);
  if (!st)
    return -1;

  if (audioCtx) {
    // Correct way: copy parameters including extradata
    avcodec_parameters_from_context(st->codecpar, audioCtx);
    int sampleRate =
        audioCtx->sample_rate > 0 ? audioCtx->sample_rate
                                  : st->codecpar->sample_rate;
    if (sampleRate <= 0) {
      sampleRate = 48000;
    }
    st->time_base = {1, sampleRate};
  } else {
    // Fallback (might fail for extradata-dependent codecs)
    int sampleRate =
        (!config.sampleRate.empty() && config.sampleRate != "default")
            ? std::stoi(config.sampleRate)
            : 48000;
    st->time_base = {1, sampleRate};
    st->codecpar->codec_id = codec->id;
    st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    st->codecpar->sample_rate = sampleRate;
    st->codecpar->ch_layout.nb_channels = 2;
  }
  return st->index;
}

void VideoEncoder::SetAudioContext(const AudioConfig &config,
                                   AVCodecContext *audioCtx) {
  savedAudioConfig = config;
  savedAudioCodecCtx = audioCtx;

  // Also add to multi-source array for compatibility
  // Clear previous contexts first (SetAudioContext is for single-source mode)
  audioContexts.clear();

  AudioStreamContext ctx;
  ctx.config = config;
  ctx.codecCtx = audioCtx;
  ctx.track = config.tracks.empty() ? 0 : config.tracks[0];
  ctx.streamIndex = -1;
  audioContexts.push_back(ctx);
}

int VideoEncoder::AddAudioContext(const AudioConfig &config,
                                  AVCodecContext *audioCtx, int track) {
  AudioStreamContext ctx;
  ctx.config = config;
  ctx.codecCtx = audioCtx;
  ctx.track = track;
  ctx.streamIndex = -1;
  audioContexts.push_back(ctx);

  DLL_Log("[VideoEncoder] AddAudioContext: track=%d, total=%d", ctx.track,
          (int)audioContexts.size());

  return ctx.track;
}

void VideoEncoder::ClearAudioContexts() {
  audioContexts.clear();
  audioStreamIndex = -1;
}

int VideoEncoder::GetAudioStreamIndex(int track) const {
  // Backward compatible: track -1 returns first stream index
  if (track < 0) {
    if (!audioContexts.empty()) {
      return audioContexts[0].streamIndex;
    }
    return audioStreamIndex;
  }

  // Find stream index for specific track
  for (const auto &ctx : audioContexts) {
    if (ctx.track == track) {
      return ctx.streamIndex;
    }
  }

  return -1;
}

bool VideoEncoder::Start() {
  // Ensure previous recording is fully finalized and resources cleaned up.
  // Stop() will signal the async finalize if needed, then we wait for it to
  // finish.
  Stop();
  if (writerThread.joinable()) {
    DLL_Log(
        "[VideoEncoder] Start: Waiting for previous recording to finalize...");
    writerThread.join();
  }

  // If fmtCtx was freed by Stop(), recreate it for the new recording
  // If fmtCtx was freed by Stop(), recreate it for the new recording
  if (!fmtCtx) {
    // Generate new output filename using robust helper
    outputFilename = GenerateOutputFilename(savedConfig);
    DLL_Log("[VideoEncoder] Creating new format context for: %s",
            outputFilename.c_str());

    if (avformat_alloc_output_context2(&fmtCtx, nullptr, nullptr,
                                       outputFilename.c_str()) < 0) {
      DLL_Log("[VideoEncoder] Failed to allocate new format context");
      return false;
    }
  }

  // If codecCtx was freed by Stop(), recreate it
  if (!codecCtx) {
    const AVCodec *codec =
        avcodec_find_encoder_by_name(savedConfig.encoder.c_str());
    if (!codec) {
      DLL_Log("[VideoEncoder] Codec not found: %s",
              savedConfig.encoder.c_str());
      return false;
    }

    codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) {
      DLL_Log("[VideoEncoder] Failed to alloc new codec context");
      return false;
    }

    codecCtx->width = width;
    codecCtx->height = height;

    // Apply configured pixel format
    std::string bd = savedConfig.bitDepth;
    if (bd == "auto" || bd.empty())
      bd = currentIsHDR ? "10" : "8";
    std::string chroma = savedConfig.chromaSubsampling;
    if (chroma == "auto" || chroma.empty())
      chroma = "420";
    bool use10bit = (bd == "10");
    if (chroma == "444")
      codecCtx->pix_fmt = use10bit ? AV_PIX_FMT_YUV444P10LE : AV_PIX_FMT_YUV444P;
    else if (chroma == "422")
      codecCtx->pix_fmt = use10bit ? AV_PIX_FMT_YUV422P10LE : AV_PIX_FMT_YUV422P;
    else
      codecCtx->pix_fmt = use10bit ? AV_PIX_FMT_P010 : AV_PIX_FMT_D3D11;

    DLL_Log("[VideoEncoder] Recreated codec context for new recording");

    if (d3d11FramesCtx) {
      codecCtx->hw_frames_ctx = av_buffer_ref(d3d11FramesCtx);
      codecCtx->extra_hw_frames = 5;
    }
  }

  // Reset flags that block recording if previous recording had issues
  codecOpenFailed = false;
  encodedDurationUs.store(0, std::memory_order_relaxed);
  lastAssignedVideoPts = -1;

  // Pre-warm device and codec to reduce first-frame latency
  // This moves heavy initialization (D3D11 device, codec open, video processor)
  // from first frame to Start() call, avoiding game stutter on recording start
  // IMPORTANT: Only pre-warm if we already have valid dimensions from common
  // discovery
  if ((luidLow != 0 || luidHigh != 0) && width > 0 && height > 0 && !initDone) {
    DLL_Log("[VideoEncoder] Pre-warming device and codec (%dx%d)...", width,
            height);
    auto prewarmStart = PerfTimer::now();

    if (!EnsureDevice()) {
      DLL_Log("[VideoEncoder] Pre-warm failed, will retry on first frame");
    } else {
      auto prewarmEnd = PerfTimer::now();
      double prewarmMs = PerfTimer::elapsed_ms(prewarmStart, prewarmEnd);
      DLL_Log("[VideoEncoder] Pre-warm complete in %.2fms (device init, codec "
              "open)",
              prewarmMs);
    }
  }

  recordingRequested = true;
  DLL_Log("[VideoEncoder] Start Recording Requested (Deferred).");

  // Start Async Allocator Thread
  if (!writerRunning) {
    writerRunning = true;
    writerThread = std::thread(&VideoEncoder::AsyncWriteLoop, this);
    DLL_Log("[VideoEncoder] Started Writer Thread");
  }

  return true;
}

void VideoEncoder::WriteFrame(AVPacket *pkt) {
  if (!fileOpened || !fmtCtx)
    return;

  // Rescale timestamps from codec time_base to stream time_base
  AVStream *st = fmtCtx->streams[pkt->stream_index];
  AVRational codec_tb;

  if (pkt->stream_index == stream->index) {
    // Video packet - use video codec time_base
    codec_tb = codecCtx->time_base;
  } else {
    // Audio packet - audio time_base is typically 1/sample_rate
    // The audio encoder uses PTS = sample_count, so time_base is {1,
    // sample_rate}
    codec_tb = {1, st->codecpar->sample_rate};

    // Debug: log audio packet writing
    static int audioPacketCount = 0;

    // Reset counter on new recording (detected by low PTS - first frame has
    // PTS near 0 or < 10000)
    if (pkt->pts < 10000) {
      audioPacketCount = 0;
    }

    if (audioPacketCount++ % 100 == 0) {
      DLL_Log("[VideoEncoder] Queuing audio pkt #%d size=%d pts=%lld "
              "stream_idx=%d",
              audioPacketCount, pkt->size, pkt->pts, pkt->stream_index);
    }
  }

  // Debug: log first few video packets to verify PTS
  static int videoPacketCount = 0;

  // Reset counter on new recording (detected by low PTS)
  if (pkt->stream_index == stream->index && pkt->pts < 10) {
    videoPacketCount = 0;
  }

  if (pkt->stream_index == stream->index && videoPacketCount++ < 5) {
    DLL_Log("[VideoEncoder] Queuing video pkt #%d: pts=%lld codec_tb=%d/%d "
            "st_tb=%d/%d",
            videoPacketCount, pkt->pts, codec_tb.num, codec_tb.den,
            st->time_base.num, st->time_base.den);
  }

  // Rescale timestamps properly using FFmpeg's exact rational math
  av_packet_rescale_ts(pkt, codec_tb, st->time_base);
  if (pkt->stream_index == stream->index && pkt->dts == AV_NOPTS_VALUE) {
    pkt->dts = pkt->pts;
  }

  // DEBUG: Log PTS after rescaling and detect corruption
  if (pkt->stream_index == stream->index) {
    static int vidDebugCount = 0;
    // Reset on each new recording (PTS restarts from 0)
    if (pkt->pts < 10) {
      vidDebugCount = 0;
    }
    if (vidDebugCount++ < 20 || pkt->pts < 0) {
      DLL_Log(
          "[VideoEncoder] PTS PRECISE: frameNum=%lld pts_ms=%lld st_tb=%d/%d",
          pkt->pts, pkt->pts, st->time_base.num, st->time_base.den);
    }

    // DEBUG LEAK: Log queue stats every 100 video frames
    if (vidDebugCount % 100 == 0) {
      size_t qBytes = currentQueueBytes.load();
      size_t qSize = 0;
      {
        std::lock_guard<std::mutex> lock(queueMutex);
        qSize = packetQueue.size();
      }
      DLL_Log("[VideoEncoder] QUEUE STATS: Count=%zu Bytes=%zu (Max=%zu)",
              qSize, qBytes, MAX_QUEUE_BYTES);

      // Memory safety check
      if (qBytes > MAX_QUEUE_BYTES) {
        DLL_Log(
            "[VideoEncoder] CRITICAL: Queue exceeds limit! Dropping disabled?");
      }
    }
  }

  // CRITICAL: For video packets, explicitly set duration after rescaling
  // The MKV muxer or interleaved write may compute wrong duration for first
  // packet. Duration should be calculated from actual configured FPS.
  if (pkt->stream_index == stream->index) {
    // Set duration to exactly 1 frame in stream time_base
    // For video with stream time_base 1/1000: duration = 1000/fps
    int fps = codecCtx->framerate.num;
    if (fps > 0) {
      // stream time_base is typically 1/1000, so duration = 1000/fps
      pkt->duration = st->time_base.den / fps;
    } else {
      // Fallback to 60fps if framerate not set
      pkt->duration = st->time_base.den / 60;
    }
  }

  // Track authoritative encoded video duration from packet timeline.
  if (pkt->stream_index == stream->index) {
    int64_t packetPts =
        (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
    if (packetPts != AV_NOPTS_VALUE) {
      int64_t packetDuration = pkt->duration;
      if (packetDuration <= 0) {
        packetDuration = av_rescale_q(1, codec_tb, st->time_base);
        if (packetDuration <= 0) {
          packetDuration = 1;
        }
      }
      int64_t packetEnd = packetPts + packetDuration;
      int64_t packetEndUs =
          av_rescale_q(packetEnd, st->time_base, AVRational{1, 1000000});
      int64_t prevEndUs = encodedDurationUs.load(std::memory_order_relaxed);
      if (packetEndUs > prevEndUs) {
        encodedDurationUs.store(packetEndUs, std::memory_order_relaxed);
      }
    }
  }

  // ASYNC WRITE: Push to queue instead of writing directly

  // IMPORTANT: Never drop encoded packets, it causes visible corruption.
  // Instead apply backpressure to the encode thread.
  // If storage is extremely slow, this will manifest as stutter/dropped input
  // frames (FrameQueue will drop/duplicate), but the bitstream stays valid.
  for (;;) {
    size_t qBytes = currentQueueBytes.load(std::memory_order_relaxed);
    if (qBytes <= MAX_QUEUE_BYTES) {
      break;
    }

    lastMuxOverloadTickMs.store(GetTickCount64(), std::memory_order_relaxed);
    PublishRuntimeState();

    static int overloadLogCount = 0;
    if (overloadLogCount++ % 60 == 0) {
      DLL_Log("[VideoEncoder] WARNING: Packet queue overloaded (%zu bytes) - "
              "applying backpressure",
              qBytes);
    }

    // Wait briefly for writer to drain.
    std::unique_lock<std::mutex> lock(queueMutex);
    queueCV.wait_for(lock, std::chrono::milliseconds(2), [this] {
      return currentQueueBytes.load(std::memory_order_relaxed) <=
                 MAX_QUEUE_BYTES ||
             isStopping || !writerRunning;
    });
    if (isStopping || !writerRunning) {
      break;
    }
  }

  AVPacket *clonePkt = av_packet_clone(pkt);
  if (clonePkt) {
    {
      std::lock_guard<std::mutex> lock(queueMutex);
      packetQueue.push(clonePkt);
      currentQueueBytes += clonePkt->size + sizeof(AVPacket);
    }
    PublishRuntimeState();
    queueCV.notify_one();
  }
}

void VideoEncoder::PublishRuntimeState() {
  if (!pSharedMem) {
    return;
  }

  // Keep this cheap and lock-free: only atomics.
  uint32_t flags = 0;
  uint64_t nowMs = GetTickCount64();

  constexpr uint64_t kOverloadHoldMs = 1000;
  uint64_t encTick = lastEncoderOverloadTickMs.load(std::memory_order_relaxed);
  uint64_t muxTick = lastMuxOverloadTickMs.load(std::memory_order_relaxed);

  if (encTick != 0 && (nowMs - encTick) <= kOverloadHoldMs) {
    flags |= 1u;
  }
  if (muxTick != 0 && (nowMs - muxTick) <= kOverloadHoldMs) {
    flags |= 2u;
  }

  pSharedMem->runtimeState.encoderOverloadFlags.store(
      flags, std::memory_order_relaxed);

  size_t qBytes = currentQueueBytes.load(std::memory_order_relaxed);
  uint32_t qBytes32 = (qBytes > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)qBytes;
  pSharedMem->runtimeState.muxQueueBytes.store(qBytes32,
                                               std::memory_order_relaxed);
}

bool VideoEncoder::EncodeFrame(HANDLE sharedHandle, HANDLE fenceHandle,
                               uint64_t fenceValue, int64_t timestamp,
                               uint32_t sourcePid, int width, int height,
                               int format, bool isHDR, bool isShmem,
                               int shmemSlot) {
  if (!recordingRequested)
    return false;

  // Debug: Log every 60th frame entry to verify loop
  if (encodeFrameCounter % 60 == 0) {
    DLL_Log("[VideoEncoder] EncodeFrame Entry: PID=%u Handle=%p FenceVal=%llu",
            sourcePid, sharedHandle, fenceValue);
  }

  if (!initDone || isHDR != currentIsHDR) {
    if (initDone) {
      DLL_Log("[VideoEncoder] HDR Mode changed (New=%d, Old=%d). "
              "Re-initializing...",
              isHDR, currentIsHDR);
      Stop(); // Clean up existing encoder
      initDone = false;
      // Also need to clear codecOpenFailed?
      codecOpenFailed = false;
    }

    currentIsHDR = isHDR;
    // Re-Init with saved config (Init uses currentIsHDR to pick format)
    if (!Init(savedConfig, width, height,
              savedConfig.fps ? savedConfig.fps : 60, onPacket)) {
      DLL_Log("[VideoEncoder] Failed to Re-Init for HDR change");
      return false;
    }
  }

  // Use captured frame dimensions if not yet set or changed
  if (this->width != width || this->height != height) {
    if (this->width == 0) {
      DLL_Log(
          "[VideoEncoder] Initial resolution discovered: %dx%d (Input: %dx%d)",
          width, height, width, height);
    } else {
      DLL_Log("[VideoEncoder] Resolution CHANGE detected: %dx%d -> %dx%d",
              this->width, this->height, width, height);
      // For now we might need to recreate the encoder, but let's at least
      // update the variables
    }
    this->width = width;
    this->height = height;
  }

  if (!EnsureDevice())
    return false;

#ifdef HAS_CUDA
  if (useCudaPath) {
    return EncodeFrameCuda(sharedHandle, fenceValue, timestamp, sourcePid,
                           width, height);
  }
#endif
  // Fall through to D3D11 path below

  if (!fileOpened) {
    DLL_Log("[VideoEncoder] Opening Output File: %s", outputFilename.c_str());
    if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
      // Use 256KB buffer for better performance on slow storage (HDD/network)
      // Default is 32KB which causes many small writes
      int ret = avio_open2(&fmtCtx->pb, outputFilename.c_str(), AVIO_FLAG_WRITE,
                           nullptr, nullptr);
      if (ret < 0) {
        DLL_Log("Failed to open output file: %d", ret);
        return false;
      }

      // Allocate custom buffer (256KB) for improved write performance
      const int bufferSize = 256 * 1024;
      [[maybe_unused]] unsigned char *buffer = nullptr;
    }

    // Debug: Log stream info before write_header
    DLL_Log("[VideoEncoder] fmtCtx has %d streams before write_header",
            fmtCtx->nb_streams);
    for (unsigned int i = 0; i < fmtCtx->nb_streams; i++) {
      AVStream *s = fmtCtx->streams[i];
      AVCodecParameters *cp = s->codecpar;
      DLL_Log("[VideoEncoder] Stream %d: type=%d codec_id=%d w=%d h=%d "
              "extradata=%p extradata_size=%d",
              i, cp->codec_type, cp->codec_id, cp->width, cp->height,
              cp->extradata, cp->extradata_size);
    }

    int ret = avformat_write_header(fmtCtx, nullptr);
    if (ret < 0) {
      char errbuf[256];
      av_strerror(ret, errbuf, sizeof(errbuf));
      DLL_Log("Failed to write header: %d (%s)", ret, errbuf);
      return false;
    }

    // Force header to hit disk immediately. This prevents 0KB files when
    // subsequent writes fail and makes I/O errors surface at the true failure
    // point.
    if (fmtCtx->pb) {
      avio_flush(fmtCtx->pb);
      if (fmtCtx->pb->error < 0) {
        DLL_Log("Failed to flush header: %d", fmtCtx->pb->error);
        if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
          avio_closep(&fmtCtx->pb);
        }
        return false;
      }
    }
    fileOpened = true;
  }

  // Frame rate control is now handled by capture engine (time-based sampling)
  // We just encode every frame we receive using frame counter for CFR output
  inputFrameCount++;

  // Log frame stats periodically (every 120 frames = ~1 sec at 120fps)
  // Detect new recording start (startPts is -1) and reset counters
  if (startPts < 0) {
    needsCounterReset = true; // Mark that we need to reset on first frame
  }

  if (inputFrameCount - lastLogFrameCount >= kFpsLogIntervalFrames) {
    if (startPts >= 0 && timestamp > startPts) {
      // timestamp and startPts are both in milliseconds
      double elapsedSec = (double)(timestamp - startPts) / 1000.0;
      double outputFps =
          (elapsedSec > 0.001) ? ((double)inputFrameCount / elapsedSec) : 0.0;
      DLL_Log("[FPS] Output: %.1f frames, %.1f fps over %.1fs",
              (double)inputFrameCount, outputFps, elapsedSec);
    }
    lastLogFrameCount = inputFrameCount;
  }

  // Reset counters on new recording
  if (needsCounterReset) {
    encodeFrameCounter = 0;
    lastLogFrameCount = 0;
    needsCounterReset = false;
    DLL_Log("[VideoEncoder] Reset frameCounter for new recording");
  }

  encodeFrameCounter++;

  // Performance timing for this frame
  FrameStats stats;
  stats.frameNumber = encodeFrameCounter;
  stats.ptsMs = timestamp;

  // Calculate frame timing for smoothness analysis
  double expectedFrameMs = 1000.0 / codecCtx->framerate.num;
  if (g_lastFramePts >= 0) {
    stats.actualPtsDiff = timestamp - g_lastFramePts;
    stats.expectedPtsDiff = (int64_t)expectedFrameMs;
  }
  g_lastFramePts = timestamp;

  auto frameStart = PerfTimer::now();

  ID3D11Texture2D *bgraTex = nullptr;
  ID3D11Fence *d3d11Fence = nullptr;
  int cacheSlot = -1;

  if (isShmem) {
    if (pShmem && pSharedMem && pSharedMem->GetShmemMappingCreated()) {
      // Shmem Path: Upload pixels to our owned texture
      int texIdx = 0; // Reuse first shared capture texture (we own it)
      bgraTex = sharedCaptureTextures[texIdx];

      if (bgraTex) {
        // Validation of slot
        int slot = (shmemSlot >= 0 && shmemSlot < 2) ? shmemSlot : 0;
        uint8_t *pSrc = pShmem->data[slot];

        if (pSrc) {
          D3D11_BOX box;
          box.left = 0;
          box.right = pSharedMem->GetWidth(); // Use current frame resolution
          box.top = 0;
          box.bottom = pSharedMem->GetHeight();
          box.front = 0;
          box.back = 1;

          // We need a pitch. Use pSharedMem->width * 4 if not stored in
          // ShmemBuffer Actually ShmemBuffer has pitch.
          d3d11Context->UpdateSubresource(bgraTex, 0, &box, pSrc, pShmem->pitch,
                                          0);
        }
        bgraTex->AddRef();    // For consistency with Release() below
        d3d11Fence = nullptr; // No fence for shmem
      }
    }
  } else {
    HANDLE sharedHandleAlt = NormalizeSourceHandleForWow64(sharedHandle, sourcePid);
    HANDLE fenceHandleAlt = NormalizeSourceHandleForWow64(fenceHandle, sourcePid);
    const bool hasSharedAlt = (sharedHandleAlt != sharedHandle);
    const bool hasFenceAlt = (fenceHandleAlt != fenceHandle);

    // Check cache for valid fence and texture (Quad-Buffered Cache)
    // Texture caching works independently of fence (for D3D11 KMT path)
    cacheSlot = -1;
    bool skipFence = (fenceValue == 0 || fenceHandle == 0 ||
                      fenceHandle == INVALID_HANDLE_VALUE);
    bool fenceValid =
        !skipFence && (sourcePid > 0 && sourcePid == cachedSourcePid &&
                       fenceHandle == cachedFenceHandle && cachedD3D11Fence);

    // For texture matching, we only need matching PID and handle
    // (fence-independent)
    bool pidMatches = (sourcePid > 0 && sourcePid == cachedSourcePid);

    // Search for cached texture by handle (works with or without fence)
    if (pidMatches) {
      for (int i = 0; i < 8; i++) {
        if (cachedTextureHandles[i] == sharedHandle &&
            cachedSharedTextures[i]) {
          cacheSlot = i;
          break;
        }
      }
    } else if (sourcePid > 0) {
      // New process -> Clear all cache
      for (int i = 0; i < 8; i++) {
        if (cachedSharedTextures[i]) {
          cachedSharedTextures[i]->Release();
          cachedSharedTextures[i] = nullptr;
        }
        cachedTextureHandles[i] = nullptr;
      }
      if (cachedD3D11Fence) {
        cachedD3D11Fence->Release();
        cachedD3D11Fence = nullptr;
      }
      cachedFenceHandle = nullptr;
      cachedSourcePid = sourcePid; // Remember new PID
    }

    // ID3D11Texture2D *bgraTex = nullptr; // Moved up
    // ID3D11Fence *d3d11Fence = nullptr;   // Moved up

    if (cacheSlot >= 0) {
      // Full Cache Hit
      bgraTex = cachedSharedTextures[cacheSlot];
      d3d11Fence =
          cachedD3D11Fence; // May be null for D3D11 KMT path (no fence)
      bgraTex->AddRef();
      if (d3d11Fence) {
        d3d11Fence->AddRef();
      }

      if (encodeFrameCounter % kCacheLogIntervalFrames == 1) {
        DLL_Log(
            "[VideoEncoder] Using cached handles (pid=%u, slot=%d, frame=%d)",
            sourcePid, cacheSlot, encodeFrameCounter);
      }
    } else {
      // Cache Miss (Partial or Full)
      // Use RAII to ensure handle is closed if we return early
      ce::HandleGuard hProcess(
          OpenProcess(PROCESS_DUP_HANDLE, FALSE, sourcePid));

      if (!hProcess) {
        DLL_Log("[VideoEncoder] Frame %d: Failed to Open Process %u",
                encodeFrameCounter, sourcePid);
        return false;
      }

      // 1. Handle Fence (Reuse if valid, Open if not)
      if (skipFence) {
        d3d11Fence = nullptr;
        if (encodeFrameCounter % 60 == 0)
          DLL_Log(
              "[VideoEncoder] Frame %d: SkipFence is true (Val=%llu Hnd=%p)",
              encodeFrameCounter, fenceValue, fenceHandle);
      } else if (fenceValid) {
        d3d11Fence = cachedD3D11Fence;
        d3d11Fence->AddRef();
      } else {
        ce::HandleGuard dupFence;
        HRESULT hr = E_FAIL;

        // Try NT handle path
        if (DuplicateHandle(hProcess.get(), fenceHandle, GetCurrentProcess(),
                            dupFence.addressof(), 0, FALSE,
                            DUPLICATE_SAME_ACCESS)) {
          hr = d3d11Device->OpenSharedFence(dupFence.get(),
                                            IID_PPV_ARGS(&d3d11Fence));
          if (FAILED(hr)) {
            DLL_Log("[VideoEncoder] OpenSharedFence failed: HR=%x (Hnd=%p)", hr,
                    dupFence.get());
          }
        } else {
          DWORD err = GetLastError();
          DLL_Log("[VideoEncoder] DuplicateHandle failed: Err=%d (SrcPid=%u "
                  "Hnd=%p)",
                  err, sourcePid, fenceHandle);
        }

        // Alternate handle representation for WOW64 sources
        if (FAILED(hr) && hasFenceAlt) {
          ce::HandleGuard dupFenceAlt;
          if (DuplicateHandle(hProcess.get(), fenceHandleAlt, GetCurrentProcess(),
                              dupFenceAlt.addressof(), 0, FALSE,
                              DUPLICATE_SAME_ACCESS)) {
            hr = d3d11Device->OpenSharedFence(dupFenceAlt.get(),
                                              IID_PPV_ARGS(&d3d11Fence));
            if (FAILED(hr)) {
              DLL_Log("[VideoEncoder] OpenSharedFence(alt) failed: HR=%x "
                      "(Hnd=%p)",
                      hr, dupFenceAlt.get());
            }
          } else {
            DWORD err = GetLastError();
            DLL_Log("[VideoEncoder] DuplicateHandle(alt) failed: Err=%d "
                    "(SrcPid=%u Hnd=%p)",
                    err, sourcePid, fenceHandleAlt);
          }
        }

        // Fallback/KMT path
        if (FAILED(hr)) {
          hr = d3d11Device->OpenSharedResource(fenceHandle,
                                               IID_PPV_ARGS(&d3d11Fence));
          if (FAILED(hr) && encodeFrameCounter < 10) {
            DLL_Log("[VideoEncoder] OpenSharedResource(Fence) failed: HR=%x",
                    hr);
          }
        }
        if (FAILED(hr) && hasFenceAlt) {
          hr = d3d11Device->OpenSharedResource(fenceHandleAlt,
                                               IID_PPV_ARGS(&d3d11Fence));
          if (FAILED(hr) && encodeFrameCounter < 10) {
            DLL_Log("[VideoEncoder] OpenSharedResource(Fence, alt) failed: "
                    "HR=%x",
                    hr);
          }
        }

        if (d3d11Fence) {
          DLL_Log("[VideoEncoder] Successfully opened shared fence for PID %u",
                  sourcePid);
          // Update Fence Cache
          if (cachedD3D11Fence)
            cachedD3D11Fence->Release();
          cachedD3D11Fence = d3d11Fence;
          cachedD3D11Fence->AddRef();
          cachedFenceHandle = fenceHandle;
          cachedSourcePid = sourcePid;
        }
      }

      // 2. Open Texture (We know it's missing if we are here)
      ce::HandleGuard dupTex;
      HRESULT hr = E_FAIL;

      if (sharedHandle == NULL) {
        DLL_Log("[VideoEncoder] Frame %d: Error: sharedHandle is NULL",
                encodeFrameCounter);
      } else {
        // Diagnostic: Check format shareability
        UINT formatSupport = 0;
        d3d11Device->CheckFormatSupport(DXGI_FORMAT_B8G8R8A8_UNORM,
                                        &formatSupport);
        if (!(formatSupport & D3D11_FORMAT_SUPPORT_SHAREABLE)) {
          DLL_Log("[VideoEncoder] CRITICAL WARNING: DXGI_FORMAT_B8G8R8A8_UNORM "
                  "(87) does NOT support "
                  "SHAREABLE! Flags=%x",
                  formatSupport);
        }

        // Try NT handle path first
        if (DuplicateHandle(hProcess.get(), sharedHandle, GetCurrentProcess(),
                            dupTex.addressof(), 0, FALSE,
                            DUPLICATE_SAME_ACCESS)) {
          // Use OpenSharedResource1 for NT handles
          hr = d3d11Device->OpenSharedResource1(dupTex.get(),
                                                IID_PPV_ARGS(&bgraTex));
          if (FAILED(hr)) {
            DLL_Log("[VideoEncoder] Frame %d: OpenSharedResource1(dup=%p) "
                    "failed HR=%x. Falling back to KMT...",
                    encodeFrameCounter, dupTex.get(), hr);
          }
        }

        if (FAILED(hr) && hasSharedAlt) {
          ce::HandleGuard dupTexAlt;
          if (DuplicateHandle(hProcess.get(), sharedHandleAlt,
                              GetCurrentProcess(), dupTexAlt.addressof(), 0,
                              FALSE, DUPLICATE_SAME_ACCESS)) {
            hr = d3d11Device->OpenSharedResource1(dupTexAlt.get(),
                                                  IID_PPV_ARGS(&bgraTex));
            if (FAILED(hr)) {
              DLL_Log("[VideoEncoder] Frame %d: OpenSharedResource1(alt dup=%p) "
                      "failed HR=%x. Falling back to KMT...",
                      encodeFrameCounter, dupTexAlt.get(), hr);
            }
          }
        }

        if (FAILED(hr)) {
          hr = d3d11Device->OpenSharedResource(sharedHandle,
                                               IID_PPV_ARGS(&bgraTex));
          if (SUCCEEDED(hr)) {
            DLL_Log("[VideoEncoder] Frame %d: Opened handle %p via KMT path",
                    encodeFrameCounter, sharedHandle);
          }
        }
        if (FAILED(hr) && hasSharedAlt) {
          hr = d3d11Device->OpenSharedResource(sharedHandleAlt,
                                               IID_PPV_ARGS(&bgraTex));
          if (SUCCEEDED(hr)) {
            DLL_Log("[VideoEncoder] Frame %d: Opened alt handle %p via KMT path",
                    encodeFrameCounter, sharedHandleAlt);
          }
        }
      }

      if (FAILED(hr)) {
        DLL_Log(
            "[VideoEncoder] Frame %d: Failed to OpenSharedResource (NT/KMT) "
            "HR=%x, handle=%p, sourcePid=%u, format=%d",
            encodeFrameCounter, hr, sharedHandle, sourcePid, format);
        // Clean up fence if we opened it but failed texture
        if (d3d11Fence) {
          d3d11Fence->Release();
        }
        return false;
      }

      // Cache Texture
      // Find empty slot (0 to 7)
      int targetSlot = 0;
      for (int i = 0; i < 8; i++) {
        if (cachedTextureHandles[i] == nullptr) {
          targetSlot = i;
          break;
        }
        if (i == 7)
          targetSlot = 0; // Fallback to 0 if all full
      }

      if (cachedSharedTextures[targetSlot]) {
        cachedSharedTextures[targetSlot]->Release();
      }

      cachedSharedTextures[targetSlot] = bgraTex;
      cachedSharedTextures[targetSlot]->AddRef();
      cachedTextureHandles[targetSlot] = sharedHandle;

      // hProcess, dupTex, dupFence are auto-closed by RAII here
      cacheSlot = targetSlot;
    }
  } // End of isShmem else block

  // 2. Wait on Synchronization using D3D11 Fence
  // PROTECTED: Immediate Context access
  D3D11ScopedLock lock;

  HRESULT hr = S_OK;

  // D3D11 FENCE PATH (Async GPU sync)
  auto beforeFence = PerfTimer::now();
  if (d3d11Fence) {
    // CPU-side timeout to prevent GPU hangs (Resilience improvement)
    if (!fenceEvent) {
      fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    }

    // Check if fence is already satisfied (avoid SetEvent overhead if possible)
    if (d3d11Fence->GetCompletedValue() < fenceValue) {
      d3d11Fence->SetEventOnCompletion(fenceValue, fenceEvent);
      DWORD waitRes = WaitForSingleObject(fenceEvent, 50); // 50ms timeout
      if (waitRes == WAIT_TIMEOUT) {
        // Frame is too late or GPU is hung - skip this frame
        DLL_Log("[VideoEncoder] Frame %d: GPU Fence Timeout (50ms) - Skipping",
                encodeFrameCounter);
        bgraTex->Release();
        d3d11Fence->Release();
        d3d11Fence = nullptr;
        return false;
      }
    }

    // Async GPU Wait (plus CPU timeout check above)
    d3d11Context->Wait(d3d11Fence, fenceValue);
  }
  auto afterFence = PerfTimer::now();
  stats.fenceWaitMs = PerfTimer::elapsed_ms(beforeFence, afterFence);
  stats.fenceWaitMs = PerfTimer::elapsed_ms(frameStart, afterFence);

  if (d3d11Fence) {
    d3d11Fence->Release();
    d3d11Fence = nullptr;
  }

  if (FAILED(hr)) {
    DLL_Log("[VideoEncoder] Frame %d: Failed to Wait on Fence. HR=%x",
            encodeFrameCounter, hr);
    bgraTex->Release();
    return false;
  }

  auto afterOpen = PerfTimer::now();
  // 4. Ensure Video Processor is initialized first (to get vpSupportsOverlay)
  if (!videoProcessorInit) {
    if (!InitVideoProcessor()) {
      DLL_Log("[VideoEncoder] Frame %d: VP init failed", encodeFrameCounter);
      bgraTex->Release();
      return false;
    }
  }

  // 5. Get cursor info for VP overlay compositing (now vpSupportsOverlay is
  // valid)
  bool cursorVisible = false;
  int cursorX = 0, cursorY = 0;

  if (captureCursor && vpSupportsOverlay && cursorRenderer) {
    CURSORINFO ci = {sizeof(CURSORINFO)};
    if (GetCursorInfo(&ci)) {
      // Log cursor state periodically (every 100 frames)
      if (encodeFrameCounter % 100 == 1) {
        DLL_Log("[Cursor] Frame %d: flags=%d hCursor=%p pos=(%d,%d)",
                encodeFrameCounter, ci.flags, (void *)ci.hCursor,
                ci.ptScreenPos.x, ci.ptScreenPos.y);
      }

      // Hardware cursor in DX12 DirectFlip may not set CURSOR_SHOWING
      // So we check if cursor handle is valid instead
      if (ci.hCursor) {
        cursorVisible = true;
        cursorX = ci.ptScreenPos.x;
        cursorY = ci.ptScreenPos.y;

        // Get cursor from LRU cache (creates if not cached)
        activeCursor = GetCursorCacheEntry(ci.hCursor);
      }
    }
  }

  // 6. Convert BGRA -> NV12 on GPU using Video Processor (with cursor
  // overlay)
  auto beforeConvert = PerfTimer::now();
  ID3D11Texture2D *nv12Tex = nullptr;
  if (!ConvertBGRAtoNV12(bgraTex, &nv12Tex, cursorVisible, cursorX, cursorY)) {
    DLL_Log("[VideoEncoder] Frame %d: GPU color conversion failed",
            encodeFrameCounter);
    bgraTex->Release();
    return false;
  }

  auto afterConvert = PerfTimer::now();
  stats.colorConvertMs = PerfTimer::elapsed_ms(beforeConvert, afterConvert);
  bgraTex->Release(); // No longer needed after conversion

  // 5. Wrap NV12 D3D11 Texture in AVFrame
  AVFrame *d3d11Frame = av_frame_alloc();
  d3d11Frame->format = AV_PIX_FMT_D3D11;
  d3d11Frame->width = scalingEnabled ? outputWidth : width;
  d3d11Frame->height = scalingEnabled ? outputHeight : height;
  d3d11Frame->hw_frames_ctx = av_buffer_ref(d3d11FramesCtx);

  d3d11Frame->buf[0] =
      av_buffer_create((uint8_t *)nv12Tex, 0, FreeD3D11Tex, NULL, 0);
  if (!d3d11Frame->buf[0]) {
    FreeD3D11Tex(NULL, (uint8_t *)nv12Tex);
    av_frame_free(&d3d11Frame);
    return false;
  }

  d3d11Frame->data[0] = (uint8_t *)nv12Tex;
  d3d11Frame->data[1] = 0; // index

  // Calculate relative PTS (start from 0)
  if (startPts < 0) {
    startPts = timestamp;
    DLL_Log("[VideoEncoder] Recording started at PTS %lld", startPts.load());
  }

  // Calculate PTS
  if (savedConfig.useVFR && startPts >= 0) {
    // VFR: PTS based on actual capture timestamp (microseconds)
    // timestamp is MS, startPts is MS. Diff is MS.
    // time_base is 1us (1/1000000). So we need to convert MS diff to us.
    d3d11Frame->pts = (timestamp - startPts) * 1000;
  } else {
    // CFR: derive PTS from real elapsed time to avoid playback speed drift
    // when delivered frame cadence differs from target FPS.
    int fps = (codecCtx && codecCtx->framerate.num > 0) ? codecCtx->framerate.num
                                                        : savedConfig.fps;
    if (fps <= 0) {
      fps = 60;
    }
    int64_t elapsedMs = timestamp - startPts;
    if (elapsedMs < 0) {
      elapsedMs = 0;
    }
    d3d11Frame->pts = av_rescale(elapsedMs, fps, 1000);
  }

  // Enforce strictly monotonic input PTS for encoder stability.
  if (lastAssignedVideoPts >= 0 && d3d11Frame->pts <= lastAssignedVideoPts) {
    d3d11Frame->pts = lastAssignedVideoPts + 1;
  }
  lastAssignedVideoPts = d3d11Frame->pts;

  // 5. Encode (Direct D3D11 Frame) - with proper packet draining
  AVPacket *pkt = av_packet_alloc();
  int packetCount = 0;

  // Pure encoding time measurement (excluding color conversion/wait)
  auto encodeStart = PerfTimer::now();

  // Helper lambda to drain all available packets
  auto drainPackets = [&]() {
    while (true) {
      int ret = avcodec_receive_packet(codecCtx, pkt);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        break;
      if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
        DLL_Log("[VideoEncoder] avcodec_receive_packet failed: %d (%s)", ret,
                errbuf);
        break;
      }

      packetCount++;
      pkt->stream_index = stream->index; // Ensure video stream index

      // Duration Logic
      if (savedConfig.useVFR) {
        // For VFR, duration is variable. Best guess is target frame duration.
        // Since time_base is 1us, duration is in us.
        pkt->duration = 1000000 / savedConfig.fps;
      } else {
        // For CFR, duration is 1 unit (1/FPS)
        pkt->duration = 1;
      }

      if (onPacket)
        onPacket(pkt);
      av_packet_unref(pkt);
    }
  };

  // First drain any pending packets
  drainPackets();

  // Try to send the frame, handling EAGAIN by draining and retrying
  int ret = avcodec_send_frame(codecCtx, d3d11Frame);
  int retries = 0;
  while (ret == AVERROR(EAGAIN) && retries < 10) {
    if (retries == 0) {
      lastEncoderOverloadTickMs.store(GetTickCount64(),
                                      std::memory_order_relaxed);
      PublishRuntimeState();
    }
    // Encoder buffer full, drain packets and retry
    drainPackets();
    ret = avcodec_send_frame(codecCtx, d3d11Frame);
    retries++;
  }

  if (ret < 0 && ret != AVERROR(EAGAIN)) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
    DLL_Log("[VideoEncoder] avcodec_send_frame failed: %d (%s)", ret, errbuf);
    av_packet_free(&pkt);
    av_frame_free(&d3d11Frame);
    return false;
  }

  // Drain packets after successful send
  drainPackets();

  auto afterEncode = PerfTimer::now();
  stats.ptsMs = timestamp;
  stats.textureOpenMs = PerfTimer::elapsed_ms(frameStart, afterOpen);
  stats.colorConvertMs = PerfTimer::elapsed_ms(afterOpen, afterConvert);
  stats.encodeMs = PerfTimer::elapsed_ms(encodeStart, afterEncode);
  stats.totalMs = PerfTimer::elapsed_ms(frameStart, afterEncode);

  // Update last frame encode time (in microseconds)
  // This is robust against timer noise/underflow compared to (Total - Wait).
  lastEncodeTimeUs =
      (int64_t)(PerfTimer::elapsed_ms(afterFence, afterEncode) * 1000.0);
  lastFenceWaitUs = (int64_t)(stats.fenceWaitMs * 1000.0);
  stats.packetsProduced = packetCount;

  av_packet_free(&pkt);

  // Update global stats
  g_framesEncoded++;
  outputFrameCount++;
  g_totalFenceWait += stats.fenceWaitMs;
  g_totalColorConvert += stats.colorConvertMs;
  g_totalEncode += stats.encodeMs;
  if (stats.totalMs > g_maxFrameTime)
    g_maxFrameTime = stats.totalMs;
  if (stats.totalMs > expectedFrameMs * 2)
    g_slowFrameCount++;

  // Log individual slow frames for debugging
  // Log more frequently for performance tuning (every 30 frames)
  if (stats.totalMs > expectedFrameMs * 2 || encodeFrameCounter <= 5 ||
      encodeFrameCounter % 30 == 0) {
    std::string features = "";
    if (savedConfig.lookahead)
      features += "Lookahead ";
    if (savedConfig.aq)
      features += "AQ ";
    if (savedConfig.bFrames > 0)
      features += "B-Frames ";
    if (!savedConfig.multipass.empty() && savedConfig.multipass != "disabled")
      features += "Multipass ";

    const char *slowLabel =
        (stats.totalMs > expectedFrameMs * 2) ? "(SLOW!)" : "";

    DLL_Log("[PERF] Frame %d: TOTAL=%.2fms %s fence=%.2f convert=%.2f "
            "encode=%.2f pts=%lldms packets=%d [Features: %s]",
            encodeFrameCounter, stats.totalMs, slowLabel, stats.fenceWaitMs,
            stats.colorConvertMs, stats.encodeMs, stats.ptsMs,
            stats.packetsProduced, features.c_str());
  }

  // Periodic performance summary (every 120 frames ~1 sec at
  // 120fps)
  if (encodeFrameCounter % 120 == 0) {
    double avgFence = g_totalFenceWait / g_framesEncoded;
    double avgConvert = g_totalColorConvert / g_framesEncoded;
    double avgEncode = g_totalEncode / g_framesEncoded;
    double avgTotal = avgFence + avgConvert + avgEncode;

    // Identify bottleneck
    const char *bottleneck = "ENCODE";
    double maxTime = avgEncode;
    if (avgFence > maxTime) {
      bottleneck = "FENCE_WAIT";
      maxTime = avgFence;
    }
    if (avgConvert > maxTime) {
      bottleneck = "COLOR_CONV";
      maxTime = avgConvert;
    }

    DLL_Log("[PERF SUMMARY] Frames=%lld Avg: total=%.2fms fence=%.2f "
            "convert=%.2f "
            "encode=%.2f | Max=%.2fms SlowFrames=%d | Bottleneck=%s",
            g_framesEncoded, avgTotal, avgFence, avgConvert, avgEncode,
            g_maxFrameTime, g_slowFrameCount, bottleneck);

    // Frame timing analysis for smoothness
    if (stats.actualPtsDiff > 0) {
      double jitter = (double)(stats.actualPtsDiff - stats.expectedPtsDiff);
      DLL_Log("[SMOOTHNESS] Expected frame interval: %lldms "
              "Actual: %lldms "
              "Jitter: %.2fms",
              stats.expectedPtsDiff, stats.actualPtsDiff, jitter);
    }
  }

  av_frame_free(&d3d11Frame); // Releases D3D11 Tex

  return true;
}

// EncodeFrameD3D11: Direct D3D11 texture encoding for framegrab
// mode Zero-copy path - texture is converted BGRA->NV12 directly
bool VideoEncoder::EncodeFrameD3D11(ID3D11Texture2D *bgraTexture, int64_t pts,
                                    uint32_t frameWidth, uint32_t frameHeight) {
  if (!recordingRequested)
    return false;

  // Use captured frame dimensions if not yet set
  if (width == 0 || height == 0) {
    width = (int)frameWidth;
    height = (int)frameHeight;
    DLL_Log("[VideoEncoder] Framegrab using dimensions: %dx%d", width, height);
  }

  // Ensure D3D11 device is available (we need it for Video
  // Processor)
  if (!d3d11Device || !d3d11Context) {
    // Get device from the texture
    ID3D11Device *texDevice = nullptr;
    bgraTexture->GetDevice(&texDevice);
    if (texDevice) {
      texDevice->QueryInterface(__uuidof(ID3D11Device5), (void **)&d3d11Device);
      ID3D11DeviceContext *ctx = nullptr;
      texDevice->GetImmediateContext(&ctx);
      if (ctx) {
        ctx->QueryInterface(__uuidof(ID3D11DeviceContext4),
                            (void **)&d3d11Context);
        ctx->Release();
      }
      texDevice->Release();
    }

    if (!d3d11Device || !d3d11Context) {
      DLL_Log("[VideoEncoder] Framegrab: Failed to get D3D11 "
              "device from "
              "texture");
      return false;
    }
  }

  // Ensure encoder is initialized with hardware context
  if (!EnsureDevice())
    return false;

  if (!fileOpened) {
    DLL_Log("[VideoEncoder] Opening Output File: %s", outputFilename.c_str());
    if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
      int ret = avio_open(&fmtCtx->pb, outputFilename.c_str(), AVIO_FLAG_WRITE);
      if (ret < 0) {
        DLL_Log("Failed to open output file: %d", ret);
        return false;
      }
    }
    if (avformat_write_header(fmtCtx, nullptr) < 0) {
      DLL_Log("Failed to write header");
      return false;
    }
    fileOpened = true;
  }

  // Detect new recording start and reset counters (using class members)
  if (startPts < 0) {
    needsCounterReset = true;
  }

  // Reset counters on new recording
  if (needsCounterReset) {
    encodeFrameCounter = 0;
    lastLogFrameCount = 0;
    needsCounterReset = false;
    DLL_Log("[VideoEncoder] ScreenGrab: Reset encodeFrameCounter for new "
            "recording");
  }

  encodeFrameCounter++;

  // Log frame stats periodically (every 120 frames = ~1 sec at 120fps)
  if (encodeFrameCounter - lastLogFrameCount >= kFpsLogIntervalFrames) {
    if (startPts >= 0 && pts > startPts) {
      // Use cached QPC frequency (class member, initialized in EncodeFrame)
      if (qpcFrequency == 0) {
        LARGE_INTEGER li;
        QueryPerformanceFrequency(&li);
        qpcFrequency = li.QuadPart;
      }

      double elapsedSec = (double)(pts - startPts) / (double)qpcFrequency;
      double outputFps = (elapsedSec > 0.001)
                             ? ((double)encodeFrameCounter / elapsedSec)
                             : 0.0;
      DLL_Log("[FPS] Framegrab: %.1f frames, %.1f fps over %.1fs",
              (double)encodeFrameCounter, outputFps, elapsedSec);
    }
    lastLogFrameCount = encodeFrameCounter;
  }

  // Performance timing
  auto frameStart = PerfTimer::now();

  // Software cursor capture (WGC native cursor disabled to avoid
  // game stutter) Throttle cursor updates to 30Hz (every 4th frame
  // at 120fps) to reduce GetCursorInfo overhead by 75% while
  // keeping cursor motion smooth
  bool cursorVisible = false;
  int cursorX = 0, cursorY = 0;
  static int cursorUpdateCounter = 0;
  static int cachedCursorX = 0, cachedCursorY = 0;
  static bool cachedCursorVisible = false;
  // static HCURSOR cachedCursorHandle = nullptr;

  if (captureCursor && cursorRenderer) {
    cursorUpdateCounter++;

    // Update cursor every 4th frame (30Hz at 120fps)
    bool shouldUpdateCursor = (cursorUpdateCounter % 4 == 0);

    if (shouldUpdateCursor) {
      CURSORINFO ci = {};
      ci.cbSize = sizeof(ci);
      if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
        cachedCursorVisible = true;
        cachedCursorX = ci.ptScreenPos.x;
        cachedCursorY = ci.ptScreenPos.y;
        // cachedCursorHandle = ci.hCursor;

        // Use LRU cursor cache (creates texture if not cached)
        activeCursor = GetCursorCacheEntry(ci.hCursor);
      } else {
        cachedCursorVisible = false;
      }
    }

    // Use cached cursor values for rendering
    cursorVisible = cachedCursorVisible;
    cursorX = cachedCursorX;
    cursorY = cachedCursorY;
  }

  // Convert BGRA → NV12 using Video Processor (with software cursor
  // overlay)
  auto beforeConvert = PerfTimer::now();
  ID3D11Texture2D *nv12Tex = nullptr;

  // Scoped Lock for D3D11 Immediate Context (protects Blt/CopyResource)
  bool convertSuccess =
      ConvertBGRAtoNV12(bgraTexture, &nv12Tex, cursorVisible, cursorX, cursorY);

  if (!convertSuccess) {
    DLL_Log("[VideoEncoder] Frame %d: GPU color conversion failed",
            encodeFrameCounter);
    return false;
  }
  auto afterConvert = PerfTimer::now();
  double convertMs = PerfTimer::elapsed_ms(beforeConvert, afterConvert);

  // Wrap NV12 D3D11 Texture in AVFrame
  AVFrame *d3d11Frame = av_frame_alloc();
  d3d11Frame->format = AV_PIX_FMT_D3D11;
  d3d11Frame->width = scalingEnabled ? outputWidth : width;
  d3d11Frame->height = scalingEnabled ? outputHeight : height;
  d3d11Frame->hw_frames_ctx = av_buffer_ref(d3d11FramesCtx);

  d3d11Frame->buf[0] =
      av_buffer_create((uint8_t *)nv12Tex, 0, FreeD3D11Tex, NULL, 0);
  if (!d3d11Frame->buf[0]) {
    FreeD3D11Tex(NULL, (uint8_t *)nv12Tex);
    av_frame_free(&d3d11Frame);
    return false;
  }
  d3d11Frame->data[0] = (uint8_t *)nv12Tex;
  d3d11Frame->data[1] = 0;

  // Calculate PTS based on elapsed time (VFR support)
  // pts is in milliseconds (from DxgiCapture/WGC)
  if (startPts < 0) {
    startPts = pts;
    DLL_Log("[VideoEncoder] Framegrab recording started at PTS %lld",
            startPts.load());
  }

  int64_t elapsedMs = pts - startPts;
  if (elapsedMs < 0)
    elapsedMs = 0;

  if (savedConfig.useVFR) {
    // VFR codec time base is 1/1000000.
    d3d11Frame->pts = elapsedMs * 1000;
  } else {
    // CFR codec time base is 1/fps.
    int fps = 60; // Default
    if (codecCtx && codecCtx->framerate.num > 0) {
      fps = codecCtx->framerate.num;
    }
    d3d11Frame->pts = av_rescale(elapsedMs, fps, 1000);
  }

  if (lastAssignedVideoPts >= 0 && d3d11Frame->pts <= lastAssignedVideoPts) {
    d3d11Frame->pts = lastAssignedVideoPts + 1;
  }
  lastAssignedVideoPts = d3d11Frame->pts;

  // Debug: Log input frame PTS
  if (encodeFrameCounter < 20 || encodeFrameCounter % 100 == 0) {
    DLL_Log("[Framegrab DEBUG] Sending frame %d with input PTS=%lld",
            encodeFrameCounter, d3d11Frame->pts);
  }

  // Encode
  AVPacket *pkt = av_packet_alloc();
  int packetCount = 0;

  auto drainPackets = [&]() {
    while (true) {
      int ret = avcodec_receive_packet(codecCtx, pkt);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        break;
      if (ret < 0)
        break;
      packetCount++;
      pkt->stream_index = stream->index;

      // Debug: Log output packet PTS
      if (encodeFrameCounter < 30 || encodeFrameCounter % 100 == 0) {
        DLL_Log("[Framegrab DEBUG] Received pkt: pts=%lld dts=%lld "
                "size=%d "
                "flags=%d",
                pkt->pts, pkt->dts, pkt->size, pkt->flags);
      }

      // Set packet duration based on VFR/CFR mode (matches inject-mode logic)
      if (savedConfig.useVFR) {
        // VFR: time_base is 1/1000000, so duration is 1 frame in microseconds
        pkt->duration = 1000000 / (savedConfig.fps > 0 ? savedConfig.fps : 60);
      } else {
        // CFR: time_base is 1/fps, duration is 1 unit
        pkt->duration = 1;
      }

      if (onPacket)
        onPacket(pkt);
      av_packet_unref(pkt);
    }
  };

  // Drain any pending packets first (like inject mode)
  drainPackets();

  int ret = avcodec_send_frame(codecCtx, d3d11Frame);

  int retries = 0;
  while (ret == AVERROR(EAGAIN) && retries < 10) {
    if (retries == 0) {
      lastEncoderOverloadTickMs.store(GetTickCount64(),
                                      std::memory_order_relaxed);
      PublishRuntimeState();
    }
    drainPackets();
    ret = avcodec_send_frame(codecCtx, d3d11Frame);
    retries++;
  }

  if (ret < 0 && ret != AVERROR(EAGAIN)) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
    DLL_Log("[VideoEncoder] ScreenGrab send_frame failed: %d (%s)", ret,
            errbuf);
    av_packet_free(&pkt);
    av_frame_free(&d3d11Frame);
    return false;
  }

  drainPackets();

  auto afterEncode = PerfTimer::now();
  double encodeMs = PerfTimer::elapsed_ms(afterConvert, afterEncode);
  double totalMs = PerfTimer::elapsed_ms(frameStart, afterEncode);

  av_packet_free(&pkt);

  // Log individual slow frames for debugging
  double expectedFrameMs = 1000.0 / (double)codecCtx->framerate.num;
  if (totalMs > expectedFrameMs * 2.0) {
    std::string features = "";
    if (savedConfig.lookahead)
      features += "Lookahead ";
    if (savedConfig.aq)
      features += "AQ ";
    if (savedConfig.bFrames > 0)
      features += "B-Frames ";
    if (!savedConfig.multipass.empty() && savedConfig.multipass != "disabled")
      features += "Multipass ";

    DLL_Log("[Framegrab PERF] Frame %d: total=%.2fms (SLOW!) convert=%.2f "
            "encode=%.2f packets=%d [Features: %s]",
            encodeFrameCounter, totalMs, convertMs, encodeMs, packetCount,
            features.c_str());
  }

  // Log periodic stats
  if (encodeFrameCounter % 120 == 0) {
    DLL_Log("[Framegrab PERF] Frame %d: total=%.2fms convert=%.2f "
            "encode=%.2f "
            "packets=%d",
            encodeFrameCounter, totalMs, convertMs, encodeMs, packetCount);
  }

  av_frame_free(&d3d11Frame);
  return true;
}

void VideoEncoder::CleanupResources() {
  // Free any queued packets (defensive)
  {
    std::lock_guard<std::mutex> lock(queueMutex);
    while (!packetQueue.empty()) {
      AVPacket *pkt = packetQueue.front();
      packetQueue.pop();
      av_packet_free(&pkt);
    }
  }

  currentQueueBytes = 0;

  if (stream)
    stream = nullptr;
  if (fmtCtx) {
    avformat_free_context(fmtCtx);
    fmtCtx = nullptr;
  }
  if (codecCtx) {
    avcodec_free_context(&codecCtx);
    codecCtx = nullptr;
  }

  if (d3d11Context) {
    d3d11Context->Release();
    d3d11Context = nullptr;
  }
  if (d3d11Device) {
    d3d11Device->Release();
    d3d11Device = nullptr;
  }

  if (d3d11DeviceCtx)
    av_buffer_unref(&d3d11DeviceCtx);
  if (d3d11FramesCtx)
    av_buffer_unref(&d3d11FramesCtx);

  if (cudaDeviceCtx)
    av_buffer_unref(&cudaDeviceCtx);
  if (cudaFramesCtx)
    av_buffer_unref(&cudaFramesCtx);
  if (hwDeviceCtx)
    av_buffer_unref(&hwDeviceCtx);
  if (hwFramesCtx)
    av_buffer_unref(&hwFramesCtx);

  for (int i = 0; i < 8; i++) {
    if (cachedSharedTextures[i]) {
      cachedSharedTextures[i]->Release();
      cachedSharedTextures[i] = nullptr;
    }
    cachedTextureHandles[i] = nullptr;
  }

  if (cachedD3D11Fence) {
    cachedD3D11Fence->Release();
    cachedD3D11Fence = nullptr;
  }
  cachedFenceHandle = nullptr;
  cachedSourcePid = 0;

  for (int i = 0; i < 8; i++) {
    if (sharedCaptureTextures[i]) {
      sharedCaptureTextures[i]->Release();
      sharedCaptureTextures[i] = nullptr;
    }
    if (sharedCaptureHandles[i]) {
      CloseHandle(sharedCaptureHandles[i]);
      sharedCaptureHandles[i] = nullptr;
    }
  }
  if (sharedCaptureFence) {
    sharedCaptureFence->Release();
    sharedCaptureFence = nullptr;
  }
  if (sharedCaptureFenceHandle) {
    CloseHandle(sharedCaptureFenceHandle);
    sharedCaptureFenceHandle = nullptr;
  }
  sharedCaptureTexturesCreated = false;

  if (bgraStagingTexture) {
    bgraStagingTexture->Release();
    bgraStagingTexture = nullptr;
  }

  CleanupVideoProcessor();
  CleanupCursorCache();
#ifdef HAS_CUDA
  CleanupCuda();
#endif

  initDone = false;
  fileOpened = false;
  startPts = -1;
  inputFrameCount = 0;
  outputFrameCount = 0;
  skippedFrameCount = 0;
  duplicatedFrameCount = 0;
  encodeFrameCounter = 0;
  lastLogFrameCount = 0;
  nextOutputTime_ms = -1;
  lastEncodeTimeUs = 0;
  lastFenceWaitUs = 0;
  encodedDurationUs.store(0, std::memory_order_relaxed);
  lastAssignedVideoPts = -1;
}

void VideoEncoder::Stop() {
  bool wasRecording = recordingRequested;
  recordingRequested = false;

  if (wasRecording && writerRunning) {
    DLL_Log("[VideoEncoder] Stop: Signaling finalize (queueBytes=%zu)...",
            currentQueueBytes.load());
    isStopping = true;
    queueCV.notify_all();
    // Fall through to join — ensures file is fully closed before returning
  }

  // Always wait for writer thread to finish (writes trailer + closes file)
  if (writerThread.joinable()) {
    DLL_Log("[VideoEncoder] Stop: Waiting for writer thread to finish...");
    writerThread.join();
    DLL_Log("[VideoEncoder] Stop: Writer thread joined.");
  }

  // Fallback: if thread wasn't running and file is still open, close it now
  if (fileOpened) {
    DLL_Log("[VideoEncoder] Sync Stop: Finalizing file...");
    if (fmtCtx) {
      av_write_trailer(fmtCtx);
      if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&fmtCtx->pb);
      }
    }
    fileOpened = false;
  }

  CleanupResources();
}

// ============================================================================
// D3D11 Video Processor for GPU-accelerated BGRA → NV12 conversion
// ============================================================================

bool VideoEncoder::InitVideoProcessor() {
  if (videoProcessorInit)
    return true;

  if (!d3d11Device) {
    DLL_Log("[VideoProcessor] D3D11 device not available");
    return false;
  }

  HRESULT hr;

  // Get video device interface
  hr = d3d11Device->QueryInterface(__uuidof(ID3D11VideoDevice),
                                   (void **)&videoDevice);
  if (FAILED(hr)) {
    DLL_Log("[VideoProcessor] Failed to get ID3D11VideoDevice. HR=%x", hr);
    return false;
  }

  // Get video context
  hr = d3d11Context->QueryInterface(__uuidof(ID3D11VideoContext),
                                    (void **)&videoContext);
  if (FAILED(hr)) {
    DLL_Log("[VideoProcessor] Failed to get ID3D11VideoContext. HR=%x", hr);
    return false;
  }

  // Store input dimensions (captured frame size)
  inputWidth = width;
  inputHeight = height;

  // Determine output dimensions based on scaling config
  if (savedConfig.scaling.enabled && savedConfig.scaling.outputWidth > 0 &&
      savedConfig.scaling.outputHeight > 0) {
    outputWidth = savedConfig.scaling.outputWidth;
    outputHeight = savedConfig.scaling.outputHeight;
  } else {
    // No scaling or native resolution
    outputWidth = width;
    outputHeight = height;
  }

  // Check if scaling is actually needed (input != output)
  scalingEnabled = (inputWidth != outputWidth || inputHeight != outputHeight);

  if (scalingEnabled) {
    DLL_Log("[VideoProcessor] GPU SCALING ENABLED: %dx%d -> %dx%d", inputWidth,
            inputHeight, outputWidth, outputHeight);
  } else {
    DLL_Log("[VideoProcessor] Scaling disabled (input matches output: %dx%d)",
            inputWidth, inputHeight);
  }

  // Create video processor enumerator with potentially different input/output
  // dims
  D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {};
  contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  contentDesc.InputWidth = inputWidth;
  contentDesc.InputHeight = inputHeight;
  contentDesc.OutputWidth = outputWidth;
  contentDesc.OutputHeight = outputHeight;
  contentDesc.Usage = (savedConfig.scaling.quality == "best")
                          ? D3D11_VIDEO_USAGE_OPTIMAL_QUALITY
                          : D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

  hr = videoDevice->CreateVideoProcessorEnumerator(&contentDesc,
                                                   &videoProcessorEnum);
  if (FAILED(hr)) {
    DLL_Log("[VideoProcessor] Failed to create enumerator. HR=%x", hr);
    return false;
  }

  // Create video processor
  hr =
      videoDevice->CreateVideoProcessor(videoProcessorEnum, 0, &videoProcessor);
  if (FAILED(hr)) {
    DLL_Log("[VideoProcessor] Failed to create processor. HR=%x", hr);
    return false;
  }

  // Check if VP supports 2+ input streams for cursor overlay
  D3D11_VIDEO_PROCESSOR_CAPS vpCaps = {};
  hr = videoProcessorEnum->GetVideoProcessorCaps(&vpCaps);
  if (SUCCEEDED(hr)) {
    vpSupportsOverlay = (vpCaps.MaxInputStreams >= 2);
    DLL_Log("[VideoProcessor] MaxInputStreams=%d, overlay support=%s",
            vpCaps.MaxInputStreams, vpSupportsOverlay ? "YES" : "NO");
  } else {
    DLL_Log("[VideoProcessor] Failed to get VP caps, overlay disabled");
    vpSupportsOverlay = false;
  }

  // Configure scaling filter if scaling is enabled
  if (scalingEnabled) {
    // Map sharpness (0-100) directly to D3D11 VP edge enhancement
    bool enableEdgeEnhancement = (savedConfig.scaling.sharpness > 0);
    int edgeEnhancementLevel = savedConfig.scaling.sharpness;

    if (enableEdgeEnhancement) {
      // Check if edge enhancement is supported
      D3D11_VIDEO_PROCESSOR_FILTER_RANGE filterRange = {};
      hr = videoProcessorEnum->GetVideoProcessorFilterRange(
          D3D11_VIDEO_PROCESSOR_FILTER_EDGE_ENHANCEMENT, &filterRange);

      if (SUCCEEDED(hr)) {
        // Map our 0-100 level to the actual VP filter range
        int filterValue = filterRange.Default;
        if (filterRange.Maximum > filterRange.Minimum) {
          filterValue = filterRange.Minimum +
                        (edgeEnhancementLevel *
                         (filterRange.Maximum - filterRange.Minimum) / 100);
        }

        videoContext->VideoProcessorSetStreamFilter(
            videoProcessor, 0, D3D11_VIDEO_PROCESSOR_FILTER_EDGE_ENHANCEMENT,
            TRUE, filterValue);

        DLL_Log("[VideoProcessor] Scaling: quality=%s, sharpness=%d "
                "(filterValue=%d, range=%d-%d)",
                savedConfig.scaling.quality.c_str(), edgeEnhancementLevel,
                filterValue, filterRange.Minimum, filterRange.Maximum);
      } else {
        DLL_Log("[VideoProcessor] Edge enhancement (sharpness) not supported "
                "by hardware");
      }
    } else {
      DLL_Log("[VideoProcessor] Scaling: quality=%s, sharpness=0 (disabled)",
              savedConfig.scaling.quality.c_str());
    }

    // CRITICAL: Set source and destination rectangles for scaling
    // Without these, VideoProcessorBlt fails with E_INVALIDARG
    RECT sourceRect = {0, 0, (LONG)inputWidth, (LONG)inputHeight};
    RECT destRect = {0, 0, (LONG)outputWidth, (LONG)outputHeight};

    // Stream 0: Source rect = full input frame
    videoContext->VideoProcessorSetStreamSourceRect(videoProcessor, 0, TRUE,
                                                    &sourceRect);
    // Stream 0: Dest rect = full output frame (scaled)
    videoContext->VideoProcessorSetStreamDestRect(videoProcessor, 0, TRUE,
                                                  &destRect);
    // Output target = full output surface
    videoContext->VideoProcessorSetOutputTargetRect(videoProcessor, TRUE,
                                                    &destRect);

    DLL_Log("[VideoProcessor] Scaling rects: source=%dx%d dest=%dx%d",
            inputWidth, inputHeight, outputWidth, outputHeight);
  }

  // Configure color space: Full RGB input -> Limited YCbCr output
  // (video standard) Input: Full range RGB (0-255) from captured
  // game frame
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColorSpace = {};
  inputColorSpace.Usage = 0;     // 0 = Playback, 1 = Video processing
  inputColorSpace.RGB_Range = 0; // 0 = Full range (0-255), 1 = Studio (16-235)
  inputColorSpace.YCbCr_Matrix = 1;  // 0 = BT.601, 1 = BT.709
  inputColorSpace.YCbCr_xvYCC = 0;   // 0 = Conventional, 1 = Extended
  inputColorSpace.Nominal_Range = 2; // 2 = Full (0-255) for input

  // Output: Limited range YCbCr (16-235) - video standard for
  // compatibility
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColorSpace = {};
  outputColorSpace.Usage = 0;
  outputColorSpace.RGB_Range = 1;    // 1 = Studio/Limited range (16-235)
  outputColorSpace.YCbCr_Matrix = 1; // BT.709
  outputColorSpace.YCbCr_xvYCC = 0;
  outputColorSpace.Nominal_Range = 1; // 1 = Studio (16-235) for output

  videoContext->VideoProcessorSetStreamColorSpace(videoProcessor, 0,
                                                  &inputColorSpace);
  videoContext->VideoProcessorSetOutputColorSpace(videoProcessor,
                                                  &outputColorSpace);
  DLL_Log("[VideoProcessor] Color space: Full RGB (0-255) -> "
          "Limited YCbCr "
          "(16-235, BT.709)");

  // Create triple-buffered NV12 staging textures for output
  // IMPORTANT: Use OUTPUT dimensions for the NV12 textures (after scaling)
  D3D11_TEXTURE2D_DESC nv12Desc = {};
  nv12Desc.Width = outputWidth;
  nv12Desc.Height = outputHeight;
  nv12Desc.MipLevels = 1;
  nv12Desc.ArraySize = 1;
  nv12Desc.Format = DXGI_FORMAT_NV12; // NV12 for NVENC
  nv12Desc.SampleDesc.Count = 1;
  nv12Desc.Usage = D3D11_USAGE_DEFAULT;
  nv12Desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER;

  ID3D11Device *baseDevice = nullptr;
  d3d11Device->QueryInterface(__uuidof(ID3D11Device), (void **)&baseDevice);

  nv12BufferCount = savedConfig.lookahead ? 40 : 3;
  if (nv12BufferCount < 3) {
    nv12BufferCount = 3;
  }
  if (nv12BufferCount > 64) {
    nv12BufferCount = 64;
  }
  nv12StagingTextures.assign(nv12BufferCount, nullptr);
  outputViews.assign(nv12BufferCount, nullptr);
  currentNV12Buffer = 0;

  for (int i = 0; i < nv12BufferCount; i++) {
    hr = baseDevice->CreateTexture2D(&nv12Desc, nullptr,
                                     &nv12StagingTextures[i]);
    if (FAILED(hr)) {
      DLL_Log("[VideoProcessor] Failed to create NV12 texture %d. "
              "HR=%x",
              i, hr);
      baseDevice->Release();
      CleanupVideoProcessor();
      return false;
    }

    // Create output view for each NV12 texture
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc = {};
    outputViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    outputViewDesc.Texture2D.MipSlice = 0;

    hr = videoDevice->CreateVideoProcessorOutputView(
        nv12StagingTextures[i], videoProcessorEnum, &outputViewDesc,
        &outputViews[i]);
    if (FAILED(hr)) {
      DLL_Log("[VideoProcessor] Failed to create output view %d. HR=%x", i, hr);
      baseDevice->Release();
      CleanupVideoProcessor();
      return false;
    }
  }
  baseDevice->Release();
  DLL_Log("[VideoProcessor] Created %d NV12 staging textures at %dx%d "
          "(triple buffering)",
          nv12BufferCount, outputWidth, outputHeight);

  // Create BGRA staging texture for Desktop Duplication
  // compatibility DD textures often have D3D11_BIND_RENDER_TARGET
  // only, which is incompatible with CreateVideoProcessorInputView.
  // This staging texture allows CopyResource.
  // Use INPUT dimensions for BGRA staging (before scaling)
  D3D11_TEXTURE2D_DESC bgraDesc = {};
  bgraDesc.Width = inputWidth;
  bgraDesc.Height = inputHeight;
  bgraDesc.MipLevels = 1;
  bgraDesc.ArraySize = 1;
  bgraDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  bgraDesc.SampleDesc.Count = 1;
  bgraDesc.Usage = D3D11_USAGE_DEFAULT;
  bgraDesc.BindFlags = 0; // No bind flags = compatible with
                          // CopyResource + VideoProcessor

  ID3D11Device *baseDevice2 = nullptr;
  d3d11Device->QueryInterface(__uuidof(ID3D11Device), (void **)&baseDevice2);
  hr = baseDevice2->CreateTexture2D(&bgraDesc, nullptr, &bgraStagingTexture);
  baseDevice2->Release();

  if (FAILED(hr)) {
    DLL_Log("[VideoProcessor] Failed to create BGRA staging "
            "texture. HR=%x",
            hr);
    return false;
  }
  DLL_Log("[VideoProcessor] Created BGRA staging texture at %dx%d for DD "
          "compatibility",
          inputWidth, inputHeight);

  videoProcessorInit = true;

  if (scalingEnabled) {
    DLL_Log("[VideoProcessor] Initialized with SCALING: %dx%d -> %dx%d "
            "BGRA→NV12",
            inputWidth, inputHeight, outputWidth, outputHeight);
  } else {
    DLL_Log("[VideoProcessor] Initialized for %dx%d "
            "BGRA→NV12 (no scaling)",
            outputWidth, outputHeight);
  }
  return true;
}

bool VideoEncoder::ConvertBGRAtoNV12(ID3D11Texture2D *bgraTexture,
                                     ID3D11Texture2D **nv12Output,
                                     bool cursorVisible, int cursorX,
                                     int cursorY) {
  if (!videoProcessorInit) {
    if (!InitVideoProcessor())
      return false;
  }

  // Debug: Log texture descriptions on first call
  static bool firstCall = true;
  if (firstCall) {
    D3D11_TEXTURE2D_DESC srcDesc;
    bgraTexture->GetDesc(&srcDesc);
    DLL_Log("[VP DEBUG] Source tex: %dx%d fmt=%d bind=%x misc=%x",
            srcDesc.Width, srcDesc.Height, srcDesc.Format, srcDesc.BindFlags,
            srcDesc.MiscFlags);
    firstCall = false;
  }

  // Try to create input view directly from source texture first
  // This works for inject mode where the texture has compatible
  // bind flags Only fall back to staging copy for Desktop
  // Duplication (screengrab) textures
  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc = {};
  inputViewDesc.FourCC = 0;
  inputViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  inputViewDesc.Texture2D.MipSlice = 0;

  ID3D11VideoProcessorInputView *localInputView = nullptr;
  // ID3D11Texture2D *inputTexture = bgraTexture; // Use source directly by
  // default

  HRESULT hr = videoDevice->CreateVideoProcessorInputView(
      bgraTexture, videoProcessorEnum, &inputViewDesc, &localInputView);

  if (FAILED(hr)) {
    // Direct access failed (likely Desktop Duplication texture with
    // incompatible bind flags) Fall back to staging copy - but we
    // need to match the format!
    static bool stagingLogged = false;
    if (!stagingLogged) {
      DLL_Log("[VP] Direct input view failed (HR=%x), using "
              "staging copy",
              hr);
      stagingLogged = true;
    }

    // Recreate staging texture with same format as source if needed
    D3D11_TEXTURE2D_DESC srcDesc;
    bgraTexture->GetDesc(&srcDesc);

    if (bgraStagingTexture) {
      D3D11_TEXTURE2D_DESC stageDesc;
      bgraStagingTexture->GetDesc(&stageDesc);

      // Check if staging texture needs to be recreated with correct
      // format
      if (stageDesc.Format != srcDesc.Format) {
        DLL_Log("[VP] Recreating staging texture: fmt %d -> %d",
                stageDesc.Format, srcDesc.Format);
        bgraStagingTexture->Release();
        bgraStagingTexture = nullptr;
      }
    }

    // Create staging texture if needed
    if (!bgraStagingTexture) {
      D3D11_TEXTURE2D_DESC stageDesc = {};
      stageDesc.Width = srcDesc.Width;
      stageDesc.Height = srcDesc.Height;
      stageDesc.MipLevels = 1;
      stageDesc.ArraySize = 1;
      stageDesc.Format = srcDesc.Format; // Match source format!
      stageDesc.SampleDesc.Count = 1;
      stageDesc.Usage = D3D11_USAGE_DEFAULT;
      stageDesc.BindFlags = 0; // Compatible with VP

      ID3D11Device *baseDevice = nullptr;
      d3d11Device->QueryInterface(__uuidof(ID3D11Device), (void **)&baseDevice);
      hr =
          baseDevice->CreateTexture2D(&stageDesc, nullptr, &bgraStagingTexture);
      baseDevice->Release();

      if (FAILED(hr)) {
        DLL_Log("[VP] Failed to create staging texture: HR=%x", hr);
        return false;
      }
      DLL_Log("[VP] Created staging texture: fmt=%d", srcDesc.Format);
    }

    // Copy to staging
    ID3D11DeviceContext *ctx = nullptr;
    d3d11Device->GetImmediateContext(&ctx);
    if (ctx) {
      ctx->CopyResource(bgraStagingTexture, bgraTexture);

      // Debug: Log copy on first few frames
      static int copyCount = 0;
      if (copyCount++ < 5) {
        DLL_Log("[VP] CopyResource to staging - frame %d", copyCount);
      }
      ctx->Release();
    }

    // Create input view from staging texture
    hr = videoDevice->CreateVideoProcessorInputView(
        bgraStagingTexture, videoProcessorEnum, &inputViewDesc,
        &localInputView);
    if (FAILED(hr)) {
      DLL_Log("[VP] Failed to create input view from staging: HR=%x", hr);
      return false;
    }
    // inputTexture = bgraStagingTexture;
  }

  // Setup streams array
  D3D11_VIDEO_PROCESSOR_STREAM streams[2] = {};
  UINT streamCount = 1;

  // Stream 0: Main frame (always enabled)
  streams[0].Enable = TRUE;
  streams[0].pInputSurface = localInputView;

  // Stream 1: Cursor overlay (only if visible and VP supports it)
  bool useCursorStream = cursorVisible && vpSupportsOverlay && activeCursor &&
                         activeCursor->inputView;
  if (useCursorStream) {
    // Calculate DPI scale from frame size vs virtual screen size
    // This works even when app is not DPI-aware (Windows lies about
    // DPI) Frame is physical pixels (e.g., 3840x2160 for native 4K)
    // Screen is virtual pixels (e.g., 2560x1440 at 150% DPI)
    static float cachedDpiScale = 0.0f;
    static bool dpiCalculated = false;

    if (!dpiCalculated) {
      int virtualScreenWidth = GetSystemMetrics(SM_CXSCREEN);
      int virtualScreenHeight = GetSystemMetrics(SM_CYSCREEN);

      // DPI scale = frame physical pixels / screen virtual pixels
      float scaleX = (float)width / (float)virtualScreenWidth;
      float scaleY = (float)height / (float)virtualScreenHeight;
      cachedDpiScale = (scaleX + scaleY) / 2.0f; // Average, should be same

      DLL_Log("[Cursor] DPI scale (calculated): %.2f (frame=%dx%d, "
              "screen=%dx%d)",
              cachedDpiScale, width, height, virtualScreenWidth,
              virtualScreenHeight);
      dpiCalculated = true;
    }

    float dpiScale = cachedDpiScale > 0.5f ? cachedDpiScale : 1.0f;

    // Scale cursor size by DPI (use cached entry)
    int scaledWidth = (int)(activeCursor->width * dpiScale);
    int scaledHeight = (int)(activeCursor->height * dpiScale);

    // Scale cursor position: screen virtual coords -> frame physical coords
    int physicalX = (int)(cursorX * dpiScale);
    int physicalY = (int)(cursorY * dpiScale);

    // Apply hotspot offset (also scaled)
    int hotspotXScaled = (int)(activeCursor->hotspotX * dpiScale);
    int hotspotYScaled = (int)(activeCursor->hotspotY * dpiScale);

    // Set cursor destination rectangle
    RECT cursorRect;
    cursorRect.left = physicalX - hotspotXScaled;
    cursorRect.top = physicalY - hotspotYScaled;
    cursorRect.right = cursorRect.left + scaledWidth;
    cursorRect.bottom = cursorRect.top + scaledHeight;

    // Log cursot rect periodically for debugging
    static int logCounter = 0;
    if (logCounter++ % 200 == 0) {
      DLL_Log("[Cursor] Rect: (%d,%d)-(%d,%d) dpi=%.2f pos=(%d,%d)",
              cursorRect.left, cursorRect.top, cursorRect.right,
              cursorRect.bottom, dpiScale, cursorX, cursorY);
    }

    // CLIPPING: VideoProcessorBlt fails with E_INVALIDARG if the destination
    // rectangle extends outside the target surface. We must clip the cursor
    // Rect to the frame bounds. If clipped, we should really modify the
    // Source rect too, but for a cursor, simple clipping (hiding
    // out-of-bounds parts) is often handled by just clamping the rect? NO, VP
    // squashes if we clamp dest but not source. However, getting SourceRect
    // scaling correct is complex. For now, let's clamp DEST rect and verify
    // if it fixes INVALIDARG. If it looks squashed at edges, we can refine
    // source clipping later. Squashed is better than dropping frames.

    // If resolution scaling is enabled (e.g. 4K -> 1080p), we must scale the
    // cursor coordinates to match the output texture dimensions. The
    // VideoProcessor applies the cursor overlay to the DESTINATION surface.
    if (scalingEnabled) {
      float scaleX = (float)outputWidth / (float)width;
      float scaleY = (float)outputHeight / (float)height;

      cursorRect.left = (LONG)(cursorRect.left * scaleX);
      cursorRect.top = (LONG)(cursorRect.top * scaleY);
      cursorRect.right = (LONG)(cursorRect.right * scaleX);
      cursorRect.bottom = (LONG)(cursorRect.bottom * scaleY);
    }

    // Clipping bounds use OUTPUT dimensions when scaling
    int frameW = scalingEnabled ? outputWidth : width;
    int frameH = scalingEnabled ? outputHeight : height;
    RECT frameRect = {0, 0, frameW, frameH};
    RECT clippedRect;
    if (IntersectRect(&clippedRect, &frameRect, &cursorRect)) {
      // Only draw if visible
      videoContext->VideoProcessorSetStreamDestRect(videoProcessor, 1, TRUE,
                                                    &clippedRect);
      videoContext->VideoProcessorSetStreamAlpha(videoProcessor, 1, TRUE, 1.0f);

      streams[1].Enable = TRUE;
      streams[1].pInputSurface = activeCursor->inputView;
      streamCount = 2;
    } else {
      // Cursor completely out of bounds - don't draw
      // (streams[1].Enable is already FALSE by default init)
    }
  }

  // Perform the conversion using current buffer
  int bufIdx = currentNV12Buffer;
  hr = videoContext->VideoProcessorBlt(videoProcessor, outputViews[bufIdx], 0,
                                       streamCount, streams);
  localInputView->Release();

  if (FAILED(hr)) {
    DLL_Log("[VideoProcessor] Blt failed. HR=%x", hr);
    return false;
  }

  // Return current buffer and advance to next
  *nv12Output = nv12StagingTextures[bufIdx];
  nv12StagingTextures[bufIdx]->AddRef(); // Caller will release
  currentNV12Buffer = (currentNV12Buffer + 1) % nv12BufferCount;
  return true;
}

void VideoEncoder::CleanupVideoProcessor() {
  for (auto *view : outputViews) {
    if (view) {
      view->Release();
    }
  }
  for (auto *tex : nv12StagingTextures) {
    if (tex) {
      tex->Release();
    }
  }
  outputViews.clear();
  nv12StagingTextures.clear();
  currentNV12Buffer = 0;

  // Cleanup cursor overlay resources (LRU cache)
  CleanupCursorCache();

  if (inputView) {
    inputView->Release();
    inputView = nullptr;
  }
  if (videoProcessor) {
    videoProcessor->Release();
    videoProcessor = nullptr;
  }
  if (videoProcessorEnum) {
    videoProcessorEnum->Release();
    videoProcessorEnum = nullptr;
  }
  if (videoContext) {
    videoContext->Release();
    videoContext = nullptr;
  }
  if (videoDevice) {
    videoDevice->Release();
    videoDevice = nullptr;
  }
  videoProcessorInit = false;
}

// ============================================================================
// LRU Cursor Cache Implementation
// ============================================================================

void VideoEncoder::CleanupCursorCache() {
  for (int i = 0; i < kCursorCacheSize; i++) {
    auto &entry = cursorCache[i];
    if (entry.inputView) {
      entry.inputView->Release();
      entry.inputView = nullptr;
    }
    if (entry.texture) {
      entry.texture->Release();
      entry.texture = nullptr;
    }
    entry.handle = nullptr;
    entry.width = 0;
    entry.height = 0;
    entry.hotspotX = 0;
    entry.hotspotY = 0;
    entry.lastUsedFrame = 0;
  }
  activeCursor = nullptr;
  cursorFrameCounter = 0;
}

VideoEncoder::CursorCacheEntry *
VideoEncoder::GetCursorCacheEntry(HCURSOR handle) {
  if (!handle)
    return nullptr;

  // VideoProcessor must be initialized before we can create cursor input views
  // On first frame, this hasn't happened yet - return existing cached entries
  // only
  if (!videoProcessorInit || !videoDevice || !videoProcessorEnum) {
    // Check if cursor is already cached (can still return cached entries)
    for (int i = 0; i < kCursorCacheSize; i++) {
      if (cursorCache[i].handle == handle && cursorCache[i].texture &&
          cursorCache[i].inputView) {
        return &cursorCache[i];
      }
    }
    return nullptr; // Can't create new cache entries without VideoProcessor
  }

  cursorFrameCounter++;

  // 1. Look for existing cache entry
  for (int i = 0; i < kCursorCacheSize; i++) {
    if (cursorCache[i].handle == handle && cursorCache[i].texture) {
      cursorCache[i].lastUsedFrame = cursorFrameCounter;
      return &cursorCache[i];
    }
  }

  // 2. Find empty slot or LRU slot
  int targetIdx = 0;
  uint64_t oldestFrame = UINT64_MAX;

  for (int i = 0; i < kCursorCacheSize; i++) {
    if (cursorCache[i].handle == nullptr) {
      targetIdx = i;
      break;
    }
    if (cursorCache[i].lastUsedFrame < oldestFrame) {
      oldestFrame = cursorCache[i].lastUsedFrame;
      targetIdx = i;
    }
  }

  // 3. Evict old entry if needed
  auto &entry = cursorCache[targetIdx];
  if (entry.inputView) {
    entry.inputView->Release();
    entry.inputView = nullptr;
  }
  if (entry.texture) {
    entry.texture->Release();
    entry.texture = nullptr;
  }

  // 4. Create new cursor texture
  if (!cursorRenderer)
    return nullptr;

  HICON icon = CopyIcon(handle);
  if (!icon)
    return nullptr;

  // Get hotspot info
  ICONINFO ii;
  if (GetIconInfo(icon, &ii)) {
    entry.hotspotX = ii.xHotspot;
    entry.hotspotY = ii.yHotspot;
    DeleteObject(ii.hbmColor);
    DeleteObject(ii.hbmMask);
  }

  uint8_t *bitmap = nullptr;
  uint32_t w, h;
  bool mono;

  if (!cursorRenderer->ExtractCursorBitmap(icon, &bitmap, &w, &h, &mono)) {
    DestroyIcon(icon);
    return nullptr;
  }
  DestroyIcon(icon);

  // Create D3D11 texture
  D3D11_TEXTURE2D_DESC texDesc = {};
  texDesc.Width = w;
  texDesc.Height = h;
  texDesc.MipLevels = 1;
  texDesc.ArraySize = 1;
  texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  texDesc.SampleDesc.Count = 1;
  texDesc.Usage = D3D11_USAGE_DEFAULT;
  texDesc.BindFlags = 0;

  D3D11_SUBRESOURCE_DATA initData = {};
  initData.pSysMem = bitmap;
  initData.SysMemPitch = w * 4;

  ID3D11Device *baseDevice = nullptr;
  d3d11Device->QueryInterface(IID_PPV_ARGS(&baseDevice));
  HRESULT hr = baseDevice->CreateTexture2D(&texDesc, &initData, &entry.texture);
  baseDevice->Release();
  delete[] bitmap;

  if (FAILED(hr)) {
    DLL_Log("[CursorCache] CreateTexture2D failed: HR=%x", hr);
    return nullptr;
  }

  // Create VP input view
  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivDesc = {};
  ivDesc.FourCC = 0;
  ivDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  ivDesc.Texture2D.MipSlice = 0;

  hr = videoDevice->CreateVideoProcessorInputView(
      entry.texture, videoProcessorEnum, &ivDesc, &entry.inputView);

  if (FAILED(hr)) {
    DLL_Log("[CursorCache] CreateVPInputView failed: HR=%x", hr);
    entry.texture->Release();
    entry.texture = nullptr;
    return nullptr;
  }

  entry.handle = handle;
  entry.width = w;
  entry.height = h;
  entry.lastUsedFrame = cursorFrameCounter;

  static int cacheHits = 0, cacheMisses = 0;
  cacheMisses++;
  if ((cacheHits + cacheMisses) % 100 == 0) {
    DLL_Log("[CursorCache] Stats: hits=%d misses=%d (%.1f%% hit rate)",
            cacheHits, cacheMisses,
            100.0 * cacheHits / (cacheHits + cacheMisses));
  }

  return &entry;
}

// ============================================================================
// CUDA Path Implementation (Optional, for NVIDIA GPUs)
// ============================================================================
#ifdef HAS_CUDA

bool VideoEncoder::InitCudaPath() {
  DLL_Log("[VideoEncoder] Initializing CUDA path...");

  cudaInterop = new CudaInterop();
  if (!cudaInterop->Init(luidLow, luidHigh)) {
    delete cudaInterop;
    cudaInterop = nullptr;
    return false;
  }

  // Create CUDA hardware context for FFmpeg
  cudaDeviceCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_CUDA);
  if (!cudaDeviceCtx) {
    DLL_Log("[VideoEncoder] Failed to alloc CUDA device context");
    return false;
  }

  AVHWDeviceContext *hwDevCtx = (AVHWDeviceContext *)cudaDeviceCtx->data;
  AVCUDADeviceContext *cudaCtx = (AVCUDADeviceContext *)hwDevCtx->hwctx;

  // Note: cudaCtx requires a CUcontext; we let FFmpeg create one
  if (av_hwdevice_ctx_init(cudaDeviceCtx) < 0) {
    DLL_Log("[VideoEncoder] Failed to init CUDA device context");
    av_buffer_unref(&cudaDeviceCtx);
    return false;
  }

  // Set CUDA pixel format in codec context
  codecCtx->pix_fmt = AV_PIX_FMT_CUDA;
  codecCtx->hw_device_ctx = av_buffer_ref(cudaDeviceCtx);

  // Create CUDA frames context
  cudaFramesCtx = av_hwframe_ctx_alloc(cudaDeviceCtx);
  AVHWFramesContext *framesCtx = (AVHWFramesContext *)cudaFramesCtx->data;
  framesCtx->format = AV_PIX_FMT_CUDA;
  framesCtx->sw_format = AV_PIX_FMT_NV12;
  framesCtx->width = width;
  framesCtx->height = height;
  framesCtx->initial_pool_size = 4;

  if (av_hwframe_ctx_init(cudaFramesCtx) < 0) {
    DLL_Log("[VideoEncoder] Failed to init CUDA frames context");
    av_buffer_unref(&cudaFramesCtx);
    av_buffer_unref(&cudaDeviceCtx);
    // CRITICAL: Reset codecCtx for D3D11 fallback
    av_buffer_unref(&codecCtx->hw_device_ctx);
    codecCtx->pix_fmt = AV_PIX_FMT_NONE;
    return false;
  }
  codecCtx->hw_frames_ctx = av_buffer_ref(cudaFramesCtx);

  // Update codec dimensions
  codecCtx->width = width;
  codecCtx->height = height;
  codecCtx->max_b_frames = 0;

  const AVCodec *codec = codecCtx->codec;
  DLL_Log("[VideoEncoder] Opening Codec (CUDA path)...");
  int ret = avcodec_open2(codecCtx, codec, nullptr);
  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
    DLL_Log("[VideoEncoder] Failed to open codec (CUDA): %s", errbuf);
    return false;
  }
  DLL_Log("[VideoEncoder] Codec Opened (CUDA path).");

  stream = avformat_new_stream(fmtCtx, codec);
  avcodec_parameters_from_context(stream->codecpar, codecCtx);
  stream->time_base = codecCtx->time_base;
  stream->avg_frame_rate = codecCtx->framerate;
  stream->r_frame_rate = codecCtx->framerate;

  initDone = true;
  DLL_Log("[VideoEncoder] CUDA path initialized successfully");
  return true;
}

bool VideoEncoder::EncodeFrameCuda(HANDLE sharedHandle, uint64_t fenceValue,
                                   int64_t pts, uint32_t pid,
                                   uint32_t frameWidth, uint32_t frameHeight) {
  if (!cudaInterop)
    return false;

  if (!fileOpened) {
    DLL_Log("[VideoEncoder] Opening Output File: %s (CUDA)",
            outputFilename.c_str());
    if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
      int ret = avio_open(&fmtCtx->pb, outputFilename.c_str(), AVIO_FLAG_WRITE);
      if (ret < 0) {
        DLL_Log("Failed to open output file: %d", ret);
        return false;
      }
    }
    if (avformat_write_header(fmtCtx, nullptr) < 0) {
      DLL_Log("Failed to write header");
      return false;
    }
    fileOpened = true;
  }

  // Import D3D12 texture to CUDA
  if (!cudaInterop->ImportD3D12Texture(sharedHandle, frameWidth, frameHeight,
                                       pid)) {
    DLL_Log("[VideoEncoder] CUDA: Failed to import D3D12 texture");
    return false;
  }

  // Convert BGRA to NV12 using CUDA kernel
  CUdeviceptr nv12Ptr = 0;
  size_t nv12Pitch = 0;
  if (!cudaInterop->ConvertBGRAtoNV12(&nv12Ptr, &nv12Pitch)) {
    DLL_Log("[VideoEncoder] CUDA: Failed to convert BGRA to NV12");
    return false;
  }

  // Create AVFrame with CUDA data
  AVFrame *cudaFrame = av_frame_alloc();
  cudaFrame->format = AV_PIX_FMT_CUDA;
  cudaFrame->width = width;
  cudaFrame->height = height;
  cudaFrame->hw_frames_ctx = av_buffer_ref(cudaFramesCtx);

  // Get a frame from the pool and copy our NV12 data
  if (av_hwframe_get_buffer(cudaFramesCtx, cudaFrame, 0) < 0) {
    DLL_Log("[VideoEncoder] CUDA: Failed to get hw frame buffer");
    av_frame_free(&cudaFrame);
    return false;
  }

  // Copy NV12 data to the frame (data[0] = Y, data[1] = UV)
  // Note: We're using the CUDA device pointer directly
  cudaFrame->data[0] = (uint8_t *)nv12Ptr;
  cudaFrame->data[1] = (uint8_t *)(nv12Ptr + nv12Pitch * height);
  cudaFrame->linesize[0] = (int)nv12Pitch;
  cudaFrame->linesize[1] = (int)nv12Pitch;

  // Calculate PTS
  if (startPts < 0) {
    startPts = pts;
    DLL_Log("[VideoEncoder] Recording started at PTS %lld (CUDA)", startPts);
  }
  int64_t relativePts_ms = pts - startPts;
  if (relativePts_ms < 0) {
    relativePts_ms = 0;
  }
  if (savedConfig.useVFR) {
    cudaFrame->pts = relativePts_ms * 1000;
  } else {
    int fps = (codecCtx && codecCtx->framerate.num > 0) ? codecCtx->framerate.num
                                                        : savedConfig.fps;
    if (fps <= 0) {
      fps = 60;
    }
    cudaFrame->pts = av_rescale(relativePts_ms, fps, 1000);
  }
  if (lastAssignedVideoPts >= 0 && cudaFrame->pts <= lastAssignedVideoPts) {
    cudaFrame->pts = lastAssignedVideoPts + 1;
  }
  lastAssignedVideoPts = cudaFrame->pts;

  // Encode
  AVPacket *pkt = av_packet_alloc();
  int packetCount = 0;

  auto drainPackets = [&]() {
    while (true) {
      int ret = avcodec_receive_packet(codecCtx, pkt);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        break;
      if (ret < 0)
        break;
      packetCount++;
      pkt->stream_index = stream->index;

      // CRITICAL: Set packet duration to 1 frame in codec time_base
      pkt->duration = 1;

      if (onPacket)
        onPacket(pkt);
      av_packet_unref(pkt);
    }
  };

  drainPackets();

  int ret = avcodec_send_frame(codecCtx, cudaFrame);
  int retries = 0;
  while (ret == AVERROR(EAGAIN) && retries < 10) {
    drainPackets();
    ret = avcodec_send_frame(codecCtx, cudaFrame);
    retries++;
  }

  drainPackets();
  av_packet_free(&pkt);

  static int framesSent = 0;
  static int totalPackets = 0;
  framesSent++;
  totalPackets += packetCount;
  if (framesSent % 60 == 0) {
    DLL_Log("[VideoEncoder] CUDA Stats: %d frames sent, %d packets "
            "produced",
            framesSent, totalPackets);
  }

  av_frame_free(&cudaFrame);
  return true;
}

void VideoEncoder::CleanupCuda() {
  if (cudaInterop) {
    cudaInterop->Cleanup();
    delete cudaInterop;
    cudaInterop = nullptr;
  }
  useCudaPath = false;
}

#endif // HAS_CUDA


int64_t VideoEncoder::GetExpectedFinalDurationUs() const {
  if (lastAssignedVideoPts < 0) return 0;
  
  if (savedConfig.useVFR) {
    return lastAssignedVideoPts + (1000000 / (savedConfig.fps > 0 ? savedConfig.fps : 60));
  } else {
    int fps = (codecCtx && codecCtx->framerate.num > 0) ? codecCtx->framerate.num : savedConfig.fps;
    if (fps <= 0) fps = 60;
    return av_rescale(lastAssignedVideoPts + 1, 1000000, fps);
  }
}

int64_t VideoEncoder::GetEncodedDurationUs() const {
  int64_t encodedUs = encodedDurationUs.load(std::memory_order_relaxed);
  if (encodedUs > 0) {
    return encodedUs;
  }

  if (!codecCtx || codecCtx->framerate.num == 0)
    return 0;

  // Fallback for early startup before first packet is emitted.
  return av_rescale(encodeFrameCounter,
                    1000000 * (int64_t)codecCtx->framerate.den,
                    codecCtx->framerate.num);
}

int64_t VideoEncoder::GetLastFrameEncodeTimeUs() const {
  return lastEncodeTimeUs;
}

int64_t VideoEncoder::GetLastFrameFenceWaitUs() const {
  return lastFenceWaitUs;
}
// Async Packet Writer Loop
void VideoEncoder::AsyncWriteLoop() {
  DLL_Log("[VideoEncoder] Async Writer Thread Started");

  while (writerRunning || isStopping) {
    std::unique_lock<std::mutex> lock(queueMutex);

    // Wait for packets or stop signal
    queueCV.wait(lock, [this] {
      return !packetQueue.empty() || isStopping || !writerRunning;
    });

    // Drain queue
    while (!packetQueue.empty()) {
      AVPacket *pkt = packetQueue.front();
      packetQueue.pop();

      size_t pktSize = pkt->size + sizeof(AVPacket);
      currentQueueBytes -= pktSize;

      lock.unlock(); // Release lock while doing I/O

      if (fileOpened && fmtCtx) {
        int ret = av_interleaved_write_frame(fmtCtx, pkt);
        if (ret < 0) {
          static int writeErrorCount = 0;
          if (writeErrorCount++ < 10) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            DLL_Log("[VideoEncoder] ERROR: av_interleaved_write_frame failed: "
                    "%d (%s) pts=%lld",
                    ret, errbuf, pkt->pts);
          }
        }
      }

      av_packet_free(&pkt);
      lock.lock(); // Re-acquire lock
    }

    // Handle Stop/Flush signal
    if (isStopping) {
      DLL_Log("[VideoEncoder] Async Finalize: Starting...");

      // 1. Flush Encoder if valid
      if (initDone && codecCtx && fileOpened) {
        DLL_Log("[VideoEncoder] Async Finalize: Flushing encoder...");
        avcodec_send_frame(codecCtx, nullptr);

        AVPacket *pkt = av_packet_alloc();
        int flushedCount = 0;
        while (avcodec_receive_packet(codecCtx, pkt) == 0) {
          // We need to set stream index and rescale PTS here
          // Note: We use the same write logic as WriteFrame but simplified
          pkt->stream_index = stream->index;

          av_packet_rescale_ts(pkt, codecCtx->time_base, stream->time_base);
          if (pkt->dts == AV_NOPTS_VALUE) {
            pkt->dts = pkt->pts;
          }
          if (pkt->duration <= 0) {
            int64_t duration =
                av_rescale_q(1, codecCtx->time_base, stream->time_base);
            pkt->duration = duration > 0 ? duration : 1;
          }

          if (pkt->pts != AV_NOPTS_VALUE) {
            int64_t packetEnd = pkt->pts + pkt->duration;
            int64_t packetEndUs = av_rescale_q(packetEnd, stream->time_base,
                                               AVRational{1, 1000000});
            int64_t prevEndUs = encodedDurationUs.load(std::memory_order_relaxed);
            if (packetEndUs > prevEndUs) {
              encodedDurationUs.store(packetEndUs, std::memory_order_relaxed);
            }
          }

          av_interleaved_write_frame(fmtCtx, pkt);
          av_packet_unref(pkt);
          flushedCount++;
        }
        av_packet_free(&pkt);
        DLL_Log("[VideoEncoder] Async Finalize: Flushed %d remaining packets",
                flushedCount);
      }

      // 2. Write Trailer and Close File
      if (fmtCtx && fileOpened) {
        DLL_Log("[VideoEncoder] Async Finalize: Writing Trailer...");
        // Set container duration so seekers and players see a valid duration
        int64_t finalDurationUs = encodedDurationUs.load(std::memory_order_relaxed);
        if (finalDurationUs > 0) {
          fmtCtx->duration = av_rescale_q(finalDurationUs, AVRational{1, 1000000}, AV_TIME_BASE_Q);
          DLL_Log("[VideoEncoder] Async Finalize: Container duration set to %lld us", finalDurationUs);
        }
        av_write_trailer(fmtCtx);
        if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
          avio_closep(&fmtCtx->pb);
        }
        fileOpened = false;
        DLL_Log("[VideoEncoder] Async Finalize: Output file closed.");
      }

      // IMPORTANT: we still hold queueMutex (lock) here.
      // CleanupResources() also locks queueMutex to drain packetQueue.
      // Unlock first to avoid self-deadlock during finalize.
      lock.unlock();

      CleanupResources();

      isStopping = false;
      writerRunning = false;
      DLL_Log("[VideoEncoder] Async Finalize: Complete.");
      break; // Exit thread
    }
  }

  DLL_Log("[VideoEncoder] Async Writer Thread Stopped");
}
