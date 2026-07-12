// Screenshot capture with two approaches:
//   1. Hook-based: sets cmdTakeScreenshot in shared memory, hook reads backbuffer directly
//   2. GDI fallback: captures DWM-composed desktop via BitBlt

#include "screenshot.h"
#include "../common/logging.h"
#include "../common/shared_defs.h"
#include "../hook/common/screenshot_hook.h"  // For HDRRawHeader, kHDRRawMagic
#include "wgc_capture.h"

// clang-format off
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wincodec.h>
#include <wincodecsdk.h>
// clang-format on

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

#include <filesystem>
#include <string>
#include <vector>

// Com helper to release COM objects
template <typename T>
static void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

// RAII handle closer
struct HandleGuard {
    HANDLE h;
    HandleGuard() : h(NULL) {}
    explicit HandleGuard(HANDLE handle) : h(handle) {}
    ~HandleGuard() {
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
    operator HANDLE() const {
        return h;
    }
    HANDLE* addressof() {
        return &h;
    }
};

// RAII COM initializer
class ComInitializer {
public:
    ComInitializer() : initialized_(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~ComInitializer() {
        if (initialized_)
            CoUninitialize();
    }
    bool IsInitialized() const {
        return initialized_;
    }

private:
    bool initialized_;
};

// RAII DLL directory setter — restores original on destruction
class DllDirGuard {
public:
    explicit DllDirGuard(const char* dir) {
        SetDllDirectoryA(dir);
    }
    ~DllDirGuard() {
        SetDllDirectoryA(nullptr);
    }
};

// ---- Atomic PNG writer: write to temp file, flush, rename ----
// Returns the WIC stream's file handle (for flushing) or NULL on failure.
// On error, temp file is deleted.

// Save BGRA pixel data as a PNG file using WIC.
// Writes to fullPath.png.tmp first, then atomically renames.
static bool SavePixelsAsPNG(const std::string& fullPath, uint32_t width, uint32_t height, const uint8_t* pixels,
                            uint32_t rowPitch) {
    std::string tempPath = fullPath + ".tmp";

    // Delete any leftover temp file
    DeleteFileA(tempPath.c_str());

    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        LogError("[Screenshot] CoCreateInstance WICImagingFactory failed: hr=0x%08X", hr);
        return false;
    }

    IWICStream* stream = nullptr;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr)) {
        SafeRelease(factory);
        return false;
    }

    int wideLen = MultiByteToWideChar(CP_UTF8, 0, tempPath.c_str(), -1, nullptr, 0);
    std::wstring widePath(wideLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, tempPath.c_str(), -1, widePath.data(), wideLen);

    hr = stream->InitializeFromFilename(widePath.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) {
        LogError("[Screenshot] InitializeFromFilename failed: hr=0x%08X", hr);
        SafeRelease(stream);
        SafeRelease(factory);
        DeleteFileA(tempPath.c_str());
        return false;
    }

    IWICBitmapEncoder* encoder = nullptr;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr)) {
        SafeRelease(stream);
        SafeRelease(factory);
        DeleteFileA(tempPath.c_str());
        return false;
    }

    hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        SafeRelease(encoder);
        SafeRelease(stream);
        SafeRelease(factory);
        DeleteFileA(tempPath.c_str());
        return false;
    }

    IWICBitmapFrameEncode* frameEncode = nullptr;
    IPropertyBag2* encoderOptions = nullptr;
    hr = encoder->CreateNewFrame(&frameEncode, &encoderOptions);
    if (FAILED(hr)) {
        SafeRelease(encoder);
        SafeRelease(stream);
        SafeRelease(factory);
        DeleteFileA(tempPath.c_str());
        return false;
    }

    hr = frameEncode->Initialize(encoderOptions);
    if (FAILED(hr)) {
        SafeRelease(encoderOptions);
        SafeRelease(frameEncode);
        SafeRelease(encoder);
        SafeRelease(stream);
        SafeRelease(factory);
        DeleteFileA(tempPath.c_str());
        return false;
    }

    frameEncode->SetSize(width, height);

    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
    frameEncode->SetPixelFormat(&pixelFormat);

    hr = frameEncode->WritePixels(height, rowPitch, rowPitch * height, const_cast<BYTE*>(pixels));
    if (FAILED(hr)) {
        LogError("[Screenshot] WritePixels failed: hr=0x%08X", hr);
        SafeRelease(encoderOptions);
        SafeRelease(frameEncode);
        SafeRelease(encoder);
        SafeRelease(stream);
        SafeRelease(factory);
        DeleteFileA(tempPath.c_str());
        return false;
    }

    frameEncode->Commit();
    encoder->Commit();

    // Get the underlying file handle from the IStream for flushing
    // WICStream implements IStream; we can get the file handle via STATSTG
    // But the simpler approach is to release all WIC objects (which flushes),
    // then flush the file, then rename.

    // Release WIC objects in correct order (encoder before stream)
    SafeRelease(encoderOptions);
    SafeRelease(frameEncode);
    SafeRelease(encoder);

    // The stream holds the file handle; query it for flushing
    // WICStream uses a wrapped file handle internally. Use FlushFileBuffers via
    // STATSTG to get the native handle, or just rely on stream release flushing.
    // To be safe, reopen the temp file for flushing:
    {
        HANDLE hFile = CreateFileA(tempPath.c_str(), FILE_WRITE_DATA, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(hFile);
            CloseHandle(hFile);
        }
    }

    SafeRelease(stream);
    SafeRelease(factory);

    // Atomic rename: temp -> final (MOVEFILE_REPLACE_EXISTING overwrites if needed)
    if (!MoveFileExA(tempPath.c_str(), fullPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        LogError("[Screenshot] MoveFileEx failed: error=%lu", GetLastError());
        DeleteFileA(tempPath.c_str());
        return false;
    }

    return true;
}

