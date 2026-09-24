// Direct3D 7 / Direct3D 8 state blocks against CE's texture-stage shadow
// (legacy_d3d_state_block_policy.h). The same defect D3D9 had: after
// ApplyStateBlock CE rebuilt the desired state from the pre-Apply logical shadow
// and wrote it back, undoing the block. With no override configured the shadow
// was not even kept current, so an Apply reset the application's samplers to the
// device-creation defaults (or to an earlier Apply's re-read).
//
// The harness below drives a fake D3D8 device and CE's shadow through the same
// policy calls the hook makes, with a reconcile that forces 16x AF on LINEAR
// WRAP materials exactly like the legacy classifier does for that case.

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <string>

#include "../hook/apis/legacy_d3d_state_block_policy.h"
#include "source_fragment_reader.h"

namespace blocks = ce::legacy_d3d_state_block;

namespace {

// D3D8 texture-stage values (index order of kTrackedTypes: U, V, W, MAG, MIN, MIP, BIAS, MAXMIP, MAXANISO).
constexpr size_t kMag = 3;
constexpr size_t kMin = 4;
constexpr size_t kMip = 5;
constexpr size_t kMaxAniso = 8;
constexpr DWORD kWrap = 1;
constexpr DWORD kClamp = 3;
constexpr DWORD kPoint = 1;
constexpr DWORD kLinear = 2;
constexpr DWORD kAnisotropic = 3;

struct Stage {
    blocks::StageValues logical = {kWrap, kWrap, kWrap, kPoint, kPoint, 0, 0, 0, 1};
    blocks::StageValues physical = logical;
    bool initialized = true;
};

using Shadow = std::array<Stage, blocks::kStageCount>;
using Device = std::array<blocks::StageValues, blocks::kStageCount>;

blocks::StageValues Desired(const blocks::StageValues& logical, bool forceAf) {
    blocks::StageValues desired = logical;
    if (forceAf && logical[0] == kWrap && logical[1] == kWrap && logical[kMin] == kLinear &&
        logical[kMip] != 0) {
        desired[kMag] = kAnisotropic;
        desired[kMin] = kAnisotropic;
        desired[kMaxAniso] = 16;
    }
    return desired;
}

struct Harness {
    Device device{};
    Shadow shadow{};
    bool overrideConfigured = false;
    bool recording = false;
    blocks::Snapshot recordingSnapshot;
    int writes = 0;

    Harness() {
        for (size_t i = 0; i < blocks::kStageCount; ++i)
            device[i] = shadow[i].physical;
    }

    // SetTextureStageState through CE's hook.
    void Set(size_t stage, size_t state, DWORD value) {
        if (recording) {
            blocks::RecordState(recordingSnapshot, stage, state, value);  // recorded unforced, not applied
            return;
        }
        if (!overrideConfigured) {
            device[stage][state] = value;  // passthrough: the shadow is NOT updated
            return;
        }
        shadow[stage].logical[state] = value;
        Reconcile(stage);
    }

    void Reconcile(size_t stage) {
        const blocks::StageValues desired = Desired(shadow[stage].logical, overrideConfigured);
        for (size_t s = 0; s < blocks::kStateCount; ++s) {
            if (device[stage][s] != desired[s]) {
                device[stage][s] = desired[s];
                ++writes;
            }
        }
        shadow[stage].physical = desired;
    }

    blocks::Snapshot Create(DWORD type) {
        blocks::Snapshot snapshot = blocks::MakeSnapshot(type, blocks::kAllStatesMask);
        blocks::CaptureSnapshot(snapshot, shadow, overrideConfigured);
        // The runtime's own copy of the block is the device state.
        return snapshot;
    }

    // ApplyStateBlock: the runtime writes the block's physical values, then CE's detour runs.
    void Apply(const blocks::Snapshot& snapshot, const Device& blockDeviceValues, bool knownToCe) {
        for (size_t i = 0; i < blocks::kStageCount; ++i) {
            for (size_t s = 0; s < blocks::kStateCount; ++s) {
                if (snapshot.stages[i].stateMask & (1u << s))
                    device[i][s] = blockDeviceValues[i][s];
            }
        }
        if (!blocks::ApplyMayWrite(overrideConfigured, false, recording)) {
            for (Stage& stage : shadow)
                stage.initialized = false;  // dropped, bootstrapped later
            return;
        }
        if (knownToCe) {
            const blocks::ApplyResult applied = blocks::ApplySnapshotToShadow(snapshot, shadow);
            for (size_t i = 0; i < blocks::kStageCount; ++i) {
                if (applied.coveredStages & (1u << i))
                    Reconcile(i);
            }
            return;
        }
        for (size_t i = 0; i < blocks::kStageCount; ++i) {
            const blocks::StageValues previous = shadow[i].physical;
            shadow[i].physical = device[i];
            blocks::AdoptExternalPhysical(shadow[i], previous);
            Reconcile(i);
        }
    }
};

Device SnapshotDevice(const Harness& harness) {
    return harness.device;
}

}  // namespace

