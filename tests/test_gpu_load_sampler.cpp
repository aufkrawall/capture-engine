// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include <gtest/gtest.h>

#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

#include "captureengine/app/gpu_load_policy.h"
#include "captureengine/app/gpu_load_sampler.h"

namespace {

using namespace scan_host;
using namespace scan_host::metrics_policy;

static_assert(std::is_nothrow_default_constructible_v<GpuLoadSampler>);

// ============================================================================
// NVML PMU sample integration
// ============================================================================

TEST(GpuLoadPolicyTest, IntegratesFreshNvmlSamplesAndIgnoresOldOrSentinelEntries) {
    const std::vector<NvmlUtilSample> samples = {
        {0, 99},          // sentinel timestamp
        {900'000, 10},    // older than lastSeen
        {1'000'000, 20},  // equal to lastSeen
        {1'200'000, 94}, {1'400'000, 96}, {1'600'000, 95},
    };
    const NvmlSampleIntegration out = IntegrateNvmlGpuSamples(samples, /*lastSeenTimestampUs=*/1'000'000);
    ASSERT_TRUE(out.valid);
    EXPECT_EQ(out.sampleCount, 3u);
    EXPECT_EQ(out.newestTimestampUs, 1'600'000ull);
    EXPECT_DOUBLE_EQ(out.percent, 95.0);
}

TEST(GpuLoadPolicyTest, NvmlIntegrationBoundsStaleHistoryOnFirstPollAndHandlesUnsortedOrder) {
    const std::vector<NvmlUtilSample> samples = {
        {2'000'000, 90}, {200'000, 5},                   // older than the 1.2s window before newest (2.2s)
        {2'200'000, 94}, {1'800'000, 92}, {400'000, 5},  // outside window
    };
    const NvmlSampleIntegration out = IntegrateNvmlGpuSamples(samples, /*lastSeenTimestampUs=*/0);
    ASSERT_TRUE(out.valid);
    EXPECT_EQ(out.sampleCount, 3u);
    EXPECT_EQ(out.newestTimestampUs, 2'200'000ull);
    EXPECT_DOUBLE_EQ(out.percent, 92.0);
}

TEST(GpuLoadPolicyTest, NvmlIntegrationReturnsInvalidWhenNoFreshSampleArrived) {
    const std::vector<NvmlUtilSample> samples = {{1'000'000, 50}};
    const NvmlSampleIntegration out = IntegrateNvmlGpuSamples(samples, 1'000'000);
    EXPECT_FALSE(out.valid);
    EXPECT_EQ(out.newestTimestampUs, 1'000'000ull);
}

// ============================================================================
// D3DKMT node classification and running-time delta
// ============================================================================

TEST(GpuLoadPolicyTest, ClassifiesVideoAndOpticalFlowD3dkmtNodesCorrectly) {
    EXPECT_FALSE(IsD3dkmtVideoEngineNode(static_cast<uint32_t>(D3dkmtEngineType::ThreeD), L""));
    EXPECT_FALSE(IsD3dkmtVideoEngineNode(static_cast<uint32_t>(D3dkmtEngineType::Copy), L""));
    EXPECT_FALSE(IsD3dkmtVideoEngineNode(static_cast<uint32_t>(D3dkmtEngineType::Other), L"VR"));
    EXPECT_TRUE(IsD3dkmtVideoEngineNode(static_cast<uint32_t>(D3dkmtEngineType::VideoDecode), L""));
    EXPECT_TRUE(IsD3dkmtVideoEngineNode(static_cast<uint32_t>(D3dkmtEngineType::VideoEncode), L""));
    EXPECT_TRUE(IsD3dkmtVideoEngineNode(static_cast<uint32_t>(D3dkmtEngineType::VideoProcessing), L""));
    EXPECT_TRUE(IsD3dkmtVideoEngineNode(static_cast<uint32_t>(D3dkmtEngineType::VideoCodec), L""));
    EXPECT_TRUE(IsD3dkmtVideoEngineNode(static_cast<uint32_t>(D3dkmtEngineType::Other), L"OFA_0"));
    EXPECT_TRUE(IsD3dkmtVideoEngineNode(static_cast<uint32_t>(D3dkmtEngineType::Other), L"JPEG_Decode_0"));
}

TEST(GpuLoadPolicyTest, ComputesBusiestNonVideoD3dkmtNodeAndRejectsRegressions) {
    // 1 second elapsed = 10,000,000 ticks (100ns)
    const int64_t elapsed = 10'000'000;
    const std::vector<D3dkmtNodeRunningTimeSample> prev = {
        {100'000, true, false},  // 3D
        {200'000, true, true},   // VideoEncode (must be ignored)
        {300'000, true, false},  // Copy
    };
    const std::vector<D3dkmtNodeRunningTimeSample> curr = {
        {7'600'000, true, false},  // 75%
        {10'200'000, true, true},  // 100% video encode - ignored
        {1'300'000, true, false},  // 10%
    };
    const D3dkmtLoadResult ok = ComputeD3dkmtAdapterLoadPercent(prev, curr, elapsed);
    ASSERT_TRUE(ok.valid);
    EXPECT_DOUBLE_EQ(ok.percent, 75.0);

    // Too-short elapsed interval is rejected.
    EXPECT_FALSE(ComputeD3dkmtAdapterLoadPercent(prev, curr, kMinD3dkmtDeltaElapsed100ns - 1).valid);

    // Counter regression on the only active node yields invalid rather than 0%.
    const std::vector<D3dkmtNodeRunningTimeSample> regressed = {
        {50'000, true, false},  // regressed
        {200'000, true, true},
        {300'000, true, false},  // 0%
    };
    EXPECT_FALSE(ComputeD3dkmtAdapterLoadPercent(prev, regressed, elapsed).valid);
}

// ============================================================================
// PDH incomplete-poll detection
// ============================================================================

TEST(GpuLoadPolicyTest, DetectsCorruptPdhPollWhenInvalidStatusLeavesZeroLoad) {
    const uint64_t render = ParseGpuEngineKey("pid_100_luid_0x0_0x1_phys_0_eng_0_engtype_3d");
    const PdhGpuLoadPoll corrupt = EvaluatePdhGpuLoadPoll({{render, 0.0, false}});
    EXPECT_TRUE(corrupt.pollCorrupt);
    EXPECT_EQ(corrupt.invalidItems, 1u);
    EXPECT_DOUBLE_EQ(corrupt.percent, 0.0);

    // Genuine idle (zero instances, no invalid items) is NOT corrupt.
    const PdhGpuLoadPoll idle = EvaluatePdhGpuLoadPoll({});
    EXPECT_FALSE(idle.pollCorrupt);
    EXPECT_DOUBLE_EQ(idle.percent, 0.0);

    // Valid non-zero load alongside an unrelated invalid instance stays usable.
    const PdhGpuLoadPoll mixed = EvaluatePdhGpuLoadPoll({{render, 82.0, true}, {render, 0.0, false}});
    EXPECT_FALSE(mixed.pollCorrupt);
    EXPECT_DOUBLE_EQ(mixed.percent, 82.0);
}

// ============================================================================
// Temporal stabilization and glitch rejection
// ============================================================================

TEST(GpuLoadStabilizerTest, SuppressesSinglePollWddmZeroDropoutUnderConstantLoad) {
    GpuLoadStabilizer stabilizer;
    EXPECT_DOUBLE_EQ(stabilizer.Update(GpuLoadSource::PdhEngine, 96.0, true).percent, 96.0);

    // Mirror's Edge-style single-poll drop to 0% is held instead of flashing 0%.
    const GpuLoadStabilizerOutput glitch = stabilizer.Update(GpuLoadSource::PdhEngine, 0.0, true);
    EXPECT_TRUE(glitch.readable);
    EXPECT_TRUE(glitch.suppressedDropout);
    EXPECT_DOUBLE_EQ(glitch.percent, 96.0);

    // Next poll recovers to 97%: overlay never flashed 0%.
    const GpuLoadStabilizerOutput recovered = stabilizer.Update(GpuLoadSource::PdhEngine, 97.0, true);
    EXPECT_FALSE(recovered.suppressedDropout);
    EXPECT_GT(recovered.percent, 95.0);
    EXPECT_EQ(stabilizer.Stats().suppressedDropouts, 1u);
    EXPECT_EQ(stabilizer.Stats().confirmedDrops, 0u);
}

TEST(GpuLoadStabilizerTest, ConfirmsRealDropToIdleOnSecondConsecutiveLowPoll) {
    GpuLoadStabilizer stabilizer;
    EXPECT_DOUBLE_EQ(stabilizer.Update(GpuLoadSource::D3dkmtNode, 90.0, true).percent, 90.0);

    const GpuLoadStabilizerOutput firstLow = stabilizer.Update(GpuLoadSource::D3dkmtNode, 0.0, true);
    EXPECT_TRUE(firstLow.suppressedDropout);
    EXPECT_DOUBLE_EQ(firstLow.percent, 90.0);

    // Second consecutive low poll confirms a real idle transition immediately.
    const GpuLoadStabilizerOutput secondLow = stabilizer.Update(GpuLoadSource::D3dkmtNode, 0.0, true);
    EXPECT_FALSE(secondLow.suppressedDropout);
    EXPECT_DOUBLE_EQ(secondLow.percent, 0.0);
    EXPECT_EQ(stabilizer.Stats().confirmedDrops, 1u);
}

TEST(GpuLoadStabilizerTest, HoldsReadingAcrossBoundedCorruptPollsThenReleases) {
    GpuLoadStabilizer stabilizer;
    EXPECT_DOUBLE_EQ(stabilizer.Update(GpuLoadSource::PdhEngine, 72.0, true).percent, 72.0);

    const GpuLoadStabilizerOutput hold1 =
        stabilizer.Update(GpuLoadSource::PdhEngine, 0.0, true, /*sampleCorrupt=*/true);
    EXPECT_TRUE(hold1.readable);
    EXPECT_TRUE(hold1.heldCorrupt);
    EXPECT_DOUBLE_EQ(hold1.percent, 72.0);

    const GpuLoadStabilizerOutput hold2 =
        stabilizer.Update(GpuLoadSource::PdhEngine, 0.0, true, /*sampleCorrupt=*/true);
    EXPECT_TRUE(hold2.heldCorrupt);
    EXPECT_DOUBLE_EQ(hold2.percent, 72.0);

    // Past kMaxCorruptHoldPolls, a query failure becomes unreadable rather than stuck forever.
    const GpuLoadStabilizerOutput failed = stabilizer.Update(GpuLoadSource::PdhEngine, 0.0, /*sampleValid=*/false);
    EXPECT_FALSE(failed.readable);
}

TEST(GpuLoadStabilizerTest, HardwarePmuRespondsFastAndAdoptsCleanlyOverInflatedWddm) {
    GpuLoadStabilizer stabilizer;
    // Poll 1: WDDM FIFO-VSync reports 99%.
    EXPECT_DOUBLE_EQ(stabilizer.Update(GpuLoadSource::PdhEngine, 99.0, true).percent, 99.0);
    // Poll 2: NVML hardware PMU reports true 42% -> adopts immediately without dragging 99% along.
    const GpuLoadStabilizerOutput upgraded = stabilizer.Update(GpuLoadSource::NvmlSamples, 42.0, true);
    EXPECT_DOUBLE_EQ(upgraded.percent, 42.0);

    // Step load up to 95% reacts within one poll (~87%).
    const GpuLoadStabilizerOutput stepUp = stabilizer.Update(GpuLoadSource::NvmlSamples, 95.0, true);
    EXPECT_GT(stepUp.percent, 85.0);
    // Step drop to 2% reacts within one poll (no 1-poll dropout stall on hardware PMU).
    const GpuLoadStabilizerOutput stepDown = stabilizer.Update(GpuLoadSource::NvmlSamples, 2.0, true);
    EXPECT_LT(stepDown.percent, 20.0);
}

// ============================================================================
// GpuLoadSampler end-to-end with injectable fake NVML + fake D3DKMT
// ============================================================================

struct FakeDriverEnv {
    static inline int nvmlInitResult = 0;
    static inline int nvmlShutdowns = 0;
    static inline unsigned int nvmlCount = 1;
    static inline unsigned int nvmlPciBus = 6;
    static inline unsigned int nvmlPciDev = 0;
    static inline unsigned int nvmlRateGpu = 48;
    static inline int nvmlSamplesStatus = 0;
    static inline std::vector<NvmlSample> nvmlSamples;

    static inline std::map<int64_t, kmt_load_abi::AdapterAddress> adapterAddresses;
    static inline std::map<int64_t, std::vector<uint32_t>> adapterNodeTypes;
    static inline std::map<int64_t, std::vector<std::wstring>> adapterNodeNames;
    static inline std::map<int64_t, std::vector<int64_t>> adapterNodeRunningTimes;
    static inline std::map<kmt_load_abi::Handle, int64_t> openHandles;
    static inline kmt_load_abi::Handle nextHandle = 100;

    static void Reset() {
        nvmlInitResult = 0;
        nvmlShutdowns = 0;
        nvmlCount = 1;
        nvmlPciBus = 6;
        nvmlPciDev = 0;
        nvmlRateGpu = 48;
        nvmlSamplesStatus = 0;
        nvmlSamples.clear();
        adapterAddresses.clear();
        adapterNodeTypes.clear();
        adapterNodeNames.clear();
        adapterNodeRunningTimes.clear();
        openHandles.clear();
        nextHandle = 100;
    }

    static int64_t CombineLuid(kmt_load_abi::Luid luid) {
        return static_cast<int64_t>((static_cast<uint64_t>(static_cast<uint32_t>(luid.highPart)) << 32) | luid.lowPart);
    }

    static int NvmlInit() {
        return nvmlInitResult;
    }
    static int NvmlShutdown() {
        ++nvmlShutdowns;
        return 0;
    }
    static int NvmlCount(unsigned int* count) {
        *count = nvmlCount;
        return 0;
    }
    static int NvmlHandle(unsigned int index, void** handle) {
        *handle = reinterpret_cast<void*>(static_cast<uintptr_t>(index + 1));
        return 0;
    }
    static int NvmlName(void*, char* name, unsigned int size) {
        std::snprintf(name, size, "Fake RTX GPU");
        return 0;
    }
    static int NvmlPci(void*, NvmlPciInfo* pci) {
        *pci = {};
        pci->domain = 0;
        pci->bus = nvmlPciBus;
        pci->device = nvmlPciDev;
        return 0;
    }
    static int NvmlRates(void*, NvmlUtilization* util) {
        util->gpu = nvmlRateGpu;
        util->memory = 12;
        return 0;
    }
    static int NvmlGetSamples(void*, int, unsigned long long, int* valType, unsigned int* count, NvmlSample* out) {
        if (nvmlSamplesStatus != 0)
            return nvmlSamplesStatus;
        if (valType)
            *valType = 1;
        if (!out) {
            *count = static_cast<unsigned int>(nvmlSamples.size());
            return 0;
        }
        const unsigned int copy = *count < nvmlSamples.size() ? *count : static_cast<unsigned int>(nvmlSamples.size());
        for (unsigned int i = 0; i < copy; ++i)
            out[i] = nvmlSamples[i];
        *count = copy;
        return 0;
    }

    static long KmtOpen(kmt_load_abi::OpenAdapterFromLuid* req) {
        const int64_t luid = CombineLuid(req->adapterLuid);
        if (adapterAddresses.find(luid) == adapterAddresses.end() &&
            adapterNodeTypes.find(luid) == adapterNodeTypes.end()) {
            return -1;
        }
        const kmt_load_abi::Handle h = nextHandle++;
        openHandles[h] = luid;
        req->adapter = h;
        return 0;
    }

    static long KmtClose(const kmt_load_abi::CloseAdapter* req) {
        openHandles.erase(req->adapter);
        return 0;
    }

    static long KmtQueryAdapterInfo(kmt_load_abi::QueryAdapterInfo* req) {
        const auto it = openHandles.find(req->adapter);
        if (it == openHandles.end())
            return -1;
        const int64_t luid = it->second;
        if (req->type == kmt_load_abi::kQueryAdapterAddress) {
            const auto addrIt = adapterAddresses.find(luid);
            if (addrIt == adapterAddresses.end() || req->privateDriverDataSize < sizeof(kmt_load_abi::AdapterAddress))
                return -1;
            *static_cast<kmt_load_abi::AdapterAddress*>(req->privateDriverData) = addrIt->second;
            return 0;
        }
        if (req->type == kmt_load_abi::kQueryNodeMetadata) {
            auto* meta = static_cast<kmt_load_abi::NodeMetadata*>(req->privateDriverData);
            const uint32_t node = meta->nodeOrdinalAndAdapterIndex & 0xFFFFu;
            const auto typeIt = adapterNodeTypes.find(luid);
            if (typeIt == adapterNodeTypes.end() || node >= typeIt->second.size())
                return -1;
            meta->engineType = typeIt->second[node];
            std::memset(meta->friendlyName, 0, sizeof(meta->friendlyName));
            const auto nameIt = adapterNodeNames.find(luid);
            if (nameIt != adapterNodeNames.end() && node < nameIt->second.size()) {
                const std::wstring& n = nameIt->second[node];
                for (size_t i = 0; i < n.size() && i < 31; ++i)
                    meta->friendlyName[i] = n[i];
            }
            return 0;
        }
        return -1;
    }

    static long KmtQueryStatistics(kmt_load_abi::QueryStatistics* req) {
        const int64_t luid = CombineLuid(req->adapterLuid);
        const auto typeIt = adapterNodeTypes.find(luid);
        if (typeIt == adapterNodeTypes.end())
            return -1;
        if (req->type == kmt_load_abi::kQueryStatsAdapter) {
            req->queryResult.adapterInfo.nbSegments = 3;
            req->queryResult.adapterInfo.nodeCount = static_cast<uint32_t>(typeIt->second.size());
            return 0;
        }
        if (req->type == kmt_load_abi::kQueryStatsNode) {
            const auto rtIt = adapterNodeRunningTimes.find(luid);
            if (rtIt == adapterNodeRunningTimes.end() || req->queryNodeId >= rtIt->second.size())
                return -1;
            req->queryResult.nodeInfo.runningTime = rtIt->second[req->queryNodeId];
            return 0;
        }
        return -1;
    }

    static NvmlLoadApi MakeNvml() {
        NvmlLoadApi api;
        api.init = &NvmlInit;
        api.shutdown = &NvmlShutdown;
        api.deviceGetCount = &NvmlCount;
        api.deviceGetHandleByIndex = &NvmlHandle;
        api.deviceGetName = &NvmlName;
        api.deviceGetPciInfo = &NvmlPci;
        api.deviceGetUtilizationRates = &NvmlRates;
        api.deviceGetSamples = &NvmlGetSamples;
        return api;
    }

    static D3dkmtLoadApi MakeKmt() {
        D3dkmtLoadApi api;
        api.openAdapterFromLuid = &KmtOpen;
        api.closeAdapter = &KmtClose;
        api.queryAdapterInfo = &KmtQueryAdapterInfo;
        api.queryStatistics = &KmtQueryStatistics;
        return api;
    }
};

TEST(GpuLoadSamplerTest, MatchesNvidiaAdapterByPciAndFallsThroughToD3dkmtOnNonNvidiaAdapter) {
    FakeDriverEnv::Reset();
    constexpr int64_t kNvidiaLuid = 0xBDF5;
    constexpr int64_t kIgpuLuid = 0xCAFE;

    FakeDriverEnv::adapterAddresses[kNvidiaLuid] = {6, 0, 0};
    FakeDriverEnv::adapterAddresses[kIgpuLuid] = {0, 2, 0};
    // iGPU has 3 nodes: 3D (node 0), VideoEncode (node 1), OFA (node 2)
    FakeDriverEnv::adapterNodeTypes[kIgpuLuid] = {1, 3, 0};
    FakeDriverEnv::adapterNodeNames[kIgpuLuid] = {L"", L"", L"OFA_0"};
    FakeDriverEnv::adapterNodeRunningTimes[kIgpuLuid] = {1'000'000, 1'000'000, 1'000'000};

    NvmlSample s1{};
    s1.timeStamp = 1'000'000;
    s1.sampleValue.uiVal = 40;
    NvmlSample s2{};
    s2.timeStamp = 1'200'000;
    s2.sampleValue.uiVal = 44;
    FakeDriverEnv::nvmlSamples = {s1, s2};

    int64_t clock100ns = 100'000'000;
    GpuLoadSampler sampler(
        FakeDriverEnv::MakeNvml(), FakeDriverEnv::MakeKmt(), [&clock100ns]() { return clock100ns; },
        []() { return 2u; });

    // NVIDIA LUID uses NVML 200ms PMU sample average (42%).
    const GpuLoadReading nvReading = sampler.SampleAdapter(kNvidiaLuid);
    ASSERT_TRUE(nvReading.valid);
    EXPECT_EQ(nvReading.source, GpuLoadSource::NvmlSamples);
    EXPECT_DOUBLE_EQ(nvReading.percent, 42.0);
    EXPECT_NE(nvReading.detail.find("0000:06:00.0"), std::string::npos);

    // No new NVML sample on immediate re-poll -> falls back to NvmlRate (48%).
    const GpuLoadReading nvFallback = sampler.SampleAdapter(kNvidiaLuid);
    ASSERT_TRUE(nvFallback.valid);
    EXPECT_EQ(nvFallback.source, GpuLoadSource::NvmlRate);
    EXPECT_DOUBLE_EQ(nvFallback.percent, 48.0);

    // Non-NVIDIA iGPU LUID does NOT match NVIDIA PCI bus -> uses D3DKMT node delta.
    // Poll 1 primes baseline (returns valid=false so caller can use PDH on poll 1).
    const GpuLoadReading igpuPrime = sampler.SampleAdapter(kIgpuLuid);
    EXPECT_FALSE(igpuPrime.valid);

    // Poll 2 (+500 ms = 5,000,000 ticks): 3D ran 3,000,000 ticks (60%),
    // VideoEncode ran 5,000,000 ticks (100%, excluded!), OFA ran 5,000,000 (excluded!).
    clock100ns += 5'000'000;
    FakeDriverEnv::adapterNodeRunningTimes[kIgpuLuid] = {4'000'000, 6'000'000, 6'000'000};
    const GpuLoadReading igpuReading = sampler.SampleAdapter(kIgpuLuid);
    ASSERT_TRUE(igpuReading.valid);
    EXPECT_EQ(igpuReading.source, GpuLoadSource::D3dkmtNode);
    EXPECT_DOUBLE_EQ(igpuReading.percent, 60.0);
    EXPECT_EQ(FakeDriverEnv::openHandles.size(), 0u) << "adapter handles must never leak";
}

TEST(GpuLoadSamplerTest, FallsBackToNvmlRateWhenGetSamplesIsUnsupported) {
    FakeDriverEnv::Reset();
    constexpr int64_t kNvidiaLuid = 0xBDF5;
    FakeDriverEnv::adapterAddresses[kNvidiaLuid] = {6, 0, 0};
    FakeDriverEnv::nvmlSamplesStatus = 3;  // NVML_ERROR_NOT_SUPPORTED
    FakeDriverEnv::nvmlRateGpu = 67;

    GpuLoadSampler sampler(
        FakeDriverEnv::MakeNvml(), FakeDriverEnv::MakeKmt(), []() { return 0; }, []() { return 1u; });
    const GpuLoadReading reading = sampler.SampleAdapter(kNvidiaLuid);
    ASSERT_TRUE(reading.valid);
    EXPECT_EQ(reading.source, GpuLoadSource::NvmlRate);
    EXPECT_DOUBLE_EQ(reading.percent, 67.0);
}

TEST(GpuLoadSamplerTest, SingleGpuFallbackMatchesNvmlWhenPciQueryUnavailable) {
    FakeDriverEnv::Reset();
    constexpr int64_t kNvidiaLuid = 0xBDF5;
    FakeDriverEnv::nvmlSamplesStatus = 3;
    FakeDriverEnv::nvmlRateGpu = 33;

    // Empty D3DKMT API (no PCI query available), 1 hardware adapter, 1 NVML GPU.
    GpuLoadSampler sampler(FakeDriverEnv::MakeNvml(), D3dkmtLoadApi{}, []() { return 0; }, []() { return 1u; });
    const GpuLoadReading reading = sampler.SampleAdapter(kNvidiaLuid);
    ASSERT_TRUE(reading.valid);
    EXPECT_EQ(reading.source, GpuLoadSource::NvmlRate);
    EXPECT_DOUBLE_EQ(reading.percent, 33.0);
    EXPECT_NE(reading.detail.find("single-gpu"), std::string::npos);
}

}  // namespace
