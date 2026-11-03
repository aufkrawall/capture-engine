#include <gtest/gtest.h>

#include <d3d9.h>

#include "hook/d3d9/dx9_sampler_config_policy.h"
#include "hook/d3d9/dx9_sampler_publication.h"

// The D3D9 sampler shadow answers the per-draw common cases - a re-set of the
// value a reconciled sampler holds, a re-bind of its texture - from a
// publication, without the device lock (hook/d3d9/dx9_sampler_publication.h).
// The protocol: whoever changes the shadow holds the lock, withdraws the
// samplers it may change first and republishes the ones that end clean.
namespace {

using ce::dx9_sampler_state::kSamplerCount;
using ce::dx9_sampler_state::kTrackedTypes;
using ce::dx9_sampler_state::RepublishClean;
using ce::dx9_sampler_state::SamplerPublication;
using ce::dx9_sampler_state::SamplerShadow;
using ce::dx9_sampler_state::StateIndex;

constexpr size_t kMinFilter = 4;

SamplerShadow CleanShadow(const void* texture) {
    SamplerShadow shadow{};
    for (auto& sampler : shadow) {
        sampler.initialized = true;
        sampler.reconciled = true;
        sampler.boundTexture = texture;
    }
    return shadow;
}

}  // namespace

TEST(DX9SamplerPublicationTest, NothingIsAnsweredBeforeASamplerIsPublished) {
    SamplerPublication publication;
    EXPECT_FALSE(publication.IsRedundantSet(0, kMinFilter, D3DTEXF_POINT));
    int texture = 0;
    EXPECT_FALSE(publication.IsSameTexture(0, &texture));
}

TEST(DX9SamplerPublicationTest, APublishedSamplerAnswersOnlyItsOwnValuesAndTexture) {
    int texture = 0;
    int other = 0;
    SamplerShadow shadow = CleanShadow(&texture);
    shadow[3].logical[kMinFilter] = D3DTEXF_LINEAR;

    SamplerPublication publication;
    publication.Withdraw(SamplerPublication::Bit(3));
    RepublishClean(publication, shadow, SamplerPublication::Bit(3), true);
    EXPECT_TRUE(publication.IsRedundantSet(3, kMinFilter, D3DTEXF_LINEAR));
    EXPECT_FALSE(publication.IsRedundantSet(3, kMinFilter, D3DTEXF_POINT)) << "a new value is a real set";
    EXPECT_FALSE(publication.IsRedundantSet(2, kMinFilter, D3DTEXF_POINT)) << "only what was withdrawn returns";
    EXPECT_TRUE(publication.IsSameTexture(3, &texture));
    EXPECT_FALSE(publication.IsSameTexture(3, &other));
}

TEST(DX9SamplerPublicationTest, AnUnbindIsNeverASameTextureRebind) {
    SamplerPublication publication;
    publication.Publish(0, SamplerShadow{}[0].logical, nullptr);
    EXPECT_FALSE(publication.IsSameTexture(0, nullptr)) << "unbinding changes the stage's texture class";
}

TEST(DX9SamplerPublicationTest, AWithdrawnSamplerIsNotAnsweredUntilItEndsClean) {
    int texture = 0;
    SamplerShadow shadow = CleanShadow(&texture);
    SamplerPublication publication;
    RepublishClean(publication, shadow, SamplerPublication::kAllSamplers, true);
    ASSERT_EQ(publication.Clean(), SamplerPublication::kAllSamplers);

    // A change to sampler 5 that leaves it unreconciled - say a texture of a
    // new class was bound and the reconcile failed.
    publication.Withdraw(SamplerPublication::Bit(5));
    shadow[5].reconciled = false;
    RepublishClean(publication, shadow, SamplerPublication::Bit(5), true);
    EXPECT_FALSE(publication.IsRedundantSet(5, kMinFilter, shadow[5].logical[kMinFilter]));
    EXPECT_FALSE(publication.IsSameTexture(5, &texture));
    EXPECT_TRUE(publication.IsRedundantSet(4, kMinFilter, shadow[4].logical[kMinFilter])) << "others stay";

    shadow[5].reconciled = true;
    publication.Withdraw(SamplerPublication::Bit(5));
    RepublishClean(publication, shadow, SamplerPublication::Bit(5), true);
    EXPECT_TRUE(publication.IsRedundantSet(5, kMinFilter, shadow[5].logical[kMinFilter]));
}

TEST(DX9SamplerPublicationTest, AnUninitializedSamplerIsNeverPublished) {
    SamplerShadow shadow = CleanShadow(nullptr);
    shadow[7].initialized = false;
    SamplerPublication publication;
    RepublishClean(publication, shadow, SamplerPublication::kAllSamplers, true);
    EXPECT_EQ(publication.Clean() & SamplerPublication::Bit(7), 0u);
}

TEST(DX9SamplerPublicationTest, ADeviceThatMayNotAnswerPublishesNothing) {
    // Recording a state block, no override, or a configuration not yet applied.
    SamplerShadow shadow = CleanShadow(nullptr);
    SamplerPublication publication;
    RepublishClean(publication, shadow, SamplerPublication::kAllSamplers, false);
    EXPECT_EQ(publication.Clean(), 0u);
}

TEST(DX9SamplerPublicationTest, RepublishingCarriesTheShadowsNewValues) {
    int first = 0;
    int second = 0;
    SamplerShadow shadow = CleanShadow(&first);
    SamplerPublication publication;
    RepublishClean(publication, shadow, SamplerPublication::kAllSamplers, true);

    publication.Withdraw(SamplerPublication::Bit(0));
    shadow[0].logical[kMinFilter] = D3DTEXF_ANISOTROPIC;
    shadow[0].boundTexture = &second;
    RepublishClean(publication, shadow, SamplerPublication::Bit(0), true);
    EXPECT_TRUE(publication.IsRedundantSet(0, kMinFilter, D3DTEXF_ANISOTROPIC));
    EXPECT_FALSE(publication.IsSameTexture(0, &first));
    EXPECT_TRUE(publication.IsSameTexture(0, &second));
}

TEST(DX9SamplerPublicationTest, EverySamplerHasItsOwnBit) {
    SamplerShadow shadow = CleanShadow(nullptr);
    SamplerPublication publication;
    for (size_t sampler = 0; sampler < kSamplerCount; ++sampler) {
        publication.Withdraw(SamplerPublication::kAllSamplers);
        RepublishClean(publication, shadow, SamplerPublication::Bit(sampler), true);
        EXPECT_EQ(publication.Clean(), SamplerPublication::Bit(sampler)) << sampler;
    }
}

TEST(DX9SamplerStateIndexTest, TheTableMatchesTheTrackedTypes) {
    for (size_t i = 0; i < kTrackedTypes.size(); ++i)
        EXPECT_EQ(StateIndex(kTrackedTypes[i]), static_cast<int>(i)) << kTrackedTypes[i];
    // Sampler states CE passes through untouched.
    for (D3DSAMPLERSTATETYPE type : {D3DSAMP_BORDERCOLOR, D3DSAMP_SRGBTEXTURE, D3DSAMP_ELEMENTINDEX,
                                     D3DSAMP_DMAPOFFSET, static_cast<D3DSAMPLERSTATETYPE>(0),
                                     static_cast<D3DSAMPLERSTATETYPE>(14), static_cast<D3DSAMPLERSTATETYPE>(0x7FFFFFFF)})
        EXPECT_EQ(StateIndex(type), -1) << type;
}
