#include "video_encoder.h"
#include "../common/raii_helpers.h"
#include "../common/shared_defs.h"
#include "mediaengine.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
}

#include <d3d11_4.h>
#include <dxgi1_5.h>
#include <chrono>
#include <functional>
#include <unordered_map>

#include <filesystem>
#include "cursor_renderer.h"

// D3D11 exception safety for MinGW/clang
// MinGW uses DWARF exception handling (libgcc) which cannot catch Windows SEH
// exceptions. D3D11 raises SEH exceptions (e.g., 0xE06D7363) for invalid handles.
//
// Protection strategy:
// 1. HandleFailureCache tracks handles that have failed - skip them on retry
// 2. DuplicateHandle-first validates handles before calling D3D11
// 3. dllexport wrapper functions prevent LTO from stripping error paths

#include <windows.h>

// Handle validation cache: tracks handles that have previously failed D3D11 OpenShared*
// calls, so we don't repeatedly trigger SEH exceptions from invalid handles.
// D3D11 throws SEH exceptions for invalid handles, and MinGW's catch(...) cannot
// catch SEH exceptions. Pre-validation is the only reliable protection.
// 
// The cache stores (handle_value, failure_count) pairs. Handles that fail >3 times
// are permanently skipped. Cache is cleared when recording starts.
#include <unordered_map>
#include <mutex>

struct HandleFailureCache {
    std::mutex mutex;
    std::unordered_map<HANDLE, int> fenceFailures;
    std::unordered_map<HANDLE, int> textureFailures;
    
    bool ShouldSkipFence(HANDLE h) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = fenceFailures.find(h);
        return it != fenceFailures.end() && it->second >= 3;
    }
    
    bool ShouldSkipTexture(HANDLE h) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = textureFailures.find(h);
        return it != textureFailures.end() && it->second >= 3;
    }
    
    void RecordFenceFailure(HANDLE h) {
        std::lock_guard<std::mutex> lock(mutex);
        fenceFailures[h]++;
    }
    
    void RecordTextureFailure(HANDLE h) {
        std::lock_guard<std::mutex> lock(mutex);
        textureFailures[h]++;
    }
    
    void Clear() {
        std::lock_guard<std::mutex> lock(mutex);
        fenceFailures.clear();
        textureFailures.clear();
    }
};

static HandleFailureCache g_HandleFailureCache;

// D3D11 OpenSharedFence/OpenSharedResource can throw SEH exceptions (0xE06D7363)
// for invalid handles. On MinGW, C++ try/catch cannot catch SEH exceptions.
// We use the VEH handler above for diagnostics. The real protection is to validate
// handles BEFORE calling D3D11 APIs - check if the handle is a valid NT handle,
// check if the source process is still alive, and cache results to avoid repeated
// failures that would trigger exceptions.
// for invalid handles. LTO (-flto) was stripping exception tables from lambdas.
// Fixed by: (1) disabling LTO for mediaengine in build.py, (2) using noinline wrappers.
// The noinline wrappers force exception table emission even if LTO is re-enabled later.

// Forward declarations - dllexport forces LTO to keep these functions
// D3D11 OpenShared* calls can throw SEH exceptions (0xE06D7363) for invalid handles.
// On MinGW, catch(...) CANNOT catch SEH exceptions. The failure cache pre-validates
// handles that have previously failed to prevent repeated crashes.
// CRITICAL: OpenSharedFence MUST only be called with handles that are known to be valid.
// The caller must use DuplicateHandle first - if it succeeds, the handle is valid.
// We also use the failure cache as a second line of defense.
extern "C" __declspec(dllexport) HRESULT __cdecl CallOpenSharedFence(ID3D11Device5* dev, HANDLE h,
                                                                      ID3D11Fence** out) {
    if (g_HandleFailureCache.ShouldSkipFence(h)) {
        return E_INVALIDARG;
    }
    HRESULT hr = dev->OpenSharedFence(h, IID_PPV_ARGS(out));
    if (FAILED(hr)) {
        g_HandleFailureCache.RecordFenceFailure(h);
    }
    return hr;
}

extern "C" __declspec(dllexport) HRESULT __cdecl CallOpenSharedResource(ID3D11Device5* dev, HANDLE h, REFIID riid,
                                                                         void** out) {
    HRESULT hr = dev->OpenSharedResource(h, riid, out);
    if (FAILED(hr)) {
        g_HandleFailureCache.RecordTextureFailure(h);
    }
    return hr;
}

extern "C" __declspec(dllexport) HRESULT __cdecl CallOpenSharedResource1(ID3D11Device5* dev, HANDLE h, REFIID riid,
                                                                          void** out) {
    HRESULT hr = dev->OpenSharedResource1(h, riid, out);
    if (FAILED(hr)) {
        g_HandleFailureCache.RecordTextureFailure(h);
    }
    return hr;
}

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
            ce::HandleGuard hProcess(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, sourcePid));
            if (hProcess) {
                BOOL wow64 = FALSE;
                if (IsWow64Process(hProcess.get(), &wow64)) {
                    isWow64Source = (wow64 == TRUE);
                }
            }
            s_isWow64ByPid[sourcePid] = isWow64Source;
        }
    }

    const uint64_t rawHandle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
    if (!isWow64Source) {
        // Some drivers publish KMT handles with bit31 set but without canonical
        // sign-extension in 64-bit IPC transport.
        if ((rawHandle >> 32) == 0 && (rawHandle & 0x80000000ull) != 0) {
            const int64_t signExtended = static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(rawHandle)));
            if (signExtended != static_cast<int64_t>(rawHandle)) {
                static std::atomic<int> s_canonicalizeLogCount{0};
                if (s_canonicalizeLogCount.fetch_add(1, std::memory_order_relaxed) < 6) {
                    DLL_Log("[VideoEncoder] Canonicalizing shared handle for PID %u: %p -> %p", sourcePid,
                            (HANDLE)(uintptr_t)rawHandle, (HANDLE)(uint64_t)signExtended);
                }
                return reinterpret_cast<HANDLE>(static_cast<uint64_t>(signExtended));
            }
        }
        return handle;
    }

    const int64_t signExtended = static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(rawHandle)));
    if (signExtended != static_cast<int64_t>(rawHandle)) {
        static std::atomic<int> s_normalizeLogCount{0};
        if (s_normalizeLogCount.fetch_add(1, std::memory_order_relaxed) < 6) {
            DLL_Log("[VideoEncoder] WOW64 handle normalized for PID %u: %p -> %p", sourcePid,
                    (HANDLE)(uintptr_t)rawHandle, (HANDLE)(uint64_t)signExtended);
        }
    }
    return reinterpret_cast<HANDLE>(static_cast<uint64_t>(signExtended));
#else
    (void)sourcePid;
    return handle;
#endif
}

// Helper to generate robust output filename
static std::string GenerateOutputFilename(const VideoConfig& config) {
    // Default to "captures" subdirectory if outputDir is not specified
    fs::path outDir = config.outputDir;
    if (outDir.empty()) {
        outDir = "captures";
    }

    // Create directory if it doesn't exist
    std::error_code ec;
    if (!fs::exists(outDir, ec)) {
        if (fs::create_directories(outDir, ec)) {
            DLL_Log("[VideoEncoder] Created output directory: %s", outDir.string().c_str());
        } else {
            DLL_Log(
                "[VideoEncoder] Failed to create output directory: %s (Error: "
                "%d). Fallback to current dir.",
                outDir.string().c_str(), ec.value());
            outDir = ".";
        }
    } else {
        // DLL_Log("[VideoEncoder] Output directory exists: %s",
        // outDir.string().c_str());
    }

    std::string filenameOnly = "capture_" + std::to_string(GetTickCount64()) + "." + config.container;
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
    D3D11ScopedLock() {
        MediaEngine_LockD3D11();
    }
    ~D3D11ScopedLock() {
        MediaEngine_UnlockD3D11();
    }
};

// Performance timing helper for pipeline analysis
class PerfTimer {
public:
    using Clock = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;

    static TimePoint now() {
        return Clock::now();
    }

    static double elapsed_ms(const TimePoint& start, const TimePoint& end) {
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
    int64_t expectedPtsDiff = 0;  // Expected ms between frames
    int64_t actualPtsDiff = 0;    // Actual ms between frames
};

// Global stats for frame analysis
static int64_t g_lastFramePts = -1;
static int64_t g_framesEncoded = 0;
// static int64_t g_framesDropped = 0;
static double g_totalFenceWait = 0;
static double g_totalColorConvert = 0;
static double g_totalEncode = 0;
static double g_maxFrameTime = 0;
static int g_slowFrameCount = 0;  // Frames taking > 2x expected time

// Helper to release D3D11 Texture when AVFrame is freed
static void FreeD3D11Tex(void* opaque, uint8_t* data) {
    ID3D11Texture2D* tex = (ID3D11Texture2D*)data;
    if (tex)
        tex->Release();
}

VideoEncoder::VideoEncoder()
    : fmtCtx(nullptr),
      codecCtx(nullptr),
      stream(nullptr),
      hwDeviceCtx(nullptr),
      hwFramesCtx(nullptr),
      d3d11DeviceCtx(nullptr),
      d3d11FramesCtx(nullptr),
      d3d11Device(nullptr),
      d3d11Context(nullptr),
      luidLow(0),
      luidHigh(0),
      initDone(false),
      currentIsHDR(false),
      fileOpened(false),
      recordingRequested(false),
      isStopping(false),
      flushRequested(false),
      codecOpenFailed(false),
      startPts(-1),
      width(0),
      height(0),
      cachedSourcePid(0),
      lastEncodeTimeUs(0),
      fenceEvent(nullptr),
      videoDevice(nullptr),
      videoContext(nullptr),
      videoProcessor(nullptr),
      videoProcessorEnum(nullptr),
      currentNV12Buffer(0),
      inputView(nullptr),
      videoProcessorInit(false) {}

VideoEncoder::~VideoEncoder() {
    Stop();  // Triiger async stop

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

bool VideoEncoder::Init(const VideoConfig& config, int width, int height, int fps,
                         std::function<void(AVPacket*)> packetCallback) {
    // Clear handle failure cache from previous recording session
    g_HandleFailureCache.Clear();
    DLL_Log("[VideoEncoder] Init Entry - config.encoder=%s w=%d h=%d fps=%d", config.encoder.c_str(), width, height,
            fps);

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
    av_log_set_level(AV_LOG_WARNING);

    DLL_Log("[VideoEncoder] Step 3: Creating output filename");
    outputFilename = GenerateOutputFilename(config);
    DLL_Log("[VideoEncoder] Output file: %s", outputFilename.c_str());

    DLL_Log("[VideoEncoder] Step 4: Calling avformat_alloc_output_context2");
    if (avformat_alloc_output_context2(&fmtCtx, nullptr, nullptr, outputFilename.c_str()) < 0) {
        DLL_Log("[VideoEncoder] Failed to alloc output context");
        return false;
    }
    DLL_Log("[VideoEncoder] Step 4 done, fmtCtx=%p", (void*)fmtCtx);

    DLL_Log("[VideoEncoder] Step 5: Finding encoder: %s", config.encoder.c_str());
    const AVCodec* codec = avcodec_find_encoder_by_name(config.encoder.c_str());
    if (!codec) {
        DLL_Log("[VideoEncoder] Codec not found: %s", config.encoder.c_str());
        return false;
    }
    DLL_Log("[VideoEncoder] Step 5 done, codec=%p name=%s", (void*)codec, codec->name);

    DLL_Log("[VideoEncoder] Step 6: Allocating codec context");
    codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) {
        DLL_Log("[VideoEncoder] Failed to alloc codec context");
        return false;
    }
    DLL_Log("[VideoEncoder] Step 6 done, codecCtx=%p", (void*)codecCtx);

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

bool VideoEncoder::CreateSharedCaptureTextures(uint32_t w, uint32_t h, uint32_t fmt, SharedMemoryLayout* sharedMem) {
    if (sharedCaptureTexturesCreated) {
        if (sharedCaptureTextureFormat == fmt) {
            return true;  // Already created with same format
        }
        // Format changed (e.g. DX9 BGRA→DX11 RGBA) — destroy and recreate
        DLL_Log("[VideoEncoder] KMT texture format changed %d -> %d, recreating", sharedCaptureTextureFormat, fmt);
        for (int i = 0; i < 4; i++) {
            if (sharedCaptureTextures[i]) {
                sharedCaptureTextures[i]->Release();
                sharedCaptureTextures[i] = nullptr;
            }
            sharedCaptureKmtHandles[i] = nullptr;
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
    }

    if (!d3d11Device) {
        DLL_Log("[VideoEncoder] CreateSharedCaptureTextures: No D3D11 device");
        return false;
    }

    DLL_Log("[VideoEncoder] Creating shared capture textures: %dx%d format=%d", w, h, fmt);

    // Create 4 KMT-only shared textures (global WDDM handles for DXVK Vulkan import)
    // AND 4 NT-handle shared textures (for non-DXVK Vulkan import)
    for (int i = 0; i < 4; i++) {
        // KMT-only texture (D3D11_RESOURCE_MISC_SHARED only)
        D3D11_TEXTURE2D_DESC kmtDesc = {};
        kmtDesc.Width = w;
        kmtDesc.Height = h;
        kmtDesc.MipLevels = 1;
        kmtDesc.ArraySize = 1;
        kmtDesc.Format = (DXGI_FORMAT)fmt;
        kmtDesc.SampleDesc.Count = 1;
        kmtDesc.SampleDesc.Quality = 0;
        kmtDesc.Usage = D3D11_USAGE_DEFAULT;
        kmtDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        kmtDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

        HRESULT hr = d3d11Device->CreateTexture2D(&kmtDesc, nullptr, &sharedCaptureTextures[i]);
        if (FAILED(hr)) {
            DLL_Log("[VideoEncoder] Failed to create KMT shared texture %d: HR=%x", i, hr);
            return false;
        }

        // Get KMT handle via IDXGIResource::GetSharedHandle
        IDXGIResource* dxgiRes = nullptr;
        hr = sharedCaptureTextures[i]->QueryInterface(IID_PPV_ARGS(&dxgiRes));
        if (FAILED(hr) || !dxgiRes) {
            DLL_Log("[VideoEncoder] Failed to get IDXGIResource for KMT texture %d: HR=%x", i, hr);
            return false;
        }

        hr = dxgiRes->GetSharedHandle(&sharedCaptureKmtHandles[i]);
        dxgiRes->Release();

        if (FAILED(hr) || !sharedCaptureKmtHandles[i]) {
            DLL_Log("[VideoEncoder] Failed to get KMT handle for texture %d: HR=%x", i, hr);
            return false;
        }

        DLL_Log("[VideoEncoder] Created KMT shared texture %d, kmtHandle=%p", i, sharedCaptureKmtHandles[i]);
    }

    // Create event for CPU-side fence waiting
    fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    // Create shared fence
    HRESULT hr = d3d11Device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&sharedCaptureFence));
    if (FAILED(hr)) {
        DLL_Log("[VideoEncoder] Failed to create shared fence: HR=%x", hr);
        return false;
    }

    // Export fence handle - CreateSharedHandle is on the fence object, not the
    // device
    hr = sharedCaptureFence->CreateSharedHandle(nullptr,      // Security attributes
                                                GENERIC_ALL,  // Access rights
                                                nullptr,      // Name (optional)
                                                &sharedCaptureFenceHandle);
    if (FAILED(hr)) {
        DLL_Log("[VideoEncoder] Failed to export fence handle: HR=%x", hr);
        return false;
    }

    DLL_Log("[VideoEncoder] Created shared fence, handle=%p", sharedCaptureFenceHandle);

    // Publish to shared memory
    if (sharedMem) {
        this->pSharedMem = sharedMem;
        for (int i = 0; i < 4; i++) {
            sharedMem->encoderTextures.SetKmtTextureHandle(i, (uint64_t)sharedCaptureKmtHandles[i]);
        }
        sharedMem->encoderTextures.SetFenceHandle((uint64_t)sharedCaptureFenceHandle);
        sharedMem->encoderTextures.SetWidth(w);
        sharedMem->encoderTextures.SetHeight(h);
        sharedMem->encoderTextures.SetFormat(fmt);
        sharedMem->encoderTextures.kmtReady.store(true, std::memory_order_release);
        sharedMem->encoderTextures.ready.store(true, std::memory_order_release);
        DLL_Log("[VideoEncoder] Published encoder KMT textures to shared memory");
    }

    sharedCaptureTextureFormat = fmt;
    sharedCaptureTexturesCreated = true;
    return true;
}

bool VideoEncoder::ConfigureAndOpenCodec() {
    if (!codecCtx || !fmtCtx) {
        DLL_Log("[VideoEncoder] ConfigureAndOpenCodec: Missing context(s)");
        return false;
    }

    const AVCodec* codec = codecCtx->codec;
    if (!codec) {
        codec = avcodec_find_encoder_by_name(savedConfig.encoder.c_str());
        if (!codec) {
            DLL_Log("[VideoEncoder] ConfigureAndOpenCodec: Codec not found");
            return false;
        }
    }

    // Build encoder options from savedConfig
    AVDictionary* opts = nullptr;

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
    DLL_Log("[VideoEncoder] lookahead=%s", savedConfig.lookahead ? "true" : "false");
    DLL_Log("[VideoEncoder] aq=%s", savedConfig.aq ? "true" : "false");
    DLL_Log("[VideoEncoder] b_frames=%d", savedConfig.bFrames);
    DLL_Log("[VideoEncoder] multipass=%s", savedConfig.multipass.c_str());
    DLL_Log("[VideoEncoder] keyframe_interval=%d", savedConfig.keyframeInterval);
    DLL_Log("[VideoEncoder] qp=%d", savedConfig.qp);
    DLL_Log("[VideoEncoder] bit_depth=%s color_space=%s color_range=%s chroma=%s", savedConfig.bitDepth.c_str(),
            savedConfig.colorSpace.c_str(), savedConfig.colorRange.c_str(), savedConfig.chromaSubsampling.c_str());
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
        codecCtx->color_trc = currentIsHDR ? AVCOL_TRC_SMPTE2084 : AVCOL_TRC_BT2020_10;
        codecCtx->colorspace = AVCOL_SPC_BT2020_NCL;
    } else {
        codecCtx->color_primaries = AVCOL_PRI_BT709;
        codecCtx->color_trc = AVCOL_TRC_BT709;
        codecCtx->colorspace = AVCOL_SPC_BT709;
    }

