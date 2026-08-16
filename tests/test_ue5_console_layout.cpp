#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../hook/common/ue5_console_layout.h"
#include "../hook/common/ue5_cvar_override_policy.h"

namespace {

std::size_t SpecIndex(const char* name) {
    for (std::size_t index = 0; index < ce::ue5_cvar::kSpecs.size(); ++index) {
        if (std::strcmp(ce::ue5_cvar::kSpecs[index].name, name) == 0)
            return index;
    }
    ADD_FAILURE() << "unknown spec " << name;
    return 0;
}

// The shape CE actually met in Talos (20260816_161158): `object+0x50` holds a
// pointer whose first dword reads 0, and `object+0x58` holds
// 0x00007FF73B3A16F0 - the same address in all four ShowFlag objects, which is
// what proves it is a shared force mask rather than a per-variable shadow pair.
ce::ue5_layout::ObjectProbe ShowFlagProbe(std::size_t offset = ce::ue5_layout::kPrimaryValueOffset,
                                          uint32_t bitNumber = 17) {
    ce::ue5_layout::ObjectProbe probe;
    probe.offset = offset;
    probe.firstQword = 0x00007FF73B3A16C0ull;
    probe.secondQword = 0x00007FF73B3A16F0ull;
    probe.bitNumber = bitNumber;
    probe.bitNumberRead = true;
    probe.pointedValue = 0;
    probe.pointedValueRead = true;
    probe.firstTargetWritable = true;
    probe.secondTargetWritable = true;
    probe.pointersShareModuleData = true;
    return probe;
}

// `FConsoleVariableRef<int32>`: a pointer to the engine's own global, with the
// shadow pair behind it mirroring the value that global holds.
ce::ue5_layout::ObjectProbe ReferenceProbe(uint32_t value,
                                           std::size_t offset = ce::ue5_layout::kPrimaryValueOffset) {
    ce::ue5_layout::ObjectProbe probe;
    probe.offset = offset;
    probe.firstQword = 0x00007FF7D93A58F0ull;
    probe.secondQword = (uint64_t{value} << 32) | value;
    probe.pointedValue = value;
    probe.pointedValueRead = true;
    probe.firstTargetWritable = true;
    return probe;
}

// `FConsoleVariable<int32>`: the {game, render} pair stored inline.
ce::ue5_layout::ObjectProbe InlineProbe(uint32_t value,
                                        std::size_t offset = ce::ue5_layout::kPrimaryValueOffset) {
    ce::ue5_layout::ObjectProbe probe;
    probe.offset = offset;
    probe.firstQword = (uint64_t{value} << 32) | value;
    return probe;
}

ce::ue5_layout::Selection Select(const std::vector<ce::ue5_layout::ObjectProbe>& probes,
                                 std::size_t specIndex, bool allowBitReference) {
    return ce::ue5_layout::SelectLayout(probes.data(), probes.size(), specIndex, allowBitReference);
}

}  // namespace

// The bug this whole unit exists for: the ShowFlag objects satisfied the old
// fixed model (a readable pointer whose dword was a plausible value) while
// being a completely different structure.
TEST(UE5ConsoleLayout, ShowFlagShapeIsNotAReferencePointer) {
    const std::size_t showFlag = SpecIndex("ShowFlag.Grain");
    const ce::ue5_layout::ObjectProbe probe = ShowFlagProbe();

    EXPECT_TRUE(ce::ue5_layout::IsBitReference(probe));
    EXPECT_FALSE(ce::ue5_layout::IsReferencePointer(probe, showFlag));
    EXPECT_FALSE(ce::ue5_layout::IsInlinePair(probe, showFlag));
}

TEST(UE5ConsoleLayout, ShowFlagShapeSelectsTheBitReference) {
    const std::size_t showFlag = SpecIndex("ShowFlag.Vignette");
    const ce::ue5_layout::Selection selection = Select({ShowFlagProbe()}, showFlag, true);

    EXPECT_EQ(selection.kind, ce::ue5_layout::Kind::BitReference);
    EXPECT_EQ(selection.offset, ce::ue5_layout::kPrimaryValueOffset);
    EXPECT_FALSE(selection.ambiguous);
}

