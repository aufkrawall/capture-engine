#include "tests/flow/d3d11_af_fixture.h"

#include <d3d11_4.h>

// CE's D3D11 device wrapper (hook/wrappers/d3d11_device_wrap.cpp), which a game gets when CE
// catches its D3D11CreateDevice. It used to keep only the highest interface the real device
// offered (ID3D11Device5 on current runtimes), so every ID3D11Device1-4 method returned
// E_NOINTERFACE through an interface QueryInterface had just handed out.
namespace {
using ce::flow::af::AFFixture;
using ce::flow::af::Path;
using Microsoft::WRL::ComPtr;

TEST(FlowD3D11WrapperDevice, ForwardsEveryInheritedInterface) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, Path::kWrapper);
    ASSERT_TRUE(af.Ready());
    ComPtr<ID3D11Device1> device1;
    ASSERT_TRUE(SUCCEEDED(af.device.As(&device1)));

    D3D11_BLEND_DESC1 blend = {};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState1> blendState;
    const HRESULT blendHr = device1->CreateBlendState1(&blend, &blendState);
    EXPECT_TRUE(SUCCEEDED(blendHr) && blendState) << "CreateBlendState1 hr=0x" << std::hex << blendHr;

    const D3D_FEATURE_LEVEL level = af.device->GetFeatureLevel();
    ComPtr<ID3DDeviceContextState> state;
    const HRESULT stateHr =
        device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &state);
    EXPECT_TRUE(SUCCEEDED(stateHr) && state) << "CreateDeviceContextState hr=0x" << std::hex << stateHr;

    ComPtr<ID3D11DeviceContext1> immediate;
    device1->GetImmediateContext1(&immediate);
    EXPECT_TRUE(immediate) << "GetImmediateContext1";

    ComPtr<ID3D11DeviceContext1> deferred;
    const HRESULT deferredHr = device1->CreateDeferredContext1(0, &deferred);
    EXPECT_TRUE(SUCCEEDED(deferredHr) && deferred) << "CreateDeferredContext1 hr=0x" << std::hex << deferredHr;

    for (const IID& iid : {__uuidof(ID3D11Device2), __uuidof(ID3D11Device3), __uuidof(ID3D11Device4)}) {
        ComPtr<IUnknown> newer;
        if (SUCCEEDED(af.device->QueryInterface(iid, &newer))) {
            EXPECT_TRUE(newer);
        }
    }
}

}  // namespace
