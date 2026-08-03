#include "screenshot.h"

#include "../common/logging.h"
#include "../common/raii_helpers.h"
#include "../common/reserved_capture_output.h"
#include "../common/secure_dll_loading.h"
#include "../common/shared_defs.h"
#include "screenshot_encoding.h"
#include "wgc_capture.h"

// clang-format off
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
// clang-format on

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace {

using ce::screenshot::kMaximumScreenshotDimension;
using ce::screenshot::MakeRawScreenshot;
using ce::screenshot::RawScreenshot;
using ce::screenshot::ReadRawScreenshot;
using ce::screenshot::SaveRawScreenshot;
using ce::screenshot::ScreenshotOutputColorSpace;

constexpr DWORD kHookScreenshotTimeoutMs = 15000;

std::atomic<uint64_t> g_screenshotRequestSequence{1};
std::mutex g_screenshotMutex;

template <typename T>
void SafeRelease(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

class HandleGuard {
public:
    HandleGuard() = default;
    explicit HandleGuard(HANDLE handle) : handle_(handle) {}
    ~HandleGuard() {
        if (handle_ && handle_ != INVALID_HANDLE_VALUE)
            CloseHandle(handle_);
    }
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    HANDLE Get() const {
        return handle_;
    }

private:
    HANDLE handle_ = nullptr;
};

class MappedViewGuard {
public:
    explicit MappedViewGuard(void* view) : view_(view) {}
    ~MappedViewGuard() {
        if (view_)
            UnmapViewOfFile(view_);
    }

private:
    void* view_ = nullptr;
};

class ComInitializer {
public:
    ComInitializer() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComInitializer() {
        if (SUCCEEDED(result_))
            CoUninitialize();
    }
    bool IsUsable() const {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

private:
    HRESULT result_ = E_FAIL;
};

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty())
        return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};
    std::string result(static_cast<size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(),
                            length, nullptr, nullptr) != length) {
        return {};
    }
    return result;
}

bool CaptureD3D11Texture(ID3D11Texture2D* texture, bool isHdrPresentation, RawScreenshot& screenshot) {
    if (!texture)
        return false;

    // Split-device WGC publishes each completed frame by releasing keyed
    // mutex key 1. Reading the consumer-side shared texture without acquiring
    // that key races the GPU handoff and commonly returns its initial black
    // contents even though CopyResource and Map both succeed.
    struct KeyedMutexReadGuard {
        IDXGIKeyedMutex* mutex = nullptr;
        bool acquired = false;

        ~KeyedMutexReadGuard() {
            if (!mutex)
                return;
            if (acquired) {
                const HRESULT releaseResult = mutex->ReleaseSync(0);
                if (FAILED(releaseResult))
                    LogError("[Screenshot] WGC keyed mutex ReleaseSync(0) failed: 0x%08X",
                             static_cast<unsigned>(releaseResult));
            }
            mutex->Release();
        }
    } keyedMutex;
    const HRESULT mutexQueryResult = texture->QueryInterface(IID_PPV_ARGS(&keyedMutex.mutex));
    if (FAILED(mutexQueryResult) && mutexQueryResult != E_NOINTERFACE) {
        LogError("[Screenshot] WGC keyed mutex query failed: 0x%08X",
                 static_cast<unsigned>(mutexQueryResult));
        return false;
    }
    if (keyedMutex.mutex) {
        const HRESULT acquireResult = keyedMutex.mutex->AcquireSync(1, 1000);
        if (acquireResult != S_OK) {
            LogError("[Screenshot] WGC keyed mutex AcquireSync(1) failed: 0x%08X",
                     static_cast<unsigned>(acquireResult));
            return false;
        }
        keyedMutex.acquired = true;
    }

    // WGCCapture may own a dedicated D3D11 device. A texture from that path
    // cannot be copied with the bootstrap device passed to WGCCapture::Init;
    // cross-device CopyResource is invalid and can silently leave a zero-filled
    // staging texture. Always read on the texture-owning device/context.
    ce::ComGuard<ID3D11Device> textureDevice;
    ce::ComGuard<ID3D11DeviceContext> textureContext;
    texture->GetDevice(textureDevice.addressof());
    if (!textureDevice)
        return false;
    textureDevice->GetImmediateContext(textureContext.addressof());
    if (!textureContext)
        return false;

    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    ScreenshotPixelFormat format{};
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    ScreenshotColorEncoding encoding{};
    switch (description.Format) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            format = ScreenshotPixelFormat::BGRA8;
            encoding = ScreenshotColorEncoding::SRGB;
            break;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            format = ScreenshotPixelFormat::RGBA8;
            encoding = ScreenshotColorEncoding::SRGB;
            break;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            format = ScreenshotPixelFormat::R10G10B10A2;
            encoding = isHdrPresentation ? ScreenshotColorEncoding::BT2020_PQ
                                         : ScreenshotColorEncoding::BT709_G22;
            break;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            format = ScreenshotPixelFormat::RGBA16F;
            encoding = isHdrPresentation ? ScreenshotColorEncoding::LinearScRGB
                                         : ScreenshotColorEncoding::LinearScRGBSdr;
            break;
        default:
            return false;
    }

    D3D11_TEXTURE2D_DESC stagingDescription = description;
    stagingDescription.Usage = D3D11_USAGE_STAGING;
    stagingDescription.BindFlags = 0;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDescription.MiscFlags = 0;
    ce::ComGuard<ID3D11Texture2D> staging;
    if (FAILED(textureDevice->CreateTexture2D(&stagingDescription, nullptr, staging.addressof())))
        return false;
    textureContext->CopyResource(staging.get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT mapResult = textureContext->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped);
    bool copied = false;
    if (SUCCEEDED(mapResult)) {
        copied = MakeRawScreenshot(static_cast<const uint8_t*>(mapped.pData), description.Width, description.Height,
                                   mapped.RowPitch, format, encoding, screenshot);
        textureContext->Unmap(staging.get(), 0);
    }
    LogInfo("[Screenshot] Texture-owned D3D11 readback: %ux%u format=%u hdr=%s keyed_mutex=%s result=%s",
            description.Width, description.Height, static_cast<unsigned>(description.Format),
            isHdrPresentation ? "yes" : "no", keyedMutex.mutex ? "acquired" : "none",
            copied ? "ok" : "failed");
    return copied;
}

