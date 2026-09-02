#include <gtest/gtest.h>

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../hook/common/nv_lod_spread_override.h"
#include "source_fragment_reader.h"

namespace {

using ce::nv_lod_spread::CodePatchResult;
using ce::nv_lod_spread::FindOnBranch;
using ce::nv_lod_spread::FindSettingSite;
using ce::nv_lod_spread::FindTableLoadDisp;
using ce::nv_lod_spread::IsIcdModuleName;
using ce::nv_lod_spread::IsNeutralizedBranch;
using ce::nv_lod_spread::kCmpLength;
using ce::nv_lod_spread::kSettingOff;
using ce::nv_lod_spread::kSettingOn;
using ce::nv_lod_spread::Mode;
using ce::nv_lod_spread::ParseMode;
using ce::nv_lod_spread::Site;
using ce::nv_lod_spread::WriteBranchDisplacementPatch;

// Layout of the purpose-built synthetic image the scanner is exercised against.
// It preserves only the structural relationships the scanner validates; it is
// not an excerpt from a driver binary.
constexpr size_t kImageSize = 0x4000;
constexpr size_t kTextRva = 0x1000;
constexpr size_t kTextSize = 0x1000;
constexpr size_t kCmpRva = 0x1100;
constexpr size_t kSettingRva = 0x3000;
// Deliberately representable in 32 bits: the 32-bit driver's cmp encodes an
// absolute address, and the tests build for both architectures.
constexpr uintptr_t kFakeImageBase = 0x10000000u;

// Representative distances that leave unrelated synthetic instructions between
// the compare, branch, and two paths.
constexpr size_t kBranchDelta = 0x21;
constexpr uint8_t kBranchDisplacement = 0x15;
constexpr uint8_t kOnSlotDisp = 0x30;
constexpr uint8_t kOffSlotDisp = 0x2C;

struct ImageOptions {
    bool is64Bit = true;
    uint32_t settingValue = kSettingOff;
    uint8_t onSlotDisp = kOnSlotDisp;
    uint8_t offSlotDisp = kOffSlotDisp;
};

class VirtualPage {
   public:
    VirtualPage()
        : data_(static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))) {}
    ~VirtualPage() {
        if (data_) {
            VirtualFree(data_, 0, MEM_RELEASE);
        }
    }

    VirtualPage(const VirtualPage&) = delete;
    VirtualPage& operator=(const VirtualPage&) = delete;

    uint8_t* Data() const { return data_; }

    bool MakeExecutableReadOnly() const {
        DWORD oldProtect = 0;
        return data_ && VirtualProtect(data_, 4096, PAGE_EXECUTE_READ, &oldProtect) != FALSE;
    }

   private:
    uint8_t* data_ = nullptr;
};

void PutBytes(std::vector<uint8_t>& image, size_t offset, std::initializer_list<uint8_t> bytes) {
    size_t cursor = offset;
    for (uint8_t byte : bytes) {
        image[cursor++] = byte;
    }
}

