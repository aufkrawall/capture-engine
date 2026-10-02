#pragma once

#include <unknwn.h>
#include <windows.h>

// Which swapchain a D3D12 capture generation copies from, held WITHOUT a COM
// reference.
//
// A reference on a flip-model swapchain keeps it alive after the game released
// it, and DXGI then refuses every replacement swapchain on that HWND with
// E_ACCESSDENIED (see hook/present/swapchain_liveness.h). The capture used to
// keep two (IDXGISwapChain3 plus its IUnknown identity) for as long as its
// generation lived, which outlasts the recording: in Talos Reawakened a
// recording made under DLSS FG pinned Streamline's swapchain, the later
// DLSS FG -> FSR FG switch had AMD's CreateSwapChainForHwnd denied, and the
// game terminated (logs/taloscrashdlssfgtofsrfg, 0.1.6852).
//
// The key is the object's IUnknown identity address. It is compared, never
// dereferenced: whoever asks passes the live swapchain it is presenting, and
// only that pointer is ever called. An address reused by a later swapchain
// matches the old key; the capture's back-buffer description check turns a
// different chain at the same address into a re-initialization.

namespace ce::capture {

// The COM identity address of `object`, taken and released in the same call so
// the count is unchanged. nullptr when the object answers no IUnknown.
inline const void* ResolveComIdentityKey(IUnknown* object) {
    if (!object) {
        return nullptr;
    }
    IUnknown* identity = nullptr;
    if (FAILED(object->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&identity))) || !identity) {
        return nullptr;
    }
    const void* key = identity;
    identity->Release();
    return key;
}

class SwapChainIdentityBinding {
public:
    // Binds to `swapChain`'s identity; false (and unbound) when it has none.
    bool Bind(IUnknown* swapChain) {
        m_Key = ResolveComIdentityKey(swapChain);
        return m_Key != nullptr;
    }

    // `swapChain` must be live and owned by the caller for the duration of the call.
    bool Matches(IUnknown* swapChain) const {
        return m_Key && ResolveComIdentityKey(swapChain) == m_Key;
    }

    void Clear() { m_Key = nullptr; }
    bool IsBound() const { return m_Key != nullptr; }
    // For diagnostics only: an address, never an object.
    const void* Key() const { return m_Key; }

private:
    const void* m_Key = nullptr;
};

}  // namespace ce::capture
