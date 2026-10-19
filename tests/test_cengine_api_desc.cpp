// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "include/cengine/cengine.hpp"
#include "runtime/api/runtime_desc.h"

#include <gtest/gtest.h>

#include <climits>
#include <cstring>
#include <string>
#include <vector>

// ce_runtime_desc_init and the checks ce_runtime_create runs first (llm-wiki/library/03 "Versioning",
// 08 DR-18): the initializer works inside the extent the caller declared, and the validator accepts only
// what a runtime of this API version can serve.
namespace {

using ce::capi::RuntimeDescCopy;
using ce::capi::ValidateRuntimeDesc;

constexpr unsigned char kCanary = 0xCD;
constexpr uint32_t kMajor2Minor1 = (2u << 16) | 1u;
constexpr uint32_t kMajor3 = 3u << 16;
constexpr uint32_t kMajor1 = 1u << 16;

// A descriptor surrounded by canary bytes, so any write outside [offset, offset + struct_size) shows.
struct Arena {
    static constexpr size_t kGuard = 32;
    std::vector<unsigned char> bytes;
    explicit Arena(size_t extent) : bytes(kGuard + extent + kGuard, kCanary) {}
    ce_runtime_desc_t* desc() {
        return reinterpret_cast<ce_runtime_desc_t*>(bytes.data() + kGuard);
    }
    bool GuardsIntact(size_t extent) const {
        for (size_t i = 0; i < kGuard; ++i) {
            if (bytes[i] != kCanary || bytes[kGuard + extent + i] != kCanary)
                return false;
        }
        return true;
    }
    bool ExtentUntouched(size_t extent) const {
        for (size_t i = 0; i < extent; ++i) {
            if (bytes[kGuard + i] != kCanary)
                return false;
        }
        return true;
    }
};

ce_runtime_desc_t ValidDesc() {
    ce_runtime_desc_t desc;
    EXPECT_EQ(ce_runtime_desc_init(&desc, sizeof(desc), CE_API_VERSION), CE_OK);
    return desc;
}

// ---- ce_runtime_desc_init ---------------------------------------------------------------------

TEST(CengineApiDescTest, InitFillsTheDeclaredExtentWithDefaults) {
    Arena arena(sizeof(ce_runtime_desc_t));
    ASSERT_EQ(ce_runtime_desc_init(arena.desc(), sizeof(ce_runtime_desc_t), CE_API_VERSION), CE_OK);
    const ce_runtime_desc_t& desc = *arena.desc();
    EXPECT_EQ(desc.struct_size, sizeof(ce_runtime_desc_t));
    EXPECT_EQ(desc.api_version, static_cast<uint32_t>(CE_API_VERSION));
    EXPECT_EQ(desc.package_dir, nullptr);
    EXPECT_EQ(desc.data_dir, nullptr);
    EXPECT_EQ(desc.client_name, nullptr);
    EXPECT_EQ(desc.features, 0u);
    EXPECT_EQ(desc.reserved, 0u);
    EXPECT_TRUE(arena.GuardsIntact(sizeof(ce_runtime_desc_t)));
}

TEST(CengineApiDescTest, InitNeverWritesBeyondALargerDeclaredExtentEither) {
    // A newer client's descriptor is larger than this runtime's: the tail belongs to the client.
    constexpr size_t kExtent = sizeof(ce_runtime_desc_t) + 24;
    Arena arena(kExtent);
    ASSERT_EQ(ce_runtime_desc_init(arena.desc(), kExtent, CE_API_VERSION), CE_OK);
    EXPECT_EQ(arena.desc()->struct_size, kExtent);
    EXPECT_TRUE(arena.GuardsIntact(kExtent));
    for (size_t i = sizeof(ce_runtime_desc_t); i < kExtent; ++i)
        EXPECT_EQ(arena.bytes[Arena::kGuard + i], 0) << "the declared extent is zeroed, byte " << i;
}

TEST(CengineApiDescTest, InitRefusesAnExtentSmallerThanTheApi20Descriptor) {
    for (uint32_t size : {0u, 4u, 8u, 24u, static_cast<uint32_t>(sizeof(ce_runtime_desc_t)) - 1}) {
        SCOPED_TRACE(size);
        Arena arena(sizeof(ce_runtime_desc_t));
        EXPECT_EQ(ce_runtime_desc_init(arena.desc(), size, CE_API_VERSION), CE_E_INVALID_ARGUMENT);
        EXPECT_TRUE(arena.GuardsIntact(sizeof(ce_runtime_desc_t)));
        EXPECT_TRUE(arena.ExtentUntouched(sizeof(ce_runtime_desc_t))) << "nothing is written on refusal";
    }
}

TEST(CengineApiDescTest, InitRejectsNull) {
    EXPECT_EQ(ce_runtime_desc_init(nullptr, sizeof(ce_runtime_desc_t), CE_API_VERSION), CE_E_INVALID_ARGUMENT);
}

TEST(CengineApiDescTest, InitAppliesTheVersionPolicy) {
    struct Case {
        uint32_t version;
        ce_status_t expected;
        const char* why;
    };
    const Case cases[] = {
        {CE_API_VERSION, CE_OK, "the version this header declares"},
        {kMajor3, CE_E_VERSION, "a newer major"},
        {kMajor1, CE_E_VERSION, "an older major"},
        {0, CE_E_VERSION, "no version"},
        {kMajor2Minor1, CE_E_VERSION, "a client minor newer than the runtime's needs features it lacks"},
        {(2u << 16) | 0xFFFFu, CE_E_VERSION, "the largest minor"},
    };
    for (const Case& test : cases) {
        SCOPED_TRACE(test.why);
        Arena arena(sizeof(ce_runtime_desc_t));
        EXPECT_EQ(ce_runtime_desc_init(arena.desc(), sizeof(ce_runtime_desc_t), test.version), test.expected);
        // Even a refused version leaves a defined descriptor behind: the extent is initialized with the
        // requested version, so create() refuses it with the same status instead of reading garbage.
        EXPECT_EQ(arena.desc()->struct_size, sizeof(ce_runtime_desc_t));
        EXPECT_EQ(arena.desc()->api_version, test.version);
        EXPECT_EQ(arena.desc()->reserved, 0u);
        EXPECT_TRUE(arena.GuardsIntact(sizeof(ce_runtime_desc_t)));
        EXPECT_EQ(ValidateRuntimeDesc(arena.desc(), nullptr), test.expected);
    }
}

TEST(CengineApiDescTest, VersionPolicyAcceptsOlderMinorsOfTheSameMajor) {
    using ce::capi::CheckApiVersion;
    const uint32_t runtime = (2u << 16) | 3u;  // a future runtime of API 2.3
    for (uint32_t minor = 0; minor <= 3; ++minor)
        EXPECT_EQ(CheckApiVersion((2u << 16) | minor, runtime), CE_OK) << "client minor " << minor;
    EXPECT_EQ(CheckApiVersion((2u << 16) | 4u, runtime), CE_E_VERSION);
    EXPECT_EQ(CheckApiVersion((3u << 16) | 0u, runtime), CE_E_VERSION);
    EXPECT_EQ(CheckApiVersion((1u << 16) | 3u, runtime), CE_E_VERSION);
}

// ---- ValidateRuntimeDesc ----------------------------------------------------------------------

TEST(CengineApiDescTest, ValidateAcceptsAnInitializedDescriptorAndCopiesTheStrings) {
    std::string packageDir = "C:\\Program Files\\CaptureEngine";
    std::string dataDir = "D:\\logs";
    std::string client = "unit-test";
    ce_runtime_desc_t desc = ValidDesc();
    desc.package_dir = packageDir.c_str();
    desc.data_dir = dataDir.c_str();
    desc.client_name = client.c_str();
    desc.features = CE_FEATURE_GLOBAL_HOTKEYS | CE_FEATURE_WATCH_SETTINGS;

    RuntimeDescCopy copy;
    const char* reason = "untouched";
    ASSERT_EQ(ValidateRuntimeDesc(&desc, &copy, &reason), CE_OK);
    EXPECT_EQ(reason, nullptr);
    packageDir.assign(packageDir.size(), 'X');  // the copy must not alias client memory
    dataDir.assign(dataDir.size(), 'X');
    client.assign(client.size(), 'X');
    EXPECT_EQ(copy.apiVersion, static_cast<uint32_t>(CE_API_VERSION));
    EXPECT_EQ(copy.features, desc.features);
    ASSERT_TRUE(copy.packageDir.has_value());
    EXPECT_EQ(*copy.packageDir, "C:\\Program Files\\CaptureEngine");
    ASSERT_TRUE(copy.dataDir.has_value());
    EXPECT_EQ(*copy.dataDir, "D:\\logs");
    EXPECT_EQ(copy.clientName, "unit-test");
}

TEST(CengineApiDescTest, ValidateTreatsNullStringsAsDefaults) {
    ce_runtime_desc_t desc = ValidDesc();
    RuntimeDescCopy copy;
    ASSERT_EQ(ValidateRuntimeDesc(&desc, &copy), CE_OK);
    EXPECT_FALSE(copy.packageDir.has_value());
    EXPECT_FALSE(copy.dataDir.has_value());
    EXPECT_TRUE(copy.clientName.empty());
    EXPECT_EQ(ValidateRuntimeDesc(&desc, nullptr), CE_OK) << "a pure check needs no output";
}

TEST(CengineApiDescTest, ValidateRejectsNullAndShortExtents) {
    const char* reason = nullptr;
    EXPECT_EQ(ValidateRuntimeDesc(nullptr, nullptr, &reason), CE_E_INVALID_ARGUMENT);
    EXPECT_NE(reason, nullptr);

    ce_runtime_desc_t desc = ValidDesc();
    for (uint32_t size : {0u, 4u, 8u, static_cast<uint32_t>(sizeof(ce_runtime_desc_t)) - 1}) {
        SCOPED_TRACE(size);
        desc.struct_size = size;
        EXPECT_EQ(ValidateRuntimeDesc(&desc, nullptr), CE_E_INVALID_ARGUMENT);
    }
    desc.struct_size = sizeof(desc) + 64;  // a larger (newer) declared extent is acceptable
    EXPECT_EQ(ValidateRuntimeDesc(&desc, nullptr), CE_OK);
}

TEST(CengineApiDescTest, ValidateRejectsVersionsReservedAndUnknownFeatureBits) {
    ce_runtime_desc_t desc = ValidDesc();
    desc.api_version = kMajor3;
    EXPECT_EQ(ValidateRuntimeDesc(&desc, nullptr), CE_E_VERSION);
    desc.api_version = kMajor1;
    EXPECT_EQ(ValidateRuntimeDesc(&desc, nullptr), CE_E_VERSION);
    desc.api_version = kMajor2Minor1;
    EXPECT_EQ(ValidateRuntimeDesc(&desc, nullptr), CE_E_VERSION);

    desc = ValidDesc();
    desc.reserved = 1;
    EXPECT_EQ(ValidateRuntimeDesc(&desc, nullptr), CE_E_INVALID_ARGUMENT);

    desc = ValidDesc();
    desc.features = ce::capi::kKnownFeatures;
    EXPECT_EQ(ValidateRuntimeDesc(&desc, nullptr), CE_OK);
    desc.features = 1u << 4;
    EXPECT_EQ(ValidateRuntimeDesc(&desc, nullptr), CE_E_INVALID_ARGUMENT);
    desc.features = 0x80000000u | CE_FEATURE_CRASH_HANDLER;
    EXPECT_EQ(ValidateRuntimeDesc(&desc, nullptr), CE_E_INVALID_ARGUMENT);
}

TEST(CengineApiDescTest, ValidateChecksTheStrings) {
    struct Case {
        const char* text;
        ce_status_t expected;
        const char* why;
    };
    const Case cases[] = {
        {"C:\\plain\\ascii", CE_OK, "ASCII"},
        {"C:\\Spiele\\\xC3\xA4\xC3\xB6\xC3\xBC", CE_OK, "two-byte characters"},
        {"\xE2\x82\xAC", CE_OK, "a three-byte character"},
        {"\xF0\x9F\x8E\xAE", CE_OK, "a four-byte character (outside the BMP)"},
        {"\xF4\x8F\xBF\xBF", CE_OK, "the last code point, U+10FFFF"},
        {"\xED\x9F\xBF", CE_OK, "the last code point before the surrogates"},
        {"", CE_E_INVALID_ARGUMENT, "empty"},
        {"\x80", CE_E_INVALID_ARGUMENT, "a lone continuation byte"},
        {"abc\xC3", CE_E_INVALID_ARGUMENT, "a truncated two-byte sequence"},
        {"abc\xE2\x82", CE_E_INVALID_ARGUMENT, "a truncated three-byte sequence"},
        {"abc\xF0\x9F\x8E", CE_E_INVALID_ARGUMENT, "a truncated four-byte sequence"},
        {"\xC0\x80", CE_E_INVALID_ARGUMENT, "an overlong NUL"},
        {"\xC1\xBF", CE_E_INVALID_ARGUMENT, "an overlong two-byte form"},
        {"\xE0\x80\x80", CE_E_INVALID_ARGUMENT, "an overlong three-byte form"},
        {"\xF0\x80\x80\x80", CE_E_INVALID_ARGUMENT, "an overlong four-byte form"},
        {"\xED\xA0\x80", CE_E_INVALID_ARGUMENT, "a UTF-16 surrogate"},
        {"\xF4\x90\x80\x80", CE_E_INVALID_ARGUMENT, "above U+10FFFF"},
        {"\xF5\x80\x80\x80", CE_E_INVALID_ARGUMENT, "an impossible lead byte"},
        {"\xFF", CE_E_INVALID_ARGUMENT, "0xFF"},
        {"\xC3\x28", CE_E_INVALID_ARGUMENT, "a lead byte followed by a non-continuation"},
    };
    for (const Case& test : cases) {
        SCOPED_TRACE(test.why);
        for (const char field : {'p', 'd', 'c'}) {
            ce_runtime_desc_t desc = ValidDesc();
            (field == 'p' ? desc.package_dir : field == 'd' ? desc.data_dir : desc.client_name) = test.text;
            ce_status_t expected = test.expected;
            if (field == 'c' && test.text[0] == '\0')
                expected = CE_OK;  // an empty client name means unnamed
            EXPECT_EQ(ValidateRuntimeDesc(&desc, nullptr), expected) << "field " << field;
        }
    }
}

TEST(CengineApiDescTest, ValidateBoundsStringLengths) {
    const std::string longestPath(ce::capi::kMaxPathBytes, 'p');
    const std::string tooLongPath(ce::capi::kMaxPathBytes + 1, 'p');
    const std::string longestName(ce::capi::kMaxClientNameBytes, 'n');
    const std::string tooLongName(ce::capi::kMaxClientNameBytes + 1, 'n');
    ce_runtime_desc_t desc = ValidDesc();
    desc.package_dir = longestPath.c_str();
    desc.data_dir = longestPath.c_str();
    desc.client_name = longestName.c_str();
    EXPECT_EQ(ValidateRuntimeDesc(&desc, nullptr), CE_OK);
    desc.package_dir = tooLongPath.c_str();
    EXPECT_EQ(ValidateRuntimeDesc(&desc, nullptr), CE_E_INVALID_ARGUMENT);
    desc.package_dir = longestPath.c_str();
    desc.data_dir = tooLongPath.c_str();
    EXPECT_EQ(ValidateRuntimeDesc(&desc, nullptr), CE_E_INVALID_ARGUMENT);
    desc.data_dir = longestPath.c_str();
    desc.client_name = tooLongName.c_str();
    EXPECT_EQ(ValidateRuntimeDesc(&desc, nullptr), CE_E_INVALID_ARGUMENT);
}

TEST(CengineApiDescTest, ValidateReportsAReasonForEveryRefusal) {
    std::vector<ce_runtime_desc_t> bad;
    ce_runtime_desc_t desc = ValidDesc();
    desc.struct_size = 8;
    bad.push_back(desc);
    desc = ValidDesc();
    desc.api_version = kMajor3;
    bad.push_back(desc);
    desc = ValidDesc();
    desc.reserved = 5;
    bad.push_back(desc);
    desc = ValidDesc();
    desc.features = 0x100;
    bad.push_back(desc);
    desc = ValidDesc();
    desc.package_dir = "\xFF";
    bad.push_back(desc);
    desc = ValidDesc();
    desc.data_dir = "";
    bad.push_back(desc);
    for (const ce_runtime_desc_t& entry : bad) {
        const char* reason = nullptr;
        EXPECT_NE(ValidateRuntimeDesc(&entry, nullptr, &reason), CE_OK);
        ASSERT_NE(reason, nullptr);
        EXPECT_GT(std::strlen(reason), 0u);
    }
}

TEST(CengineApiDescTest, Utf8ValidatorAgreesWithTheReferenceRanges) {
    using ce::capi::IsValidUtf8;
    EXPECT_TRUE(IsValidUtf8(""));
    EXPECT_TRUE(IsValidUtf8("plain"));
    // Every scalar value round-trips through the encoder below, and its encoding is accepted; the
    // surrogate range and everything past U+10FFFF have no encoding the validator may accept.
    auto encode = [](uint32_t cp) {
        std::string out;
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        return out;
    };
    for (uint32_t cp = 0; cp <= 0x10FFFF; cp += (cp < 0x2000 || (cp >= 0xD700 && cp <= 0xE100)) ? 1 : 0x61) {
        const bool surrogate = cp >= 0xD800 && cp <= 0xDFFF;
        EXPECT_EQ(IsValidUtf8(encode(cp)), !surrogate) << "U+" << std::hex << cp;
    }
    for (uint32_t cp : {0x7Fu, 0x80u, 0x7FFu, 0x800u, 0xFFFFu, 0x10000u, 0x10FFFFu})
        EXPECT_TRUE(IsValidUtf8(encode(cp))) << "U+" << std::hex << cp;
    EXPECT_FALSE(IsValidUtf8(encode(0x110000)));
}

}  // namespace
