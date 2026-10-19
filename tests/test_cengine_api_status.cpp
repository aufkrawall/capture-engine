// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "include/cengine/cengine.hpp"
#include "common/platform/build_identity.h"
#include "runtime/api/runtime_desc.h"

#include <gtest/gtest.h>

#include <climits>
#include <cstring>
#include <set>
#include <string>

// The entry points that work without a runtime (llm-wiki/library/03 "Versioning", "Error model"),
// called through the C ABI exactly as a client would.
namespace {

struct NamedStatus {
    ce_status_t code;
    const char* name;
};

const NamedStatus kAllStatuses[] = {
    {CE_OK, "CE_OK"},
    {CE_E_INVALID_ARGUMENT, "CE_E_INVALID_ARGUMENT"},
    {CE_E_INVALID_STATE, "CE_E_INVALID_STATE"},
    {CE_E_BUSY, "CE_E_BUSY"},
    {CE_E_VERSION, "CE_E_VERSION"},
    {CE_E_PACKAGE, "CE_E_PACKAGE"},
    {CE_E_UNSUPPORTED, "CE_E_UNSUPPORTED"},
    {CE_E_TIMEOUT, "CE_E_TIMEOUT"},
    {CE_E_SETTINGS, "CE_E_SETTINGS"},
    {CE_E_CONFLICT, "CE_E_CONFLICT"},
    {CE_E_IPC, "CE_E_IPC"},
    {CE_E_PROCESS, "CE_E_PROCESS"},
    {CE_E_IO, "CE_E_IO"},
    {CE_E_NO_MEMORY, "CE_E_NO_MEMORY"},
    {CE_E_BUFFER_TOO_SMALL, "CE_E_BUFFER_TOO_SMALL"},
    {CE_E_NOT_FOUND, "CE_E_NOT_FOUND"},
    {CE_E_INTERNAL, "CE_E_INTERNAL"},
};

TEST(CengineApiStatusTest, ApiVersionIsTheHeadersVersion) {
    EXPECT_EQ(ce_api_version(), static_cast<uint32_t>(CE_API_VERSION));
    EXPECT_EQ(ce_api_version() >> 16, CE_API_VERSION_MAJOR);
    EXPECT_EQ(ce_api_version() & 0xFFFFu, CE_API_VERSION_MINOR);
    EXPECT_EQ(ce_api_version() >> 16, 2u) << "v2 is the first library API";
}

TEST(CengineApiStatusTest, VersionStringIsTheProductBuildAndStable) {
    const char* version = ce_version_string();
    ASSERT_NE(version, nullptr);
    EXPECT_GT(std::strlen(version), 0u);
    EXPECT_STREQ(version, GetCaptureVersion());
    EXPECT_EQ(version, ce_version_string()) << "static storage: the same pointer on every call";
}

TEST(CengineApiStatusTest, EveryStatusCodeHasADistinctNonEmptyUtf8Text) {
    std::set<std::string> texts;
    const char* unknown = ce_status_string(INT32_MIN);
    ASSERT_NE(unknown, nullptr);
    for (const NamedStatus& status : kAllStatuses) {
        SCOPED_TRACE(status.name);
        const char* text = ce_status_string(status.code);
        ASSERT_NE(text, nullptr);
        EXPECT_GT(std::strlen(text), 0u);
        EXPECT_STRNE(text, unknown) << "a defined code must not fall through to the unknown text";
        EXPECT_TRUE(ce::capi::IsValidUtf8(text));
        EXPECT_EQ(text, ce_status_string(status.code)) << "static storage";
        EXPECT_TRUE(texts.insert(text).second) << "two codes share the text: " << text;
    }
    EXPECT_EQ(texts.size(), std::size(kAllStatuses));
}

TEST(CengineApiStatusTest, CodesMatchThePublishedValues) {
    // The numbers are ABI (03 "Error model"): clients compare them, never the strings.
    EXPECT_EQ(CE_OK, 0);
    EXPECT_EQ(CE_E_INVALID_ARGUMENT, -1);
    EXPECT_EQ(CE_E_INVALID_STATE, -2);
    EXPECT_EQ(CE_E_BUSY, -3);
    EXPECT_EQ(CE_E_VERSION, -4);
    EXPECT_EQ(CE_E_PACKAGE, -5);
    EXPECT_EQ(CE_E_UNSUPPORTED, -6);
    EXPECT_EQ(CE_E_TIMEOUT, -7);
    EXPECT_EQ(CE_E_SETTINGS, -8);
    EXPECT_EQ(CE_E_CONFLICT, -9);
    EXPECT_EQ(CE_E_IPC, -10);
    EXPECT_EQ(CE_E_PROCESS, -11);
    EXPECT_EQ(CE_E_IO, -12);
    EXPECT_EQ(CE_E_NO_MEMORY, -13);
    EXPECT_EQ(CE_E_BUFFER_TOO_SMALL, -14);
    EXPECT_EQ(CE_E_NOT_FOUND, -15);
    EXPECT_EQ(CE_E_INTERNAL, -100);
}

TEST(CengineApiStatusTest, UnknownCodesGetOneStableText) {
    const char* unknown = ce_status_string(INT32_MIN);
    ASSERT_NE(unknown, nullptr);
    for (ce_status_t code : {1, 2, 100, -16, -17, -99, -101, INT32_MAX, INT32_MIN + 1}) {
        SCOPED_TRACE(code);
        EXPECT_EQ(ce_status_string(code), unknown);
    }
    // Every code in the published band that is not defined is "unknown", so a new code added to the
    // header without a text is caught by the first test and by this sweep.
    size_t defined = 0;
    for (ce_status_t code = 1; code >= -120; --code) {
        if (ce_status_string(code) != unknown)
            ++defined;
    }
    EXPECT_EQ(defined, std::size(kAllStatuses));
}

TEST(CengineApiStatusTest, WrapperStatusMessageComesFromTheSameTable) {
    EXPECT_STREQ(cengine::Status(CE_E_TIMEOUT).message(), ce_status_string(CE_E_TIMEOUT));
    EXPECT_STREQ(cengine::Status().message(), ce_status_string(CE_OK));
}

}  // namespace
