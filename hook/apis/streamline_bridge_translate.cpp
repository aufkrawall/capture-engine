#include "streamline_bridge_translate.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>

#include "../common/hook_common.h"
#include "streamline_bridge_policy.h"
#include "streamline_bridge_v1_abi.h"

// Streamline is a 64-bit runtime: there is no 32-bit sl.interposer, and the build already
// skips the FG SDK runtimes for x86. The hook DLL is built for both, so the translation
// compiles only where it can mean anything - the 1.x mirrors below are pointer-size
// dependent and their layout assertions are x64 facts, which is exactly how the 32-bit
// build caught this rather than silently mis-laying the structs out.
#if defined(_M_X64) || defined(__x86_64__)

#include "sl.h"
#include "sl_consts.h"
#include "sl_dlss.h"
#include "sl_dlss_g.h"
#include "sl_reflex.h"

namespace ce::streamline_bridge {
namespace {

// ---------------------------------------------------------------------------
// Resolved 2.x entry points
// ---------------------------------------------------------------------------

PFun_slSetTag* g_slSetTag = nullptr;
PFun_slSetConstants* g_slSetConstants = nullptr;
PFun_slGetNewFrameToken* g_slGetNewFrameToken = nullptr;
PFun_slEvaluateFeature* g_slEvaluateFeature = nullptr;
PFun_slIsFeatureSupported* g_slIsFeatureSupported = nullptr;
PFun_slGetFeatureFunction* g_slGetFeatureFunction = nullptr;
PFun_slSetD3DDevice* g_slSetD3DDevice = nullptr;

// Readiness, and how it is established.
//
// `g_runtimeUsable` is the gate every device-dependent translation reads, and it is set by
// exactly one thing: Streamline answering `slGetFeatureFunction`. Nothing else may set it.
//
// That rule is the lesson of session `20260821_163534`, which crashed the same way as the
// first bridged run - `Bridged_slSetConstants -> sl_interposer!slSetConstants+0x49 -> 0x0` -
// while the code believed the runtime was ready. It believed that because the game's
// `D3D12CreateDevice` had come through the bridge and been marked as proof. It was not proof:
// Streamline's own log shows that device was a probe the game threw away 400 ms later -
//     d3d12Device.cpp:396[Release] Destroyed D3D12Device proxy ... ref count 0
// - after which the plugin manager had no device at all and said so, repeatedly:
//     pluginManager.cpp:1331[initializePlugins] D3D or VK API hook is activated without
//     device being created, did you forget to call `slSetD3DDevice`
// while `slGetFeatureFunction` returned nothing for every feature. The runtime was telling
// CE the truth the whole time; CE had stopped asking because it had assumed an answer.
//
// So: a device is an *action* CE takes, never a conclusion CE draws.
std::atomic<bool> g_runtimeUsable{false};

// Asking Streamline is cheap but not free, and the gate sits in front of per-frame calls, so
// the probe is driven by events rather than repeated blindly. The epoch is bumped by the
// things that can actually change the answer: a device handed over, and a new frame - which
// is what a game reaching its render loop looks like, and is when Streamline finishes
// bringing plugins up around the swapchain. Not a timer, and not once-only.
std::atomic<uint32_t> g_readinessEpoch{1};
std::atomic<uint32_t> g_probedEpoch{0};
PFun_slDLSSSetOptions* g_slDLSSSetOptions = nullptr;
PFun_slDLSSGetOptimalSettings* g_slDLSSGetOptimalSettings = nullptr;
PFun_slDLSSGSetOptions* g_slDLSSGSetOptions = nullptr;
PFun_slReflexSetOptions* g_slReflexSetOptions = nullptr;

// Feature entry points exist only once a device is set - `slGetFeatureFunction` says so in
// the SDK header itself - so they are resolved from the calls that need them rather than at
// activation.
//
// Resolving them ONCE would be wrong, and quietly so. The first call that asks may well
// arrive before the game has created its device, in which case every pointer comes back
// null; a `std::call_once` around that latches those nulls for the life of the process and
// the bridge then refuses DLSS and DLSS-G forever with a message about the runtime "not
// having them yet" that would never stop being true. So each pointer is retried until it
// resolves, and the log line fires once, when the set is actually complete.
std::mutex g_featureFunctionMutex;

void ResolveFeatureFunctions() {
    if (g_slDLSSSetOptions && g_slDLSSGetOptimalSettings && g_slDLSSGSetOptions) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_featureFunctionMutex);
    if (!g_slGetFeatureFunction) {
        return;
    }
    if (!g_slDLSSSetOptions) {
        g_slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", reinterpret_cast<void*&>(g_slDLSSSetOptions));
    }
    if (!g_slDLSSGetOptimalSettings) {
        g_slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings",
                               reinterpret_cast<void*&>(g_slDLSSGetOptimalSettings));
    }
    if (!g_slDLSSGSetOptions) {
        g_slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions",
                               reinterpret_cast<void*&>(g_slDLSSGSetOptions));
    }
    if (!g_slReflexSetOptions) {
        g_slGetFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions",
                               reinterpret_cast<void*&>(g_slReflexSetOptions));
    }
    // Logged once the set is complete, and once more if it is still incomplete after the
    // runtime has become usable - reporting every failed attempt would be noise, because
    // failing before the device exists is the expected case this is polling for.
    static std::atomic<bool> logged{false};
    if ((g_slDLSSSetOptions && g_slDLSSGetOptimalSettings && g_slDLSSGSetOptions) &&
        !logged.exchange(true, std::memory_order_relaxed)) {
        HookLogImportant(
            "Streamline bridge: feature entry points %s - slDLSSSetOptions=%p slDLSSGetOptimalSettings=%p "
            "slDLSSGSetOptions=%p slReflexSetOptions=%p",
            (g_slDLSSSetOptions && g_slDLSSGetOptimalSettings && g_slDLSSGSetOptions && g_slReflexSetOptions)
                ? "resolved"
                : "PARTIALLY resolved",
            reinterpret_cast<void*>(g_slDLSSSetOptions), reinterpret_cast<void*>(g_slDLSSGetOptimalSettings),
            reinterpret_cast<void*>(g_slDLSSGSetOptions), reinterpret_cast<void*>(g_slReflexSetOptions));
    }
}

