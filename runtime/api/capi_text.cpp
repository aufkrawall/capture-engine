// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "runtime/api/capi_text.h"

#include "runtime/api/runtime_desc.h"

#include <windows.h>

#include <limits>
#include <utility>

namespace ce::capi {

ce_status_t ReadClientText(const char* text, size_t maxBytes, std::string& out) {
    // Bounded scan: a string longer than the limit is refused without reading past it.
    size_t length = 0;
    while (length <= maxBytes && text[length] != '\0')
        ++length;
    if (length > maxBytes)
        return CE_E_INVALID_ARGUMENT;
    const std::string_view view(text, length);
    if (!IsValidUtf8(view))
        return CE_E_INVALID_ARGUMENT;
    out.assign(view);
    return CE_OK;
}

std::string_view TruncateUtf8(std::string_view text, size_t maxBytes) {
    if (text.size() <= maxBytes)
        return text;
    size_t cut = maxBytes;
    // A continuation byte (10xxxxxx) at the cut means the cut splits a character: back up to its lead byte.
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
        --cut;
    return text.substr(0, cut);
}

std::string SanitizeLogText(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        const auto value = static_cast<unsigned char>(c);
        if (value < 0x20 || value == 0x7F)
            c = ' ';
    }
    return out;
}

bool Utf8ToWideStrict(std::string_view text, std::wstring& out) {
    out.clear();
    if (text.empty())
        return true;
    if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        return false;
    const int needed =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0)
        return false;
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(),
                            needed) != needed)
        return false;
    out = std::move(wide);
    return true;
}

}  // namespace ce::capi
