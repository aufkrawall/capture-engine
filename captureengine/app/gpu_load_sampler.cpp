// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "gpu_load_sampler.h"

#include <dxgi.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "common/platform/secure_dll_loading.h"

namespace scan_host {
namespace {

constexpr int kNvmlSuccess = 0;
constexpr int kNvmlGpuUtilizationSamples = 1;
constexpr uint32_t kInvalidPciNumber = 0xFFFFu;
constexpr uint32_t kMaxReasonableNodeCount = 128;

template <typename T>
void ResolveProc(HMODULE module, const char* name, T* out) {
    *out = reinterpret_cast<T>(GetProcAddress(module, name));
}

kmt_load_abi::Luid SplitLuid(int64_t adapterLuid) {
    kmt_load_abi::Luid luid{};
    luid.lowPart = static_cast<uint32_t>(adapterLuid & 0xFFFFFFFFull);
    luid.highPart = static_cast<int32_t>((static_cast<uint64_t>(adapterLuid) >> 32) & 0xFFFFFFFFull);
    return luid;
}

int64_t QueryMonotonic100ns() {
    static const int64_t frequency = [] {
        LARGE_INTEGER freq{};
        return QueryPerformanceFrequency(&freq) && freq.QuadPart > 0 ? freq.QuadPart : 1;
    }();
    LARGE_INTEGER counter{};
    if (!QueryPerformanceCounter(&counter) || counter.QuadPart <= 0)
        return 0;
    const int64_t wholeSeconds = counter.QuadPart / frequency;
    const int64_t remainder = counter.QuadPart % frequency;
    return wholeSeconds * 10'000'000ll + (remainder * 10'000'000ll) / frequency;
}

std::wstring_view NodeFriendlyNameView(const wchar_t (&name)[32]) {
    size_t len = 0;
    while (len < 32 && name[len] != L'\0')
        ++len;
    return std::wstring_view(name, len);
}

struct NvmlDeviceEntry {
    void* handle = nullptr;
    std::string name;
    bool hasPci = false;
    uint32_t domain = 0;
    uint32_t bus = 0;
    uint32_t device = 0;
    bool samplesSupported = true;
    uint64_t lastSampleTimestampUs = 0;
};

struct D3dkmtNodeState {
    int64_t adapterLuid = 0;
    bool probed = false;
    bool usable = false;
    uint32_t nodeCount = 0;
    uint32_t nonVideoNodeCount = 0;
    std::vector<bool> nodeIsVideo;
    bool havePrevious = false;
    int64_t previousTime100ns = 0;
    std::vector<metrics_policy::D3dkmtNodeRunningTimeSample> previousSamples;
};

}  // namespace

NvmlLoadApi LoadSystemNvmlLoadApi() {
    NvmlLoadApi api;
    HMODULE module = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        wchar_t programFiles[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(L"ProgramW6432", programFiles, MAX_PATH);
        if (length > 0 && length < MAX_PATH) {
            const std::wstring path = std::wstring(programFiles) + L"\\NVIDIA Corporation\\NVSMI\\nvml.dll";
            module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        }
    }
    if (!module)
        return api;
    api.module = module;
    ResolveProc(module, "nvmlInit_v2", &api.init);
    ResolveProc(module, "nvmlShutdown", &api.shutdown);
    ResolveProc(module, "nvmlDeviceGetCount_v2", &api.deviceGetCount);
    ResolveProc(module, "nvmlDeviceGetHandleByIndex_v2", &api.deviceGetHandleByIndex);
    ResolveProc(module, "nvmlDeviceGetName", &api.deviceGetName);
    ResolveProc(module, "nvmlDeviceGetPciInfo_v3", &api.deviceGetPciInfo);
    if (!api.deviceGetPciInfo)
        ResolveProc(module, "nvmlDeviceGetPciInfo_v2", &api.deviceGetPciInfo);
    ResolveProc(module, "nvmlDeviceGetUtilizationRates", &api.deviceGetUtilizationRates);
    ResolveProc(module, "nvmlDeviceGetSamples", &api.deviceGetSamples);
    return api;
}

D3dkmtLoadApi LoadSystemD3dkmtLoadApi() {
    D3dkmtLoadApi api;
    HMODULE gdi32 = GetModuleHandleW(L"gdi32.dll");
    if (!gdi32)
        gdi32 = ce::security::LoadSystemLibrary(L"gdi32.dll");
    if (!gdi32)
        return api;
    ResolveProc(gdi32, "D3DKMTOpenAdapterFromLuid", &api.openAdapterFromLuid);
    ResolveProc(gdi32, "D3DKMTCloseAdapter", &api.closeAdapter);
    ResolveProc(gdi32, "D3DKMTQueryAdapterInfo", &api.queryAdapterInfo);
    ResolveProc(gdi32, "D3DKMTQueryStatistics", &api.queryStatistics);
    return api;
}

unsigned int CountNonSoftwareDxgiAdapters() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory)
        return 0;
    unsigned int count = 0;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
        DXGI_ADAPTER_DESC1 desc{};
        if (adapter && SUCCEEDED(adapter->GetDesc1(&desc)) && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0)
            ++count;
        if (adapter)
            adapter->Release();
        adapter = nullptr;
    }
    factory->Release();
    return count;
}

