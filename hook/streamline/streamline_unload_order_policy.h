#pragma once

#include <cstdint>

// When CE hands a title's slSetFeatureLoaded(DLSS-G, false) to Streamline. No Windows calls: the integration
// (streamline_unload_order.cpp) feeds it facts, and unit tests drive it directly.
//
// Streamline rebuilds its hook lists without a disabled plugin (PluginManager::setFeatureEnabled), and the proxy
// swapchain's final Release dispatches eIDXGISwapChain_Destroyed only through those lists. A title that unloads
// DLSS-G while its swapchain proxy is alive and drops the proxy afterwards (GTA V Enhanced, DLSS FG -> FSR FG, session
// 20261110_182518) therefore never lets DLSS-G learn that its chain died: DLSS-G keeps its reference to the native
// chain and its buffers, and DXGI refuses the window's replacement swapchain with E_ACCESSDENIED. The startup
// direction works because the release comes first: slSetFeatureLoaded(true), then the destroy dispatch, then the
// native Release.
//
// CE holds the unload while a live DLSS-G chain exists and forwards it right after that chain's proxy released the
// native chain, which restores Streamline's own order. CE never frees or releases anything of DLSS-G's. While the
// unload is held the title is told what it asked for: slSetFeatureLoaded answers eOk and slIsFeatureLoaded answers
// "not loaded". Only a repeated unload request ends the hold early (the title kept presenting on its chain and asks
// again); slFreeResources and slIsFeatureLoaded calls never do: GTA frees resources around its unload.

namespace ce::streamline_unload_order {

using ChainId = uintptr_t;  // a native swapchain's address: an identity, never dereferenced

enum class RequestAction : uint8_t {
    kForward,  // Streamline gets the call now
    kHold,     // the call is answered with eOk and held
};

// Why a request or a hold ended in a forward (kNone while held).
enum class ForwardReason : uint8_t {
    kNone,
    kObserverMissing,     // not held: nothing would tell CE when the proxy released the chain
    kDestroyHookMissing,  // not held: CE would not see the destroy dispatch
    kNoLiveChain,         // not held: nothing to wait for, Streamline's order cannot go wrong
    kProxyReleased,       // the chain's proxy released the native chain after the destroy dispatch
    kTitleKeptChain,      // the title requested the unload again while its chain kept presenting
    kChainGone,           // the chain is gone and nothing reported its release
};

inline const char* ForwardReasonName(ForwardReason reason) {
    switch (reason) {
        case ForwardReason::kNone:
            return "none";
        case ForwardReason::kObserverMissing:
            return "observer-missing";
        case ForwardReason::kDestroyHookMissing:
            return "destroy-hook-missing";
        case ForwardReason::kNoLiveChain:
            return "no-live-chain";
        case ForwardReason::kProxyReleased:
            return "proxy-released";
        case ForwardReason::kTitleKeptChain:
            return "title-kept-chain";
        case ForwardReason::kChainGone:
            return "chain-gone";
    }
    return "?";
}

// Whether a module file name is one of Streamline's own (sl.interposer.dll, sl.dlss_g.dll, sl.common.dll ...): the
// modules whose Release of the native chain is the proxy dropping it.
inline bool IsStreamlineModuleName(const char* name) {
    if (!name) {
        return false;
    }
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    if (lower(name[0]) != 's' || lower(name[1]) != 'l' || name[2] != '.') {
        return false;
    }
    uint32_t length = 0;
    while (name[length] != '\0') {
        ++length;
    }
    if (length < 8) {  // "sl." + at least one character + ".dll"
        return false;
    }
    const char* const extension = name + length - 4;
    return extension[0] == '.' && lower(extension[1]) == 'd' && lower(extension[2]) == 'l' &&
           lower(extension[3]) == 'l';
}

struct Capabilities {
    bool observerInstalled = false;     // DXGI's swapchain Release is observed
    bool destroyHookInstalled = false;  // the DLSS-G plugin's slHookSwapChainDestroyed is observed
    bool liveChain = false;             // a DLSS-G chain exists and its lifetime witness is alive
};

struct RequestOutcome {
    RequestAction action = RequestAction::kForward;
    ForwardReason reason = ForwardReason::kNone;
};

enum class LoadAction : uint8_t {
    kForward,    // nothing is held: Streamline gets the load request
    kCancelled,  // a held unload was withdrawn; Streamline never saw it, so the feature is still loaded
};

struct ReleaseOutcome {
    bool forwardUnload = false;
    uint32_t residualRefs = 0;  // what the native chain still held after the proxy's release
};

class UnloadOrderPolicy {
public:
    // The title asks for slSetFeatureLoaded(DLSS-G, false); `liveChain` is the DLSS-G chain it may still be using.
    RequestOutcome OnUnloadRequested(const Capabilities& capabilities, ChainId liveChain) {
        if (pending_) {
            if (titleKeptChain_) {
                Clear();
                return {RequestAction::kForward, ForwardReason::kTitleKeptChain};
            }
            return {RequestAction::kHold, ForwardReason::kNone};
        }
        // Without a chain nothing can go wrong, whatever else is missing: say so first.
        if (!capabilities.liveChain || liveChain == 0) {
            return {RequestAction::kForward, ForwardReason::kNoLiveChain};
        }
        if (!capabilities.observerInstalled) {
            return {RequestAction::kForward, ForwardReason::kObserverMissing};
        }
        if (!capabilities.destroyHookInstalled) {
            return {RequestAction::kForward, ForwardReason::kDestroyHookMissing};
        }
        pending_ = true;
        chain_ = liveChain;
        return {RequestAction::kHold, ForwardReason::kNone};
    }