std::filesystem::path ReadyPathForPart(const std::filesystem::path& partPath) {
    std::wstring ready = partPath.wstring();
    if (ready.size() <= 5 || ready.compare(ready.size() - 5, 5, L".part") != 0)
        return {};
    ready.resize(ready.size() - 5);
    ready += L".ready";
    return ready;
}

void CleanupHookPayload(const std::filesystem::path& partPath) {
    DeleteFileW(partPath.c_str());
    const std::filesystem::path readyPath = ReadyPathForPart(partPath);
    if (!readyPath.empty())
        DeleteFileW(readyPath.c_str());
}

bool TryHookScreenshot(const std::filesystem::path& outputDirectory, RawScreenshot& screenshot) {
    HandleGuard discovery(OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY));
    if (!discovery.Get())
        return false;
    auto* discoveryInfo =
        static_cast<DiscoveryInfo*>(MapViewOfFile(discovery.Get(), FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo)));
    if (!discoveryInfo)
        return false;
    MappedViewGuard discoveryView(discoveryInfo);
    if (!ValidateDiscoveryInfo(discoveryInfo) || discoveryInfo->GetInjectPid() == 0)
        return false;

    wchar_t sharedMemoryName[64]{};
    GenerateSharedMemName(sharedMemoryName, std::size(sharedMemoryName), discoveryInfo->GetInjectPid());
    HandleGuard mapping(OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, sharedMemoryName));
    if (!mapping.Get())
        return false;
    auto* sharedMemory = static_cast<SharedMemoryLayout*>(
        MapViewOfFile(mapping.Get(), FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(SharedMemoryLayout)));
    if (!sharedMemory)
        return false;
    MappedViewGuard sharedMemoryView(sharedMemory);
    if (!ValidateSharedMemory(sharedMemory) || sharedMemory->GetVersion() != SHARED_MEMORY_VERSION)
        return false;
    const uint32_t sourcePid = sharedMemory->GetSourcePid();
    if (sourcePid == 0) {
        LogInfo("[Screenshot] No active injected source; selecting desktop capture without a hook wait");
        return false;
    }

    const auto currentStatus = static_cast<ScreenshotRequestStatus>(
        sharedMemory->runtimeState.screenshotStatus.load(std::memory_order_acquire));
    if (currentStatus == ScreenshotRequestStatus::Pending || currentStatus == ScreenshotRequestStatus::Writing)
        return false;

    const uint32_t processId = GetCurrentProcessId();
    for (uint32_t attempt = 0; attempt < 16; ++attempt) {
        const uint64_t requestId = g_screenshotRequestSequence.fetch_add(1, std::memory_order_relaxed);
        wchar_t rawName[160]{};
        _snwprintf_s(rawName, std::size(rawName), _TRUNCATE, L"._ce_screenshot_p%08X_r%016llX.raw.part", processId,
                     static_cast<unsigned long long>(requestId));
        const std::filesystem::path partPath = outputDirectory / rawName;
        if (GetFileAttributesW(partPath.c_str()) != INVALID_FILE_ATTRIBUTES)
            continue;
        const std::string partPathUtf8 = WideToUtf8(partPath.wstring());
        if (partPathUtf8.empty() || partPathUtf8.size() >= sizeof(sharedMemory->runtimeState.screenshotPath))
            return false;

        char eventName[128]{};
        snprintf(eventName, sizeof(eventName), "Local\\CE_Screenshot_%08X_%016llX", processId,
                 static_cast<unsigned long long>(requestId));
        HandleGuard completionEvent(CreateEventA(nullptr, FALSE, FALSE, eventName));
        if (!completionEvent.Get() || GetLastError() == ERROR_ALREADY_EXISTS)
            continue;

        sharedMemory->runtimeState.screenshotRequestId.store(0, std::memory_order_release);
        memset(sharedMemory->runtimeState.screenshotPath, 0, sizeof(sharedMemory->runtimeState.screenshotPath));
        memcpy(sharedMemory->runtimeState.screenshotPath, partPathUtf8.data(), partPathUtf8.size());
        memset(sharedMemory->runtimeState.screenshotCompletionEventName, 0,
               sizeof(sharedMemory->runtimeState.screenshotCompletionEventName));
        memcpy(sharedMemory->runtimeState.screenshotCompletionEventName, eventName, strlen(eventName));
        sharedMemory->runtimeState.screenshotError.store(ERROR_SUCCESS, std::memory_order_relaxed);
        sharedMemory->runtimeState.screenshotPayloadKind.store(static_cast<uint32_t>(ScreenshotPayloadKind::None),
                                                               std::memory_order_relaxed);
        sharedMemory->runtimeState.screenshotStatus.store(static_cast<uint32_t>(ScreenshotRequestStatus::Pending),
                                                          std::memory_order_release);
        sharedMemory->runtimeState.screenshotRequestId.store(requestId, std::memory_order_release);

        const DWORD waitResult = WaitForSingleObject(completionEvent.Get(), kHookScreenshotTimeoutMs);
        const bool completed =
            waitResult == WAIT_OBJECT_0 &&
            sharedMemory->runtimeState.screenshotCompletedRequestId.load(std::memory_order_acquire) == requestId;
        const auto status = static_cast<ScreenshotRequestStatus>(
            sharedMemory->runtimeState.screenshotStatus.load(std::memory_order_acquire));
        const auto payloadKind = static_cast<ScreenshotPayloadKind>(
            sharedMemory->runtimeState.screenshotPayloadKind.load(std::memory_order_acquire));
        const uint32_t error = sharedMemory->runtimeState.screenshotError.load(std::memory_order_acquire);
        if (sharedMemory->runtimeState.screenshotRequestId.load(std::memory_order_acquire) == requestId)
            sharedMemory->runtimeState.screenshotRequestId.store(0, std::memory_order_release);

        if (!completed || status != ScreenshotRequestStatus::Succeeded || payloadKind != ScreenshotPayloadKind::RawV2) {
            LogWarn("[Screenshot] Hook request %llu failed (wait=%lu status=%u error=%u)",
                    static_cast<unsigned long long>(requestId), waitResult, static_cast<unsigned>(status), error);
            CleanupHookPayload(partPath);
            return false;
        }

        const std::filesystem::path readyPath = ReadyPathForPart(partPath);
        if (!readyPath.empty() && ReadRawScreenshot(readyPath, requestId, screenshot)) {
            DeleteFileW(readyPath.c_str());
            return true;
        }
        LogError("[Screenshot] Hook published a truncated or invalid raw payload");
        CleanupHookPayload(partPath);
        return false;
    }
    return false;
}

