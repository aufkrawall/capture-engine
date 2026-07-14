#include <gtest/gtest.h>

#include "strict_integer_parse.h"

#include <cstdint>
#include <limits>

TEST(StrictIntegerParseTest, AcceptsExactSignedAndUnsignedValues) {
    int32_t signedValue = 0;
    uint32_t unsignedValue = 0;
    EXPECT_TRUE(ce::TryParseInt32("-7", signedValue));
    EXPECT_EQ(signedValue, -7);
    EXPECT_TRUE(ce::TryParseUInt32("4294967295", unsignedValue));
    EXPECT_EQ(unsignedValue, std::numeric_limits<uint32_t>::max());
    EXPECT_TRUE(ce::TryParseUInt32("FF", unsignedValue, 16));
    EXPECT_EQ(unsignedValue, 255u);
}

TEST(StrictIntegerParseTest, RejectsTrailingTextSignsAndOverflow) {
    int32_t signedValue = 123;
    uint32_t unsignedValue = 123;
    EXPECT_FALSE(ce::TryParseInt32("7oops", signedValue));
    EXPECT_FALSE(ce::TryParseInt32("2147483648", signedValue));
    EXPECT_FALSE(ce::TryParseUInt32("-1", unsignedValue));
    EXPECT_FALSE(ce::TryParseUInt32("4294967296", unsignedValue));
    EXPECT_FALSE(ce::TryParseUInt32("", unsignedValue));
    EXPECT_EQ(signedValue, 123);
    EXPECT_EQ(unsignedValue, 123u);
}
