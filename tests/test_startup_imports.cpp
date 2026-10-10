// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "common/platform/startup_imports.h"

#include <gtest/gtest.h>
#include <cstring>
#include <vector>

namespace {

template <typename T> void Put(std::vector<std::byte>& bytes, size_t offset, const T& value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

template <typename Header> std::vector<std::byte> Fixture(WORD magic, WORD machine) {
    std::vector<std::byte> bytes(4096);
    IMAGE_DOS_HEADER dos{};
    dos.e_magic = IMAGE_DOS_SIGNATURE;
    dos.e_lfanew = 128;
    Put(bytes, 0, dos);
    Header header{};
    header.Signature = IMAGE_NT_SIGNATURE;
    header.FileHeader.Machine = machine;
    header.FileHeader.SizeOfOptionalHeader = sizeof(header.OptionalHeader);
    header.OptionalHeader.Magic = magic;
    header.OptionalHeader.SizeOfImage = static_cast<DWORD>(bytes.size());
    header.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    header.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] = {1024, 40};
    Put(bytes, 128, header);
    IMAGE_IMPORT_DESCRIPTOR entry{};
    entry.Name = 2048;
    entry.FirstThunk = 3072;
    entry.OriginalFirstThunk = 3088;
    Put(bytes, 1024, entry);
    constexpr char name[] = "sl.interposer.dll";
    std::memcpy(bytes.data() + 2048, name, sizeof(name));
    return bytes;
}

ce::startup_imports::Status Inspect(const std::vector<std::byte>& bytes, ce::startup_imports::Image& image) {
    return ce::startup_imports::Inspect([&](DWORD rva, std::span<std::byte> output) {
        if (rva > bytes.size() || output.size() > bytes.size() - rva) return false;
        std::memcpy(output.data(), bytes.data() + rva, output.size());
        return true;
    }, image);
}

}  // namespace

TEST(StartupImports, PreservesPe32AndPe64ImportDescriptors) {
    for (const auto& bytes : {Fixture<IMAGE_NT_HEADERS32>(IMAGE_NT_OPTIONAL_HDR32_MAGIC, IMAGE_FILE_MACHINE_I386),
                              Fixture<IMAGE_NT_HEADERS64>(IMAGE_NT_OPTIONAL_HDR64_MAGIC, IMAGE_FILE_MACHINE_AMD64)}) {
        ce::startup_imports::Image image;
        ASSERT_EQ(Inspect(bytes, image), ce::startup_imports::Status::Unchanged);
        ASSERT_EQ(image.imports.size(), 1u);
        EXPECT_EQ(image.imports[0].name, "sl.interposer.dll");
        EXPECT_EQ(image.imports[0].descriptorRva, 1024u);
        EXPECT_EQ(image.imports[0].descriptor.FirstThunk, 3072u);
        EXPECT_EQ(image.imports[0].descriptor.OriginalFirstThunk, 3088u);
    }
}

TEST(StartupImports, RejectsEveryTruncatedHeaderAndImportTable) {
    const auto fixture = Fixture<IMAGE_NT_HEADERS64>(IMAGE_NT_OPTIONAL_HDR64_MAGIC, IMAGE_FILE_MACHINE_AMD64);
    for (size_t size : {size_t(0), size_t(63), size_t(128), size_t(391), size_t(1024), size_t(1063), size_t(2064)}) {
        ce::startup_imports::Image image;
        const std::vector<std::byte> truncated(fixture.begin(), fixture.begin() + size);
        EXPECT_EQ(Inspect(truncated, image), ce::startup_imports::Status::InvalidImage) << size;
    }
}

TEST(StartupImports, RejectsOverflowMissingTerminatorAndOutOfImageName) {
    auto bytes = Fixture<IMAGE_NT_HEADERS64>(IMAGE_NT_OPTIONAL_HDR64_MAGIC, IMAGE_FILE_MACHINE_AMD64);
    for (DWORD name : {4096u, 0xfffffff0u}) {
        Put(bytes, 1024 + offsetof(IMAGE_IMPORT_DESCRIPTOR, Name), name);
        ce::startup_imports::Image image;
        EXPECT_EQ(Inspect(bytes, image), ce::startup_imports::Status::InvalidImage);
    }
    bytes = Fixture<IMAGE_NT_HEADERS64>(IMAGE_NT_OPTIONAL_HDR64_MAGIC, IMAGE_FILE_MACHINE_AMD64);
    std::fill(bytes.begin() + 2048, bytes.end(), std::byte{'x'});
    ce::startup_imports::Image image;
    EXPECT_EQ(Inspect(bytes, image), ce::startup_imports::Status::InvalidImage);
    bytes = Fixture<IMAGE_NT_HEADERS64>(IMAGE_NT_OPTIONAL_HDR64_MAGIC, IMAGE_FILE_MACHINE_AMD64);
    IMAGE_IMPORT_DESCRIPTOR missingName{};
    missingName.OriginalFirstThunk = 3088;
    missingName.FirstThunk = 3072;
    Put(bytes, 1044, missingName);
    EXPECT_EQ(Inspect(bytes, image), ce::startup_imports::Status::InvalidImage);
}

TEST(StartupImports, RejectsRelativeAndEmbeddedNullReplacementWithoutTouchingProcess) {
    for (const auto& path : {std::string("sl.interposer.dll"), std::string("C:sl.interposer.dll"),
                             std::string("C:\\ok\0bad", 9)}) {
        const auto result = ce::startup_imports::Redirect(nullptr, "sl.interposer.dll", path);
        EXPECT_EQ(result.status, ce::startup_imports::Status::Failed);
        EXPECT_EQ(result.error, ERROR_INVALID_PARAMETER);
    }
}