bool IsHdrDesktop() {
    HMONITOR monitor = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    DXGI_OUTPUT_DESC1 description{};
    return WGCCapture::QueryOutputDesc1ForMonitor(monitor, description) &&
           WGCCapture::IsHdrOutputColorSpace(description.ColorSpace);
}

float QueryPrimarySdrWhiteNits() {
    constexpr float kFallbackNits = 203.0f;
    HMONITOR monitor = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo))
        return kFallbackNits;

    for (int attempt = 0; attempt < 3; ++attempt) {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        LONG result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
        if (result != ERROR_SUCCESS || pathCount == 0)
            break;
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        if (result == ERROR_INSUFFICIENT_BUFFER)
            continue;
        if (result != ERROR_SUCCESS)
            break;
        paths.resize(pathCount);
        for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
            // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName{};
            sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            sourceName.header.size = sizeof(sourceName);
            sourceName.header.adapterId = path.sourceInfo.adapterId;
            sourceName.header.id = path.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS ||
                lstrcmpiW(sourceName.viewGdiDeviceName, monitorInfo.szDevice) != 0) {
                continue;
            }
            // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
            DISPLAYCONFIG_SDR_WHITE_LEVEL whiteLevel{};
            whiteLevel.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
            whiteLevel.header.size = sizeof(whiteLevel);
            whiteLevel.header.adapterId = path.targetInfo.adapterId;
            whiteLevel.header.id = path.targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&whiteLevel.header) == ERROR_SUCCESS && whiteLevel.SDRWhiteLevel != 0) {
                const float nits = std::clamp(static_cast<float>(whiteLevel.SDRWhiteLevel) * (80.0f / 1000.0f),
                                              80.0f, 1000.0f);
                LogInfo("[Screenshot] Windows SDR white level: raw=%lu nits=%.1f",
                        static_cast<unsigned long>(whiteLevel.SDRWhiteLevel), nits);
                return nits;
            }
            break;
        }
        break;
    }
    LogWarn("[Screenshot] Windows SDR white-level query unavailable; using %.1f-nit fallback", kFallbackNits);
    return kFallbackNits;
}