TEST(LegacyD3DStateBlockPolicyTest, ApplyWithNoOverrideWritesNothing) {
    Harness h;
    // The application sets a LINEAR material on stage 0 (passthrough, shadow stale).
    h.Set(0, kMin, kLinear);
    h.Set(0, kMag, kLinear);
    h.Set(0, kMip, 2);
    const blocks::Snapshot block = h.Create(blocks::kBlockTypeAll);
    const Device blockValues = SnapshotDevice(h);
    // Then changes it and restores via the block.
    h.Set(0, 0, kClamp);
    h.Apply(block, blockValues, /*knownToCe=*/true);
    EXPECT_EQ(h.writes, 0) << "no override: CE must never write after an Apply";
    EXPECT_EQ(h.device[0][kMin], kLinear);
    EXPECT_EQ(h.device[0][0], kWrap);
    // The stale shadow was dropped, so an override enabled later re-reads the device.
    EXPECT_FALSE(h.shadow[0].initialized);
    // Created while inactive, the snapshot is not trusted as the application's.
    EXPECT_FALSE(block.stages[0].valid);
}

TEST(LegacyD3DStateBlockPolicyTest, ApplyWithForcedFilteringKeepsTheBlockAndForcesIt) {
    Harness h;
    h.overrideConfigured = true;
    h.Set(0, kMin, kLinear);
    h.Set(0, kMag, kLinear);
    h.Set(0, kMip, 2);
    EXPECT_EQ(h.device[0][kMin], kAnisotropic);
    const blocks::Snapshot block = h.Create(blocks::kBlockTypePixelState);
    const Device blockValues = SnapshotDevice(h);
    EXPECT_TRUE(block.stages[0].valid);
    EXPECT_EQ(block.stages[0].logical[kMin], kLinear);
    EXPECT_EQ(block.stages[0].physical[kMin], kAnisotropic);

    // The application switches the stage to a CLAMP POINT UI sampler, then applies the block.
    h.Set(0, 0, kClamp);
    h.Set(0, kMin, kPoint);
    h.Set(0, kMag, kPoint);
    EXPECT_EQ(h.device[0][kMin], kPoint);
    h.writes = 0;
    h.Apply(block, blockValues, /*knownToCe=*/true);
    // The block's material is back (not undone to CLAMP/POINT) and still forced.
    EXPECT_EQ(h.device[0][0], kWrap);
    EXPECT_EQ(h.device[0][kMin], kAnisotropic);
    EXPECT_EQ(h.device[0][kMaxAniso], 16u);
    EXPECT_EQ(h.shadow[0].logical[0], kWrap);
    EXPECT_EQ(h.shadow[0].logical[kMin], kLinear);
    EXPECT_EQ(h.writes, 0) << "the block already restored the forced values";
}

TEST(LegacyD3DStateBlockPolicyTest, RecordedBlockRestoresTheApplicationsValuesAndCeForcesThem) {
    Harness h;
    h.overrideConfigured = true;
    h.recording = true;
    h.recordingSnapshot = blocks::Snapshot{};
    h.Set(1, kMin, kLinear);
    h.Set(1, kMag, kLinear);
    h.Set(1, kMip, 2);
    h.recording = false;
    const blocks::Snapshot recorded = h.recordingSnapshot;
    // Recording applied nothing and left the shadow alone.
    EXPECT_EQ(h.device[1][kMin], kPoint);
    EXPECT_EQ(h.shadow[1].logical[kMin], kPoint);
    EXPECT_EQ(recorded.stages[1].stateMask, (1u << kMin) | (1u << kMag) | (1u << kMip));
    EXPECT_EQ(recorded.stages[0].stateMask, 0u);

    Device blockValues = h.device;
    blockValues[1] = recorded.stages[1].physical;
    h.Apply(recorded, blockValues, /*knownToCe=*/true);
    EXPECT_EQ(h.shadow[1].logical[kMin], kLinear);
    EXPECT_EQ(h.device[1][kMin], kAnisotropic);
    // Stage 0 was not in the block and was not touched.
    EXPECT_EQ(h.device[0][kMin], kPoint);
}

TEST(LegacyD3DStateBlockPolicyTest, UnseenBlockAdoptsOnlyWhatChanged) {
    Harness h;
    h.overrideConfigured = true;
    h.Set(2, kMip, 2);
    h.Set(2, kMin, kLinear);
    blocks::Snapshot foreign = blocks::MakeSnapshot(blocks::kBlockTypeAll, blocks::kAllStatesMask);
    Device blockValues = h.device;
    blockValues[2][0] = kClamp;  // the block (made before injection) sets CLAMP only
    h.Apply(foreign, blockValues, /*knownToCe=*/false);
    EXPECT_EQ(h.shadow[2].logical[0], kClamp);
    EXPECT_EQ(h.shadow[2].logical[kMin], kLinear) << "an unchanged forced value is not adopted as the app's";
    EXPECT_EQ(h.device[2][0], kClamp);
}

