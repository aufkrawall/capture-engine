#include <gtest/gtest.h>

#include <string>

#include "hook/d3d9/dx9_sampler_config_policy.h"
#include "hook/d3d9/dx9_sampler_interception_policy.h"
#include "hook/d3d9/dx9_state_block_sampler_policy.h"
#include "source_fragment_reader.h"

namespace arm = ce::dx9_sampler_interception;
namespace sb = ce::dx9_sampler_state;

namespace {

constexpr size_t kMinFilter = 4;
constexpr size_t kMaxAnisotropy = 8;

// Sampler 0 as CE keeps it while forcing 16x AF on a LINEAR material.
sb::SamplerState ForcedSampler() {
    sb::SamplerState sampler;
    sampler.initialized = true;
    sampler.bootstrapAttempted = true;
    sampler.reconciled = true;
    sampler.logical[kMinFilter] = D3DTEXF_LINEAR;
    sampler.physical[kMinFilter] = D3DTEXF_ANISOTROPIC;
    sampler.physical[kMaxAnisotropy] = 16;
    sampler.textureBound = true;
    sampler.textureMipLevels = 10;
    return sampler;
}

}  // namespace

// The per-draw calls are what made a CPU-bound DX9 game (Borderlands 2) ~15 %
// slower with nothing forced: without an override only the rare state-block
// lifecycle calls may be hooked.
TEST(Dx9SamplerInterceptionPolicyTest, NoOverrideHooksOnlyTheStateBlockLifecycle) {
    arm::VTableArming arming;
    EXPECT_EQ(arming.OnDeviceHooks(false), arm::kLifecycleSlots);
    EXPECT_FALSE(arming.PerDrawArmed());
    EXPECT_EQ(arming.OnPresent(false), 0u);
    EXPECT_EQ(arming.OnDeviceHooks(false), 0u) << "a second device on the vtable adds nothing";
    EXPECT_FALSE(arming.PerDrawArmed());

    const uint32_t perDraw = arm::kSlotSetTexture | arm::kSlotSetSamplerState | arm::kSlotGetSamplerState |
                             arm::kSlotStateBlockApply;
    EXPECT_EQ(arm::kPerDrawSlots & perDraw, perDraw);
    EXPECT_EQ(arm::kPerDrawSlots & arm::kLifecycleSlots, 0u);
}

TEST(Dx9SamplerInterceptionPolicyTest, OverrideConfiguredLaterArmsThePerDrawSlotsOnceAtPresent) {
    arm::VTableArming arming;
    ASSERT_EQ(arming.OnDeviceHooks(false), arm::kLifecycleSlots);
    EXPECT_EQ(arming.OnPresent(true), arm::kPerDrawSlots);
    EXPECT_TRUE(arming.PerDrawArmed());
    EXPECT_EQ(arming.OnPresent(true), 0u) << "handed out once: a failed install is not retried per Present";
    // Switching the override off restores the application's values through the
    // armed detours, so the vtable stays armed.
    EXPECT_EQ(arming.OnPresent(false), 0u);
    EXPECT_TRUE(arming.PerDrawArmed());
}

TEST(Dx9SamplerInterceptionPolicyTest, OverrideAtDeviceCreationArmsEverythingAtOnce) {
    arm::VTableArming arming;
    EXPECT_EQ(arming.OnDeviceHooks(true), arm::kLifecycleSlots | arm::kPerDrawSlots);
    EXPECT_TRUE(arming.PerDrawArmed());
    EXPECT_EQ(arming.OnDeviceHooks(true), 0u);
    EXPECT_EQ(arming.OnPresent(true), 0u);
}

// The shadow may start from the D3D9 defaults only while CE sees every Set*:
// a Reset between a config change and the arming Present must not seed it.
TEST(Dx9SamplerInterceptionPolicyTest, ShadowAndRecordingsAreTrustedOnlyWhileIntercepted) {
    EXPECT_TRUE(arm::ShadowTracksFromDefaults(true, true));
    EXPECT_FALSE(arm::ShadowTracksFromDefaults(false, true));
    EXPECT_FALSE(arm::ShadowTracksFromDefaults(true, false));
    EXPECT_FALSE(arm::ShadowTracksFromDefaults(false, false));

    EXPECT_TRUE(arm::RecordingSnapshotTrusted(true));
    EXPECT_FALSE(arm::RecordingSnapshotTrusted(false)) << "a recording begun before arming missed Set* calls";
}

// With AF forced, engines re-set every sampler state per draw. Such a call
// changes nothing and is answered without the AF policy or the driver.
TEST(Dx9SamplerInterceptionPolicyTest, RedundantSetIsSkippedOnlyOnAReconciledTrackedSampler) {
    const sb::SamplerState forced = ForcedSampler();
    EXPECT_TRUE(sb::IsRedundantLogicalSet(forced, kMinFilter, D3DTEXF_LINEAR));
    EXPECT_FALSE(sb::IsRedundantLogicalSet(forced, kMinFilter, D3DTEXF_POINT)) << "a real change";
    EXPECT_FALSE(sb::IsRedundantLogicalSet(forced, kMinFilter, D3DTEXF_ANISOTROPIC))
        << "the forced physical value is not the application's";
    EXPECT_FALSE(sb::IsRedundantLogicalSet(forced, sb::kStateCount, 0));

    sb::SamplerState unreconciled = forced;
    unreconciled.reconciled = false;
    EXPECT_FALSE(sb::IsRedundantLogicalSet(unreconciled, kMinFilter, D3DTEXF_LINEAR));

    sb::SamplerState untracked = forced;
    untracked.initialized = false;
    EXPECT_FALSE(sb::IsRedundantLogicalSet(untracked, kMinFilter, D3DTEXF_LINEAR));

    sb::SamplerState invalidated = forced;
    sb::InvalidateStageSamplerShadow(invalidated);
    EXPECT_FALSE(sb::IsRedundantLogicalSet(invalidated, kMinFilter, D3DTEXF_LINEAR));
}

