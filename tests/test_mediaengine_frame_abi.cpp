#include <gtest/gtest.h>

#include <windows.h>
#include <type_traits>

// These functions are statically linked into the tests and exported by the same
// product translation unit that exports them from mediaengine.dll.
#define MEDIAENGINE_EXPORTS
#include "mediaengine/engine/mediaengine.h"

namespace {
using LegacyFrame = bool (*)(uint64_t, uint64_t, uint64_t, int64_t, int32_t, int32_t, uint32_t, uint32_t, uint32_t,
                             uint32_t, bool, bool, int, const ce::cursor::CaptureState*);
using LegacyD3D11Frame = bool (*)(void*, int64_t, uint32_t, uint32_t, bool, int32_t, int32_t, int64_t,
                                  const ce::cursor::CaptureState*);

static_assert(std::is_same_v<decltype(static_cast<LegacyFrame>(&MediaEngine_ProcessFrame)), LegacyFrame>);
static_assert(
    std::is_same_v<decltype(static_cast<LegacyD3D11Frame>(&MediaEngine_ProcessFrameD3D11)), LegacyD3D11Frame>);
}  // namespace

TEST(MediaEngineFrameAbiTest, LegacyExportNamesKeepTheirOriginalCallingContract) {
    const HMODULE module = GetModuleHandleW(nullptr);
    const auto inject = reinterpret_cast<LegacyFrame>(GetProcAddress(module, "MediaEngine_ProcessFrame"));
    const auto d3d11 = reinterpret_cast<LegacyD3D11Frame>(GetProcAddress(module, "MediaEngine_ProcessFrameD3D11"));
    ASSERT_NE(inject, nullptr);
    ASSERT_NE(d3d11, nullptr);
    // There is no recording; zero input must be rejected without treating the
    // first legacy argument as a pointer to a descriptor.
    EXPECT_FALSE(inject(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, false, false, 0, nullptr));
    EXPECT_FALSE(d3d11(nullptr, 0, 0, 0, false, 0, 0, -1, nullptr));
}

TEST(MediaEngineFrameAbiTest, DescriptorEntryPointsHaveDistinctExportsAndRejectNull) {
    const HMODULE module = GetModuleHandleW(nullptr);
    using Inject = bool (*)(const VideoFrameSubmissionDesc*);
    using D3D11 = bool (*)(const D3D11FrameSubmissionDesc*);
    const auto inject = reinterpret_cast<Inject>(GetProcAddress(module, "MediaEngine_SubmitFrame"));
    const auto d3d11 = reinterpret_cast<D3D11>(GetProcAddress(module, "MediaEngine_SubmitFrameD3D11"));
    ASSERT_NE(inject, nullptr);
    ASSERT_NE(d3d11, nullptr);
    EXPECT_FALSE(inject(nullptr));
    EXPECT_FALSE(d3d11(nullptr));
}
