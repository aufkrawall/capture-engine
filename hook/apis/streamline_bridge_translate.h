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

// Records that the 2.x interposer created the game's device itself, so it already has it.
//
// This is the ordinary path and it must NOT be followed by `slSetD3DDevice`. Streamline
// offers device creation through the interposer OR `slSetD3DDevice` for a host that made its
// own - one or the other, not both - and calling it a second time on a device the interposer
// just bound is a state change made into somebody else's runtime for no reason.
void NoteV2RuntimeOwnsDevice(const char* how);

// Hands the game's D3D12 device to the CE-owned 2.x runtime, for the case where the runtime
// did NOT create it - an Agility SDK title makes its device through
// `ID3D12DeviceFactory::CreateDevice`, which Streamline does not interpose at all.
//
// Not optional in that case, and not merely an optimisation: most of Streamline 2.x's
// exported entry points forward through a plugin pointer the manager binds only once the
// device is known, and before that they jump to null rather than returning an error - see
// V2CallRequiresDevice in the policy header for the crash that established this. Until the
// runtime has a device, every device-dependent translation is refused.
//
// Idempotent, and safe to call from more than one discovery route: the first device wins.
bool SetV2RuntimeDevice(void* d3d12Device);

// Whether the 2.x runtime has a device and its device-dependent entry points are callable.
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