// A state-block Apply changes the device behind the Set* calls; the merge must
// clear `reconciled` so the next Set reaches the policy again.
TEST(Dx9SamplerInterceptionPolicyTest, StateBlockApplyEndsTheRedundantSetShortcut) {
    sb::SamplerShadow shadow;
    shadow[0] = ForcedSampler();
    sb::StateBlockSnapshot block = sb::MakeStateBlockSnapshot(sb::CoverageForStateBlockType(D3DSBT_PIXELSTATE));
    sb::CaptureStateBlock(block, shadow);
    ASSERT_TRUE(sb::IsRedundantLogicalSet(shadow[0], kMinFilter, D3DTEXF_LINEAR));

    sb::ApplyStateBlockToShadow(block, shadow);
    EXPECT_FALSE(shadow[0].reconciled);
    EXPECT_FALSE(sb::IsRedundantLogicalSet(shadow[0], kMinFilter, D3DTEXF_LINEAR));
}

// With AF forced, SetTexture used to ask every newly bound texture for its
// level count and type over COM. The cache answers instead, but only while CE
// sees every texture creation: a creation is the one way an address comes to
// name another texture, and it forgets that address's entry.
TEST(Dx9SamplerInterceptionPolicyTest, CachedTextureMetadataNeedsCreationTrackingAndForgetsRecreatedAddresses) {
    sb::TextureMetaCache cache{};
    int textures[2] = {};
    const void* first = &textures[0];
    const void* collides = nullptr;
    for (uintptr_t offset = sizeof(int);; offset += sizeof(int)) {
        const void* candidate = reinterpret_cast<const char*>(first) + offset;
        if (sb::HashTexturePointer(candidate) == sb::HashTexturePointer(first)) {
            collides = candidate;
            break;
        }
    }
    cache[sb::HashTexturePointer(first)] = {first, 10, D3DRTYPE_TEXTURE, 10};

    const sb::TextureMetaCacheEntry* hit = sb::FindTrustedTextureMetadata(cache, first, true);
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(hit->effectiveMipLevels, 10u);
    EXPECT_EQ(sb::FindTrustedTextureMetadata(cache, first, false), nullptr) << "untracked: ask the texture";
    EXPECT_EQ(sb::FindTrustedTextureMetadata(cache, collides, true), nullptr) << "another address in the same slot";

    sb::ForgetCreatedTexture(cache, collides);
    EXPECT_NE(sb::FindTrustedTextureMetadata(cache, first, true), nullptr) << "a creation elsewhere keeps the entry";
    sb::ForgetCreatedTexture(cache, first);
    EXPECT_EQ(sb::FindTrustedTextureMetadata(cache, first, true), nullptr)
        << "a texture created at a cached address is a different texture";
}

// The creation slots are armed with the per-draw ones and hooked before
// SetTexture, so no texture is cached before its creation could be seen; the
// trust report runs outside the vtable mutex (it takes the device locks).
TEST(Dx9SamplerInterceptionSourceTest, TextureCreationIsHookedBeforeSetTextureAndReportedOutsideTheLock) {
    EXPECT_EQ(arm::kPerDrawSlots & arm::kSlotTextureCreation, arm::kSlotTextureCreation);
    EXPECT_EQ(arm::kLifecycleSlots & arm::kSlotTextureCreation, 0u);

    using ce::test_source::FindSource;
    const std::string detours = ce::test_source::ReadFile(FindSource("hook", "dx9_hook_state_detours.cpp"));
    ASSERT_FALSE(detours.empty());
    const size_t hookSlots = detours.find("D3D9TextureCreationEvent HookD3D9SamplerSlotsLocked(");
    ASSERT_NE(hookSlots, std::string::npos);
    const size_t creation = detours.find("HookD3D9TextureCreationSlotsLocked(record, vtable)", hookSlots);
    const size_t setTexture = detours.find("&vtable[65]", hookSlots);
    ASSERT_NE(creation, std::string::npos);
    EXPECT_LT(creation, setTexture);
    for (const char* caller : {"bool InstallD3D9SamplerHooks(", "void ArmD3D9SamplerHooksAtPresent(",
                               "void CheckD3D9SamplerHookDrift("}) {
        const size_t body = detours.find(caller);
        ASSERT_NE(body, std::string::npos) << caller;
        const size_t unlock = detours.find("\n    }\n", detours.find("std::lock_guard<std::mutex> lock(", body));
        const size_t report = detours.find("ReportD3D9TextureCreationEvent(creation);", body);
        ASSERT_NE(report, std::string::npos) << caller;
        EXPECT_LT(unlock, report) << caller;
    }
}
