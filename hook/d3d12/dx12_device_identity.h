#pragma once

// Whether two ID3D12Device interfaces lead to one device. An interposer can hand out its own device interface for a
// device it wraps, so comparing COM identity alone calls one device two (W3 with its overlay stack, session
// 20261106_195839: every in-game screenshot failed over to desktop capture). A device's private data belongs to the
// device, and a view forwards it, so a value written through one interface and read back through the other proves they
// lead to one device; two real devices keep separate stores. A view that keeps private data to itself reads as
// different devices, which is the safe side: recording work against another device's resource removes the device.

#include <d3d12.h>
#include <guiddef.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <mutex>

namespace ce::dx12_device_identity {

enum class Relation {
    kSameObject,           // one COM object
    kSameDeviceOtherView,  // two interfaces of one device
    kDifferentDevices,     // no evidence they share a device
    kUnknown,              // an interface is missing or refused the probe
};

inline bool IsOneDevice(Relation relation) {
    return relation == Relation::kSameObject || relation == Relation::kSameDeviceOtherView;
}

inline const char* Describe(Relation relation) {
    switch (relation) {
        case Relation::kSameObject:
            return "same object";
        case Relation::kSameDeviceOtherView:
            return "same device through another view";
        case Relation::kDifferentDevices:
            return "different devices";
        case Relation::kUnknown:
            return "unknown";
    }
    return "invalid";
}

// {6F2C7A44-3B19-4E7D-9A51-0D3C8E6B21F7}
inline constexpr GUID kProbeGuid = {0x6f2c7a44, 0x3b19, 0x4e7d, {0x9a, 0x51, 0x0d, 0x3c, 0x8e, 0x6b, 0x21, 0xf7}};

inline Relation Compare(ID3D12Object* left, ID3D12Object* right) {
    if (!left || !right)
        return Relation::kUnknown;
    Microsoft::WRL::ComPtr<IUnknown> leftIdentity;
    Microsoft::WRL::ComPtr<IUnknown> rightIdentity;
    if (SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&leftIdentity))) &&
        SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&rightIdentity))) && leftIdentity.Get() == rightIdentity.Get()) {
        return Relation::kSameObject;
    }

    // Only reached when the identities differ, which is once per screenshot request at most: never on a frame path.
    static std::mutex probeMutex;
    static std::atomic<uint64_t> nextProbe{1};
    std::lock_guard<std::mutex> lock(probeMutex);
    const uint64_t written = nextProbe.fetch_add(1, std::memory_order_relaxed);
    if (FAILED(left->SetPrivateData(kProbeGuid, sizeof(written), &written)))
        return Relation::kUnknown;
    uint64_t read = 0;
    UINT readSize = sizeof(read);
    const HRESULT readResult = right->GetPrivateData(kProbeGuid, &readSize, &read);
    left->SetPrivateData(kProbeGuid, 0, nullptr);
    return SUCCEEDED(readResult) && readSize == sizeof(read) && read == written ? Relation::kSameDeviceOtherView
                                                                                : Relation::kDifferentDevices;
}

}  // namespace ce::dx12_device_identity
