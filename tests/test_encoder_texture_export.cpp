// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include <gtest/gtest.h>

#include <windows.h>
#include <d3d11_4.h>

#include <cstdint>
#include <memory>

#include "common/capture/recording_memory.h"
#include "common/ipc/inject_transport_snapshot.h"
#include "common/ipc/shared_defs.h"
#include "common/platform/raii_helpers.h"
#include "mediaengine/video/encoder_texture_export.h"
#include "warp_adapter.h"

using ce::media::EncoderTextureExport;
using ce::media::EncoderTextureRequest;
using ce::media::ExportedTextureViews;

namespace {

ce::ComGuard<ID3D11Device> CreateWarpDevice(ce::ComGuard<ID3D11DeviceContext>& context) {
    ce::ComGuard<ID3D11Device> device;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                 device.put(), nullptr, context.put())))
        device.reset();
    return device;
}

class EncoderTextureExportTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(WarpAdapterLuid(luid_));
        encoderDevice_ = CreateWarpDevice(encoderContext_);
        ASSERT_TRUE(encoderDevice_);
    }

    EncoderTextureRequest Request(uint32_t format = DXGI_FORMAT_B8G8R8A8_UNORM) const {
        return {64, 32, format, static_cast<int32_t>(luid_.LowPart), luid_.HighPart};
    }

    D3D11_TEXTURE2D_DESC DescOf(ID3D11Texture2D* texture) const {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        return desc;
    }

    LUID luid_{};
    std::unique_ptr<SharedMemoryLayout> layout_ = std::make_unique<SharedMemoryLayout>();
    ce::RecordingMemory route_;
    ce::ComGuard<ID3D11DeviceContext> encoderContext_;
    ce::ComGuard<ID3D11Device> encoderDevice_;
};

}  // namespace

TEST_F(EncoderTextureExportTest, EnsurePublishesACompleteSetOnTheBinding) {
    EncoderTextureExport exported(route_);
    ASSERT_TRUE(exported.Ensure(Request(), layout_.get()));
    EXPECT_EQ(route_.get(), layout_.get());
    EXPECT_TRUE(exported.Exported());

    const auto& published = layout_->encoderTextures;
    EXPECT_TRUE(published.kmtReady.load());
    EXPECT_TRUE(published.ready.load());
    EXPECT_EQ(published.GetWidth(), 64u);
    EXPECT_EQ(published.GetHeight(), 32u);
    EXPECT_EQ(published.GetFormat(), static_cast<uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM));
    EXPECT_EQ(published.GetFenceHandle(), 0u);
    EXPECT_EQ(published.GetKmtAdapterKey(), (static_cast<uint64_t>(static_cast<uint32_t>(luid_.HighPart)) << 32) |
                                               luid_.LowPart);
    const EncoderTextureExport::Handles snapshot = exported.Snapshot();
    EXPECT_NE(snapshot.generation, 0u);
    for (int i = 0; i < ENCODER_TEXTURE_SLOT_COUNT; ++i) {
        ASSERT_NE(snapshot.kmt[i], nullptr) << "slot " << i;
        EXPECT_EQ(published.GetKmtTextureHandle(i), reinterpret_cast<uint64_t>(snapshot.kmt[i])) << "slot " << i;
    }
}

TEST_F(EncoderTextureExportTest, ARepeatedRequestKeepsTheSetAndAdvertisesItAgain) {
    EncoderTextureExport exported(route_);
    ASSERT_TRUE(exported.Ensure(Request(), layout_.get()));
    const EncoderTextureExport::Handles first = exported.Snapshot();
    layout_->encoderTextures.kmtReady.store(false);
    layout_->encoderTextures.ready.store(false);

    ASSERT_TRUE(exported.Ensure(Request(), layout_.get()));
    const EncoderTextureExport::Handles second = exported.Snapshot();
    EXPECT_EQ(second.generation, first.generation);
    EXPECT_EQ(second.kmt[0], first.kmt[0]);
    // True means the route carries the set, even after its publication was withdrawn.
    EXPECT_TRUE(layout_->encoderTextures.kmtReady.load());
    EXPECT_TRUE(layout_->encoderTextures.ready.load());
    EXPECT_GT(exported.GpuBytes(), 0u);
}

