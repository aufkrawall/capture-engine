// Streamline is a 64-bit runtime; see streamline_bridge_translate.cpp.
#if defined(_M_X64) || defined(__x86_64__)

#include "streamline_bridge_dlssg.h"

#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "../common/hook_common.h"
#include "streamline_bridge_diag.h"
#include "streamline_bridge_dlssg_gate.h"
#include "streamline_bridge_reflex.h"

namespace ce::streamline_bridge {
namespace {

// 2.x treats repeated SetOptions as a Present-race warning; 1.x drives feature constants
// every frame. Keep the title's request and the last forwarded state per viewport and forward
// only changes.
struct DlssgViewportState {
    DlssgViewportRequest request;
    DlssgForwardedOptions forwarded;
    bool haveForwarded = false;
};
std::mutex g_dlssgOptionsMutex;
std::unordered_map<uint32_t, DlssgViewportState> g_dlssgOptionsByViewport;

// Forwards the viewport's effective DLSS-G state when it differs from what 2.x last accepted.
// Callers are the title's own constants calls, which 1.x issues on one thread per frame.
bool SyncDlssgOptions(PFun_slDLSSGSetOptions* setOptions, uint32_t id, bool fromTitleConstants) {
    DlssgViewportRequest request;
    DlssgForwardedOptions wanted;
    bool titleIntentChanged = false;  // the title's FG on/off, which is what Reflex follows
    {
        std::lock_guard<std::mutex> lock(g_dlssgOptionsMutex);
        const DlssgViewportState& state = g_dlssgOptionsByViewport[id];
        if (!DlssgGateNeedsSync(state.request, state.haveForwarded, state.forwarded)) {
            return true;  // unchanged: forwarding again is a documented Present race
        }
        request = state.request;
        wanted = DlssgOptionsFor(request);
        titleIntentChanged = !state.haveForwarded ||
                             state.forwarded.retainResourcesWhenOff != wanted.retainResourcesWhenOff;
    }
    sl::DLSSGOptions options{};
    options.mode = wanted.on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    // Left at whatever 1.x asked for; CE's own dlss_fg_factor override applies later,
    // on its existing slDLSSGSetOptions hook, exactly as it does for a native 2.x game.
    options.numFramesToGenerate = wanted.numFramesToGenerate;
    if (wanted.retainResourcesWhenOff) {
        options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    }
    static ResultTracker latch;
    if (!ResultOk(setOptions(sl::ViewportHandle(id), options), "slDLSSGSetOptions", latch)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_dlssgOptionsMutex);
        DlssgViewportState& state = g_dlssgOptionsByViewport[id];
        state.forwarded = wanted;
        state.haveForwarded = true;
    }
    static std::atomic<uint32_t> syncLogs{0};
    const uint32_t n = syncLogs.fetch_add(1, std::memory_order_relaxed);
    if (n < 32 || (n % 256) == 0) {
        HookLogImportant(
            "Streamline bridge: DLSS-G %s on viewport %u (%s; title mode=%u notRenderingGameFrames=%u frames=%u "
            "retain=%u) #%u",
            wanted.on ? "generating" : "off", id,
            fromTitleConstants ? "title DLSS-G constants" : "notRenderingGameFrames",
            request.gameRequestsOn ? 1u : 0u, request.notRenderingGameFrames ? 1u : 0u,
            wanted.numFramesToGenerate, wanted.retainResourcesWhenOff ? 1u : 0u, n + 1);
    }
    // 2.x refuses to generate frames unless Reflex is detected at runtime. Some 1.x titles
    // (including The Witcher 3) leave their SL Reflex mode at zero while using NVAPI Reflex
    // separately, which the 2.x plugin cannot observe. Promote Reflex while the TITLE wants FG
    // on and restore its mode when it turns FG off; a non-game stretch does not churn it.
    if (titleIntentChanged && !UpdateReflexForDlssg(request.gameRequestsOn)) {
        HookLogImportant("Streamline bridge: failed to update Reflex for DLSS-G state %u",
                         request.gameRequestsOn ? 1u : 0u);
    }
    return true;
}

}  // namespace