// Every other variable stays out of the bit path entirely, so two adjacent
// module pointers on an ordinary CVar object can never be driven as a bit.
TEST(UE5ConsoleLayout, BitReferenceIsRefusedWhenTheSpecIsNotAShowFlag) {
    const std::size_t lumen = SpecIndex("r.Lumen.Reflections.Temporal");
    const ce::ue5_layout::Selection selection = Select({ShowFlagProbe()}, lumen, false);

    EXPECT_EQ(selection.kind, ce::ue5_layout::Kind::None);
    EXPECT_FALSE(selection.ambiguous);
}

TEST(UE5ConsoleLayout, ShowFlagPredicateMatchesExactlyTheShowFlagSpecs) {
    std::size_t showFlags = 0;
    for (std::size_t index = 0; index < ce::ue5_cvar::kSpecs.size(); ++index) {
        const bool named = std::strncmp(ce::ue5_cvar::kSpecs[index].name, "ShowFlag.", 9) == 0;
        EXPECT_EQ(ce::ue5_cvar::IsShowFlagSpec(index), named) << ce::ue5_cvar::kSpecs[index].name;
        showFlags += named ? 1 : 0;
    }
    EXPECT_GT(showFlags, 0u);
}

TEST(UE5ConsoleLayout, ReferencePointerNeedsTheShadowPairToMirrorTheGlobal) {
    const std::size_t spec = SpecIndex("r.Lumen.Reflections.Temporal");

    EXPECT_TRUE(ce::ue5_layout::IsReferencePointer(ReferenceProbe(1), spec));

    // The pair disagreeing with the pointed value is the ShowFlag failure mode:
    // it means the qword behind the pointer is not this variable's shadow.
    ce::ue5_layout::ObjectProbe stale = ReferenceProbe(1);
    stale.secondQword = 0x00007FF73B3A16F0ull;
    EXPECT_FALSE(ce::ue5_layout::IsReferencePointer(stale, spec));

    // An implausible pointed value means the pointer is not a value pointer.
    ce::ue5_layout::ObjectProbe implausible = ReferenceProbe(0x41005600);
    EXPECT_FALSE(ce::ue5_layout::IsReferencePointer(implausible, spec));
}

TEST(UE5ConsoleLayout, ReferencePointerIsSelectedAcrossTheProbeWindow) {
    const std::size_t spec = SpecIndex("r.Lumen.Reflections.Temporal");
    const ce::ue5_layout::Selection selection =
        Select({InlineProbe(0, 0x48), ReferenceProbe(1, 0x58)}, spec, false);

    EXPECT_EQ(selection.kind, ce::ue5_layout::Kind::ReferencePointer);
    EXPECT_EQ(selection.offset, 0x58u);
}

// Two offsets carrying the same shape means CE cannot say which one holds the
// value, and guessing is what produced the ShowFlag writes in the first place.
TEST(UE5ConsoleLayout, TwoReferenceMatchesAreRefusedAsAmbiguous) {
    const std::size_t spec = SpecIndex("r.Lumen.Reflections.Temporal");
    const ce::ue5_layout::Selection selection =
        Select({ReferenceProbe(1, 0x48), ReferenceProbe(1, 0x50)}, spec, false);

    EXPECT_EQ(selection.kind, ce::ue5_layout::Kind::None);
    EXPECT_TRUE(selection.ambiguous);
}

// The inline pair is the weakest shape - zeroed padding satisfies it - so it is
// only accepted where the value block is known to begin.
TEST(UE5ConsoleLayout, InlinePairIsOnlyAcceptedAtThePrimaryOffset) {
    const std::size_t spec = SpecIndex("r.Lumen.Reflections.Temporal");

    const ce::ue5_layout::Selection primary = Select({InlineProbe(4)}, spec, false);
    EXPECT_EQ(primary.kind, ce::ue5_layout::Kind::InlinePair);
    EXPECT_EQ(primary.offset, ce::ue5_layout::kPrimaryValueOffset);

    const ce::ue5_layout::Selection padding = Select({InlineProbe(0, 0x48)}, spec, false);
    EXPECT_EQ(padding.kind, ce::ue5_layout::Kind::None);
    EXPECT_FALSE(padding.ambiguous);
}