std::vector<uint8_t> MakeImage(const ImageOptions& options) {
    std::vector<uint8_t> image(kImageSize, 0x00);

    // cmp dword ptr [<setting>], 0x37299934
    image[kCmpRva] = 0x81;
    image[kCmpRva + 1] = 0x3D;
    if (options.is64Bit) {
        const auto displacement =
            static_cast<int32_t>(static_cast<int64_t>(kSettingRva) - static_cast<int64_t>(kCmpRva + kCmpLength));
        memcpy(&image[kCmpRva + 2], &displacement, sizeof(displacement));
    } else {
        const auto absolute = static_cast<uint32_t>(kFakeImageBase + kSettingRva);
        memcpy(&image[kCmpRva + 2], &absolute, sizeof(absolute));
    }
    const uint32_t immediate = kSettingOn;
    memcpy(&image[kCmpRva + 6], &immediate, sizeof(immediate));

    // Fill the unrelated area with synthetic sentinels so it cannot be mistaken
    // for a branch, table load, or pre-existing NOP patch.
    std::fill(image.begin() + kCmpRva + kCmpLength, image.begin() + kCmpRva + kBranchDelta, 0xCC);

    // jne <off path>
    PutBytes(image, kCmpRva + kBranchDelta, {0x75, kBranchDisplacement});

    // Fall-through: the ON slot load.
    PutBytes(image, kCmpRva + kBranchDelta + 2,
             {0x0F, 0xB6, 0x51, 0x29,                 // movzx edx, byte [rcx+0x29]
              0x44, 0x8B, 0x41, options.onSlotDisp});  // mov r8d, [rcx+0x30]

    // Branch target: the OFF slot load.
    const size_t offPath = kCmpRva + kBranchDelta + 2 + kBranchDisplacement;
    // The ON path skips over the OFF block after loading its table slot. This
    // also lets the scanner prove that a pre-existing pair of NOPs is the
    // documented patch rather than unrelated padding.
    PutBytes(image, offPath - 2, {0xEB, 0x08});
    PutBytes(image, offPath,
             {0x0F, 0xB6, 0x41, 0x29,                   // movzx eax, byte [rcx+0x29]
              0x44, 0x8B, 0x41, options.offSlotDisp});  // mov r8d, [rcx+0x2C]

    memcpy(&image[kSettingRva], &options.settingValue, sizeof(options.settingValue));
    return image;
}

bool Scan(const std::vector<uint8_t>& image, const ImageOptions& options, Site& site) {
    return FindSettingSite(image.data(), image.size(), kTextRva, kTextSize, options.is64Bit, kFakeImageBase, site);
}

TEST(NvLodSpreadOverride, ParseModeOnlyOptsInOnExplicitSpellings) {
    EXPECT_EQ(ParseMode("on"), Mode::kOn);
    EXPECT_EQ(ParseMode("ON"), Mode::kOn);
    EXPECT_EQ(ParseMode("  true  "), Mode::kOn);
    EXPECT_EQ(ParseMode("1"), Mode::kOn);
    EXPECT_EQ(ParseMode("enabled"), Mode::kOn);

    // Everything else, including a typo, leaves the driver alone.
    EXPECT_EQ(ParseMode("off"), Mode::kOff);
    EXPECT_EQ(ParseMode("default"), Mode::kOff);
    EXPECT_EQ(ParseMode(""), Mode::kOff);
    EXPECT_EQ(ParseMode("onn"), Mode::kOff);
    EXPECT_EQ(ParseMode("yes"), Mode::kOff);
}

TEST(NvLodSpreadOverride, IsIcdModuleNameMatchesBothArchitectures) {
    EXPECT_TRUE(IsIcdModuleName("nvoglv64.dll"));
    EXPECT_TRUE(IsIcdModuleName("nvoglv32.dll"));
    EXPECT_TRUE(IsIcdModuleName("C:\\Windows\\System32\\DriverStore\\FileRepository\\nv_dispi\\NVOGLV64.DLL"));
    EXPECT_TRUE(IsIcdModuleName("C:/Windows/System32/nvoglv64.dll"));

    EXPECT_FALSE(IsIcdModuleName("nvapi64.dll"));
    EXPECT_FALSE(IsIcdModuleName("vulkan-1.dll"));
    EXPECT_FALSE(IsIcdModuleName("nvoglv64.dll.bak"));
    EXPECT_FALSE(IsIcdModuleName(""));
    EXPECT_FALSE(IsIcdModuleName(nullptr));
}

