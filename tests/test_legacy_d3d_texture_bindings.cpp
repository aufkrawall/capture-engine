#include <gtest/gtest.h>

#include <cstddef>

#include "../hook/common/legacy_d3d_texture_bindings.h"

// A Direct3D 7 state block stores the texture bound to each stage as a raw
// surface pointer with no reference, so an application releasing a still-bound
// surface leaves the block holding a destroyed interface. `ApplyStateBlock`
// then hands that interface to IDirect3DDevice7::SetTexture, which reads its
// freed `lpLcl` and faults (Gothic II session 20260916_000027). These tests pin
// the ownership rules that make CE's restore legal.

namespace {

struct FakeSurface {
    int references = 0;
    bool destroyed = false;
    bool addRefAfterDestruction = false;
    bool releaseBelowZero = false;
};

// Fixed storage, not a map: the traits run from the binding set's destructor,
// and an allocating container there is an exception escape clang-tidy rightly
// refuses.
constexpr size_t kFakeSurfaceCount = 8;
FakeSurface g_surfaces[kFakeSurfaceCount];

FakeSurface* AsSurface(void* handle) {
    auto* surface = static_cast<FakeSurface*>(handle);
    return (surface >= g_surfaces && surface < g_surfaces + kFakeSurfaceCount) ? surface : nullptr;
}

// The traits record violations instead of asserting, for the same reason.
struct CountingTraits {
    static void AddRef(void* texture) {
        FakeSurface* surface = AsSurface(texture);
        if (!surface) {
            return;
        }
        if (surface->destroyed) {
            surface->addRefAfterDestruction = true;
        }
        ++surface->references;
    }
    static void Release(void* texture) {
        FakeSurface* surface = AsSurface(texture);
        if (!surface) {
            return;
        }
        if (surface->references <= 0) {
            surface->releaseBelowZero = true;
            return;
        }
        if (--surface->references == 0) {
            surface->destroyed = true;
        }
    }
};

using Bindings = ce::legacy_d3d::TextureBindingSet<CountingTraits>;

// A surface the application owns one reference to, as DirectDraw hands it out.
void* CreateSurface(size_t index) {
    g_surfaces[index] = FakeSurface{1, false, false, false};
    return &g_surfaces[index];
}

void ApplicationRelease(void* handle) {
    CountingTraits::Release(handle);
}

int References(void* handle) {
    return AsSurface(handle)->references;
}

bool Destroyed(void* handle) {
    return AsSurface(handle)->destroyed;
}

// Every test asserts this: a binding set must never revive or over-release a
// surface, whatever sequence of binds it sees.
::testing::AssertionResult ReferenceCountingStayedSound() {
    for (size_t i = 0; i < kFakeSurfaceCount; ++i) {
        if (g_surfaces[i].addRefAfterDestruction) {
            return ::testing::AssertionFailure() << "AddRef on destroyed surface " << i;
        }
        if (g_surfaces[i].releaseBelowZero) {
            return ::testing::AssertionFailure() << "Release below zero references on surface " << i;
        }
    }
    return ::testing::AssertionSuccess();
}

class LegacyD3DTextureBindingsTest : public ::testing::Test {
protected:
    void SetUp() override {
        for (auto& surface : g_surfaces) {
            surface = FakeSurface{};
        }
    }
    void TearDown() override {
        EXPECT_TRUE(ReferenceCountingStayedSound());
    }
};

}  // namespace

TEST_F(LegacyD3DTextureBindingsTest, ABoundSurfaceSurvivesTheApplicationReleasingIt) {
    Bindings bindings;
    void* texture = CreateSurface(1);

    ASSERT_TRUE(bindings.Bind(0, texture));
    ApplicationRelease(texture);

    // This is the crash: without CE's reference the surface is destroyed here
    // while the device still has it bound, and the state-block restore hands
    // the destroyed interface back to SetTexture.
    EXPECT_FALSE(Destroyed(texture));
    EXPECT_EQ(bindings.Get(0), texture);
    EXPECT_TRUE(bindings.RestoreIsReferenceSafe());
}

TEST_F(LegacyD3DTextureBindingsTest, BindingAnotherTextureReleasesThePreviousOne) {
    Bindings bindings;
    void* first = CreateSurface(1);
    void* second = CreateSurface(2);

    ASSERT_TRUE(bindings.Bind(0, first));
    ApplicationRelease(first);
    ASSERT_TRUE(bindings.Bind(0, second));

    // The application had already let go of the first surface, so CE's binding
    // was the last reference and dropping it is what finally destroys it. The
    // extra lifetime is exactly one binding, never unbounded.
    EXPECT_TRUE(Destroyed(first));
    EXPECT_FALSE(Destroyed(second));
    EXPECT_EQ(bindings.Get(0), second);
}

