/**
 * D3D11 Device Context Wrapper Implementation
 *
 * Object lifetime and the wrapper's side of forced AF. The AF state machine itself
 * is the engine the raw context hooks share (hook/d3d11/d3d11_af_engine.h); this
 * unit only tells it how to reach the real context. The COM/forwarding surface is
 * split across d3d11_devicecontext_forward.cpp and d3d11_devicecontext_get.cpp.
 */

#include "d3d11_devicecontext_wrap.h"
#include "d3d11_devicecontext_wrap_internal.h"

#include "hook/d3d11/dx11_hook.h"
#include "hook/overrides/sampler_override/translation_scope.h"
#include "d3d11_device_wrap.h"
#include "hook/runtime/hook_common.h"

// The engine's view of this wrapper's real context. Binds and creates go below CE's
// own interception (the raw hooks see the forwarding scopes and pass through).
class WrapperAFHost final : public ce::d3d11_af::Host {
public:
    explicit WrapperAFHost(CWrapD3D11DeviceContext& context) : context_(context) {}

    void BindPhysical(UINT startSlot, UINT count, ID3D11SamplerState* const* samplers) override {
        DX11WrapperForwardingScope forwardingScope;
        context_.m_pReal->PSSetSamplers(startSlot, count, samplers);
    }
    ID3D11PixelShader* AcquireBoundShader() override {
        ID3D11PixelShader* shader = nullptr;
        UINT classInstanceCount = 0;
        context_.m_pReal->PSGetShader(&shader, nullptr, &classInstanceCount);
        return shader;
    }
    ID3D11ShaderResourceView* AcquireBoundView(UINT slot) override {
        ID3D11ShaderResourceView* view = nullptr;
        context_.m_pReal->PSGetShaderResources(slot, 1, &view);
        return view;
    }
    ID3D11Device* AcquireDevice() override {
        if (context_.m_pAFRealDevice) {
            context_.m_pAFRealDevice->AddRef();
        }
        return context_.m_pAFRealDevice;
    }
    HRESULT CreateReplacement(ID3D11Device* device, const D3D11_SAMPLER_DESC& desc,
                              ID3D11SamplerState** sampler) override {
        DX11Hook_BeginWrapperSamplerForwarding();
        HRESULT hr = E_FAIL;
        {
            ce::sampler_override::D3D11SamplerCreationScope translationScope;
            hr = device->CreateSamplerState(&desc, sampler);
        }
        DX11Hook_EndWrapperSamplerForwarding();
        return hr;
    }
    const char* PathName() const override { return "wrapper"; }

private:
    CWrapD3D11DeviceContext& context_;
};

namespace {
// A wrapper context is single-threaded by the D3D11 contract: its state needs no lock.
struct WrapperAFAccess {
    // Every draw entry point of the wrapper reaches PreparePixelSamplersForDraw, so a sampler bind
    // waits for the draw that uses it (d3d11_af_engine.h, Deferring).
    static constexpr bool kDefersSamplerBinds = true;

    ce::d3d11_af::PixelState& state;

    template <class Fn>
    void Run(Fn&& fn) {
        fn(state);
    }
    bool IsClean(uint64_t epoch) const { return state.dirtyMask == 0 && state.epoch == epoch; }
};
}  // namespace

// ============================================================================
// Constructor / Destructor
// ============================================================================

CWrapD3D11DeviceContext::CWrapD3D11DeviceContext(ID3D11DeviceContext* pReal, CWrapD3D11Device* pDevice)
    : m_pReal(pReal),
      m_pReal1(nullptr),
      m_pReal2(nullptr),
      m_pReal3(nullptr),
      m_pReal4(nullptr),
      m_pAFRealDevice(nullptr),
      m_pDevice(pDevice),
      m_RefCount(1),
      m_Version(0) {
    if (pReal) {
        pReal->AddRef();
        pReal->GetDevice(&m_pAFRealDevice);
        PromoteInterfaces();
    }
    if (m_pDevice) {
        m_pDevice->AddRef();
    }
    ce::d3d11_af::RegisterReportedState(&m_AF);
    WrapperLog("D3D11 Context Wrapper: Created (real=%p, version=%d, ctx1=%d ctx2=%d ctx3=%d ctx4=%d)", pReal,
               m_Version, m_pReal1 != nullptr, m_pReal2 != nullptr, m_pReal3 != nullptr, m_pReal4 != nullptr);
}

