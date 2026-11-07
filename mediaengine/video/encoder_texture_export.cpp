// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "mediaengine/video/encoder_texture_export.h"

#include <dxgi1_4.h>

#include <utility>

#include "mediaengine/engine/mediaengine.h"
#include "mediaengine/video/gpu_memory_breakdown.h"

namespace ce::media {

struct EncoderTextureExport::Set {
    ComGuard<ID3D11Texture2D> textures[ENCODER_TEXTURE_SLOT_COUNT];
    HANDLE kmt[ENCODER_TEXTURE_SLOT_COUNT] = {};  // legacy global handles: they die with the textures
    ComGuard<ID3D11Fence> fence;
    HandleGuard fenceHandle;
    EncoderTextureRequest request;
};

namespace {

unsigned long Hex(HRESULT hr) { return static_cast<unsigned long>(hr); }

}  // namespace

EncoderTextureExport::EncoderTextureExport(RecordingMemory& route) : route_(route) {}

EncoderTextureExport::~EncoderTextureExport() {
    set_.reset();
    device_.reset();
}

ID3D11Device5* EncoderTextureExport::DeviceFor(const EncoderTextureRequest& request, const char** failedStage,
                                               HRESULT* hr) {
    if (device_ && deviceLuidLow_ == request.luidLow && deviceLuidHigh_ == request.luidHigh)
        return device_.get();
    device_.reset();

    ComGuard<IDXGIAdapter> adapter;
    if (request.luidLow != 0 || request.luidHigh != 0) {
        LUID luid{};
        luid.LowPart = static_cast<DWORD>(request.luidLow);
        luid.HighPart = request.luidHigh;
        ComGuard<IDXGIFactory4> factory;
        *hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.put()));
        if (SUCCEEDED(*hr))
            *hr = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(adapter.put()));
        if (FAILED(*hr) || !adapter) {
            // Textures on another adapter could not be imported by the producer.
            *failedStage = "adapter lookup";
            return nullptr;
        }
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL level = {};
    ComGuard<ID3D11Device> base;
    *hr = D3D11CreateDevice(adapter.get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                            base.put(), &level, nullptr);
    if (FAILED(*hr) || !base) {
        *failedStage = "device creation";
        return nullptr;
    }
    // Shared fences need ID3D11Device5.
    *hr = base->QueryInterface(IID_PPV_ARGS(device_.put()));
    if (FAILED(*hr) || !device_) {
        device_.reset();
        *failedStage = "ID3D11Device5";
        return nullptr;
    }
    deviceLuidLow_ = request.luidLow;
    deviceLuidHigh_ = request.luidHigh;
    DLL_Log("[EncoderTextures] Device created on adapter %08x:%08x (feature level 0x%x)",
            static_cast<unsigned>(request.luidHigh), static_cast<unsigned>(request.luidLow),
            static_cast<unsigned>(level));
    return device_.get();
}

bool EncoderTextureExport::Ensure(const EncoderTextureRequest& request, SharedMemoryLayout* binding) {
    std::lock_guard serialize(serialize_);
    route_.InitializeIfEmpty(binding);

    std::shared_ptr<const Set> retired;
    uint64_t retiredGeneration = 0;
    {
        std::unique_lock state(state_);
        if (set_ && set_->request == request) {
            // serialize_ keeps the set installed while it is published.
            const Set& kept = *set_;
            state.unlock();
            Publish(kept, generation_.load(std::memory_order_acquire), false);
            return true;
        }
        if (set_) {
            retired = std::move(set_);
            retiredGeneration = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
            exported_.store(false, std::memory_order_release);
        }
    }
    if (retired) {
        // The producer drops the old imports on the withdrawn publication; views the
        // encoder still holds keep their allocation alive until it refreshes.
        Withdraw();
        const auto& old = retired->request;
        DLL_Log("[EncoderTextures] Retired generation=%llu %ux%u format=%u adapter=%08x:%08x for "
                "%ux%u format=%u adapter=%08x:%08x",
                static_cast<unsigned long long>(retiredGeneration), old.width, old.height, old.format,
                static_cast<unsigned>(old.luidHigh), static_cast<unsigned>(old.luidLow), request.width,
                request.height, request.format, static_cast<unsigned>(request.luidHigh),
                static_cast<unsigned>(request.luidLow));
        retired.reset();
    }

    if (request.width == 0 || request.height == 0 || request.format == 0) {
        LogFailure(request, "request", E_INVALIDARG);
        return false;
    }
    const char* failedStage = nullptr;
    HRESULT hr = S_OK;
    ID3D11Device5* device = DeviceFor(request, &failedStage, &hr);
    if (!device) {
        LogFailure(request, failedStage, hr);
        return false;
    }

    auto fresh = std::make_shared<Set>();
    fresh->request = request;
    for (int i = 0; i < ENCODER_TEXTURE_SLOT_COUNT; ++i) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = request.width;
        desc.Height = request.height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = static_cast<DXGI_FORMAT>(request.format);
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;  // a legacy (KMT) handle, which DXVK imports
        hr = device->CreateTexture2D(&desc, nullptr, fresh->textures[i].put());
        if (FAILED(hr)) {
            LogFailure(request, "texture creation", hr);
            return false;
        }
        ComGuard<IDXGIResource> resource;
        hr = fresh->textures[i]->QueryInterface(IID_PPV_ARGS(resource.put()));
        if (SUCCEEDED(hr))
            hr = resource->GetSharedHandle(&fresh->kmt[i]);
        if (FAILED(hr) || !fresh->kmt[i]) {
            LogFailure(request, "texture handle", FAILED(hr) ? hr : E_HANDLE);
            return false;
        }
    }
    hr = device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(fresh->fence.put()));
    if (FAILED(hr)) {
        LogFailure(request, "fence creation", hr);
        return false;
    }
    HANDLE fenceHandle = nullptr;
    hr = fresh->fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fenceHandle);
    if (FAILED(hr) || !fenceHandle) {
        LogFailure(request, "fence handle", FAILED(hr) ? hr : E_HANDLE);
        return false;
    }
    fresh->fenceHandle.reset(fenceHandle);

    // Installed before it is published: a producer frame that carries the new handles
    // must find them.
    uint64_t generation = 0;
    const Set* installed = fresh.get();
    {
        std::lock_guard state(state_);
        set_ = std::move(fresh);
        generation = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
        exported_.store(true, std::memory_order_release);
    }
    Publish(*installed, generation, true);
    failureGate_.Reset();
    return true;
}

