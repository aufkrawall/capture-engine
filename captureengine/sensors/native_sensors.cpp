// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "native_sensors.h"

#include "common/config/config.h"
#include "common/logging/logging.h"

#include <windows.h>
#include <dxgi.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <utility>

namespace ce::hardware_sensors {
namespace {

constexpr int kNvmlSuccess = 0;
constexpr int kNvmlTemperatureGpu = 0;
constexpr int kNvmlClockGraphics = 0;
constexpr int kNvmlClockMemory = 2;

constexpr size_t kGpuTemperature = 1;
constexpr size_t kGpuPackagePower = 3;
constexpr size_t kCpuCoreClock = policy::kCpuCoreClockMetric;
constexpr size_t kGpuCoreClock = 6;
constexpr size_t kGpuMemoryClock = 7;
constexpr size_t kCpuMaxCoreClock = policy::kCpuMaxCoreClockMetric;
static_assert(policy::kMetricCount <= 32, "MetricMask holds one bit per metric");

SensorValue& ValueOf(HardwareSensorSnapshot& snapshot, size_t metric) {
    switch (metric) {
        case 0:
            return snapshot.cpuTemperature;
        case 1:
            return snapshot.gpuTemperature;
        case 2:
            return snapshot.cpuPackagePower;
        case 3:
            return snapshot.gpuPackagePower;
        case 4:
            return snapshot.gpuFan;
        case 5:
            return snapshot.cpuCoreClock;
        case 6:
            return snapshot.gpuCoreClock;
        case 7:
            return snapshot.gpuMemoryClock;
        case 8:
            return snapshot.gpuVoltage;
        default:
            return snapshot.cpuMaxCoreClock;
    }
}

// The selector a metric follows; the fastest core has none of its own and follows the average's.
std::string& SelectorOf(HardwareSensorsConfig& config, size_t metric) {
    switch (metric) {
        case 0:
            return config.cpuTemperature;
        case 1:
            return config.gpuTemperature;
        case 2:
            return config.cpuPackagePower;
        case 3:
            return config.gpuPackagePower;
        case 4:
            return config.gpuFan;
        case 6:
            return config.gpuCoreClock;
        case 7:
            return config.gpuMemoryClock;
        case 8:
            return config.gpuVoltage;
        default:
            return config.cpuCoreClock;
    }
}

void SetValue(SensorValue& value, float reading, const std::string& identifier) {
    value.value = reading;
    value.valid = true;
    value.identifier = identifier;
}

class NvmlProvider final : public NativeSensorProvider {
public:
    NvmlProvider(NvmlApi api, void* device, const std::string& name, MetricMask metrics)
        : api_(api), device_(device), identifier_("native:nvml:" + name), metrics_(metrics) {}
    ~NvmlProvider() override {
        api_.shutdown();
        if (api_.module)
            FreeLibrary(static_cast<HMODULE>(api_.module));
    }
    const char* Name() const override { return "nvml"; }
    MetricMask Metrics() const override { return metrics_; }
    void Sample(HardwareSensorSnapshot& snapshot) override {
        unsigned int value = 0;
        if ((metrics_ & MetricBit(kGpuTemperature)) &&
            api_.deviceGetTemperature(device_, kNvmlTemperatureGpu, &value) == kNvmlSuccess)
            SetValue(snapshot.gpuTemperature, static_cast<float>(value), identifier_);
        if ((metrics_ & MetricBit(kGpuPackagePower)) && api_.deviceGetPowerUsage(device_, &value) == kNvmlSuccess)
            SetValue(snapshot.gpuPackagePower, static_cast<float>(value) / 1000.0f, identifier_);  // mW -> W
        if ((metrics_ & MetricBit(kGpuCoreClock)) &&
            api_.deviceGetClockInfo(device_, kNvmlClockGraphics, &value) == kNvmlSuccess)
            SetValue(snapshot.gpuCoreClock, static_cast<float>(value), identifier_);
        if ((metrics_ & MetricBit(kGpuMemoryClock)) &&
            api_.deviceGetClockInfo(device_, kNvmlClockMemory, &value) == kNvmlSuccess)
            SetValue(snapshot.gpuMemoryClock, static_cast<float>(value), identifier_);
    }

private:
    NvmlApi api_;
    void* device_;
    std::string identifier_;
    MetricMask metrics_;
};

class CpuClockProvider final : public NativeSensorProvider {
public:
    explicit CpuClockProvider(std::function<CpuClockSample()> read) : read_(std::move(read)) {}
    const char* Name() const override { return "pdh"; }
    MetricMask Metrics() const override { return MetricBit(kCpuCoreClock) | MetricBit(kCpuMaxCoreClock); }
    void Sample(HardwareSensorSnapshot& snapshot) override {
        const CpuClockSample sample = read_();
        if (!sample.valid)
            return;
        SetValue(snapshot.cpuCoreClock, sample.averageMHz, "native:pdh:average");
        if (sample.maximumMHz > 0.0f)
            SetValue(snapshot.cpuMaxCoreClock, sample.maximumMHz, "native:pdh:maximum");
    }

private:
    std::function<CpuClockSample()> read_;
};

// The efficiency class of every logical processor, keyed by group and number.
std::vector<LogicalProcessorPerformance> LogicalProcessors() {
    std::vector<LogicalProcessorPerformance> processors;
    ULONG bytes = 0;
    GetSystemCpuSetInformation(nullptr, 0, &bytes, GetCurrentProcess(), 0);
    if (bytes == 0)
        return processors;
    std::vector<BYTE> buffer(bytes);
    if (!GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()), bytes, &bytes,
                                    GetCurrentProcess(), 0))
        return processors;
    for (ULONG offset = 0; offset + sizeof(DWORD) * 2 <= bytes;) {
        const auto* entry = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
        if (entry->Size == 0)
            break;
        if (entry->Type == CpuSetInformation) {
            LogicalProcessorPerformance processor;
            processor.group = entry->CpuSet.Group;
            processor.number = entry->CpuSet.LogicalProcessorIndex;
            processor.efficiencyClass = entry->CpuSet.EfficiencyClass;
            processors.push_back(processor);
        }
        offset += entry->Size;
    }
    return processors;
}

