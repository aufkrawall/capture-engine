#include "encoder_device_bridge.h"

#include <dxgi1_2.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ce::media {

namespace {

std::string HrText(const char* what, HRESULT hr) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%s HR=0x%08lX", what, static_cast<unsigned long>(hr));
    return buffer;
}

void SetError(std::string* error, std::string text) {
    if (error)
        *error = std::move(text);
}

template <typename T>
void SafeRelease(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

}  // namespace

bool EncoderUsesSeparateDevice(const char* encoderName) {
    return encoderName && std::strstr(encoderName, "_nvenc") != nullptr;
}

size_t DeferredSubmitDepth(int fps) {
    if (fps <= 0)
        return 1;
    const int64_t frames = (static_cast<int64_t>(kDeferredSubmitBudgetMs) * fps + 999) / 1000;
    return std::clamp<size_t>(static_cast<size_t>(frames), 1, kDeferredSubmitMaxDepth);
}

SubmitGateAction NextSubmitGateAction(size_t pending, size_t depth, bool frontComplete, bool drainAll) {
    if (pending == 0)
        return SubmitGateAction::Hold;
    if (frontComplete)
        return SubmitGateAction::Send;
    return (drainAll || pending > depth) ? SubmitGateAction::WaitThenSend : SubmitGateAction::Hold;
}

EncoderDeviceBridge::~EncoderDeviceBridge() {
    Reset();
}

void EncoderDeviceBridge::Reset() {
    for (Alias& alias : aliases_) {
        SafeRelease(alias.workTexture);
        SafeRelease(alias.encoderTexture);
    }
    aliases_.clear();
    SafeRelease(fence_);
    if (event_) {
        CloseHandle(event_);
        event_ = nullptr;
    }
    SafeRelease(workDevice1_);
    SafeRelease(encoderDevice_);
    signaled_ = 0;
}

bool EncoderDeviceBridge::Create(ID3D11Device* workDevice, std::string* error) {
    Reset();
    if (!workDevice) {
        SetError(error, "no work device");
        return false;
    }
    HRESULT hr = workDevice->QueryInterface(IID_PPV_ARGS(&workDevice1_));
    if (FAILED(hr)) {
        SetError(error, HrText("work device lacks ID3D11Device1", hr));
        return false;
    }
    ID3D11Device5* workDevice5 = nullptr;
    hr = workDevice->QueryInterface(IID_PPV_ARGS(&workDevice5));
    if (FAILED(hr)) {
        SetError(error, HrText("work device lacks ID3D11Device5 (fences)", hr));
        Reset();
        return false;
    }
    hr = workDevice5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    workDevice5->Release();
    if (FAILED(hr)) {
        SetError(error, HrText("CreateFence", hr));
        Reset();
        return false;
    }
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) {
        SetError(error, HrText("CreateEvent", HRESULT_FROM_WIN32(GetLastError())));
        Reset();
        return false;
    }

    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    hr = workDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (SUCCEEDED(hr))
        hr = dxgiDevice->GetAdapter(&adapter);
    SafeRelease(dxgiDevice);
    if (FAILED(hr) || !adapter) {
        SetError(error, HrText("work device adapter", hr));
        Reset();
        return false;
    }
    const D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, featureLevels,
                           ARRAYSIZE(featureLevels), D3D11_SDK_VERSION, &encoderDevice_, nullptr, nullptr);
    adapter->Release();
    if (FAILED(hr) || !encoderDevice_) {
        SetError(error, HrText("D3D11CreateDevice(encoder)", hr));
        Reset();
        return false;
    }
    ID3D10Multithread* multithread = nullptr;
    if (SUCCEEDED(encoderDevice_->QueryInterface(IID_PPV_ARGS(&multithread)))) {
        multithread->SetMultithreadProtected(TRUE);
        multithread->Release();
    }
    return true;
}

ID3D11Texture2D* EncoderDeviceBridge::OpenOnWorkDevice(ID3D11Texture2D* encoderTexture, HRESULT* hrOut) {
    IDXGIResource1* resource = nullptr;
    HRESULT hr = encoderTexture->QueryInterface(IID_PPV_ARGS(&resource));
    HANDLE handle = nullptr;
    if (SUCCEEDED(hr))
        hr = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                          &handle);
    SafeRelease(resource);
    ID3D11Texture2D* workTexture = nullptr;
    if (SUCCEEDED(hr))
        hr = workDevice1_->OpenSharedResource1(handle, IID_PPV_ARGS(&workTexture));
    if (handle)
        CloseHandle(handle);
    if (hrOut)
        *hrOut = hr;
    return SUCCEEDED(hr) ? workTexture : nullptr;
}

ID3D11Texture2D* EncoderDeviceBridge::WorkTexture(ID3D11Texture2D* encoderTexture) {
    if (!encoderTexture || !Active())
        return nullptr;
    for (const Alias& alias : aliases_) {
        if (alias.encoderTexture == encoderTexture)
            return alias.workTexture;
    }
    HRESULT hr = S_OK;
    ID3D11Texture2D* workTexture = OpenOnWorkDevice(encoderTexture, &hr);
    if (!workTexture)
        return nullptr;
    // The reference on the encoder texture keeps its address from being reused
    // by a later pool allocation while this entry exists.
    encoderTexture->AddRef();
    aliases_.push_back({encoderTexture, workTexture});
    return workTexture;
}

uint64_t EncoderDeviceBridge::SignalWorkDone(ID3D11DeviceContext* workContext) {
    if (!fence_ || !workContext)
        return 0;
    ID3D11DeviceContext4* context4 = nullptr;
    if (FAILED(workContext->QueryInterface(IID_PPV_ARGS(&context4))))
        return 0;
    const uint64_t value = signaled_ + 1;
    const HRESULT hr = context4->Signal(fence_, value);
    if (SUCCEEDED(hr)) {
        // The work device carries no encoder submit, so this Flush only hands
        // the frame's commands to the GPU; nothing on it waits for NVENC.
        context4->Flush();
        signaled_ = value;
    }
    context4->Release();
    return SUCCEEDED(hr) ? value : 0;
}

bool EncoderDeviceBridge::IsComplete(uint64_t value) const {
    return fence_ && fence_->GetCompletedValue() >= value;
}

EncoderDeviceBridge::WaitResult EncoderDeviceBridge::Wait(uint64_t value, DWORD timeoutMs) {
    if (!fence_ || !event_)
        return WaitResult::Failed;
    const uint64_t completed = fence_->GetCompletedValue();
    if (completed == UINT64_MAX)
        return WaitResult::DeviceLost;
    if (completed >= value)
        return WaitResult::Complete;
    if (FAILED(fence_->SetEventOnCompletion(value, event_)))
        return WaitResult::Failed;
    // The event is auto-reset and may still carry the signal of an earlier,
    // timed-out wait, so a wake only counts once the value is really reached.
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (true) {
        const ULONGLONG now = GetTickCount64();
        const DWORD remaining = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
        const DWORD wait = WaitForSingleObject(event_, remaining);
        const uint64_t reached = fence_->GetCompletedValue();
        if (reached == UINT64_MAX)
            return WaitResult::DeviceLost;
        if (reached >= value)
            return WaitResult::Complete;
        if (wait == WAIT_TIMEOUT)
            return WaitResult::Timeout;
        if (wait != WAIT_OBJECT_0)
            return WaitResult::Failed;
    }
}

}  // namespace ce::media