void EncoderTextureExport::Publish(const Set& set, uint64_t generation, bool fresh) {
    bool published = false;
    if (auto publication = route_.Publish(); publication) {
        const auto& current = publication->encoderTextures;
        if (!fresh && current.kmtReady.load(std::memory_order_acquire) &&
            current.ready.load(std::memory_order_acquire) &&
            current.GetKmtTextureHandle(0) == reinterpret_cast<uint64_t>(set.kmt[0]))
            return;  // a re-request of the set the route already advertises
        for (int i = 0; i < ENCODER_TEXTURE_SLOT_COUNT; ++i)
            publication->encoderTextures.SetKmtTextureHandle(i, reinterpret_cast<uint64_t>(set.kmt[i]));
        if (!publication->useEncoderTextures.load(std::memory_order_acquire)) {
            publication->encoderTextures.SetFenceHandle(reinterpret_cast<uint64_t>(set.fenceHandle.get()));
        }
        publication->encoderTextures.SetWidth(set.request.width);
        publication->encoderTextures.SetHeight(set.request.height);
        publication->encoderTextures.SetFormat(set.request.format);
        publication->encoderTextures.kmtReady.store(true, std::memory_order_release);
        publication->encoderTextures.ready.store(true, std::memory_order_release);
        published = true;
    }
    if (!fresh && !published)
        return;
    // A withdrawn publication of a kept set is advertised again, so true from Ensure
    // always means the current route carries the set.
    DLL_Log("[EncoderTextures] %s generation=%llu %ux%u format=%u slots=%d adapter=%08x:%08x fence=%p "
            "published=%d",
            fresh ? "Exported" : "Republished", static_cast<unsigned long long>(generation), set.request.width,
            set.request.height, set.request.format, ENCODER_TEXTURE_SLOT_COUNT,
            static_cast<unsigned>(set.request.luidHigh), static_cast<unsigned>(set.request.luidLow),
            set.fenceHandle.get(), published ? 1 : 0);
}

void EncoderTextureExport::Withdraw() {
    if (auto publication = route_.Publish(); publication) {
        publication->encoderTextures.kmtReady.store(false, std::memory_order_release);
        publication->encoderTextures.ready.store(false, std::memory_order_release);
    }
}

bool EncoderTextureExport::Release(const char* reason) {
    std::lock_guard serialize(serialize_);
    std::shared_ptr<const Set> released;
    uint64_t generation = 0;
    {
        std::lock_guard state(state_);
        released = std::move(set_);
        if (released) {
            generation = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
            exported_.store(false, std::memory_order_release);
        }
    }
    if (released)
        Withdraw();
    released.reset();
    const bool hadDevice = device_.get() != nullptr;
    device_.reset();
    deviceLuidLow_ = 0;
    deviceLuidHigh_ = 0;
    failureGate_.Reset();
    if (generation != 0)
        DLL_Log("[EncoderTextures] Released (%s): generation=%llu device=%d", reason ? reason : "unspecified",
                static_cast<unsigned long long>(generation), hadDevice ? 1 : 0);
    return generation != 0;
}

EncoderTextureExport::Handles EncoderTextureExport::Snapshot() const {
    Handles handles;
    std::lock_guard state(state_);
    handles.generation = generation_.load(std::memory_order_acquire);
    if (set_) {
        for (int i = 0; i < ENCODER_TEXTURE_SLOT_COUNT; ++i)
            handles.kmt[i] = set_->kmt[i];
    }
    return handles;
}

