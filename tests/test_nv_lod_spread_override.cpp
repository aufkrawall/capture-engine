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

using ce::nv_lod_spread::AtomicPatchWidth;
using ce::nv_lod_spread::CanPatchTwoBytesAtomically;
using ce::nv_lod_spread::CodePatchResult;
using ce::nv_lod_spread::FindOnBranch;
using ce::nv_lod_spread::FindSettingSite;
using ce::nv_lod_spread::FindTableLoadDisp;
using ce::nv_lod_spread::IsIcdModuleName;
using ce::nv_lod_spread::kCmpLength;
using ce::nv_lod_spread::kSettingOff;
using ce::nv_lod_spread::kSettingOn;
using ce::nv_lod_spread::Mode;
using ce::nv_lod_spread::ParseMode;
using ce::nv_lod_spread::SelectAtomicPatchWidth;
using ce::nv_lod_spread::Site;
using ce::nv_lod_spread::WriteTwoByteCodePatch;

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

TEST(NvLodSpreadOverride, AtomicPatchLayoutUses64BitsAcrossA32BitBoundary) {
    EXPECT_EQ(SelectAtomicPatchWidth(reinterpret_cast<const void*>(uintptr_t{0x1000})), AtomicPatchWidth::k32Bit);
    EXPECT_EQ(SelectAtomicPatchWidth(reinterpret_cast<const void*>(uintptr_t{0x1001})), AtomicPatchWidth::k32Bit);
    EXPECT_EQ(SelectAtomicPatchWidth(reinterpret_cast<const void*>(uintptr_t{0x1002})), AtomicPatchWidth::k32Bit);
    EXPECT_EQ(SelectAtomicPatchWidth(reinterpret_cast<const void*>(uintptr_t{0x1003})), AtomicPatchWidth::k64Bit);
    EXPECT_EQ(SelectAtomicPatchWidth(reinterpret_cast<const void*>(uintptr_t{0x1007})), AtomicPatchWidth::kNone);

    EXPECT_TRUE(CanPatchTwoBytesAtomically(reinterpret_cast<const void*>(uintptr_t{0x1003})));
    EXPECT_FALSE(CanPatchTwoBytesAtomically(reinterpret_cast<const void*>(uintptr_t{0x1007})));
    EXPECT_FALSE(CanPatchTwoBytesAtomically(nullptr));
}

TEST(NvLodSpreadOverride, AtomicallyPatchesTheObservedStrangeBrigadeX64Alignment) {
    VirtualPage page;
    ASSERT_NE(page.Data(), nullptr);

    // The failing 32.0.16.1088 x64 site was nvoglv64+0x4E35DB. Module
    // bases are page-aligned, so +0xDB reproduces the exact word alignment.
    constexpr size_t kPatchOffset = 0xDB;
    for (size_t i = kPatchOffset - 3; i < kPatchOffset + 5; ++i) {
        page.Data()[i] = static_cast<uint8_t>(0x40 + i - (kPatchOffset - 3));
    }
    page.Data()[kPatchOffset] = 0x75;
    page.Data()[kPatchOffset + 1] = 0x05;
    uint8_t before[8] = {};
    memcpy(before, page.Data() + kPatchOffset - 3, sizeof(before));
    ASSERT_TRUE(page.MakeExecutableReadOnly());

    const auto outcome = WriteTwoByteCodePatch(page.Data() + kPatchOffset, 0x75, 0x05);

    EXPECT_TRUE(outcome.Succeeded());
    EXPECT_EQ(outcome.result, CodePatchResult::kPatched);
    EXPECT_EQ(outcome.width, AtomicPatchWidth::k64Bit);
    EXPECT_TRUE(outcome.bytesPatched);
    EXPECT_TRUE(outcome.wroteBytes);
    EXPECT_TRUE(outcome.instructionCacheFlushed);
    EXPECT_TRUE(outcome.protectionRestored);
    EXPECT_TRUE(outcome.verified);
    EXPECT_EQ(page.Data()[kPatchOffset], 0x90);
    EXPECT_EQ(page.Data()[kPatchOffset + 1], 0x90);
    for (size_t i = 0; i < sizeof(before); ++i) {
        if (i == 3 || i == 4) {
            continue;
        }
        EXPECT_EQ(page.Data()[kPatchOffset - 3 + i], before[i]);
    }
}

TEST(NvLodSpreadOverride, AtomicallyPatchesAnOrdinary32BitContainer) {
    VirtualPage page;
    ASSERT_NE(page.Data(), nullptr);

    constexpr size_t kPatchOffset = 0xD9;
    page.Data()[kPatchOffset - 1] = 0xCC;
    page.Data()[kPatchOffset] = 0x75;
    page.Data()[kPatchOffset + 1] = 0x05;
    page.Data()[kPatchOffset + 2] = 0xC3;
    ASSERT_TRUE(page.MakeExecutableReadOnly());

    const auto outcome = WriteTwoByteCodePatch(page.Data() + kPatchOffset, 0x75, 0x05);

    EXPECT_TRUE(outcome.Succeeded());
    EXPECT_EQ(outcome.result, CodePatchResult::kPatched);
    EXPECT_EQ(outcome.width, AtomicPatchWidth::k32Bit);
    EXPECT_EQ(page.Data()[kPatchOffset - 1], 0xCC);
    EXPECT_EQ(page.Data()[kPatchOffset], 0x90);
    EXPECT_EQ(page.Data()[kPatchOffset + 1], 0x90);
    EXPECT_EQ(page.Data()[kPatchOffset + 2], 0xC3);
}

TEST(NvLodSpreadOverride, RefusesAPairCrossingThe64BitAtomicWord) {
    alignas(8) uint8_t bytes[16] = {};
    bytes[7] = 0x75;
    bytes[8] = 0x05;

    const auto outcome = WriteTwoByteCodePatch(bytes + 7, 0x75, 0x05);

    EXPECT_FALSE(outcome.Succeeded());
    EXPECT_EQ(outcome.result, CodePatchResult::kUnsupportedAlignment);
    EXPECT_EQ(outcome.width, AtomicPatchWidth::kNone);
    EXPECT_EQ(bytes[7], 0x75);
    EXPECT_EQ(bytes[8], 0x05);
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
