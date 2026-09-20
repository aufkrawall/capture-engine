#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include "../../common/sharpen_policy.h"
#include "sharpen_pass_log.h"

// D3D11 sharpen pass.
//
// Runs on the render target the overlay is about to draw into, immediately
// before it draws, so the game's frame is filtered and CE's own overlay pixels
// are written afterwards and never sharpened.
namespace ce::sharpen {

class D3D11Pass {
public:
    // `targetView` is the view the overlay will render through. Returns true
    // when the frame was filtered; every other outcome is a logged refusal and
    // leaves the target exactly as it was.
    bool Render(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11RenderTargetView* targetView,
                const Request& request, Route route, TargetEncoding encoding);

    // Releases every device object. Safe to call repeatedly and safe to call
    // when nothing was ever created.
    void Shutdown();

    // Drops every reference WITHOUT releasing it, for the one case where the
    // D3D device is already being torn down underneath us and calling Release
    // on anything it owns can fault. This deliberately leaks; the process is
    // going away and the overlay path makes the same trade for the same reason.
    void Abandon();

private:
    bool EnsureDeviceObjects(ID3D11Device* device);
    bool EnsurePipelineState(ID3D11Device* device);
    bool EnsureSourceCopy(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& targetDesc, DXGI_FORMAT viewFormat);

    // Identity only, never dereferenced: a different device means every cached
    // object below belongs to a dead one and the whole set is rebuilt.
    ID3D11Device* ownerDevice_ = nullptr;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> casShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> rcasShader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constantBuffer_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerState_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blendState_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthState_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> sourceCopy_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sourceView_;
    uint32_t copyWidth_ = 0;
    uint32_t copyHeight_ = 0;
    DXGI_FORMAT copyFormat_ = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT copyViewFormat_ = DXGI_FORMAT_UNKNOWN;

    DecisionLogGate logGate_;
};

// True for the formats whose views decode on load and re-encode on store.
bool FormatAppliesSrgbConversion(DXGI_FORMAT format);

// Presentation meaning for a DXGI target, given the swapchain's HDR contract.
// Storage format alone never decides this, which is why `isHDR` is required.
TargetEncoding ResolveDxgiEncoding(DXGI_FORMAT format, bool isHDR);

}  // namespace ce::sharpen