// One log line per distinct refusal reason, so a first bridged run diagnoses itself without
// a per-frame call turning the log into noise.
void RefuseOnce(std::atomic<bool>& latch, const char* what, const char* why) {
    if (!latch.exchange(true, std::memory_order_relaxed)) {
        HookLogImportant("Streamline bridge: refusing %s - %s", what, why);
    }
}

std::mutex g_deviceMutex;
void* g_lastDeviceHandedOver = nullptr;  // guarded by g_deviceMutex
bool g_haveExplicitDevice = false;       // guarded by g_deviceMutex

void BumpReadinessEpoch() { g_readinessEpoch.fetch_add(1, std::memory_order_relaxed); }

// Asks Streamline whether the feature contexts exist yet, at most once per epoch.
//
// `slGetFeatureFunction` succeeding is not a proxy for readiness, it IS readiness: it returns
// a pointer out of the very plugin context whose absence makes `slSetConstants` jump through
// null. The SDK header says as much - "can only be used AFTER device is set" - and it is the
// same question the runtime answers in its own log when the answer is no.
bool TryBecomeUsable() {
    if (g_runtimeUsable.load(std::memory_order_acquire)) {
        return true;
    }
    const uint32_t epoch = g_readinessEpoch.load(std::memory_order_relaxed);
    if (g_probedEpoch.exchange(epoch, std::memory_order_acq_rel) == epoch) {
        return false;  // nothing has changed since the last time this was asked
    }
    ResolveFeatureFunctions();
    if (!g_slDLSSSetOptions) {
        return false;
    }
    if (!g_runtimeUsable.exchange(true, std::memory_order_acq_rel)) {
        HookLogImportant(
            "Streamline bridge: the 2.x runtime answered slGetFeatureFunction - its feature contexts are up and "
            "the tag/constant/evaluate path is live from here");
    }
    return true;
}