bool TranslateDlssgConstants(PFun_slDLSSGSetOptions* setOptions, uint32_t mode, uint32_t numFramesToGenerate,
                             uint32_t id) {
    if (!setOptions) {
        return false;
    }
    const bool on = mode != 0;
    const uint32_t frames = numFramesToGenerate ? numFramesToGenerate : 1;
    {
        std::lock_guard<std::mutex> lock(g_dlssgOptionsMutex);
        DlssgViewportRequest& request = g_dlssgOptionsByViewport[id].request;
        request.haveGameRequest = true;
        request.gameRequestsOn = on;
        request.numFramesToGenerate = frames;
    }
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        HookLogImportant("Streamline bridge: first DLSS-G options translated - mode=%u numFramesToGenerate=%u", mode,
                         frames);
    }
    return SyncDlssgOptions(setOptions, id, true);
}

void ApplyNotRenderingGameFrames(PFun_slDLSSGSetOptions* setOptions, uint32_t id, uint32_t frameIndex,
                                 uint8_t notRenderingGameFrames) {
    const bool notGame = V1FrameIsNotGameFrame(notRenderingGameFrames);
    bool changed = false;
    bool configured = false;
    {
        std::lock_guard<std::mutex> lock(g_dlssgOptionsMutex);
        DlssgViewportState& state = g_dlssgOptionsByViewport[id];
        changed = state.request.notRenderingGameFrames != notGame;
        state.request.notRenderingGameFrames = notGame;
        configured = state.request.haveGameRequest;
    }
    if (!changed) {
        return;
    }
    static std::atomic<uint32_t> transitions{0};
    const uint32_t n = transitions.fetch_add(1, std::memory_order_relaxed);
    if (n < 32 || (n % 256) == 0) {
        HookLogImportant(
            "Streamline bridge: title marked frame %u on viewport %u as %s (notRenderingGameFrames raw=%u, "
            "DLSS-G configured=%u) - transition #%u",
            frameIndex, id, notGame ? "NOT a game frame" : "a game frame", notRenderingGameFrames,
            configured ? 1u : 0u, n + 1);
    }
    if (configured && setOptions) {
        SyncDlssgOptions(setOptions, id, false);
    }
}

bool DlssgEnabledOnAnyViewport() {
    std::lock_guard<std::mutex> lock(g_dlssgOptionsMutex);
    for (const auto& [viewport, state] : g_dlssgOptionsByViewport) {
        (void)viewport;
        if (state.haveForwarded && state.forwarded.on) {
            return true;
        }
    }
    return false;
}

bool DlssgTitleRequestsOn(uint32_t id) {
    std::lock_guard<std::mutex> lock(g_dlssgOptionsMutex);
    auto it = g_dlssgOptionsByViewport.find(id);
    return it != g_dlssgOptionsByViewport.end() && it->second.request.gameRequestsOn;
}

namespace {
std::mutex g_presentTagMutex;
std::vector<PersistentTag> g_presentTags;  // guarded by g_presentTagMutex; a handful of entries
bool g_titleTaggedSinceRefresh = false;    // guarded by g_presentTagMutex
}  // namespace

void RememberPresentTag(uint32_t viewport, uint32_t bufferType, const sl::Resource* resource,
                        const sl::Extent* extent) {
    if (!V1TagPersistsAcrossPresents(bufferType)) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_presentTagMutex);
    g_titleTaggedSinceRefresh = true;
    auto it = g_presentTags.begin();
    while (it != g_presentTags.end() && (it->viewport != viewport || it->bufferType != bufferType)) {
        ++it;
    }
    if (!resource || !resource->native) {
        // A null tag is the title withdrawing the input; never resurrect it.
        if (it != g_presentTags.end()) {
            g_presentTags.erase(it);
        }
        return;
    }
    if (it == g_presentTags.end()) {
        it = g_presentTags.insert(g_presentTags.end(), PersistentTag{});
        it->viewport = viewport;
        it->bufferType = bufferType;
    }
    it->resource = *resource;
    it->haveExtent = extent != nullptr;
    it->extent = extent ? *extent : sl::Extent{};
}

bool TakePresentTagsForRefresh(std::vector<PersistentTag>& out, bool& titleRetagged) {
    out.clear();
    if (!DlssgEnabledOnAnyViewport()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_presentTagMutex);
    titleRetagged = g_titleTaggedSinceRefresh;
    g_titleTaggedSinceRefresh = false;
    out = g_presentTags;
    return !out.empty();
}

}  // namespace ce::streamline_bridge

#endif  // x64