    // slSetFeatureLoaded(DLSS-G, true): the feature was never unloaded while its unload is held.
    LoadAction OnLoadRequested() {
        if (!pending_) {
            return LoadAction::kForward;
        }
        Clear();
        return LoadAction::kCancelled;
    }

    // slIsFeatureLoaded(DLSS-G): true = answer "not loaded" without asking Streamline.
    bool AnswersNotLoaded() const { return pending_; }

    // A present on the held chain: the title still uses it.
    void OnPresent(ChainId chain) {
        if (pending_ && chain == chain_) {
            titleKeptChain_ = true;
        }
    }

    // The plugin's destroy hook returned for `chain` on `thread`. Releases inside the hook are the plugin's own.
    void OnDestroyReturned(ChainId chain, uint32_t thread) {
        if (pending_ && chain == chain_) {
            destroyReturned_ = true;
            destroyThread_ = thread;
        }
    }

    // DXGI's Release of `chain` returned `remaining` to `callerIsStreamline` on `thread`. The release that follows
    // the destroy dispatch on the same thread is the proxy dropping the native chain.
    ReleaseOutcome OnNativeReleaseReturned(ChainId chain, bool callerIsStreamline, uint32_t thread,
                                           uint32_t remaining) {
        if (!pending_ || chain != chain_ || !destroyReturned_ || thread != destroyThread_ || !callerIsStreamline) {
            return {};
        }
        Clear();
        return {true, remaining};
    }

    // The held chain is gone without a release report (its witness ended): true = forward the held unload.
    bool OnChainGone() {
        if (!pending_) {
            return false;
        }
        Clear();
        return true;
    }

    // Streamline's interposer unloaded or the process is ending: nothing left to forward to. True when one was held.
    bool Drop() {
        const bool wasPending = pending_;
        Clear();
        return wasPending;
    }

    bool Pending() const { return pending_; }
    ChainId PendingChain() const { return pending_ ? chain_ : 0; }
    bool TitleKeptChain() const { return titleKeptChain_; }

private:
    void Clear() {
        pending_ = false;
        titleKeptChain_ = false;
        destroyReturned_ = false;
        chain_ = 0;
        destroyThread_ = 0;
    }

    bool pending_ = false;
    bool titleKeptChain_ = false;
    bool destroyReturned_ = false;
    ChainId chain_ = 0;
    uint32_t destroyThread_ = 0;
};

}  // namespace ce::streamline_unload_order