struct GpuLoadSampler::Impl {
    bool useSystemLoaders = true;
    NvmlLoadApi nvml{};
    D3dkmtLoadApi kmt{};
    Clock100nsFn clock100ns;
    HardwareAdapterCountFn hardwareAdapterCount;

    bool backendsLoaded = false;
    bool nvmlInitialized = false;
    std::vector<NvmlDeviceEntry> nvmlDevices;

    int64_t matchedLuid = 0;
    int matchedNvmlIndex = -1;
    std::string matchedNvmlDetail;

    D3dkmtNodeState kmtState{};

    void EnsureBackendsLoaded() {
        if (backendsLoaded)
            return;
        backendsLoaded = true;
        if (useSystemLoaders) {
            nvml = LoadSystemNvmlLoadApi();
            kmt = LoadSystemD3dkmtLoadApi();
        }
        if (!clock100ns)
            clock100ns = &QueryMonotonic100ns;
        if (!hardwareAdapterCount)
            hardwareAdapterCount = &CountNonSoftwareDxgiAdapters;

        InitNvml();
    }

    void InitNvml() {
        if (!nvml.init || !nvml.shutdown || !nvml.deviceGetCount || !nvml.deviceGetHandleByIndex ||
            (!nvml.deviceGetUtilizationRates && !nvml.deviceGetSamples)) {
            ReleaseNvmlModule();
            return;
        }
        if (nvml.init() != kNvmlSuccess) {
            ReleaseNvmlModule();
            return;
        }
        nvmlInitialized = true;

        unsigned int count = 0;
        if (nvml.deviceGetCount(&count) != kNvmlSuccess || count == 0) {
            ShutdownNvml();
            return;
        }
        nvmlDevices.reserve(count);
        for (unsigned int i = 0; i < count; ++i) {
            void* handle = nullptr;
            if (nvml.deviceGetHandleByIndex(i, &handle) != kNvmlSuccess || !handle)
                continue;
            NvmlDeviceEntry entry;
            entry.handle = handle;
            char name[96] = "NVIDIA GPU";
            if (nvml.deviceGetName && nvml.deviceGetName(handle, name, sizeof(name)) == kNvmlSuccess)
                name[sizeof(name) - 1] = '\0';
            entry.name = name;
            if (nvml.deviceGetPciInfo) {
                NvmlPciInfo pci{};
                if (nvml.deviceGetPciInfo(handle, &pci) == kNvmlSuccess) {
                    entry.hasPci = true;
                    entry.domain = pci.domain;
                    entry.bus = pci.bus;
                    entry.device = pci.device;
                }
            }
            nvmlDevices.push_back(std::move(entry));
        }
        if (nvmlDevices.empty())
            ShutdownNvml();
    }

    void ReleaseNvmlModule() {
        if (nvml.module) {
            FreeLibrary(static_cast<HMODULE>(nvml.module));
            nvml.module = nullptr;
        }
    }

