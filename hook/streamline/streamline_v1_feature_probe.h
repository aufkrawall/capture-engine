#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// Passive recorder for the two Streamline 1.x calls whose payload layout NVIDIA never
// published for the interposer shipping games actually use.
//
// `slSetFeatureConstants` and `slGetFeatureSettings` carry an opaque per-feature struct -
// `sl::DLSSConstants`, `sl::DLSSGConstants`, `sl::DLSSGSettings`. Translating them to 2.x
// is the last piece of the generation bridge, and it needs those layouts as they stand in
// 1.5.6. They are not obtainable from any published source: NVIDIA shipped no 1.x release
// at all and the public 1.x tags stop at v1.1.1, which predates DLSS-G entirely. sl.dlss_g
// 1.5.6 proves the types exist - its own diagnostics name `sl::DLSSGSettings::status` and
// `numFramesToGenerate` - without revealing a single offset.
//
// Guessing them is the one thing CE must not do. A wrong field here is not a crash, it is
// silently wrong frame generation, which no test in this repo can catch. So CE measures
// instead: these hooks record what the game passes and then forward the call unchanged, so
// the game keeps running its own Streamline exactly as it would have. That makes an
// ordinary unbridged session - DLSS and frame generation genuinely working - produce the
// ground truth the bridge is waiting on, instead of requiring a degraded bridged run in
// which the game would give up before ever passing these structs.
//
// Both entry points exist ONLY in 1.x (they are `kV1OnlyExports` markers), so these hooks
// can never land on a 2.x module; they are still installed behind the ordinary V1
// generation gate rather than relying on that.
namespace ce::streamline_v1 {

// 1.x, verified against OptiScaler's vendored SL1 headers and the 1.5.6 export set:
//   bool slSetFeatureConstants(Feature, const void* consts, uint32_t frameIndex, uint32_t id)
//   bool slGetFeatureSettings(Feature, const void* consts, void* settings)
// Both return `bool` in AL, like every other 1.x entry point.
using PFN_slSetFeatureConstantsV1 = bool (*)(uint32_t feature, const void* consts, uint32_t frameIndex,
                                            uint32_t id);
using PFN_slGetFeatureSettingsV1 = bool (*)(uint32_t feature, const void* consts, void* settings);

extern PFN_slSetFeatureConstantsV1 g_Original_slSetFeatureConstantsV1;
extern std::atomic<bool> g_SetFeatureConstantsV1Hooked;
extern std::atomic<void*> g_SetFeatureConstantsV1Target;

extern PFN_slGetFeatureSettingsV1 g_Original_slGetFeatureSettingsV1;
extern std::atomic<bool> g_GetFeatureSettingsV1Hooked;
extern std::atomic<void*> g_GetFeatureSettingsV1Target;

bool Hooked_slSetFeatureConstantsV1(uint32_t feature, const void* consts, uint32_t frameIndex, uint32_t id);
bool Hooked_slGetFeatureSettingsV1(uint32_t feature, const void* consts, void* settings);

// Records a bounded, readability-checked, rate-limited prefix of an opaque 1.x per-feature
// payload. Shared with the bridge so there is one implementation of "look at somebody
// else's struct without ever being the thing that faults".
void RecordOpaqueFeaturePayload(const char* call, uint32_t v1Feature, const void* payload);

}  // namespace ce::streamline_v1
