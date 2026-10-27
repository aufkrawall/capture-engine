// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall

#include "flip_metering_bridge.h"

#include <windows.h>

#include <d3d12.h>

#include <atomic>
#include <cwchar>
#include <cwctype>
#include <mutex>

#include "common/logging/log_meter.h"
#include "flip_metering_bridge_policy.h"
#include "hook/hooking/vtable_hook.h"
#include "hook/overlay/overlay_compat.h"
#include "hook/present/vulkan_layer_metering_bridge.h"
#include "hook/runtime/hook_common.h"

namespace ce::flip_metering_bridge {

namespace {

// vkd3d-proton's ID3DLowLatencyDevice (include/vkd3d_device_vkd3d_ext.idl). dxvk-nvapi passes every
// NVAPI latency marker through it with the frame id vkd3d later hands to vkSetLatencyMarkerNV.
constexpr GUID kIidD3DLowLatencyDevice = {0xf3112584, 0x41f9, 0x348d, {0xa5, 0x9b, 0x00, 0xb7, 0xe1, 0xd2, 0x85, 0xd6}};
// IUnknown (3), SupportsLowLatency, LatencySleep, SetLatencySleepMode, SetLatencyMarker, GetLatencyInfo.
constexpr size_t kSetLatencyMarkerSlot = 6;

using PfnSetLatencyMarker = HRESULT(STDMETHODCALLTYPE*)(void* self, UINT64 frameId, UINT32 markerType);
std::atomic<PfnSetLatencyMarker> g_originalSetLatencyMarker{nullptr};
std::once_flag g_markerHookOnce;
// Without the markers no batch can be bound, and a generator told its configuration took effect stops
// pacing on its own; SetFlipConfig then reports NVAPI_NOT_SUPPORTED instead.
std::atomic<bool> g_markerHookActive{false};

// One binder per D3D12 device; titles run frame generation on one, so a handful of slots is plenty.
// A device that reappears at a recycled address restarts from the evicted slot's empty state.
struct BinderSlot {
    const void* device = nullptr;
    BatchBinder binder;
    uint64_t lastUse = 0;
};
std::mutex g_binderMutex;
BinderSlot g_binders[4];
uint64_t g_binderUse = 0;

BatchBinder& BinderForLocked(const void* device) {
    BinderSlot* oldest = &g_binders[0];
    for (BinderSlot& slot : g_binders) {
        if (slot.device == device) {
            slot.lastUse = ++g_binderUse;
            return slot.binder;
        }
        if (slot.lastUse < oldest->lastUse)
            oldest = &slot;
    }
    *oldest = BinderSlot{};
    oldest->device = device;
    oldest->lastUse = ++g_binderUse;
    return oldest->binder;
}

std::atomic<uint64_t> g_bindingsPublished{0};
std::atomic<uint64_t> g_bindingsUndelivered{0};

void PublishBinding(const Binding& binding) {
    if (ce::vulkan_layer_bridge::PublishFlipMeteringBatch(binding.frameId, binding.framesPerBatch)) {
        g_bindingsPublished.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint64_t undelivered = g_bindingsUndelivered.fetch_add(1, std::memory_order_relaxed) + 1;
    if (undelivered <= 4 || (undelivered % 1024) == 0) {
        HookLogImportant("NVAPI flip metering: no resident Vulkan layer took the batch for frame %llu "
                         "(numFramesPerBatch=%u, undelivered=%llu)",
                         static_cast<unsigned long long>(binding.frameId), binding.framesPerBatch,
                         static_cast<unsigned long long>(undelivered));
    }
}

HRESULT STDMETHODCALLTYPE Detour_SetLatencyMarker(void* self, UINT64 frameId, UINT32 markerType) {
    const PfnSetLatencyMarker original = g_originalSetLatencyMarker.load(std::memory_order_acquire);
    const HRESULT hr = original ? original(self, frameId, markerType) : E_NOTIMPL;
    if (HookIsShuttingDown() ||
        (markerType != kMarkerOutOfBandPresentStart && markerType != kMarkerOutOfBandPresentEnd)) {
        return hr;
    }
    Binding binding;
    bool publish = false;
    {
        std::lock_guard<std::mutex> lock(g_binderMutex);
        publish = BinderForLocked(self).OnLatencyMarker(frameId, markerType, binding);
    }
    if (publish)
        PublishBinding(binding);
    return hr;
}

void EnsureMarkerHook(IUnknown* lowLatencyDevice) {
    std::call_once(g_markerHookOnce, [lowLatencyDevice]() {
        void** vtable = *reinterpret_cast<void***>(lowLatencyDevice);
        // Published before the slot is patched: a marker arriving in between must still reach the runtime,
        // or that frame's id never gets recorded for its presents.
        g_originalSetLatencyMarker.store(reinterpret_cast<PfnSetLatencyMarker>(vtable[kSetLatencyMarkerSlot]),
                                         std::memory_order_release);
        void* original = nullptr;
        const VTableHook::Status status = VTableHook::Create(
            &vtable[kSetLatencyMarkerSlot], reinterpret_cast<void*>(&Detour_SetLatencyMarker), &original);
        if (original && original != reinterpret_cast<void*>(&Detour_SetLatencyMarker))
            g_originalSetLatencyMarker.store(reinterpret_cast<PfnSetLatencyMarker>(original), std::memory_order_release);
        g_markerHookActive.store(vtable[kSetLatencyMarkerSlot] == reinterpret_cast<void*>(&Detour_SetLatencyMarker),
                                 std::memory_order_release);
        char owner[MAX_PATH] = {};
        ce::overlay_compat::TryGetModulePathFromCodeAddress(original, owner, sizeof(owner));
        HookLogImportant("NVAPI flip metering: %s ID3DLowLatencyDevice::SetLatencyMarker (status=%d slot=%p "
                         "original=%p in %s) - OUT_OF_BAND_PRESENT frame ids now bind batch sizes",
                         status == VTableHook::Success || status == VTableHook::ErrorAlreadyCreated
                             ? "observing"
                             : "FAILED to observe",
                         static_cast<int>(status), static_cast<void*>(&vtable[kSetLatencyMarkerSlot]), original,
                         owner[0] ? owner : "unknown module");
    });
}

// The D3D12 runtime's low-latency interface for `commandQueue`'s device, used as the binder key; the
// pointer is the identity the marker detour receives. Null when the runtime has no such interface,
// i.e. when the queue is not a D3D12-on-Vulkan one.
const void* ResolveLowLatencyDevice(IUnknown* commandQueue) {
    ID3D12CommandQueue* queue = nullptr;
    if (FAILED(commandQueue->QueryInterface(__uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue))) ||
        !queue) {
        return nullptr;
    }
    ID3D12Device* device = nullptr;
    const HRESULT deviceHr = queue->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device));
    queue->Release();
    if (FAILED(deviceHr) || !device)
        return nullptr;
    IUnknown* lowLatencyDevice = nullptr;
    const HRESULT lowLatencyHr =
        device->QueryInterface(kIidD3DLowLatencyDevice, reinterpret_cast<void**>(&lowLatencyDevice));
    device->Release();
    if (FAILED(lowLatencyHr) || !lowLatencyDevice)
        return nullptr;
    EnsureMarkerHook(lowLatencyDevice);
    const void* identity = lowLatencyDevice;
    // The D3D12 device that owns the interface outlives this call; the key is never dereferenced.
    lowLatencyDevice->Release();
    return identity;
}