TEST_F(EncoderTextureExportTest, ARepeatedRequestCannotOverwriteAnAdoptedProducerFence) {
    EncoderTextureExport exported(route_);
    ASSERT_TRUE(exported.Ensure(Request(), layout_.get()));
    ce::AdoptEncoderTexturesWithFence(*layout_, 0x12345678ull);
    EXPECT_EQ(layout_->encoderTextures.GetFenceHandle(), 0x12345678ull);
    EXPECT_TRUE(layout_->useEncoderTextures.load());

    layout_->encoderTextures.kmtReady.store(false);
    layout_->encoderTextures.ready.store(false);
    ASSERT_TRUE(exported.Ensure(Request(), layout_.get()));
    EXPECT_EQ(layout_->encoderTextures.GetFenceHandle(), 0x12345678ull);
}

TEST_F(EncoderTextureExportTest, AFormatChangeReplacesTheSetAndTheEncoderViewsFollow) {
    EncoderTextureExport exported(route_);
    ExportedTextureViews views;
    ASSERT_TRUE(exported.Ensure(Request(), layout_.get()));
    const uint64_t first = exported.Generation();
    {
        ce::ComGuard<ID3D11Texture2D> view(views.AcquireSlot(exported, encoderDevice_.get(), 0));
        ASSERT_TRUE(view);
        EXPECT_EQ(DescOf(view.get()).Format, DXGI_FORMAT_B8G8R8A8_UNORM);
    }

    ASSERT_TRUE(exported.Ensure(Request(DXGI_FORMAT_R10G10B10A2_UNORM), layout_.get()));
    EXPECT_GT(exported.Generation(), first);
    EXPECT_EQ(layout_->encoderTextures.GetFormat(), static_cast<uint32_t>(DXGI_FORMAT_R10G10B10A2_UNORM));
    EXPECT_TRUE(layout_->encoderTextures.kmtReady.load());
    ce::ComGuard<ID3D11Texture2D> view(views.AcquireSlot(exported, encoderDevice_.get(), 0));
    ASSERT_TRUE(view);
    EXPECT_EQ(DescOf(view.get()).Format, DXGI_FORMAT_R10G10B10A2_UNORM);
}

TEST_F(EncoderTextureExportTest, AResizeReplacesTheSetWhileAnAcquiredViewStaysValid) {
    EncoderTextureExport exported(route_);
    ExportedTextureViews views;
    EncoderTextureRequest request = Request();
    ASSERT_TRUE(exported.Ensure(request, layout_.get()));
    const uint64_t first = exported.Generation();
    ce::ComGuard<ID3D11Texture2D> oldView(views.AcquireSlot(exported, encoderDevice_.get(), 0));
    ASSERT_TRUE(oldView);

    request.width = 80;
    request.height = 48;
    ASSERT_TRUE(exported.Ensure(request, layout_.get()));
    EXPECT_GT(exported.Generation(), first);
    EXPECT_EQ(layout_->encoderTextures.GetWidth(), 80u);
    EXPECT_EQ(layout_->encoderTextures.GetHeight(), 48u);
    ce::ComGuard<ID3D11Texture2D> view(views.AcquireSlot(exported, encoderDevice_.get(), 0));
    ASSERT_TRUE(view);
    EXPECT_EQ(DescOf(view.get()).Width, 80u);
    EXPECT_EQ(DescOf(view.get()).Height, 48u);
    EXPECT_EQ(DescOf(oldView.get()).Width, 64u);
    EXPECT_EQ(DescOf(oldView.get()).Height, 32u);
}

