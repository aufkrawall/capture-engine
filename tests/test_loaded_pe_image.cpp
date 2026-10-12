#include <gtest/gtest.h>
#include <windows.h>
#include <cstring>
#include "common/platform/loaded_pe_image.h"
#include "common/platform/raii_helpers.h"
#include "hook/ngx/ngx_drs_override.h"

namespace {
class LoadedPeImageTest : public testing::Test {
protected:
    void SetUp() override {
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        ASSERT_NE(base, nullptr);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        dos->e_magic = IMAGE_DOS_SIGNATURE;
        dos->e_lfanew = 256;
        nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + 256);
        nt->Signature = IMAGE_NT_SIGNATURE;
        nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER);
        nt->FileHeader.NumberOfSections = 1;
        nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR_MAGIC;
        nt->OptionalHeader.SizeOfImage = 4096;
        nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    }
    void TearDown() override {
        ce::ngx_drs::RestoreRetargetedPointers();
        if (base) EXPECT_TRUE(VirtualFree(base, 0, MEM_RELEASE));
    }
    uint8_t* base = nullptr;
    IMAGE_NT_HEADERS* nt = nullptr;
};
}

TEST_F(LoadedPeImageTest, HeaderOffsetsSizesAndTablesMustStayInsideTheActualAllocation) {
    ASSERT_TRUE(ce::pe::LoadedImage::Open(base));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    for (LONG offset : {LONG{-1}, LONG{0x7fffffff}, LONG{8192}}) {
        dos->e_lfanew = offset;
        EXPECT_FALSE(ce::pe::LoadedImage::Open(base));
    }
    dos->e_lfanew = 256;
    nt->OptionalHeader.SizeOfImage = 16384;
    EXPECT_FALSE(ce::pe::LoadedImage::Open(base));
    nt->OptionalHeader.SizeOfImage = 4096;
    nt->FileHeader.NumberOfSections = 65535;
    EXPECT_FALSE(ce::pe::LoadedImage::Open(base));
    nt->FileHeader.NumberOfSections = 1;
    const auto image = ce::pe::LoadedImage::Open(base);
    ASSERT_TRUE(image);
    EXPECT_EQ(image->Read<DWORD>(4094), nullptr);
    EXPECT_EQ(image->Read<DWORD>(0, SIZE_MAX), nullptr);
    EXPECT_FALSE(image->Contains(SIZE_MAX, 8));
}

TEST_F(LoadedPeImageTest, ForgedWritableSectionCannotRetargetBytesBeyondTheImageBoundary) {
    auto* section = IMAGE_FIRST_SECTION(nt);
    section->Characteristics = IMAGE_SCN_MEM_WRITE;
    section->VirtualAddress = 4096 + 16;
    section->Misc.VirtualSize = 64;
    void* from = reinterpret_cast<void*>(uintptr_t{0x12340000});
    void* to = reinterpret_cast<void*>(uintptr_t{0x56780000});
    auto** outsideImage = reinterpret_cast<void**>(base + section->VirtualAddress);
    *outsideImage = from;
    EXPECT_EQ(ce::ngx_drs::RetargetCachedPointers(base, from, to), 0u);
    EXPECT_EQ(*outsideImage, from);
    section->VirtualAddress = UINT32_MAX - 16;
    EXPECT_EQ(ce::ngx_drs::RetargetCachedPointers(base, from, to), 0u);
}

TEST(LoadedPeImageNativeTest, LoadedSystemImagesRemainReadable) {
    const auto image = ce::pe::LoadedImage::Open(GetModuleHandleW(L"kernel32.dll"));
    ASSERT_TRUE(image);
    EXPECT_GT(image->size, sizeof(IMAGE_NT_HEADERS));
}