// The single point every device-dependent translation passes through.
//
// One latch per call site would be neater to read but wrong to use: this is the invariant
// that two crashed sessions established, and it is worth one line naming the call that was
// held back.
bool DeviceReadyFor(V2Call call, std::atomic<bool>& latch) {
    if (!V2CallRequiresDevice(call) || TryBecomeUsable()) {
        return true;
    }
    if (!latch.exchange(true, std::memory_order_relaxed)) {
        HookLogImportant(
            "Streamline bridge: holding %s back - the 2.x runtime has no feature context yet. Forwarding it now "
            "would jump through a plugin pointer Streamline has not bound, which is a null call, not an error",
            DescribeV2Call(call));
    }
    return false;
}

bool ResultOk(sl::Result result, const char* call, std::atomic<bool>& latch) {
    if (result == sl::Result::eOk) {
        return true;
    }
    if (!latch.exchange(true, std::memory_order_relaxed)) {
        HookLogImportant("Streamline bridge: %s returned sl::Result=%d", call, static_cast<int>(result));
    }
    return false;
}

// 1.x threads a bare frame index; 2.x wants a token obtained once per frame and reused
// across that frame's constants, tags and evaluate. Cached so all three see the same one.
sl::FrameToken* TokenFor(uint32_t frameIndex) {
    static std::mutex mutex;
    static uint32_t cachedIndex = UINT32_MAX;
    static sl::FrameToken* cachedToken = nullptr;

    std::lock_guard<std::mutex> lock(mutex);
    if (cachedToken && cachedIndex == frameIndex) {
        return cachedToken;
    }
    if (!g_slGetNewFrameToken) {
        return nullptr;
    }
    sl::FrameToken* token = nullptr;
    if (g_slGetNewFrameToken(token, &frameIndex) != sl::Result::eOk || !token) {
        return nullptr;
    }
    cachedIndex = frameIndex;
    cachedToken = token;
    return token;
}

// 1.x `slSetTag` carries no command buffer, so tags are held until the next
// `slEvaluateFeature`, which supplies one. That is the same deferral the non-bridged 1.x
// overlay route already uses, and it still lands before the present DLSS-G consumes.
constexpr size_t kMaxPendingTags = 16;

// Held as plain data, not as `sl::Resource` / `sl::ResourceTag`.
//
// Those carry non-trivial constructors, and an array of them at namespace scope is a
// throwing static initialization inside a DLL - something that cannot be caught and that
// clang-tidy rightly refuses. Staging the fields instead and building the SDK structs on
// the stack when they are flushed keeps the hot path allocation-free and the module's
// static initialization trivial.
struct PendingTag {
    bool hasResource;
    bool hasExtent;
    uint32_t bufferType;
    uint32_t resourceType;
    void* native;
    void* memory;
    void* view;
    uint32_t state;
    uint32_t top, left, width, height;
};

std::mutex g_tagMutex;
PendingTag g_pendingTags[kMaxPendingTags]{};
size_t g_pendingTagCount = 0;
uint32_t g_pendingViewport = 0;

}  // namespace

bool ResolveTranslationTargets(void* v2InterposerModule) {
    auto module = static_cast<HMODULE>(v2InterposerModule);
    if (!module) {
        return false;
    }
    auto get = [module](const char* name) { return reinterpret_cast<void*>(GetProcAddress(module, name)); };

    g_slSetTag = reinterpret_cast<PFun_slSetTag*>(get("slSetTag"));
    g_slSetConstants = reinterpret_cast<PFun_slSetConstants*>(get("slSetConstants"));
    g_slGetNewFrameToken = reinterpret_cast<PFun_slGetNewFrameToken*>(get("slGetNewFrameToken"));
    g_slEvaluateFeature = reinterpret_cast<PFun_slEvaluateFeature*>(get("slEvaluateFeature"));
    g_slIsFeatureSupported = reinterpret_cast<PFun_slIsFeatureSupported*>(get("slIsFeatureSupported"));
    g_slGetFeatureFunction = reinterpret_cast<PFun_slGetFeatureFunction*>(get("slGetFeatureFunction"));
    g_slSetD3DDevice = reinterpret_cast<PFun_slSetD3DDevice*>(get("slSetD3DDevice"));

    const bool complete = g_slSetTag && g_slSetConstants && g_slGetNewFrameToken && g_slEvaluateFeature &&
                          g_slIsFeatureSupported && g_slGetFeatureFunction && g_slSetD3DDevice;
    if (!complete) {
        HookLogImportant(
            "Streamline bridge: the 2.x runtime is missing an entry point the translation needs (setTag=%p "
            "setConstants=%p frameToken=%p evaluate=%p isSupported=%p getFeatureFunction=%p setD3DDevice=%p) - "
            "not bridging",
            reinterpret_cast<void*>(g_slSetTag), reinterpret_cast<void*>(g_slSetConstants),
            reinterpret_cast<void*>(g_slGetNewFrameToken), reinterpret_cast<void*>(g_slEvaluateFeature),
            reinterpret_cast<void*>(g_slIsFeatureSupported), reinterpret_cast<void*>(g_slGetFeatureFunction),
            reinterpret_cast<void*>(g_slSetD3DDevice));
    }
    return complete;
}

