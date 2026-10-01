#include "streamline_bridge_translate.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "../common/hook_common.h"
#include "streamline_bridge_policy.h"
#include "streamline_bridge_v1_abi.h"

// Streamline is a 64-bit runtime: there is no 32-bit sl.interposer, and the build already
// skips the FG SDK runtimes for x86. The hook DLL is built for both, so the translation
// compiles only where it can mean anything - the 1.x mirrors below are pointer-size
// dependent and their layout assertions are x64 facts, which is exactly how the 32-bit
// build caught this rather than silently mis-laying the structs out.
#if defined(_M_X64) || defined(__x86_64__)

#include <d3d12.h>
#include <wrl/client.h>

#include "sl.h"
#include "sl_consts.h"
#include "sl_dlss.h"
#include "sl_dlss_g.h"
#include "sl_reflex.h"
#include "streamline_bridge_diag.h"
#include "streamline_bridge_dlssg.h"
#include "streamline_bridge_dlssg_gate.h"
#include "streamline_bridge_present.h"
#include "streamline_bridge_reflex.h"

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
    if (g_slDLSSSetOptions && g_slDLSSGetOptimalSettings && g_slDLSSGSetOptions &&
        ResolveReflexFunctions(g_slGetFeatureFunction)) {
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
    // Report only the complete set; failures before the device exists are the expected reason
    // this event-driven path retries, not useful per-attempt diagnostics.
    static std::atomic<bool> logged{false};
    const bool reflexComplete = ResolveReflexFunctions(g_slGetFeatureFunction);
    const bool complete = g_slDLSSSetOptions && g_slDLSSGetOptimalSettings && g_slDLSSGSetOptions && reflexComplete;
    if (complete && !logged.exchange(true, std::memory_order_relaxed)) {
        HookLogImportant(
            "Streamline bridge: feature entry points resolved - slDLSSSetOptions=%p "
            "slDLSSGetOptimalSettings=%p slDLSSGSetOptions=%p slReflexSetOptions=%p slReflexSleep=%p",
            reinterpret_cast<void*>(g_slDLSSSetOptions), reinterpret_cast<void*>(g_slDLSSGetOptimalSettings),
            reinterpret_cast<void*>(g_slDLSSGSetOptions), ReflexSetOptionsEntry(), ReflexSleepEntry());
    }
}