ID3D11Texture2D* EncoderTextureExport::OpenOn(ID3D11Device* device, uint64_t generation, int slot) const {
    if (!device || slot < 0 || slot >= ENCODER_TEXTURE_SLOT_COUNT)
        return nullptr;
    std::shared_ptr<const Set> pinned;
    {
        std::lock_guard state(state_);
        if (!set_ || generation_.load(std::memory_order_acquire) != generation || !set_->kmt[slot])
            return nullptr;
        pinned = set_;
    }
    // The KMT handle needs its backing allocation until the driver returns. Pin the
    // set without making capture-side Ensure/Release wait for the encoder's device.
    ComGuard<ID3D11Texture2D> view;
    const HRESULT hr = device->OpenSharedResource(pinned->kmt[slot], IID_PPV_ARGS(view.put()));
    if (SUCCEEDED(hr) && view)
        return view.release();
    if (const auto verdict = openFailureGate_.Observe(log_meter::FieldKey(generation, hr)))
        DLL_Log("[EncoderTextures] Opening slot %d of generation=%llu on the encoder device failed: HR=0x%08lx "
                "(repeats suppressed=%llu)",
                slot, static_cast<unsigned long long>(generation), Hex(hr),
                static_cast<unsigned long long>(verdict.suppressed));
    return nullptr;
}

uint64_t EncoderTextureExport::GpuBytes() const {
    std::lock_guard state(state_);
    if (!set_)
        return 0;
    return ENCODER_TEXTURE_SLOT_COUNT *
           ce::gpu_memory::EstimateTextureBytes(set_->request.width, set_->request.height,
                                                static_cast<DXGI_FORMAT>(set_->request.format));
}

void EncoderTextureExport::LogFailure(const EncoderTextureRequest& request, const char* stage, HRESULT hr) {
    // The callers retry every capture-loop pass while no set is exported.
    const uint64_t key = log_meter::FieldKey(reinterpret_cast<uintptr_t>(stage), hr, request.width, request.height,
                                             request.format, request.luidLow, request.luidHigh);
    if (const auto verdict = failureGate_.Observe(key))
        DLL_Log("[EncoderTextures] Export failed at %s: HR=0x%08lx request=%ux%u format=%u adapter=%08x:%08x "
                "(repeats suppressed=%llu)",
                stage, Hex(hr), request.width, request.height, request.format, static_cast<unsigned>(request.luidHigh),
                static_cast<unsigned>(request.luidLow), static_cast<unsigned long long>(verdict.suppressed));
}

void ExportedTextureViews::Refresh(const EncoderTextureExport& source, ID3D11Device* device) {
    if (device == device_ && source.Generation() == generation_)
        return;
    Reset();
    const EncoderTextureExport::Handles snapshot = source.Snapshot();
    generation_ = snapshot.generation;
    device_ = device;
    for (int i = 0; i < ENCODER_TEXTURE_SLOT_COUNT; ++i)
        handles_[i] = snapshot.kmt[i];
}

ID3D11Texture2D* ExportedTextureViews::View(const EncoderTextureExport& source, int slot) {
    if (!views_[slot] && handles_[slot] && !unopenable_[slot]) {
        views_[slot] = source.OpenOn(device_, generation_, slot);
        unopenable_[slot] = views_[slot] == nullptr;
    }
    return views_[slot];
}

ID3D11Texture2D* ExportedTextureViews::Acquire(const EncoderTextureExport& source, ID3D11Device* device,
                                               HANDLE kmtHandle, int* slot) {
    if (!device || !kmtHandle)
        return nullptr;
    Refresh(source, device);
    for (int i = 0; i < ENCODER_TEXTURE_SLOT_COUNT; ++i) {
        if (handles_[i] != kmtHandle)
            continue;
        ID3D11Texture2D* view = View(source, i);
        if (!view)
            return nullptr;
        view->AddRef();
        if (slot)
            *slot = i;
        return view;
    }
    return nullptr;
}

ID3D11Texture2D* ExportedTextureViews::AcquireSlot(const EncoderTextureExport& source, ID3D11Device* device,
                                                   int slot) {
    if (!device || slot < 0 || slot >= ENCODER_TEXTURE_SLOT_COUNT)
        return nullptr;
    Refresh(source, device);
    ID3D11Texture2D* view = View(source, slot);
    if (view)
        view->AddRef();
    return view;
}

void ExportedTextureViews::Reset() {
    for (int i = 0; i < ENCODER_TEXTURE_SLOT_COUNT; ++i) {
        if (views_[i]) {
            views_[i]->Release();
            views_[i] = nullptr;
        }
        handles_[i] = nullptr;
        unopenable_[i] = false;
    }
    generation_ = 0;
    device_ = nullptr;
}

bool ExportedTextureViews::HasViews() const {
    for (int i = 0; i < ENCODER_TEXTURE_SLOT_COUNT; ++i) {
        if (views_[i])
            return true;
    }
    return false;
}

}  // namespace ce::media