bool V2RuntimeHasDevice() { return g_runtimeUsable.load(std::memory_order_acquire); }

bool SetV2RuntimeDevice(void* d3d12Device, bool explicitHandoff) {
    if (!d3d12Device) {
        return false;
    }
    // Hand over every DISTINCT device CE learns of, not just the first.
    //
    // The first one is routinely a throwaway: The Witcher 3 creates a device, has Streamline
    // proxy it, and releases it 400 ms later - `Destroyed D3D12Device proxy ... ref count 0`
    // in Streamline's own log - long before the device it actually renders with exists. Code
    // that hands over only the first device gives Streamline the one that is about to die and
    // never the one that matters, which is precisely how `20260821_163534` ended up with a
    // runtime whose plugin manager kept asking, by name, for the call CE had already decided
    // it had made.
    sl::Result result = sl::Result::eErrorNotInitialized;
    bool attempted = false;
    {
        // `slSetD3DDevice` is documented as NOT thread safe, and CE reaches here from two
        // independent discovery routes, so exactly one caller may be inside it.
        std::lock_guard<std::mutex> lock(g_deviceMutex);
        // Never let the queue-derived fallback overwrite an explicitly selected device.
        const bool superseded = !explicitHandoff && g_haveExplicitDevice;
        if (!superseded && g_lastDeviceHandedOver != d3d12Device && g_slSetD3DDevice) {
            result = g_slSetD3DDevice(d3d12Device);
            g_lastDeviceHandedOver = d3d12Device;
            g_haveExplicitDevice = g_haveExplicitDevice || explicitHandoff;
            attempted = true;
        }
    }
    if (attempted) {
        HookLogImportant(
            "Streamline bridge: handed %s device %p to the 2.x runtime (slSetD3DDevice sl::Result=%d)",
            explicitHandoff ? "an explicitly handed" : "the game's queue-derived", d3d12Device,
            static_cast<int>(result));
        BumpReadinessEpoch();
    }
    return TryBecomeUsable();
}

// The game's own slInit. CE has already initialised the 2.x runtime with the pinned plugin
// folder before any import slot was repointed, so there is nothing left to do and nothing
// of the game's 1.x Preferences that could improve on it - its plugin paths point at the
// 1.x set. Reporting success is what lets the game go on to configure DLSS at all.
bool TranslateInit(const void* /*preferences1x*/, int /*applicationId*/) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        HookLogImportant(
            "Streamline bridge: the game called slInit; the CE-owned 2.x runtime is already up, so this reports "
            "success without re-initialising");
    }
    return true;
}

// CE owns the runtime's lifetime, and the game shutting its Streamline down must not tear
// down a runtime CE also drives for the overlay.
bool TranslateShutdown() { return true; }

