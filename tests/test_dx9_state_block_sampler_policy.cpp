#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../hook/apis/dx9_sampler_rearm_policy.h"
#include "../hook/apis/dx9_state_block_sampler_policy.h"
#include "source_fragment_reader.h"

namespace sb = ce::dx9_sampler_state;
namespace rearm = ce::dx9_sampler_rearm;

namespace {

constexpr size_t kAddressU = 0;
constexpr size_t kMinFilter = 4;
constexpr size_t kMaxAnisotropy = 8;

// A shadow in the state CE keeps while forcing 16x AF: sampler 0 is a WRAP
// material the application filters LINEAR, physically ANISOTROPIC.
sb::SamplerShadow ForcedShadow() {
    sb::SamplerShadow shadow;
    for (sb::SamplerState& sampler : shadow) {
        sampler.initialized = true;
        sampler.bootstrapAttempted = true;
    }
    shadow[0].logical[kMinFilter] = D3DTEXF_LINEAR;
    shadow[0].physical[kMinFilter] = D3DTEXF_ANISOTROPIC;
    shadow[0].physical[kMaxAnisotropy] = 16;
    shadow[0].textureBound = true;
    shadow[0].textureMipLevels = 10;
    return shadow;
}

std::string ReadSource(const char* relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

}  // namespace

// The bug: after an Apply, CE re-derived the physical state from the logical
// values it had BEFORE the Apply and wrote them back, undoing the block. The
// snapshot merge makes the shadow agree with what the block restored.
TEST(Dx9StateBlockSamplerPolicyTest, ApplyMergesTheBlocksValuesInsteadOfUndoingThem) {
    sb::SamplerShadow shadow = ForcedShadow();
    // Captured while the game had sampler 0 CLAMPed.
    shadow[0].logical[kAddressU] = D3DTADDRESS_CLAMP;
    shadow[0].physical[kAddressU] = D3DTADDRESS_CLAMP;
    sb::StateBlockSnapshot block = sb::MakeStateBlockSnapshot(sb::CoverageForStateBlockType(D3DSBT_ALL));
    sb::CaptureStateBlock(block, shadow);

    // The game moves on to WRAP, then applies the block.
    shadow[0].logical[kAddressU] = D3DTADDRESS_WRAP;
    shadow[0].physical[kAddressU] = D3DTADDRESS_WRAP;
    const uint32_t covered = sb::ApplyStateBlockToShadow(block, shadow);
    EXPECT_EQ(covered, (1u << sb::kSamplerCount) - 1u);
    EXPECT_EQ(shadow[0].logical[kAddressU], static_cast<DWORD>(D3DTADDRESS_CLAMP))
        << "the reconcile that follows must keep the block's CLAMP, not restore WRAP";
    EXPECT_EQ(shadow[0].physical[kAddressU], static_cast<DWORD>(D3DTADDRESS_CLAMP));
    // The forced view survives: the block captured the forced physical value.
    EXPECT_EQ(shadow[0].logical[kMinFilter], static_cast<DWORD>(D3DTEXF_LINEAR));
    EXPECT_EQ(shadow[0].physical[kMinFilter], static_cast<DWORD>(D3DTEXF_ANISOTROPIC));
    EXPECT_EQ(shadow[0].textureMipLevels, 10u);
}

// Between BeginStateBlock and EndStateBlock D3D9 records instead of applying.
// The recorded values are the application's own (unforced); only the samplers
// and states the application touched are covered.
TEST(Dx9StateBlockSamplerPolicyTest, RecordedBlocksCoverOnlyWhatWasRecorded) {
    sb::SamplerShadow shadow = ForcedShadow();
    sb::StateBlockSnapshot block;
    sb::RecordSamplerState(block, 3, kAddressU, D3DTADDRESS_MIRROR);
    sb::SamplerState texture;
    texture.textureBound = true;
    texture.textureMipLevels = 7;
    texture.textureSupportsAnisotropy = true;
    sb::RecordTexture(block, 5, texture);
    sb::RecordSamplerState(block, 99, kAddressU, 1);  // out of range: ignored
    sb::RecordSamplerState(block, 0, 42, 1);

    const uint32_t covered = sb::ApplyStateBlockToShadow(block, shadow);
    EXPECT_EQ(covered, (1u << 3) | (1u << 5));
    EXPECT_EQ(shadow[3].logical[kAddressU], static_cast<DWORD>(D3DTADDRESS_MIRROR));
    EXPECT_EQ(shadow[3].physical[kAddressU], static_cast<DWORD>(D3DTADDRESS_MIRROR))
        << "recorded unforced: the Apply puts the application's value on the device";
    EXPECT_EQ(shadow[5].textureMipLevels, 7u);
    EXPECT_TRUE(shadow[5].textureSupportsAnisotropy);
    EXPECT_EQ(shadow[0].physical[kMinFilter], static_cast<DWORD>(D3DTEXF_ANISOTROPIC)) << "uncovered: untouched";
}

TEST(Dx9StateBlockSamplerPolicyTest, CoverageFollowsTheStateBlockType) {
    EXPECT_EQ(sb::CoverageForStateBlockType(D3DSBT_ALL), sb::StateBlockCoverage::kSamplersAndTextures);
    EXPECT_EQ(sb::CoverageForStateBlockType(D3DSBT_PIXELSTATE), sb::StateBlockCoverage::kSamplers);
    EXPECT_EQ(sb::CoverageForStateBlockType(D3DSBT_VERTEXSTATE), sb::StateBlockCoverage::kNone);

    sb::SamplerShadow shadow = ForcedShadow();
    sb::StateBlockSnapshot pixel = sb::MakeStateBlockSnapshot(sb::StateBlockCoverage::kSamplers);
    sb::CaptureStateBlock(pixel, shadow);
    shadow[0].textureMipLevels = 1;
    sb::ApplyStateBlockToShadow(pixel, shadow);
    EXPECT_EQ(shadow[0].textureMipLevels, 1u) << "a pixel-state block does not restore textures";

    sb::StateBlockSnapshot vertex = sb::MakeStateBlockSnapshot(sb::StateBlockCoverage::kNone);
    sb::CaptureStateBlock(vertex, shadow);
    EXPECT_EQ(sb::ApplyStateBlockToShadow(vertex, shadow), 0u);
}

// A snapshot taken while the shadow knew nothing (no override then) cannot be
// merged; the covered samplers lose their shadow and bootstrap from the device.
TEST(Dx9StateBlockSamplerPolicyTest, UnknownSnapshotValuesForceABootstrapInsteadOfAStaleMerge) {
    sb::SamplerShadow untracked;
    sb::StateBlockSnapshot block = sb::MakeStateBlockSnapshot(sb::StateBlockCoverage::kSamplers);
    sb::CaptureStateBlock(block, untracked);

    sb::SamplerShadow shadow = ForcedShadow();
    const uint32_t covered = sb::ApplyStateBlockToShadow(block, shadow);
    EXPECT_NE(covered, 0u);
    EXPECT_FALSE(shadow[0].initialized);
    EXPECT_FALSE(shadow[0].bootstrapAttempted) << "the bootstrap must run again";
}

// A block CE never saw created (made before injection) holds the application's
// own values; whatever the device re-read shows changed becomes logical.
TEST(Dx9StateBlockSamplerPolicyTest, UnseenBlocksAdoptOnlyChangedValues) {
    sb::SamplerState state = ForcedShadow()[0];
    const sb::SamplerStateValues before = state.physical;
    state.physical[kAddressU] = D3DTADDRESS_BORDER;  // the block set it
    sb::AdoptExternalPhysical(state, before);
    EXPECT_EQ(state.logical[kAddressU], static_cast<DWORD>(D3DTADDRESS_BORDER));
    EXPECT_EQ(state.logical[kMinFilter], static_cast<DWORD>(D3DTEXF_LINEAR)) << "unchanged forced value stays logical";
}

// Re-arm: only a drift that holds still, once, and never onto foreign code.
TEST(Dx9SamplerRearmPolicyTest, StableDriftArmsOnceAndTransientDriftNever) {
    int detour = 0;
    int foreign = 0;
    int other = 0;
    rearm::SlotWatch watch;
    EXPECT_EQ(rearm::Observe(watch, &detour, &detour, true), rearm::Action::kNone);

    // A tool mid-install flips the slot back and forth: never stable.
    for (uint32_t i = 0; i < rearm::kSettlePresents * 2; ++i) {
        const void* value = (i % 2 == 0) ? static_cast<const void*>(&foreign) : static_cast<const void*>(&detour);
        EXPECT_NE(rearm::Observe(watch, value, &detour, true), rearm::Action::kArm);
    }
    // A change of the drifted-to value restarts the count.
    for (uint32_t i = 0; i + 1 < rearm::kSettlePresents; ++i)
        EXPECT_EQ(rearm::Observe(watch, &foreign, &detour, true), rearm::Action::kWait);
    EXPECT_EQ(rearm::Observe(watch, &other, &detour, true), rearm::Action::kWait);
    for (uint32_t i = 1; i + 1 < rearm::kSettlePresents; ++i)
        EXPECT_EQ(rearm::Observe(watch, &other, &detour, true), rearm::Action::kWait);
    EXPECT_EQ(rearm::Observe(watch, &other, &detour, true), rearm::Action::kArm);

    // Once per slot per session: no ping-pong, whatever the slot does next.
    for (uint32_t i = 0; i < rearm::kSettlePresents * 2; ++i)
        EXPECT_EQ(rearm::Observe(watch, &foreign, &detour, true), rearm::Action::kNone);
}

TEST(Dx9SamplerRearmPolicyTest, ForeignSavedOriginalIsRefusedOnce) {
    int detour = 0;
    int foreign = 0;
    rearm::SlotWatch watch;
    rearm::Action last = rearm::Action::kNone;
    for (uint32_t i = 0; i < rearm::kSettlePresents; ++i)
        last = rearm::Observe(watch, &foreign, &detour, false);
    EXPECT_EQ(last, rearm::Action::kRefuseForeignOriginal);
    EXPECT_EQ(rearm::Observe(watch, &foreign, &detour, false), rearm::Action::kNone);
}

// Source policy for the state-block wiring and the body re-arm.
TEST(Dx9StateBlockSamplerSourceTest, StateBlocksAndReArmAreWiredThroughTheSnapshotAndTheTrampolines) {
    const std::string detours = ReadSource("hook/apis/dx9_hook_state_detours.cpp");
    ASSERT_FALSE(detours.empty());
    EXPECT_NE(detours.find("ce::dx9_sampler_state::OnCreateStateBlock(device, *stateBlock, type)"), std::string::npos);
    EXPECT_NE(detours.find("ce::dx9_sampler_state::OnBeginStateBlock(device)"), std::string::npos);
    EXPECT_NE(detours.find("ce::dx9_sampler_state::OnCaptureStateBlock(device, stateBlock)"), std::string::npos);
    EXPECT_NE(detours.find("ReconcileAfterExternalStateChange(device, stateBlock,"), std::string::npos);
    EXPECT_NE(detours.find("VTableHook::Create(&vtable[60]"), std::string::npos) << "BeginStateBlock";
    EXPECT_NE(detours.find("VTableHook::Create(&vtable[4]"), std::string::npos) << "IDirect3DStateBlock9::Capture";

    const std::string state = ReadSource("hook/apis/dx9_sampler_state.cpp");
    ASSERT_FALSE(state.empty());
    const size_t reconcile = state.find("void ReconcileAfterExternalStateChange(");
    ASSERT_NE(reconcile, std::string::npos);
    const size_t merge = state.find("ApplyStateBlockToShadow(", reconcile);
    const size_t configRefresh = state.find("RefreshConfigLocked(", reconcile);
    ASSERT_NE(merge, std::string::npos);
    EXPECT_LT(merge, configRefresh) << "merge first: a reconcile against the pre-Apply shadow undoes the block";
    // Recording never reaches the shadow or a config-driven reconcile.
    EXPECT_NE(state.find("RecordSamplerState(deviceState->recordingSnapshot"), std::string::npos);
    EXPECT_NE(state.find("RecordTexture(deviceState->recordingSnapshot"), std::string::npos);
    const size_t refresh = state.find("void RefreshConfigLocked(");
    ASSERT_NE(refresh, std::string::npos);
    EXPECT_LT(state.find("deviceState.recording.load(", refresh), state.find("GetActiveGraphicsConfigVersion()", refresh));

    const std::string rearmUnit = ReadSource("hook/apis/dx9_hook_sampler_rearm.cpp");
    ASSERT_FALSE(rearmUnit.empty());
    EXPECT_NE(rearmUnit.find("InlineHook::InstallPublished(pristine"), std::string::npos);
    EXPECT_EQ(rearmUnit.find("VTableHook::Create("), std::string::npos) << "the slot is never written";
    // Body detours pass CE's own nested calls straight to the trampoline.
    EXPECT_NE(rearmUnit.find("dx9_hook_t_SamplerProcessingDepth > 0"), std::string::npos);
    EXPECT_NE(detours.find("entry->pristineOwned[slot] = IsVtableOwnersCode("), std::string::npos);
}