TEST(UE5ConsoleLayout, InlinePairNeedsBothHalvesToAgreeAndBePlausible) {
    const std::size_t spec = SpecIndex("r.Lumen.Reflections.Temporal");

    ce::ue5_layout::ObjectProbe mismatched = InlineProbe(4);
    mismatched.firstQword = (uint64_t{5} << 32) | 4;
    EXPECT_FALSE(ce::ue5_layout::IsInlinePair(mismatched, spec));

    ce::ue5_layout::ObjectProbe implausible = InlineProbe(0x7F000000);
    EXPECT_FALSE(ce::ue5_layout::IsInlinePair(implausible, spec));

    // A usable pointer at the offset is the reference layout, never a pair.
    ce::ue5_layout::ObjectProbe pointerShaped = InlineProbe(4);
    pointerShaped.firstTargetWritable = true;
    EXPECT_FALSE(ce::ue5_layout::IsInlinePair(pointerShaped, spec));
}

TEST(UE5ConsoleLayout, BitReferenceRequiresTwoDistinctMaskPointersInOneModule) {
    ce::ue5_layout::ObjectProbe foreign = ShowFlagProbe();
    foreign.pointersShareModuleData = false;
    EXPECT_FALSE(ce::ue5_layout::IsBitReference(foreign));

    ce::ue5_layout::ObjectProbe identical = ShowFlagProbe();
    identical.secondQword = identical.firstQword;
    EXPECT_FALSE(ce::ue5_layout::IsBitReference(identical));

    ce::ue5_layout::ObjectProbe readOnly = ShowFlagProbe();
    readOnly.secondTargetWritable = false;
    EXPECT_FALSE(ce::ue5_layout::IsBitReference(readOnly));

    ce::ue5_layout::ObjectProbe wildBit = ShowFlagProbe(ce::ue5_layout::kPrimaryValueOffset,
                                                        ce::ue5_layout::kMaxBitNumber);
    EXPECT_FALSE(ce::ue5_layout::IsBitReference(wildBit));

    ce::ue5_layout::ObjectProbe unread = ShowFlagProbe();
    unread.bitNumberRead = false;
    EXPECT_FALSE(ce::ue5_layout::IsBitReference(unread));
}

// Diagnostic addressing only. CE does not write these bits: 0.1.6128 forced the
// four configured show flags off through the very numbers their console objects
// reported and Talos lost all lighting, so the index a console object carries is
// not proven to be the index the renderer reads the mask by. The decomposition
// is still checked because it is what the reported bit map prints - and because
// `Vignette` at 13 sits one bit above `GlobalIllumination` at 12, which is the
// neighbour an off-by-one would land on.
TEST(UE5ConsoleLayout, ForceMaskBitAddressing) {
    EXPECT_EQ(ce::ue5_layout::BitByteIndex(0), 0u);
    EXPECT_EQ(ce::ue5_layout::BitMask(0), 0x01);
    EXPECT_EQ(ce::ue5_layout::BitByteIndex(7), 0u);
    EXPECT_EQ(ce::ue5_layout::BitMask(7), 0x80);
    EXPECT_EQ(ce::ue5_layout::BitByteIndex(8), 1u);
    EXPECT_EQ(ce::ue5_layout::BitMask(8), 0x01);
    EXPECT_EQ(ce::ue5_layout::BitByteIndex(12), 1u);
    EXPECT_EQ(ce::ue5_layout::BitMask(12), 0x10);
    EXPECT_EQ(ce::ue5_layout::BitByteIndex(13), 1u);
    EXPECT_EQ(ce::ue5_layout::BitMask(13), 0x20);
    EXPECT_EQ(ce::ue5_layout::BitByteIndex(137), 17u);
    EXPECT_EQ(ce::ue5_layout::BitMask(137), 0x02);
}