bool TranslateIsFeatureSupported(uint32_t feature1x, uint32_t* adapterBitMask) {
    uint32_t feature2x = 0;
    if (!TranslateV1FeatureToV2(feature1x, &feature2x)) {
        static std::atomic<bool> latch{false};
        RefuseOnce(latch, "slIsFeatureSupported", "the 1.x feature has no faithful 2.x equivalent");
        return false;
    }
    if (!g_slIsFeatureSupported) {
        return false;
    }
    // 1.x answers with a bitmask of supporting adapters; 2.x asks about one adapter. CE
    // drives a single device, so an empty AdapterInfo asks "is this feature available at
    // all", and every adapter bit is reported set when it is.
    sl::AdapterInfo adapter{};
    const sl::Result result = g_slIsFeatureSupported(feature2x, adapter);
    const bool supported = (result == sl::Result::eOk);
    if (adapterBitMask) {
        *adapterBitMask = supported ? ~0u : 0u;
    }
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        HookLogImportant("Streamline bridge: slIsFeatureSupported(%s) -> %s (sl::Result=%d)",
                         DescribeV1Feature(feature1x), supported ? "yes" : "no", static_cast<int>(result));
    }
    return supported;
}

bool TranslateSetTag(const void* resource1x, uint32_t bufferType, uint32_t id, const void* extent1x) {
    // Tags are only staged here, but staging one for a runtime that may never receive it
    // just grows a buffer that the flush would then push into a deviceless runtime.
    static std::atomic<bool> deviceLatch{false};
    if (!DeviceReadyFor(V2Call::SetTag, deviceLatch)) {
        return false;
    }
    uint32_t bufferType2x = 0;
    if (!TranslateV1BufferTypeToV2(bufferType, &bufferType2x)) {
        static std::atomic<bool> latch{false};
        RefuseOnce(latch, "slSetTag", "the buffer type is outside the verified 1.x table");
        return false;
    }

    std::lock_guard<std::mutex> lock(g_tagMutex);
    if (g_pendingTagCount >= kMaxPendingTags) {
        static std::atomic<bool> latch{false};
        RefuseOnce(latch, "slSetTag", "more tags are pending than one frame should ever carry");
        return false;
    }

    PendingTag& pending = g_pendingTags[g_pendingTagCount];
    pending = {};
    pending.bufferType = bufferType2x;

    // A null resource is a legitimate 1.x "untag this slot".
    if (resource1x) {
        const auto* source = static_cast<const V1Resource*>(resource1x);
        pending.hasResource = true;
        pending.resourceType = source->type;
        pending.native = source->native;
        pending.memory = source->memory;
        pending.view = source->view;
        pending.state = source->state;
    }
    if (extent1x) {
        const auto* source = static_cast<const V1Extent*>(extent1x);
        pending.hasExtent = true;
        pending.top = source->top;
        pending.left = source->left;
        pending.width = source->width;
        pending.height = source->height;
    }
    g_pendingViewport = id;
    ++g_pendingTagCount;
    return true;
}

bool TranslateSetConstants(const void* constants1x, uint32_t frameIndex, uint32_t id) {
    if (!constants1x || !g_slSetConstants) {
        return false;
    }
    // A frame boundary is the other thing that can change the readiness answer: Streamline
    // finishes bringing DLSS-G's context up around swapchain creation and the first presents,
    // which CE observes here as the game's frame index moving. Re-asking once per frame while
    // the answer is no converges without a timer and stops entirely once it is yes.
    {
        static std::atomic<uint32_t> lastFrame{UINT32_MAX};
        if (lastFrame.exchange(frameIndex, std::memory_order_relaxed) != frameIndex) {
            BumpReadinessEpoch();
        }
    }
    static std::atomic<bool> deviceLatch{false};
    if (!DeviceReadyFor(V2Call::SetConstants, deviceLatch)) {
        return false;
    }
    sl::FrameToken* token = TokenFor(frameIndex);
    if (!token) {
        static std::atomic<bool> latch{false};
        RefuseOnce(latch, "slSetConstants", "the 2.x runtime would not issue a frame token");
        return false;
    }

    const auto& in = *static_cast<const V1Constants*>(constants1x);
    sl::Constants out{};  // leaves 2.x-only fields at their defaults, which is what they are for
    memcpy(&out.cameraViewToClip, &in.cameraViewToClip, sizeof(V1Float4x4) * 5);
    memcpy(&out.jitterOffset, &in.jitterOffset, sizeof(V1Float2) * 3);
    memcpy(&out.cameraPos, &in.cameraPos, sizeof(V1Float3) * 4);
    out.cameraNear = in.cameraNear;
    out.cameraFar = in.cameraFar;
    out.cameraFOV = in.cameraFOV;
    out.cameraAspectRatio = in.cameraAspectRatio;
    out.motionVectorsInvalidValue = in.motionVectorsInvalidValue;
    out.depthInverted = static_cast<sl::Boolean>(in.depthInverted);
    out.cameraMotionIncluded = static_cast<sl::Boolean>(in.cameraMotionIncluded);
    out.motionVectors3D = static_cast<sl::Boolean>(in.motionVectors3D);
    out.reset = static_cast<sl::Boolean>(in.reset);
    out.orthographicProjection = static_cast<sl::Boolean>(in.orthographicProjection);
    out.motionVectorsDilated = static_cast<sl::Boolean>(in.motionVectorsDilated);
    out.motionVectorsJittered = static_cast<sl::Boolean>(in.motionVectorsJittered);
    // `notRenderingGameFrames` has no 2.x field and is deliberately dropped;
    // `minRelativeLinearDepthObjectSeparation` keeps its 40.0f default rather than a zero.

    static std::atomic<bool> latch{false};
    return ResultOk(g_slSetConstants(out, *token, sl::ViewportHandle(id)), "slSetConstants", latch);
}