class PdhCpuClocks {
public:
    PdhCpuClocks() : processors_(LogicalProcessors()) {
        if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) {
            query_ = nullptr;
            return;
        }
        if (PdhAddEnglishCounterW(query_, L"\\Processor Information(*)\\% Processor Performance", 0, &performance_) !=
                ERROR_SUCCESS ||
            PdhAddEnglishCounterW(query_, L"\\Processor Information(_Total)\\Processor Frequency", 0, &frequency_) !=
                ERROR_SUCCESS) {
            PdhCloseQuery(query_);
            query_ = nullptr;
            return;
        }
        PdhCollectQueryData(query_);  // the rate counter's first sample
    }
    ~PdhCpuClocks() {
        if (query_)
            PdhCloseQuery(query_);
    }
    explicit operator bool() const { return query_ != nullptr; }
    CpuClockSample Read() {
        if (!query_ || PdhCollectQueryData(query_) != ERROR_SUCCESS)
            return {};
        PDH_FMT_COUNTERVALUE frequency{};
        if (PdhGetFormattedCounterValue(frequency_, PDH_FMT_DOUBLE, nullptr, &frequency) != ERROR_SUCCESS)
            return {};
        DWORD bytes = 0;
        DWORD count = 0;
        const PDH_STATUS sizing = PdhGetFormattedCounterArrayW(performance_, PDH_FMT_DOUBLE, &bytes, &count, nullptr);
        if (sizing != static_cast<PDH_STATUS>(PDH_MORE_DATA))
            return {};
        std::vector<BYTE> buffer(bytes);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        if (PdhGetFormattedCounterArrayW(performance_, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS)
            return {};
        std::vector<LogicalProcessorPerformance> readings;
        readings.reserve(count);
        for (DWORD i = 0; i < count; ++i) {
            LogicalProcessorPerformance reading;
            if (!ParseProcessorInstance(items[i].szName, &reading.group, &reading.number))
                continue;
            for (const LogicalProcessorPerformance& known : processors_) {
                if (known.group == reading.group && known.number == reading.number)
                    reading.efficiencyClass = known.efficiencyClass;
            }
            reading.performancePercent = items[i].FmtValue.doubleValue;
            readings.push_back(reading);
        }
        return ReduceCpuClocks(frequency.doubleValue, readings);
    }

private:
    std::vector<LogicalProcessorPerformance> processors_;
    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER performance_ = nullptr;
    PDH_HCOUNTER frequency_ = nullptr;
};