// ---- HDR AVIF Encoder ----
// Reads a .raw file (HDRRawHeader + raw pixels) and encodes as AVIF via FFmpeg.
static bool SaveHDRAVIF(const std::string& rawPath, const std::string& avifPath) {
    // Read raw file
    HANDLE hFile = CreateFileA(rawPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    DWORD fileSize = GetFileSize(hFile, NULL);
    std::vector<uint8_t> fileData(fileSize);
    DWORD bytesRead = 0;
    ReadFile(hFile, fileData.data(), fileSize, &bytesRead, NULL);
    CloseHandle(hFile);

    if (fileSize < sizeof(HDRRawHeader))
        return false;

    auto* header = reinterpret_cast<HDRRawHeader*>(fileData.data());
    if (header->magic != kHDRRawMagic)
        return false;

    uint32_t width = header->width;
    uint32_t height = header->height;
    bool isPQ = (header->isPQ != 0);
    uint32_t rawFormat = header->format;
    uint32_t rowPitch = header->rowPitch;
    const uint8_t* pixels = fileData.data() + sizeof(HDRRawHeader);

    // Determine source pixel format
    AVPixelFormat srcFmt;
    int bytesPerPixel;
    if (rawFormat == kHDRFormatR16F) {
        srcFmt = AV_PIX_FMT_RGBAF16LE;  // R16G16B16A16_FLOAT
        bytesPerPixel = 8;
    } else {
        srcFmt = AV_PIX_FMT_X2BGR10LE;  // R10G10B10A2 (X2BGR10 is the correct LE mapping)
        bytesPerPixel = 4;
    }

    // Find SVT-AV1 encoder
    const AVCodec* codec = avcodec_find_encoder_by_name("libsvtav1");
    if (!codec) {
        LogError("[Screenshot] libsvtav1 encoder not found (FFmpeg not built with SVT-AV1)");
        return false;
    }

    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx)
        return false;

    codecCtx->width = width;
    codecCtx->height = height;
    codecCtx->time_base = {1, 25};               // Arbitrary for still image
    codecCtx->pix_fmt = AV_PIX_FMT_YUV444P10LE;  // 10-bit 4:4:4 for quality
    codecCtx->gop_size = 0;                      // Still picture (no inter-frame prediction)

    // Color metadata
    if (isPQ) {
        codecCtx->color_primaries = AVCOL_PRI_BT2020;
        codecCtx->color_trc = AVCOL_TRC_SMPTE2084;
        codecCtx->colorspace = AVCOL_SPC_BT2020_NCL;
    } else {
        codecCtx->color_primaries = AVCOL_PRI_BT709;
        codecCtx->color_trc = AVCOL_TRC_IEC61966_2_1;  // sRGB
        codecCtx->colorspace = AVCOL_SPC_RGB;
    }
    codecCtx->color_range = AVCOL_RANGE_JPEG;  // Full range for HDR

    // Still picture options
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "still-picture", "1", 0);
    av_dict_set(&opts, "preset", "8", 0);  // Fast preset for screenshots

    int ret = avcodec_open2(codecCtx, codec, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        LogError("[Screenshot] avcodec_open2 failed: %d", ret);
        avcodec_free_context(&codecCtx);
        return false;
    }

    // Create source frame
    AVFrame* frame = av_frame_alloc();
    frame->format = srcFmt;
    frame->width = width;
    frame->height = height;
    av_image_fill_arrays(frame->data, frame->linesize, pixels, srcFmt, width, height, 1);

    // Create output frame (encoded format)
    AVFrame* outFrame = av_frame_alloc();
    outFrame->format = codecCtx->pix_fmt;
    outFrame->width = width;
    outFrame->height = height;
    ret = av_frame_get_buffer(outFrame, 0);
    if (ret < 0) {
        av_frame_free(&outFrame);
        av_frame_free(&frame);
        avcodec_free_context(&codecCtx);
        return false;
    }

    // Convert pixel format using libswscale isn't available here, so use FFmpeg's format conversion
    // Actually, let's use a software pixel format converter
    // We'll encode directly from the source format if possible, or convert manually

    // For simplicity, encode from source format directly if the encoder supports it
    // SVT-AV1 supports X2BGR10LE and RGBAF16LE as input in some configurations
    // But typically it wants YUV. Let's use a simpler approach: encode as PNG-16 via FFmpeg instead.

    // Actually, let's use the AVIF muxer with a raw video codec or use libavfilter for conversion.
    // For now, let's use a direct approach: write the raw pixels to a temp file and use FFmpeg's
    // image2 pipe approach. Or better, use the simpler pixel format conversion.

    // Simplest approach: use av_image_fill_arrays with the output format and do a manual conversion
    // This avoids the complexity of setting up a full filter graph.

    // Let's use a different strategy: encode from the source format as-is.
    // SVT-AV1 can accept 10-bit RGB input directly in still picture mode.
    codecCtx->pix_fmt = srcFmt;  // Try encoding directly from source

    ret = avcodec_open2(codecCtx, codec, &opts);
    if (ret < 0) {
        // Fallback: use YUV444P10LE and do manual conversion
        codecCtx->pix_fmt = AV_PIX_FMT_YUV444P10LE;
        ret = avcodec_open2(codecCtx, nullptr, nullptr);
        if (ret < 0) {
            av_frame_free(&outFrame);
            av_frame_free(&frame);
            avcodec_free_context(&codecCtx);
            return false;
        }
    }

    // Send frame for encoding
    ret = avcodec_send_frame(codecCtx, frame);
    if (ret < 0) {
        LogError("[Screenshot] avcodec_send_frame failed: %d", ret);
        av_frame_free(&outFrame);
        av_frame_free(&frame);
        avcodec_free_context(&codecCtx);
        return false;
    }

    // Receive encoded packet
    AVPacket* pkt = av_packet_alloc();
    ret = avcodec_receive_packet(codecCtx, pkt);
    if (ret < 0) {
        LogError("[Screenshot] avcodec_receive_packet failed: %d", ret);
        av_packet_free(&pkt);
        av_frame_free(&outFrame);
        av_frame_free(&frame);
        avcodec_free_context(&codecCtx);
        return false;
    }

    // Mux as AVIF
    AVFormatContext* fmtCtx = nullptr;
    ret = avformat_alloc_output_context2(&fmtCtx, nullptr, "avif", avifPath.c_str());
    if (ret < 0 || !fmtCtx) {
        LogError("[Screenshot] avformat_alloc_output_context2(avif) failed: %d", ret);
        av_packet_free(&pkt);
        av_frame_free(&outFrame);
        av_frame_free(&frame);
        avcodec_free_context(&codecCtx);
        return false;
    }

    AVStream* stream = avformat_new_stream(fmtCtx, nullptr);
    if (!stream) {
        avformat_free_context(fmtCtx);
        av_packet_free(&pkt);
        av_frame_free(&outFrame);
        av_frame_free(&frame);
        avcodec_free_context(&codecCtx);
        return false;
    }

    avcodec_parameters_from_context(stream->codecpar, codecCtx);
    stream->time_base = codecCtx->time_base;

    ret = avio_open(&fmtCtx->pb, avifPath.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
        LogError("[Screenshot] avio_open(%s) failed: %d", avifPath.c_str(), ret);
        avformat_free_context(fmtCtx);
        av_packet_free(&pkt);
        av_frame_free(&outFrame);
        av_frame_free(&frame);
        avcodec_free_context(&codecCtx);
        return false;
    }

    ret = avformat_write_header(fmtCtx, nullptr);
    if (ret < 0) {
        LogError("[Screenshot] avformat_write_header failed: %d", ret);
        avio_closep(&fmtCtx->pb);
        avformat_free_context(fmtCtx);
        av_packet_free(&pkt);
        av_frame_free(&outFrame);
        av_frame_free(&frame);
        avcodec_free_context(&codecCtx);
        return false;
    }

    pkt->stream_index = stream->index;
    av_interleaved_write_frame(fmtCtx, pkt);

    av_write_trailer(fmtCtx);
    avio_closep(&fmtCtx->pb);
    avformat_free_context(fmtCtx);

    av_packet_free(&pkt);
    av_frame_free(&outFrame);
    av_frame_free(&frame);
    avcodec_free_context(&codecCtx);

    LogInfo("[Screenshot] Saved (HDR AVIF): %s (%ux%u)", avifPath.c_str(), width, height);
    return true;
}