bool TranslateSetFeatureConstants(uint32_t feature1x, const void* constants1x, uint32_t /*frameIndex*/,
                                  uint32_t id) {
    if (!constants1x) {
        return false;
    }
    static std::atomic<bool> deviceLatch{false};
    if (!DeviceReadyFor(V2Call::SetFeatureConstants, deviceLatch)) {
        return false;
    }
    ResolveFeatureFunctions();

    if (feature1x == kV1FeatureDLSS) {
        if (!g_slDLSSSetOptions) {
            static std::atomic<bool> latch{false};
            RefuseOnce(latch, "slSetFeatureConstants(DLSS)", "the 2.x runtime has no slDLSSSetOptions yet");
            return false;
        }
        const auto& in = *static_cast<const V1DLSSConstants*>(constants1x);
        sl::DLSSOptions options{};
        options.mode = static_cast<sl::DLSSMode>(in.mode);
        options.outputWidth = in.outputWidth;
        options.outputHeight = in.outputHeight;
        // `sharpness` is deprecated in 2.x and ignored, so it is deliberately not carried over.
        options.preExposure = in.preExposure;
        options.exposureScale = in.exposureScale;
        options.colorBuffersHDR = static_cast<sl::Boolean>(in.colorBuffersHDR);
        static std::atomic<bool> latch{false};
        return ResultOk(g_slDLSSSetOptions(sl::ViewportHandle(id), options), "slDLSSSetOptions", latch);
    }

    if (feature1x == kV1FeatureDLSS_G) {
        if (!g_slDLSSGSetOptions) {
            static std::atomic<bool> latch{false};
            RefuseOnce(latch, "slSetFeatureConstants(DLSS-G)", "the 2.x runtime has no slDLSSGSetOptions yet");
            return false;
        }
        const auto& in = *static_cast<const V1DLSSGConstants*>(constants1x);
        sl::DLSSGOptions options{};
        options.mode = (in.mode == 0) ? sl::DLSSGMode::eOff : sl::DLSSGMode::eOn;
        // Left at whatever 1.x asked for; CE's own dlss_fg_factor override applies later,
        // on its existing slDLSSGSetOptions hook, exactly as it does for a native 2.x game.
        options.numFramesToGenerate = in.numFramesToGenerate ? in.numFramesToGenerate : 1;
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed)) {
            HookLogImportant("Streamline bridge: first DLSS-G options translated - mode=%u numFramesToGenerate=%u",
                             in.mode, options.numFramesToGenerate);
        }
        static std::atomic<bool> latch{false};
        return ResultOk(g_slDLSSGSetOptions(sl::ViewportHandle(id), options), "slDLSSGSetOptions", latch);
    }

    // Reflex. 1.x configures it through slSetFeatureConstants; 2.x through slReflexSetOptions.
    //
    // This is not optional for the feature the bridge exists to deliver: DLSS-G does not engage
    // with Reflex off, so refusing this call - which is what the bridge did at first - would
    // leave frame generation configured and inert. Only `mode` is carried, because only `mode`
    // was measured; see V1ReflexConstants for why the rest of that capture is stack, not struct.
    if (feature1x == kV1FeatureReflex) {
        if (!g_slReflexSetOptions) {
            static std::atomic<bool> latch{false};
            RefuseOnce(latch, "slSetFeatureConstants(Reflex)", "the 2.x runtime has no slReflexSetOptions yet");
            return false;
        }
        const auto& in = *static_cast<const V1ReflexConstants*>(constants1x);
        // eOff / eLowLatency / eLowLatencyWithBoost, identical in both generations. Anything
        // outside that is refused rather than cast into an enum it does not belong to.
        if (in.mode > static_cast<uint32_t>(sl::ReflexMode::eLowLatencyWithBoost)) {
            static std::atomic<bool> latch{false};
            RefuseOnce(latch, "slSetFeatureConstants(Reflex)", "the 1.x Reflex mode is outside the known range");
            return false;
        }
        sl::ReflexOptions options{};
        options.mode = static_cast<sl::ReflexMode>(in.mode);
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed)) {
            HookLogImportant("Streamline bridge: first Reflex options translated - mode=%u", in.mode);
        }
        static std::atomic<bool> latch{false};
        return ResultOk(g_slReflexSetOptions(options), "slReflexSetOptions", latch);
    }

    static std::atomic<bool> latch{false};
    RefuseOnce(latch, "slSetFeatureConstants",
               "only DLSS, DLSS-G and Reflex constants have a verified 1.x layout");
    return false;
}