CWrapD3D11DeviceContext::~CWrapD3D11DeviceContext() {
    try {
    WrapperLog("D3D11 Context Wrapper: Destroyed");
    ce::d3d11_af::UnregisterReportedState(&m_AF);
    ClearForcedAFTracking();
    if (m_pReal4)
        m_pReal4->Release();
    if (m_pReal3)
        m_pReal3->Release();
    if (m_pReal2)
        m_pReal2->Release();
    if (m_pReal1)
        m_pReal1->Release();
    if (m_pAFRealDevice)
        m_pAFRealDevice->Release();
    if (m_pDevice)
        m_pDevice->Release();
    if (m_pReal)
        m_pReal->Release();
    } catch (...) {
        WrapperLog("D3D11 Context Wrapper: suppressed exception during destruction");
    }
}

void CWrapD3D11DeviceContext::InvalidateDeviceWrapper() {
    if (m_pDevice) {
        m_pDevice->Release();
        m_pDevice = nullptr;
    }
}

void CWrapD3D11DeviceContext::PromoteInterfaces() {
    if (!m_pReal)
        return;

    DX11Hook_BeginInternalIdentityProbe();
    // Cache every inherited context interface independently. A modern context may
    // support ID3D11DeviceContext4 while callers still use Context1 methods like
    // ClearView; stopping at the highest interface would leave those forwards null.
    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal1)))) {
        m_Version = 1;
    }
    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal2)))) {
        m_Version = 2;
    }
    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal3)))) {
        m_Version = 3;
    }
    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal4)))) {
        m_Version = 4;
    }
    DX11Hook_EndInternalIdentityProbe();
}

// ClearState, command-list boundaries and SwapDeviceContextState leave the real
// context with nothing bound; so does the tracked state.
void CWrapD3D11DeviceContext::ClearForcedAFTracking() {
    ce::d3d11_af::ClearState(m_AF);
}

void CWrapD3D11DeviceContext::TrackPixelShaderResources(UINT startSlot, UINT numViews,
                                                        ID3D11ShaderResourceView* const* views) {
    WrapperAFAccess access{m_AF};
    ce::d3d11_af::OnViews(access, startSlot, numViews, views);
}

void CWrapD3D11DeviceContext::TrackPixelShader(ID3D11PixelShader* shader) {
    WrapperAFAccess access{m_AF};
    ce::d3d11_af::OnShader(access, shader);
}

void CWrapD3D11DeviceContext::TranslatePixelSamplers(UINT startSlot, UINT numSamplers,
                                                     ID3D11SamplerState** samplers) const {
    ce::d3d11_af::TranslateToLogical(m_AF, startSlot, numSamplers, samplers);
}

void CWrapD3D11DeviceContext::PreparePixelSamplersForDraw() {
    // Every draw entry point on the GAME's immediate context passes through
    // here, which makes it the one place that can answer "did the application
    // render anything since the previous present?" - the evidence the DX11
    // present-interposer classifier needs. Off unless an interposer chain is
    // registered, so the ordinary path pays a single relaxed atomic load.
    g_FGCompat.NoteApplicationSubmission();
    WrapperAFAccess access{m_AF};
    WrapperAFHost host(*this);
    ce::d3d11_af::BeforeDraw(access, host);
}

void CWrapD3D11DeviceContext::FlushPendingPixelSamplers() {
    WrapperAFAccess access{m_AF};
    WrapperAFHost host(*this);
    ce::d3d11_af::FlushPending(access, host);
}

void CWrapD3D11DeviceContext::BindTrackedSamplers(UINT stageIndex, UINT startSlot, UINT numSamplers,
                                                  ID3D11SamplerState* const* samplers) {
    if (!m_pReal) {
        return;
    }
    if (stageIndex == kWrapperStagePS) {
        WrapperAFAccess access{m_AF};
        WrapperAFHost host(*this);
        ce::d3d11_af::OnSamplers(access, host, startSlot, numSamplers, samplers);
        return;
    }
    // Forced AF replaces pixel samplers only; the other stages go through unchanged.
    DX11WrapperForwardingScope forwardingScope;
    switch (stageIndex) {
        case kWrapperStageVS:
            m_pReal->VSSetSamplers(startSlot, numSamplers, samplers);
            break;
        case kWrapperStageGS:
            m_pReal->GSSetSamplers(startSlot, numSamplers, samplers);
            break;
        case kWrapperStageHS:
            m_pReal->HSSetSamplers(startSlot, numSamplers, samplers);
            break;
        case kWrapperStageDS:
            m_pReal->DSSetSamplers(startSlot, numSamplers, samplers);
            break;
        case kWrapperStageCS:
            m_pReal->CSSetSamplers(startSlot, numSamplers, samplers);
            break;
        default:
            break;
    }
}
