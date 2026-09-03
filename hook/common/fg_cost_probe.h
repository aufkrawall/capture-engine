#pragma once

#include <cstdint>

// Frame-generation cost probe.
//
// Under 2x FSR frame generation CaptureEngine costs the game about 2 ms per base
// frame, and no CPU measurement can see it: the game thread spends that time
// blocked inside the FFX runtime's Present, CE's own hook cycles come to ~1.4% of
// one core, and the per-thread totals barely move. The GPU-engine counter does
// see it — 4.89 ms of GPU busy per base frame without CE against 6.71 ms with it
// on the same scene — so what has to be attributed is GPU work CE appends to the
// frame-generation runtime's own command lists, not CPU work in a hook.
//
// A frame-rate A/B is the right instrument for that, but only if it can remove
// one contribution at a time. Each bit below suppresses exactly one thing CE does
// per present, so a run with that bit set answers "is this the 2 ms?".
//
// Diagnostic only. The mask is read once from the hooked process's
// CE_FG_COST_PROBE environment variable and is 0 (nothing suppressed) for every
// normal run. A bit that recovers frames names code that then has to be fixed
// properly — setting one is never the fix.
namespace ce::fg_cost_probe {

enum Bit : uint32_t {
    // The FFX present-callback bridge forwards the runtime callback and returns:
    // no self-compose, no overlay, no breadcrumbs, no metrics publication.
    kBridgeTailOff = 0x1,
    // No WriteBufferImmediate overlay breadcrumbs on any command list. These are
    // MARKER_OUT writes appended to the frame-generation runtime's list twice per
    // callback whether or not the overlay drew anything.
    kBreadcrumbsOff = 0x2,
    // The bridge keeps its breadcrumbs and metrics but draws no overlay, which
    // separates the overlay's own GPU cost from the rest of the bridge.
    kBridgeOverlayOff = 0x4,
    // The bridge draws the output buffer only and never mirrors the overlay into
    // currentBackBuffer, halving its render targets per callback.
    kBridgeMirrorOff = 0x8,
    // ExecuteCommandLists forwards immediately. Frame generation multiplies
    // submissions, and CE's detour runs on the runtime's own submission threads.
    kEclPassthrough = 0x10,
    // The DXGI present hook forwards immediately: no ProcessFrame, no overlay
    // site, no per-present bookkeeping on the runtime's presenter thread.
    kPresentPassthrough = 0x20,
    // ffxConfigure forwards the game's own present callback unchanged, so CE is
    // never on the frame-generation runtime's callback path at all.
    kFfxBridgeOff = 0x40,
    // ExecuteCommandLists skips the third-party-overlay caller identification,
    // which resolves the return address to a module path (loader lock) per call.
    kEclCallerModuleLookupOff = 0x80,
    // ExecuteCommandLists skips the Streamline UI bootstrap-overlay observers,
    // which take one process-global recursive mutex twice per submission.
    kEclStreamlineUiHooksOff = 0x100,
    // ExecuteCommandLists skips the PostSL/Streamline startup-activation block.
    kEclStartupBlockOff = 0x200,
    // ExecuteCommandLists forwards as soon as its guards have run, skipping every
    // per-call classification, queue-registration and observer step below them.
    kEclClassificationOff = 0x400,
    // ExecuteCommandLists skips its per-call wall-clock diagnostic and the render
    // watchdog heartbeat, the two unconditional steps above the classification.
    kEclDiagnosticsOff = 0x800,
    // ExecuteCommandLists never re-registers the submitting queue. Registration
    // takes the command-queue mutex and calls back into the queue, so a queue the
    // fast path does not recognise pays it on every submission.
    kEclQueueRegistrationOff = 0x1000,
    // Queue registration stops short of hooking the queue's vtable.
    kQueueVTableHookOff = 0x2000,
    // Queue registration adopts the queue but never resolves or publishes its
    // device: no g_Device, no limiter device, no adapter LUID, no bridge notify.
    kQueueDevicePublishOff = 0x4000,
    // Queue registration never adopts the queue or its device: CE keeps no
    // g_CommandQueue/g_Device, which is the state every configuration that
    // recovers the frame rate happens to share.
    kQueueAdoptionOff = 0x8000,
    // The sampler/anisotropy overrides never detour the game's real D3D12 device
    // vtable (CreateRootSignature, CreateSampler).
    kSamplerDeviceHooksOff = 0x10000,
};

// Parses the mask the way the environment variable is written: decimal, or
// hexadecimal with an 0x/0X prefix. Anything unparsable, negative, or empty
// means "no bit set", because a typo must never silently disable production
// behaviour.
inline uint32_t ParseMask(const char* text) {
    if (!text) {
        return 0;
    }
    while (*text == ' ' || *text == '\t') {
        ++text;
    }
    uint32_t base = 10;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text += 2;
    }
    if (*text == '\0') {
        return 0;
    }
    uint64_t value = 0;
    for (const char* c = text; *c != '\0'; ++c) {
        uint32_t digit = 0;
        if (*c >= '0' && *c <= '9') {
            digit = static_cast<uint32_t>(*c - '0');
        } else if (base == 16 && *c >= 'a' && *c <= 'f') {
            digit = static_cast<uint32_t>(*c - 'a') + 10;
        } else if (base == 16 && *c >= 'A' && *c <= 'F') {
            digit = static_cast<uint32_t>(*c - 'A') + 10;
        } else {
            return 0;
        }
        if (digit >= base) {
            return 0;
        }
        value = value * base + digit;
        if (value > 0xFFFFFFFFull) {
            return 0;
        }
    }
    return static_cast<uint32_t>(value);
}

// Resolved once per process from CE_FG_COST_PROBE.
uint32_t Mask();

inline bool Active(uint32_t bit) { return (Mask() & bit) != 0; }

}  // namespace ce::fg_cost_probe