bool TranslateGetFeatureSettings(uint32_t feature1x, const void* constants1x, void* settings1x) {
    if (feature1x != kV1FeatureDLSS || !constants1x || !settings1x) {
        static std::atomic<bool> latch{false};
        RefuseOnce(latch, "slGetFeatureSettings", "only the DLSS settings query has a verified 1.x layout");
        return false;
    }
    static std::atomic<bool> deviceLatch{false};
    if (!DeviceReadyFor(V2Call::GetFeatureSettings, deviceLatch)) {
        return false;
    }
    ResolveFeatureFunctions();
    if (!g_slDLSSGetOptimalSettings) {
        return false;
    }

    const auto& in = *static_cast<const V1DLSSConstants*>(constants1x);
    sl::DLSSOptions options{};
    options.mode = static_cast<sl::DLSSMode>(in.mode);
    options.outputWidth = in.outputWidth;
    options.outputHeight = in.outputHeight;

    sl::DLSSOptimalSettings optimal{};
    static std::atomic<bool> latch{false};
    if (!ResultOk(g_slDLSSGetOptimalSettings(options, optimal), "slDLSSGetOptimalSettings", latch)) {
        return false;
    }

    // Only the three fields the real 1.5.6 runtime was measured filling. It returned zeroes
    // for everything past them, so writing more would invent behaviour it never had.
    auto& out = *static_cast<V1DLSSSettings*>(settings1x);
    out.optimalRenderWidth = optimal.optimalRenderWidth;
    out.optimalRenderHeight = optimal.optimalRenderHeight;
    out.optimalSharpness = optimal.optimalSharpness;
    return true;
}

