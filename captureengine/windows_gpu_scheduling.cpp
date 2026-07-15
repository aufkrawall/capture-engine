#include "windows_gpu_scheduling.h"
#include "../common/secure_dll_loading.h"

#include <dxgi.h>
#include <algorithm>
#include <cstdio>

namespace ce::windows_gpu_scheduling {
namespace {

using KmtHandle = UINT;

struct KmtOpenAdapterFromLuid {
    LUID adapterLuid;
    KmtHandle adapter;
};

struct KmtCloseAdapter {
    KmtHandle adapter;
};

struct KmtQueryAdapterInfo {
    KmtHandle adapter;
    UINT type;
    void* privateData;
    UINT privateDataSize;
};

union Wddm27Caps {
    struct {
        UINT hwSchSupported : 1;
        UINT hwSchEnabled : 1;
        UINT hwSchEnabledByDefault : 1;
        UINT independentVidPnVSyncControl : 1;
        UINT reserved : 28;
    } bits;
    UINT value;
};

union Wddm29Caps {
    struct {
        UINT hwSchSupportState : 2;
        UINT hwSchEnabled : 1;
        UINT selfRefreshMemorySupported : 1;
        UINT reserved : 28;
    } bits;
    UINT value;
};

constexpr UINT kQueryWddm27Caps = 70;
constexpr UINT kQueryWddm29Caps = 75;

using OpenAdapterFn = LONG(WINAPI*)(KmtOpenAdapterFromLuid*);
using QueryAdapterInfoFn = LONG(WINAPI*)(KmtQueryAdapterInfo*);
using CloseAdapterFn = LONG(WINAPI*)(KmtCloseAdapter*);

struct RtlOsVersionInfo {
    ULONG size;
    ULONG majorVersion;
    ULONG minorVersion;
    ULONG buildNumber;
    ULONG platformId;
    WCHAR csdVersion[128];
};

uint32_t QueryWindowsBuild() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    using RtlGetVersionFn = LONG(WINAPI*)(RtlOsVersionInfo*);
    auto rtlGetVersion = ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
    if (!rtlGetVersion) {
        return 0;
    }
    RtlOsVersionInfo version{};
    version.size = sizeof(version);
    return rtlGetVersion(&version) >= 0 ? version.buildNumber : 0;
}

ce::gpu_scheduling::HagsSupportState DecodeSupportState(UINT value) {
    switch (value) {
        case 0:
            return ce::gpu_scheduling::HagsSupportState::kUnsupported;
        case 1:
            return ce::gpu_scheduling::HagsSupportState::kExperimental;
        case 2:
            return ce::gpu_scheduling::HagsSupportState::kStable;
        case 3:
            return ce::gpu_scheduling::HagsSupportState::kAlwaysOn;
        default:
            return ce::gpu_scheduling::HagsSupportState::kUnknown;
    }
}

bool ResolveAdapter(const LUID& luid, IDXGIAdapter** outAdapter) {
    if (!outAdapter) {
        return false;
    }
    *outAdapter = nullptr;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) {
        return false;
    }
    for (UINT index = 0;; ++index) {
        IDXGIAdapter* adapter = nullptr;
        const HRESULT hr = factory->EnumAdapters(index, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(hr) || !adapter) {
            continue;
        }
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(adapter->GetDesc(&desc)) && SameLuid(desc.AdapterLuid, luid)) {
            *outAdapter = adapter;
            factory->Release();
            return true;
        }
        adapter->Release();
    }
    factory->Release();
    return false;
}

}  // namespace

static_assert(sizeof(Wddm27Caps) == sizeof(UINT));
static_assert(sizeof(Wddm29Caps) == sizeof(UINT));

bool SameLuid(const LUID& lhs, const LUID& rhs) {
    return lhs.LowPart == rhs.LowPart && lhs.HighPart == rhs.HighPart;
}

std::string FormatLuid(const LUID& luid) {
    char text[32]{};
    std::snprintf(text, sizeof(text), "%08lX:%08lX", static_cast<unsigned long>(luid.HighPart),
                  static_cast<unsigned long>(luid.LowPart));
    return text;
}

