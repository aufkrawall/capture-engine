#include <gtest/gtest.h>

#include "common/graphics/post_process_present_scope.h"

TEST(GammaPresentScope, NormalAndPostSlShareOneSuccessButNestedFramesAreIndependent) {
    ce::post_process_present::Scope scope;
    scope.Begin(1);
    EXPECT_FALSE(scope.Processed(1));
    scope.Mark(1);
    EXPECT_TRUE(scope.Processed(1));
    scope.Begin(1);
    EXPECT_FALSE(scope.Processed(1));
    scope.Mark(1);
    EXPECT_TRUE(scope.Processed(1));
    scope.End();
    EXPECT_TRUE(scope.Processed(1));
    scope.End();
    scope.Begin(1);
    EXPECT_FALSE(scope.Processed(1));
    scope.Mark(2);
    EXPECT_FALSE(scope.Processed(1));
    scope.End();
}

TEST(GammaPresentScope, BoundedStorageCannotOverwriteTheParentDuringDeepReentry) {
    ce::post_process_present::Scope scope;
    scope.Begin(1);
    scope.Mark(1);
    for (int depth = 0; depth < 100; ++depth) {
        scope.Begin(2);
        scope.Mark(2);
    }
    for (int depth = 0; depth < 100; ++depth)
        scope.End();
    EXPECT_TRUE(scope.Processed(1));
    scope.End();
    scope.End();
    EXPECT_FALSE(scope.Processed(1));
}