std::mutex g_deviceMutex;
Microsoft::WRL::ComPtr<IUnknown> g_lastDeviceIdentity;  // guarded by g_deviceMutex
bool g_haveExplicitDevice = false;                      // guarded by g_deviceMutex

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
    PFun_slDLSSSetOptions* readinessFunction = nullptr;
    if (g_slGetFeatureFunction) {
        g_slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions",
                               reinterpret_cast<void*&>(readinessFunction));
    }
    if (!readinessFunction) {
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

// 1.x threads a bare frame index; 2.x wants one token per frame, reused across that frame's
// constants, tags, markers and evaluate - on whichever thread they arrive. See RecentFrameTokens.
std::mutex g_tokenMutex;
RecentFrameTokens<sl::FrameToken, 16> g_frameTokens;

sl::FrameToken* TokenFor(uint32_t frameIndex) {
    std::lock_guard<std::mutex> lock(g_tokenMutex);
    if (sl::FrameToken* known = g_frameTokens.Find(frameIndex)) {
        return known;
    }
    if (!g_slGetNewFrameToken) {
        return nullptr;
    }
    sl::FrameToken* token = nullptr;
    if (g_slGetNewFrameToken(token, &frameIndex) != sl::Result::eOk || !token) {
        return nullptr;
    }
    g_frameTokens.Remember(frameIndex, token);
    return token;
}

// The newest frame's token, for a 1.x call that names no frame (sleep passes 0).
sl::FrameToken* LatestToken() {
    std::lock_guard<std::mutex> lock(g_tokenMutex);
    return g_frameTokens.Latest();
}

// 1.x `slSetTag` carries no command buffer, so tags are held until the next
// `slEvaluateFeature`, which supplies one. That is the same deferral the non-bridged 1.x
// overlay route already uses, and it still lands before the present DLSS-G consumes.
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
    Microsoft::WRL::ComPtr<ID3D12Device> nativeDevice;
    Microsoft::WRL::ComPtr<IUnknown> identity;
    const HRESULT deviceHr = static_cast<IUnknown*>(d3d12Device)->QueryInterface(IID_PPV_ARGS(&nativeDevice));
    const HRESULT identityHr = SUCCEEDED(deviceHr) ? nativeDevice.As(&identity) : deviceHr;
    if (FAILED(identityHr)) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed)) {
            HookLogImportant("Streamline bridge: refusing D3D12 device handoff because COM identity "
                             "normalization failed (device=%p hr=0x%08X)",
                             d3d12Device, static_cast<uint32_t>(identityHr));
        }
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
        const bool sameDevice = g_lastDeviceIdentity.Get() == identity.Get();
        if (!superseded && sameDevice && explicitHandoff) {
            g_haveExplicitDevice = true;
        } else if (!superseded && !sameDevice && g_slSetD3DDevice) {
            // Calls already admitted for the prior device must stop until the plugin manager
            // proves its contexts survived or completed this genuinely new handoff.
            g_runtimeUsable.store(false, std::memory_order_release);
            result = g_slSetD3DDevice(nativeDevice.Get());
            if (result == sl::Result::eOk) {
                g_lastDeviceIdentity = identity;
                g_haveExplicitDevice = g_haveExplicitDevice || explicitHandoff;
            }
            attempted = true;
        }
    }
    if (attempted) {
        HookLogImportant(
            "Streamline bridge: handed %s device %p to the 2.x runtime (slSetD3DDevice sl::Result=%d)",
            explicitHandoff ? "an explicitly handed" : "the game's queue-derived", nativeDevice.Get(),
            static_cast<int>(result));
        if (result == sl::Result::eOk) {
            BumpReadinessEpoch();
        }
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
    NoteTitleActivity(TitleActivity::kTag, 0);
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

    if (!g_slSetTag) {
        return false;
    }

    // Translate immediately. 2.x's deprecated slSetTag explicitly permits a null command
    // buffer when every tag is eValidUntilPresent, which is exactly the 1.x call shape.
    // Deferring these tags until evaluate mixed frames/viewports once the title staged more
    // than the arbitrary queue capacity, after which evaluate had neither the complete input
    // set nor valid SR resources (`20260822_005204`).
    sl::Resource resource{};
    if (resource1x) {
        const auto* source = static_cast<const V1Resource*>(resource1x);
        resource.type = (source->type == 0) ? sl::ResourceType::eTex2d : sl::ResourceType::eBuffer;
        resource.native = source->native;
        resource.memory = source->memory;
        resource.view = source->view;
        resource.state = source->state;
    }

    sl::Extent extent{};
    if (extent1x) {
        const auto in = *static_cast<const V1Extent*>(extent1x);
        extent.top = in.top;
        extent.left = in.left;
        extent.width = in.width;
        extent.height = in.height;
    }
    sl::ResourceTag tag(resource1x ? &resource : nullptr, static_cast<sl::BufferType>(bufferType2x),
                        sl::ResourceLifecycle::eValidUntilPresent, extent1x ? &extent : nullptr);
    sl::ResourceTag tags[] = {tag};
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        HookLogImportant("Streamline bridge: first slSetTag translated - viewport=%u buffer=%u resource=%p",
                         id, bufferType2x, resource1x ? resource.native : nullptr);
    }
    static std::atomic<bool> latch{false};
    if (!ResultOk(g_slSetTag(sl::ViewportHandle(id), tags, 1, nullptr), "slSetTag", latch)) {
        return false;
    }
    RememberPresentTag(id, bufferType2x, resource1x ? &resource : nullptr, extent1x ? &extent : nullptr);
    return true;
}