bool GetAdapterLuid(ID3D11Device* device, LUID& luid) {
    luid = {};
    if (!device) {
        return false;
    }
    IDXGIDevice* dxgiDevice = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || !dxgiDevice) {
        return false;
    }
    IDXGIAdapter* adapter = nullptr;
    const HRESULT hr = dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();
    if (FAILED(hr) || !adapter) {
        return false;
    }
    DXGI_ADAPTER_DESC desc{};
    const bool ok = SUCCEEDED(adapter->GetDesc(&desc));
    adapter->Release();
    if (ok) {
        luid = desc.AdapterLuid;
    }
    return ok;
}

bool QueryAdapterSchedulingEnvironment(ID3D11Device* device, AdapterSchedulingEnvironment& environment) {
    LUID luid{};
    return GetAdapterLuid(device, luid) && QueryAdapterSchedulingEnvironment(luid, environment);
}

bool QueryAdapterSchedulingEnvironment(const LUID& luid, AdapterSchedulingEnvironment& environment) {
    environment = {};
    environment.luid = luid;
    environment.windowsBuild = QueryWindowsBuild();

    IDXGIAdapter* adapter = nullptr;
    if (ResolveAdapter(luid, &adapter) && adapter) {
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(adapter->GetDesc(&desc))) {
            environment.description = desc.Description;
            environment.vendorId = desc.VendorId;
            environment.deviceId = desc.DeviceId;
        }
        LARGE_INTEGER driverVersion{};
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driverVersion))) {
            environment.driverVersion = static_cast<uint64_t>(driverVersion.QuadPart);
        }
        adapter->Release();
    }

    HMODULE gdi32 = GetModuleHandleW(L"gdi32.dll");
    if (!gdi32) {
        gdi32 = ce::security::LoadSystemLibrary(L"gdi32.dll");
    }
    auto openAdapter =
        gdi32 ? reinterpret_cast<OpenAdapterFn>(GetProcAddress(gdi32, "D3DKMTOpenAdapterFromLuid")) : nullptr;
    auto queryAdapter =
        gdi32 ? reinterpret_cast<QueryAdapterInfoFn>(GetProcAddress(gdi32, "D3DKMTQueryAdapterInfo")) : nullptr;
    auto closeAdapter = gdi32 ? reinterpret_cast<CloseAdapterFn>(GetProcAddress(gdi32, "D3DKMTCloseAdapter")) : nullptr;
    if (!openAdapter || !queryAdapter || !closeAdapter) {
        environment.openStatus = static_cast<LONG>(ERROR_PROC_NOT_FOUND);
        return false;
    }

    KmtOpenAdapterFromLuid open{};
    open.adapterLuid = luid;
    environment.openStatus = openAdapter(&open);
    if (environment.openStatus < 0 || open.adapter == 0) {
        return false;
    }

    Wddm27Caps caps27{};
    KmtQueryAdapterInfo query{};
    query.adapter = open.adapter;
    query.type = kQueryWddm27Caps;
    query.privateData = &caps27;
    query.privateDataSize = sizeof(caps27);
    environment.caps27Status = queryAdapter(&query);
    if (environment.caps27Status >= 0) {
        environment.hags.querySucceeded = true;
        environment.hags.supported = caps27.bits.hwSchSupported != 0;
        environment.hags.enabled = caps27.bits.hwSchEnabled != 0;
        environment.hags.enabledByDefault = caps27.bits.hwSchEnabledByDefault != 0;
        environment.hags.supportState = environment.hags.supported ? ce::gpu_scheduling::HagsSupportState::kStable
                                                                   : ce::gpu_scheduling::HagsSupportState::kUnsupported;

        Wddm29Caps caps29{};
        query.type = kQueryWddm29Caps;
        query.privateData = &caps29;
        query.privateDataSize = sizeof(caps29);
        environment.caps29Status = queryAdapter(&query);
        if (environment.caps29Status >= 0) {
            environment.hags.enabled = caps29.bits.hwSchEnabled != 0;
            environment.hags.supportState = DecodeSupportState(caps29.bits.hwSchSupportState);
            environment.hags.supported =
                environment.hags.supportState != ce::gpu_scheduling::HagsSupportState::kUnsupported;
        }
    }

    KmtCloseAdapter close{open.adapter};
    environment.closeStatus = closeAdapter(&close);
    return environment.hags.querySucceeded;
}

}  // namespace ce::windows_gpu_scheduling
