#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "../hook/wrappers/inline_hook_pristine_image.h"

namespace {

struct SyntheticPe32 {
    static constexpr DWORD kPreferredBase = 0x10000000;
    static constexpr DWORD kTextRva = 0x1000;
    static constexpr DWORD kTextRaw = 0x200;
    static constexpr DWORD kRelocRva = 0x2000;
    static constexpr DWORD kRelocRaw = 0x400;
    static constexpr DWORD kFunctionRva = kTextRva + 0x10;

    SyntheticPe32() : bytes(0x600, 0) {
        auto* dos = At<IMAGE_DOS_HEADER>(0);
        dos->e_magic = IMAGE_DOS_SIGNATURE;
        dos->e_lfanew = 0x80;

        auto* nt = At<IMAGE_NT_HEADERS32>(0x80);
        nt->Signature = IMAGE_NT_SIGNATURE;
        nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
        nt->FileHeader.NumberOfSections = 2;
        nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER32);
        nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
        nt->OptionalHeader.ImageBase = kPreferredBase;
        nt->OptionalHeader.SizeOfHeaders = kTextRaw;
        nt->OptionalHeader.SizeOfImage = 0x3000;
        nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress = kRelocRva;
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size = 12;

        auto* sections = reinterpret_cast<IMAGE_SECTION_HEADER*>(
            bytes.data() + 0x80 + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + sizeof(IMAGE_OPTIONAL_HEADER32));
        sections[0].VirtualAddress = kTextRva;
        sections[0].Misc.VirtualSize = 0x200;
        sections[0].PointerToRawData = kTextRaw;
        sections[0].SizeOfRawData = 0x200;
        sections[1].VirtualAddress = kRelocRva;
        sections[1].Misc.VirtualSize = 0x200;
        sections[1].PointerToRawData = kRelocRaw;
        sections[1].SizeOfRawData = 0x200;

        auto* block = At<IMAGE_BASE_RELOCATION>(kRelocRaw);
        block->VirtualAddress = kTextRva;
        block->SizeOfBlock = 12;
        auto* entries = At<WORD>(kRelocRaw + sizeof(IMAGE_BASE_RELOCATION));
        entries[0] = static_cast<WORD>((IMAGE_REL_BASED_HIGHLOW << 12) | 0x11);
        entries[1] = IMAGE_REL_BASED_ABSOLUTE;
    }

    template <typename T>
    T* At(size_t offset) {
        return reinterpret_cast<T*>(bytes.data() + offset);
    }

    uint8_t* FunctionBytes() { return bytes.data() + kTextRaw + (kFunctionRva - kTextRva); }

    std::vector<uint8_t> bytes;
};

struct SyntheticPe64 {
    static constexpr ULONGLONG kPreferredBase = 0x180000000;
    static constexpr DWORD kTextRva = 0x1000;
    static constexpr DWORD kRelocRva = 0x2000;
    static constexpr DWORD kFunctionRva = kTextRva + 0x20;

    SyntheticPe64() : bytes(0x600, 0) {
        auto* dos = At<IMAGE_DOS_HEADER>(0);
        dos->e_magic = IMAGE_DOS_SIGNATURE;
        dos->e_lfanew = 0x80;
        auto* nt = At<IMAGE_NT_HEADERS64>(0x80);
        nt->Signature = IMAGE_NT_SIGNATURE;
        nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
        nt->FileHeader.NumberOfSections = 2;
        nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
        nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        nt->OptionalHeader.ImageBase = kPreferredBase;
        nt->OptionalHeader.SizeOfHeaders = 0x200;
        nt->OptionalHeader.SizeOfImage = 0x3000;
        nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC] = {kRelocRva, 12};

        auto* sections = reinterpret_cast<IMAGE_SECTION_HEADER*>(
            bytes.data() + 0x80 + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + sizeof(IMAGE_OPTIONAL_HEADER64));
        sections[0].VirtualAddress = kTextRva;
        sections[0].Misc.VirtualSize = 0x200;
        sections[0].PointerToRawData = 0x200;
        sections[0].SizeOfRawData = 0x200;
        sections[1].VirtualAddress = kRelocRva;
        sections[1].Misc.VirtualSize = 0x200;
        sections[1].PointerToRawData = 0x400;
        sections[1].SizeOfRawData = 0x200;

        auto* block = At<IMAGE_BASE_RELOCATION>(0x400);
        block->VirtualAddress = kTextRva;
        block->SizeOfBlock = 12;
        auto* entries = At<WORD>(0x400 + sizeof(IMAGE_BASE_RELOCATION));
        entries[0] = static_cast<WORD>((IMAGE_REL_BASED_DIR64 << 12) | 0x20);
        entries[1] = IMAGE_REL_BASED_ABSOLUTE;
    }

    template <typename T>
    T* At(size_t offset) {
        return reinterpret_cast<T*>(bytes.data() + offset);
    }

    uint8_t* FunctionBytes() { return bytes.data() + 0x220; }

    std::vector<uint8_t> bytes;
};

}  // namespace

