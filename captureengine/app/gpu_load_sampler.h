// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

// Native GPU load sampler with a two-tier hardware/kernel hierarchy:
//   1. NVIDIA NVML hardware PMU utilization (LUID-matched via D3DKMT PCI
//      bus/device), integrating 200 ms PMU samples over the poll interval with
//      nvmlDeviceGetUtilizationRates as fallback. Measures actual GPU core
//      execution activity rather than WDDM queue-wait time, avoiding both
//      HAGS/DX9 PDH 0%<->100% dropouts and FIFO-VSync ~100% inflation.
//   2. Direct WDDM D3DKMTQueryStatistics(NODE) + KMTQAITYPE_NODEMETADATA for
//      non-NVIDIA adapters (AMD, Intel) or when NVML is unavailable/unmatched,
//      avoiding PDH wildcard instance churn and per-process attribution drops.
//
// All OS/driver function pointers are injectable via NvmlLoadApi / D3dkmtLoadApi
// so every branch is deterministically unit-tested without live hardware.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "gpu_load_policy.h"

namespace scan_host {

// ABI mirrors for NVML utilization/PCI queries (nvml.h).
struct NvmlUtilization {
    unsigned int gpu = 0;
    unsigned int memory = 0;
};

union NvmlSampleValue {
    double dVal;
    unsigned int uiVal;
    unsigned long ulVal;
    unsigned long long ullVal;
    long long sllVal;
};

struct NvmlSample {
    unsigned long long timeStamp = 0;
    NvmlSampleValue sampleValue{};
};

struct NvmlPciInfo {
    char busIdLegacy[16]{};
    unsigned int domain = 0;
    unsigned int bus = 0;
    unsigned int device = 0;
    unsigned int pciDeviceId = 0;
    unsigned int pciSubSystemId = 0;
    char busId[32]{};
};

struct NvmlLoadApi {
    void* module = nullptr;
    int (*init)() = nullptr;
    int (*shutdown)() = nullptr;
    int (*deviceGetCount)(unsigned int*) = nullptr;
    int (*deviceGetHandleByIndex)(unsigned int, void**) = nullptr;
    int (*deviceGetName)(void*, char*, unsigned int) = nullptr;
    int (*deviceGetPciInfo)(void*, NvmlPciInfo*) = nullptr;
    int (*deviceGetUtilizationRates)(void*, NvmlUtilization*) = nullptr;
    int (*deviceGetSamples)(void*, int, unsigned long long, int*, unsigned int*, NvmlSample*) = nullptr;
};

// ABI mirrors for WDDM D3DKMT adapter/node queries (d3dkmthk.h / d3dkmdt.h).
namespace kmt_load_abi {

using Handle = uint32_t;

struct Luid {
    uint32_t lowPart = 0;
    int32_t highPart = 0;
};

struct OpenAdapterFromLuid {
    Luid adapterLuid{};
    Handle adapter = 0;
};

struct CloseAdapter {
    Handle adapter = 0;
};

struct QueryAdapterInfo {
    Handle adapter = 0;
    uint32_t type = 0;
    void* privateDriverData = nullptr;
    uint32_t privateDriverDataSize = 0;
};

struct AdapterAddress {
    uint32_t busNumber = 0;
    uint32_t deviceNumber = 0;
    uint32_t functionNumber = 0;
};

#pragma pack(push, 1)
struct NodeMetadata {
    uint32_t nodeOrdinalAndAdapterIndex = 0;
    uint32_t engineType = 0;
    wchar_t friendlyName[32]{};
    uint32_t flags = 0;
    uint8_t gpuMmuSupported = 0;
    uint8_t ioMmuSupported = 0;
};
#pragma pack(pop)

// Layout of x64 D3DKMT_QUERYSTATISTICS (d3dkmthk.h C_ASSERT size == 0x328).
// Only the fields we read/write are named; offsets are pinned with static_assert.
struct QueryStatistics {
    uint32_t type = 0;          // offset 0
    Luid adapterLuid{};         // offset 4
    uint32_t pad0 = 0;          // offset 12
    void* process = nullptr;    // offset 16 (8 bytes on x64)
    union {
        struct {
            uint32_t nbSegments;
            uint32_t nodeCount;
        } adapterInfo;
        struct {
            int64_t runningTime;  // GlobalInformation.RunningTime (100ns ticks)
        } nodeInfo;
        uint8_t raw[0x308];
    } queryResult{};            // offset 24 .. 0x31F
    uint32_t queryNodeId = 0;   // offset 0x320
    uint32_t pad1 = 0;          // offset 0x324
};

inline constexpr uint32_t kQueryAdapterAddress = 6;
inline constexpr uint32_t kQueryNodeMetadata = 25;
inline constexpr uint32_t kQueryStatsAdapter = 0;
inline constexpr uint32_t kQueryStatsNode = 5;

static_assert(sizeof(OpenAdapterFromLuid) == 12, "D3DKMT_OPENADAPTERFROMLUID size");
static_assert(sizeof(CloseAdapter) == 4, "D3DKMT_CLOSEADAPTER size");
static_assert(sizeof(AdapterAddress) == 12, "D3DKMT_ADAPTERADDRESS size");
static_assert(sizeof(NodeMetadata) == 78, "packed D3DKMT_NODEMETADATA size");
static_assert(offsetof(NodeMetadata, engineType) == 4, "D3DKMT_NODEMETADATA::NodeData.EngineType");
static_assert(offsetof(NodeMetadata, friendlyName) == 8, "D3DKMT_NODEMETADATA::NodeData.FriendlyName");
#if defined(_WIN64)
static_assert(sizeof(QueryAdapterInfo) == 24, "64-bit D3DKMT_QUERYADAPTERINFO size");
static_assert(sizeof(QueryStatistics) == 0x328, "64-bit D3DKMT_QUERYSTATISTICS size");
static_assert(offsetof(QueryStatistics, adapterLuid) == 4, "D3DKMT_QUERYSTATISTICS::AdapterLuid");
static_assert(offsetof(QueryStatistics, process) == 16, "D3DKMT_QUERYSTATISTICS::hProcess");
static_assert(offsetof(QueryStatistics, queryResult) == 24, "D3DKMT_QUERYSTATISTICS::QueryResult");
static_assert(offsetof(QueryStatistics, queryNodeId) == 0x320, "D3DKMT_QUERYSTATISTICS::QueryNode");
#endif

}  // namespace kmt_load_abi

struct D3dkmtLoadApi {
    long (*openAdapterFromLuid)(kmt_load_abi::OpenAdapterFromLuid*) = nullptr;
    long (*closeAdapter)(const kmt_load_abi::CloseAdapter*) = nullptr;
    long (*queryAdapterInfo)(kmt_load_abi::QueryAdapterInfo*) = nullptr;
    long (*queryStatistics)(kmt_load_abi::QueryStatistics*) = nullptr;
};

struct GpuLoadReading {
    metrics_policy::GpuLoadSource source = metrics_policy::GpuLoadSource::Unavailable;
    double percent = 0.0;
    bool valid = false;
    std::string detail;
};

NvmlLoadApi LoadSystemNvmlLoadApi();
D3dkmtLoadApi LoadSystemD3dkmtLoadApi();
unsigned int CountNonSoftwareDxgiAdapters();

class GpuLoadSampler {
public:
    using Clock100nsFn = std::function<int64_t()>;
    using HardwareAdapterCountFn = std::function<unsigned int()>;

    GpuLoadSampler();
    GpuLoadSampler(NvmlLoadApi nvmlApi, D3dkmtLoadApi d3dkmtApi, Clock100nsFn clock100ns = {},
                   HardwareAdapterCountFn hardwareAdapterCount = {});
    ~GpuLoadSampler();

    GpuLoadSampler(const GpuLoadSampler&) = delete;
    GpuLoadSampler& operator=(const GpuLoadSampler&) = delete;

    // Samples GPU load for `adapterLuid` via NVML (if matched) or direct WDDM
    // D3DKMT node statistics. Returns `valid = false` when neither backend can
    // answer yet (e.g. first D3DKMT baseline poll), letting the caller use PDH.
    GpuLoadReading SampleAdapter(int64_t adapterLuid);

    // Releases cached per-adapter baselines and driver handles.
    void Reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace scan_host
