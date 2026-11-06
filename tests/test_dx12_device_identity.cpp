#include <gtest/gtest.h>

#include "hook/d3d12/dx12_device_identity.h"

#include <cstring>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace {

using ce::dx12_device_identity::Compare;
using ce::dx12_device_identity::IsOneDevice;
using ce::dx12_device_identity::kProbeGuid;
using ce::dx12_device_identity::Relation;

// A device's private data store: what every view of one device shares.
struct DeviceStore {
    std::vector<uint8_t> probe;
    int writes = 0;
    DeviceStore* matchingStaleCopy = nullptr;
};

// One interface of a device: COM identity of its own, private data forwarded to the device's store.
class DeviceView final : public ID3D12Object {
public:
    explicit DeviceView(std::shared_ptr<DeviceStore> store) : store_(std::move(store)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (refuseIdentity)
            return E_NOINTERFACE;
        if (id == __uuidof(IUnknown) || id == __uuidof(ID3D12Object)) {
            *out = static_cast<ID3D12Object*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { return --references_; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID id, UINT* size, void* data) override {
        if (id != kProbeGuid || store_->probe.empty() || !size || *size < store_->probe.size())
            return DXGI_ERROR_NOT_FOUND;
        *size = static_cast<UINT>(store_->probe.size());
        std::memcpy(data, store_->probe.data(), store_->probe.size());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID id, UINT size, const void* data) override {
        if (refusePrivateData || id != kProbeGuid)
            return E_FAIL;
        ++store_->writes;
        if (size == 0) {
            store_->probe.clear();
        } else {
            const auto* bytes = static_cast<const uint8_t*>(data);
            store_->probe.assign(bytes, bytes + size);
            if (store_->matchingStaleCopy) {
                store_->matchingStaleCopy->probe = store_->probe;
                store_->matchingStaleCopy = nullptr;
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return E_NOTIMPL; }

    bool refuseIdentity = false;
    bool refusePrivateData = false;

private:
    std::shared_ptr<DeviceStore> store_;
    ULONG references_ = 1;
};

TEST(DX12DeviceIdentityTest, OneInterfaceIsOneObjectWithoutTouchingTheDevice) {
    const auto store = std::make_shared<DeviceStore>();
    DeviceView view(store);
    EXPECT_EQ(Compare(&view, &view), Relation::kSameObject);
    EXPECT_EQ(store->writes, 0);
}

// W3 with Streamline, ReShade, OptiScaler and Steam (session 20261106_195839): the back buffer and the queue gave
// two interfaces of one device, and comparing identity alone failed every in-game screenshot.
TEST(DX12DeviceIdentityTest, TwoViewsOfOneDeviceAreOneDevice) {
    const auto store = std::make_shared<DeviceStore>();
    DeviceView native(store);
    DeviceView interposed(store);
    const Relation relation = Compare(&native, &interposed);
    EXPECT_EQ(relation, Relation::kSameDeviceOtherView);
    EXPECT_TRUE(IsOneDevice(relation));
    EXPECT_TRUE(store->probe.empty()) << "the probe must not stay on the game's device";
}

TEST(DX12DeviceIdentityTest, ViewsWhoseIdentityQueryFailsAreStillJudgedByTheirDevice) {
    const auto store = std::make_shared<DeviceStore>();
    DeviceView native(store);
    DeviceView interposed(store);
    interposed.refuseIdentity = true;
    EXPECT_EQ(Compare(&native, &interposed), Relation::kSameDeviceOtherView);
}

TEST(DX12DeviceIdentityTest, TwoDevicesAreNotOneDevice) {
    DeviceView first(std::make_shared<DeviceStore>());
    DeviceView second(std::make_shared<DeviceStore>());
    const Relation relation = Compare(&first, &second);
    EXPECT_EQ(relation, Relation::kDifferentDevices);
    EXPECT_FALSE(IsOneDevice(relation));
}

TEST(DX12DeviceIdentityTest, AStaleProbeOnTheOtherDeviceProvesNothing) {
    const auto firstStore = std::make_shared<DeviceStore>();
    const auto staleStore = std::make_shared<DeviceStore>();
    // Force the other store's stale value to collide with the first challenge, independently of test order.
    firstStore->matchingStaleCopy = staleStore.get();
    DeviceView first(firstStore);
    DeviceView second(staleStore);
    EXPECT_EQ(Compare(&first, &second), Relation::kDifferentDevices);
    EXPECT_EQ(Compare(&first, &second), Relation::kDifferentDevices);
    EXPECT_TRUE(firstStore->probe.empty());
    EXPECT_FALSE(staleStore->probe.empty()) << "the other device's store is never modified by the comparison";
}

TEST(DX12DeviceIdentityTest, AnUnprovableComparisonIsNotOneDevice) {
    const auto store = std::make_shared<DeviceStore>();
    DeviceView left(store);
    DeviceView right(store);
    left.refusePrivateData = true;
    EXPECT_EQ(Compare(&left, &right), Relation::kUnknown);
    EXPECT_FALSE(IsOneDevice(Relation::kUnknown));
    EXPECT_EQ(Compare(nullptr, &right), Relation::kUnknown);
    EXPECT_EQ(Compare(&left, nullptr), Relation::kUnknown);
}

}  // namespace
