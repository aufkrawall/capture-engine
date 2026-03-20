// Screenshot capture with two approaches:
//   1. Hook-based: sets cmdTakeScreenshot in shared memory, hook reads backbuffer directly (DX11, DX10, DX9, OpenGL)
//   2. GDI fallback: captures DWM-composed desktop via BitBlt (works for windowed/borderless, may fail for exclusive
//   fullscreen)

#include "screenshot.h"
#include "../common/logging.h"
#include "../common/shared_defs.h"

// clang-format off
#include <windows.h>
#include <wincodec.h>
#include <wincodecsdk.h>
// clang-format on

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
    // Determine output directory
    std::string outDir = screenshotDir;
    if (outDir.empty()) {
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        std::string baseDir = std::string(exePath).substr(0, std::string(exePath).find_last_of("\\/"));
        outDir = baseDir + "\\screenshots";
    }
    std::filesystem::create_directories(outDir);

    // Generate filename (.png always)
    SYSTEMTIME st;
    GetLocalTime(&st);
    char filename[128];
    snprintf(filename, sizeof(filename), "screenshot_%04d%02d%02d_%02d%02d%02d.png", st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
    std::string fullPath = outDir + "\\" + filename;

    // --- Try hook-based screenshot (DX11, DX10, DX9, OpenGL, Vulkan) ---
    // The hook reads the backbuffer directly using the game's device.
    // Instead of waiting for an ack signal (unreliable), we poll for the .tmp.bmp
    // file to appear on disk. This avoids race conditions when the hook is slow.
    std::string bmpPath = fullPath + ".tmp.bmp";
    DeleteFileA(bmpPath.c_str());  // Clean up any stale temp file

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

                            // Poll for .tmp.bmp to appear on disk (up to 3 seconds)
                            DWORD lastSize = 0;
                            int stableCount = 0;
                            for (int i = 0; i < 60; ++i) {
                                Sleep(50);
                                HANDLE hCheck =
                                    CreateFileA(bmpPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                                OPEN_EXISTING, 0, NULL);
                                if (hCheck != INVALID_HANDLE_VALUE) {
                                    DWORD sz = GetFileSize(hCheck, NULL);
                                    CloseHandle(hCheck);
                                    if (sz > 54 && sz == lastSize) {
                                        stableCount++;
                                        if (stableCount >= 3) {
                                            // File is stable (hook finished writing)
                                            break;
                                        }
                                    } else {
                                        stableCount = 0;
                                    }
                                    lastSize = sz;
                                }
                            }

                            // Check if we got a stable BMP file
                            HANDLE hBmp = CreateFileA(bmpPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                            if (hBmp != INVALID_HANDLE_VALUE) {
                                DWORD fileSize = GetFileSize(hBmp, NULL);
                                if (fileSize > 54) {
                                    std::vector<uint8_t> bmpData(fileSize);
                                    DWORD bytesRead = 0;
                                    ReadFile(hBmp, bmpData.data(), fileSize, &bytesRead, NULL);
                                    CloseHandle(hBmp);

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
                                    if (fileSize > 54 && hdr->w > 0 && hdr->h != 0) {
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
                                                hookHandled =
                                                    SavePixelsAsPNG(fullPath, w, h, bgraPixels.data(), bgraRowPitch);
                                            } else if (bpp == 32) {
                                                hookHandled = SavePixelsAsPNG(fullPath, w, h, pixelData, bmpRowPitch);
                                            }
                                        }
                                    }
                                } else {
                                    CloseHandle(hBmp);
                                }
                            }

                            DeleteFileA(bmpPath.c_str());

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

    // Clean up any .tmp.bmp the hook may have created
    DeleteFileA(bmpPath.c_str());

    // --- GDI fallback ---
    LogInfo("[Screenshot] Hook screenshot not available, using GDI fallback");

    ComInitializer com;
    if (!com.IsInitialized()) {
        LogError("[Screenshot] CoInitializeEx failed");
        return false;
    }

    return TakeGdiScreenshot(fullPath);
}
