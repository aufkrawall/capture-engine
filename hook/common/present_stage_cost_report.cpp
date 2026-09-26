#include "present_stage_cost.h"

#include "hook_common.h"
#include "hook_cpu_cost.h"

#include <cstdarg>
#include <cstdio>

DWORD DX12_GetGamePresentThreadId();

namespace ce::present_stage_cost {

int64_t QpcLapBackend::Now() {
    return HookQpcTicks();
}

uint64_t QpcLapBackend::ToNs(int64_t ticks) {
    return ticks > 0 ? static_cast<uint64_t>(static_cast<double>(ticks) * HookQpcTicksToUs() * 1000.0) : 0;
}

Aggregator& PresentStageAggregator() {
    static Aggregator aggregator;
    return aggregator;
}

// Early returns before the present context is captured (shutdown, reentrancy,
// invisible helper windows) only know the thread id.
ThreadRole ClassifyCurrentThreadWithoutPresentContext() {
    return ClassifyThreadRole(DX12_GetGamePresentThreadId(), GetCurrentThreadId(), false);
}

namespace {
double Us(uint64_t ns) {
    return static_cast<double>(ns) / 1000.0;
}

int Append(char* buffer, size_t bufferSize, int written, const char* fmt, ...) {
    if (written < 0 || static_cast<size_t>(written) >= bufferSize) {
        return written;
    }
    va_list args;
    va_start(args, fmt);
    const int added = vsnprintf(buffer + written, bufferSize - static_cast<size_t>(written), fmt, args);
    va_end(args);
    return added < 0 ? written : written + added;
}
}  // namespace

// One line per role and window. Stage fields are mean/p95/max in microseconds
// with n = presents that entered the stage; means are taken over every present
// of the role, so the stage means (forward excluded) add up to own's mean.
int FormatRoleSummary(const RoleSummary& summary, uint64_t windowMs, char* buffer, size_t bufferSize) {
    if (buffer == nullptr || bufferSize == 0) {
        return 0;
    }
    buffer[0] = '\0';
    int written = Append(buffer, bufferSize, 0,
                         "[PRESENT STAGE COST] role=%s window=%llums calls=%llu own=%.1f/%.1f/%.1f "
                         "detour=%.1f/%.1f/%.1f forward=%.1f/%.1f/%.1f |",
                         RoleName(summary.role), static_cast<unsigned long long>(windowMs),
                         static_cast<unsigned long long>(summary.calls), summary.own.MeanUs(),
                         Us(summary.own.p95Ns), Us(summary.own.maxNs), summary.detour.MeanUs(),
                         Us(summary.detour.p95Ns), Us(summary.detour.maxNs),
                         summary.stages[StageIndex(Stage::kForward)].MeanUs(),
                         Us(summary.stages[StageIndex(Stage::kForward)].p95Ns),
                         Us(summary.stages[StageIndex(Stage::kForward)].maxNs));
    for (size_t stage = 0; stage < kStageCount; ++stage) {
        if (stage == StageIndex(Stage::kForward)) {
            continue;
        }
        const StageSummary& stats = summary.stages[stage];
        written = Append(buffer, bufferSize, written, " %s=%.1f/%.1f/%.1f(n=%llu)",
                         StageName(static_cast<Stage>(stage)), stats.MeanUs(), Us(stats.p95Ns), Us(stats.maxNs),
                         static_cast<unsigned long long>(stats.samples));
    }
    written = Append(buffer, bufferSize, written,
                     " — us mean/p95/max; own = detour minus forwarded Present = sum of the stage means");
    // vsnprintf reports the untruncated length; the caller gets what is in the buffer.
    const int capacity = static_cast<int>(bufferSize - 1);
    return written < 0 ? 0 : (written > capacity ? capacity : written);
}

// Hook service thread only, so the window state needs no synchronization and the
// present threads never pay for formatting or the log write.
void ReportPresentStageCostIfDue() {
    static constexpr uint64_t kWindowMs = 10'000;
    static uint64_t s_windowStartMs = 0;
    const uint64_t now = GetTickCount64();
    if (s_windowStartMs == 0) {
        s_windowStartMs = now;
        return;
    }
    const uint64_t elapsedMs = now - s_windowStartMs;
    if (elapsedMs < kWindowMs) {
        return;
    }
    s_windowStartMs = now;
    for (size_t role = 0; role < kRoleCount; ++role) {
        const RoleSummary summary = PresentStageAggregator().Drain(static_cast<ThreadRole>(role));
        if (summary.calls == 0) {
            continue;
        }
        char line[1024];
        FormatRoleSummary(summary, elapsedMs, line, sizeof(line));
        HookLogImportant("%s", line);
    }
}

}  // namespace ce::present_stage_cost
