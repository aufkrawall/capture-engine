#pragma once

#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "common/config/application_profile.h"
#include "mediaengine/audio/process_loopback_selector_validation.h"

namespace ce::process_loopback {

// A length-delimited immutable policy, carried in an inherited mapping. The
// worker never relies on the process command-line length for profile counts.
inline constexpr uint32_t kSelectorPolicyMagic = 0x31504353;  // SCP1
// A Windows selector has fewer than 32767 UTF-16 code units. Four bytes per
// unit is a conservative UTF-8 bound and keeps conversion lengths in int.
inline constexpr size_t kMaximumSelectorUtf8Bytes = 4u * 32766u;

inline void AppendPolicyUint32(std::vector<uint8_t>& data, uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8)
        data.push_back(static_cast<uint8_t>(value >> shift));
}

inline bool ReadPolicyUint32(std::span<const uint8_t> data, size_t& offset, uint32_t& value) {
    if (offset > data.size() || data.size() - offset < 4)
        return false;
    value = 0;
    for (unsigned shift = 0; shift != 32; shift += 8)
        value |= static_cast<uint32_t>(data[offset++]) << shift;
    return true;
}

inline bool EncodeSelectorPolicy(const std::vector<WhitelistEntry>& entries, std::vector<uint8_t>& output) {
    output.clear();
    if (entries.size() > std::numeric_limits<uint32_t>::max())
        return false;
    AppendPolicyUint32(output, kSelectorPolicyMagic);
    AppendPolicyUint32(output, static_cast<uint32_t>(entries.size()));
    for (const WhitelistEntry& entry : entries) {
        if (entry.pattern.size() > kMaximumSelectorUtf8Bytes) {
            output.clear();
            return false;
        }
        const std::wstring wide = ce::utf8::ToWide(entry.pattern);
        if (!IsValidWorkerProcessSelector(wide)) {
            output.clear();
            return false;
        }
        AppendPolicyUint32(output, static_cast<uint32_t>(entry.pattern.size()));
        output.push_back(entry.mode == MatchMode::kExact ? 0 : 1);
        output.insert(output.end(), entry.pattern.begin(), entry.pattern.end());
    }
    return true;
}

inline bool DecodeSelectorPolicy(std::span<const uint8_t> data, std::vector<WhitelistEntry>& output) {
    output.clear();
    size_t offset = 0;
    uint32_t magic = 0;
    uint32_t count = 0;
    if (!ReadPolicyUint32(data, offset, magic) || magic != kSelectorPolicyMagic ||
        !ReadPolicyUint32(data, offset, count) || count > (data.size() - offset) / 6)
        return false;
    std::vector<WhitelistEntry> decoded;
    decoded.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        uint32_t length = 0;
        if (!ReadPolicyUint32(data, offset, length) || length == 0 || offset >= data.size())
            return false;
        const uint8_t mode = data[offset++];
        if (mode > 1 || length > kMaximumSelectorUtf8Bytes || length > data.size() - offset)
            return false;
        WhitelistEntry entry;
        entry.pattern.assign(reinterpret_cast<const char*>(data.data() + offset), length);
        entry.mode = mode == 0 ? MatchMode::kExact : MatchMode::kTitleExecutable;
        if (!IsValidWorkerProcessSelector(ce::utf8::ToWide(entry.pattern)))
            return false;
        decoded.push_back(std::move(entry));
        offset += length;
    }
    if (offset != data.size())
        return false;
    output = std::move(decoded);
    return true;
}

}  // namespace ce::process_loopback