TEST(InlineHookPristineImageTest, AppliesHighLowRelocationToGothicDxgiAbsoluteOperand) {
    SyntheticPe32 image;
    const uint8_t rawFunction[] = {0xA1, 0xC0, 0x86, 0x0D, 0x10, 0x33, 0xC5, 0x89};
    std::memcpy(image.FunctionBytes(), rawFunction, sizeof(rawFunction));
    uint8_t relocated[sizeof(rawFunction)] = {};

    const auto result = ce::inline_hook_pristine_image::ReadRelocatedImageBytes(
        image.bytes.data(), image.bytes.size(), 0x64210000, SyntheticPe32::kFunctionRva, relocated,
        sizeof(relocated));

    ASSERT_TRUE(result.Succeeded());
    EXPECT_EQ(result.relocationsApplied, 1u);
    const uint8_t expected[] = {0xA1, 0xC0, 0x86, 0x2E, 0x64, 0x33, 0xC5, 0x89};
    EXPECT_EQ(std::memcmp(relocated, expected, sizeof(expected)), 0);
}

TEST(InlineHookPristineImageTest, AppliesRelocationThatPartiallyOverlapsRequestedSpan) {
    SyntheticPe32 image;
    const uint32_t preferredAddress = 0x100D86C0;
    std::memcpy(image.FunctionBytes() + 1, &preferredAddress, sizeof(preferredAddress));
    uint8_t relocatedTail[2] = {};

    const auto result = ce::inline_hook_pristine_image::ReadRelocatedImageBytes(
        image.bytes.data(), image.bytes.size(), 0x64210000, SyntheticPe32::kFunctionRva + 3, relocatedTail,
        sizeof(relocatedTail));

    ASSERT_TRUE(result.Succeeded());
    EXPECT_EQ(result.relocationsApplied, 1u);
    EXPECT_EQ(relocatedTail[0], 0x2E);
    EXPECT_EQ(relocatedTail[1], 0x64);
}

TEST(InlineHookPristineImageTest, AppliesDir64RelocationForX64Images) {
    SyntheticPe64 image;
    const uint64_t preferredAddress = SyntheticPe64::kPreferredBase + 0x123456;
    std::memcpy(image.FunctionBytes(), &preferredAddress, sizeof(preferredAddress));
    uint64_t relocatedAddress = 0;
    constexpr uintptr_t kLoadedBase = 0x00007FF900000000;

    const auto result = ce::inline_hook_pristine_image::ReadRelocatedImageBytes(
        image.bytes.data(), image.bytes.size(), kLoadedBase, SyntheticPe64::kFunctionRva,
        reinterpret_cast<uint8_t*>(&relocatedAddress), sizeof(relocatedAddress));

    ASSERT_TRUE(result.Succeeded());
    EXPECT_EQ(result.relocationsApplied, 1u);
    EXPECT_EQ(relocatedAddress, kLoadedBase + 0x123456);
}

TEST(InlineHookPristineImageTest, RefusesMovedImageWithoutRelocationDirectory) {
    SyntheticPe32 image;
    auto* nt = image.At<IMAGE_NT_HEADERS32>(0x80);
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC] = {};
    uint8_t output[8] = {};

    const auto result = ce::inline_hook_pristine_image::ReadRelocatedImageBytes(
        image.bytes.data(), image.bytes.size(), 0x64210000, SyntheticPe32::kFunctionRva, output, sizeof(output));

    EXPECT_EQ(result.issue, ce::inline_hook_pristine_image::ImageBytesIssue::MissingRelocations);
}

TEST(InlineHookPristineImageTest, AllowsUnrelocatedImageWithoutRelocationDirectory) {
    SyntheticPe32 image;
    auto* nt = image.At<IMAGE_NT_HEADERS32>(0x80);
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC] = {};
    image.FunctionBytes()[0] = 0xC3;
    uint8_t output = 0;

    const auto result = ce::inline_hook_pristine_image::ReadRelocatedImageBytes(
        image.bytes.data(), image.bytes.size(), SyntheticPe32::kPreferredBase, SyntheticPe32::kFunctionRva, &output,
        sizeof(output));

    ASSERT_TRUE(result.Succeeded());
    EXPECT_EQ(output, 0xC3);
    EXPECT_EQ(result.relocationsApplied, 0u);
}

TEST(InlineHookPristineImageTest, RefusesMalformedRelocationBlock) {
    SyntheticPe32 image;
    image.At<IMAGE_BASE_RELOCATION>(SyntheticPe32::kRelocRaw)->SizeOfBlock = 7;
    uint8_t output[8] = {};

    const auto result = ce::inline_hook_pristine_image::ReadRelocatedImageBytes(
        image.bytes.data(), image.bytes.size(), 0x64210000, SyntheticPe32::kFunctionRva, output, sizeof(output));

    EXPECT_EQ(result.issue, ce::inline_hook_pristine_image::ImageBytesIssue::InvalidRelocationDirectory);
}