template <typename T>
void Resolve(HMODULE module, const char* name, T* out) {
    *out = reinterpret_cast<T>(GetProcAddress(module, name));
}

}  // namespace

NvmlApi LoadSystemNvml() {
    NvmlApi api;
    // The driver installs nvml.dll in System32; older drivers only below Program Files (NVSMI). Full paths only.
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
    Resolve(module, "nvmlInit_v2", &api.init);
    Resolve(module, "nvmlShutdown", &api.shutdown);
    Resolve(module, "nvmlDeviceGetCount_v2", &api.deviceGetCount);
    Resolve(module, "nvmlDeviceGetHandleByIndex_v2", &api.deviceGetHandleByIndex);
    Resolve(module, "nvmlDeviceGetName", &api.deviceGetName);
    Resolve(module, "nvmlDeviceGetTemperature", &api.deviceGetTemperature);
    Resolve(module, "nvmlDeviceGetPowerUsage", &api.deviceGetPowerUsage);
    Resolve(module, "nvmlDeviceGetClockInfo", &api.deviceGetClockInfo);
    return api;
}

unsigned int CountHardwareAdapters() {
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

std::unique_ptr<NativeSensorProvider> OpenNvmlProvider(const NvmlApi& api, unsigned int hardwareAdapters) {
    const auto release = [&api]() {
        if (api.module)
            FreeLibrary(static_cast<HMODULE>(api.module));
    };
    if (!api.init || !api.shutdown || !api.deviceGetCount || !api.deviceGetHandleByIndex ||
        !api.deviceGetTemperature || !api.deviceGetPowerUsage || !api.deviceGetClockInfo) {
        release();
        return nullptr;
    }
    if (api.init() != kNvmlSuccess) {
        release();
        return nullptr;
    }
    unsigned int count = 0;
    void* device = nullptr;
    if (api.deviceGetCount(&count) != kNvmlSuccess || count != 1 || hardwareAdapters != 1 ||
        api.deviceGetHandleByIndex(0, &device) != kNvmlSuccess || !device) {
        if (count != 0)
            LogInfo("[Sensors] NVML not used: nvidia_gpus=%u hardware_gpus=%u (the bridge chooses among several)",
                    count, hardwareAdapters);
        api.shutdown();
        release();
        return nullptr;
    }
    char name[96] = "GPU 0";
    if (api.deviceGetName)
        api.deviceGetName(device, name, sizeof(name));
    name[sizeof(name) - 1] = '\0';
    // A metric the driver refuses at open is left to the bridge.
    MetricMask metrics = 0;
    unsigned int probe = 0;
    if (api.deviceGetTemperature(device, kNvmlTemperatureGpu, &probe) == kNvmlSuccess)
        metrics |= MetricBit(kGpuTemperature);
    if (api.deviceGetPowerUsage(device, &probe) == kNvmlSuccess)
        metrics |= MetricBit(kGpuPackagePower);
    if (api.deviceGetClockInfo(device, kNvmlClockGraphics, &probe) == kNvmlSuccess)
        metrics |= MetricBit(kGpuCoreClock);
    if (api.deviceGetClockInfo(device, kNvmlClockMemory, &probe) == kNvmlSuccess)
        metrics |= MetricBit(kGpuMemoryClock);
    if (metrics == 0) {
        LogInfo("[Sensors] NVML not used: device='%s' refuses every metric", name);
        api.shutdown();
        release();
        return nullptr;
    }
    LogInfo("[Sensors] NVML opened: device='%s' metrics=0x%x", name, metrics);
    return std::make_unique<NvmlProvider>(api, device, name, metrics);
}

bool ParseProcessorInstance(const wchar_t* name, uint16_t* group, uint8_t* number) {
    if (!name || !iswdigit(name[0]))
        return false;
    wchar_t* end = nullptr;
    const unsigned long groupValue = wcstoul(name, &end, 10);
    if (!end || *end != L',' || !iswdigit(end[1]) || groupValue > 0xFFFF)
        return false;
    const wchar_t* numberText = end + 1;
    const unsigned long numberValue = wcstoul(numberText, &end, 10);
    if (!end || *end != L'\0' || numberValue > 0xFF)
        return false;
    *group = static_cast<uint16_t>(groupValue);
    *number = static_cast<uint8_t>(numberValue);
    return true;
}

CpuClockSample ReduceCpuClocks(double nominalMHz, const std::vector<LogicalProcessorPerformance>& processors) {
    CpuClockSample sample;
    if (nominalMHz <= 0.0 || processors.empty())
        return sample;
    uint8_t fastestClass = 0;
    uint8_t slowestClass = 0xFF;
    for (const LogicalProcessorPerformance& processor : processors) {
        fastestClass = std::max(fastestClass, processor.efficiencyClass);
        slowestClass = std::min(slowestClass, processor.efficiencyClass);
    }
    double total = 0.0;
    size_t averaged = 0;
    double maximum = 0.0;
    for (const LogicalProcessorPerformance& processor : processors) {
        const double mhz = nominalMHz * processor.performancePercent / 100.0;
        maximum = std::max(maximum, mhz);
        // A hybrid CPU averages its performance cores only, like the bridge's P-core average.
        if (fastestClass != slowestClass && processor.efficiencyClass != fastestClass)
            continue;
        total += mhz;
        ++averaged;
    }
    if (averaged == 0 || total <= 0.0)
        return sample;
    sample.averageMHz = static_cast<float>(total / static_cast<double>(averaged));
    sample.maximumMHz = static_cast<float>(maximum);
    sample.valid = true;
    return sample;
}

std::unique_ptr<NativeSensorProvider> OpenCpuClockProvider(std::function<CpuClockSample()> read) {
    if (read)
        return std::make_unique<CpuClockProvider>(std::move(read));
    auto clocks = std::make_shared<PdhCpuClocks>();
    if (!*clocks) {
        LogInfo("[Sensors] CPU clock counters unavailable; the bridge reads CPU clocks");
        return nullptr;
    }
    return std::make_unique<CpuClockProvider>([clocks]() { return clocks->Read(); });
}

SourcePlan PlanSources(const HardwareSensorsConfig& config, const std::vector<MetricMask>& providerMetrics,
                       HardwareSensorsConfig* bridgeSelectors) {
    SourcePlan plan;
    plan.provider.fill(-1);
    HardwareSensorsConfig requested = config;
    HardwareSensorsConfig bridge = config;
    const bool enabled = config.enabled != "off";
    for (size_t metric = 0; metric < policy::kMetricCount; ++metric) {
        const std::string& selector = SelectorOf(requested, metric);
        MetricSource source = MetricSource::Off;
        if (enabled && selector != "off") {
            int provider = -1;
            // Only "auto" may be served natively: an exact LibreHardwareMonitor identifier names a bridge sensor.
            if (config.backend != "lhm" && selector == "auto") {
                for (size_t i = 0; i < providerMetrics.size(); ++i) {
                    if (providerMetrics[i] & MetricBit(metric)) {
                        provider = static_cast<int>(i);
                        break;
                    }
                }
            }
            if (provider >= 0) {
                source = MetricSource::Native;
                plan.provider[metric] = provider;
            } else {
                source = config.backend == "native" ? MetricSource::Unavailable : MetricSource::Bridge;
            }
        }
        plan.sources[metric] = source;
        if (source == MetricSource::Bridge)
            plan.bridgeNeeded = true;
    }
    // The bridge keeps a selector only for a metric it serves; the CPU clock selector serves the average and the
    // fastest core alike.
    for (size_t metric = 0; metric < policy::kSelectorCount; ++metric) {
        bool bridged = plan.sources[metric] == MetricSource::Bridge;
        if (metric == kCpuCoreClock)
            bridged = bridged || plan.sources[kCpuMaxCoreClock] == MetricSource::Bridge;
        if (!bridged)
            SelectorOf(bridge, metric) = "off";
    }
    if (bridgeSelectors)
        *bridgeSelectors = bridge;
    return plan;
}

std::string DescribeSourcePlan(const SourcePlan& plan, const std::vector<std::string>& providerNames) {
    std::string text;
    for (size_t metric = 0; metric < policy::kMetricCount; ++metric) {
        if (!text.empty())
            text += ' ';
        text += policy::kMetrics[metric].key;
        text += '=';
        switch (plan.sources[metric]) {
            case MetricSource::Off:
                text += "off";
                break;
            case MetricSource::Native: {
                const int provider = plan.provider[metric];
                text += provider >= 0 && static_cast<size_t>(provider) < providerNames.size() ? providerNames[provider]
                                                                                              : "native";
                break;
            }
            case MetricSource::Bridge:
                text += "lhm";
                break;
            case MetricSource::Unavailable:
                text += "unavailable";
                break;
        }
    }
    return text;
}

struct HardwareSensorSources::Impl {
    HardwareSensorsConfig config;
    ProviderFactory factory;
    std::vector<std::unique_ptr<NativeSensorProvider>> providers;
    std::unique_ptr<LibreHardwareMonitorPlugin> bridge;
    mutable uint32_t sequence = 0;
};

HardwareSensorSources::HardwareSensorSources(const HardwareSensorsConfig& config, ProviderFactory factory)
    : impl_(std::make_unique<Impl>()) {
    impl_->config = config;
    impl_->factory = std::move(factory);
}

HardwareSensorSources::~HardwareSensorSources() = default;

bool HardwareSensorSources::Start() {
    if (impl_->config.enabled != "off" && impl_->config.backend != "lhm") {
        if (impl_->factory) {
            impl_->providers = impl_->factory();
        } else {
            if (auto nvml = OpenNvmlProvider(LoadSystemNvml(), CountHardwareAdapters()))
                impl_->providers.push_back(std::move(nvml));
            if (auto cpu = OpenCpuClockProvider())
                impl_->providers.push_back(std::move(cpu));
        }
    }
    std::vector<MetricMask> masks;
    std::vector<std::string> names;
    for (const auto& provider : impl_->providers) {
        masks.push_back(provider ? provider->Metrics() : 0);
        names.emplace_back(provider ? provider->Name() : "-");
    }
    HardwareSensorsConfig bridgeSelectors;
    plan_ = PlanSources(impl_->config, masks, &bridgeSelectors);
    LogInfo("[Sensors] sources (backend=%s): %s", impl_->config.backend.c_str(),
            DescribeSourcePlan(plan_, names).c_str());
    bool bridgeActive = false;
    if (plan_.bridgeNeeded) {
        impl_->bridge = std::make_unique<LibreHardwareMonitorPlugin>(bridgeSelectors);
        bridgeActive = impl_->bridge->Start();
    }
    const bool nativeActive = std::any_of(plan_.sources.begin(), plan_.sources.end(),
                                          [](MetricSource source) { return source == MetricSource::Native; });
    return nativeActive || bridgeActive;
}

void HardwareSensorSources::Poll() {
    if (impl_->bridge)
        impl_->bridge->Poll();
}

HardwareSensorSnapshot HardwareSensorSources::GetSnapshot() const {
    HardwareSensorSnapshot snapshot = impl_->bridge ? impl_->bridge->GetSnapshot() : HardwareSensorSnapshot{};
    bool nativeValue = false;
    for (size_t index = 0; index < impl_->providers.size(); ++index) {
        NativeSensorProvider* provider = impl_->providers[index].get();
        if (!provider)
            continue;
        HardwareSensorSnapshot sampled;
        provider->Sample(sampled);
        for (size_t metric = 0; metric < policy::kMetricCount; ++metric) {
            if (plan_.sources[metric] != MetricSource::Native || plan_.provider[metric] != static_cast<int>(index))
                continue;
            ValueOf(snapshot, metric) = ValueOf(sampled, metric);
            nativeValue = nativeValue || ValueOf(sampled, metric).valid;
        }
    }
    // Native values are read now; without the bridge their snapshot carries its own sequence and time.
    if (nativeValue && !impl_->bridge) {
        snapshot.sequence = ++impl_->sequence;
        snapshot.receivedTickMs = GetTickCount64();
    }
    return snapshot;
}

}  // namespace ce::hardware_sensors
