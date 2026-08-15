#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../hook/common/ue5_rr_override_policy.h"

namespace {

std::vector<uint8_t> Utf16Le(const std::string& value) {
    std::vector<uint8_t> bytes;
    for (unsigned char character : value) {
        bytes.push_back(character);
        bytes.push_back(0);
    }
    bytes.push_back(0);
    bytes.push_back(0);
    return bytes;
}

std::array<uint8_t, 7> MakeLea(uint8_t modrm, uintptr_t instruction, uintptr_t target) {
    std::array<uint8_t, 7> bytes{0x48, 0x8D, modrm, 0, 0, 0, 0};
    const int64_t wideDisplacement = static_cast<int64_t>(target) - static_cast<int64_t>(instruction + bytes.size());
    const int32_t displacement = static_cast<int32_t>(wideDisplacement);
    std::memcpy(bytes.data() + 3, &displacement, sizeof(displacement));
    return bytes;
}

}  // namespace

TEST(UE5RROverridePolicyTest, FindsDenoiserModeUtf16LiteralCaseInsensitively) {
    std::vector<uint8_t> image(19, 0xCC);
    const std::vector<uint8_t> literal = Utf16Le("r.ngx.dlss.DENOISERmode");
    image.insert(image.end(), literal.begin(), literal.end());
    image.insert(image.end(), 11, 0xCC);

    EXPECT_EQ(ce::ue5_rr::FindUtf16LeAsciiInsensitive(image.data(), image.size(),
                                                      ce::ue5_rr::kDenoiserModeCVar),
              19u);
    EXPECT_EQ(ce::ue5_rr::FindUtf16LeAsciiInsensitive(image.data(), image.size(), "r.NGX.DLSS.Missing"),
              ce::ue5_rr::kNotFound);
}

TEST(UE5RROverridePolicyTest, RejectsNonUtf16AndMissingTerminatorLookalikes) {
    std::vector<uint8_t> literal = Utf16Le(ce::ue5_rr::kDenoiserModeCVar);
    literal[3] = 1;
    EXPECT_EQ(ce::ue5_rr::FindUtf16LeAsciiInsensitive(literal.data(), literal.size(),
                                                      ce::ue5_rr::kDenoiserModeCVar),
              ce::ue5_rr::kNotFound);

    literal = Utf16Le(ce::ue5_rr::kDenoiserModeCVar);
    literal.pop_back();
    EXPECT_EQ(ce::ue5_rr::FindUtf16LeAsciiInsensitive(literal.data(), literal.size(),
                                                      ce::ue5_rr::kDenoiserModeCVar),
              ce::ue5_rr::kNotFound);
}

TEST(UE5RROverridePolicyTest, DecodesPositiveAndNegativeRipRelativeLeaTargets) {
    constexpr uintptr_t kForwardInstruction = 0x1000;
    constexpr uintptr_t kForwardTarget = 0x1A20;
    const auto forwardBytes = MakeLea(0x15, kForwardInstruction, kForwardTarget);  // RDX
    const auto forward = ce::ue5_rr::DecodeRipRelativeReference(
        forwardBytes.data(), forwardBytes.size(), kForwardInstruction);
    ASSERT_TRUE(forward.valid);
    EXPECT_TRUE(forward.TakesAddress());
    EXPECT_EQ(forward.registerIndex, 2u);
    EXPECT_EQ(forward.length, 7u);
    EXPECT_EQ(forward.target, kForwardTarget);

    constexpr uintptr_t kBackwardInstruction = 0x3000;
    constexpr uintptr_t kBackwardTarget = 0x2100;
    const auto backwardBytes = MakeLea(0x0D, kBackwardInstruction, kBackwardTarget);  // RCX
    const auto backward = ce::ue5_rr::DecodeRipRelativeReference(
        backwardBytes.data(), backwardBytes.size(), kBackwardInstruction);
    ASSERT_TRUE(backward.valid);
    EXPECT_EQ(backward.registerIndex, 1u);
    EXPECT_EQ(backward.target, kBackwardTarget);
}

