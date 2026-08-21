#pragma once

#include <cstdint>

// The 1.x -> 2.x call translation behind `streamline_upgrade`.
//
// `streamline_bridge.cpp` owns the game's import slots; this unit turns each 1.x call it
// receives into the 2.x call that does the same thing against the CE-owned runtime.
//
// Every layout used here is either measured or taken from a real header, never inferred:
//   * The 2.x side uses the actual Streamline SDK headers. The hook DLL gets them on its
//     include path specifically so this file does not have to hand-mirror eight structs -
//     the one mirror that was hand-written (sl::Preferences) was wrong on its first write.
//   * The 1.x side is mirrored, because no 1.5.6 header exists publicly. `Constants` comes
//     from two independent sources that agree (upstream v1.1.1 and OptiScaler's vendored
//     SL1 set). `DLSSConstants`, `DLSSSettings`, `ReflexConstants` and `DLSSGConstants`
//     were measured from a real Witcher 3 session by CE's own probe
//     (`streamline_v1_feature_probe.cpp`, session `20260821_042540`).
//
// Anything that cannot be translated faithfully refuses rather than guessing, and says so
// once in the log. A refused call leaves the game without that Streamline feature; a
// guessed one would leave it with silently wrong frame generation.
namespace ce::streamline_bridge {

// Resolves the 2.x entry points from the CE-owned runtime. Called once, at activation,
// before any import slot is repointed. False means the runtime is missing something the
// translation needs, and the bridge must not activate.
bool ResolveTranslationTargets(void* v2InterposerModule);

// Hands a D3D12 device to the CE-owned 2.x runtime. Call it for every device CE learns of,
// from any route; each distinct one is passed on, and the same pointer is never re-sent.
//
// Streamline asks for this by name when it is missing - "D3D or VK API hook is activated
// without device being created, did you forget to call `slSetD3DDevice`" - and it needs it
// even when its own interposer created the device, because the FIRST device a title creates
// is routinely a capability probe it releases moments later. The Witcher 3 does exactly that.
// Handing over only the first device therefore gives Streamline the one that is about to die
// and never the one the game renders with.
//
// `interposed` marks a device that came out of the 2.x interposer's own `D3D12CreateDevice`,
// which is the one Streamline wants: it is Streamline's proxy, at the moment the SDK
// documents the call for. CE's other route derives a device from the game's command queue,
// which is the NATIVE device and arrives later - the right answer only for a title whose
// device Streamline never interposed at all. So an interposed device, once seen, wins; the
// queue-derived one is a fallback rather than a competing opinion.
//
// Returns whether the runtime is usable AFTERWARDS, which is a separate question - see
// V2RuntimeHasDevice.
bool SetV2RuntimeDevice(void* d3d12Device, bool interposed);

// Whether the 2.x runtime's feature contexts are up, so its device-dependent entry points can
// actually be called.
//
// This is Streamline's own answer via `slGetFeatureFunction`, never CE's inference from
// having created or handed over a device. Both of those were tried and both produced the same
// crash: a `slSetConstants` that jumps through a plugin pointer the manager never bound.
bool V2RuntimeHasDevice();

// The eight 1.x entry points, in the shapes 1.5.6 actually uses. Each returns `bool` in AL.
bool TranslateInit(const void* preferences1x, int applicationId);
bool TranslateShutdown();
bool TranslateIsFeatureSupported(uint32_t feature1x, uint32_t* adapterBitMask);
bool TranslateSetTag(const void* resource1x, uint32_t bufferType, uint32_t id, const void* extent1x);
bool TranslateSetConstants(const void* constants1x, uint32_t frameIndex, uint32_t id);
bool TranslateSetFeatureConstants(uint32_t feature1x, const void* constants1x, uint32_t frameIndex, uint32_t id);
bool TranslateGetFeatureSettings(uint32_t feature1x, const void* constants1x, void* settings1x);
bool TranslateEvaluateFeature(void* commandBuffer, uint32_t feature1x, uint32_t frameIndex, uint32_t id);

}  // namespace ce::streamline_bridge
