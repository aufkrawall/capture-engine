// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include <stddef.h>
#include <stdint.h>

#include "cengine_abi_layout.h"

#pragma pack(push, 1)
struct CengineAmbientPackingProbe {
    char prefix;
    uint64_t value;
};
#pragma pack(pop)
static_assert(sizeof(CengineAmbientPackingProbe) == 9);
static_assert(offsetof(CengineAmbientPackingProbe, value) == 1);

#include "include/cengine/cengine.hpp"
#include <gtest/gtest.h>
#include <type_traits>

extern "C" uint32_t CengineDraftC11Version(void);

static_assert(std::is_standard_layout_v<ce_runtime_desc_t>);
static_assert(std::is_trivially_copyable_v<ce_runtime_desc_t>);
static_assert(std::is_standard_layout_v<ce_status_info_t>);
static_assert(std::is_trivially_copyable_v<ce_status_info_t>);
static_assert(std::is_standard_layout_v<ce_event_t>);
static_assert(std::is_trivially_copyable_v<ce_event_t>);
static_assert(std::is_standard_layout_v<ce_settings_diagnostic_t>);
static_assert(std::is_trivially_copyable_v<ce_settings_diagnostic_t>);
static_assert(std::is_standard_layout_v<ce_setup_status_t>);
static_assert(std::is_trivially_copyable_v<ce_setup_status_t>);
static_assert(std::is_standard_layout_v<ce_monitor_info_t>);
static_assert(std::is_trivially_copyable_v<ce_monitor_info_t>);

static_assert(!std::is_copy_constructible_v<cengine::Runtime>);
static_assert(!std::is_copy_assignable_v<cengine::Runtime>);
static_assert(std::is_nothrow_move_constructible_v<cengine::Runtime>);
static_assert(std::is_nothrow_move_assignable_v<cengine::Runtime>);
static_assert(std::is_nothrow_destructible_v<cengine::Runtime>);
static_assert(!std::is_copy_constructible_v<cengine::Event>);
static_assert(!std::is_copy_assignable_v<cengine::Event>);
static_assert(std::is_nothrow_move_constructible_v<cengine::Event>);
static_assert(std::is_nothrow_move_assignable_v<cengine::Event>);
static_assert(std::is_nothrow_destructible_v<cengine::Event>);
static_assert(!std::is_copy_constructible_v<cengine::SettingsEdit>);
static_assert(!std::is_copy_assignable_v<cengine::SettingsEdit>);
static_assert(std::is_nothrow_move_constructible_v<cengine::SettingsEdit>);
static_assert(std::is_nothrow_move_assignable_v<cengine::SettingsEdit>);
static_assert(std::is_nothrow_destructible_v<cengine::SettingsEdit>);

static_assert(std::is_same_v<decltype(&ce_runtime_desc_init), ce_status_t (*)(ce_runtime_desc_t*, uint32_t, uint32_t)>);

static_assert(std::is_same_v<decltype(&cengine::Runtime::create),
                             cengine::Status (*)(const cengine::Options&, cengine::Runtime&)>);
static_assert(std::is_same_v<decltype(std::declval<cengine::Runtime&>().startRecording(cengine::Mode::Video, "")),
                             cengine::Status>);
static_assert(std::is_same_v<decltype(std::declval<cengine::Runtime&>().toggleRecording(cengine::Mode::AudioOnly, "")),
                             cengine::Status>);
static_assert(std::is_same_v<decltype(std::declval<cengine::Runtime&>().stopRecording("")), cengine::Status>);
static_assert(std::is_same_v<decltype(std::declval<cengine::Runtime&>().toggleOverlay()), cengine::Status>);
static_assert(std::is_same_v<decltype(std::declval<cengine::Runtime&>().toggleBenchmark()), cengine::Status>);
static_assert(std::is_same_v<decltype(std::declval<cengine::Runtime&>().takeScreenshot()), cengine::Status>);
static_assert(std::is_same_v<decltype(std::declval<cengine::Runtime&>().launch("")), cengine::Status>);
static_assert(std::is_same_v<decltype(std::declval<cengine::Runtime&>().status()), cengine::StatusInfo>);
static_assert(std::is_same_v<decltype(std::declval<cengine::Runtime&>().eventHandle()), void*>);
static_assert(std::is_same_v<decltype(std::declval<cengine::Event&>().raw()), const ce_event_t*>);

TEST(CengineAbiTest, C11TranslationUnitIsCompiledAndLinkedAsC) {
    EXPECT_EQ(CengineDraftC11Version(), 201112u);
}

TEST(CengineAbiTest, StatusPreservesCAdmissionFailures) {
    const cengine::Status accepted;
    const cengine::Status timeout(CE_E_TIMEOUT);
    EXPECT_TRUE(accepted.ok());
    EXPECT_EQ(accepted.code(), CE_OK);
    EXPECT_FALSE(timeout.ok());
    EXPECT_EQ(timeout.code(), CE_E_TIMEOUT);
}