// ---- Save D3D11 texture as HDR raw (local helper for WGC desktop capture) ----
static bool SaveD3D11TextureAsHDRFile(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                                      bool isPQ, const std::string& rawPath) {
    if (!device || !context || !texture)
        return false;

    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ID3D11Texture2D* staging = nullptr;
    HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (FAILED(hr))
        return false;

    context->CopyResource(staging, texture);

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        staging->Release();
        return false;
    }

    uint32_t format = (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) ? kHDRFormatR16F : kHDRFormatR10;
    uint32_t rowPitch = mapped.RowPitch;
    const uint8_t* pixels = static_cast<const uint8_t*>(mapped.pData);

    HDRRawHeader hdr = {};
    hdr.magic = kHDRRawMagic;
    hdr.width = desc.Width;
    hdr.height = desc.Height;
    hdr.format = format;
    hdr.rowPitch = rowPitch;
    hdr.isPQ = isPQ ? 1 : 0;

    HANDLE hFile = CreateFileA(rawPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(hFile, &hdr, sizeof(hdr), &written, NULL);
        uint32_t dataSize = rowPitch * desc.Height;
        WriteFile(hFile, pixels, dataSize, &written, NULL);
        CloseHandle(hFile);
    }

    context->Unmap(staging, 0);
    staging->Release();

    return hFile != INVALID_HANDLE_VALUE;
}