TEST(UE5RROverridePolicyTest, RejectsNonRipRelativeOrUnsupportedInstructions) {
    const std::array<uint8_t, 7> registerAddressing{0x48, 0x8D, 0x11, 0, 0, 0, 0};
    EXPECT_FALSE(ce::ue5_rr::DecodeRipRelativeReference(registerAddressing.data(), registerAddressing.size(),
                                                        0x1000)
                     .valid);

    const std::array<uint8_t, 7> callInstruction{0x48, 0xE8, 0x05, 0, 0, 0, 0};
    EXPECT_FALSE(ce::ue5_rr::DecodeRipRelativeReference(callInstruction.data(), callInstruction.size(), 0x1000)
                     .valid);
}

TEST(UE5RROverridePolicyTest, ScoresOnlyFullyValidatedAutoConsoleVariableCandidates) {
    ce::ue5_rr::CandidateEvidence evidence;
    evidence.nameLoadedIntoSecondArgument = true;
    evidence.objectLoadedIntoThis = true;
    evidence.objectAligned = true;
    evidence.objectInWritableSection = true;
    evidence.objectPageWritable = true;
    evidence.objectVtableCallable = true;
    evidence.targetObjectCallable = true;
    evidence.referenceDataReadable = true;
    evidence.shadowValuesPlausible = true;
    evidence.instructionDistance = 12;
    evidence.baseReferenceCount = 1;
    evidence.referenceFieldReferenceCount = 2;

    const int strongScore = ce::ue5_rr::ScoreCandidate(evidence);
    EXPECT_TRUE(ce::ue5_rr::IsUniquelyStrongCandidate(strongScore, strongScore - 8));
    EXPECT_FALSE(ce::ue5_rr::IsUniquelyStrongCandidate(strongScore, strongScore - 2));

    evidence.objectLoadedIntoThis = false;
    evidence.referenceFieldReferenceCount = 3;
    EXPECT_GE(ce::ue5_rr::ScoreCandidate(evidence), 125)
        << "inlined UE constructors may store the three object fields without materializing this in RCX";

    evidence.targetObjectCallable = false;
    EXPECT_EQ(ce::ue5_rr::ScoreCandidate(evidence), -1);
}

TEST(UE5RROverridePolicyTest, AcceptsLoneValidatedCandidateBelowStrongCutoff) {
    constexpr int kAllChecksScore = ce::ue5_rr::kMinimumAcceptableScore;
    EXPECT_TRUE(ce::ue5_rr::ShouldAcceptCandidate(kAllChecksScore, -1))
        << "a single candidate whose hard layout checks pass is unambiguous";
    EXPECT_TRUE(ce::ue5_rr::ShouldAcceptCandidate(124, -1));
    EXPECT_TRUE(ce::ue5_rr::ShouldAcceptCandidate(140, -1));
    EXPECT_FALSE(ce::ue5_rr::ShouldAcceptCandidate(kAllChecksScore - 1, -1))
        << "below the baseline a required layout check failed";
    EXPECT_FALSE(ce::ue5_rr::ShouldAcceptCandidate(kAllChecksScore, kAllChecksScore - 2))
        << "competing candidates still require a strong margin";
    EXPECT_FALSE(ce::ue5_rr::ShouldAcceptCandidate(124, 124));
}

TEST(UE5RROverridePolicyTest, AcceptsOnlyRealDenoiserModeShadowValues) {
    EXPECT_TRUE(ce::ue5_rr::IsPlausibleDenoiserModeShadow(0));
    EXPECT_TRUE(ce::ue5_rr::IsPlausibleDenoiserModeShadow(1));
    EXPECT_FALSE(ce::ue5_rr::IsPlausibleDenoiserModeShadow(-1));
    EXPECT_FALSE(ce::ue5_rr::IsPlausibleDenoiserModeShadow(2));
}