namespace {
// Runs on the title's present-end marker: re-issues its DLSS-G input tags so they outlive one
// extra present, as 1.x tags did (see V1TagPersistsAcrossPresents).
void RefreshPersistentPresentTags(uint32_t frameIndex) {
    std::vector<PersistentTag> persistent;
    bool titleRetagged = true;
    if (!g_slSetTag || !TakePresentTagsForRefresh(persistent, titleRetagged)) {
        return;
    }
    uint32_t failures = 0;
    for (PersistentTag& entry : persistent) {
        sl::ResourceTag tag(&entry.resource, static_cast<sl::BufferType>(entry.bufferType),
                            sl::ResourceLifecycle::eValidUntilPresent, entry.haveExtent ? &entry.extent : nullptr);
        if (g_slSetTag(sl::ViewportHandle(entry.viewport), &tag, 1, nullptr) != sl::Result::eOk) {
            ++failures;
        }
    }
    static std::atomic<bool> loggedFirst{false};
    if (!loggedFirst.exchange(true, std::memory_order_relaxed)) {
        HookLogImportant("Streamline bridge: re-issuing %zu DLSS-G input tag(s) after each present so they outlive "
                         "an extra present, as 1.x tags did (frame %u, failures=%u)",
                         persistent.size(), frameIndex, failures);
    }
    if (!titleRetagged || failures) {
        static std::atomic<uint32_t> gaps{0};
        const uint32_t n = gaps.fetch_add(1, std::memory_order_relaxed);
        if (n < 32 || (n % 256) == 0) {
            HookLogImportant("Streamline bridge: title presented again without re-tagging (frame %u) - kept its last "
                             "%zu DLSS-G input tag(s) alive (failures=%u) #%u",
                             frameIndex, persistent.size(), failures, n + 1);
        }
    }
}
}  // namespace

bool TranslateSetConstants(const void* constants1x, uint32_t frameIndex, uint32_t id) {
    if (!constants1x || !g_slSetConstants) {
        return false;
    }
    NoteTitleActivity(TitleActivity::kConstants, frameIndex);
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
    // `notRenderingGameFrames` has no 2.x field; it gates DLSS-G below, as 1.x did.
    // `minRelativeLinearDepthObjectSeparation` keeps its 40.0f default rather than a zero.

    // Sleep before forwarding common state: this is the earliest point in the translated
    // frame where both the frame boundary and the complete FrameToken exist.
    MaybeSynthesizeReflexSleep(frameIndex, token, DlssgEnabledOnAnyViewport());

    const sl::Result result = g_slSetConstants(out, *token, sl::ViewportHandle(id));
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        HookLogImportant(
            "Streamline bridge: first slSetConstants translated - inputFrame=%u tokenFrame=%u viewport=%u "
            "sl::Result=%d",
            frameIndex, static_cast<uint32_t>(*token), id, static_cast<int>(result));
    }
    // A history reset is the title's own camera-cut signal; when DLSS-G misbehaves around a load
    // or cut, whether one arrived is the first question.
    if (in.reset != 0) {
        static std::atomic<uint32_t> resets{0};
        const uint32_t n = resets.fetch_add(1, std::memory_order_relaxed);
        if (n < 16 || (n % 256) == 0) {
            HookLogImportant("Streamline bridge: title requested a history reset on frame %u viewport %u (raw=%u) #%u",
                             frameIndex, id, in.reset, n + 1);
        }
    }
    ApplyNotRenderingGameFrames(g_slDLSSGSetOptions, id, frameIndex, in.notRenderingGameFrames);
    static std::atomic<bool> latch{false};
    return ResultOk(result, "slSetConstants", latch);
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
        return TranslateDlssgConstants(g_slDLSSGSetOptions, in.mode, in.numFramesToGenerate, id);
    }

    if (feature1x == kV1FeatureReflex) {
        return TranslateReflexConstants(constants1x, DlssgTitleRequestsOn(id));
    }

    static std::atomic<bool> latch{false};
    RefuseOnce(latch, "slSetFeatureConstants",
               "only DLSS, DLSS-G and Reflex constants have a verified 1.x layout");
    return false;
}

