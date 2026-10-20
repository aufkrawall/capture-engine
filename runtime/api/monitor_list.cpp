// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "runtime/api/monitor_list.h"

#include "common/logging/logging.h"

#include <algorithm>
#include <cstring>

namespace ce::capi {
namespace {

// The whole text with its terminator, or nothing when it does not fit: an id is never returned cut.
bool CopyWhole(const std::string& text, char* field, size_t size) {
    if (text.size() + 1 > size)
        return false;
    std::memcpy(field, text.data(), text.size());
    field[text.size()] = '\0';
    return true;
}

// At most size-1 bytes, cut back to a character boundary: a continuation byte (10xxxxxx) never starts a character.
void CopyCut(const std::string& text, char* field, size_t size) {
    size_t length = std::min(text.size(), size - 1);
    if (length < text.size()) {
        while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80)
            --length;
    }
    std::memcpy(field, text.data(), length);
    field[length] = '\0';
}

}  // namespace

ce_status_t EnumerateMonitors(const MonitorListing& listing, ce_monitor_info_t* items, uint32_t itemSize,
                              uint32_t capacity, uint32_t* count) {
    if (itemSize != sizeof(ce_monitor_info_t) || !count || (!items && capacity != 0))
        return CE_E_INVALID_ARGUMENT;
    uint32_t total = 0;
    for (const ce::monitor_selection::MonitorInfo& monitor : listing()) {
        ce_monitor_info_t info{};
        if (!CopyWhole(monitor.id, info.id, sizeof(info.id))) {
            LogWarn("[CApi] ce_enumerate_monitors: a monitor id of %zu bytes does not fit and is skipped",
                    monitor.id.size());
            continue;
        }
        CopyCut(monitor.name, info.name, sizeof(info.name));
        info.left = monitor.desktopRect.left;
        info.top = monitor.desktopRect.top;
        info.right = monitor.desktopRect.right;
        info.bottom = monitor.desktopRect.bottom;
        info.primary = monitor.primary ? 1u : 0u;
        if (total < capacity)
            items[total] = info;
        ++total;
    }
    *count = total;
    return CE_OK;
}

}  // namespace ce::capi
