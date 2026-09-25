#include "display_timing_refresh.h"

#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <vector>

#include "../common/logging.h"

namespace {

// The path's own refresh rate is the mode it is driven at. Some drivers leave
// it zero on an active path; the target mode's vertical sync frequency is the
// same value from the mode table.
void ReadRefresh(const DISPLAYCONFIG_PATH_INFO& path, const std::vector<DISPLAYCONFIG_MODE_INFO>& modes,
                 DisplayPathRefresh& refresh) {
    refresh.refreshNumerator = path.targetInfo.refreshRate.Numerator;
    refresh.refreshDenominator = path.targetInfo.refreshRate.Denominator;
    if (refresh.refreshNumerator != 0 && refresh.refreshDenominator != 0)
        return;
    const UINT32 modeIndex = path.targetInfo.modeInfoIdx;
    if (modeIndex == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || modeIndex >= modes.size())
        return;
    const DISPLAYCONFIG_MODE_INFO& mode = modes[modeIndex];
    if (mode.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_TARGET)
        return;
    refresh.refreshNumerator = mode.targetMode.targetVideoSignalInfo.vSyncFreq.Numerator;
    refresh.refreshDenominator = mode.targetMode.targetVideoSignalInfo.vSyncFreq.Denominator;
}

}  // namespace

DisplayRefreshPeriods QueryDisplayRefreshPeriods() {
    for (int attempt = 0; attempt < 3; ++attempt) {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
            return {};

        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        const LONG result =
            QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        if (result == ERROR_INSUFFICIENT_BUFFER)
            continue;
        if (result != ERROR_SUCCESS)
            return {};
        paths.resize(pathCount);
        modes.resize(modeCount);

        std::vector<DisplayPathRefresh> refreshes;
        refreshes.reserve(paths.size());
        for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
            DisplayPathRefresh refresh;
            refresh.adapterLuid = (static_cast<uint64_t>(static_cast<uint32_t>(path.sourceInfo.adapterId.HighPart))
                                   << 32u) |
                                  path.sourceInfo.adapterId.LowPart;
            refresh.sourceId = path.sourceInfo.id;
            ReadRefresh(path, modes, refresh);
            refreshes.push_back(refresh);
        }
        return DisplayRefreshPeriods::FromPaths(refreshes);
    }
    return {};
}

void LogDisplayRefreshPeriods(const DisplayRefreshPeriods& periods) {
    char text[256] = {};
    std::size_t used = 0;
    for (std::size_t i = 0; i < periods.count() && used < sizeof(text); ++i) {
        const int written = std::snprintf(text + used, sizeof(text) - used, " source%u=%lldus", periods.SourceAt(i),
                                          static_cast<long long>(periods.PeriodAt(i)));
        if (written <= 0)
            break;
        used += static_cast<std::size_t>(written);
    }
    LogInfo("[DisplayTiming] Display minimum refresh periods (0 = unknown or ambiguous):%s",
            periods.count() != 0 ? text : " none");
}