TEST_F(EncoderTextureExportTest, ReleaseWithdrawsThePublicationAndLeavesNothingToOpen) {
    EncoderTextureExport exported(route_);
    ExportedTextureViews views;
    ASSERT_TRUE(exported.Ensure(Request(), layout_.get()));
    const HANDLE handle = exported.Snapshot().kmt[3];
    {
        int slot = -1;
        ce::ComGuard<ID3D11Texture2D> view(views.Acquire(exported, encoderDevice_.get(), handle, &slot));
        ASSERT_TRUE(view);
        EXPECT_EQ(slot, 3);
    }

    EXPECT_TRUE(exported.Release("test"));
    EXPECT_FALSE(exported.Exported());
    EXPECT_FALSE(layout_->encoderTextures.kmtReady.load());
    EXPECT_FALSE(layout_->encoderTextures.ready.load());
    EXPECT_EQ(views.Acquire(exported, encoderDevice_.get(), handle), nullptr);
    EXPECT_EQ(views.AcquireSlot(exported, encoderDevice_.get(), 3), nullptr);
    EXPECT_FALSE(exported.Release("nothing left"));
}

TEST_F(EncoderTextureExportTest, ViewsResolveOnlyExportedHandlesOnTheEncoderDevice) {
    EncoderTextureExport exported(route_);
    ExportedTextureViews views;
    ASSERT_TRUE(exported.Ensure(Request(), layout_.get()));
    EXPECT_EQ(views.Acquire(exported, encoderDevice_.get(), reinterpret_cast<HANDLE>(0x1234)), nullptr);
    EXPECT_EQ(views.AcquireSlot(exported, encoderDevice_.get(), ENCODER_TEXTURE_SLOT_COUNT), nullptr);

    ce::ComGuard<ID3D11Texture2D> view(views.AcquireSlot(exported, encoderDevice_.get(), 7));
    ASSERT_TRUE(view);
    ce::ComGuard<ID3D11Device> owner;
    view->GetDevice(owner.put());
    EXPECT_EQ(owner.get(), encoderDevice_.get()) << "the encoder must use a view on its own device";
    EXPECT_EQ(DescOf(view.get()).Width, 64u);
    EXPECT_EQ(DescOf(view.get()).Height, 32u);
}

// The zero-copy property: what a producer renders through an exported handle is what the
// encoder reads through its view, with no copy in between.
TEST_F(EncoderTextureExportTest, TheEncoderReadsWhatAProducerRenderedIntoTheExportedSlot) {
    EncoderTextureExport exported(route_);
    ExportedTextureViews views;
    ASSERT_TRUE(exported.Ensure(Request(), layout_.get()));
    const HANDLE handle = exported.Snapshot().kmt[5];

    ce::ComGuard<ID3D11DeviceContext> producerContext;
    ce::ComGuard<ID3D11Device> producer = CreateWarpDevice(producerContext);
    ASSERT_TRUE(producer);
    {
        ce::ComGuard<ID3D11Texture2D> target;
        ASSERT_TRUE(SUCCEEDED(producer->OpenSharedResource(handle, IID_PPV_ARGS(target.put()))));
        ce::ComGuard<ID3D11RenderTargetView> rtv;
        ASSERT_TRUE(SUCCEEDED(producer->CreateRenderTargetView(target.get(), nullptr, rtv.put())));
        ce::ComGuard<ID3D11Device5> device5;
        ce::ComGuard<ID3D11DeviceContext4> context4;
        ASSERT_TRUE(SUCCEEDED(producer->QueryInterface(IID_PPV_ARGS(device5.put()))));
        ASSERT_TRUE(SUCCEEDED(producerContext->QueryInterface(IID_PPV_ARGS(context4.put()))));
        ce::ComGuard<ID3D11Fence> fence;
        ASSERT_TRUE(SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put()))));
        ce::HandleGuard completed(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        ASSERT_TRUE(completed);
        ASSERT_TRUE(SUCCEEDED(fence->SetEventOnCompletion(1, completed.get())));
        const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
        producerContext->ClearRenderTargetView(rtv.get(), red);
        ASSERT_TRUE(SUCCEEDED(context4->Signal(fence.get(), 1)));
        producerContext->Flush();
        ASSERT_EQ(WaitForSingleObject(completed.get(), 5000), WAIT_OBJECT_0);
    }

    ce::ComGuard<ID3D11Texture2D> view(views.Acquire(exported, encoderDevice_.get(), handle));
    ASSERT_TRUE(view);
    D3D11_TEXTURE2D_DESC stagingDesc = DescOf(view.get());
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.MiscFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ce::ComGuard<ID3D11Texture2D> staging;
    ASSERT_TRUE(SUCCEEDED(encoderDevice_->CreateTexture2D(&stagingDesc, nullptr, staging.put())));
    encoderContext_->CopyResource(staging.get(), view.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    ASSERT_TRUE(SUCCEEDED(encoderContext_->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)));
    const auto* pixel = static_cast<const uint8_t*>(mapped.pData);
    const uint8_t b = pixel[0], g = pixel[1], r = pixel[2], a = pixel[3];
    encoderContext_->Unmap(staging.get(), 0);
    EXPECT_EQ(b, 0);
    EXPECT_EQ(g, 0);
    EXPECT_EQ(r, 255);
    EXPECT_EQ(a, 255);
}