TEST(NvLodSpreadOverride, RecognizesBothNeutralizedBranchForms) {
    // CE's own form, at every short-jcc opcode.
    EXPECT_TRUE(IsNeutralizedBranch(0x75, 0x00));
    EXPECT_TRUE(IsNeutralizedBranch(0x70, 0x00));
    EXPECT_TRUE(IsNeutralizedBranch(0x7F, 0x00));
    // The pair an on-disk patch, or a CE build before 0.1.6368, leaves behind.
    EXPECT_TRUE(IsNeutralizedBranch(0x90, 0x90));

    EXPECT_FALSE(IsNeutralizedBranch(0x75, 0x05));
    EXPECT_FALSE(IsNeutralizedBranch(0x90, 0x00));
    EXPECT_FALSE(IsNeutralizedBranch(0x6F, 0x00));
    EXPECT_FALSE(IsNeutralizedBranch(0x80, 0x00));
    EXPECT_FALSE(IsNeutralizedBranch(0x00, 0x00));
}

// The regression this file exists for: 32.0.16.1656 put the 32-bit ICD's branch
// at nvoglv32+0x576C27. Module bases are 64KB-aligned, so the site's address is
// 7 modulo 8 - the one alignment the former two-byte NOP writer had to refuse,
// which silently turned nv_lod_spread_fix=on into a no-op. Forcing the
// fall-through touches a single byte, so every alignment in a page works.
TEST(NvLodSpreadOverride, NeutralizesTheBranchAtEveryAlignmentInAPage) {
    for (size_t alignment = 0; alignment < 16; ++alignment) {
        VirtualPage page;
        ASSERT_NE(page.Data(), nullptr) << "alignment " << alignment;

        const size_t patchOffset = 0x100 + alignment;
        uint8_t* site = page.Data() + patchOffset;
        // Surround the branch with recognizable neighbours so a wider write
        // than the displacement byte itself cannot go unnoticed.
        for (size_t i = 0; i < 8; ++i) {
            page.Data()[patchOffset - 4 + i] = static_cast<uint8_t>(0xA0 + i);
        }
        site[0] = 0x75;
        site[1] = 0x05;
        uint8_t before[12] = {};
        memcpy(before, page.Data() + patchOffset - 4, sizeof(before));
        ASSERT_TRUE(page.MakeExecutableReadOnly()) << "alignment " << alignment;

        const auto outcome = WriteBranchDisplacementPatch(site, 0x75, 0x05);

        EXPECT_TRUE(outcome.Succeeded()) << "alignment " << alignment;
        EXPECT_EQ(outcome.result, CodePatchResult::kPatched) << "alignment " << alignment;
        EXPECT_TRUE(outcome.wroteBytes) << "alignment " << alignment;
        EXPECT_TRUE(outcome.instructionCacheFlushed) << "alignment " << alignment;
        EXPECT_TRUE(outcome.protectionRestored) << "alignment " << alignment;
        EXPECT_TRUE(outcome.verified) << "alignment " << alignment;
        // The opcode is deliberately preserved: jcc +0 falls through either way.
        EXPECT_EQ(site[0], 0x75) << "alignment " << alignment;
        EXPECT_EQ(site[1], 0x00) << "alignment " << alignment;
        EXPECT_TRUE(IsNeutralizedBranch(site[0], site[1])) << "alignment " << alignment;
        for (size_t i = 0; i < sizeof(before); ++i) {
            if (i == 5) {  // the displacement byte itself
                continue;
            }
            EXPECT_EQ(page.Data()[patchOffset - 4 + i], before[i]) << "alignment " << alignment << " byte " << i;
        }
    }
}

// The Strange Brigade x64 site (nvoglv64+0x4E35DB, 3 modulo 8) that motivated
// the former 64-bit writer must keep working through the narrower write.
TEST(NvLodSpreadOverride, NeutralizesTheObservedStrangeBrigadeX64Alignment) {
    VirtualPage page;
    ASSERT_NE(page.Data(), nullptr);

    constexpr size_t kPatchOffset = 0x5DB;
    page.Data()[kPatchOffset - 1] = 0xCC;
    page.Data()[kPatchOffset] = 0x75;
    page.Data()[kPatchOffset + 1] = 0x15;
    page.Data()[kPatchOffset + 2] = 0x0F;
    ASSERT_TRUE(page.MakeExecutableReadOnly());

    const auto outcome = WriteBranchDisplacementPatch(page.Data() + kPatchOffset, 0x75, 0x15);

    EXPECT_TRUE(outcome.Succeeded());
    EXPECT_EQ(outcome.result, CodePatchResult::kPatched);
    EXPECT_EQ(page.Data()[kPatchOffset - 1], 0xCC);
    EXPECT_EQ(page.Data()[kPatchOffset], 0x75);
    EXPECT_EQ(page.Data()[kPatchOffset + 1], 0x00);
    EXPECT_EQ(page.Data()[kPatchOffset + 2], 0x0F);
}

