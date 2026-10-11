#include "translated_swapchain_owner.h"
#include "translated_swapchain_cache.h"

#include <atomic>
#include <wrl/client.h>

#include "common/logging/log_meter.h"
#include "hook/present/dxgi_shared_internal.h"
#include "hook/present/present_vtable_dispatch.h"
#include "hook/runtime/hook_common.h"
#include "hook/wrappers/dxgi_swapchain_wrap.h"

namespace ce::translated_swapchain {
namespace {
thread_local CreateScope* g_createScope = nullptr;
std::atomic<bool> g_hasNativeOutputs{false};
constexpr GUID kNativeOwnerMarker = {0x749fef11, 0x7d18, 0x490b, {0xb8, 0x02, 0x58, 0x36, 0x47, 0x77, 0xc3, 0x68}};
struct OwnerMarker {
    const void* native = nullptr;
};

bool PublishOwner(IDXGISwapChain* frontend, const void* native) {
    const OwnerMarker marker{native};
    const HRESULT hr = frontend->SetPrivateData(kNativeOwnerMarker, sizeof(marker), &marker);
    if (SUCCEEDED(hr)) {
        g_hasNativeOutputs.store(true, std::memory_order_release);
        if (detail::PublishDecision(frontend, native, true) == detail::CachePublication::kChanged)
            HookLogImportant("DXGI translation: native output owns overlay/capture/post-processing "
                             "(application=%p native=%p)", frontend, native);
        return true;
    }
    static ce::log_meter::ChangeGate failure;
    if (failure.Observe(ce::log_meter::FieldKey(frontend, native, hr)))
        HookLogImportant("DXGI translation: could not publish native output ownership "
                         "(application=%p native=%p hr=0x%08X)", frontend, native, hr);
    return false;
}
}  // namespace

CreateScope::CreateScope(HWND window, bool applicationApi)
    : previous_(g_createScope), window_(window), applicationApi_(applicationApi) {
    g_createScope = this;
}

CreateScope::~CreateScope() {
    g_createScope = previous_;
}

bool CreateScope::HasSingleNativeOwner() const {
    return native_ && !ambiguous_;
}

void ObserveNativeCreate(IDXGISwapChain* swapchain, HWND window, bool presentCovered) {
    if (!swapchain || !window || !presentCovered) return;
    for (auto* scope = g_createScope; scope; scope = scope->previous_) {
        if (!scope->applicationApi_ || scope->window_ != window) continue;
        if (scope->native_ && scope->native_ != swapchain) scope->ambiguous_ = true;
        scope->native_ = swapchain;
    }
}

void CreateScope::Publish(IDXGISwapChain* applicationSwapchain) const {
    if (!applicationSwapchain || !HasSingleNativeOwner()) return;
    Microsoft::WRL::ComPtr<CWrapDXGISwapChain> wrapper;
    if (applicationSwapchain && SUCCEEDED(applicationSwapchain->QueryInterface(
            IID_CWrapDXGISwapChain, reinterpret_cast<void**>(wrapper.GetAddressOf()))) && wrapper &&
        reinterpret_cast<IDXGISwapChain*>(wrapper.Get()) == applicationSwapchain)
        applicationSwapchain = wrapper->GetReal();
    if (!applicationSwapchain || applicationSwapchain == native_) return;
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(applicationSwapchain->GetDesc(&desc)) || desc.OutputWindow != window_) return;
    // Private data may be forwarded to the output; it carries no frontend lifetime or decision.
    PublishOwner(applicationSwapchain, native_);
}

void ObserveNativeCreate(IDXGISwapChain* swapchain) {
    if (!swapchain || !DXGIShared::HasPresentDetourHooks()) return;
    Microsoft::WRL::ComPtr<CWrapDXGISwapChain> wrapper;
    auto* native = swapchain;
    if (SUCCEEDED(swapchain->QueryInterface(IID_CWrapDXGISwapChain,
                                           reinterpret_cast<void**>(wrapper.GetAddressOf()))) && wrapper &&
        reinterpret_cast<IDXGISwapChain*>(wrapper.Get()) == swapchain)
        native = wrapper->GetReal();
    DXGI_SWAP_CHAIN_DESC desc{};
    if (native && SUCCEEDED(native->GetDesc(&desc))) {
        void** vtable = *reinterpret_cast<void***>(native);
        const bool covered = vtable &&
            (DXGIShared::IsPresentDetourAddress(vtable[8]) ||
             DXGIShared::IsAddressInsideSystemDXGI(vtable[8]));
        if (covered) {
            // Cached application exports can bypass every frontend create scope. A forwarding
            // proxy still exposes this exact output's private data at its first Present.
            const OwnerMarker marker{native};
            if (SUCCEEDED(native->SetPrivateData(kNativeOwnerMarker, sizeof(marker), &marker)))
                g_hasNativeOutputs.store(true, std::memory_order_release);
        }
        ObserveNativeCreate(native, desc.OutputWindow, covered);
    }
}

bool HasNativeOwner(IDXGISwapChain* applicationSwapchain) {
    if (!applicationSwapchain || HookIsShuttingDown() || !g_hasNativeOutputs.load(std::memory_order_acquire) ||
        !DXGIShared::HasPresentDetourHooks()) return false;
    OwnerMarker marker{};
    UINT size = sizeof(marker);
    if (FAILED(applicationSwapchain->GetPrivateData(kNativeOwnerMarker, &size, &marker)) ||
        size != sizeof(marker) || !marker.native || marker.native == applicationSwapchain) return false;
    if (const auto decision = detail::FindDecision(applicationSwapchain, marker.native)) return *decision;

    // A forwarded native marker plus a different API device proves this is a translated
    // frontend. DX12/FG proxies retain their own routing, even when they forward private data.
    Microsoft::WRL::ComPtr<ID3D12Device> dx12;
    Microsoft::WRL::ComPtr<ID3D11Device> dx11;
    Microsoft::WRL::ComPtr<ID3D10Device> dx10;
    const bool translated = FAILED(applicationSwapchain->GetDevice(IID_PPV_ARGS(&dx12))) &&
        ((SUCCEEDED(applicationSwapchain->GetDevice(IID_PPV_ARGS(&dx11))) && dx11) ||
         (SUCCEEDED(applicationSwapchain->GetDevice(IID_PPV_ARGS(&dx10))) && dx10));
    if (translated) return PublishOwner(applicationSwapchain, marker.native);

    // A negative decision has the same frontend lifetime boundary as a positive one.
    detail::PublishDecision(applicationSwapchain, marker.native, false);
    return false;
}

}  // namespace ce::translated_swapchain