bool TranslateEvaluateFeature(void* commandBuffer, uint32_t feature1x, uint32_t frameIndex, uint32_t id) {
    uint32_t feature2x = 0;
    if (!TranslateV1FeatureToV2(feature1x, &feature2x)) {
        static std::atomic<bool> latch{false};
        RefuseOnce(latch, "slEvaluateFeature", "the 1.x feature has no faithful 2.x equivalent");
        return false;
    }
    if (!g_slEvaluateFeature || !commandBuffer) {
        return false;
    }
    static std::atomic<bool> deviceLatch{false};
    if (!DeviceReadyFor(V2Call::EvaluateFeature, deviceLatch)) {
        return false;
    }
    sl::FrameToken* token = TokenFor(frameIndex);
    if (!token) {
        static std::atomic<bool> latch{false};
        RefuseOnce(latch, "slEvaluateFeature", "the 2.x runtime would not issue a frame token");
        return false;
    }

    // Flush the tags 1.x had no command buffer to submit with. This is the point at which
    // one exists, and it still precedes the present that DLSS-G consumes.
    {
        std::lock_guard<std::mutex> lock(g_tagMutex);
        if (g_pendingTagCount > 0 && g_slSetTag) {
            // Built here rather than held: see PendingTag on why these types must not have
            // static storage. The resources have to outlive the slSetTag call, so both
            // arrays live in this scope together.
            sl::Resource resources[kMaxPendingTags]{};
            sl::ResourceTag tags[kMaxPendingTags]{};
            for (size_t i = 0; i < g_pendingTagCount; ++i) {
                const PendingTag& pending = g_pendingTags[i];
                sl::Resource* resource = nullptr;
                if (pending.hasResource) {
                    resources[i].type =
                        (pending.resourceType == 0) ? sl::ResourceType::eTex2d : sl::ResourceType::eBuffer;
                    resources[i].native = pending.native;
                    resources[i].memory = pending.memory;
                    resources[i].view = pending.view;
                    resources[i].state = pending.state;
                    resource = &resources[i];
                }
                tags[i] = sl::ResourceTag(resource, static_cast<sl::BufferType>(pending.bufferType),
                                          sl::ResourceLifecycle::eValidUntilPresent);
                if (pending.hasExtent) {
                    tags[i].extent.top = pending.top;
                    tags[i].extent.left = pending.left;
                    tags[i].extent.width = pending.width;
                    tags[i].extent.height = pending.height;
                }
            }
            static std::atomic<bool> tagLatch{false};
            ResultOk(g_slSetTag(sl::ViewportHandle(g_pendingViewport), tags,
                                static_cast<uint32_t>(g_pendingTagCount),
                                static_cast<sl::CommandBuffer*>(commandBuffer)),
                     "slSetTag", tagLatch);
            g_pendingTagCount = 0;
        }
    }

    // The viewport travels in the input chain, not as a parameter. 1.x threads it through
    // every call as a bare `id`, and the 2.x header is explicit that "frame and viewport
    // must match whatever is used to set common and or feature options and constants" -
    // which for this bridge means the same `id` that slSetConstants and slDLSSSetOptions
    // already used. Evaluating with no inputs silently evaluates viewport 0 instead, so a
    // title that renders on any other one would upscale a viewport it never configured.
    sl::ViewportHandle viewport(id);
    const sl::BaseStructure* inputs[] = {&viewport};

    static std::atomic<bool> latch{false};
    return ResultOk(g_slEvaluateFeature(feature2x, *token, inputs, 1,
                                        static_cast<sl::CommandBuffer*>(commandBuffer)),
                    "slEvaluateFeature", latch);
}

}  // namespace ce::streamline_bridge

#else  // 32-bit: no Streamline runtime exists, so nothing can be translated.

namespace ce::streamline_bridge {

namespace {
void RefuseOn32Bit(const char* call) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        HookLogImportant(
            "Streamline bridge: %s reached the 32-bit hook, but Streamline is a 64-bit runtime and the bridge "
            "never activates here",
            call);
    }
}
}  // namespace

bool ResolveTranslationTargets(void*) { return false; }
bool SetV2RuntimeDevice(void*, bool) { return false; }
bool V2RuntimeHasDevice() { return false; }
bool TranslateInit(const void*, int) { RefuseOn32Bit("slInit"); return false; }
bool TranslateShutdown() { return false; }
bool TranslateIsFeatureSupported(uint32_t, uint32_t*) { return false; }
bool TranslateSetTag(const void*, uint32_t, uint32_t, const void*) { return false; }
bool TranslateSetConstants(const void*, uint32_t, uint32_t) { return false; }
bool TranslateSetFeatureConstants(uint32_t, const void*, uint32_t, uint32_t) { return false; }
bool TranslateGetFeatureSettings(uint32_t, const void*, void*) { return false; }
bool TranslateEvaluateFeature(void*, uint32_t, uint32_t, uint32_t) { return false; }

}  // namespace ce::streamline_bridge

#endif  // x64