int32_t __cdecl Bridge_SetFlipConfig(IUnknown* commandQueue, UINT vidpnSrcId, void* flipConfig) {
    const FlipConfigRequest request = ReadFlipConfig(flipConfig);
    if (request.status != kNvApiOk || !commandQueue) {
        static std::atomic<uint32_t> s_rejected{0};
        const uint32_t rejected = s_rejected.fetch_add(1, std::memory_order_relaxed) + 1;
        if (rejected <= 4 || (rejected % 1024) == 0) {
            HookLogImportant("NVAPI flip metering: SetFlipConfig rejected (status=%d version=0x%X queue=%p, "
                             "rejected=%u)",
                             commandQueue ? request.status : kNvApiInvalidPointer, request.version,
                             static_cast<void*>(commandQueue), rejected);
        }
        return commandQueue ? request.status : kNvApiInvalidPointer;
    }
    const void* lowLatencyDevice = ResolveLowLatencyDevice(commandQueue);
    if (!lowLatencyDevice || !g_markerHookActive.load(std::memory_order_acquire)) {
        static std::atomic<bool> s_logged{false};
        if (!s_logged.exchange(true, std::memory_order_relaxed)) {
            HookLogImportant("NVAPI flip metering: SetFlipConfig on queue %p - %s - NVAPI_NOT_SUPPORTED, the "
                             "generator keeps its own pacing",
                             static_cast<void*>(commandQueue),
                             lowLatencyDevice ? "the frame-id markers cannot be observed"
                                              : "its device has no D3D12-on-Vulkan low-latency interface");
        }
        return kNvApiNotSupported;
    }
    Binding binding;
    bool publish = false;
    uint64_t replacedPending = 0;
    {
        std::lock_guard<std::mutex> lock(g_binderMutex);
        BatchBinder& binder = BinderForLocked(lowLatencyDevice);
        publish = binder.OnFlipConfig(request.framesPerBatch, binding);
        replacedPending = binder.ReplacedPending();
    }
    if (publish)
        PublishBinding(binding);

    // One line per batch-size change, with what happened in between.
    static ce::log_meter::ChangeGate s_configGate(10000);
    const uint64_t key = ce::log_meter::FieldKey(request.framesPerBatch, request.version, vidpnSrcId);
    if (const auto verdict = s_configGate.Observe(key, GetTickCount64())) {
        HookLogImportant("NVAPI flip metering: SetFlipConfig numFramesPerBatch=%u (struct v%u vidpnSrc=%u "
                         "device=%p, published=%llu undelivered=%llu unclaimed=%llu)%s",
                         request.framesPerBatch, request.version >> 16, vidpnSrcId, lowLatencyDevice,
                         static_cast<unsigned long long>(g_bindingsPublished.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_bindingsUndelivered.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(replacedPending),
                         ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
    }
    return kNvApiOk;
}

int32_t __cdecl Bridge_NgxGetDriverFeatureSupport(void* params) {
    const int32_t status = AnswerDriverFeatureSupport(params);
    static ce::log_meter::ChangeGate s_answerGate;
    if (const auto verdict = s_answerGate.Observe(static_cast<uint32_t>(status))) {
        HookLogImportant("NVAPI flip metering: NGX_GetDriverFeatureSupport answered in-process (status=%d; "
                         "SET_FLIP_CONFIG_V2 supported, every other feature unsupported)%s",
                         status, ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
    }
    return status;
}

bool IsDriverNvApiPath(const wchar_t* path) {
    // The driver installs NVAPI into System32/SysWOW64 (DriverStore lives under System32 as well).
    return wcsstr(path, L"\\system32\\") != nullptr || wcsstr(path, L"\\syswow64\\") != nullptr;
}

}  // namespace

bool IsTranslatedNvApiResident() {
#ifdef _WIN64
    const HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll");
#else
    const HMODULE nvapi = GetModuleHandleW(L"nvapi.dll");
#endif
    if (!nvapi)
        return false;
    static std::atomic<HMODULE> s_module{nullptr};
    static std::atomic<bool> s_translated{false};
    if (s_module.load(std::memory_order_acquire) == nvapi)
        return s_translated.load(std::memory_order_acquire);

    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nvapi, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return false;
    for (wchar_t* c = path; *c; ++c)
        *c = static_cast<wchar_t>(towlower(*c));
    const bool translated = !IsDriverNvApiPath(path);
    s_translated.store(translated, std::memory_order_release);
    if (s_module.exchange(nvapi, std::memory_order_acq_rel) != nvapi && translated) {
        HookLogImportant("NVAPI flip metering: resident NVAPI is not the driver's (%ls) - DLSS consumers' "
                         "nvapi_QueryInterface is routed through CE so a frame generator can reach flip metering",
                         path);
    }
    return translated;
}

void* MaybeAnswerQueryInterface(uint32_t functionId, void* resolved, const void* callerAddress) {
    if (functionId != kNvApiIdD3D12SetFlipConfig && functionId != kNvApiIdNgxGetDriverFeatureSupport)
        return nullptr;
    if (HookIsShuttingDown())
        return nullptr;
    const QueryAnswer answer = DecideQueryInterface(functionId, resolved != nullptr,
                                                    !resolved && ce::vulkan_layer_bridge::FlipMeteringBridgeAvailable());

    static ce::log_meter::StreamChangeGate<4> s_answerGate;
    if (const auto verdict = s_answerGate.Observe(functionId, static_cast<uint64_t>(answer))) {
        char caller[MAX_PATH] = {};
        ce::overlay_compat::TryGetModulePathFromCodeAddress(callerAddress, caller, sizeof(caller));
        const char* name =
            functionId == kNvApiIdD3D12SetFlipConfig ? "NvAPI_D3D12_SetFlipConfig" : "NvAPI_NGX_GetDriverFeatureSupport";
        const char* outcome = answer == QueryAnswer::kPassThrough ? "resident NVAPI implements it - passed through"
                              : answer == QueryAnswer::kDeclinedNoMeteringDevice
                                  ? "resident NVAPI has none and no Vulkan device can meter - declined"
                                  : "resident NVAPI has none - answered by CE's flip-metering bridge";
        HookLogImportant("NVAPI flip metering: %s requested by %s - %s%s", name, caller[0] ? caller : "unknown",
                         outcome, ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
    }

    switch (answer) {
        case QueryAnswer::kSetFlipConfig:
            return reinterpret_cast<void*>(&Bridge_SetFlipConfig);
        case QueryAnswer::kDriverFeatureSupport:
            return reinterpret_cast<void*>(&Bridge_NgxGetDriverFeatureSupport);
        default:
            return nullptr;
    }
}

}  // namespace ce::flip_metering_bridge
