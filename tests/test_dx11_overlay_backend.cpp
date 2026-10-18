#include <gtest/gtest.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

#include "hook/overlay/custom_overlay_dx11.h"

// The D3D11 overlay backend on a real WARP device. It binds the overlay's target itself, inside its
// save and restore of the application's state: the D3D11 hook used to bind it first and restore only
// the first render target, unbinding the others. A cached frame draws the geometry the dynamic
// buffers still hold instead of writing it again (each Map is a call through DXUP's translation).
namespace {
using Microsoft::WRL::ComPtr;
using CustomOverlay::DrawCommand;
using CustomOverlay::DrawVertex;
using CustomOverlay::DX11Backend;

constexpr UINT kSize = 16;
constexpr uint32_t kRed = 0xFF0000FF;  // R8G8B8A8: R in the low byte

struct Target {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> view;
};

class DX11OverlayBackendTest : public testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                    &device_, nullptr, &context_),
                  S_OK);
        backend_ = std::make_unique<DX11Backend>(device_.Get(), context_.Get());
        const std::vector<uint8_t> font(4 * 4 * 4, 0xFF);
        ASSERT_TRUE(backend_->Initialize(4, 4, font.data()));
        // One solid red quad over the top-left 8x8 pixels.
        vertices_ = {{0, 0, 0, 0, kRed}, {8, 0, 0, 0, kRed}, {8, 8, 0, 0, kRed}, {0, 8, 0, 0, kRed}};
        indices_ = {0, 1, 2, 0, 2, 3};
        DrawCommand command = {};
        command.vertexCount = 4;
        command.indexCount = 6;
        command.useTexture = false;
        commands_ = {command};
    }
    void TearDown() override {
        if (backend_)
            backend_->Shutdown();
    }

    Target MakeTarget() {
        Target target;
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = kSize;
        desc.Height = kSize;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        EXPECT_EQ(device_->CreateTexture2D(&desc, nullptr, &target.texture), S_OK);
        EXPECT_EQ(device_->CreateRenderTargetView(target.texture.Get(), nullptr, &target.view), S_OK);
        const float clear[4] = {0, 0, 0, 0};
        context_->ClearRenderTargetView(target.view.Get(), clear);
        return target;
    }
    uint32_t Pixel(ID3D11Texture2D* texture, UINT x, UINT y) {
        D3D11_TEXTURE2D_DESC desc = {};
        texture->GetDesc(&desc);
        desc.BindFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging)))
            return 0xDEADBEEF;
        context_->CopyResource(staging.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return 0xDEADBEEF;
        const uint32_t value =
            *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch + x * 4);
        context_->Unmap(staging.Get(), 0);
        return value;
    }
    void Render() {
        backend_->Render(vertices_, indices_, commands_, kSize, kSize);
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    std::unique_ptr<DX11Backend> backend_;
    std::vector<DrawVertex> vertices_;
    std::vector<uint16_t> indices_;
    std::vector<DrawCommand> commands_;
};

TEST_F(DX11OverlayBackendTest, DrawsIntoItsTargetAndRestoresEveryApplicationBinding) {
    Target overlay = MakeTarget();
    Target game0 = MakeTarget();
    Target game1 = MakeTarget();
    D3D11_TEXTURE2D_DESC depthDesc = {};
    depthDesc.Width = kSize;
    depthDesc.Height = kSize;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11DepthStencilView> depthView;
    ASSERT_EQ(device_->CreateTexture2D(&depthDesc, nullptr, &depth), S_OK);
    ASSERT_EQ(device_->CreateDepthStencilView(depth.Get(), nullptr, &depthView), S_OK);

    // The application's multiple-render-target binding and two viewports at Present.
    ID3D11RenderTargetView* gameViews[2] = {game0.view.Get(), game1.view.Get()};
    context_->OMSetRenderTargets(2, gameViews, depthView.Get());
    const D3D11_VIEWPORT gameViewports[2] = {{0, 0, 4, 4, 0, 1}, {4, 4, 4, 4, 0, 1}};
    context_->RSSetViewports(2, gameViewports);

    backend_->SetExternalRenderTarget(overlay.view.Get());
    Render();
    backend_->SetExternalRenderTarget(nullptr);

    EXPECT_EQ(Pixel(overlay.texture.Get(), 1, 1), kRed) << "the overlay draws into the target it was given";
    EXPECT_EQ(Pixel(overlay.texture.Get(), 12, 12), 0u);
    EXPECT_EQ(Pixel(game0.texture.Get(), 1, 1), 0u) << "not into the application's bound target";

    ID3D11RenderTargetView* bound[8] = {};
    ComPtr<ID3D11DepthStencilView> boundDepth;
    context_->OMGetRenderTargets(8, bound, &boundDepth);
    EXPECT_EQ(bound[0], game0.view.Get());
    EXPECT_EQ(bound[1], game1.view.Get()) << "the second render target survives the overlay";
    EXPECT_EQ(boundDepth.Get(), depthView.Get());
    for (ID3D11RenderTargetView* view : bound) {
        if (view)
            view->Release();
    }
    UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    context_->RSGetViewports(&viewportCount, viewports);
    ASSERT_EQ(viewportCount, 2u);
    EXPECT_EQ(viewports[1].TopLeftX, 4.0f);
    EXPECT_EQ(viewports[1].Width, 4.0f);
}

TEST_F(DX11OverlayBackendTest, ACachedFrameDrawsWithoutWritingItsBuffersAgain) {
    Target overlay = MakeTarget();
    backend_->SetExternalRenderTarget(overlay.view.Get());
    backend_->OnDrawDataChanged();
    Render();
    EXPECT_EQ(backend_->GetUploadCounts().geometryUploads, 1u);
    EXPECT_EQ(backend_->GetUploadCounts().constantUploads, 1u);

    // Cached frames: same geometry, same viewport and HDR parameters.
    for (int frame = 0; frame < 3; ++frame) {
        const float clear[4] = {0, 0, 0, 0};
        context_->ClearRenderTargetView(overlay.view.Get(), clear);
        Render();
        EXPECT_EQ(Pixel(overlay.texture.Get(), 1, 1), kRed) << "drawn from the retained buffers, frame " << frame;
    }
    EXPECT_EQ(backend_->GetUploadCounts().geometryUploads, 1u);
    EXPECT_EQ(backend_->GetUploadCounts().geometryReuses, 3u);
    EXPECT_EQ(backend_->GetUploadCounts().constantUploads, 1u);

    // A rebuilt frame and a changed HDR mode are written.
    for (DrawVertex& vertex : vertices_)
        vertex.x += 8;
    backend_->OnDrawDataChanged();
    backend_->SetHDRParams(1, 203.0f);
    const float clear[4] = {0, 0, 0, 0};
    context_->ClearRenderTargetView(overlay.view.Get(), clear);
    backend_->SetHDRParams(0, 203.0f);
    Render();
    EXPECT_EQ(backend_->GetUploadCounts().geometryUploads, 2u);
    EXPECT_EQ(backend_->GetUploadCounts().constantUploads, 2u) << "paper white changed from the default";
    EXPECT_EQ(Pixel(overlay.texture.Get(), 9, 1), kRed);
    EXPECT_EQ(Pixel(overlay.texture.Get(), 1, 1), 0u) << "the new geometry, not the old";
    backend_->SetExternalRenderTarget(nullptr);
}

}  // namespace