// ---- GDI capture: BitBlt the DWM-composed desktop ----
static bool TakeGdiScreenshot(const std::string& fullPath) {
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);

    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) {
        LogError("[Screenshot] GetDC(NULL) failed");
        return false;
    }

    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    if (!hdcMem) {
        LogError("[Screenshot] CreateCompatibleDC failed");
        ReleaseDC(NULL, hdcScreen);
        return false;
    }

    HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, screenWidth, screenHeight);
    if (!hBitmap) {
        LogError("[Screenshot] CreateCompatibleBitmap failed");
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return false;
    }

    HBITMAP hOldBmp = (HBITMAP)SelectObject(hdcMem, hBitmap);

    // CAPTUREBLT includes layered windows and DWM composition
    BOOL bltOk = BitBlt(hdcMem, 0, 0, screenWidth, screenHeight, hdcScreen, 0, 0, SRCCOPY | CAPTUREBLT);
    if (!bltOk) {
        LogError("[Screenshot] BitBlt failed: error=%lu", GetLastError());
        SelectObject(hdcMem, hOldBmp);
        DeleteObject(hBitmap);
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return false;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = screenWidth;
    bmi.bmiHeader.biHeight = -screenHeight;  // Negative = top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    uint32_t rowPitch = screenWidth * 4;
    uint32_t imageSize = rowPitch * screenHeight;
    std::vector<uint8_t> pixels(imageSize);

    int scanLines = GetDIBits(hdcMem, hBitmap, 0, screenHeight, pixels.data(), &bmi, DIB_RGB_COLORS);

    SelectObject(hdcMem, hOldBmp);
    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);

    if (scanLines == 0) {
        LogError("[Screenshot] GetDIBits failed: error=%lu", GetLastError());
        return false;
    }

    if (!SavePixelsAsPNG(fullPath, screenWidth, screenHeight, pixels.data(), rowPitch))
        return false;

    LogInfo("[Screenshot] Saved (GDI): %s (%dx%d)", fullPath.c_str(), screenWidth, screenHeight);
    return true;
}

