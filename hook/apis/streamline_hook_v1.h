#pragma once

#include <cstdint>

#include "../common/streamline_api_generation.h"

// Streamline 1.x entry points, in the shapes 1.x actually uses.
//
// These are not a translation of the 2.x hooks; they are separate functions because the
// calling convention differs in argument count, argument order and return type. Verified
// against sl.interposer.dll 1.5.6 (The Witcher 3) by disassembly:
//
//   slSetTag           rcx=const Resource*  edx=BufferType  r8d=id          r9=const Extent*   -> al
//   slEvaluateFeature  rcx=CommandBuffer*   edx=Feature     r8d=frameIndex  r9d=id             -> al
//
// Both return `bool` in AL, not the 2.x `sl::Result` in EAX, so the hooks must return bool
// as well: a 32-bit zero written where the caller reads one byte is a silent behavior change.
namespace ce::streamline_v1 {

using PFN_slSetTagV1 = bool (*)(const void* resource, uint32_t bufferType, uint32_t id, const void* extent);
using PFN_slEvaluateFeatureV1 = bool (*)(void* commandBuffer, uint32_t feature, uint32_t frameIndex, uint32_t id);

// 1.x has no ray-reconstruction feature; sl.interposer 1.5.6's own feature-name table is
// DLSS, NRD, NIS, Reflex, Debug, DLSS_G, Common. Only the upscaler matters to CE's
// published DLSS state.
inline constexpr uint32_t kV1FeatureDLSS = 0;

bool Hooked_slSetTagV1(const void* resource, uint32_t bufferType, uint32_t id, const void* extent);
bool Hooked_slEvaluateFeatureV1(void* commandBuffer, uint32_t feature, uint32_t frameIndex, uint32_t id);

// Drops the remembered UI tag. Called when the Streamline modules go away so a later
// generation cannot record against a resource from the previous one.
void ForgetPendingUiTag(const char* reason);

}  // namespace ce::streamline_v1