TEST_F(LegacyD3DTextureBindingsTest, RebindingTheSameTextureDoesNotChangeReferences) {
    Bindings bindings;
    void* texture = CreateSurface(1);

    ASSERT_TRUE(bindings.Bind(0, texture));
    const int afterFirstBind = References(texture);
    ASSERT_TRUE(bindings.Bind(0, texture));

    EXPECT_EQ(References(texture), afterFirstBind);
}

TEST_F(LegacyD3DTextureBindingsTest, MovingATextureBetweenStagesNeverDropsItToZero) {
    Bindings bindings;
    void* texture = CreateSurface(1);

    ASSERT_TRUE(bindings.Bind(0, texture));
    ApplicationRelease(texture);
    ASSERT_TRUE(bindings.Bind(1, texture));
    ASSERT_TRUE(bindings.Bind(0, nullptr));

    EXPECT_FALSE(Destroyed(texture));
    EXPECT_EQ(bindings.Get(1), texture);
    EXPECT_EQ(bindings.Get(0), nullptr);
}

TEST_F(LegacyD3DTextureBindingsTest, UnbindingReleasesTheOwnedReference) {
    Bindings bindings;
    void* texture = CreateSurface(1);

    ASSERT_TRUE(bindings.Bind(0, texture));
    ApplicationRelease(texture);
    ASSERT_TRUE(bindings.Bind(0, nullptr));

    EXPECT_TRUE(Destroyed(texture));
    EXPECT_EQ(bindings.Get(0), nullptr);
}

TEST_F(LegacyD3DTextureBindingsTest, AStageOutsideTheTrackedRangeRevokesTheRestoreProof) {
    Bindings bindings;
    void* texture = CreateSurface(1);

    EXPECT_TRUE(bindings.RestoreIsReferenceSafe());
    EXPECT_FALSE(bindings.Bind(ce::legacy_d3d::kTextureStageCount, texture));

    // CE cannot own what it cannot record, so the state-block restore must stop
    // being trusted rather than silently restoring an unowned binding.
    EXPECT_FALSE(bindings.RestoreIsReferenceSafe());
    EXPECT_EQ(bindings.Get(ce::legacy_d3d::kTextureStageCount), nullptr);
}

TEST_F(LegacyD3DTextureBindingsTest, ReleaseAllDropsEveryOwnedReferenceAndRestoresTheProof) {
    Bindings bindings;
    void* first = CreateSurface(1);
    void* second = CreateSurface(2);

    ASSERT_TRUE(bindings.Bind(0, first));
    ASSERT_TRUE(bindings.Bind(3, second));
    ApplicationRelease(first);
    ApplicationRelease(second);
    EXPECT_FALSE(bindings.Bind(ce::legacy_d3d::kTextureStageCount, first));

    bindings.ReleaseAll();

    EXPECT_TRUE(Destroyed(first));
    EXPECT_TRUE(Destroyed(second));
    EXPECT_EQ(bindings.Get(0), nullptr);
    EXPECT_EQ(bindings.Get(3), nullptr);
    EXPECT_TRUE(bindings.RestoreIsReferenceSafe());
}

TEST_F(LegacyD3DTextureBindingsTest, TeardownCanDropBindingsWithoutCallingIntoTheRuntime) {
    void* texture = CreateSurface(1);
    {
        Bindings bindings;
        ASSERT_TRUE(bindings.Bind(0, texture));
        // Process teardown: the runtime may already be unloaded, so leaking a
        // reference the process is about to drop beats calling into it.
        bindings.ReleaseAll(false);
        EXPECT_EQ(bindings.Get(0), nullptr);
    }

    EXPECT_EQ(References(texture), 2);
    EXPECT_FALSE(Destroyed(texture));
}

TEST_F(LegacyD3DTextureBindingsTest, DestructionReleasesStillHeldBindings) {
    void* texture = CreateSurface(1);
    {
        Bindings bindings;
        ASSERT_TRUE(bindings.Bind(0, texture));
        ApplicationRelease(texture);
        EXPECT_FALSE(Destroyed(texture));
    }

    EXPECT_TRUE(Destroyed(texture));
}