    // Color range
    std::string cr = savedConfig.colorRange;
    if (cr == "auto" || cr.empty()) {
        codecCtx->color_range = AVCOL_RANGE_MPEG;  // TV/limited is standard
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
            // Use D3D11 HW path with P010 sw_format (set in EnsureDevice)
            // This allows NVENC to receive P010 textures via hardware frames
            codecCtx->pix_fmt = AV_PIX_FMT_D3D11;
        } else {
            codecCtx->pix_fmt = AV_PIX_FMT_D3D11;  // NV12 via D3D11 HW path
        }
    }

    DLL_Log(
        "[VideoEncoder] Color config: space=%s range=%s bitDepth=%s chroma=%s "
        "pixFmt=%d hdr=%d",
        cs.c_str(), cr.c_str(), bd.c_str(), chroma.c_str(), codecCtx->pix_fmt, currentIsHDR);

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

        if (isNVENC && (savedConfig.rateControl == "CQ" || savedConfig.rateControl == "CQP")) {
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
            } catch (...) {}
        if (bitrate_val > 0)
            codecCtx->bit_rate = bitrate_val;
    }

    if (!savedConfig.maxBitrate.empty()) {
        std::string maxbr = savedConfig.maxBitrate;
        int64_t maxbitrate_val = 0;
        if (maxbr.find("Mbps") != std::string::npos)
            maxbitrate_val = std::stoll(maxbr.substr(0, maxbr.find("Mbps"))) * 1000000;
        else if (maxbr.find("Kbps") != std::string::npos)
            maxbitrate_val = std::stoll(maxbr.substr(0, maxbr.find("Kbps"))) * 1000;
        else
            try {
                maxbitrate_val = std::stoll(maxbr);
            } catch (...) {}
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

    codecCtx->max_b_frames = (luidLow == 0 && luidHigh == 0) ? 0 : savedConfig.bFrames;
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
        DLL_Log("[VideoEncoder] Failed to open codec: %d. Error details: %s", ret, errbuf);
        codecOpenFailed = true;
        return false;
    }

    DLL_Log("[VideoEncoder] Codec Opened Successfully.");
    stream = avformat_new_stream(fmtCtx, codec);
    avcodec_parameters_from_context(stream->codecpar, codecCtx);
    stream->time_base = codecCtx->time_base;
    stream->avg_frame_rate = codecCtx->framerate;
    stream->r_frame_rate = codecCtx->framerate;

    for (auto& actx : audioContexts) {
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

    DLL_Log("[VideoEncoder] EnsureDevice with LUID: %08x %08x", luidLow, luidHigh);

    if (luidLow == 0 && luidHigh == 0) {
        DLL_Log(
            "[VideoEncoder] WARNING: EnsureDevice called with default LUID (0:0). "
            "In inject mode, this usually indicates a propagation failure.");
    }

    // D3D11 Video Processor is the only supported color conversion path
    // (D3D12 does not have an equivalent VideoProcessorBlt API)

    // 1. Find Adapter by LUID
    IDXGIAdapter* targetAdapter = nullptr;
    if (luidLow != 0 || luidHigh != 0) {
        LUID searchLuid;
        searchLuid.LowPart = (DWORD)luidLow;
        searchLuid.HighPart = (LONG)luidHigh;

        DLL_Log("[VideoEncoder] Searching for Adapter with LUID: %08x-%08x", searchLuid.HighPart, searchLuid.LowPart);

        IDXGIFactory4* factory4 = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory4)))) {
            if (SUCCEEDED(factory4->EnumAdapterByLuid(searchLuid, IID_PPV_ARGS(&targetAdapter)))) {
                DLL_Log("[VideoEncoder] Found Adapter matching LUID via IDXGIFactory4");
            }
            factory4->Release();
        }

        if (!targetAdapter) {
            IDXGIFactory1* factory = nullptr;
            if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
                IDXGIAdapter* adapter = nullptr;
                for (UINT i = 0; factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                    DXGI_ADAPTER_DESC desc;
                    adapter->GetDesc(&desc);
                    if (desc.AdapterLuid.LowPart == searchLuid.LowPart &&
                        desc.AdapterLuid.HighPart == searchLuid.HighPart) {
                        targetAdapter = adapter;
                        DLL_Log("[VideoEncoder] Found Adapter matching LUID via manual scan");
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
    extern ID3D11Device* g_SharedD3D11Device;
    extern ID3D11DeviceContext* g_SharedD3D11Context;

    // Skip device creation if already preserved (DXVK zero-copy across recordings)
    if (d3d11Device && d3d11Context) {
        DLL_Log("[VideoEncoder] Reusing existing D3D11 device (preserved for encoder textures)");
    } else if ((luidLow == 0 && luidHigh == 0) && g_SharedD3D11Device) {
        // Framegrab mode - use the shared device that ScreenCapture also uses
        DLL_Log("[VideoEncoder] Framegrab using dimensions: %dx%d", width, height);
        g_SharedD3D11Device->QueryInterface(IID_PPV_ARGS(&d3d11Device));
        g_SharedD3D11Context->QueryInterface(IID_PPV_ARGS(&d3d11Context));
        DLL_Log("[VideoEncoder] Using shared D3D11 device for framegrab");
    } else {
        // Inject mode - create device on specific adapter
        DLL_Log("[VideoEncoder] Creating D3D11 Device (Flags: BGRA + VIDEO)...");

        UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
// createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG; // Optional
#endif

        D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        D3D_FEATURE_LEVEL featureLevel;
        ID3D11Device* baseDevice = nullptr;
        ID3D11DeviceContext* baseContext = nullptr;

        HRESULT hr = D3D11CreateDevice(
            targetAdapter, targetAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, 0, createDeviceFlags,
            featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION, &baseDevice, &featureLevel, &baseContext);

        if (targetAdapter)
            targetAdapter->Release();

        if (FAILED(hr)) {
            DLL_Log("[VideoEncoder] D3D11CreateDevice Failed: 0x%x (Target: %p)", hr, targetAdapter);
            return false;
        }
        DLL_Log("[VideoEncoder] D3D11 Device Created (Feature Level: 0x%x)", featureLevel);

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

        AVHWDeviceContext* deviceCtx = (AVHWDeviceContext*)d3d11DeviceCtx->data;
        AVD3D11VADeviceContext* d3d11Ctx = (AVD3D11VADeviceContext*)deviceCtx->hwctx;
        d3d11Ctx->device = baseDevice;
        baseDevice->AddRef();

        if (av_hwdevice_ctx_init(d3d11DeviceCtx) < 0)
            return false;

        // baseDeviceGuard and baseContextGuard will auto-release on scope exit
    }  // End of else block (inject mode device creation)

    // Apply GPU priority from config to ensure encoder/game balance
    if (d3d11Device) {
        IDXGIDevice* dxgiDevice = nullptr;
        if (SUCCEEDED(d3d11Device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice))) {
            // Range -7 to 7 (DXGI_GPU_THREAD_PRIORITY_MAX/MIN)
            // Positive = higher than game, negative = lower than game
            // Value comes from config.ini [Performance] gpu_priority
            int priority = gpuPriority;  // Passed to Init() from config

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

    // CreateSharedCaptureTextures can run before Start() recreates codec/container
    // contexts after a previous Stop(). In that pre-start phase we only need the
    // D3D11 device for texture allocation; defer FFmpeg HW context wiring until
    // Start() has rebuilt fmtCtx/codecCtx.
    if (!codecCtx || !fmtCtx) {
        if (!recordingRequested) {
            DLL_Log(
                "[VideoEncoder] EnsureDevice: device-only init (fmtCtx=%p codecCtx=%p), "
                "deferring codec prewarm to Start()",
                (void*)fmtCtx, (void*)codecCtx);
            return true;
        }

        DLL_Log("[VideoEncoder] EnsureDevice failed: missing contexts while recording (fmtCtx=%p codecCtx=%p)",
                (void*)fmtCtx, (void*)codecCtx);
        return false;
    }

    // Set up FFmpeg HW device context with our D3D11 device (shared for both
    // paths)
    if (!d3d11DeviceCtx) {
        // 2. Wrap in AVHWDeviceContext - for screengrab mode using shared device
        d3d11DeviceCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!d3d11DeviceCtx)
            return false;

        AVHWDeviceContext* deviceCtx = (AVHWDeviceContext*)d3d11DeviceCtx->data;
        AVD3D11VADeviceContext* d3d11Ctx = (AVD3D11VADeviceContext*)deviceCtx->hwctx;

        // Get base device from our QI'd interface
        ce::ComGuard<ID3D11Device> baseDevice;
        if (FAILED(d3d11Device->QueryInterface(__uuidof(ID3D11Device), (void**)baseDevice.addressof()))) {
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
    AVHWFramesContext* d11Frames = (AVHWFramesContext*)d3d11FramesCtx->data;
    d11Frames->format = AV_PIX_FMT_D3D11;

    // Use P010 for 10-bit/HDR content, NV12 for 8-bit SDR
    bool use10bit = (savedConfig.bitDepth == "10") || (savedConfig.bitDepth == "auto" && currentIsHDR);
    d11Frames->sw_format = use10bit ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12;
    if (use10bit) {
        DLL_Log("[VideoEncoder] Using P010 (10-bit) sw_format for D3D11 HW frames");
    }

    int framesWidth = width;
    int framesHeight = height;
    if (savedConfig.scaling.enabled && savedConfig.scaling.outputWidth > 0 && savedConfig.scaling.outputHeight > 0) {
        framesWidth = savedConfig.scaling.outputWidth;
        framesHeight = savedConfig.scaling.outputHeight;
    }

    // D3D11 NV12 HW frames require even-aligned dimensions
    framesWidth = framesWidth & ~1;
    framesHeight = framesHeight & ~1;

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

int VideoEncoder::AddAudioStream(const AudioConfig& config, AVCodecContext* audioCtx) {
    if (!fmtCtx)
        return -1;

    const AVCodec* codec = nullptr;
    if (audioCtx) {
        codec = audioCtx->codec;
    } else {
        codec = avcodec_find_encoder_by_name(config.codec.empty() ? "aac" : config.codec.c_str());
    }

    if (!codec)
        return -1;
    AVStream* st = avformat_new_stream(fmtCtx, codec);
    if (!st)
        return -1;

    if (audioCtx) {
        // Correct way: copy parameters including extradata
        avcodec_parameters_from_context(st->codecpar, audioCtx);
        int sampleRate = audioCtx->sample_rate > 0 ? audioCtx->sample_rate : st->codecpar->sample_rate;
        if (sampleRate <= 0) {
            sampleRate = 48000;
        }
        st->time_base = {1, sampleRate};
    } else {
        // Fallback (might fail for extradata-dependent codecs)
        int sampleRate =
            (!config.sampleRate.empty() && config.sampleRate != "default") ? std::stoi(config.sampleRate) : 48000;
        st->time_base = {1, sampleRate};
        st->codecpar->codec_id = codec->id;
        st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        st->codecpar->sample_rate = sampleRate;
        st->codecpar->ch_layout.nb_channels = 2;
    }
    return st->index;
}

void VideoEncoder::SetAudioContext(const AudioConfig& config, AVCodecContext* audioCtx) {
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

int VideoEncoder::AddAudioContext(const AudioConfig& config, AVCodecContext* audioCtx, int track) {
    AudioStreamContext ctx;
    ctx.config = config;
    ctx.codecCtx = audioCtx;
    ctx.track = track;
    ctx.streamIndex = -1;
    audioContexts.push_back(ctx);

    DLL_Log("[VideoEncoder] AddAudioContext: track=%d, total=%d", ctx.track, (int)audioContexts.size());

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
    for (const auto& ctx : audioContexts) {
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
        DLL_Log("[VideoEncoder] Start: Waiting for previous recording to finalize...");
        writerThread.join();
    }

    // If fmtCtx was freed by Stop(), recreate it for the new recording
    // If fmtCtx was freed by Stop(), recreate it for the new recording
    if (!fmtCtx) {
        // Generate new output filename using robust helper
        outputFilename = GenerateOutputFilename(savedConfig);
        DLL_Log("[VideoEncoder] Creating new format context for: %s", outputFilename.c_str());

        if (avformat_alloc_output_context2(&fmtCtx, nullptr, nullptr, outputFilename.c_str()) < 0) {
            DLL_Log("[VideoEncoder] Failed to allocate new format context");
            return false;
        }
    }

    // If codecCtx was freed by Stop(), recreate it
    if (!codecCtx) {
        const AVCodec* codec = avcodec_find_encoder_by_name(savedConfig.encoder.c_str());
        if (!codec) {
            DLL_Log("[VideoEncoder] Codec not found: %s", savedConfig.encoder.c_str());
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

    // Reset WriteFrame debug counters for multi-recording support
    audioPacketCount = 0;
    videoPacketCount = 0;
    vidDebugCount = 0;

    // Pre-warm device and codec to reduce first-frame latency
    // This moves heavy initialization (D3D11 device, codec open, video processor)
    // from first frame to Start() call, avoiding game stutter on recording start
    // IMPORTANT: Only pre-warm if we already have valid dimensions from common
    // discovery
    if ((luidLow != 0 || luidHigh != 0) && width > 0 && height > 0 && !initDone) {
        DLL_Log("[VideoEncoder] Pre-warming device and codec (%dx%d)...", width, height);
        auto prewarmStart = PerfTimer::now();

        if (!EnsureDevice()) {
            DLL_Log("[VideoEncoder] Pre-warm failed, will retry on first frame");
        } else {
            auto prewarmEnd = PerfTimer::now();
            double prewarmMs = PerfTimer::elapsed_ms(prewarmStart, prewarmEnd);
            DLL_Log(
                "[VideoEncoder] Pre-warm complete in %.2fms (device init, codec "
                "open)",
                prewarmMs);
        }
    }

    recordingRequested = true;
    DLL_Log("[VideoEncoder] Start Recording Requested (Deferred).");

    // Reset per-recording perf stats so second+ recordings show correct data.
    g_lastFramePts = -1;
    g_framesEncoded = 0;
    g_totalFenceWait = 0.0;
    g_totalColorConvert = 0.0;
    g_totalEncode = 0.0;
    g_maxFrameTime = 0.0;
    g_slowFrameCount = 0;

    // Start Async Allocator Thread
    if (!writerRunning) {
        writerRunning = true;
        writerThread = std::thread(&VideoEncoder::AsyncWriteLoop, this);
        DLL_Log("[VideoEncoder] Started Writer Thread");
    }

    return true;
}

void VideoEncoder::WriteFrame(AVPacket* pkt) {
    if (!fileOpened || !fmtCtx)
        return;

    // Rescale timestamps from codec time_base to stream time_base
    AVStream* st = fmtCtx->streams[pkt->stream_index];
    AVRational codec_tb;

    if (pkt->stream_index == stream->index) {
        // Video packet - use video codec time_base
        codec_tb = codecCtx->time_base;
    } else {
        // Audio packet - audio time_base is typically 1/sample_rate
        // The audio encoder uses PTS = sample_count, so time_base is {1,
        // sample_rate}
        codec_tb = {1, st->codecpar->sample_rate};

        if (audioPacketCount++ % 100 == 0) {
            DLL_Log(
                "[VideoEncoder] Queuing audio pkt #%d size=%d pts=%lld "
                "dur=%lld stream_idx=%d",
                audioPacketCount, pkt->size, (long long)pkt->pts, (long long)pkt->duration, pkt->stream_index);
        }
    }

    // Debug: log first few video packets to verify PTS
    if (pkt->stream_index == stream->index && videoPacketCount++ < 5) {
        DLL_Log(
            "[VideoEncoder] Queuing video pkt #%d: pts=%lld codec_tb=%d/%d "
            "st_tb=%d/%d",
            videoPacketCount, pkt->pts, codec_tb.num, codec_tb.den, st->time_base.num, st->time_base.den);
    }

    // Rescale timestamps properly using FFmpeg's exact rational math
    av_packet_rescale_ts(pkt, codec_tb, st->time_base);
    if (pkt->stream_index == stream->index && pkt->dts == AV_NOPTS_VALUE) {
        pkt->dts = pkt->pts;
    }

    // DEBUG: Log PTS after rescaling and detect corruption
    if (pkt->stream_index == stream->index) {
        if (vidDebugCount++ < 20 || pkt->pts < 0) {
            DLL_Log("[VideoEncoder] PTS PRECISE: frameNum=%lld pts_ms=%lld st_tb=%d/%d", pkt->pts, pkt->pts,
                    st->time_base.num, st->time_base.den);
        }

        // DEBUG LEAK: Log queue stats every 100 video frames
        if (vidDebugCount % 100 == 0) {
            size_t qBytes = currentQueueBytes.load();
            size_t qSize = 0;
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                qSize = packetQueue.size();
            }
            DLL_Log("[VideoEncoder] QUEUE STATS: Count=%zu Bytes=%zu (Max=%zu)", qSize, qBytes, MAX_QUEUE_BYTES);

            // Memory safety check
            if (qBytes > MAX_QUEUE_BYTES) {
                DLL_Log("[VideoEncoder] CRITICAL: Queue exceeds limit! Dropping disabled?");
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
        int64_t packetPts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
        if (packetPts != AV_NOPTS_VALUE) {
            int64_t packetDuration = pkt->duration;
            if (packetDuration <= 0) {
                packetDuration = av_rescale_q(1, codec_tb, st->time_base);
                if (packetDuration <= 0) {
                    packetDuration = 1;
                }
            }
            int64_t packetEnd = packetPts + packetDuration;
            int64_t packetEndUs = av_rescale_q(packetEnd, st->time_base, AVRational{1, 1000000});
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
            DLL_Log(
                "[VideoEncoder] WARNING: Packet queue overloaded (%zu bytes) - "
                "applying backpressure",
                qBytes);
        }

        // Wait briefly for writer to drain.
        std::unique_lock<std::mutex> lock(queueMutex);
        queueCV.wait_for(lock, std::chrono::milliseconds(2), [this] {
            return currentQueueBytes.load(std::memory_order_relaxed) <= MAX_QUEUE_BYTES || isStopping || !writerRunning;
        });
        if (isStopping || !writerRunning) {
            break;
        }
    }

    AVPacket* clonePkt = av_packet_clone(pkt);
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

    pSharedMem->runtimeState.encoderOverloadFlags.store(flags, std::memory_order_relaxed);

    size_t qBytes = currentQueueBytes.load(std::memory_order_relaxed);
    uint32_t qBytes32 = (qBytes > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)qBytes;
    pSharedMem->runtimeState.muxQueueBytes.store(qBytes32, std::memory_order_relaxed);
}

bool VideoEncoder::EncodeFrame(HANDLE sharedHandle, HANDLE fenceHandle, uint64_t fenceValue, int64_t timestamp,
                               uint32_t sourcePid, int width, int height, int format, bool isHDR, bool isShmem,
                               int shmemSlot) {
    if (!recordingRequested)
        return false;

    // Debug: Log every 60th frame entry to verify loop
    if (encodeFrameCounter % 60 == 0) {
        DLL_Log("[VideoEncoder] EncodeFrame Entry: PID=%u Handle=%p FenceVal=%llu", sourcePid, sharedHandle,
                fenceValue);
    }

    if (!initDone || isHDR != currentIsHDR) {
        if (initDone) {
            DLL_Log(
                "[VideoEncoder] HDR Mode changed (New=%d, Old=%d). "
                "Re-initializing...",
                isHDR, currentIsHDR);
            Stop();  // Clean up existing encoder
            initDone = false;
            // Also need to clear codecOpenFailed?
            codecOpenFailed = false;
        }

        currentIsHDR = isHDR;
        // Re-Init with saved config (Init uses currentIsHDR to pick format)
        if (!Init(savedConfig, width, height, savedConfig.fps ? savedConfig.fps : 60, onPacket)) {
            DLL_Log("[VideoEncoder] Failed to Re-Init for HDR change");
            return false;
        }
    }

    // Use captured frame dimensions if not yet set or changed
    if (this->width != width || this->height != height) {
        if (this->width == 0) {
            DLL_Log("[VideoEncoder] Initial resolution discovered: %dx%d (Input: %dx%d)", width, height, width, height);
        } else {
            DLL_Log("[VideoEncoder] Resolution CHANGE detected: %dx%d -> %dx%d", this->width, this->height, width,
                    height);
            if (!fileOpened && initDone) {
                // Pre-warm used stale/wrong dimensions. Reset codec and container
                // so EnsureDevice() reinitializes them at the correct resolution
                // before the file header is written.
                DLL_Log("[VideoEncoder] Reinitializing encoder at correct resolution (pre-file-open)");
                avcodec_free_context(&codecCtx);
                if (d3d11FramesCtx) {
                    av_buffer_unref(&d3d11FramesCtx);
                    d3d11FramesCtx = nullptr;
                }
                stream = nullptr;
                if (fmtCtx) {
                    avformat_free_context(fmtCtx);
                    fmtCtx = nullptr;
                    avformat_alloc_output_context2(&fmtCtx, nullptr, nullptr, outputFilename.c_str());
                }
                const AVCodec* c = avcodec_find_encoder_by_name(savedConfig.encoder.c_str());
                if (c)
                    codecCtx = avcodec_alloc_context3(c);
                audioStreamIndex = -1;
                initDone = false;
            }
        }
        this->width = width;
        this->height = height;
    }

    if (!EnsureDevice())
        return false;

    // Fall through to D3D11 path below

    if (!fileOpened) {
        DLL_Log("[VideoEncoder] Opening Output File: %s", outputFilename.c_str());
        if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
            // Use 256KB buffer for better performance on slow storage (HDD/network)
            // Default is 32KB which causes many small writes
            int ret = avio_open2(&fmtCtx->pb, outputFilename.c_str(), AVIO_FLAG_WRITE, nullptr, nullptr);
            if (ret < 0) {
                DLL_Log("Failed to open output file: %d", ret);
                return false;
            }

            // Allocate custom buffer (256KB) for improved write performance
            const int bufferSize = 256 * 1024;
            [[maybe_unused]] unsigned char* buffer = nullptr;
        }

        // Debug: Log stream info before write_header
        DLL_Log("[VideoEncoder] fmtCtx has %d streams before write_header", fmtCtx->nb_streams);
        for (unsigned int i = 0; i < fmtCtx->nb_streams; i++) {
            AVStream* s = fmtCtx->streams[i];
            AVCodecParameters* cp = s->codecpar;
            DLL_Log(
                "[VideoEncoder] Stream %d: type=%d codec_id=%d w=%d h=%d "
                "extradata=%p extradata_size=%d",
                i, cp->codec_type, cp->codec_id, cp->width, cp->height, cp->extradata, cp->extradata_size);
        }

        // Pre-allocate space for MKV cues (seek index) at the front of the file.
        // Without this, cues are written at the END and many players can't seek
        // or show correct duration without reading the whole file first.
        if (fmtCtx->priv_data) {
            av_opt_set(fmtCtx->priv_data, "reserve_index_space", "200000", 0);  // 200KB
        }

        int ret = avformat_write_header(fmtCtx, nullptr);
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            DLL_Log("Failed to write header: %d (%s)", ret, errbuf);
            return false;
        }

        // Log actual stream time_base after muxer init (MKV may override)
        DLL_Log("[VideoEncoder] Stream time_base after write_header: %d/%d (codec: %d/%d)", stream->time_base.num,
                stream->time_base.den, codecCtx->time_base.num, codecCtx->time_base.den);

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
        needsCounterReset = true;  // Mark that we need to reset on first frame
    }

    if (inputFrameCount - lastLogFrameCount >= kFpsLogIntervalFrames) {
        if (startPts >= 0 && timestamp > startPts) {
            // Inject-mode timestamps are in microseconds.
            double elapsedSec = (double)(timestamp - startPts) / 1000000.0;
            double outputFps = (elapsedSec > 0.001) ? ((double)inputFrameCount / elapsedSec) : 0.0;
            DLL_Log("[FPS] Output: %.1f frames, %.1f fps over %.1fs", (double)inputFrameCount, outputFps, elapsedSec);
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
    stats.ptsMs = timestamp / 1000;

    // Calculate frame timing for smoothness analysis
    double expectedFrameMs = 1000.0 / codecCtx->framerate.num;
    if (g_lastFramePts >= 0) {
        stats.actualPtsDiff = (timestamp - g_lastFramePts) / 1000;
        stats.expectedPtsDiff = (int64_t)expectedFrameMs;
    }
    g_lastFramePts = timestamp;

    auto frameStart = PerfTimer::now();

    ID3D11Texture2D* bgraTex = nullptr;
    ID3D11Fence* d3d11Fence = nullptr;
    int cacheSlot = -1;

    if (isShmem) {
        if (pShmem && pSharedMem && pSharedMem->GetShmemMappingCreated()) {
            // Shmem Path: Upload pixels to our owned texture
            int texIdx = 0;  // Reuse first shared capture texture (we own it)
            bgraTex = sharedCaptureTextures[texIdx];

            if (bgraTex) {
                // Validation of slot
                int slot = (shmemSlot >= 0 && shmemSlot < 2) ? shmemSlot : 0;
                uint8_t* pSrc = pShmem->GetData(slot);

                if (pSrc) {
                    D3D11_BOX box;
                    box.left = 0;
                    box.right = pSharedMem->GetWidth();  // Use current frame resolution
                    box.top = 0;
                    box.bottom = pSharedMem->GetHeight();
                    box.front = 0;
                    box.back = 1;

                    // We need a pitch. Use pSharedMem->width * 4 if not stored in
                    // ShmemBuffer Actually ShmemBuffer has pitch.
                    d3d11Context->UpdateSubresource(bgraTex, 0, &box, pSrc, pShmem->pitch, 0);
                }
                bgraTex->AddRef();     // For consistency with Release() below
                d3d11Fence = nullptr;  // No fence for shmem
            }
        }
    } else {
        // Check if layer told us to use our own encoder textures directly
        // (DXVK zero-copy path: layer imported our KMT handles into Vulkan)
        if (pSharedMem && pSharedMem->useEncoderTextures.load(std::memory_order_acquire) &&
            sharedCaptureTexturesCreated) {
            // Find which encoder texture matches by KMT handle
            int matchIdx = -1;
            for (int i = 0; i < 4; i++) {
                if (sharedCaptureKmtHandles[i] == sharedHandle) {
                    matchIdx = i;
                    break;
                }
            }
            if (matchIdx >= 0) {
                bgraTex = sharedCaptureTextures[matchIdx];
            }
            if (bgraTex) {
                bgraTex->AddRef();
                // Still need the fence
                if (fenceValue > 0 && fenceHandle != nullptr && fenceHandle != INVALID_HANDLE_VALUE) {
                    if (cachedD3D11Fence && cachedFenceHandle == fenceHandle) {
                        d3d11Fence = cachedD3D11Fence;
                        d3d11Fence->AddRef();
                    } else {
                        ce::HandleGuard hProcess(OpenProcess(PROCESS_DUP_HANDLE, FALSE, sourcePid));
                        HRESULT hr = E_FAIL;
                        if (hProcess) {
                            // CRITICAL: Always DuplicateHandle first to validate the handle.
                            // OpenSharedFence throws SEH exceptions for invalid handles,
                            // and MinGW catch(...) cannot catch SEH exceptions.
                            // DuplicateHandle fails gracefully, preventing the crash.
                            ce::HandleGuard dupFence;
                            if (DuplicateHandle(hProcess.get(), fenceHandle, GetCurrentProcess(),
                                                dupFence.addressof(), 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                                // Handle is valid - safe to call OpenSharedFence
                                hr = CallOpenSharedFence(d3d11Device, dupFence.get(), &d3d11Fence);
                            } else {
                                if (encodeFrameCounter < 10)
                                    DLL_Log("[VideoEncoder] Fence DuplicateHandle failed for PID %u handle %p",
                                            sourcePid, fenceHandle);
                            }
                        }
                        if (FAILED(hr)) {
                            // Fallback: try direct handle (may work for same-process or KMT handles)
                            if (!g_HandleFailureCache.ShouldSkipFence(fenceHandle)) {
                                hr = CallOpenSharedFence(d3d11Device, fenceHandle, &d3d11Fence);
                            }
                        }
                        if (d3d11Fence) {
                            if (cachedD3D11Fence)
                                cachedD3D11Fence->Release();
                            cachedD3D11Fence = d3d11Fence;
                            cachedD3D11Fence->AddRef();
                            cachedFenceHandle = fenceHandle;
                            cachedSourcePid = sourcePid;
                        }
                    }
                }
                if (encodeFrameCounter < 10) {
                    DLL_Log("[VideoEncoder] Frame %d: Using encoder-owned texture[%d] directly (DXVK zero-copy)",
                            encodeFrameCounter, matchIdx);
                }
            }
        }

        if (!bgraTex) {
            // Standard shared handle path
            HANDLE sharedHandleAlt = NormalizeSourceHandleForWow64(sharedHandle, sourcePid);
            HANDLE fenceHandleAlt = NormalizeSourceHandleForWow64(fenceHandle, sourcePid);
            const bool hasSharedAlt = (sharedHandleAlt != sharedHandle);
            const bool hasFenceAlt = (fenceHandleAlt != fenceHandle);

            // Check cache for valid fence and texture (Quad-Buffered Cache)
            // Texture caching works independently of fence (for D3D11 KMT path)
            cacheSlot = -1;
            bool skipFence = (fenceValue == 0 || fenceHandle == 0 || fenceHandle == INVALID_HANDLE_VALUE);
            bool fenceValid = !skipFence && (sourcePid > 0 && sourcePid == cachedSourcePid &&
                                             fenceHandle == cachedFenceHandle && cachedD3D11Fence);

            // For texture matching, we only need matching PID and handle
            // (fence-independent)
            bool pidMatches = (sourcePid > 0 && sourcePid == cachedSourcePid);

            // Search for cached texture by handle (works with or without fence)
            if (pidMatches) {
                for (int i = 0; i < 8; i++) {
                    if (cachedTextureHandles[i] == sharedHandle && cachedSharedTextures[i]) {
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
                cachedSourcePid = sourcePid;  // Remember new PID
            }

            // ID3D11Texture2D *bgraTex = nullptr; // Moved up
            // ID3D11Fence *d3d11Fence = nullptr;   // Moved up

            if (cacheSlot >= 0) {
                // Full Cache Hit
                bgraTex = cachedSharedTextures[cacheSlot];
                d3d11Fence = cachedD3D11Fence;  // May be null for D3D11 KMT path (no fence)
                bgraTex->AddRef();
                if (d3d11Fence) {
                    d3d11Fence->AddRef();
                }

                if (encodeFrameCounter % kCacheLogIntervalFrames == 1) {
                    DLL_Log("[VideoEncoder] Using cached handles (pid=%u, slot=%d, frame=%d)", sourcePid, cacheSlot,
                            encodeFrameCounter);
                }
            } else {
                // Cache Miss (Partial or Full)
                // Use RAII to ensure handle is closed if we return early
                ce::HandleGuard hProcess(OpenProcess(PROCESS_DUP_HANDLE, FALSE, sourcePid));

                if (!hProcess) {
                    DLL_Log("[VideoEncoder] Frame %d: Failed to Open Process %u", encodeFrameCounter, sourcePid);
                    return false;
                }

                // 1. Handle Fence (Reuse if valid, Open if not)
                if (skipFence) {
                    d3d11Fence = nullptr;
                    if (encodeFrameCounter % 60 == 0)
                        DLL_Log("[VideoEncoder] Frame %d: SkipFence is true (Val=%llu Hnd=%p)", encodeFrameCounter,
                                fenceValue, fenceHandle);
                } else if (fenceValid) {
                    d3d11Fence = cachedD3D11Fence;
                    d3d11Fence->AddRef();
                } else {
                    ce::HandleGuard dupFence;
                    HRESULT hr = E_FAIL;

                    // CRITICAL: Always DuplicateHandle first to validate handles.
                    if (DuplicateHandle(hProcess.get(), fenceHandle, GetCurrentProcess(), dupFence.addressof(),
                                        0, FALSE, DUPLICATE_SAME_ACCESS)) {
                        // Handle duplicated successfully - safe to call OpenSharedFence
                        hr = CallOpenSharedFence(d3d11Device, dupFence.get(), &d3d11Fence);
                        if (FAILED(hr) && encodeFrameCounter < 10) {
                            DLL_Log("[VideoEncoder] OpenSharedFence(dup) failed: HR=%x (Hnd=%p)", hr,
                                    dupFence.get());
                        }
                    } else {
                        DWORD err = GetLastError();
                        if (encodeFrameCounter < 10) {
                            DLL_Log("[VideoEncoder] DuplicateHandle failed: Err=%d (SrcPid=%u Hnd=%p)",
                                    err, sourcePid, fenceHandle);
                        }
                        // Last resort: try direct handle (may work for same-process or KMT handles)
                        if (!g_HandleFailureCache.ShouldSkipFence(fenceHandle)) {
                            hr = CallOpenSharedFence(d3d11Device, fenceHandle, &d3d11Fence);
                        }
                    }

                    // Alternate handle representation for WOW64 sources
                    if (FAILED(hr) && hasFenceAlt) {
                        ce::HandleGuard dupFenceAlt;
                        // Try direct first
                        hr = CallOpenSharedFence(d3d11Device, fenceHandleAlt, &d3d11Fence);
                        if (FAILED(hr)) {
                            if (DuplicateHandle(hProcess.get(), fenceHandleAlt, GetCurrentProcess(),
                                                dupFenceAlt.addressof(), 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                                hr = CallOpenSharedFence(d3d11Device, dupFenceAlt.get(), &d3d11Fence);
                            }
                        }
                    }

                    // Final fallback - try as generic shared resource
                    if (FAILED(hr)) {
                        hr = CallOpenSharedResource(d3d11Device, fenceHandle, IID_PPV_ARGS(&d3d11Fence));
                    }
                    if (FAILED(hr) && hasFenceAlt) {
                        hr = CallOpenSharedResource(d3d11Device, fenceHandleAlt, IID_PPV_ARGS(&d3d11Fence));
                    }
                    if (FAILED(hr) && encodeFrameCounter < 10) {
                        DLL_Log(
                            "[VideoEncoder] OpenSharedFence(direct) failed: HR=%x (Hnd=%p), trying "
                            "DuplicateHandle...",
                            hr, fenceHandle);
                    }

                    // Fallback to DuplicateHandle path (for handles that support it)
                    if (FAILED(hr)) {
                        if (DuplicateHandle(hProcess.get(), fenceHandle, GetCurrentProcess(), dupFence.addressof(),
                                            0, FALSE, DUPLICATE_SAME_ACCESS)) {
                            hr = CallOpenSharedFence(d3d11Device, dupFence.get(), &d3d11Fence);
                            if (FAILED(hr)) {
                                DLL_Log("[VideoEncoder] OpenSharedFence(dup) failed: HR=%x (Hnd=%p)", hr,
                                        dupFence.get());
                            }
                        } else {
                            DWORD err = GetLastError();
                            DLL_Log(
                                "[VideoEncoder] DuplicateHandle failed: Err=%d (SrcPid=%u "
                                "Hnd=%p)",
                                err, sourcePid, fenceHandle);
                        }
                    }

                    // Alternate handle representation for WOW64 sources
                    if (FAILED(hr) && hasFenceAlt) {
                        ce::HandleGuard dupFenceAlt;
                        // Use DuplicateHandle first for safety
                        if (DuplicateHandle(hProcess.get(), fenceHandleAlt, GetCurrentProcess(),
                                            dupFenceAlt.addressof(), 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                            hr = CallOpenSharedFence(d3d11Device, dupFenceAlt.get(), &d3d11Fence);
                        }
                    }

                    // Final fallback - try as generic shared resource
                    if (FAILED(hr)) {
                        hr = CallOpenSharedResource(d3d11Device, fenceHandle, IID_PPV_ARGS(&d3d11Fence));
                    }
                    if (FAILED(hr) && hasFenceAlt) {
                        hr = CallOpenSharedResource(d3d11Device, fenceHandleAlt, IID_PPV_ARGS(&d3d11Fence));
                    }

                    if (d3d11Fence && encodeFrameCounter < 10) {
                        DLL_Log("[VideoEncoder] Successfully opened shared fence for PID %u", sourcePid);
                    }
                    // Cache Fence if successfully opened
                    if (d3d11Fence) {
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
                HRESULT hrNtDirect = E_FAIL;
                HRESULT hrNtDup = E_FAIL;
                HRESULT hrKmtDup = E_FAIL;
                HRESULT hrNtAltDirect = E_FAIL;
                HRESULT hrNtAltDup = E_FAIL;
                HRESULT hrKmtAltDup = E_FAIL;
                HRESULT hrKmtDirect = E_FAIL;
                HRESULT hrKmtAltDirect = E_FAIL;

                if (encodeFrameCounter < 10) {
                    DLL_Log(
                        "[VideoEncoder] Frame %d: Opening shared texture: handle=%p, "
                        "sourcePid=%u (cached=%u, match=%s), format=%d",
                        encodeFrameCounter, sharedHandle, sourcePid, cachedSourcePid,
                        (sourcePid == cachedSourcePid) ? "yes" : "no", format);
                }

                if (sharedHandle == NULL) {
                    DLL_Log("[VideoEncoder] Frame %d: Error: sharedHandle is NULL", encodeFrameCounter);
                } else {
                    // D3D11 OpenSharedResource can throw SEH exceptions for invalid handles or
                    // incompatible formats. DuplicateHandle first to validate handle accessibility.
                    // Even duplicated handles can fail if D3D12/D3D11 devices are incompatible.
                    ce::HandleGuard dupTexDirect;
                    bool handleValid = DuplicateHandle(hProcess.get(), sharedHandle, GetCurrentProcess(),
                                                       dupTexDirect.addressof(), 0, FALSE, DUPLICATE_SAME_ACCESS);
                    if (handleValid) {
                        // Handle duplicated - try OpenSharedResource1 with the valid handle
                        hr = CallOpenSharedResource1(d3d11Device, dupTexDirect.get(), IID_PPV_ARGS(&bgraTex));
                        hrNtDirect = hr;
                    } else {
                        hrNtDirect = HRESULT_FROM_WIN32(GetLastError());
                        if (encodeFrameCounter < 10)
                            DLL_Log("[VideoEncoder] Frame %d: DuplicateHandle for texture failed: %p",
                                    encodeFrameCounter, sharedHandle);
                    }

                    if (FAILED(hr) && encodeFrameCounter < 10) {
                        DLL_Log(
                            "[VideoEncoder] Frame %d: OpenSharedResource1(direct=%p) "
                            "failed HR=%x. Trying KMT path...",
                            encodeFrameCounter, sharedHandle, hr);
                    }

                    // Fallback to KMT path with duplicated handle
                    if (FAILED(hr) && handleValid) {
                        hr = CallOpenSharedResource(d3d11Device, dupTexDirect.get(), IID_PPV_ARGS(&bgraTex));
                        hrKmtDup = hr;
                        if (SUCCEEDED(hr) && encodeFrameCounter < 10) {
                            DLL_Log("[VideoEncoder] Frame %d: Opened duplicated handle %p via KMT path",
                                    encodeFrameCounter, dupTexDirect.get());
                        }
                    }

                    // Try original handle as last resort (may work for same-process)
                    if (FAILED(hr) && !g_HandleFailureCache.ShouldSkipTexture(sharedHandle)) {
                        hr = CallOpenSharedResource(d3d11Device, sharedHandle, IID_PPV_ARGS(&bgraTex));
                        hrKmtDirect = hr;
                        if (SUCCEEDED(hr) && encodeFrameCounter < 10) {
                            DLL_Log("[VideoEncoder] Frame %d: Opened handle %p via KMT direct path",
                                    encodeFrameCounter, sharedHandle);
                        }
                    }

                    // Retry logic for transient failures (handle not yet published by Vulkan layer)
                    int retryCount = 0;
                    const int maxRetries = 3;
                    HRESULT retryHr = hr;
                    while (FAILED(retryHr) && retryCount < maxRetries) {
                        retryCount++;
                        DLL_Log("[VideoEncoder] Frame %d: OpenSharedResource failed (HR=%x), retry %d/%d...",
                                encodeFrameCounter, retryHr, retryCount, maxRetries);
                        Sleep(2);

                        // Retry with duplicated handle first (safe - DuplicateHandle fails gracefully)
                        ce::HandleGuard dupTexRetry;
                        if (DuplicateHandle(hProcess.get(), sharedHandle, GetCurrentProcess(),
                                            dupTexRetry.addressof(), 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                            retryHr = CallOpenSharedResource1(d3d11Device, dupTexRetry.get(), IID_PPV_ARGS(&bgraTex));
                            if (FAILED(retryHr)) {
                                retryHr = CallOpenSharedResource(d3d11Device, dupTexRetry.get(), IID_PPV_ARGS(&bgraTex));
                            }
                        }
                        if (FAILED(retryHr) && hasSharedAlt) {
                            ce::HandleGuard dupTexAltRetry;
                            if (DuplicateHandle(hProcess.get(), sharedHandleAlt, GetCurrentProcess(),
                                                dupTexAltRetry.addressof(), 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                                retryHr = CallOpenSharedResource1(d3d11Device, dupTexAltRetry.get(),
                                                                  IID_PPV_ARGS(&bgraTex));
                                if (FAILED(retryHr)) {
                                    retryHr = CallOpenSharedResource(d3d11Device, dupTexAltRetry.get(),
                                                                      IID_PPV_ARGS(&bgraTex));
                                }
                            }
                        }
                    }
                    if (SUCCEEDED(retryHr)) hr = retryHr;
                }  // end of else (sharedHandle != NULL)

                if (FAILED(hr)) {
                    static std::atomic<int> s_openDetailLogCount{0};
                    if (s_openDetailLogCount.fetch_add(1, std::memory_order_relaxed) < 16) {
                        DLL_Log(
                            "[VideoEncoder] Frame %d: Open detail h=%p alt=%p ntDir=%x ntDup=%x ntAltDir=%x "
                            "ntAltDup=%x "
                            "kmtDup=%x kmtAltDup=%x kmtDir=%x kmtAltDir=%x",
                            encodeFrameCounter, sharedHandle, sharedHandleAlt, hrNtDirect, hrNtDup, hrNtAltDirect,
                            hrNtAltDup, hrKmtDup, hrKmtAltDup, hrKmtDirect, hrKmtAltDirect);
                    }
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
                        targetSlot = 0;  // Fallback to 0 if all full
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
        }  // End of if (!bgraTex) - standard shared handle path
    }  // End of isShmem else block

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
            DWORD waitRes = WaitForSingleObject(fenceEvent, 200);  // 200ms timeout (increased for GPU contention)
            if (waitRes == WAIT_TIMEOUT) {
                // Frame is too late or GPU is hung - skip this frame
                DLL_Log("[VideoEncoder] Frame %d: GPU Fence Timeout (50ms) - Skipping", encodeFrameCounter);
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
        DLL_Log("[VideoEncoder] Frame %d: Failed to Wait on Fence. HR=%x", encodeFrameCounter, hr);
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
                DLL_Log("[Cursor] Frame %d: flags=%d hCursor=%p pos=(%d,%d)", encodeFrameCounter, ci.flags,
                        (void*)ci.hCursor, ci.ptScreenPos.x, ci.ptScreenPos.y);
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
    ID3D11Texture2D* nv12Tex = nullptr;
    if (!ConvertBGRAtoNV12(bgraTex, &nv12Tex, cursorVisible, cursorX, cursorY)) {
        DLL_Log("[VideoEncoder] Frame %d: GPU color conversion failed", encodeFrameCounter);
        bgraTex->Release();
        return false;
    }

    auto afterConvert = PerfTimer::now();
    stats.colorConvertMs = PerfTimer::elapsed_ms(beforeConvert, afterConvert);
    bgraTex->Release();  // No longer needed after conversion

    // 5. Wrap NV12 D3D11 Texture in AVFrame
    AVFrame* d3d11Frame = av_frame_alloc();
    d3d11Frame->format = AV_PIX_FMT_D3D11;
    d3d11Frame->width = scalingEnabled ? outputWidth : width;
    d3d11Frame->height = scalingEnabled ? outputHeight : height;
    d3d11Frame->hw_frames_ctx = av_buffer_ref(d3d11FramesCtx);

    d3d11Frame->buf[0] = av_buffer_create((uint8_t*)nv12Tex, 0, FreeD3D11Tex, NULL, 0);
    if (!d3d11Frame->buf[0]) {
        FreeD3D11Tex(NULL, (uint8_t*)nv12Tex);
        av_frame_free(&d3d11Frame);
        return false;
    }

    d3d11Frame->data[0] = (uint8_t*)nv12Tex;
    d3d11Frame->data[1] = 0;  // index

    // Calculate relative PTS (start from 0) — timestamp is in microseconds
    if (startPts < 0) {
        startPts = timestamp;
        DLL_Log("[VideoEncoder] Recording started at PTS %lld us", startPts.load());
    }

    int64_t targetPts = 0;
    if (savedConfig.useVFR && startPts >= 0) {
        // VFR: PTS in microseconds, matches time_base 1/1000000
        targetPts = timestamp - startPts;
    } else {
        // CFR: derive PTS from real elapsed time to avoid playback speed drift
        int fps = (codecCtx && codecCtx->framerate.num > 0) ? codecCtx->framerate.num : savedConfig.fps;
        if (fps <= 0) {
            fps = 60;
        }
        int64_t elapsedUs = timestamp - startPts;
        if (elapsedUs < 0) {
            elapsedUs = 0;
        }
        targetPts = av_rescale(elapsedUs, fps, 1000000);
    }

    // 5. Encode (Direct D3D11 Frame) - with proper packet draining
    AVPacket* pkt = av_packet_alloc();
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
                DLL_Log("[VideoEncoder] avcodec_receive_packet failed: %d (%s)", ret, errbuf);
                break;
            }

            packetCount++;
            pkt->stream_index = stream->index;  // Ensure video stream index

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

    auto sendFrame = [&](AVFrame* frame) -> bool {
        drainPackets();
        int ret = avcodec_send_frame(codecCtx, frame);
        int retries = 0;
        while (ret == AVERROR(EAGAIN) && retries < 10) {
            if (retries == 0) {
                lastEncoderOverloadTickMs.store(GetTickCount64(), std::memory_order_relaxed);
                PublishRuntimeState();
            }
            drainPackets();
            ret = avcodec_send_frame(codecCtx, frame);
            retries++;
        }
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
            DLL_Log("[VideoEncoder] avcodec_send_frame failed: %d (%s)", ret, errbuf);
            return false;
        }
        drainPackets();
        return true;
    };

    bool success = true;

    if (!savedConfig.useVFR) {
        if (lastAssignedVideoPts >= 0) {
            if (targetPts <= lastAssignedVideoPts) {
                // Frame arrived too early (game FPS > target FPS). Drop it.
                skippedFrameCount++;
                av_packet_free(&pkt);
                av_frame_free(&d3d11Frame);
                return true;
            }

            // If targetPts > lastAssignedVideoPts + 1, we need to duplicate frames
            while (targetPts > lastAssignedVideoPts + 1) {
                AVFrame* dupFrame = av_frame_clone(d3d11Frame);
                if (dupFrame) {
                    dupFrame->pts = lastAssignedVideoPts + 1;
                    lastAssignedVideoPts = dupFrame->pts;
                    duplicatedFrameCount++;
                    if (!sendFrame(dupFrame)) {
                        success = false;
                    }
                    av_frame_free(&dupFrame);
                } else {
                    break;
                }
            }
        }
    }

    d3d11Frame->pts = targetPts;
    lastAssignedVideoPts = d3d11Frame->pts;

    if (success) {
        success = sendFrame(d3d11Frame);
    }

    auto afterEncode = PerfTimer::now();
    stats.ptsMs = timestamp;
    stats.textureOpenMs = PerfTimer::elapsed_ms(frameStart, afterOpen);
    stats.colorConvertMs = PerfTimer::elapsed_ms(afterOpen, afterConvert);
    stats.encodeMs = PerfTimer::elapsed_ms(encodeStart, afterEncode);
    stats.totalMs = PerfTimer::elapsed_ms(frameStart, afterEncode);

    // Update last frame encode time (in microseconds)
    // This is robust against timer noise/underflow compared to (Total - Wait).
    lastEncodeTimeUs = (int64_t)(PerfTimer::elapsed_ms(afterFence, afterEncode) * 1000.0);
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
    if (stats.totalMs > expectedFrameMs * 2 || encodeFrameCounter <= 5 || encodeFrameCounter % 30 == 0) {
        std::string features = "";
        if (savedConfig.lookahead)
            features += "Lookahead ";
        if (savedConfig.aq)
            features += "AQ ";
        if (savedConfig.bFrames > 0)
            features += "B-Frames ";
        if (!savedConfig.multipass.empty() && savedConfig.multipass != "disabled")
            features += "Multipass ";

        const char* slowLabel = (stats.totalMs > expectedFrameMs * 2) ? "(SLOW!)" : "";

        DLL_Log(
            "[PERF] Frame %d: TOTAL=%.2fms %s fence=%.2f convert=%.2f "
            "encode=%.2f pts=%lldms packets=%d [Features: %s]",
            encodeFrameCounter, stats.totalMs, slowLabel, stats.fenceWaitMs, stats.colorConvertMs, stats.encodeMs,
            stats.ptsMs, stats.packetsProduced, features.c_str());
    }

    // Periodic performance summary (every 120 frames ~1 sec at
    // 120fps)
    if (encodeFrameCounter % 120 == 0) {
        double avgFence = g_totalFenceWait / g_framesEncoded;
        double avgConvert = g_totalColorConvert / g_framesEncoded;
        double avgEncode = g_totalEncode / g_framesEncoded;
        double avgTotal = avgFence + avgConvert + avgEncode;

        // Identify bottleneck
        const char* bottleneck = "ENCODE";
        double maxTime = avgEncode;
        if (avgFence > maxTime) {
            bottleneck = "FENCE_WAIT";
            maxTime = avgFence;
        }
        if (avgConvert > maxTime) {
            bottleneck = "COLOR_CONV";
            maxTime = avgConvert;
        }

        DLL_Log(
            "[PERF SUMMARY] Frames=%lld Avg: total=%.2fms fence=%.2f "
            "convert=%.2f "
            "encode=%.2f | Max=%.2fms SlowFrames=%d | Bottleneck=%s",
            g_framesEncoded, avgTotal, avgFence, avgConvert, avgEncode, g_maxFrameTime, g_slowFrameCount, bottleneck);

        // Frame timing analysis for smoothness
        if (stats.actualPtsDiff > 0) {
            double jitter = (double)(stats.actualPtsDiff - stats.expectedPtsDiff);
            DLL_Log(
                "[SMOOTHNESS] Expected frame interval: %lldms "
                "Actual: %lldms "
                "Jitter: %.2fms",
                stats.expectedPtsDiff, stats.actualPtsDiff, jitter);
        }
    }

    av_frame_free(&d3d11Frame);  // Releases D3D11 Tex

    return true;
}

// EncodeFrameD3D11: Direct D3D11 texture encoding for framegrab
// mode Zero-copy path - texture is converted BGRA->NV12 directly
bool VideoEncoder::EncodeFrameD3D11(ID3D11Texture2D* bgraTexture, int64_t pts, uint32_t frameWidth,
                                    uint32_t frameHeight) {
    if (!recordingRequested)
        return false;

    inputFrameCount++;

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
        ID3D11Device* texDevice = nullptr;
        bgraTexture->GetDevice(&texDevice);
        if (texDevice) {
            texDevice->QueryInterface(__uuidof(ID3D11Device5), (void**)&d3d11Device);
            ID3D11DeviceContext* ctx = nullptr;
            texDevice->GetImmediateContext(&ctx);
            if (ctx) {
                ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**)&d3d11Context);
                ctx->Release();
            }
            texDevice->Release();
        }

        if (!d3d11Device || !d3d11Context) {
            DLL_Log(
                "[VideoEncoder] Framegrab: Failed to get D3D11 "
                "device from "
                "texture");
            return false;
        }
    }

    // Detect input format BEFORE encoder init so we can configure
    // P010/NV12 frames context correctly
    if (bgraTexture) {
        D3D11_TEXTURE2D_DESC texDesc = {};
        bgraTexture->GetDesc(&texDesc);
        bool isHDR =
            (texDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM || texDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (isHDR && !currentIsHDR) {
            currentIsHDR = true;
            use10BitPipeline = true;
            DLL_Log("[VideoEncoder] Pre-init: 10-bit/HDR input detected (fmt=%d), will use P010 pipeline",
                    texDesc.Format);
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
        if (fmtCtx->priv_data) {
            av_opt_set(fmtCtx->priv_data, "reserve_index_space", "200000", 0);
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
        DLL_Log(
            "[VideoEncoder] ScreenGrab: Reset encodeFrameCounter for new "
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
            double outputFps = (elapsedSec > 0.001) ? ((double)encodeFrameCounter / elapsedSec) : 0.0;
            DLL_Log("[FPS] Framegrab: %.1f frames, %.1f fps over %.1fs", (double)encodeFrameCounter, outputFps,
                    elapsedSec);
        }
        lastLogFrameCount = encodeFrameCounter;
    }

    // Performance timing
    auto frameStart = PerfTimer::now();

    // Detect format for 10-bit/HDR auto-selection (WGC mode)
    {
        D3D11_TEXTURE2D_DESC frameDesc;
        bgraTexture->GetDesc(&frameDesc);
        bool isHDR =
            (frameDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM || frameDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (isHDR != currentIsHDR) {
            currentIsHDR = isHDR;
            DLL_Log("[VideoEncoder] WGC format change: fmt=%d, HDR=%d", frameDesc.Format, isHDR);
        }
    }

    // WGC capture includes the cursor natively (composited by DWM).
    // No software cursor overlay needed — pass false to disable it.
    bool cursorVisible = false;
    int cursorX = 0, cursorY = 0;

    // Convert BGRA → NV12 using Video Processor (no cursor overlay —
    // WGC already has the cursor baked into the captured frame)
    auto beforeConvert = PerfTimer::now();
    ID3D11Texture2D* nv12Tex = nullptr;

    // Scoped Lock for D3D11 Immediate Context (protects Blt/CopyResource)
    bool convertSuccess = ConvertBGRAtoNV12(bgraTexture, &nv12Tex, cursorVisible, cursorX, cursorY);

    if (!convertSuccess) {
        DLL_Log("[VideoEncoder] Frame %d: GPU color conversion failed", encodeFrameCounter);
        return false;
    }
    auto afterConvert = PerfTimer::now();
    double convertMs = PerfTimer::elapsed_ms(beforeConvert, afterConvert);

    // Wrap NV12 D3D11 Texture in AVFrame
    AVFrame* d3d11Frame = av_frame_alloc();
    d3d11Frame->format = AV_PIX_FMT_D3D11;
    d3d11Frame->width = scalingEnabled ? outputWidth : width;
    d3d11Frame->height = scalingEnabled ? outputHeight : height;
    d3d11Frame->hw_frames_ctx = av_buffer_ref(d3d11FramesCtx);

    d3d11Frame->buf[0] = av_buffer_create((uint8_t*)nv12Tex, 0, FreeD3D11Tex, NULL, 0);
    if (!d3d11Frame->buf[0]) {
        FreeD3D11Tex(NULL, (uint8_t*)nv12Tex);
        av_frame_free(&d3d11Frame);
        return false;
    }
    d3d11Frame->data[0] = (uint8_t*)nv12Tex;
    d3d11Frame->data[1] = 0;

    // Calculate PTS — pts is in microseconds
    if (startPts < 0) {
        startPts = pts;
        DLL_Log("[VideoEncoder] Framegrab recording started at PTS %lld us", startPts.load());
    }

    int64_t targetPts = 0;
    int64_t elapsedUs = pts - startPts;
    if (elapsedUs < 0)
        elapsedUs = 0;

    if (savedConfig.useVFR) {
        // VFR: PTS in microseconds, matches time_base 1/1000000
        targetPts = elapsedUs;
    } else {
        // CFR: derive frame index from elapsed time
        int fps = 60;  // Default
        if (codecCtx && codecCtx->framerate.num > 0) {
            fps = codecCtx->framerate.num;
        }
        targetPts = av_rescale(elapsedUs, fps, 1000000);
    }

    // Encode
    AVPacket* pkt = av_packet_alloc();
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
                DLL_Log(
                    "[Framegrab DEBUG] Received pkt: pts=%lld dts=%lld "
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

    auto sendFrame = [&](AVFrame* frame) -> bool {
        drainPackets();
        int ret = avcodec_send_frame(codecCtx, frame);
        int retries = 0;
        while (ret == AVERROR(EAGAIN) && retries < 10) {
            if (retries == 0) {
                lastEncoderOverloadTickMs.store(GetTickCount64(), std::memory_order_relaxed);
                PublishRuntimeState();
            }
            drainPackets();
            ret = avcodec_send_frame(codecCtx, frame);
            retries++;
        }
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
            DLL_Log("[VideoEncoder] ScreenGrab send_frame failed: %d (%s)", ret, errbuf);
            return false;
        }
        drainPackets();
        return true;
    };

    bool success = true;

    if (!savedConfig.useVFR) {
        if (lastAssignedVideoPts >= 0) {
            if (targetPts <= lastAssignedVideoPts) {
                // Frame arrived too early (game FPS > target FPS). Drop it.
                skippedFrameCount++;
                av_packet_free(&pkt);
                av_frame_free(&d3d11Frame);
                return true;
            }

            // If targetPts > lastAssignedVideoPts + 1, we need to duplicate frames
            while (targetPts > lastAssignedVideoPts + 1) {
                AVFrame* dupFrame = av_frame_clone(d3d11Frame);
                if (dupFrame) {
                    dupFrame->pts = lastAssignedVideoPts + 1;
                    lastAssignedVideoPts = dupFrame->pts;
                    duplicatedFrameCount++;
                    if (!sendFrame(dupFrame)) {
                        success = false;
                    }
                    av_frame_free(&dupFrame);
                } else {
                    break;
                }
            }
        }
    }

    d3d11Frame->pts = targetPts;
    lastAssignedVideoPts = d3d11Frame->pts;

    // Debug: Log input frame PTS
    if (encodeFrameCounter < 20 || encodeFrameCounter % 100 == 0) {
        DLL_Log("[Framegrab DEBUG] Sending frame %d with input PTS=%lld", encodeFrameCounter, d3d11Frame->pts);
    }

    if (success) {
        success = sendFrame(d3d11Frame);
        if (success) {
            outputFrameCount++;
        }
    }

    if (!success) {
        av_packet_free(&pkt);
        av_frame_free(&d3d11Frame);
        return false;
    }

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

        DLL_Log(
            "[Framegrab PERF] Frame %d: total=%.2fms (SLOW!) convert=%.2f "
            "encode=%.2f packets=%d [Features: %s]",
            encodeFrameCounter, totalMs, convertMs, encodeMs, packetCount, features.c_str());
    }

    // Log periodic stats
    if (encodeFrameCounter % 120 == 0) {
        DLL_Log(
            "[Framegrab PERF] Frame %d: total=%.2fms convert=%.2f "
            "encode=%.2f "
            "packets=%d",
            encodeFrameCounter, totalMs, convertMs, encodeMs, packetCount);
    }

    av_frame_free(&d3d11Frame);
    return true;
}

void VideoEncoder::CleanupResources() {
    // Check if we should preserve encoder-owned textures (DXVK zero-copy path).
    // The Vulkan layer imported our KMT handles — destroying them invalidates the pipeline.
    bool preserveEncoderTextures =
        pSharedMem && pSharedMem->useEncoderTextures.load(std::memory_order_acquire) && sharedCaptureTexturesCreated;

    // Free any queued packets (defensive)
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        while (!packetQueue.empty()) {
            AVPacket* pkt = packetQueue.front();
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

    if (preserveEncoderTextures) {
        // Keep D3D11 device/context alive — textures are bound to it.
        // Only free FFmpeg HW contexts; they'll be recreated in EnsureDevice().
        if (d3d11DeviceCtx)
            av_buffer_unref(&d3d11DeviceCtx);
        if (d3d11FramesCtx)
            av_buffer_unref(&d3d11FramesCtx);
        // Reset initDone so EnsureDevice() rebuilds FFmpeg contexts but reuses the device
        initDone = false;
    } else {
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
    }

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

    if (!preserveEncoderTextures) {
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
        sharedCaptureTextureFormat = 0;
    }

    if (bgraStagingTexture) {
        bgraStagingTexture->Release();
        bgraStagingTexture = nullptr;
    }

    CleanupVideoProcessor();
    CleanupCursorCache();

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
    asyncWriteErrorCount = 0;
    cursorUpdateCounter = 0;
    cachedCursorX = 0;
    cachedCursorY = 0;
    cachedCursorVisible = false;
}

void VideoEncoder::ReleasePreservedEncoderTextures() {
    if (!sharedCaptureTexturesCreated)
        return;

    DLL_Log("[VideoEncoder] Releasing preserved encoder textures (game exited)");

    // Clear shared memory flags so a new game won't try to import stale handles
    if (pSharedMem) {
        pSharedMem->encoderTextures.kmtReady.store(false, std::memory_order_release);
        pSharedMem->encoderTextures.ready.store(false, std::memory_order_release);
    }

    // Release encoder-owned KMT textures (mirrors !preserveEncoderTextures path in CleanupResources)
    for (int i = 0; i < 8; i++) {
        if (sharedCaptureTextures[i]) {
            sharedCaptureTextures[i]->Release();
            sharedCaptureTextures[i] = nullptr;
        }
        if (sharedCaptureHandles[i]) {
            CloseHandle(sharedCaptureHandles[i]);
            sharedCaptureHandles[i] = nullptr;
        }
        sharedCaptureKmtHandles[i] = nullptr;
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
    sharedCaptureTextureFormat = 0;

    // Release D3D11 device and all resources that depend on it
    CleanupVideoProcessor();

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

    initDone = false;
    DLL_Log("[VideoEncoder] Preserved encoder textures released");
}

void VideoEncoder::Stop() {
    bool wasRecording = recordingRequested;
    recordingRequested = false;

    if (wasRecording) {
        DLL_Log("[VideoEncoder] Recording stats: input=%lld output=%lld skipped=%lld duplicated=%lld", inputFrameCount,
                outputFrameCount, skippedFrameCount, duplicatedFrameCount);
    }

    if (wasRecording && writerRunning) {
        DLL_Log("[VideoEncoder] Stop: Signaling finalize (queueBytes=%zu)...", currentQueueBytes.load());
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
            int64_t finalDurationUs = encodedDurationUs.load(std::memory_order_relaxed);
            if (finalDurationUs > 0) {
                fmtCtx->duration = av_rescale_q(finalDurationUs, AVRational{1, 1000000}, AV_TIME_BASE_Q);
                for (unsigned int i = 0; i < fmtCtx->nb_streams; ++i) {
                    AVStream* st = fmtCtx->streams[i];
                    if (st && st->codecpar && st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                        int64_t stDuration = av_rescale_q(finalDurationUs, AVRational{1, 1000000}, st->time_base);
                        if (stDuration > 0) {
                            st->duration = stDuration;
                        }
                    }
                }
            }
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
    hr = d3d11Device->QueryInterface(__uuidof(ID3D11VideoDevice), (void**)&videoDevice);
    if (FAILED(hr)) {
        DLL_Log("[VideoProcessor] Failed to get ID3D11VideoDevice. HR=%x", hr);
        return false;
    }

    // Get video context
    hr = d3d11Context->QueryInterface(__uuidof(ID3D11VideoContext), (void**)&videoContext);
    if (FAILED(hr)) {
        DLL_Log("[VideoProcessor] Failed to get ID3D11VideoContext. HR=%x", hr);
        return false;
    }

    // Store input dimensions (captured frame size)
    inputWidth = width;
    inputHeight = height;

    // Determine output dimensions based on scaling config
    if (savedConfig.scaling.enabled && savedConfig.scaling.outputWidth > 0 && savedConfig.scaling.outputHeight > 0) {
        outputWidth = savedConfig.scaling.outputWidth;
        outputHeight = savedConfig.scaling.outputHeight;
    } else {
        // No scaling or native resolution
        outputWidth = width;
        outputHeight = height;
    }

    // NV12 output textures require even-aligned dimensions
    outputWidth = outputWidth & ~1u;
    outputHeight = outputHeight & ~1u;
    if (outputWidth == 0 || outputHeight == 0) {
        DLL_Log("[VideoProcessor] Dimensions too small after NV12 alignment");
        return false;
    }

    // Check if scaling is actually needed (input != output)
    scalingEnabled = (inputWidth != outputWidth || inputHeight != outputHeight);

    if (scalingEnabled) {
        DLL_Log("[VideoProcessor] GPU SCALING ENABLED: %dx%d -> %dx%d", inputWidth, inputHeight, outputWidth,
                outputHeight);
    } else {
        DLL_Log("[VideoProcessor] Scaling disabled (input matches output: %dx%d)", inputWidth, inputHeight);
    }

    // Create video processor enumerator with potentially different input/output
    // dims
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {};
    contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    contentDesc.InputWidth = inputWidth;
    contentDesc.InputHeight = inputHeight;
    contentDesc.OutputWidth = outputWidth;
    contentDesc.OutputHeight = outputHeight;
    contentDesc.Usage =
        (savedConfig.scaling.quality == "best") ? D3D11_VIDEO_USAGE_OPTIMAL_QUALITY : D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    hr = E_FAIL;
    try {
        hr = videoDevice->CreateVideoProcessorEnumerator(&contentDesc, &videoProcessorEnum);
    } catch (...) {
        hr = E_FAIL;
    }
    if (FAILED(hr)) {
        DLL_Log("[VideoProcessor] Failed to create enumerator. HR=%x", hr);
        return false;
    }

    // Create video processor
    try {
        hr = videoDevice->CreateVideoProcessor(videoProcessorEnum, 0, &videoProcessor);
    } catch (...) {
        hr = E_FAIL;
    }
    if (FAILED(hr)) {
        DLL_Log("[VideoProcessor] Failed to create processor. HR=%x", hr);
        return false;
    }

    // Check if VP supports 2+ input streams for cursor overlay
    D3D11_VIDEO_PROCESSOR_CAPS vpCaps = {};
    hr = videoProcessorEnum->GetVideoProcessorCaps(&vpCaps);
    if (SUCCEEDED(hr)) {
        vpSupportsOverlay = (vpCaps.MaxInputStreams >= 2);
        DLL_Log("[VideoProcessor] MaxInputStreams=%d, overlay support=%s", vpCaps.MaxInputStreams,
                vpSupportsOverlay ? "YES" : "NO");
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
            hr = videoProcessorEnum->GetVideoProcessorFilterRange(D3D11_VIDEO_PROCESSOR_FILTER_EDGE_ENHANCEMENT,
                                                                  &filterRange);

            if (SUCCEEDED(hr)) {
                // Map our 0-100 level to the actual VP filter range
                int filterValue = filterRange.Default;
                if (filterRange.Maximum > filterRange.Minimum) {
                    filterValue = filterRange.Minimum +
                                  (edgeEnhancementLevel * (filterRange.Maximum - filterRange.Minimum) / 100);
                }

                videoContext->VideoProcessorSetStreamFilter(
                    videoProcessor, 0, D3D11_VIDEO_PROCESSOR_FILTER_EDGE_ENHANCEMENT, TRUE, filterValue);

                DLL_Log(
                    "[VideoProcessor] Scaling: quality=%s, sharpness=%d "
                    "(filterValue=%d, range=%d-%d)",
                    savedConfig.scaling.quality.c_str(), edgeEnhancementLevel, filterValue, filterRange.Minimum,
                    filterRange.Maximum);
            } else {
                DLL_Log(
                    "[VideoProcessor] Edge enhancement (sharpness) not supported "
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
        videoContext->VideoProcessorSetStreamSourceRect(videoProcessor, 0, TRUE, &sourceRect);
        // Stream 0: Dest rect = full output frame (scaled)
        videoContext->VideoProcessorSetStreamDestRect(videoProcessor, 0, TRUE, &destRect);
        // Output target = full output surface
        videoContext->VideoProcessorSetOutputTargetRect(videoProcessor, TRUE, &destRect);

        DLL_Log("[VideoProcessor] Scaling rects: source=%dx%d dest=%dx%d", inputWidth, inputHeight, outputWidth,
                outputHeight);
    }

    // Configure color space: Full RGB input -> Limited YCbCr output
    // (video standard) Input: Full range RGB (0-255) from captured
    // game frame
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColorSpace = {};
    inputColorSpace.Usage = 0;          // 0 = Playback, 1 = Video processing
    inputColorSpace.RGB_Range = 0;      // 0 = Full range (0-255), 1 = Studio (16-235)
    inputColorSpace.YCbCr_Matrix = 1;   // 0 = BT.601, 1 = BT.709
    inputColorSpace.YCbCr_xvYCC = 0;    // 0 = Conventional, 1 = Extended
    inputColorSpace.Nominal_Range = 2;  // 2 = Full (0-255) for input

    // Output: Limited range YCbCr (16-235) - video standard for
    // compatibility
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColorSpace = {};
    outputColorSpace.Usage = 0;
    outputColorSpace.RGB_Range = 1;     // 1 = Studio/Limited range (16-235)
    outputColorSpace.YCbCr_Matrix = 1;  // BT.709
    outputColorSpace.YCbCr_xvYCC = 0;
    outputColorSpace.Nominal_Range = 1;  // 1 = Studio (16-235) for output

    videoContext->VideoProcessorSetStreamColorSpace(videoProcessor, 0, &inputColorSpace);
    videoContext->VideoProcessorSetOutputColorSpace(videoProcessor, &outputColorSpace);
    DLL_Log(
        "[VideoProcessor] Color space: Full RGB (0-255) -> "
        "Limited YCbCr "
        "(16-235, BT.709)");

    // Create triple-buffered NV12 staging textures for output
    // IMPORTANT: Use OUTPUT dimensions for the NV12 textures (after scaling)
    D3D11_TEXTURE2D_DESC nv12Desc = {};
    nv12Desc.Width = outputWidth;
    nv12Desc.Height = outputHeight;
    nv12Desc.MipLevels = 1;
    nv12Desc.ArraySize = 1;
    nv12Desc.Format = DXGI_FORMAT_NV12;  // NV12 for NVENC
    nv12Desc.SampleDesc.Count = 1;
    nv12Desc.Usage = D3D11_USAGE_DEFAULT;
    nv12Desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER;

    ID3D11Device* baseDevice = nullptr;
    d3d11Device->QueryInterface(__uuidof(ID3D11Device), (void**)&baseDevice);

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
        hr = baseDevice->CreateTexture2D(&nv12Desc, nullptr, &nv12StagingTextures[i]);
        if (FAILED(hr)) {
            DLL_Log(
                "[VideoProcessor] Failed to create NV12 texture %d. "
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

        try {
            hr = videoDevice->CreateVideoProcessorOutputView(nv12StagingTextures[i], videoProcessorEnum,
                                                              &outputViewDesc, &outputViews[i]);
        } catch (...) {
            hr = E_FAIL;
            DLL_Log("[VideoProcessor] CreateVideoProcessorOutputView threw exception for view %d", i);
        }
        if (FAILED(hr)) {
            DLL_Log("[VideoProcessor] Failed to create output view %d. HR=%x", i, hr);
            baseDevice->Release();
            CleanupVideoProcessor();
            return false;
        }
    }
    baseDevice->Release();
    DLL_Log(
        "[VideoProcessor] Created %d NV12 staging textures at %dx%d "
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
    bgraDesc.BindFlags = 0;  // No bind flags = compatible with
                             // CopyResource + VideoProcessor

    ID3D11Device* baseDevice2 = nullptr;
    d3d11Device->QueryInterface(__uuidof(ID3D11Device), (void**)&baseDevice2);
    hr = baseDevice2->CreateTexture2D(&bgraDesc, nullptr, &bgraStagingTexture);
    baseDevice2->Release();

    if (FAILED(hr)) {
        DLL_Log(
            "[VideoProcessor] Failed to create BGRA staging "
            "texture. HR=%x",
            hr);
        return false;
    }
    DLL_Log(
        "[VideoProcessor] Created BGRA staging texture at %dx%d for DD "
        "compatibility",
        inputWidth, inputHeight);

    videoProcessorInit = true;

    if (scalingEnabled) {
        DLL_Log(
            "[VideoProcessor] Initialized with SCALING: %dx%d -> %dx%d "
            "BGRA→NV12",
            inputWidth, inputHeight, outputWidth, outputHeight);
    } else {
        DLL_Log(
            "[VideoProcessor] Initialized for %dx%d "
            "BGRA→NV12 (no scaling)",
            outputWidth, outputHeight);
    }
    return true;
}

bool VideoEncoder::ConvertBGRAtoNV12(ID3D11Texture2D* bgraTexture, ID3D11Texture2D** nv12Output, bool cursorVisible,
                                     int cursorX, int cursorY) {
    if (!videoProcessorInit) {
        if (!InitVideoProcessor())
            return false;
    }

    // Debug: Log texture descriptions on first call per recording
    if (!vpFirstCallLogged) {
        D3D11_TEXTURE2D_DESC srcDesc;
        bgraTexture->GetDesc(&srcDesc);
        DLL_Log("[VP DEBUG] Source tex: %dx%d fmt=%d bind=%x misc=%x", srcDesc.Width, srcDesc.Height, srcDesc.Format,
                srcDesc.BindFlags, srcDesc.MiscFlags);
    }

    // Format detection: route 10-bit/HDR inputs to compute shader → P010 path
    // VP only accepts 8-bit BGRA/RGBA input; 10-bit and FP16 formats need GPU compute conversion
    {
        D3D11_TEXTURE2D_DESC srcDesc;
        bgraTexture->GetDesc(&srcDesc);
        if (srcDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM || srcDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            if (!use10BitPipeline) {
                use10BitPipeline = true;
                DLL_Log("[VP] 10-bit/HDR input detected (fmt=%d), using P010 pipeline", srcDesc.Format);
            }
            ID3D11Texture2D* p010Tex = nullptr;
            if (ConvertRGBtoP010_GPU(bgraTexture, srcDesc.Format, &p010Tex, srcDesc.Width, srcDesc.Height)) {
                *nv12Output = p010Tex;
                return true;
            }
            DLL_Log("[VP] P010 GPU conversion failed, frame dropped");
            return false;
        } else if (use10BitPipeline) {
            use10BitPipeline = false;
            DLL_Log("[VP] Input switched back to 8-bit, using VP NV12 pipeline");
        }
    }

    // Try to create input view directly from source texture first
    // This works for inject mode where the texture has compatible
    // bind flags Only fall back to staging copy for Desktop
    // Duplication (screengrab) textures
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc = {};
    inputViewDesc.FourCC = 0;
    inputViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputViewDesc.Texture2D.MipSlice = 0;

    ID3D11VideoProcessorInputView* localInputView = nullptr;

    // Debug: Compare texture device vs VP device on first call per recording
    if (!vpDeviceCompareLogged) {
        vpDeviceCompareLogged = true;
        ID3D11Device* texDev = nullptr;
        bgraTexture->GetDevice(&texDev);
        // Compare IUnknown identity (COM identity rule)
        IUnknown* texUnk = nullptr;
        IUnknown* vpDevUnk = nullptr;
        if (texDev)
            texDev->QueryInterface(__uuidof(IUnknown), (void**)&texUnk);

        // Get device from videoDevice
        ID3D11Device* vpBaseDevice = nullptr;
        if (videoDevice) {
            videoDevice->QueryInterface(__uuidof(ID3D11Device), (void**)&vpBaseDevice);
            if (vpBaseDevice)
                vpBaseDevice->QueryInterface(__uuidof(IUnknown), (void**)&vpDevUnk);
        }
        bool sameDevice = (texUnk && vpDevUnk && texUnk == vpDevUnk);
        DLL_Log("[VP DEBUG] Texture device=%p VP device=%p IUnknown: tex=%p vp=%p SAME=%s", texDev, vpBaseDevice,
                texUnk, vpDevUnk, sameDevice ? "YES" : "NO");
        if (texUnk)
            texUnk->Release();
        if (vpDevUnk)
            vpDevUnk->Release();
        if (vpBaseDevice)
            vpBaseDevice->Release();
        if (texDev)
            texDev->Release();
    }

    // If input is RGBA, swap R/B channels to produce BGRA before VP processing.
    // D3D11 Video Processor expects BGRA input; DXVK KMT textures may be RGBA.
    // A fullscreen shader pass with a BGRA render target handles the byte reorder.
    ID3D11Texture2D* vpInputTexture = bgraTexture;
    bool needReleaseConverted = false;
    {
        D3D11_TEXTURE2D_DESC srcDesc;
        bgraTexture->GetDesc(&srcDesc);
        if (srcDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
            ID3D11Texture2D* converted = SwapRBChannels(bgraTexture, srcDesc.Width, srcDesc.Height);
            if (converted) {
                vpInputTexture = converted;
                needReleaseConverted = true;
                if (!vpFirstCallLogged)
                    DLL_Log("[VP] RGBA input detected - R/B swap applied before VP");
            } else {
                if (!vpFirstCallLogged)
                    DLL_Log("[VP] RGBA input: R/B swap failed, proceeding with original (colors may be wrong)");
            }
        }
    }

    vpFirstCallLogged = true;

    HRESULT hr = E_FAIL;
    try {
        hr = videoDevice->CreateVideoProcessorInputView(vpInputTexture, videoProcessorEnum, &inputViewDesc,
                                                         &localInputView);
    } catch (...) {
        hr = E_FAIL;
        DLL_Log("[VP] CreateVideoProcessorInputView threw exception (fmt=%d)", vpInputTexture ? 0 : -1);
    }

    // Log CreateVideoProcessorInputView result on first call per recording
    if (!vpInputViewLogged) {
        vpInputViewLogged = true;
        D3D11_TEXTURE2D_DESC vpDesc;
        vpInputTexture->GetDesc(&vpDesc);
        DLL_Log("[VP] CreateVideoProcessorInputView(fmt=%d, bind=%x): HR=%x%s", vpDesc.Format, vpDesc.BindFlags, hr,
                SUCCEEDED(hr) ? " (direct OK)" : "");
    }

    if (FAILED(hr)) {
        // Direct access failed (likely Desktop Duplication texture with
        // incompatible bind flags) Fall back to staging copy - but we
        // need to match the format!
        static bool stagingLogged = false;
        if (!stagingLogged) {
            DLL_Log(
                "[VP] Direct input view failed (HR=%x), using "
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
                DLL_Log("[VP] Recreating staging texture: fmt %d -> %d", stageDesc.Format, srcDesc.Format);
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
            stageDesc.Format = srcDesc.Format;  // Match source format!
            stageDesc.SampleDesc.Count = 1;
            stageDesc.Usage = D3D11_USAGE_DEFAULT;
            stageDesc.BindFlags = 0;  // Compatible with VP

            ID3D11Device* baseDevice = nullptr;
            d3d11Device->QueryInterface(__uuidof(ID3D11Device), (void**)&baseDevice);
            hr = baseDevice->CreateTexture2D(&stageDesc, nullptr, &bgraStagingTexture);
            baseDevice->Release();

            if (FAILED(hr)) {
                DLL_Log("[VP] Failed to create staging texture: HR=%x", hr);
                return false;
            }
            DLL_Log("[VP] Created staging texture: fmt=%d", srcDesc.Format);
        }

        // Copy to staging
        ID3D11DeviceContext* ctx = nullptr;
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
        try {
            hr = videoDevice->CreateVideoProcessorInputView(bgraStagingTexture, videoProcessorEnum, &inputViewDesc,
                                                            &localInputView);
        } catch (...) {
            hr = E_FAIL;
        }
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
    bool useCursorStream = cursorVisible && vpSupportsOverlay && activeCursor && activeCursor->inputView;
    if (useCursorStream) {
        // Get actual DPI for virtual→physical coordinate conversion.
        // GetCursorInfo() returns cursor position in virtual (DPI-scaled) screen coords.
        // The frame is in physical pixels, so we need to convert.
        UINT dpiX = 96, dpiY = 96;
        HDC hdc = GetDC(nullptr);
        if (hdc) {
            dpiX = GetDeviceCaps(hdc, LOGPIXELSX);
            dpiY = GetDeviceCaps(hdc, LOGPIXELSY);
            ReleaseDC(nullptr, hdc);
        }
        float positionScaleX = dpiX / 96.0f;
        float positionScaleY = dpiY / 96.0f;

        static int dpiLogCount = 0;
        if (dpiLogCount++ % 600 == 0) {
            DLL_Log("[Cursor] DPI position scale: %.2f x %.2f (DPI=%ux%u, frame=%dx%d)", positionScaleX, positionScaleY,
                    dpiX, dpiY, width, height);
        }

        // Cursor size: already pre-scaled to DPI-correct display size in GetCursorCacheEntry.
        // No additional scaling needed — VP does 1:1 blit (no bilinear blur).
        int scaledWidth = (int)activeCursor->width;
        int scaledHeight = (int)activeCursor->height;

        // Convert cursor position from virtual → physical pixels
        int physicalX = (int)(cursorX * positionScaleX);
        int physicalY = (int)(cursorY * positionScaleY);

        // Apply hotspot offset (already pre-scaled in cache entry)
        int hotspotXScaled = activeCursor->hotspotX;
        int hotspotYScaled = activeCursor->hotspotY;

        // Set cursor destination rectangle
        RECT cursorRect;
        cursorRect.left = physicalX - hotspotXScaled;
        cursorRect.top = physicalY - hotspotYScaled;
        cursorRect.right = cursorRect.left + scaledWidth;
        cursorRect.bottom = cursorRect.top + scaledHeight;

        // Log cursor rect periodically for debugging
        static int logCounter = 0;
        if (logCounter++ % 200 == 0) {
            DLL_Log("[Cursor] Rect: (%d,%d)-(%d,%d) posScale=%.2f pos=(%d,%d) size=%dx%d", cursorRect.left,
                    cursorRect.top, cursorRect.right, cursorRect.bottom, positionScaleX, cursorX, cursorY, scaledWidth,
                    scaledHeight);
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
            videoContext->VideoProcessorSetStreamDestRect(videoProcessor, 1, TRUE, &clippedRect);
            videoContext->VideoProcessorSetStreamAlpha(videoProcessor, 1, TRUE, 1.0f);

            streams[1].Enable = TRUE;
            streams[1].pInputSurface = activeCursor->inputView;
            streamCount = 2;
        } else {
            // Cursor completely out of bounds - don't draw
            // (streams[1].Enable is already FALSE by default init)
        }
    }

    // Acquire keyed mutex if the source texture has SHARED_KEYEDMUTEX.
    // NT-handle shared textures require mutex acquisition before GPU reads.
    IDXGIKeyedMutex* keyedMutex = nullptr;
    bgraTexture->QueryInterface(IID_PPV_ARGS(&keyedMutex));
    if (keyedMutex) {
        HRESULT kmHr = keyedMutex->AcquireSync(0, 0);
        if (FAILED(kmHr)) {
            static int kmFailCount = 0;
            if (kmFailCount++ < 5) {
                DLL_Log("[VideoProcessor] KeyedMutex AcquireSync failed: HR=%x", kmHr);
            }
            keyedMutex->Release();
            keyedMutex = nullptr;
        }
    }

    // Perform the conversion using current buffer
    int bufIdx = currentNV12Buffer;
    hr = videoContext->VideoProcessorBlt(videoProcessor, outputViews[bufIdx], 0, streamCount, streams);

    // GPU flush + retry: under 100% GPU load, the VP engine may be starved on first attempt.
    // Flush the GPU pipeline to drain pending work, then retry once.
    if (FAILED(hr)) {
        ID3D11DeviceContext* immediateCtx = nullptr;
        d3d11Device->GetImmediateContext(&immediateCtx);
        if (immediateCtx) {
            immediateCtx->Flush();
            Sleep(1);  // Yield to let GPU schedule VP operation
            hr = videoContext->VideoProcessorBlt(videoProcessor, outputViews[bufIdx], 0, streamCount, streams);
            immediateCtx->Release();
        }
    }

    localInputView->Release();

    if (keyedMutex) {
        keyedMutex->ReleaseSync(0);
        keyedMutex->Release();
    }

    if (FAILED(hr)) {
        static int bltFailCount = 0;
        if (bltFailCount++ < 5) {
            D3D11_TEXTURE2D_DESC srcDesc = {};
            bgraTexture->GetDesc(&srcDesc);
            DLL_Log(
                "[VideoProcessor] Blt failed (after retry). HR=%x streams=%u bufIdx=%d "
                "srcFmt=%d srcW=%u srcH=%u srcBind=%x srcMisc=%x "
                "inputW=%d inputH=%d outputW=%d outputH=%d",
                hr, streamCount, bufIdx, srcDesc.Format, srcDesc.Width, srcDesc.Height, srcDesc.BindFlags,
                srcDesc.MiscFlags, inputWidth, inputHeight, outputWidth, outputHeight);
        }
        if (needReleaseConverted)
            vpInputTexture->Release();
        return false;
    }

    if (needReleaseConverted)
        vpInputTexture->Release();

    // Return current buffer and advance to next
    *nv12Output = nv12StagingTextures[bufIdx];
    nv12StagingTextures[bufIdx]->AddRef();  // Caller will release
    currentNV12Buffer = (currentNV12Buffer + 1) % nv12BufferCount;
    return true;
}

// HLSL shader: fullscreen triangle that samples RGBA input and writes to BGRA render target.
// The pixel shader is identity — the BGRA render target format handles the byte reorder.
static const char* SWAP_RB_SHADER_SRC = R"(
Texture2D texIn : register(t0);
SamplerState sam : register(s0);

struct VS_OUT {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD;
};

VS_OUT VS_Main(uint id : SV_VertexID) {
    VS_OUT o;
    o.uv  = float2((id == 1) ? 2.0f : 0.0f, (id == 2) ? 2.0f : 0.0f);
    o.pos = float4(o.uv.x * 2.0f - 1.0f, 1.0f - o.uv.y * 2.0f, 0.0f, 1.0f);
    return o;
}

float4 PS_Main(VS_OUT input) : SV_TARGET {
    float4 c = texIn.Sample(sam, input.uv);
    return c;  // BGRA render target handles RGBA->BGRA byte reorder
}
)";

bool VideoEncoder::EnsureSwapRBShader() {
    if (swapRBShaderCreated)
        return true;

    HMODULE d3dCompiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!d3dCompiler) {
        DLL_Log("[SwapRB] Failed to load d3dcompiler_47.dll");
        return false;
    }

    typedef HRESULT(WINAPI * pD3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
                                          LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    pD3DCompile d3dCompile = (pD3DCompile)GetProcAddress(d3dCompiler, "D3DCompile");
    if (!d3dCompile) {
        DLL_Log("[SwapRB] Failed to get D3DCompile");
        FreeLibrary(d3dCompiler);
        return false;
    }

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* errBlob = nullptr;

    HRESULT hr = d3dCompile(SWAP_RB_SHADER_SRC, strlen(SWAP_RB_SHADER_SRC), nullptr, nullptr, nullptr, "VS_Main",
                            "vs_4_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) {
        if (errBlob) {
            DLL_Log("[SwapRB] VS error: %s", (char*)errBlob->GetBufferPointer());
            errBlob->Release();
        }
        FreeLibrary(d3dCompiler);
        return false;
    }

    hr = d3dCompile(SWAP_RB_SHADER_SRC, strlen(SWAP_RB_SHADER_SRC), nullptr, nullptr, nullptr, "PS_Main", "ps_4_0", 0,
                    0, &psBlob, &errBlob);
    if (FAILED(hr)) {
        if (errBlob) {
            DLL_Log("[SwapRB] PS error: %s", (char*)errBlob->GetBufferPointer());
            errBlob->Release();
        }
        vsBlob->Release();
        FreeLibrary(d3dCompiler);
        return false;
    }

    hr = d3d11Device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &swapRBShaderVS);
    vsBlob->Release();
    if (FAILED(hr)) {
        DLL_Log("[SwapRB] CreateVertexShader failed: HR=%x", hr);
        psBlob->Release();
        FreeLibrary(d3dCompiler);
        return false;
    }

    hr = d3d11Device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &swapRBShaderPS);
    psBlob->Release();
    FreeLibrary(d3dCompiler);
    if (FAILED(hr)) {
        DLL_Log("[SwapRB] CreatePixelShader failed: HR=%x", hr);
        return false;
    }

    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampDesc.AddressU = sampDesc.AddressV = sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = d3d11Device->CreateSamplerState(&sampDesc, &swapRBSampler);
    if (FAILED(hr)) {
        DLL_Log("[SwapRB] CreateSamplerState failed: HR=%x", hr);
        return false;
    }

    swapRBShaderCreated = true;
    DLL_Log("[SwapRB] Shader created successfully");
    return true;
}

ID3D11Texture2D* VideoEncoder::SwapRBChannels(ID3D11Texture2D* input, uint32_t w, uint32_t h) {
    if (!EnsureSwapRBShader())
        return nullptr;

    // Recreate BGRA output texture if size changed
    if (!swapRBTexture || swapRBTexWidth != w || swapRBTexHeight != h) {
        if (swapRBTextureRTV) {
            swapRBTextureRTV->Release();
            swapRBTextureRTV = nullptr;
        }
        if (swapRBTexture) {
            swapRBTexture->Release();
            swapRBTexture = nullptr;
        }

        D3D11_TEXTURE2D_DESC outDesc = {};
        outDesc.Width = w;
        outDesc.Height = h;
        outDesc.MipLevels = 1;
        outDesc.ArraySize = 1;
        outDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        outDesc.SampleDesc.Count = 1;
        outDesc.Usage = D3D11_USAGE_DEFAULT;
        outDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = d3d11Device->CreateTexture2D(&outDesc, nullptr, &swapRBTexture);
        if (FAILED(hr)) {
            DLL_Log("[SwapRB] Failed to create BGRA output texture: HR=%x", hr);
            return nullptr;
        }

        hr = d3d11Device->CreateRenderTargetView(swapRBTexture, nullptr, &swapRBTextureRTV);
        if (FAILED(hr)) {
            DLL_Log("[SwapRB] Failed to create RTV: HR=%x", hr);
            swapRBTexture->Release();
            swapRBTexture = nullptr;
            return nullptr;
        }

        swapRBTexWidth = w;
        swapRBTexHeight = h;
    }

    // Create SRV for the RGBA input
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srv = nullptr;
    HRESULT hr = d3d11Device->CreateShaderResourceView(input, &srvDesc, &srv);
    if (FAILED(hr)) {
        DLL_Log("[SwapRB] Failed to create SRV for input: HR=%x", hr);
        return nullptr;
    }

    // Draw fullscreen triangle: RGBA input → BGRA output
    D3D11_VIEWPORT vp = {};
    vp.Width = (float)w;
    vp.Height = (float)h;
    vp.MaxDepth = 1.0f;
    d3d11Context->RSSetViewports(1, &vp);
    d3d11Context->OMSetRenderTargets(1, &swapRBTextureRTV, nullptr);
    d3d11Context->VSSetShader(swapRBShaderVS, nullptr, 0);
    d3d11Context->PSSetShader(swapRBShaderPS, nullptr, 0);
    d3d11Context->PSSetShaderResources(0, 1, &srv);
    d3d11Context->PSSetSamplers(0, 1, &swapRBSampler);
    d3d11Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d3d11Context->IASetInputLayout(nullptr);
    d3d11Context->Draw(3, 0);

    // Unbind render target and SRV
    ID3D11RenderTargetView* nullRTV = nullptr;
    d3d11Context->OMSetRenderTargets(1, &nullRTV, nullptr);
    ID3D11ShaderResourceView* nullSRV = nullptr;
    d3d11Context->PSSetShaderResources(0, 1, &nullSRV);
    srv->Release();

    swapRBTexture->AddRef();  // Caller releases
    return swapRBTexture;
}

void VideoEncoder::CleanupVideoProcessor() {
    for (auto* view : outputViews) {
        if (view) {
            view->Release();
        }
    }
    for (auto* tex : nv12StagingTextures) {
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

    // Cleanup SwapRB shader resources
    if (swapRBTextureRTV) {
        swapRBTextureRTV->Release();
        swapRBTextureRTV = nullptr;
    }
    if (swapRBTexture) {
        swapRBTexture->Release();
        swapRBTexture = nullptr;
    }
    if (swapRBSampler) {
        swapRBSampler->Release();
        swapRBSampler = nullptr;
    }
    if (swapRBShaderPS) {
        swapRBShaderPS->Release();
        swapRBShaderPS = nullptr;
    }
    if (swapRBShaderVS) {
        swapRBShaderVS->Release();
        swapRBShaderVS = nullptr;
    }
    swapRBShaderCreated = false;
    swapRBTexWidth = 0;
    swapRBTexHeight = 0;

    // Reset per-recording log flags
    vpFirstCallLogged = false;
    vpDeviceCompareLogged = false;
    vpInputViewLogged = false;
}

// ============================================================================
// LRU Cursor Cache Implementation
// ============================================================================

void VideoEncoder::CleanupCursorCache() {
    for (int i = 0; i < kCursorCacheSize; i++) {
        auto& entry = cursorCache[i];
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

    // Clean up GPU cursor scaling resources
    if (cursorScaleVS) {
        cursorScaleVS->Release();
        cursorScaleVS = nullptr;
    }
    if (cursorScalePS) {
        cursorScalePS->Release();
        cursorScalePS = nullptr;
    }
    if (cursorScaleSampler) {
        cursorScaleSampler->Release();
        cursorScaleSampler = nullptr;
    }
    cursorScalingInit = false;
}

bool VideoEncoder::InitCursorScaling() {
    if (cursorScalingInit)
        return true;

    if (!d3d11Device)
        return false;

    // Minimal fullscreen-triangle shaders for point-filtered texture copy/scale
    static const char* SCALE_SHADER_SRC = R"(
Texture2D srcTex : register(t0);
SamplerState pointSam : register(s0);
struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };
VS_OUT VS_Main(uint id : SV_VertexID) {
    VS_OUT o;
    o.uv = float2((id == 1) ? 2.0f : 0.0f, (id == 2) ? 2.0f : 0.0f);
    o.pos = float4(o.uv.x * 2.0f - 1.0f, 1.0f - o.uv.y * 2.0f, 0.0f, 1.0f);
    return o;
}
float4 PS_Main(VS_OUT i) : SV_TARGET { return srcTex.Sample(pointSam, i.uv); }
)";

    ID3D11Device* baseDev = nullptr;
    d3d11Device->QueryInterface(IID_PPV_ARGS(&baseDev));
    if (!baseDev)
        return false;

    // Load compiler
    HMODULE d3dCompiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!d3dCompiler) {
        baseDev->Release();
        return false;
    }
    typedef HRESULT(WINAPI * PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
                                             LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    auto d3dCompile = (PFN_D3DCompile)GetProcAddress(d3dCompiler, "D3DCompile");
    if (!d3dCompile) {
        FreeLibrary(d3dCompiler);
        baseDev->Release();
        return false;
    }

    // Compile shaders
    ID3DBlob *vsBlob = nullptr, *psBlob = nullptr, *errBlob = nullptr;
    HRESULT hr = d3dCompile(SCALE_SHADER_SRC, strlen(SCALE_SHADER_SRC), nullptr, nullptr, nullptr, "VS_Main", "vs_4_0",
                            0, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) {
        if (errBlob)
            errBlob->Release();
        FreeLibrary(d3dCompiler);
        baseDev->Release();
        return false;
    }
    hr = d3dCompile(SCALE_SHADER_SRC, strlen(SCALE_SHADER_SRC), nullptr, nullptr, nullptr, "PS_Main", "ps_4_0", 0, 0,
                    &psBlob, &errBlob);
    if (FAILED(hr)) {
        vsBlob->Release();
        if (errBlob)
            errBlob->Release();
        FreeLibrary(d3dCompiler);
        baseDev->Release();
        return false;
    }

    hr = baseDev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &cursorScaleVS);
    vsBlob->Release();
    if (FAILED(hr)) {
        psBlob->Release();
        FreeLibrary(d3dCompiler);
        baseDev->Release();
        return false;
    }

    hr = baseDev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &cursorScalePS);
    psBlob->Release();
    FreeLibrary(d3dCompiler);
    if (FAILED(hr)) {
        baseDev->Release();
        return false;
    }

    // Point sampler for crisp nearest-neighbor scaling
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = baseDev->CreateSamplerState(&sampDesc, &cursorScaleSampler);
    baseDev->Release();
    if (FAILED(hr))
        return false;

    cursorScalingInit = true;
    return true;
}

bool VideoEncoder::ScaleCursorOnGPU(ID3D11Texture2D* srcTex, uint32_t srcW, uint32_t srcH, ID3D11Texture2D** dstTex,
                                    uint32_t dstW, uint32_t dstH) {
    if (!cursorScalingInit && !InitCursorScaling())
        return false;

    ID3D11Device* baseDev = nullptr;
    d3d11Device->QueryInterface(IID_PPV_ARGS(&baseDev));
    if (!baseDev)
        return false;

    ID3D11DeviceContext* baseCtx = nullptr;
    d3d11Context->QueryInterface(IID_PPV_ARGS(&baseCtx));
    if (!baseCtx) {
        baseDev->Release();
        return false;
    }

    // Create source SRV
    ID3D11ShaderResourceView* srcSRV = nullptr;
    HRESULT hr = baseDev->CreateShaderResourceView(srcTex, nullptr, &srcSRV);
    if (FAILED(hr)) {
        baseDev->Release();
        baseCtx->Release();
        return false;
    }

    // Create destination texture (with RTV bind for rendering)
    D3D11_TEXTURE2D_DESC dstDesc = {};
    dstDesc.Width = dstW;
    dstDesc.Height = dstH;
    dstDesc.MipLevels = 1;
    dstDesc.ArraySize = 1;
    dstDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    dstDesc.SampleDesc.Count = 1;
    dstDesc.Usage = D3D11_USAGE_DEFAULT;
    dstDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    ID3D11Texture2D* scaledTex = nullptr;
    hr = baseDev->CreateTexture2D(&dstDesc, nullptr, &scaledTex);
    if (FAILED(hr)) {
        srcSRV->Release();
        baseDev->Release();
        baseCtx->Release();
        return false;
    }

    // Create RTV for destination
    ID3D11RenderTargetView* dstRTV = nullptr;
    hr = baseDev->CreateRenderTargetView(scaledTex, nullptr, &dstRTV);
    if (FAILED(hr)) {
        scaledTex->Release();
        srcSRV->Release();
        baseDev->Release();
        baseCtx->Release();
        return false;
    }

    // Save state
    ID3D11RenderTargetView* oldRTV = nullptr;
    ID3D11DepthStencilView* oldDSV = nullptr;
    D3D11_VIEWPORT oldVP;
    UINT numVPs = 1;
    baseCtx->OMGetRenderTargets(1, &oldRTV, &oldDSV);
    baseCtx->RSGetViewports(&numVPs, &oldVP);

    // Set up render state
    D3D11_VIEWPORT vp = {};
    vp.Width = (float)dstW;
    vp.Height = (float)dstH;
    vp.MaxDepth = 1.0f;

    float clearColor[4] = {0, 0, 0, 0};
    baseCtx->OMSetRenderTargets(1, &dstRTV, nullptr);
    baseCtx->RSSetViewports(1, &vp);
    baseCtx->ClearRenderTargetView(dstRTV, clearColor);

    // Set shaders
    baseCtx->VSSetShader(cursorScaleVS, nullptr, 0);
    baseCtx->PSSetShader(cursorScalePS, nullptr, 0);
    baseCtx->PSSetShaderResources(0, 1, &srcSRV);
    baseCtx->PSSetSamplers(0, 1, &cursorScaleSampler);

    // Disable blending (straight copy)
    baseCtx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

    // Draw fullscreen triangle
    baseCtx->IASetInputLayout(nullptr);
    baseCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    baseCtx->Draw(3, 0);

    // Restore state
    baseCtx->OMSetRenderTargets(1, &oldRTV, oldDSV);
    baseCtx->RSSetViewports(1, &oldVP);

    if (oldRTV)
        oldRTV->Release();
    if (oldDSV)
        oldDSV->Release();

    // Cleanup
    dstRTV->Release();
    srcSRV->Release();
    baseDev->Release();
    baseCtx->Release();

    *dstTex = scaledTex;
    return true;
}

// ============================================================================
// GPU Compute Shader: RGB→YUV P010 Conversion for 10-bit SDR/HDR Capture
// ============================================================================

// Compute shader: writes Y and UV values to StructuredBuffers.
// Handles both gamma-encoded sRGB (BGRA8) and linear RGB (FP16 from WGC) input.
// When isLinear=1, applies sRGB gamma encoding before BT.709 YUV conversion.
static const char* RGB_TO_YUV_CS_SRC = R"(
Texture2D<float4> srcTex : register(t0);
RWStructuredBuffer<uint> yOut : register(u0);    // w*h entries, one Y per pixel
RWStructuredBuffer<uint> uvUOut : register(u1);  // (w/2)*(h/2) entries
RWStructuredBuffer<uint> uvVOut : register(u2);  // (w/2)*(h/2) entries

cbuffer GammaCB : register(b0)
{
    uint isLinear;  // 1 = input is linear RGB (FP16), apply gamma before BT.709
    uint _pad0;
    uint _pad1;
    uint _pad2;
};

// Approximate sRGB gamma encoding: linear -> sRGB
float3 LinearToSRGB(float3 c)
{
    // Fast approximation: pow(x, 1/2.2)
    // Accurate enough for YUV conversion, avoids expensive exact sRGB curve
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055;
    return float3(c.r < 0.0031308 ? lo.r : hi.r,
                  c.g < 0.0031308 ? lo.g : hi.g,
                  c.b < 0.0031308 ? lo.b : hi.b);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    uint w, h;
    srcTex.GetDimensions(w, h);
    if (dtid.x >= w || dtid.y >= h) return;

    float4 c = srcTex.Load(dtid.xyz);
    float3 rgb = c.rgb;

    // If input is linear (FP16 from WGC), encode to sRGB gamma before BT.709
    if (isLinear != 0)
    {
        rgb = LinearToSRGB(saturate(rgb));
    }

    // BT.709 RGB→YUV (expects gamma-encoded sRGB input)
    float Yf = saturate(0.2126 * rgb.r + 0.7152 * rgb.g + 0.0722 * rgb.b);
    float Uf = saturate(-0.1146 * rgb.r - 0.3854 * rgb.g + 0.5000 * rgb.b + 0.5);
    float Vf = saturate(0.5000 * rgb.r - 0.4542 * rgb.g - 0.0458 * rgb.b + 0.5);

    // P010 range: 10-bit in 16-bit container
    uint Y = (uint)(Yf * 65472.0 + 0.5);
    uint U = (uint)(Uf * 65472.0 + 0.5);
    uint V = (uint)(Vf * 65472.0 + 0.5);

    // Y: one value per pixel
    yOut[dtid.y * w + dtid.x] = Y;

    // UV: 4:2:0 subsampling, one value per 2x2 block (top-left pixel writes)
    if ((dtid.x & 1) == 0 && (dtid.y & 1) == 0) {
        uint uvIdx = (dtid.y >> 1) * (w >> 1) + (dtid.x >> 1);
        uvUOut[uvIdx] = U;
        uvVOut[uvIdx] = V;
    }
}
)";

bool VideoEncoder::InitRgbToYuvCS() {
    if (rgbToYuvInit)
        return true;

    if (!d3d11Device) {
        DLL_Log("[P010] InitRgbToYuvCS: d3d11Device is null");
        return false;
    }

    // Load compiler
    HMODULE d3dCompiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!d3dCompiler) {
        DLL_Log("[P010] InitRgbToYuvCS: failed to load d3dcompiler_47.dll");
        return false;
    }
    typedef HRESULT(WINAPI * PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
                                             LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    auto d3dCompile = (PFN_D3DCompile)GetProcAddress(d3dCompiler, "D3DCompile");
    if (!d3dCompile) {
        FreeLibrary(d3dCompiler);
        return false;
    }

    ID3D11Device* baseDev = nullptr;
    d3d11Device->QueryInterface(IID_PPV_ARGS(&baseDev));
    if (!baseDev) {
        FreeLibrary(d3dCompiler);
        return false;
    }

    HRESULT hr;

    // 1. Compile compute shader
    {
        ID3DBlob* csBlob = nullptr;
        ID3DBlob* errBlob = nullptr;
        hr = d3dCompile(RGB_TO_YUV_CS_SRC, strlen(RGB_TO_YUV_CS_SRC), nullptr, nullptr, nullptr, "CSMain", "cs_5_0", 0,
                        0, &csBlob, &errBlob);
        if (FAILED(hr)) {
            if (errBlob) {
                DLL_Log("[P010] CS compile error: %s", (const char*)errBlob->GetBufferPointer());
                errBlob->Release();
            }
            baseDev->Release();
            FreeLibrary(d3dCompiler);
            return false;
        }
        hr = baseDev->CreateComputeShader(csBlob->GetBufferPointer(), csBlob->GetBufferSize(), nullptr, &rgbToYuvCS);
        csBlob->Release();
        if (FAILED(hr)) {
            DLL_Log("[P010] InitRgbToYuvCS: CreateComputeShader failed HR=0x%08X", hr);
            baseDev->Release();
            FreeLibrary(d3dCompiler);
            return false;
        }
    }

    FreeLibrary(d3dCompiler);

    // 2. Create constant buffer for gamma flag
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = 16;  // 4 uints (aligned to 16 bytes)
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = baseDev->CreateBuffer(&cbDesc, nullptr, &gammaCB);
    baseDev->Release();
    if (FAILED(hr)) {
        DLL_Log("[P010] Failed to create gamma constant buffer: HR=0x%08X", hr);
        return false;
    }

    rgbToYuvInit = true;
    return true;
}

void VideoEncoder::SetP010ShaderInput(bool isLinear) {
    if (!gammaCB || !d3d11Context)
        return;
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = d3d11Context->Map(gammaCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        uint32_t* data = (uint32_t*)mapped.pData;
        data[0] = isLinear ? 1u : 0u;  // isLinear flag
        data[1] = 0;
        data[2] = 0;
        data[3] = 0;
        d3d11Context->Unmap(gammaCB, 0);
    }
}

bool VideoEncoder::ConvertRGBtoP010_GPU(ID3D11Texture2D* srcTex, DXGI_FORMAT srcFmt, ID3D11Texture2D** dstTex,
                                        uint32_t w, uint32_t h) {
    static int failCount = 0;
    auto logFail = [&](const char* step, HRESULT hr) {
        if (failCount++ < 5)
            DLL_Log("[P010] FAILED at %s: HR=0x%08X %ux%u fmt=%d", step, hr, w, h, srcFmt);
    };

    if (!InitRgbToYuvCS()) {
        logFail("InitRgbToYuvCS", E_FAIL);
        return false;
    }

    ID3D11Device* baseDev = nullptr;
    d3d11Device->QueryInterface(IID_PPV_ARGS(&baseDev));
    if (!baseDev)
        return false;

    ID3D11DeviceContext* baseCtx = nullptr;
    d3d11Context->QueryInterface(IID_PPV_ARGS(&baseCtx));
    if (!baseCtx) {
        baseDev->Release();
        return false;
    }

    HRESULT hr;

    // Source texture (WGC pool) may not have SHADER_RESOURCE bind flag.
    // Copy to an intermediate texture with SRV support.
    D3D11_TEXTURE2D_DESC srcDesc;
    srcTex->GetDesc(&srcDesc);

    D3D11_TEXTURE2D_DESC srvTexDesc = srcDesc;
    srvTexDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    srvTexDesc.MiscFlags = 0;
    srvTexDesc.CPUAccessFlags = 0;

    ID3D11Texture2D* srvTex = nullptr;
    hr = baseDev->CreateTexture2D(&srvTexDesc, nullptr, &srvTex);
    if (FAILED(hr)) {
        logFail("CreateSRVTex", hr);
        baseDev->Release();
        baseCtx->Release();
        return false;
    }
    baseCtx->CopyResource(srvTex, srcTex);

    // Create source SRV
    ID3D11ShaderResourceView* srcSRV = nullptr;
    hr = baseDev->CreateShaderResourceView(srvTex, nullptr, &srcSRV);
    if (FAILED(hr)) {
        logFail("CreateShaderResourceView", hr);
        srvTex->Release();
        baseDev->Release();
        baseCtx->Release();
        return false;
    }

    // Create StructuredBuffers: one uint per pixel/value (no packing)
    uint32_t yCount = w * h;
    uint32_t uvCount = (w / 2) * (h / 2);

    auto createBuffer = [&](uint32_t count, ID3D11Buffer** out) -> bool {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = count * sizeof(uint32_t);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = sizeof(uint32_t);
        HRESULT r = baseDev->CreateBuffer(&desc, nullptr, out);
        if (FAILED(r))
            logFail("CreateBuffer", r);
        return SUCCEEDED(r);
    };

    ID3D11Buffer* yBuf = nullptr;
    ID3D11Buffer* uvUBuf = nullptr;
    ID3D11Buffer* uvVBuf = nullptr;
    if (!createBuffer(yCount, &yBuf) || !createBuffer(uvCount, &uvUBuf) || !createBuffer(uvCount, &uvVBuf)) {
        if (yBuf)
            yBuf->Release();
        if (uvUBuf)
            uvUBuf->Release();
        srcSRV->Release();
        baseDev->Release();
        baseCtx->Release();
        return false;
    }

    // Create UAVs
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.Flags = 0;

    ID3D11UnorderedAccessView* yUAV = nullptr;
    ID3D11UnorderedAccessView* uvUUAV = nullptr;
    ID3D11UnorderedAccessView* uvVUAV = nullptr;

    uavDesc.Buffer.NumElements = yCount;
    hr = baseDev->CreateUnorderedAccessView(yBuf, &uavDesc, &yUAV);
    if (FAILED(hr)) {
        logFail("CreateUAV_Y", hr);
        goto cleanup;
    }
    uavDesc.Buffer.NumElements = uvCount;
    hr = baseDev->CreateUnorderedAccessView(uvUBuf, &uavDesc, &uvUUAV);
    if (FAILED(hr)) {
        logFail("CreateUAV_U", hr);
        goto cleanup;
    }
    hr = baseDev->CreateUnorderedAccessView(uvVBuf, &uavDesc, &uvVUAV);
    if (FAILED(hr)) {
        logFail("CreateUAV_V", hr);
        goto cleanup;
    }

    // Clear and dispatch compute shader
    {
        UINT clear[4] = {0, 0, 0, 0};
        baseCtx->ClearUnorderedAccessViewUint(yUAV, clear);
        baseCtx->ClearUnorderedAccessViewUint(uvUUAV, clear);
        baseCtx->ClearUnorderedAccessViewUint(uvVUAV, clear);

        baseCtx->CSSetShader(rgbToYuvCS, nullptr, 0);
        baseCtx->CSSetShaderResources(0, 1, &srcSRV);
        baseCtx->CSSetConstantBuffers(0, 1, &gammaCB);  // isLinear flag
        ID3D11UnorderedAccessView* uavs[] = {yUAV, uvUUAV, uvVUAV};
        UINT initialCounts[] = {0, 0, 0};
        baseCtx->CSSetUnorderedAccessViews(0, 3, uavs, initialCounts);
        UINT groupsX = (w + 7) / 8;
        UINT groupsY = (h + 7) / 8;

        // Set gamma flag: FP16 input is linear RGB (needs gamma encoding before BT.709)
        bool isLinear = (srcFmt == DXGI_FORMAT_R16G16B16A16_FLOAT || srcFmt == DXGI_FORMAT_R16G16B16A16_UNORM);
        SetP010ShaderInput(isLinear);

        baseCtx->Dispatch(groupsX, groupsY, 1);

        // Unbind
        ID3D11UnorderedAccessView* nullUAVs[] = {nullptr, nullptr, nullptr};
        ID3D11ShaderResourceView* nullSRV = nullptr;
        UINT zeroCounts[] = {0, 0, 0};
        baseCtx->CSSetUnorderedAccessViews(0, 3, nullUAVs, zeroCounts);
        baseCtx->CSSetShaderResources(0, 1, &nullSRV);
        baseCtx->Flush();
    }

    // Create staging buffers for CPU readback
    {
        D3D11_BUFFER_DESC stageDesc = {};
        stageDesc.Usage = D3D11_USAGE_STAGING;
        stageDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stageDesc.StructureByteStride = sizeof(uint32_t);

        ID3D11Buffer* yStage = nullptr;
        ID3D11Buffer* uvUStage = nullptr;
        ID3D11Buffer* uvVStage = nullptr;

        stageDesc.ByteWidth = yCount * sizeof(uint32_t);
        baseDev->CreateBuffer(&stageDesc, nullptr, &yStage);
        stageDesc.ByteWidth = uvCount * sizeof(uint32_t);
        baseDev->CreateBuffer(&stageDesc, nullptr, &uvUStage);
        baseDev->CreateBuffer(&stageDesc, nullptr, &uvVStage);

        if (!yStage || !uvUStage || !uvVStage) {
            logFail("CreateStagingBuffers", E_FAIL);
            if (yStage)
                yStage->Release();
            if (uvUStage)
                uvUStage->Release();
            if (uvVStage)
                uvVStage->Release();
            goto cleanup;
        }

        // Copy GPU buffers to staging
        baseCtx->CopyResource(yStage, yBuf);
        baseCtx->CopyResource(uvUStage, uvUBuf);
        baseCtx->CopyResource(uvVStage, uvVBuf);

        // Create P010 output texture
        D3D11_TEXTURE2D_DESC p010Desc = {};
        p010Desc.Width = w;
        p010Desc.Height = h;
        p010Desc.MipLevels = 1;
        p010Desc.ArraySize = 1;
        p010Desc.Format = DXGI_FORMAT_P010;
        p010Desc.SampleDesc.Count = 1;
        p010Desc.Usage = D3D11_USAGE_DEFAULT;
        p010Desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        p010Desc.CPUAccessFlags = 0;

        ID3D11Texture2D* p010Tex = nullptr;
        hr = baseDev->CreateTexture2D(&p010Desc, nullptr, &p010Tex);
        if (FAILED(hr)) {
            logFail("CreateP010Texture", hr);
            yStage->Release();
            uvUStage->Release();
            uvVStage->Release();
            goto cleanup;
        }

        // Flush to ensure GPU work (CopyResource) completes before CPU readback
        baseCtx->Flush();

        // Map Y staging buffer and copy to P010 subresource 0
        D3D11_MAPPED_SUBRESOURCE yMapped = {}, uvUMapped = {}, uvVMapped = {};
        hr = baseCtx->Map(yStage, 0, D3D11_MAP_READ, 0, &yMapped);
        if (FAILED(hr) || !yMapped.pData) {
            logFail("MapYStage", hr);
            p010Tex->Release();
            yStage->Release();
            uvUStage->Release();
            uvVStage->Release();
            goto cleanup;
        }
        hr = baseCtx->Map(uvUStage, 0, D3D11_MAP_READ, 0, &uvUMapped);
        if (FAILED(hr) || !uvUMapped.pData) {
            logFail("MapUVUStage", hr);
            baseCtx->Unmap(yStage, 0);
            p010Tex->Release();
            yStage->Release();
            uvUStage->Release();
            uvVStage->Release();
            goto cleanup;
        }
        hr = baseCtx->Map(uvVStage, 0, D3D11_MAP_READ, 0, &uvVMapped);
        if (FAILED(hr) || !uvVMapped.pData) {
            logFail("MapUVVStage", hr);
            baseCtx->Unmap(uvUStage, 0);
            baseCtx->Unmap(yStage, 0);
            p010Tex->Release();
            yStage->Release();
            uvUStage->Release();
            uvVStage->Release();
            goto cleanup;
        }

        // Build P010 Y and UV data in CPU buffers, then upload via a P010 staging texture.
        // Strategy:
        //   1. Create P010 staging texture (USAGE_STAGING + CPU_ACCESS_WRITE)
        //   2. Map subresource 0 (Y plane) - works on all drivers
        //   3. Write Y data via mapped pointer
        //   4. For UV plane (subresource 1): Map may fail on some drivers (E_INVALIDARG)
        //      → fallback to UpdateSubresource on the staging texture (safe, only fails on DEFAULT)
        //   5. CopyResource from staging to final P010 DEFAULT texture (fast GPU blit)
        {
            const uint32_t* yData = (const uint32_t*)yMapped.pData;
            const uint32_t* uData = (const uint32_t*)uvUMapped.pData;
            const uint32_t* vData = (const uint32_t*)uvVMapped.pData;

            // Create P010 staging texture
            D3D11_TEXTURE2D_DESC p010StageDesc = p010Desc;
            p010StageDesc.Usage = D3D11_USAGE_STAGING;
            p010StageDesc.BindFlags = 0;
            p010StageDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;

            ID3D11Texture2D* p010Stage = nullptr;
            hr = baseDev->CreateTexture2D(&p010StageDesc, nullptr, &p010Stage);
            if (FAILED(hr) || !p010Stage) {
                logFail("CreateP010Stage", hr);
                baseCtx->Unmap(uvVStage, 0);
                baseCtx->Unmap(uvUStage, 0);
                baseCtx->Unmap(yStage, 0);
                yStage->Release();
                uvUStage->Release();
                uvVStage->Release();
                p010Tex->Release();
                goto cleanup;
            }

            // Map Y plane (subresource 0) - should work on all drivers
            D3D11_MAPPED_SUBRESOURCE stageYMap = {};
            hr = baseCtx->Map(p010Stage, 0, D3D11_MAP_WRITE, 0, &stageYMap);
            if (FAILED(hr) || !stageYMap.pData) {
                logFail("MapP010StageY", hr);
                p010Stage->Release();
                baseCtx->Unmap(uvVStage, 0);
                baseCtx->Unmap(uvUStage, 0);
                baseCtx->Unmap(yStage, 0);
                yStage->Release();
                uvUStage->Release();
                uvVStage->Release();
                p010Tex->Release();
                goto cleanup;
            }

            // Write Y data
            for (uint32_t row = 0; row < h; row++) {
                uint16_t* dstRow = (uint16_t*)((uint8_t*)stageYMap.pData + row * stageYMap.RowPitch);
                const uint32_t* srcRow = yData + row * w;
                for (uint32_t col = 0; col < w; col++) {
                    dstRow[col] = (uint16_t)(srcRow[col] & 0xFFFFu);
                }
            }
            baseCtx->Unmap(p010Stage, 0);

            // Try Map for UV plane (subresource 1) - may fail on some drivers
            D3D11_MAPPED_SUBRESOURCE stageUVMap = {};
            hr = baseCtx->Map(p010Stage, 1, D3D11_MAP_WRITE, 0, &stageUVMap);
            if (SUCCEEDED(hr) && stageUVMap.pData) {
                // Map succeeded - write UV data directly
                uint32_t uvH = h / 2;
                uint32_t uvW = w / 2;
                for (uint32_t row = 0; row < uvH; row++) {
                    uint32_t* dstRow = (uint32_t*)((uint8_t*)stageUVMap.pData + row * stageUVMap.RowPitch);
                    const uint32_t* uRow = uData + row * uvW;
                    const uint32_t* vRow = vData + row * uvW;
                    for (uint32_t col = 0; col < uvW; col++) {
                        dstRow[col] = (uRow[col] & 0xFFFFu) | ((vRow[col] & 0xFFFFu) << 16);
                    }
                }
                baseCtx->Unmap(p010Stage, 1);
            } else {
                // Map failed (common on some drivers for P010 subresource 1)
                // Use UpdateSubresource on staging texture (safe - only DEFAULT textures crash on NVIDIA)
                uint32_t uvH = h / 2;
                uint32_t uvW = w / 2;
                uint32_t uvPitch = w * 2;
                auto uvBuf = std::make_unique<uint8_t[]>(uvPitch * uvH);
                for (uint32_t row = 0; row < uvH; row++) {
                    uint32_t* dstRow = (uint32_t*)(uvBuf.get() + row * uvPitch);
                    const uint32_t* uRow = uData + row * uvW;
                    const uint32_t* vRow = vData + row * uvW;
                    for (uint32_t col = 0; col < uvW; col++) {
                        dstRow[col] = (uRow[col] & 0xFFFFu) | ((vRow[col] & 0xFFFFu) << 16);
                    }
                }
                D3D11_BOX dstBox = {};
                dstBox.right = w / 2;
                dstBox.bottom = uvH;
                dstBox.back = 1;
                baseCtx->UpdateSubresource(p010Stage, 1, &dstBox, uvBuf.get(), uvPitch, 0);
            }

            // Copy staging P010 to final P010 DEFAULT texture (fast GPU blit)
            baseCtx->CopySubresourceRegion(p010Tex, 0, 0, 0, 0, p010Stage, 0, nullptr);
            baseCtx->CopySubresourceRegion(p010Tex, 1, 0, 0, 0, p010Stage, 1, nullptr);
            p010Stage->Release();

            // Unmap source buffers
            baseCtx->Unmap(uvVStage, 0);
            baseCtx->Unmap(uvUStage, 0);
            baseCtx->Unmap(yStage, 0);

            // Cleanup staging buffers
            yStage->Release();
            uvUStage->Release();
            uvVStage->Release();

            *dstTex = p010Tex;
            static int p010Count = 0;
            if (p010Count++ < 3)
                DLL_Log("[P010] GPU converted %ux%u (fmt=%d) → P010 (P010 staging → CopySubresource)", w, h, srcFmt);
        }  // end P010 data block
    }  // end staging buffers block

cleanup:
    if (uvVUAV)
        uvVUAV->Release();
    if (uvUUAV)
        uvUUAV->Release();
    if (yUAV)
        yUAV->Release();
    if (uvVBuf)
        uvVBuf->Release();
    if (uvUBuf)
        uvUBuf->Release();
    if (yBuf)
        yBuf->Release();
    srcSRV->Release();
    srvTex->Release();
    baseDev->Release();
    baseCtx->Release();

    bool success = (*dstTex != nullptr);
    if (!success && failCount++ < 5) {
        DLL_Log("[P010] FAILED (unspecified) %ux%u fmt=%d dstTex=null", w, h, srcFmt);
    }
    return success;
}

VideoEncoder::CursorCacheEntry* VideoEncoder::GetCursorCacheEntry(HCURSOR handle) {
    if (!handle)
        return nullptr;

    // VideoProcessor must be initialized before we can create cursor input views
    // On first frame, this hasn't happened yet - return existing cached entries
    // only
    if (!videoProcessorInit || !videoDevice || !videoProcessorEnum) {
        // Check if cursor is already cached (can still return cached entries)
        for (int i = 0; i < kCursorCacheSize; i++) {
            if (cursorCache[i].handle == handle && cursorCache[i].texture && cursorCache[i].inputView) {
                return &cursorCache[i];
            }
        }
        return nullptr;  // Can't create new cache entries without VideoProcessor
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
    auto& entry = cursorCache[targetIdx];
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

    // Get hotspot info (original, before any scaling)
    ICONINFO ii;
    int32_t origHotspotX = 0, origHotspotY = 0;
    if (GetIconInfo(icon, &ii)) {
        origHotspotX = (int32_t)ii.xHotspot;
        origHotspotY = (int32_t)ii.yHotspot;
        DeleteObject(ii.hbmColor);
        DeleteObject(ii.hbmMask);
    }

    uint8_t* bitmap = nullptr;
    uint32_t w, h;
    bool mono;

    if (!cursorRenderer->ExtractCursorBitmap(icon, &bitmap, &w, &h, &mono)) {
        DestroyIcon(icon);
        return nullptr;
    }
    DestroyIcon(icon);

    // Pre-scale cursor bitmap to DPI-correct display size on the GPU.
    // Uses a pixel shader with point sampling for crisp nearest-neighbor upscale.
    // This prevents bilinear blur from VP scaling when the extracted bitmap
    // (typically 32x32) is smaller than the expected display size at >100% DPI.
    int expectedW = GetSystemMetrics(SM_CXCURSOR);
    int expectedH = GetSystemMetrics(SM_CYCURSOR);
    bool needsScaling = (expectedW > 0 && expectedH > 0 && ((uint32_t)expectedW != w || (uint32_t)expectedH != h));

    if (needsScaling) {
        // Scale hotspot proportionally
        float sx = (float)expectedW / (float)w;
        float sy = (float)expectedH / (float)h;
        origHotspotX = (int32_t)(origHotspotX * sx);
        origHotspotY = (int32_t)(origHotspotY * sy);
    }

    entry.hotspotX = origHotspotX;
    entry.hotspotY = origHotspotY;

    // Create D3D11 texture at ORIGINAL extracted size (with SRV bind for GPU scaling)
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = w;
    texDesc.Height = h;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = needsScaling ? D3D11_BIND_SHADER_RESOURCE : 0;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = bitmap;
    initData.SysMemPitch = w * 4;

    ID3D11Texture2D* srcTexture = nullptr;
    ID3D11Device* baseDevice = nullptr;
    d3d11Device->QueryInterface(IID_PPV_ARGS(&baseDevice));
    HRESULT hr = baseDevice->CreateTexture2D(&texDesc, &initData, &srcTexture);
    baseDevice->Release();
    delete[] bitmap;

    if (FAILED(hr)) {
        DLL_Log("[CursorCache] CreateTexture2D failed: HR=%x", hr);
        return nullptr;
    }

    if (needsScaling) {
        // GPU scale: render srcTexture to a new texture at target display size
        // using point sampling (nearest-neighbor, no blur)
        ID3D11Texture2D* scaledTex = nullptr;
        if (ScaleCursorOnGPU(srcTexture, w, h, &scaledTex, (uint32_t)expectedW, (uint32_t)expectedH)) {
            srcTexture->Release();
            entry.texture = scaledTex;
            entry.width = (uint32_t)expectedW;
            entry.height = (uint32_t)expectedH;
            DLL_Log("[CursorCache] GPU-scaled cursor: %ux%u -> %ux%u", w, h, expectedW, expectedH);
        } else {
            // GPU scaling failed - fall back to original texture
            DLL_Log("[CursorCache] GPU scaling failed, using original %ux%u", w, h);
            entry.texture = srcTexture;
            entry.width = w;
            entry.height = h;
        }
    } else {
        entry.texture = srcTexture;
        entry.width = w;
        entry.height = h;
    }

    // Create VP input view from the (possibly GPU-scaled) texture
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivDesc = {};
    ivDesc.FourCC = 0;
    ivDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivDesc.Texture2D.MipSlice = 0;

    hr = E_FAIL;
    try {
        hr = videoDevice->CreateVideoProcessorInputView(entry.texture, videoProcessorEnum, &ivDesc, &entry.inputView);
    } catch (...) {
        hr = E_FAIL;
    }

    if (FAILED(hr)) {
        DLL_Log("[CursorCache] CreateVPInputView failed: HR=%x", hr);
        entry.texture->Release();
        entry.texture = nullptr;
        return nullptr;
    }

    entry.handle = handle;
    entry.lastUsedFrame = cursorFrameCounter;

    static int cacheHits = 0, cacheMisses = 0;
    cacheMisses++;
    if ((cacheHits + cacheMisses) % 100 == 0) {
        DLL_Log("[CursorCache] Stats: hits=%d misses=%d (%.1f%% hit rate)", cacheHits, cacheMisses,
                100.0 * cacheHits / (cacheHits + cacheMisses));
    }

    return &entry;
}

int64_t VideoEncoder::GetExpectedFinalDurationUs() const {
    if (lastAssignedVideoPts < 0)
        return 0;

    if (savedConfig.useVFR) {
        return lastAssignedVideoPts + (1000000 / (savedConfig.fps > 0 ? savedConfig.fps : 60));
    } else {
        int fps = (codecCtx && codecCtx->framerate.num > 0) ? codecCtx->framerate.num : savedConfig.fps;
        if (fps <= 0)
            fps = 60;
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
    return av_rescale(encodeFrameCounter, 1000000 * (int64_t)codecCtx->framerate.den, codecCtx->framerate.num);
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
        queueCV.wait(lock, [this] { return !packetQueue.empty() || isStopping || !writerRunning; });

        // Drain queue
        while (!packetQueue.empty()) {
            AVPacket* pkt = packetQueue.front();
            packetQueue.pop();

            size_t pktSize = pkt->size + sizeof(AVPacket);
            currentQueueBytes -= pktSize;

            lock.unlock();  // Release lock while doing I/O

            if (fileOpened && fmtCtx) {
                int ret = av_interleaved_write_frame(fmtCtx, pkt);
                if (ret < 0) {
                    if (asyncWriteErrorCount++ < 10) {
                        char errbuf[AV_ERROR_MAX_STRING_SIZE];
                        av_strerror(ret, errbuf, sizeof(errbuf));
                        DLL_Log(
                            "[VideoEncoder] ERROR: av_interleaved_write_frame failed: "
                            "%d (%s) pts=%lld",
                            ret, errbuf, pkt->pts);
                    }
                }
            }

            av_packet_free(&pkt);
            lock.lock();  // Re-acquire lock
        }

        // Handle Stop/Flush signal
        if (isStopping) {
            DLL_Log("[VideoEncoder] Async Finalize: Starting...");

            // 1. Flush Encoder if valid
            if (initDone && codecCtx && fileOpened) {
                DLL_Log("[VideoEncoder] Async Finalize: Flushing encoder...");
                avcodec_send_frame(codecCtx, nullptr);

                AVPacket* pkt = av_packet_alloc();
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
                        int64_t duration = av_rescale_q(1, codecCtx->time_base, stream->time_base);
                        pkt->duration = duration > 0 ? duration : 1;
                    }

                    if (pkt->pts != AV_NOPTS_VALUE) {
                        int64_t packetEnd = pkt->pts + pkt->duration;
                        int64_t packetEndUs = av_rescale_q(packetEnd, stream->time_base, AVRational{1, 1000000});
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
                DLL_Log("[VideoEncoder] Async Finalize: Flushed %d remaining packets", flushedCount);
            }

            // 2. Write Trailer and Close File
            if (fmtCtx && fileOpened) {
                DLL_Log("[VideoEncoder] Async Finalize: Writing Trailer...");
                // Set container duration so seekers and players see a valid duration
                int64_t finalDurationUs = encodedDurationUs.load(std::memory_order_relaxed);
                if (finalDurationUs > 0) {
                    fmtCtx->duration = av_rescale_q(finalDurationUs, AVRational{1, 1000000}, AV_TIME_BASE_Q);
                    for (unsigned int i = 0; i < fmtCtx->nb_streams; ++i) {
                        AVStream* st = fmtCtx->streams[i];
                        if (st && st->codecpar && st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                            int64_t stDuration = av_rescale_q(finalDurationUs, AVRational{1, 1000000}, st->time_base);
                            if (stDuration > 0) {
                                st->duration = stDuration;
                            }
                        }
                    }
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
            break;  // Exit thread
        }
    }

    DLL_Log("[VideoEncoder] Async Writer Thread Stopped");
}
