#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// One active display path as the OS reports it: the VidPn source the graphics
// kernel names in its flip and blank events, and the mode's refresh rate. Under
// variable refresh the mode rate is the ceiling, which is what the refresh
// bound needs; measured blank gaps are not, because below the ceiling they
// follow the game's frame rate.
struct DisplayPathRefresh {
    uint64_t adapterLuid = 0;
    uint32_t sourceId = 0;
    uint32_t refreshNumerator = 0;
    uint32_t refreshDenominator = 0;
};

// Minimum refresh period per VidPn source id. The kernel events carry only the
// source id, not the adapter, so an id that two adapters use at different rates
// has no period at all rather than a guessed one.
class DisplayRefreshPeriods {
public:
    static constexpr std::size_t kMaxSources = 16;

    static DisplayRefreshPeriods FromPaths(const std::vector<DisplayPathRefresh>& paths) {
        DisplayRefreshPeriods result;
        for (const auto& path : paths) {
            if (path.refreshNumerator == 0 || path.refreshDenominator == 0)
                continue;
            const int64_t periodUs =
                (static_cast<int64_t>(path.refreshDenominator) * 1'000'000 + path.refreshNumerator / 2) /
                path.refreshNumerator;
            if (periodUs <= 0)
                continue;
            Entry* entry = result.FindOrCreate(path.sourceId);
            if (!entry)
                continue;
            if (entry->periodUs == 0 && !entry->ambiguous) {
                entry->periodUs = periodUs;
            } else if (entry->periodUs != periodUs) {
                entry->periodUs = 0;
                entry->ambiguous = true;
            }
        }
        return result;
    }

    // 0 when the source is unknown or ambiguous.
    int64_t PeriodUs(uint32_t sourceId) const {
        for (std::size_t i = 0; i < count_; ++i) {
            if (entries_[i].sourceId == sourceId)
                return entries_[i].periodUs;
        }
        return 0;
    }

    bool operator==(const DisplayRefreshPeriods& other) const {
        if (count_ != other.count_)
            return false;
        for (std::size_t i = 0; i < count_; ++i) {
            if (entries_[i].sourceId != other.entries_[i].sourceId ||
                entries_[i].periodUs != other.entries_[i].periodUs ||
                entries_[i].ambiguous != other.entries_[i].ambiguous)
                return false;
        }
        return true;
    }
    bool operator!=(const DisplayRefreshPeriods& other) const { return !(*this == other); }

    std::size_t count() const { return count_; }
    uint32_t SourceAt(std::size_t index) const { return entries_[index].sourceId; }
    int64_t PeriodAt(std::size_t index) const { return entries_[index].periodUs; }

private:
    struct Entry {
        uint32_t sourceId = 0;
        int64_t periodUs = 0;
        bool ambiguous = false;
    };

    Entry* FindOrCreate(uint32_t sourceId) {
        for (std::size_t i = 0; i < count_; ++i) {
            if (entries_[i].sourceId == sourceId)
                return &entries_[i];
        }
        if (count_ >= kMaxSources)
            return nullptr;
        entries_[count_].sourceId = sourceId;
        return &entries_[count_++];
    }

    std::array<Entry, kMaxSources> entries_ = {};
    std::size_t count_ = 0;
};

// Queries the active display paths. Returns an empty table when the OS query
// fails, which disables the refresh bound rather than guessing.
DisplayRefreshPeriods QueryDisplayRefreshPeriods();

// One info line naming every source and its period.
void LogDisplayRefreshPeriods(const DisplayRefreshPeriods& periods);