    void ShutdownNvml() {
        if (nvmlInitialized && nvml.shutdown)
            nvml.shutdown();
        nvmlInitialized = false;
        nvmlDevices.clear();
        matchedLuid = 0;
        matchedNvmlIndex = -1;
        matchedNvmlDetail.clear();
        ReleaseNvmlModule();
    }

    bool QueryAdapterPciAddress(int64_t adapterLuid, kmt_load_abi::AdapterAddress& address) const {
        address = {};
        if (!kmt.openAdapterFromLuid || !kmt.closeAdapter || !kmt.queryAdapterInfo || adapterLuid == 0)
            return false;
        kmt_load_abi::OpenAdapterFromLuid open{};
        open.adapterLuid = SplitLuid(adapterLuid);
        if (kmt.openAdapterFromLuid(&open) < 0 || open.adapter == 0)
            return false;

        kmt_load_abi::QueryAdapterInfo query{};
        query.adapter = open.adapter;
        query.type = kmt_load_abi::kQueryAdapterAddress;
        query.privateDriverData = &address;
        query.privateDriverDataSize = sizeof(address);
        const long status = kmt.queryAdapterInfo(&query);

        const kmt_load_abi::CloseAdapter close{open.adapter};
        kmt.closeAdapter(&close);
        if (status < 0)
            return false;
        // Software adapters (WARP) return sentinel all-ones bus/device numbers.
        return address.busNumber < kInvalidPciNumber && address.deviceNumber < kInvalidPciNumber;
    }

    void ResolveNvmlDeviceForLuid(int64_t adapterLuid) {
        if (matchedLuid == adapterLuid)
            return;
        matchedLuid = adapterLuid;
        matchedNvmlIndex = -1;
        matchedNvmlDetail.clear();
        if (!nvmlInitialized || nvmlDevices.empty() || adapterLuid == 0)
            return;

        kmt_load_abi::AdapterAddress address{};
        if (QueryAdapterPciAddress(adapterLuid, address)) {
            int matchIndex = -1;
            int matchCount = 0;
            for (size_t i = 0; i < nvmlDevices.size(); ++i) {
                if (nvmlDevices[i].hasPci && nvmlDevices[i].bus == address.busNumber &&
                    nvmlDevices[i].device == address.deviceNumber) {
                    matchIndex = static_cast<int>(i);
                    ++matchCount;
                }
            }
            if (matchCount == 1 && matchIndex >= 0) {
                matchedNvmlIndex = matchIndex;
                nvmlDevices[static_cast<size_t>(matchIndex)].lastSampleTimestampUs = 0;
                char detail[160]{};
                std::snprintf(detail, sizeof(detail), "pci=%04x:%02x:%02x.%u device=%s",
                              nvmlDevices[static_cast<size_t>(matchIndex)].domain, address.busNumber,
                              address.deviceNumber, address.functionNumber,
                              nvmlDevices[static_cast<size_t>(matchIndex)].name.c_str());
                matchedNvmlDetail = detail;
                return;
            }
            // PCI address was validly resolved and did NOT match any NVIDIA GPU
            // (for example, the game is rendering on an AMD/Intel iGPU).
            return;
        }

        // Single-GPU fallback when PCI address query is unavailable.
        if (nvmlDevices.size() == 1 && hardwareAdapterCount && hardwareAdapterCount() == 1) {
            matchedNvmlIndex = 0;
            nvmlDevices[0].lastSampleTimestampUs = 0;
            matchedNvmlDetail = "single-gpu device=" + nvmlDevices[0].name;
        }
    }