TEST(NvLodSpreadOverride, ASecondPassOverTheSameSiteReportsAlreadyPatched) {
    VirtualPage page;
    ASSERT_NE(page.Data(), nullptr);

    constexpr size_t kPatchOffset = 0x27;
    page.Data()[kPatchOffset] = 0x75;
    page.Data()[kPatchOffset + 1] = 0x05;
    ASSERT_TRUE(page.MakeExecutableReadOnly());

    ASSERT_TRUE(WriteBranchDisplacementPatch(page.Data() + kPatchOffset, 0x75, 0x05).Succeeded());
    const auto again = WriteBranchDisplacementPatch(page.Data() + kPatchOffset, 0x75, 0x05);

    EXPECT_TRUE(again.Succeeded());
    EXPECT_EQ(again.result, CodePatchResult::kAlreadyPatched);
    EXPECT_FALSE(again.wroteBytes);
    EXPECT_TRUE(again.verified);
}

TEST(NvLodSpreadOverride, RefusesToWriteWhenTheSiteNoLongerHoldsTheValidatedBranch) {
    alignas(8) uint8_t bytes[16] = {};
    bytes[7] = 0x75;
    bytes[8] = 0x07;  // some other displacement than the one that was validated

    const auto stale = WriteBranchDisplacementPatch(bytes + 7, 0x75, 0x05);
    EXPECT_FALSE(stale.Succeeded());
    EXPECT_EQ(stale.result, CodePatchResult::kUnexpectedBytes);
    EXPECT_EQ(bytes[8], 0x07);

    bytes[7] = 0xE9;  // not a short conditional branch at all
    const auto notABranch = WriteBranchDisplacementPatch(bytes + 7, 0xE9, 0x07);
    EXPECT_FALSE(notABranch.Succeeded());
    EXPECT_EQ(notABranch.result, CodePatchResult::kUnexpectedBytes);
    EXPECT_EQ(bytes[8], 0x07);

    const auto nullSite = WriteBranchDisplacementPatch(nullptr, 0x75, 0x05);
    EXPECT_FALSE(nullSite.Succeeded());
    EXPECT_EQ(nullSite.result, CodePatchResult::kInvalidAddress);
}

TEST(NvLodSpreadOverride, FindsValidatedSyntheticEncoding) {
    const ImageOptions options;
    const std::vector<uint8_t> image = MakeImage(options);

    Site site;
    ASSERT_TRUE(Scan(image, options, site));
    EXPECT_TRUE(site.found);
    EXPECT_EQ(site.cmpRva, kCmpRva);
    EXPECT_EQ(site.settingRva, kSettingRva);
    EXPECT_EQ(site.settingValue, kSettingOff);
    EXPECT_TRUE(site.branchFound);
    EXPECT_EQ(site.branchRva, kCmpRva + kBranchDelta);
    EXPECT_EQ(site.branchLength, 2u);
}

TEST(NvLodSpreadOverride, Finds32BitAbsoluteEncoding) {
    ImageOptions options;
    options.is64Bit = false;
    const std::vector<uint8_t> image = MakeImage(options);

    Site site;
    ASSERT_TRUE(Scan(image, options, site));
    EXPECT_EQ(site.settingRva, kSettingRva);
    EXPECT_EQ(site.settingValue, kSettingOff);
    EXPECT_TRUE(site.branchFound);
}

