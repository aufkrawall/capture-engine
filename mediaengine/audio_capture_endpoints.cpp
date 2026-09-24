#include "audio_capture_internal.h"

// Endpoint notifications for AudioCapture (see audio_recovery_policy.h,
// "Following the Windows default device"). Windows delivers these on its own
// notification thread; they must not block and must not call back into the
// enumerator, so they only publish flags the capture worker acts on.

namespace {
// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - __uuidof resolves to a compile-time constant GUID
const IID kIidMMNotificationClient = __uuidof(IMMNotificationClient);
}  // namespace

HRESULT STDMETHODCALLTYPE AudioCapture::EndpointListener::QueryInterface(REFIID riid, void** object) {
    if (!object) {
        return E_POINTER;
    }
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, kIidMMNotificationClient)) {
        *object = static_cast<IMMNotificationClient*>(this);
        AddRef();
        return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
}

// The listener is a member of its AudioCapture, and the worker unregisters it
// before the capture can be destroyed, so the count is informational only.
ULONG STDMETHODCALLTYPE AudioCapture::EndpointListener::AddRef() {
    return refs_.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE AudioCapture::EndpointListener::Release() {
    return refs_.fetch_sub(1, std::memory_order_relaxed) - 1;
}

HRESULT STDMETHODCALLTYPE AudioCapture::EndpointListener::OnDeviceStateChanged(LPCWSTR, DWORD newState) {
    if (newState == DEVICE_STATE_ACTIVE) {
        owner_.endpointArrived_.store(true, std::memory_order_release);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AudioCapture::EndpointListener::OnDeviceAdded(LPCWSTR) {
    owner_.endpointArrived_.store(true, std::memory_order_release);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AudioCapture::EndpointListener::OnDeviceRemoved(LPCWSTR) {
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AudioCapture::EndpointListener::OnDefaultDeviceChanged(EDataFlow flow, ERole role,
                                                                                LPCWSTR) {
    // deviceId_ and isLoopback_ are fixed for the lifetime of a registration
    // (set by Start() before the worker registers, cleared only after join).
    if (ce::audio::IsFollowedDefaultDeviceChange(owner_.deviceId_.empty(), owner_.isLoopback_,
                                                 static_cast<int>(flow), static_cast<int>(role))) {
        owner_.defaultDeviceChanged_.store(true, std::memory_order_release);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AudioCapture::EndpointListener::OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) {
    return S_OK;
}

void AudioCapture::RegisterEndpointListenerOnWorker() {
    if (endpointListenerRegistered_ || !pEnumerator) {
        return;
    }
    defaultDeviceChanged_.store(false, std::memory_order_release);
    endpointArrived_.store(false, std::memory_order_release);
    const HRESULT hr = pEnumerator->RegisterEndpointNotificationCallback(&endpointListener_);
    if (FAILED(hr)) {
        DLL_Log(
            "[AudioCapture] Endpoint notifications unavailable (0x%lx); a Windows default-device switch will not be "
            "followed until the old endpoint is invalidated",
            static_cast<unsigned long>(hr));
        return;
    }
    endpointListenerRegistered_ = true;
}

void AudioCapture::UnregisterEndpointListenerOnWorker() {
    if (!endpointListenerRegistered_ || !pEnumerator) {
        return;
    }
    // Returns only once no callback is running or will run for this client.
    pEnumerator->UnregisterEndpointNotificationCallback(&endpointListener_);
    endpointListenerRegistered_ = false;
}

bool AudioCapture::DefaultEndpointDiffersFromActiveOnWorker() {
    if (!pEnumerator) {
        return false;
    }
    IMMDevice* defaultDevice = nullptr;
    if (FAILED(pEnumerator->GetDefaultAudioEndpoint(isLoopback_ ? eRender : eCapture, eConsole, &defaultDevice)) ||
        !defaultDevice) {
        return false;
    }
    LPWSTR defaultId = nullptr;
    LPWSTR activeId = nullptr;
    defaultDevice->GetId(&defaultId);
    if (pDevice) {
        pDevice->GetId(&activeId);
    }
    const bool differs = ce::audio::ShouldSwitchToNewDefaultEndpoint(pCaptureClient != nullptr, activeId, defaultId);
    if (defaultId) {
        CoTaskMemFree(defaultId);
    }
    if (activeId) {
        CoTaskMemFree(activeId);
    }
    defaultDevice->Release();
    return differs;
}
