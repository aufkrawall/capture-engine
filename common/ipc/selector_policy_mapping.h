#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/ipc/shared_defs.h"
#include "common/platform/raii_helpers.h"
#include "common/platform/utf8.h"

namespace ce::selector_policy {

inline constexpr uint32_t kMagic = 0x31535043;  // CPS1
struct Header {
    uint32_t magic;
    uint32_t version;
    uint32_t ownerPid;
    uint32_t generation;
    uint32_t payloadLow;
    uint32_t payloadHigh;
};
static_assert(sizeof(Header) == 24);

inline std::wstring MappingName(uint32_t pid, uint32_t generation) {
    wchar_t name[96]{};
    swprintf(name, std::size(name), CE_SESSION_NAMESPACE L"SelectorPolicy_%u_%08X_%08X", SHARED_MEMORY_VERSION, pid,
             generation);
    return name;
}

// Validate the complete NUL-terminated selector sequence before any reader walks it.
inline bool ValidateList(std::string_view list) {
    if (list.empty() || list.back() != '\0' || (list.size() > 1 && list[list.size() - 2] != '\0'))
        return false;
    size_t cursor = 0;
    while (cursor + 1 < list.size()) {
        const size_t end = list.find('\0', cursor);
        if (end == std::string_view::npos || end == cursor)
            return false;
        cursor = end + 1;
    }
    return true;
}

// Owns exactly one published immutable mapping. A reader that already opened
// an older generation retains its own handle; a reader racing its retirement
// retries only after observing that the generation actually changed.
class Publisher {
public:
    bool Publish(uint32_t ownerPid, const std::vector<std::string>& entries, std::atomic<uint32_t>& generation) {
        if (!ownerPid)
            return false;
        std::string payload;
        for (const std::string& entry : entries) {
            if (entry.empty() || entry.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
                entry.find('\0') != std::string::npos || ce::utf8::ToWide(entry).empty())
                return false;
            payload.append(entry);
            payload.push_back('\0');
        }
        payload.push_back('\0');
        if (payload.size() > std::numeric_limits<size_t>::max() - sizeof(Header))
            return false;
        const uint64_t bytes = sizeof(Header) + static_cast<uint64_t>(payload.size());
        std::lock_guard<std::mutex> lock(mutex_);
        ce::HandleGuard mapping;
        uint32_t candidate = nextGeneration_;
        do {
            if (++candidate == 0)
                ++candidate;
            const std::wstring name = MappingName(ownerPid, candidate);
            HANDLE handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                               static_cast<DWORD>(bytes >> 32), static_cast<DWORD>(bytes), name.c_str());
            const DWORD error = GetLastError();
            if (!handle)
                return false;
            if (error != ERROR_ALREADY_EXISTS) {
                mapping.reset(handle);
                break;
            }
            CloseHandle(handle);
        } while (candidate != nextGeneration_);
        if (!mapping)
            return false;
        ce::MappingGuard view(MapViewOfFile(mapping.get(), FILE_MAP_WRITE, 0, 0, static_cast<SIZE_T>(bytes)));
        if (!view)
            return false;
        const Header header{kMagic, 1, ownerPid, candidate, static_cast<uint32_t>(payload.size()),
                            static_cast<uint32_t>(static_cast<uint64_t>(payload.size()) >> 32)};
        memcpy(view.get(), &header, sizeof(header));
        memcpy(static_cast<uint8_t*>(view.get()) + sizeof(header), payload.data(), payload.size());
        nextGeneration_ = candidate;
        generation.store(candidate, std::memory_order_release);
        current_ = std::move(mapping);
        return true;
    }

    void Reset(std::atomic<uint32_t>& generation) {
        std::lock_guard<std::mutex> lock(mutex_);
        generation.store(0, std::memory_order_release);
        current_.reset();
    }

private:
    std::mutex mutex_;
    ce::HandleGuard current_;
    uint32_t nextGeneration_ = 0;
};

template <typename Visitor>
bool Read(uint32_t ownerPid, const std::atomic<uint32_t>& generation, Visitor&& visitor) {
    for (;;) {
        const uint32_t observed = generation.load(std::memory_order_acquire);
        if (!ownerPid || !observed)
            return false;
        const std::wstring name = MappingName(ownerPid, observed);
        ce::HandleGuard mapping(OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str()));
        if (!mapping) {
            if (generation.load(std::memory_order_acquire) != observed)
                continue;
            return false;
        }
        ce::MappingGuard prefix(MapViewOfFile(mapping.get(), FILE_MAP_READ, 0, 0, sizeof(Header)));
        if (!prefix)
            return false;
        Header header{};
        memcpy(&header, prefix.get(), sizeof(header));
        const uint64_t payloadBytes = static_cast<uint64_t>(header.payloadLow) |
                                      (static_cast<uint64_t>(header.payloadHigh) << 32);
        if (header.magic != kMagic || header.version != 1 || header.ownerPid != ownerPid ||
            header.generation != observed || payloadBytes == 0 ||
            payloadBytes > std::numeric_limits<size_t>::max() - sizeof(Header))
            return false;
        const size_t bytes = sizeof(Header) + static_cast<size_t>(payloadBytes);
        ce::MappingGuard full(MapViewOfFile(mapping.get(), FILE_MAP_READ, 0, 0, bytes));
        if (!full)
            return false;
        const char* payload = static_cast<const char*>(full.get()) + sizeof(Header);
        const std::string_view list(payload, static_cast<size_t>(payloadBytes));
        if (!ValidateList(list))
            return false;
        if (generation.load(std::memory_order_acquire) != observed)
            continue;
        visitor(list);
        return true;
    }
}

}  // namespace ce::selector_policy