TEST(NvLodSpreadOverride, AcceptsADriverThatAlreadyReportsOn) {
    ImageOptions options;
    options.settingValue = kSettingOn;
    const std::vector<uint8_t> image = MakeImage(options);

    Site site;
    ASSERT_TRUE(Scan(image, options, site));
    EXPECT_EQ(site.settingValue, kSettingOn);
}

// The self-validation that keeps this from ever becoming a blind write: the
// resolved address must actually hold one of the two documented enum payloads.
TEST(NvLodSpreadOverride, RefusesWhenTheResolvedGlobalIsNotTheSetting) {
    ImageOptions options;
    options.settingValue = 0xDEADBEEFu;
    const std::vector<uint8_t> image = MakeImage(options);

    Site site;
    EXPECT_FALSE(Scan(image, options, site));
    EXPECT_FALSE(site.found);
}

TEST(NvLodSpreadOverride, RefusesWhenTheImmediateIsNotTheOnConstant) {
    const ImageOptions options;
    std::vector<uint8_t> image = MakeImage(options);
    const uint32_t wrongImmediate = kSettingOff;
    memcpy(&image[kCmpRva + 6], &wrongImmediate, sizeof(wrongImmediate));

    Site site;
    EXPECT_FALSE(Scan(image, options, site));
}

TEST(NvLodSpreadOverride, RefusesAResolvedAddressOutsideTheImage) {
    ImageOptions options;
    std::vector<uint8_t> image = MakeImage(options);
    const int32_t runaway = 0x00400000;
    memcpy(&image[kCmpRva + 2], &runaway, sizeof(runaway));

    Site site;
    EXPECT_FALSE(Scan(image, options, site));
}

// The setting is still found, but no code is changed unless NOPing is provably
// equivalent to selecting the ON table slot.
TEST(NvLodSpreadOverride, WithholdsThePatchWhenTableSlotsAreNotAdjacent) {
    ImageOptions options;
    options.offSlotDisp = 0x20;
    const std::vector<uint8_t> image = MakeImage(options);

    Site site;
    ASSERT_TRUE(Scan(image, options, site));
    EXPECT_TRUE(site.found);
    EXPECT_FALSE(site.branchFound);
}

TEST(NvLodSpreadOverride, WithholdsThePatchWhenFallthroughIsTheOffPath) {
    ImageOptions options;
    options.onSlotDisp = kOffSlotDisp;
    options.offSlotDisp = kOnSlotDisp;
    const std::vector<uint8_t> image = MakeImage(options);

    Site site;
    ASSERT_TRUE(Scan(image, options, site));
    EXPECT_TRUE(site.found);
    EXPECT_FALSE(site.branchFound);
}

TEST(NvLodSpreadOverride, WithholdsThePatchWhenNoShortBranchGuardsThePath) {
    ImageOptions options;
    std::vector<uint8_t> image = MakeImage(options);
    // Turn the jne into a two-byte nop so no conditional branch remains.
    image[kCmpRva + kBranchDelta] = 0x66;
    image[kCmpRva + kBranchDelta + 1] = 0x90;

    Site site;
    ASSERT_TRUE(Scan(image, options, site));
    EXPECT_TRUE(site.found);
    EXPECT_FALSE(site.branchFound);
}

TEST(NvLodSpreadOverride, RecognizesTheStructurallyValidatedNopPatch) {
    const ImageOptions options;
    std::vector<uint8_t> image = MakeImage(options);
    image[kCmpRva + kBranchDelta] = 0x90;
    image[kCmpRva + kBranchDelta + 1] = 0x90;

    Site site;
    ASSERT_TRUE(Scan(image, options, site));
    EXPECT_TRUE(site.branchFound);
    EXPECT_TRUE(site.branchAlreadyPatched);
    EXPECT_EQ(site.branchRva, kCmpRva + kBranchDelta);
    EXPECT_EQ(site.branchLength, 2u);
}