    GpuLoadReading SampleNvml(int64_t adapterLuid) {
        GpuLoadReading reading;
        ResolveNvmlDeviceForLuid(adapterLuid);
        if (matchedNvmlIndex < 0 || static_cast<size_t>(matchedNvmlIndex) >= nvmlDevices.size())
            return reading;

        NvmlDeviceEntry& device = nvmlDevices[static_cast<size_t>(matchedNvmlIndex)];

        // Prefer hardware 200ms PMU sample integration across the elapsed poll
        // interval so the reading reflects the entire interval with no aliasing.
        if (nvml.deviceGetSamples && device.samplesSupported) {
            int valueType = 0;
            unsigned int sampleCount = 0;
            const int countStatus = nvml.deviceGetSamples(device.handle, kNvmlGpuUtilizationSamples,
                                                          device.lastSampleTimestampUs, &valueType, &sampleCount,
                                                          nullptr);
            if (countStatus == kNvmlSuccess && sampleCount > 0) {
                std::vector<NvmlSample> raw(sampleCount);
                unsigned int fetchedCount = sampleCount;
                if (nvml.deviceGetSamples(device.handle, kNvmlGpuUtilizationSamples, device.lastSampleTimestampUs,
                                          &valueType, &fetchedCount, raw.data()) == kNvmlSuccess &&
                    fetchedCount > 0) {
                    const size_t usableCount = (std::min<size_t>)(fetchedCount, raw.size());
                    std::vector<metrics_policy::NvmlUtilSample> samples;
                    samples.reserve(usableCount);
                    for (size_t i = 0; i < usableCount; ++i) {
                        samples.push_back({static_cast<uint64_t>(raw[i].timeStamp), raw[i].sampleValue.uiVal});
                    }
                    const metrics_policy::NvmlSampleIntegration integrated =
                        metrics_policy::IntegrateNvmlGpuSamples(samples, device.lastSampleTimestampUs);
                    if (integrated.valid) {
                        device.lastSampleTimestampUs = integrated.newestTimestampUs;
                        reading.source = metrics_policy::GpuLoadSource::NvmlSamples;
                        reading.percent = integrated.percent;
                        reading.valid = true;
                        reading.detail = matchedNvmlDetail;
                        return reading;
                    }
                }
            } else if (countStatus == 3 /* NVML_ERROR_NOT_SUPPORTED */) {
                device.samplesSupported = false;
            }
        }

        if (nvml.deviceGetUtilizationRates) {
            NvmlUtilization util{};
            if (nvml.deviceGetUtilizationRates(device.handle, &util) == kNvmlSuccess) {
                reading.source = metrics_policy::GpuLoadSource::NvmlRate;
                reading.percent = std::clamp(static_cast<double>(util.gpu), 0.0, 100.0);
                reading.valid = true;
                reading.detail = matchedNvmlDetail;
                return reading;
            }
        }
        return reading;
    }

    void EnsureD3dkmtAdapterProbed(int64_t adapterLuid) {
        if (kmtState.probed && kmtState.adapterLuid == adapterLuid)
            return;
        kmtState = {};
        kmtState.adapterLuid = adapterLuid;
        kmtState.probed = true;
        if (!kmt.queryStatistics || !kmt.openAdapterFromLuid || !kmt.closeAdapter || !kmt.queryAdapterInfo ||
            adapterLuid == 0) {
            return;
        }

        kmt_load_abi::QueryStatistics adapterQuery{};
        adapterQuery.type = kmt_load_abi::kQueryStatsAdapter;
        adapterQuery.adapterLuid = SplitLuid(adapterLuid);
        if (kmt.queryStatistics(&adapterQuery) < 0)
            return;
        const uint32_t nodeCount = adapterQuery.queryResult.adapterInfo.nodeCount;
        if (nodeCount == 0 || nodeCount > kMaxReasonableNodeCount)
            return;

        kmt_load_abi::OpenAdapterFromLuid open{};
        open.adapterLuid = SplitLuid(adapterLuid);
        if (kmt.openAdapterFromLuid(&open) < 0 || open.adapter == 0)
            return;

        kmtState.nodeCount = nodeCount;
        kmtState.nodeIsVideo.assign(nodeCount, false);
        uint32_t metadataSuccesses = 0;
        uint32_t nonVideoCount = 0;
        for (uint32_t node = 0; node < nodeCount; ++node) {
            kmt_load_abi::NodeMetadata metadata{};
            metadata.nodeOrdinalAndAdapterIndex = node;
            kmt_load_abi::QueryAdapterInfo query{};
            query.adapter = open.adapter;
            query.type = kmt_load_abi::kQueryNodeMetadata;
            query.privateDriverData = &metadata;
            query.privateDriverDataSize = sizeof(metadata);
            if (kmt.queryAdapterInfo(&query) >= 0) {
                ++metadataSuccesses;
                const bool isVideo =
                    metrics_policy::IsD3dkmtVideoEngineNode(metadata.engineType, NodeFriendlyNameView(metadata.friendlyName));
                kmtState.nodeIsVideo[node] = isVideo;
                if (!isVideo)
                    ++nonVideoCount;
            } else {
                // Without metadata for this node, avoid accidentally folding a
                // video engine into overlay load.
                kmtState.nodeIsVideo[node] = true;
            }
        }
        const kmt_load_abi::CloseAdapter close{open.adapter};
        kmt.closeAdapter(&close);

        kmtState.nonVideoNodeCount = nonVideoCount;
        kmtState.usable = metadataSuccesses > 0 && nonVideoCount > 0;
    }

