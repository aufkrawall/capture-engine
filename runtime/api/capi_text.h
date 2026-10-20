// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

#include "include/cengine/cengine.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace ce::capi {

// Limits of client text, in UTF-8 bytes. A command string is input the engine acts on, so an over-long one is
// refused; log text is only shortened, because a truncated line still tells what happened.
inline constexpr size_t kMaxReasonBytes = 1024;
inline constexpr size_t kMaxCommandLineBytes = 98304;  // 32767 UTF-16 units at 3 UTF-8 bytes each
inline constexpr size_t kMaxLogMessageBytes = 1024;

// Copies a client's NUL-terminated string into `out`. CE_E_INVALID_ARGUMENT when it is longer than `maxBytes` or
// not valid UTF-8. NULL is the caller's to handle: some arguments may be absent, others are required.
ce_status_t ReadClientText(const char* text, size_t maxBytes, std::string& out);

// The longest prefix of `text` that is at most `maxBytes` long and ends on a character boundary.
std::string_view TruncateUtf8(std::string_view text, size_t maxBytes);

// Control characters, tab, CR and LF included, become spaces: one log call stays one line.
std::string SanitizeLogText(std::string_view text);

// Strict UTF-8 to UTF-16. False (and `out` empty) for invalid UTF-8 or a conversion failure.
bool Utf8ToWideStrict(std::string_view text, std::wstring& out);

}  // namespace ce::capi