bool TranslateGetFeatureSettings(uint32_t feature1x, const void* constants1x, void* settings1x) {
    if (feature1x == kV1FeatureReflex) {
        // The 1.x guide passes null constants for this query. Device-dependent like the rest.
        static std::atomic<bool> reflexDeviceLatch{false};
        if (!DeviceReadyFor(V2Call::GetFeatureSettings, reflexDeviceLatch)) {
            return false;
        }
        ResolveFeatureFunctions();
        return TranslateReflexSettings(settings1x);
    }
    if (feature1x != kV1FeatureDLSS || !constants1x || !settings1x) {
        static std::atomic<bool> latch{false};
        RefuseOnce(latch, "slGetFeatureSettings", "only DLSS and Reflex settings have a verified 1.x layout");
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
    static std::atomic<bool> deviceLatch{false};
    NoteTitleActivity(feature1x == kV1FeatureReflex ? TitleActivity::kMarker : TitleActivity::kEvaluate, frameIndex,
                      id);
    if (feature1x == kV1FeatureReflex) {
        // Markers and sleep: `id` is the marker and the command buffer is null by design.
        if (!DeviceReadyFor(V2Call::EvaluateFeature, deviceLatch)) {
            return false;
        }
        ResolveFeatureFunctions();
        const bool namesFrame = frameIndex != 0 || id != kV1ReflexMarkerSleep;
        const bool forwarded =
            TranslateReflexEvaluate(id, frameIndex, namesFrame ? TokenFor(frameIndex) : LatestToken());
        if (id == kV1ReflexMarkerSleep) {
            NoteTitleSleepReturned();
        }
        if (id == kV1ReflexMarkerPresentEnd) {
            RefreshPersistentPresentTags(frameIndex);
        }
        return forwarded;
    }
    if (!g_slEvaluateFeature || !commandBuffer) {
        return false;
    }
    if (!DeviceReadyFor(V2Call::EvaluateFeature, deviceLatch)) {
        return false;
    }
    sl::FrameToken* token = TokenFor(frameIndex);
    if (!token) {
        static std::atomic<bool> latch{false};
        RefuseOnce(latch, "slEvaluateFeature", "the 2.x runtime would not issue a frame token");
        return false;
    }
    MaybeSynthesizeReflexSleep(frameIndex, token, DlssgEnabledOnAnyViewport());

    // The viewport travels in the input chain, not as a parameter. 1.x threads it through
    // every call as a bare `id`, and the 2.x header is explicit that "frame and viewport
    // must match whatever is used to set common and or feature options and constants" -
    // which for this bridge means the same `id` that slSetConstants and slDLSSSetOptions
    // already used. Evaluating with no inputs silently evaluates viewport 0 instead, so a
    // title that renders on any other one would upscale a viewport it never configured.
    sl::ViewportHandle viewport(id);
    const sl::BaseStructure* inputs[] = {&viewport};

    const sl::Result result = g_slEvaluateFeature(feature2x, *token, inputs, 1,
                                                  static_cast<sl::CommandBuffer*>(commandBuffer));
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        HookLogImportant(
            "Streamline bridge: first slEvaluateFeature translated - feature=%u->%u inputFrame=%u "
            "tokenFrame=%u viewport=%u commandBuffer=%p sl::Result=%d",
            feature1x, feature2x, frameIndex, static_cast<uint32_t>(*token), id, commandBuffer,
            static_cast<int>(result));
    }
    static std::atomic<bool> latch{false};
    return ResultOk(result, "slEvaluateFeature", latch);
}

// Only a token the title's frame already has: issuing one for an old index would move
// Streamline's frame counter backwards (see RecentFrameTokens).
bool SynthesizePresentMarkersFor(uint32_t frameIndex) {
    const sl::FrameToken* token = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_tokenMutex);
        token = g_frameTokens.Find(frameIndex);
    }
    return token && SynthesizePresentMarkers(*token);
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