    GpuLoadReading SampleD3dkmt(int64_t adapterLuid) {
        GpuLoadReading reading;
        EnsureD3dkmtAdapterProbed(adapterLuid);
        if (!kmtState.usable || !kmt.queryStatistics)
            return reading;

        const int64_t now100ns = clock100ns ? clock100ns() : 0;
        std::vector<metrics_policy::D3dkmtNodeRunningTimeSample> current(kmtState.nodeCount);
        for (uint32_t node = 0; node < kmtState.nodeCount; ++node) {
            current[node].videoEngine = kmtState.nodeIsVideo[node];
            if (kmtState.nodeIsVideo[node])
                continue;
            kmt_load_abi::QueryStatistics query{};
            query.type = kmt_load_abi::kQueryStatsNode;
            query.adapterLuid = SplitLuid(adapterLuid);
            query.queryNodeId = node;
            if (kmt.queryStatistics(&query) >= 0) {
                current[node].runningTime100ns = query.queryResult.nodeInfo.runningTime;
                current[node].valid = true;
            }
        }

        if (!kmtState.havePrevious) {
            kmtState.previousSamples = std::move(current);
            kmtState.previousTime100ns = now100ns;
            kmtState.havePrevious = true;
            return reading;
        }

        const int64_t elapsed100ns = now100ns - kmtState.previousTime100ns;
        const metrics_policy::D3dkmtLoadResult delta =
            metrics_policy::ComputeD3dkmtAdapterLoadPercent(kmtState.previousSamples, current, elapsed100ns);
        kmtState.previousSamples = std::move(current);
        kmtState.previousTime100ns = now100ns;

        if (!delta.valid)
            return reading;

        reading.source = metrics_policy::GpuLoadSource::D3dkmtNode;
        reading.percent = delta.percent;
        reading.valid = true;
        char detail[96]{};
        std::snprintf(detail, sizeof(detail), "nodes=%u nonVideo=%u", kmtState.nodeCount, kmtState.nonVideoNodeCount);
        reading.detail = detail;
        return reading;
    }
};

GpuLoadSampler::GpuLoadSampler() : impl_(std::make_unique<Impl>()) {}

GpuLoadSampler::GpuLoadSampler(NvmlLoadApi nvmlApi, D3dkmtLoadApi d3dkmtApi, Clock100nsFn clock100ns,
                               HardwareAdapterCountFn hardwareAdapterCount)
    : impl_(std::make_unique<Impl>()) {
    impl_->useSystemLoaders = false;
    impl_->nvml = nvmlApi;
    impl_->kmt = d3dkmtApi;
    impl_->clock100ns = std::move(clock100ns);
    impl_->hardwareAdapterCount = std::move(hardwareAdapterCount);
}

GpuLoadSampler::~GpuLoadSampler() {
    if (impl_)
        impl_->ShutdownNvml();
}

GpuLoadReading GpuLoadSampler::SampleAdapter(int64_t adapterLuid) {
    if (!impl_ || adapterLuid == 0)
        return {};
    impl_->EnsureBackendsLoaded();
    if (GpuLoadReading nvmlReading = impl_->SampleNvml(adapterLuid); nvmlReading.valid)
        return nvmlReading;
    return impl_->SampleD3dkmt(adapterLuid);
}

void GpuLoadSampler::Reset() {
    if (!impl_)
        return;
    impl_->ShutdownNvml();
    impl_->kmtState = {};
    impl_->backendsLoaded = false;
}

}  // namespace scan_host