// A process CE already patched, or one a second CE copy patched first, holds a
// zero-displacement branch rather than NOPs. It has to be recognized as the
// documented patch, and only when the same two paths remain provable.
TEST(NvLodSpreadOverride, RecognizesTheStructurallyValidatedZeroDisplacementPatch) {
    const ImageOptions options;
    std::vector<uint8_t> image = MakeImage(options);
    image[kCmpRva + kBranchDelta + 1] = 0x00;

    Site site;
    ASSERT_TRUE(Scan(image, options, site));
    EXPECT_TRUE(site.branchFound);
    EXPECT_TRUE(site.branchAlreadyPatched);
    EXPECT_EQ(site.branchRva, kCmpRva + kBranchDelta);
    EXPECT_EQ(site.branchLength, 2u);
}

TEST(NvLodSpreadOverride, RefusesAZeroDisplacementBranchWithoutTheProvenPaths) {
    ImageOptions options;
    options.offSlotDisp = 0x20;  // no longer four bytes below the ON slot
    std::vector<uint8_t> image = MakeImage(options);
    image[kCmpRva + kBranchDelta + 1] = 0x00;

    Site site;
    ASSERT_TRUE(Scan(image, options, site));
    EXPECT_TRUE(site.found);
    EXPECT_FALSE(site.branchFound);
    EXPECT_FALSE(site.branchAlreadyPatched);
}

// The literal instruction sequence at nvoglv32+0x576C04 in 32.0.16.1656, whose
// branch lands at +0x576C27. The scanner has to resolve it end to end, because
// this is the exact layout that reached the field as "nothing patched".
TEST(NvLodSpreadOverride, ResolvesTheReal32BitDriverSequence) {
    static constexpr uint8_t kSequence[] = {
        0x81, 0x3D, 0x00, 0x00, 0x00, 0x00, 0x34, 0x99, 0x29, 0x37,  // cmp [setting], ON
        0x8B, 0x8B, 0xB4, 0xAA, 0x02, 0x00,                          // mov ecx, [ebx+0x2AAB4]
        0x89, 0x45, 0x0C,                                            // mov [ebp+0x0C], eax
        0x89, 0x45, 0xF8,                                            // mov [ebp-0x08], eax
        0x0F, 0xB6, 0x51, 0x29,                                      // movzx edx, byte [ecx+0x29]
        0x8B, 0x41, 0x24,                                            // mov eax, [ecx+0x24]
        0x89, 0x45, 0xFC,                                            // mov [ebp-0x04], eax
        0x89, 0x55, 0x08,                                            // mov [ebp+0x08], edx
        0x75, 0x05,                                                  // jne <off path>
        0x8B, 0x41, 0x30,                                            // mov eax, [ecx+0x30]  (ON)
        0xEB, 0x03,                                                  // jmp <join>
        0x8B, 0x41, 0x2C,                                            // mov eax, [ecx+0x2C]  (OFF)
        0x0F, 0xB6, 0x49, 0x28,                                      // movzx ecx, byte [ecx+0x28]
        0x89, 0x4D, 0xF4,                                            // mov [ebp-0x0C], ecx
    };
    static constexpr size_t kBranchInSequence = 0x23;
    static_assert(kSequence[kBranchInSequence] == 0x75);
    // Placed at the driver's own word alignment (0x576C04 is 4 modulo 8), so
    // the branch lands 7 modulo 8 exactly as it does in the shipped ICD - the
    // alignment the former two-byte writer refused is reproduced, not described.
    static constexpr size_t kRealCmpRva = kTextRva + 0x104;
    static_assert(kRealCmpRva % 8 == 0x576C04 % 8);
    static_assert((kRealCmpRva + kBranchInSequence) % 8 == 0x576C27 % 8);

    ImageOptions options;
    options.is64Bit = false;
    std::vector<uint8_t> image(kImageSize, 0xCC);
    memcpy(&image[kRealCmpRva], kSequence, sizeof(kSequence));
    const auto absolute = static_cast<uint32_t>(kFakeImageBase + kSettingRva);
    memcpy(&image[kRealCmpRva + 2], &absolute, sizeof(absolute));
    const uint32_t settingValue = kSettingOff;
    memcpy(&image[kSettingRva], &settingValue, sizeof(settingValue));

    Site site;
    ASSERT_TRUE(Scan(image, options, site));
    EXPECT_TRUE(site.found);
    EXPECT_EQ(site.cmpRva, kRealCmpRva);
    EXPECT_EQ(site.settingRva, kSettingRva);
    EXPECT_EQ(site.settingValue, kSettingOff);
    ASSERT_TRUE(site.branchFound);
    EXPECT_FALSE(site.branchAlreadyPatched);
    EXPECT_EQ(site.branchRva, kRealCmpRva + kBranchInSequence);
    EXPECT_EQ(site.branchLength, 2u);
}