bool TryWgcScreenshot(RawScreenshot& screenshot) {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3D_FEATURE_LEVEL featureLevel{};
    const HRESULT createResult =
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                          D3D11_SDK_VERSION, &device, &featureLevel, &context);
    if (FAILED(createResult) || !device || !context) {
        SafeRelease(context);
        SafeRelease(device);
        return false;
    }

    bool captured = false;
    WGCCapture wgc;
    if (wgc.Init(device) && wgc.StartCapture()) {
        HANDLE event = wgc.GetFrameArrivedEvent();
        if (event && WaitForSingleObject(event, 2000) == WAIT_OBJECT_0) {
            WGCCapturedFrame frame;
            if (wgc.GetNextFrame(frame) && frame.texture) {
                captured = CaptureD3D11Texture(frame.texture, frame.isHDR, screenshot);
                frame.texture->Release();
            }
        }
        wgc.StopCapture();
    }
    context->Release();
    device->Release();
    return captured;
}

bool TakeGdiScreenshot(RawScreenshot& screenshot) {
    const int width = GetSystemMetrics(SM_CXSCREEN);
    const int height = GetSystemMetrics(SM_CYSCREEN);
    if (width <= 0 || height <= 0 || width > static_cast<int>(kMaximumScreenshotDimension) ||
        height > static_cast<int>(kMaximumScreenshotDimension)) {
        return false;
    }

    HDC screen = GetDC(nullptr);
    HDC memory = screen ? CreateCompatibleDC(screen) : nullptr;
    HBITMAP bitmap = memory ? CreateCompatibleBitmap(screen, width, height) : nullptr;
    if (!screen || !memory || !bitmap) {
        if (bitmap)
            DeleteObject(bitmap);
        if (memory)
            DeleteDC(memory);
        if (screen)
            ReleaseDC(nullptr, screen);
        return false;
    }
    HGDIOBJ previous = SelectObject(memory, bitmap);
    const BOOL copied = BitBlt(memory, 0, 0, width, height, screen, 0, 0, SRCCOPY | CAPTUREBLT);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const uint32_t rowPitch = static_cast<uint32_t>(width) * 4;
    std::vector<uint8_t> pixels(static_cast<size_t>(rowPitch) * height);
    const int rows = copied ? GetDIBits(memory, bitmap, 0, height, pixels.data(), &info, DIB_RGB_COLORS) : 0;

    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return rows == height &&
           MakeRawScreenshot(pixels.data(), static_cast<uint32_t>(width), static_cast<uint32_t>(height), rowPitch,
                             ScreenshotPixelFormat::BGRA8, ScreenshotColorEncoding::SRGB, screenshot);
}

}  // namespace

