#pragma once

#include <unknwn.h>
#include <windows.h>
#include <dxgi.h>

#include <atomic>
#include <cstdint>

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
// only that pointer is ever called.
//
// An address alone cannot tell a later swapchain from the one it replaced: the
// allocator hands a released chain's block to the next chain of the same size,
// whether it does depends on what else the process allocated in between, and a
// chain with the same back-buffer size and format then silently inherited the
// old capture generation - its shared textures, transport generation, copy and
// embedded-completion queues. The flow scenario
// FlowInjectFGSwitch.CallbackFSRKeepsRecording saw one transport generation
// across three swapchains under parallel load (2026-10-15). So every binding
// also writes a unique stamp into the chain's own DXGI private data (plain
// bytes, no interface, so still no reference): it lives and dies with that
// object, and a different chain at the same address does not carry it.

namespace ce::capture {

// {C464EC7D-0A1B-4889-98FC-F390EF6F6E65}
inline constexpr GUID kCaptureBindingStampGuid = {
    0xc464ec7d, 0x0a1b, 0x4889, {0x98, 0xfc, 0xf3, 0x90, 0xef, 0x6f, 0x6e, 0x65}};

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

// The binding stamp `object` carries; 0 when it carries none (or answers no IDXGIObject).
inline uint64_t ReadCaptureBindingStamp(IUnknown* object) {
    IDXGIObject* dxgiObject = nullptr;
    if (!object || FAILED(object->QueryInterface(__uuidof(IDXGIObject), reinterpret_cast<void**>(&dxgiObject))) ||
        !dxgiObject) {
        return 0;
    }
    uint64_t stamp = 0;
    UINT size = sizeof(stamp);
    const HRESULT hr = dxgiObject->GetPrivateData(kCaptureBindingStampGuid, &size, &stamp);
    dxgiObject->Release();
    return SUCCEEDED(hr) && size == sizeof(stamp) ? stamp : 0;
}

// Writes `stamp` and reads it back; true only when the object keeps it.
inline bool WriteCaptureBindingStamp(IUnknown* object, uint64_t stamp) {
    IDXGIObject* dxgiObject = nullptr;
    if (!object || FAILED(object->QueryInterface(__uuidof(IDXGIObject), reinterpret_cast<void**>(&dxgiObject))) ||
        !dxgiObject) {
        return false;
    }
    const HRESULT hr = dxgiObject->SetPrivateData(kCaptureBindingStampGuid, sizeof(stamp), &stamp);
    dxgiObject->Release();
    return SUCCEEDED(hr) && ReadCaptureBindingStamp(object) == stamp;
}

enum class SwapChainBindingMatch {
    kUnbound,        // nothing bound
    kOtherIdentity,  // a different live object
    kReusedAddress,  // the bound identity address, but not the bound object
    kMatch,          // the bound object
};

inline const char* SwapChainBindingMatchName(SwapChainBindingMatch match) {
    switch (match) {
        case SwapChainBindingMatch::kOtherIdentity:
            return "otherIdentity";
        case SwapChainBindingMatch::kReusedAddress:
            return "reusedAddress";
        case SwapChainBindingMatch::kMatch:
            return "match";
        default:
            return "unbound";
    }
}

class SwapChainIdentityBinding {
public:
    // Binds to `swapChain`'s identity; false (and unbound) when it has none.
    // One binder per chain: a later Bind of the same chain replaces the stamp.
    bool Bind(IUnknown* swapChain) {
        m_Stamp = 0;
        m_Key = ResolveComIdentityKey(swapChain);
        if (!m_Key) {
            return false;
        }
        // An object that cannot keep the stamp falls back to identity alone.
        const uint64_t stamp = NextStamp();
        m_Stamp = WriteCaptureBindingStamp(swapChain, stamp) ? stamp : 0;
        return true;
    }

    // `swapChain` must be live and owned by the caller for the duration of the call.
    SwapChainBindingMatch Classify(IUnknown* swapChain) const {
        if (!m_Key) {
            return SwapChainBindingMatch::kUnbound;
        }
        if (ResolveComIdentityKey(swapChain) != m_Key) {
            return SwapChainBindingMatch::kOtherIdentity;
        }
        if (m_Stamp != 0 && ReadCaptureBindingStamp(swapChain) != m_Stamp) {
            return SwapChainBindingMatch::kReusedAddress;
        }
        return SwapChainBindingMatch::kMatch;
    }
    bool Matches(IUnknown* swapChain) const { return Classify(swapChain) == SwapChainBindingMatch::kMatch; }

    void Clear() {
        m_Key = nullptr;
        m_Stamp = 0;
    }
    bool IsBound() const { return m_Key != nullptr; }
    // For diagnostics only: an address, never an object.
    const void* Key() const { return m_Key; }
    // 0 when the bound object could not keep a stamp (identity-only binding).
    uint64_t Stamp() const { return m_Stamp; }

private:
    static uint64_t NextStamp() {
        static std::atomic<uint64_t> s_nextStamp{0};
        return s_nextStamp.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    const void* m_Key = nullptr;
    uint64_t m_Stamp = 0;
};

}  // namespace ce::capture
