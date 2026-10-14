// Regression: session 20261014_204616 reported "Streamline runtime is unloading"
// for sl.reflex.dll/sl.pcl.dll half a second before those plugins first loaded.
#include "hook/streamline/streamline_feature_query_skip.h"

#include <gtest/gtest.h>

#include <cstring>

namespace {

using ce::streamline_runtime_policy::FeatureQuerySkip;
using ce::streamline_runtime_policy::FeatureQuerySkipName;
using ce::streamline_runtime_policy::IsTeardownFeatureQuerySkip;
using ce::streamline_runtime_policy::ShouldLogFeatureQuerySkip;

TEST(StreamlineFeatureQuerySkip, NotYetLoadedIsNeitherTeardownNorLogged) {
    for (FeatureQuerySkip skip : {FeatureQuerySkip::FeatureNotLoaded, FeatureQuerySkip::NoInterposer}) {
        EXPECT_FALSE(IsTeardownFeatureQuerySkip(skip)) << FeatureQuerySkipName(skip);
        EXPECT_FALSE(ShouldLogFeatureQuerySkip(skip)) << FeatureQuerySkipName(skip);
    }
    EXPECT_FALSE(ShouldLogFeatureQuerySkip(FeatureQuerySkip::None));
}

TEST(StreamlineFeatureQuerySkip, OnlyUnloadEvidenceIsReportedAsTeardown) {
    for (FeatureQuerySkip skip :
         {FeatureQuerySkip::TeardownInFlight, FeatureQuerySkip::UnloadRaced, FeatureQuerySkip::PinFailed}) {
        EXPECT_TRUE(IsTeardownFeatureQuerySkip(skip)) << FeatureQuerySkipName(skip);
        EXPECT_TRUE(ShouldLogFeatureQuerySkip(skip)) << FeatureQuerySkipName(skip);
    }
    for (FeatureQuerySkip skip :
         {FeatureQuerySkip::NotListed, FeatureQuerySkip::PinCapacity, FeatureQuerySkip::SnapshotFailed}) {
        EXPECT_FALSE(IsTeardownFeatureQuerySkip(skip)) << FeatureQuerySkipName(skip);
        EXPECT_TRUE(ShouldLogFeatureQuerySkip(skip)) << FeatureQuerySkipName(skip);
    }
}

TEST(StreamlineFeatureQuerySkip, EveryReasonHasADistinctName) {
    const FeatureQuerySkip all[] = {FeatureQuerySkip::None,          FeatureQuerySkip::FeatureNotLoaded,
                                    FeatureQuerySkip::NoInterposer,  FeatureQuerySkip::TeardownInFlight,
                                    FeatureQuerySkip::UnloadRaced,   FeatureQuerySkip::PinFailed,
                                    FeatureQuerySkip::NotListed,     FeatureQuerySkip::PinCapacity,
                                    FeatureQuerySkip::SnapshotFailed};
    for (size_t i = 0; i < std::size(all); ++i) {
        EXPECT_STRNE(FeatureQuerySkipName(all[i]), "unknown");
        for (size_t j = i + 1; j < std::size(all); ++j)
            EXPECT_STRNE(FeatureQuerySkipName(all[i]), FeatureQuerySkipName(all[j]));
    }
}

}  // namespace
