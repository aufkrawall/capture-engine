#include <gtest/gtest.h>

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../hook/common/nv_lod_spread_override.h"

namespace {

using ce::nv_lod_spread::FindOnBranch;
using ce::nv_lod_spread::FindSettingSite;
using ce::nv_lod_spread::FindTableLoadDisp;
using ce::nv_lod_spread::IsIcdModuleName;
using ce::nv_lod_spread::kCmpLength;
using ce::nv_lod_spread::kSettingOff;
using ce::nv_lod_spread::kSettingOn;
using ce::nv_lod_spread::Mode;
using ce::nv_lod_spread::ParseMode;
using ce::nv_lod_spread::Site;

// Layout of the synthetic image the scanner is exercised against. The code bytes
// below are copied from nvoglv64.dll 32.0.16.1088 so a regression here means CE
// stopped recognizing a driver it is known to handle.
constexpr size_t kImageSize = 0x4000;
constexpr size_t kTextRva = 0x1000;
constexpr size_t kTextSize = 0x1000;
constexpr size_t kCmpRva = 0x1100;
constexpr size_t kSettingRva = 0x3000;
// Deliberately representable in 32 bits: the 32-bit driver's cmp encodes an
// absolute address, and the tests build for both architectures.
constexpr uintptr_t kFakeImageBase = 0x10000000u;

// Distances measured in the shipped driver: the guarding jne sits 0x21 bytes past
// the cmp and skips 0x15 bytes into the OFF path.
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

    // The instructions the driver runs between the compare and the branch. None
    // of them may be mistaken for the branch or for a table load.
    PutBytes(image, kCmpRva + kCmpLength,
             {0x4C, 0x8B, 0xD0,                          // mov r10, rax
              0x48, 0x8B, 0x8E, 0xE0, 0x69, 0x02, 0x00,  // mov rcx, [rsi+0x269E0]
              0x48, 0x89, 0x45, 0x20,                    // mov [rbp+0x20], rax
              0x44, 0x8B, 0x59, 0x24,                    // mov r11d, [rcx+0x24]
              0x44, 0x0F, 0xB6, 0x49, 0x28});            // movzx r9d, byte [rcx+0x28]

    // jne <off path>
    PutBytes(image, kCmpRva + kBranchDelta, {0x75, kBranchDisplacement});

    // Fall-through: the ON slot load.
    PutBytes(image, kCmpRva + kBranchDelta + 2,
             {0x0F, 0xB6, 0x51, 0x29,                 // movzx edx, byte [rcx+0x29]
              0x44, 0x8B, 0x41, options.onSlotDisp});  // mov r8d, [rcx+0x30]

    // Branch target: the OFF slot load.
    const size_t offPath = kCmpRva + kBranchDelta + 2 + kBranchDisplacement;
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

TEST(NvLodSpreadOverride, FindsShippedDriverEncoding) {
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

// The setting is still found and the data write still applies; only the code
// fallback is withheld, because NOPing would no longer be provably equivalent.
TEST(NvLodSpreadOverride, WithholdsTheFallbackWhenTableSlotsAreNotAdjacent) {
    ImageOptions options;
    options.offSlotDisp = 0x20;
    const std::vector<uint8_t> image = MakeImage(options);

    Site site;
    ASSERT_TRUE(Scan(image, options, site));
    EXPECT_TRUE(site.found);
    EXPECT_FALSE(site.branchFound);
}

TEST(NvLodSpreadOverride, WithholdsTheFallbackWhenFallthroughIsTheOffPath) {
    ImageOptions options;
    options.onSlotDisp = kOffSlotDisp;
    options.offSlotDisp = kOnSlotDisp;
    const std::vector<uint8_t> image = MakeImage(options);

    Site site;
    ASSERT_TRUE(Scan(image, options, site));
    EXPECT_TRUE(site.found);
    EXPECT_FALSE(site.branchFound);
}

TEST(NvLodSpreadOverride, WithholdsTheFallbackWhenNoShortBranchGuardsThePath) {
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
