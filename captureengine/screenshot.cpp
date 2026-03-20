// Screenshot capture using DXGI Desktop Duplication + WIC PNG encoding.
// Captures the composed desktop output which includes fullscreen and windowed games.

#include "screenshot.h"
#include "../common/logging.h"

// clang-format off
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <wincodecsdk.h>
// clang-format on

#include <filesystem>
#include <string>

// Com helper to release COM objects
template <typename T>
static void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

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

    // Generate filename: screenshot_YYYYMMDD_HHMMSS.png
    SYSTEMTIME st;
    GetLocalTime(&st);
    char filename[128];
    snprintf(filename, sizeof(filename), "screenshot_%04d%02d%02d_%02d%02d%02d.png", st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
    std::string fullPath = outDir + "\\" + filename;

    // --- Create D3D11 device ---
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   nullptr, 0, D3D11_SDK_VERSION, &device, &featureLevel, &context);
    if (FAILED(hr)) {
        LogError("[Screenshot] D3D11CreateDevice failed: hr=0x%08X", hr);
        return false;
    }

    // --- Get DXGI adapter and output ---
    IDXGIDevice* dxgiDevice = nullptr;
    hr = device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    if (FAILED(hr)) {
        LogError("[Screenshot] QueryInterface IDXGIDevice failed: hr=0x%08X", hr);
        SafeRelease(context);
        SafeRelease(device);
        return false;
    }

    IDXGIAdapter* adapter = nullptr;
    hr = dxgiDevice->GetAdapter(&adapter);
    SafeRelease(dxgiDevice);
    if (FAILED(hr)) {
        LogError("[Screenshot] GetAdapter failed: hr=0x%08X", hr);
        SafeRelease(context);
        SafeRelease(device);
        return false;
    }

    // Get primary output (output 0)
    IDXGIOutput* output = nullptr;
    hr = adapter->EnumOutputs(0, &output);
    SafeRelease(adapter);
    if (FAILED(hr)) {
        LogError("[Screenshot] EnumOutputs(0) failed: hr=0x%08X", hr);
        SafeRelease(context);
        SafeRelease(device);
        return false;
    }

    IDXGIOutput1* output1 = nullptr;
    hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
    SafeRelease(output);
    if (FAILED(hr)) {
        LogError("[Screenshot] QueryInterface IDXGIOutput1 failed: hr=0x%08X", hr);
        SafeRelease(context);
        SafeRelease(device);
        return false;
    }

    // --- Duplicate output ---
    IDXGIOutputDuplication* duplication = nullptr;
    hr = output1->DuplicateOutput(device, &duplication);
    SafeRelease(output1);
    if (FAILED(hr)) {
        LogError("[Screenshot] DuplicateOutput failed: hr=0x%08X", hr);
        SafeRelease(context);
        SafeRelease(device);
        return false;
    }

    DXGI_OUTDUPL_DESC dupDesc;
    duplication->GetDesc(&dupDesc);

    // --- Acquire one frame ---
    IDXGIResource* desktopResource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;

    // Try to acquire a frame (wait up to 500ms)
    hr = duplication->AcquireNextFrame(500, &frameInfo, &desktopResource);
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            LogError("[Screenshot] AcquireNextFrame timed out (no desktop changes)");
        } else {
            LogError("[Screenshot] AcquireNextFrame failed: hr=0x%08X", hr);
        }
        SafeRelease(duplication);
        SafeRelease(context);
        SafeRelease(device);
        return false;
    }

    // Get desktop texture
    ID3D11Texture2D* desktopTexture = nullptr;
    hr = desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&desktopTexture);
    SafeRelease(desktopResource);
    if (FAILED(hr)) {
        LogError("[Screenshot] QueryInterface ID3D11Texture2D failed: hr=0x%08X", hr);
        duplication->ReleaseFrame();
        SafeRelease(duplication);
        SafeRelease(context);
        SafeRelease(device);
        return false;
    }

    // Get texture dimensions
    D3D11_TEXTURE2D_DESC texDesc;
    desktopTexture->GetDesc(&texDesc);
    uint32_t width = texDesc.Width;
    uint32_t height = texDesc.Height;

    // --- Create staging texture for CPU readback ---
    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = width;
    stagingDesc.Height = height;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ID3D11Texture2D* stagingTexture = nullptr;
    hr = device->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);
    if (FAILED(hr)) {
        LogError("[Screenshot] CreateTexture2D (staging) failed: hr=0x%08X", hr);
        SafeRelease(desktopTexture);
        duplication->ReleaseFrame();
        SafeRelease(duplication);
        SafeRelease(context);
        SafeRelease(device);
        return false;
    }

    // Copy desktop texture to staging
    context->CopyResource(stagingTexture, desktopTexture);
    SafeRelease(desktopTexture);
    duplication->ReleaseFrame();
    SafeRelease(duplication);

    // --- Map staging texture ---
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = context->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        LogError("[Screenshot] Map staging texture failed: hr=0x%08X", hr);
        SafeRelease(stagingTexture);
        SafeRelease(context);
        SafeRelease(device);
        return false;
    }

    // --- Save as PNG using WIC ---
    IWICImagingFactory* factory = nullptr;
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        LogError("[Screenshot] CoCreateInstance WICImagingFactory failed: hr=0x%08X", hr);
        context->Unmap(stagingTexture, 0);
        SafeRelease(stagingTexture);
        SafeRelease(context);
        SafeRelease(device);
        return false;
    }

    IWICStream* stream = nullptr;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr)) {
        LogError("[Screenshot] CreateStream failed: hr=0x%08X", hr);
        SafeRelease(factory);
        context->Unmap(stagingTexture, 0);
        SafeRelease(stagingTexture);
        SafeRelease(context);
        SafeRelease(device);
        return false;
    }

    // Convert path to wide string for WIC
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, fullPath.c_str(), -1, nullptr, 0);
    std::wstring widePath(wideLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, fullPath.c_str(), -1, widePath.data(), wideLen);

    hr = stream->InitializeFromFilename(widePath.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) {
        LogError("[Screenshot] InitializeFromFilename failed: hr=0x%08X", hr);
        SafeRelease(stream);
        SafeRelease(factory);
        context->Unmap(stagingTexture, 0);
        SafeRelease(stagingTexture);
        SafeRelease(context);
        SafeRelease(device);
        return false;
    }

    IWICBitmapEncoder* encoder = nullptr;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr)) {
        LogError("[Screenshot] CreateEncoder(PNG) failed: hr=0x%08X", hr);
        SafeRelease(stream);
        SafeRelease(factory);
        context->Unmap(stagingTexture, 0);
        SafeRelease(stagingTexture);
        SafeRelease(context);
        SafeRelease(device);
        return false;
    }

    hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        LogError("[Screenshot] encoder->Initialize failed: hr=0x%08X", hr);
        SafeRelease(encoder);
        SafeRelease(stream);
        SafeRelease(factory);
        context->Unmap(stagingTexture, 0);
        SafeRelease(stagingTexture);
        SafeRelease(context);
        SafeRelease(device);
        return false;
    }

    IWICBitmapFrameEncode* frameEncode = nullptr;
    IPropertyBag2* encoderOptions = nullptr;
    hr = encoder->CreateNewFrame(&frameEncode, &encoderOptions);
    if (FAILED(hr)) {
        LogError("[Screenshot] CreateNewFrame failed: hr=0x%08X", hr);
        SafeRelease(encoder);
        SafeRelease(stream);
        SafeRelease(factory);
        context->Unmap(stagingTexture, 0);
        SafeRelease(stagingTexture);
        SafeRelease(context);
        SafeRelease(device);
        return false;
    }

    hr = frameEncode->Initialize(encoderOptions);
    if (FAILED(hr)) {
        LogError("[Screenshot] frameEncode->Initialize failed: hr=0x%08X", hr);
        SafeRelease(encoderOptions);
        SafeRelease(frameEncode);
        SafeRelease(encoder);
        SafeRelease(stream);
        SafeRelease(factory);
        context->Unmap(stagingTexture, 0);
        SafeRelease(stagingTexture);
        SafeRelease(context);
        SafeRelease(device);
        return false;
    }

    frameEncode->SetSize(width, height);

    // Set pixel format
    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
    frameEncode->SetPixelFormat(&pixelFormat);

    // Write pixels from mapped staging texture
    // Desktop Duplication returns BGRA which matches WIC's 32bppBGRA
    hr = frameEncode->WritePixels(height, mapped.RowPitch, mapped.RowPitch * height, static_cast<BYTE*>(mapped.pData));
    if (FAILED(hr)) {
        LogError("[Screenshot] WritePixels failed: hr=0x%08X", hr);
        SafeRelease(encoderOptions);
        SafeRelease(frameEncode);
        SafeRelease(encoder);
        SafeRelease(stream);
        SafeRelease(factory);
        context->Unmap(stagingTexture, 0);
        SafeRelease(stagingTexture);
        SafeRelease(context);
        SafeRelease(device);
        return false;
    }

    frameEncode->Commit();
    encoder->Commit();

    // --- Cleanup ---
    SafeRelease(encoderOptions);
    SafeRelease(frameEncode);
    SafeRelease(encoder);
    SafeRelease(stream);
    SafeRelease(factory);

    context->Unmap(stagingTexture, 0);
    SafeRelease(stagingTexture);
    SafeRelease(context);
    SafeRelease(device);

    LogInfo("[Screenshot] Saved: %s (%ux%u)", fullPath.c_str(), width, height);
    return true;
}