TEST(NvLodSpreadOverride, VulkanLayerPatchesAfterInstanceAndBeforeDeviceCreation) {
    const std::string source = ce::test_source::ReadLogicalSource(
        std::filesystem::current_path() / "hook" / "vulkan_layer" / "vulkan_layer_hooks.cpp");
    ASSERT_FALSE(source.empty());

    const size_t instance = source.find("Capture_vkCreateInstance(");
    const size_t nextInstance = source.find("res = create_fn", instance);
    const size_t postInstancePatch = source.find("ApplyConfiguredNvLodSpreadFix();", nextInstance);
    const size_t device = source.find("Capture_vkCreateDevice(", postInstancePatch);
    ASSERT_NE(instance, std::string::npos);
    ASSERT_NE(nextInstance, std::string::npos);
    ASSERT_NE(postInstancePatch, std::string::npos);
    ASSERT_NE(device, std::string::npos);
    EXPECT_LT(nextInstance, postInstancePatch);
    EXPECT_LT(postInstancePatch, device);
}

TEST(NvLodSpreadOverride, FindTableLoadDispHandlesBothRegisterForms) {
    // mov r8d, dword ptr [rcx+0x30] - REX-prefixed, as in the 64-bit driver.
    const uint8_t withRex[] = {0x0F, 0xB6, 0x51, 0x29, 0x44, 0x8B, 0x41, 0x30};
    uint8_t disp = 0;
    ASSERT_TRUE(FindTableLoadDisp(withRex, sizeof(withRex), disp));
    EXPECT_EQ(disp, 0x30);

    // mov eax, dword ptr [ecx+0x2C] - no prefix, as in the 32-bit driver.
    const uint8_t withoutRex[] = {0x0F, 0xB6, 0x41, 0x29, 0x8B, 0x41, 0x2C};
    disp = 0;
    ASSERT_TRUE(FindTableLoadDisp(withoutRex, sizeof(withoutRex), disp));
    EXPECT_EQ(disp, 0x2C);
}

TEST(NvLodSpreadOverride, FindTableLoadDispSkipsSibAndRejectsEmptyWindows) {
    // mov eax, dword ptr [ecx+eax*1+0x30] - the SIB byte would move the
    // displacement, so this encoding must not be read as a table slot.
    const uint8_t withSib[] = {0x8B, 0x44, 0x01, 0x30};
    uint8_t disp = 0;
    EXPECT_FALSE(FindTableLoadDisp(withSib, sizeof(withSib), disp));

    const uint8_t noLoad[] = {0x90, 0x90, 0x90, 0x90};
    EXPECT_FALSE(FindTableLoadDisp(noLoad, sizeof(noLoad), disp));
    EXPECT_FALSE(FindTableLoadDisp(nullptr, 0, disp));
}

TEST(NvLodSpreadOverride, FindOnBranchIsBoundedByTheImage) {
    const ImageOptions options;
    const std::vector<uint8_t> image = MakeImage(options);

    // A cmp that sits so close to the end that neither path fits must not read
    // past the buffer, and must simply report no fallback site.
    Site site;
    EXPECT_FALSE(FindOnBranch(image.data(), kCmpRva + kCmpLength + 2, kCmpRva, site));
    EXPECT_FALSE(site.branchFound);
}

}  // namespace
