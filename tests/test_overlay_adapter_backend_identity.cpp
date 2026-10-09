// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include "hook/overlay/custom_overlay_dx12.h"
#include "hook/overlay/overlay_adapter.h"

namespace {

struct BackendEvents {
    int initialized = 0;
    int shutDown = 0;
    int destroyed = 0;
    int uploadSlot = -1;
};

// A custom DX12 renderer shares the API label, but none of the texture
// renderer's layout. Spare storage makes accidental non-virtual writes
// observable without corrupting the test process.
class CustomDX12Backend final : public CustomOverlay::RendererBackend {
public:
    explicit CustomDX12Backend(BackendEvents& events) : events_(events) {}
    ~CustomDX12Backend() override {
        ++events_.destroyed;
    }
    bool Initialize(int, int, const uint8_t*) override {
        ++events_.initialized;
        return true;
    }
    void Shutdown() override {
        ++events_.shutDown;
    }
    void SetNextUploadSlot(int slot) override {
        events_.uploadSlot = slot;
    }
    void Render(const std::vector<CustomOverlay::DrawVertex>&, const std::vector<uint16_t>&,
                const std::vector<CustomOverlay::DrawCommand>&, int, int) override {}

    std::array<uint8_t, sizeof(CustomOverlay::DX12Backend)> storage{};

private:
    BackendEvents& events_;
};

TEST(OverlayAdapterBackendIdentityTest, CustomDX12DoesNotReceiveTextureBackendWrites) {
    BackendEvents events;
    OverlayAdapter adapter;
    auto* backend = new CustomDX12Backend(events);
    ASSERT_TRUE(adapter.InitCustom(backend));
    EXPECT_EQ(adapter.GetBackendType(), OverlayBackendType::DX12);
    EXPECT_EQ(events.initialized, 1);

    adapter.SetDX12RenderTarget(reinterpret_cast<void*>(0x1234), reinterpret_cast<void*>(0x5678));
    adapter.SetDX12UploadSlotFence(nullptr, 17);
    const bool untouched = std::all_of(backend->storage.begin(), backend->storage.end(),
                                       [](uint8_t value) { return value == 0; });
    // Keep pre-fix teardown safe even after an invalid texture-backend write.
    backend->storage.fill(0);
    EXPECT_TRUE(untouched) << "the DX12 API label does not prove the concrete renderer type";
    EXPECT_FALSE(adapter.HasPendingDX12Resources());
    EXPECT_FALSE(adapter.PrimeDX12Resources(nullptr));
    adapter.SetDX12NextUploadSlot(3);
    EXPECT_EQ(events.uploadSlot, 3);
    adapter.Shutdown();
    EXPECT_EQ(events.shutDown, 1);
    EXPECT_EQ(events.destroyed, 1);
    EXPECT_FALSE(adapter.IsInitialized());
}

TEST(OverlayAdapterBackendIdentityTest, CustomDX12ShutdownAndRebindDoNotReadTextureStorage) {
    BackendEvents events;
    OverlayAdapter adapter;
    for (int iteration = 0; iteration < 3; ++iteration) {
        auto* backend = new CustomDX12Backend(events);
        backend->storage.fill(0xFF);
        ASSERT_TRUE(adapter.InitCustom(backend));
        EXPECT_FALSE(adapter.HasPendingDX12Resources());
        EXPECT_FALSE(adapter.PrimeDX12Resources(nullptr));
        adapter.Shutdown();
        EXPECT_EQ(adapter.GetBackend(), nullptr);
        EXPECT_EQ(events.shutDown, iteration + 1);
        EXPECT_EQ(events.destroyed, iteration + 1);
    }
}

TEST(OverlayAdapterBackendIdentityTest, TextureDX12AdvertisesItsOwnConcreteInterface) {
    CustomOverlay::DX12Backend backend(nullptr, nullptr, DXGI_FORMAT_R8G8B8A8_UNORM);
    CustomOverlay::RendererBackend* base = &backend;
    EXPECT_EQ(base->AsTextureDX12Backend(), &backend);
}

}  // namespace