TEST(LegacyD3DStateBlockPolicyTest, CoverageAndMasks) {
    EXPECT_TRUE(blocks::BlockTypeCoversTextureStageStates(1));   // D3DSBT_ALL
    EXPECT_TRUE(blocks::BlockTypeCoversTextureStageStates(2));   // D3DSBT_PIXELSTATE
    EXPECT_FALSE(blocks::BlockTypeCoversTextureStageStates(3));  // D3DSBT_VERTEXSTATE
    const blocks::Snapshot vertex = blocks::MakeSnapshot(3, blocks::kAllStatesMask);
    EXPECT_EQ(vertex.stages[0].stateMask, 0u);
    EXPECT_EQ(blocks::kD3D7StatesMask & (1u << 2), 0u) << "D3D7 has no ADDRESSW";
    EXPECT_FALSE(blocks::ApplyMayWrite(false, false, false));
    EXPECT_TRUE(blocks::ApplyMayWrite(true, false, false));
    EXPECT_TRUE(blocks::ApplyMayWrite(false, true, false));
    EXPECT_FALSE(blocks::ApplyMayWrite(true, true, true));

    // A covered, initialized stage whose snapshot is unknown is dropped, not merged.
    Shadow shadow{};
    blocks::Snapshot unknown = blocks::MakeSnapshot(1, blocks::kAllStatesMask);
    const blocks::ApplyResult result = blocks::ApplySnapshotToShadow(unknown, shadow);
    EXPECT_EQ(result.coveredStages, 0xFFu);
    EXPECT_EQ(result.staleStages, 0xFFu);
}

TEST(LegacyD3DStateBlockSourceTest, ApplyIsMergedFromTheSnapshotAndInactiveApplyWritesNothing) {
    const auto root = std::filesystem::current_path() / "hook" / "apis";
    const std::string state = ce::test_source::ReadFile(root / "legacy_d3d_sampler_state.cpp");
    const std::string ddraw = ce::test_source::ReadFile(root / "ddraw_hook_detours_legacy_d3d.cpp");
    const std::string ddrawInstall = ce::test_source::ReadFile(root / "ddraw_hook_install.cpp");
    const std::string dx8 = ce::test_source::ReadFile(root / "dx8_hook_detours.cpp");
    ASSERT_FALSE(state.empty());
    const size_t reconcile = state.find("void ReconcileAfterExternalStateChange(");
    ASSERT_NE(reconcile, std::string::npos);
    const size_t mayWrite = state.find("blocks::ApplyMayWrite(", reconcile);
    const size_t merge = state.find("blocks::ApplySnapshotToShadow(", reconcile);
    const size_t firstReconcile = state.find("ReconcileStage(", reconcile);
    ASSERT_NE(mayWrite, std::string::npos);
    ASSERT_NE(merge, std::string::npos);
    EXPECT_LT(mayWrite, merge);
    EXPECT_LT(merge, firstReconcile) << "merge FIRST, or the reconcile undoes the block";
    EXPECT_NE(state.find("blocks::AdoptExternalPhysical(stage, previousPhysical)", reconcile), std::string::npos);
    EXPECT_NE(state.find("blocks::RecordState(deviceState->recordingSnapshot"), std::string::npos);
    // Every hooked API passes the block handle and tracks the block lifetime.
    EXPECT_NE(ddraw.find("ddraw_hook_device, ddraw_hook_blockHandle,"), std::string::npos);
    EXPECT_NE(ddrawInstall.find("InstallD3D7StateBlockTrackingHooks(record, vtable);"), std::string::npos);
    EXPECT_NE(dx8.find("device, dx8_hook_Token,"), std::string::npos);
    EXPECT_NE(dx8.find("InstallD3D8StateBlockTrackingHooks(record, vtable);"), std::string::npos);
    for (const char* fn : {"OnBeginStateBlock", "OnEndStateBlock", "OnCreateStateBlock", "OnCaptureStateBlock",
                           "ForgetStateBlock"}) {
        EXPECT_NE(ddraw.find(fn), std::string::npos) << fn;
        EXPECT_NE(dx8.find(fn), std::string::npos) << fn;
    }
    // The recording pair is installed all-or-nothing.
    EXPECT_NE(ddraw.find("const bool beginHooked = endHooked && "), std::string::npos);
    EXPECT_NE(dx8.find("const bool beginHooked = endHooked && "), std::string::npos);
}
