#include "translated_swapchain_owner.h"

#include <atomic>
#include <wrl/client.h>

#include "hook/present/dxgi_shared_internal.h"
#include "hook/runtime/hook_common.h"
#include "hook/wrappers/dxgi_swapchain_wrap.h"

namespace ce::translated_swapchain {
namespace {
thread_local CreateScope* g_createScope = nullptr;
std::atomic<bool> g_hasTranslatedOwners{false};
constexpr GUID kNativeOwnerMarker = {0x749fef11, 0x7d18, 0x490b, {0xb8, 0x02, 0x58, 0x36, 0x47, 0x77, 0xc3, 0x68}};
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
    // Private data follows the object's lifetime and needs no global map or native-object lease.
    // Include the outer identity because translators often forward private data to their native chain.
    const void* identity = applicationSwapchain;
    const HRESULT hr = applicationSwapchain->SetPrivateData(kNativeOwnerMarker, sizeof(identity), &identity);
    if (SUCCEEDED(hr)) {
        g_hasTranslatedOwners.store(true, std::memory_order_release);
        HookLogImportant("DXGI translation: native output owns overlay/capture/post-processing "
                         "(application=%p native=%p hwnd=%p)", applicationSwapchain, native_, window_);
    } else {
        HookLogImportant("DXGI translation: could not publish native output ownership "
                         "(application=%p native=%p hr=0x%08X)", applicationSwapchain, native_, hr);
    }
}

void ObserveNativeCreate(IDXGISwapChain* swapchain) {
    if (!g_createScope || !swapchain || !DXGIShared::HasPresentDetourHooks()) return;
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
            (vtable[8] == reinterpret_cast<void*>(DXGIShared::DetourPresent) ||
             DXGIShared::IsAddressInsideSystemDXGI(vtable[8]));
        ObserveNativeCreate(native, desc.OutputWindow, covered);
    }
}

bool HasNativeOwner(IDXGISwapChain* applicationSwapchain) {
    if (!applicationSwapchain || !g_hasTranslatedOwners.load(std::memory_order_acquire) ||
        !DXGIShared::HasPresentDetourHooks()) return false;
    const void* identity = nullptr;
    UINT size = sizeof(identity);
    return SUCCEEDED(applicationSwapchain->GetPrivateData(kNativeOwnerMarker, &size, &identity)) &&
           size == sizeof(identity) && identity == applicationSwapchain;
}

}  // namespace ce::translated_swapchain