TEST_F(EncoderTextureExportTest, AnAdapterThatIsNotPresentExportsNothing) {
    EncoderTextureExport exported(route_);
    EncoderTextureRequest request = Request();
    request.luidLow = 0x7ffffff0;
    request.luidHigh = 0x7ffffff0;
    EXPECT_FALSE(exported.Ensure(request, layout_.get()));
    EXPECT_FALSE(exported.Exported());
    EXPECT_EQ(exported.Generation(), 0u);
    EXPECT_FALSE(layout_->encoderTextures.kmtReady.load());
}

TEST_F(EncoderTextureExportTest, AnAdapterChangeCannotReuseTexturesFromThePreviousAdapter) {
    EncoderTextureExport exported(route_);
    EncoderTextureRequest request = Request();
    ASSERT_TRUE(exported.Ensure(request, layout_.get()));
    const auto first = exported.Snapshot();
    request.luidLow = 0x7ffffff0;
    request.luidHigh = 0x7ffffff0;

    EXPECT_FALSE(exported.Ensure(request, layout_.get()));
    EXPECT_FALSE(exported.Exported());
    EXPECT_FALSE(layout_->encoderTextures.kmtReady.load());
    EXPECT_FALSE(layout_->encoderTextures.ready.load());
    EXPECT_GT(exported.Generation(), first.generation);
    ce::ComGuard<ID3D11Texture2D> retired(exported.OpenOn(encoderDevice_.get(), first.generation, 0));
    EXPECT_FALSE(retired);
    ASSERT_TRUE(exported.Ensure(Request(), layout_.get()));
    EXPECT_TRUE(layout_->encoderTextures.kmtReady.load());
}

// Late texture creation after a stop must not advertise handles on the live mapping that
// the next recording already owns.
TEST_F(EncoderTextureExportTest, ADetachedRecordingPublishesPrivately) {
    auto detached = std::make_unique<SharedMemoryLayout>();
    route_.Retarget(layout_.get(), nullptr);
    route_.Detach(*layout_, *detached);

    EncoderTextureExport exported(route_);
    ASSERT_TRUE(exported.Ensure(Request(), layout_.get()));
    EXPECT_FALSE(layout_->encoderTextures.kmtReady.load());
    EXPECT_TRUE(detached->encoderTextures.kmtReady.load());
    EXPECT_EQ(route_.get(), detached.get());
}
