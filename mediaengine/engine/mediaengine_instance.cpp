#include "mediaengine_instance.h"

#include <dxgi1_5.h>

#include <functional>
#include <string>

#include "common/config/encoder_auto_request.h"
#include "mediaengine/video/encoder_auto_probe.h"
#include "mediaengine_internal.h"
#include "screen_grab_adapter.h"

MediaEngineInstance::MediaEngineInstance() = default;

MediaEngineInstance::~MediaEngineInstance() = default;

namespace {

// Clears and flushes the context, trims the device's residency, then drops both references.
void ReleaseD3D11Objects(ID3D11Device*& device, ID3D11DeviceContext*& context) {
    if (context) {
        context->ClearState();
        context->Flush();
    }
    if (device) {
        IDXGIDevice3* dxgiDevice3 = nullptr;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice3))) && dxgiDevice3) {
            dxgiDevice3->Trim();
            dxgiDevice3->Release();
            DLL_Log("[MediaEngine] Trimmed shared D3D11 device residency");
        }
    }
    if (context) {
        context->Release();
        context = nullptr;
    }
    if (device) {
        // The last reference: anything left means a holder outlives the pipeline, with its driver threads and VRAM.
        const ULONG left = device->Release();
        device = nullptr;
        DLL_Log("[MediaEngine] Shared D3D11 device released (references left=%lu)", left);
    }
}

}  // namespace

ID3D11Device* MediaEngineInstance::GetSharedDevice() {
    if (sharedDevice)
        return sharedDevice;

    // Create D3D11 device with video support
    D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL featureLevel;

    UINT createFlags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    // The GPU the encoder can run on and, among those, the one driving the
    // captured monitor (screen_grab_adapter.h); adapter 0 used to be taken.
    std::string captureMonitor = "auto";
    VideoConfig videoConfig;
    {
        std::lock_guard<std::recursive_mutex> apiLock(apiMutex);
        if (engine) {
            videoConfig = engine->config.video;
            captureMonitor = engine->config.captureMonitor;
        }
    }
    // encoder=auto: only a GPU on which an encoder opens is eligible; the probe
    // runs without the engine lock and its results serve the encoder later.
    std::function<bool(const LUID&)> hasAutoEncoder;
    if (ce::encoder_auto::IsAutoEncoderRequest(videoConfig.encoder)) {
        hasAutoEncoder = [this, &videoConfig](const LUID& luid) {
            const auto resolution = ce::media::ResolveAutoEncoderForAdapter(videoConfig.encoder, luid, videoConfig,
                                                                            autoEncoderLastResolutions);
            DLL_Log("%s site=screen-grab-adapter", resolution.log.c_str());
            return !resolution.encoder.empty();
        };
    }
    std::string adapterLog;
    IDXGIAdapter1* adapter =
        ce::media::SelectScreenGrabAdapter(videoConfig.encoder, captureMonitor, &adapterLog, hasAutoEncoder);
    DLL_Log("%s", adapterLog.c_str());
    HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   createFlags, featureLevels, 2, D3D11_SDK_VERSION, &sharedDevice, &featureLevel,
                                   &sharedContext);
    if (adapter)
        adapter->Release();

    if (FAILED(hr)) {
        DLL_Log("[MediaEngine] Failed to create shared D3D11 device: HR=0x%x", static_cast<unsigned>(hr));
        return nullptr;
    }

    DLL_Log("[MediaEngine] Created shared D3D11 device (Feature Level: 0x%x)", featureLevel);
    // Enable Multithreaded protection for D3D11 device
    ID3D11Multithread* pMultithread = nullptr;
    if (SUCCEEDED(sharedDevice->QueryInterface(__uuidof(ID3D11Multithread), (void**)&pMultithread))) {
        pMultithread->SetMultithreadProtected(TRUE);
        pMultithread->Release();
        DLL_Log("[Media] D3D11 Multithread protection ENABLED");
    }

    return sharedDevice;
}

void MediaEngineInstance::ReleaseSharedDevice() {
    // The converter holds a device reference, so it is released before the device.
    ReleaseRetainedFrameConverter();
    ReleaseD3D11Objects(sharedDevice, sharedContext);
}