bool TakeScreenshot(const std::string& screenshotDirectory, const std::string& colorSpace) {
    std::lock_guard<std::mutex> lock(g_screenshotMutex);
    const std::filesystem::path executableDirectory = ce::capture_output::GetExecutableDirectory();
    DWORD dllSearchError = ERROR_SUCCESS;
    if (!ce::security::EnsureSecureDllSearchDirectory(executableDirectory / L"ffmpeg", &dllSearchError)) {
        LogError("[Screenshot] Failed to register secure FFmpeg directory: error %lu", dllSearchError);
        return false;
    }
    const std::filesystem::path outputDirectory =
        ce::capture_output::ResolveCaptureDirectory(screenshotDirectory, executableDirectory);

    ComInitializer com;
    if (!com.IsUsable()) {
        LogError("[Screenshot] COM initialization failed");
        return false;
    }

    RawScreenshot screenshot;
    std::filesystem::path publishedPath;
    const auto outputColorSpace = colorSpace == "bt709" ? ScreenshotOutputColorSpace::Bt709
                                                         : ScreenshotOutputColorSpace::PreserveSource;
    const float sdrWhiteNits =
        outputColorSpace == ScreenshotOutputColorSpace::Bt709 ? QueryPrimarySdrWhiteNits() : 203.0f;
    LogInfo("[Screenshot] Output color policy: requested=%s resolved=%s", colorSpace.c_str(),
            outputColorSpace == ScreenshotOutputColorSpace::Bt709 ? "BT.709 SDR PNG" : "preserve source");
    if (TryHookScreenshot(outputDirectory, screenshot)) {
        if (SaveRawScreenshot(outputDirectory, screenshot, publishedPath, outputColorSpace, sdrWhiteNits)) {
            LogInfo("[Screenshot] Saved (hook): %s", WideToUtf8(publishedPath.wstring()).c_str());
            return true;
        }
        LogError("[Screenshot] Hook capture encoding failed; no partial output was published");
        return false;
    }

    if (IsHdrDesktop()) {
        LogInfo("[Screenshot] HDR desktop detected; using WGC readback");
        if (TryWgcScreenshot(screenshot) &&
            SaveRawScreenshot(outputDirectory, screenshot, publishedPath, outputColorSpace, sdrWhiteNits)) {
            LogInfo("[Screenshot] Saved (WGC %s): %s",
                    outputColorSpace == ScreenshotOutputColorSpace::Bt709 ? "HDR->SDR" : "HDR",
                    WideToUtf8(publishedPath.wstring()).c_str());
            return true;
        }
        LogError("[Screenshot] HDR WGC capture failed; refusing to publish a clipped SDR fallback");
        return false;
    }

    if (!TakeGdiScreenshot(screenshot) ||
        !SaveRawScreenshot(outputDirectory, screenshot, publishedPath, outputColorSpace, sdrWhiteNits)) {
        LogError("[Screenshot] GDI capture failed; no partial output was published");
        return false;
    }
    LogInfo("[Screenshot] Saved (GDI): %s", WideToUtf8(publishedPath.wstring()).c_str());
    return true;
}
