#include "d3d11_wrapper_af_cost.h"

#include <atomic>

#include "hook/metrics/hook_cpu_cost.h"
#include "hook/runtime/hook_common.h"

namespace ce::wrapper_af_cost {
namespace {

std::atomic<uint64_t> g_totals[kCount];

struct Batch {
    uint64_t values[kCount] = {};
    unsigned pending = 0;

    void Flush() {
        for (unsigned i = 0; i < kCount; ++i) {
            if (values[i] != 0) {
                g_totals[i].fetch_add(values[i], std::memory_order_relaxed);
                values[i] = 0;
            }
        }
        pending = 0;
    }
};
// Trivially destructible on purpose: no TLS destructor in an injected DLL. A
// thread that exits loses at most its last unflushed batch of counts.

thread_local Batch t_batch;

}  // namespace

void Add(Counter counter, uint64_t amount) {
    t_batch.values[counter] += amount;
    if (++t_batch.pending >= 1024) {
        t_batch.Flush();
    }
}

Sample::Sample() {
    thread_local uint32_t t_calls = 0;
    if ((++t_calls & 31u) == 0) {
        startTicks_ = HookQpcTicks();
    }
}

Sample::~Sample() {
    if (startTicks_ != 0) {
        Add(kTimedWork);
        Add(kTimedWorkTicks, static_cast<uint64_t>(HookQpcTicks() - startTicks_));
    }
}

void LogWindowIfActive(uint64_t elapsedMs) {
    static uint64_t s_reported[kCount] = {};
    uint64_t window[kCount] = {};
    for (unsigned i = 0; i < kCount; ++i) {
        const uint64_t total = g_totals[i].load(std::memory_order_relaxed);
        window[i] = total - s_reported[i];
        s_reported[i] = total;
    }
    const uint64_t calls = window[kSrvBinds] + window[kSamplerBinds] + window[kShaderBinds] + window[kPasses];
    if (calls == 0 || elapsedMs == 0) {
        return;
    }
    const double seconds = static_cast<double>(elapsedMs) / 1000.0;
    const double workUs = window[kTimedWork] != 0 ? static_cast<double>(window[kTimedWorkTicks]) * HookQpcTicksToUs() /
                                                         static_cast<double>(window[kTimedWork])
                                                   : 0.0;
    // Every counted bind and pass carries one sample stream, so the mean covers both.
    const double estMsPerS = workUs * static_cast<double>(calls) / seconds / 1000.0;
    HookLogImportant(
        "[D3D11 AF COST wrapper] window=%llums binds/s(srv=%.0f sampler=%.0f shader=%.0f) passes/s=%.0f "
        "resolves=%llu variantLookups=%llu rebinds=%llu workUs=%.2f(sampled=%llu) estMsPerS=%.2f",
        static_cast<unsigned long long>(elapsedMs), static_cast<double>(window[kSrvBinds]) / seconds,
        static_cast<double>(window[kSamplerBinds]) / seconds, static_cast<double>(window[kShaderBinds]) / seconds,
        static_cast<double>(window[kPasses]) / seconds, static_cast<unsigned long long>(window[kResolves]),
        static_cast<unsigned long long>(window[kVariantLookups]), static_cast<unsigned long long>(window[kRebinds]),
        workUs, static_cast<unsigned long long>(window[kTimedWork]), estMsPerS);
}

}  // namespace ce::wrapper_af_cost