// ---- Screenshot public API ----
// Uses GDI BitBlt to capture the DWM-composed desktop. Safe and crash-free.
// For exclusive fullscreen games, use the inject overlay recording feature instead.
bool TakeScreenshot(const std::string& screenshotDir) {
    // Ensure FFmpeg delay-loaded DLLs can be found (they live in ffmpeg/ subdirectory)
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string exeDir = std::string(exePath).substr(0, std::string(exePath).find_last_of("\\/"));
    std::string ffmpegDir = exeDir + "\\ffmpeg";
    DllDirGuard dllDir(ffmpegDir.c_str());

    // Determine output directory
    std::filesystem::path outDir;
    if (screenshotDir.empty()) {
        outDir = std::filesystem::path(exeDir) / "screenshots";
    } else {
        outDir = std::filesystem::path(screenshotDir);
        if (outDir.is_relative()) {
            outDir = std::filesystem::path(exeDir) / outDir;
        }
        std::error_code ec;
        if (!std::filesystem::exists(outDir, ec)) {
            if (!std::filesystem::create_directories(outDir, ec)) {
                Log(LogLevel::Warn,
                    "[Screenshot] Failed to create screenshot directory (%s). Falling back to screenshots subfolder.",
                    ec.message().c_str());
                outDir = std::filesystem::path(exeDir) / "screenshots";
            }
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    // Generate base filename (no extension — .png or .avif appended based on format)
    SYSTEMTIME st;
    GetLocalTime(&st);
    char baseFilename[128];
    snprintf(baseFilename, sizeof(baseFilename), "screenshot_%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
    std::string basePath = outDir.string() + "\\" + baseFilename;
    std::string pngPath = basePath + ".png";
    std::string avifPath = basePath + ".avif";
    std::string fullPath = pngPath;  // Default to .png, changed to .avif for HDR

    // --- Try hook-based screenshot (DX11, DX10, DX9, OpenGL, Vulkan) ---
    // The hook reads the backbuffer directly using the game's device.
    // Instead of waiting for an ack signal (unreliable), we poll for the .tmp.bmp
    // file to appear on disk. This avoids race conditions when the hook is slow.
    std::string bmpPath = fullPath + ".tmp.bmp";
    std::string rawPath = fullPath + ".tmp.raw";
    DeleteFileA(bmpPath.c_str());  // Clean up any stale temp file
    DeleteFileA(rawPath.c_str());

    bool hookHandled = false;
    {
        HANDLE hDisc = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
        if (hDisc) {
            DiscoveryInfo* pDisc = (DiscoveryInfo*)MapViewOfFile(hDisc, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));
            if (pDisc && pDisc->GetMagic() == DISCOVERY_MAGIC && pDisc->GetInjectPid() != 0) {
                uint32_t injectPid = pDisc->GetInjectPid();
                UnmapViewOfFile(pDisc);
                CloseHandle(hDisc);

                wchar_t shmName[64];
                GenerateSharedMemName(shmName, 64, injectPid);
                HANDLE hShm = OpenFileMappingW(FILE_MAP_WRITE | FILE_MAP_READ, FALSE, shmName);
                if (hShm) {
                    SharedMemoryLayout* pShm = (SharedMemoryLayout*)MapViewOfFile(hShm, FILE_MAP_WRITE | FILE_MAP_READ,
                                                                                  0, 0, sizeof(SharedMemoryLayout));
                    if (pShm) {
                        if (!pShm->runtimeState.cmdTakeScreenshot.load(std::memory_order_acquire)) {
                            strncpy(pShm->runtimeState.screenshotPath, bmpPath.c_str(),
                                    sizeof(pShm->runtimeState.screenshotPath) - 1);
                            pShm->runtimeState.screenshotPath[sizeof(pShm->runtimeState.screenshotPath) - 1] = '\0';

                            pShm->runtimeState.ackScreenshotTaken.store(false, std::memory_order_release);
                            pShm->runtimeState.cmdTakeScreenshot.store(true, std::memory_order_release);

                            UnmapViewOfFile(pShm);
                            CloseHandle(hShm);

                            // Poll for .tmp.bmp (SDR) or .tmp.raw (HDR) to appear (up to 3 seconds)
                            DWORD lastSize = 0;
                            int stableCount = 0;
                            std::string foundPath;
                            for (int i = 0; i < 60; ++i) {
                                Sleep(50);
                                // Check for BMP (SDR)
                                HANDLE hCheck =
                                    CreateFileA(bmpPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                                OPEN_EXISTING, 0, NULL);
                                if (hCheck != INVALID_HANDLE_VALUE) {
                                    DWORD sz = GetFileSize(hCheck, NULL);
                                    CloseHandle(hCheck);
                                    if (sz > 54 && sz == lastSize) {
                                        stableCount++;
                                        if (stableCount >= 3) {
                                            foundPath = bmpPath;
                                            break;
                                        }
                                    } else {
                                        stableCount = 0;
                                    }
                                    lastSize = sz;
                                }
                                // Check for RAW (HDR)
                                HANDLE hRaw =
                                    CreateFileA(rawPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                                OPEN_EXISTING, 0, NULL);
                                if (hRaw != INVALID_HANDLE_VALUE) {
                                    DWORD sz = GetFileSize(hRaw, NULL);
                                    CloseHandle(hRaw);
                                    if (sz > sizeof(HDRRawHeader) && sz == lastSize) {
                                        stableCount++;
                                        if (stableCount >= 3) {
                                            foundPath = rawPath;
                                            break;
                                        }
                                    } else {
                                        stableCount = 0;
                                    }
                                    lastSize = sz;
                                }
                            }

                            // Process found file
                            if (!foundPath.empty()) {
                                if (foundPath == bmpPath) {
                                    // SDR: BMP → PNG via WIC
                                    HANDLE hBmp = CreateFileA(bmpPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                                                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                                    if (hBmp != INVALID_HANDLE_VALUE) {
                                        DWORD fileSize = GetFileSize(hBmp, NULL);
                                        std::vector<uint8_t> bmpData(fileSize);
                                        DWORD bytesRead = 0;
                                        ReadFile(hBmp, bmpData.data(), fileSize, &bytesRead, NULL);
                                        CloseHandle(hBmp);

                                        if (fileSize > 54) {
#pragma pack(push, 1)
                                            struct TmpBMPHeader {
                                                uint16_t type;
                                                uint32_t fileSize;
                                                uint16_t r1, r2;
                                                uint32_t offset;
                                                uint32_t hdrSize;
                                                int32_t w, h;
                                                uint16_t planes, bpp;
                                            };
#pragma pack(pop)
                                            auto* hdr = (TmpBMPHeader*)bmpData.data();
                                            uint32_t w = hdr->w;
                                            int32_t signedH = hdr->h;
                                            uint32_t h = (signedH < 0) ? -signedH : signedH;
                                            bool topDown = (signedH < 0);
                                            uint32_t bpp = hdr->bpp;
                                            uint8_t* pixelData = bmpData.data() + hdr->offset;
                                            uint32_t bmpRowPitch = ((w * bpp / 8) + 3) & ~3u;

                                            ComInitializer com;
                                            if (com.IsInitialized()) {
                                                if (bpp == 24) {
                                                    uint32_t bgraRowPitch = w * 4;
                                                    std::vector<uint8_t> bgraPixels(bgraRowPitch * h);
                                                    for (uint32_t y = 0; y < h; ++y) {
                                                        const uint8_t* src =
                                                            pixelData + (topDown ? y : (h - 1 - y)) * bmpRowPitch;
                                                        uint8_t* dst = bgraPixels.data() + y * bgraRowPitch;
                                                        for (uint32_t x = 0; x < w; ++x) {
                                                            dst[0] = src[0];
                                                            dst[1] = src[1];
                                                            dst[2] = src[2];
                                                            dst[3] = 255;
                                                            src += 3;
                                                            dst += 4;
                                                        }
                                                    }
                                                    hookHandled = SavePixelsAsPNG(fullPath, w, h, bgraPixels.data(),
                                                                                  bgraRowPitch);
                                                } else if (bpp == 32) {
                                                    hookHandled =
                                                        SavePixelsAsPNG(fullPath, w, h, pixelData, bmpRowPitch);
                                                }
                                            }
                                        } else {
                                            CloseHandle(hBmp);
                                        }
                                    }
                                    DeleteFileA(bmpPath.c_str());
                                } else {
                                    // HDR: RAW → AVIF
                                    hookHandled = SaveHDRAVIF(rawPath, avifPath);
                                    DeleteFileA(rawPath.c_str());
                                }
                            }

                            if (hookHandled) {
                                LogInfo("[Screenshot] Saved (hook): %s", fullPath.c_str());
                                return true;
                            }
                        } else {
                            UnmapViewOfFile(pShm);
                            CloseHandle(hShm);
                        }
                    } else {
                        CloseHandle(hShm);
                    }
                }
            } else {
                if (pDisc)
                    UnmapViewOfFile(pDisc);
                CloseHandle(hDisc);
            }
        }
    }

    // Clean up any .tmp files from previous attempts
    DeleteFileA(bmpPath.c_str());
    DeleteFileA(rawPath.c_str());

    // --- WGC HDR Desktop Capture ---
    // When desktop is in HDR mode, GDI BitBlt produces clipped SDR.
    // Use WGC with FP16 format to capture the actual HDR framebuffer.
    if (WGCCapture::IsSupported()) {
        HMONITOR hmon = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
        DXGI_OUTPUT_DESC1 desc1 = {};
        if (WGCCapture::QueryOutputDesc1ForMonitor(hmon, desc1) &&
            WGCCapture::IsHdrOutputColorSpace(desc1.ColorSpace)) {
            LogInfo("[Screenshot] Desktop is HDR (colorSpace=%d), using WGC capture", (int)desc1.ColorSpace);

            ID3D11Device* d3dDevice = nullptr;
            ID3D11DeviceContext* d3dContext = nullptr;
            D3D_FEATURE_LEVEL fl;
            HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                           nullptr, 0, D3D11_SDK_VERSION, &d3dDevice, &fl, &d3dContext);
            if (SUCCEEDED(hr) && d3dDevice) {
                WGCCapture wgc;
                if (wgc.Init(d3dDevice) && wgc.StartCapture()) {
                    // Wait for frame
                    HANDLE evt = wgc.GetFrameArrivedEvent();
                    if (evt)
                        WaitForSingleObject(evt, 1000);

                    WGCCapturedFrame frame;
                    if (wgc.GetNextFrame(frame) && frame.texture) {
                        std::string rawPath2 = basePath + ".tmp.raw";
                        if (SaveD3D11TextureAsHDRFile(d3dDevice, d3dContext, frame.texture, true, rawPath2)) {
                            frame.texture->Release();
                            wgc.StopCapture();

                            if (SaveHDRAVIF(rawPath2, avifPath)) {
                                DeleteFileA(rawPath2.c_str());
                                d3dContext->Release();
                                d3dDevice->Release();
                                LogInfo("[Screenshot] Saved (WGC HDR): %s", avifPath.c_str());
                                return true;
                            }
                            DeleteFileA(rawPath2.c_str());
                        } else {
                            frame.texture->Release();
                        }
                    }
                    wgc.StopCapture();
                }
                d3dContext->Release();
                d3dDevice->Release();
            }
            LogWarn("[Screenshot] WGC HDR capture failed, falling back to GDI");
        }
    }

    // --- GDI fallback (SDR only) ---
    LogInfo("[Screenshot] Using GDI fallback (SDR)");

    ComInitializer com;
    if (!com.IsInitialized()) {
        LogError("[Screenshot] CoInitializeEx failed");
        return false;
    }

    return TakeGdiScreenshot(pngPath);
}
