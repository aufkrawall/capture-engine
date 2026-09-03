#include "sensor_bridge_lhm.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "clr_interop.h"

namespace ce::hardware_sensors {
namespace {

using policy::GpuLoadCandidate;
using policy::kMetricCount;
using policy::kMetrics;
using policy::kNoSelection;
using policy::MetricScope;
using policy::SensorCandidate;

constexpr wchar_t kLibraryFileName[] = L"LibreHardwareMonitorLib.dll";
constexpr wchar_t kComputerTypeName[] = L"LibreHardwareMonitor.Hardware.Computer";
constexpr wchar_t kSensorTypeEnumName[] = L"LibreHardwareMonitor.Hardware.SensorType";
constexpr wchar_t kHardwareTypeEnumName[] = L"LibreHardwareMonitor.Hardware.HardwareType";
constexpr wchar_t kHardwareEventTypeName[] = L"LibreHardwareMonitor.Hardware.HardwareEventHandler";
constexpr char kGpuCoreLoadSensorName[] = "GPU Core";

// Depth guard for a hardware tree that is two levels deep in practice; it only
// exists so a malformed or cyclic tree cannot recurse without bound.
constexpr int kMaximumHardwareDepth = 8;

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty())
        return {};
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                             static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0)
        return {};
    std::wstring result(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                            result.data(), required) != required) {
        return {};
    }
    return result;
}

struct TypedSensor {
    int32_t sensorType = 0;
    SensorCandidate candidate;
};

struct HardwareRoot {
    clr::Object object;
    std::string identifier;
    int32_t hardwareType = 0;
    bool isGpu = false;
    bool isCpu = false;
};

}  // namespace

struct LibreHardwareMonitorSession::Impl {
    clr::Host host;
    clr::Object computer;
    clr::Object addedQueue;
    clr::Object removedQueue;
    clr::Type assemblyType;
    clr::Value libraryAssembly;
    std::string libraryVersion;

    // Enum ordinals resolved from the loaded library's own metadata, never
    // hardcoded: LibreHardwareMonitor is free to renumber them between releases.
    int32_t sensorTypeValues[kMetricCount] = {};
    int32_t loadSensorType = 0;
    bool loadSensorTypeKnown = false;
    int32_t cpuHardwareType = 0;
    bool cpuHardwareTypeKnown = false;
    std::vector<int32_t> gpuHardwareTypes;

    BridgeSelectors selectors;
    bool metricRequested[kMetricCount] = {};
    std::string previousIdentifiers[kMetricCount];
    std::string activeGpuIdentifier;
    std::vector<HardwareRoot> roots;
    bool opened = false;

    bool ResolveEnum(const wchar_t* enumTypeName, clr::Type& enumType, clr::Value& enumTypeValue);
    bool ResolveEnumMember(const clr::Type& enumType, const clr::Value& enumTypeValue, const std::wstring& member,
                           int32_t& value);
    bool ResolveEnumNames(const clr::Type& enumType, const clr::Value& enumTypeValue,
                          std::vector<std::string>& names);
    bool SubscribeHardwareEvents(std::string& failureToken);
    void DrainHardwareEvents();
    void UpdateTree(const clr::Object& hardware, int depth);
    void CollectSensors(const clr::Object& hardware, int depth, std::vector<TypedSensor>& sensors);
    bool NeedsSensorType(int32_t sensorType) const;
};

namespace {

// Reads one LibreHardwareMonitor sensor into the policy's flat candidate form.
// The caller has already resolved and filtered on SensorType.
bool ReadSensor(const clr::Object& sensor, SensorCandidate& candidate) {
    candidate.name = clr::ToText(sensor.Get(L"Name"));
    candidate.identifier = clr::ToText(sensor.Get(L"Identifier"));
    const clr::Value reading = sensor.Get(L"Value");
    double numeric = 0.0;
    // ISensor.Value is a nullable float: an unpopulated rail arrives as a
    // managed null, which marshals to an empty VARIANT rather than a zero.
    candidate.hasValue = !reading.IsEmpty() && reading.AsDouble(numeric);
    candidate.value = candidate.hasValue ? static_cast<float>(numeric) : 0.0f;
    return !candidate.identifier.empty();
}

std::vector<SensorCandidate> FilterByType(const std::vector<TypedSensor>& sensors, int32_t sensorType) {
    std::vector<SensorCandidate> filtered;
    for (const TypedSensor& sensor : sensors) {
        if (sensor.sensorType == sensorType)
            filtered.push_back(sensor.candidate);
    }
    return filtered;
}

}  // namespace

bool LibreHardwareMonitorSession::Impl::ResolveEnum(const wchar_t* enumTypeName, clr::Type& enumType,
                                                    clr::Value& enumTypeValue) {
    const clr::Value name = clr::Value::FromString(enumTypeName);
    HRESULT status = E_FAIL;
    enumTypeValue = assemblyType.Invoke(L"GetType", clr::kBindInvokeMethod | clr::kBindPublic | clr::kBindInstance,
                                        libraryAssembly, &name, 1, &status);
    if (FAILED(status) || !enumTypeValue.IsObject())
        return false;
    enumType = clr::Type::FromValue(enumTypeValue);
    return enumType.Valid();
}

bool LibreHardwareMonitorSession::Impl::ResolveEnumMember(const clr::Type& enumType, const clr::Value& enumTypeValue,
                                                          const std::wstring& member, int32_t& value) {
    if (!enumType.Valid())
        return false;
    clr::Value arguments[2] = {enumTypeValue.Clone(), clr::Value::FromString(member)};
    HRESULT status = E_FAIL;
    // Enum.Parse is a static on System.Enum, so reaching it from the concrete
    // enum type requires FlattenHierarchy.
    const clr::Value parsed =
        enumType.Invoke(L"Parse", clr::kBindInvokeMethod | clr::kBindPublic | clr::kBindStatic |
                                      clr::kBindFlattenHierarchy,
                        clr::Value(), arguments, 2, &status);
    return SUCCEEDED(status) && parsed.AsInt32(value);
}

bool LibreHardwareMonitorSession::Impl::ResolveEnumNames(const clr::Type& enumType, const clr::Value& enumTypeValue,
                                                         std::vector<std::string>& names) {
    if (!enumType.Valid())
        return false;
    clr::Value arguments[1] = {enumTypeValue.Clone()};
    HRESULT status = E_FAIL;
    const clr::Value result =
        enumType.Invoke(L"GetNames", clr::kBindInvokeMethod | clr::kBindPublic | clr::kBindStatic |
                                         clr::kBindFlattenHierarchy,
                        clr::Value(), arguments, 1, &status);
    if (FAILED(status) || !result.IsArray())
        return false;
    for (size_t index = 0; index < result.ArraySize(); ++index) {
        std::string name;
        if (result.ArrayString(index, name))
            names.push_back(std::move(name));
    }
    return !names.empty();
}

// The root hardware list is IList<IHardware>, and the runtime refuses to
// marshal a constructed generic type to COM. The public HardwareAdded and
// HardwareRemoved events carry the same roots one IHardware at a time, so the
// bridge binds each event to a System.Collections.Queue and drains it. That is
// also strictly better than re-reading a list: hot-plugged hardware appears and
// disappears through the same path.
bool LibreHardwareMonitorSession::Impl::SubscribeHardwareEvents(std::string& failureToken) {
    const clr::Type& reflection = host.Reflection();
    struct Subscription {
        const wchar_t* eventMember;
        clr::Object* queue;
    };
    const Subscription subscriptions[] = {{L"add_HardwareAdded", &addedQueue},
                                          {L"add_HardwareRemoved", &removedQueue}};

    const clr::Value handlerName = clr::Value::FromString(kHardwareEventTypeName);
    HRESULT status = E_FAIL;
    const clr::Value handlerTypeValue =
        assemblyType.Invoke(L"GetType", clr::kBindInvokeMethod | clr::kBindPublic | clr::kBindInstance,
                            libraryAssembly, &handlerName, 1, &status);
    const clr::Type handlerType = clr::Type::FromValue(handlerTypeValue);
    if (FAILED(status) || !handlerType.Valid()) {
        failureToken = "HardwareEventTypeMissing";
        return false;
    }

    for (const Subscription& subscription : subscriptions) {
        *subscription.queue = host.CreateFromAssembly(L"mscorlib", L"System.Collections.Queue");
        if (!subscription.queue->Valid()) {
            failureToken = "EventQueueUnavailable";
            return false;
        }
        const clr::Value methodName = clr::Value::FromString(L"Enqueue");
        const clr::Value queueTypeValue = subscription.queue->ObjectType().AsValue();
        const clr::Value enqueueMethod =
            reflection.Invoke(L"GetMethod", clr::kBindInvokeMethod | clr::kBindPublic | clr::kBindInstance,
                              queueTypeValue, &methodName, 1, &status);
        if (FAILED(status) || !enqueueMethod.IsObject()) {
            failureToken = "EventQueueMethodMissing";
            return false;
        }
        // Delegate.CreateDelegate relaxes the parameter type, so the
        // IHardware-taking event handler binds to Queue.Enqueue(object).
        clr::Value delegateArguments[3] = {handlerTypeValue.Clone(), clr::Value::Borrow(subscription.queue->Self().AsObject()),
                                           enqueueMethod.Clone()};
        const clr::Value handler =
            handlerType.Invoke(L"CreateDelegate", clr::kBindInvokeMethod | clr::kBindPublic | clr::kBindStatic |
                                                      clr::kBindFlattenHierarchy,
                               clr::Value(), delegateArguments, 3, &status);
        if (FAILED(status) || !handler.IsObject()) {
            failureToken = "EventDelegateRejected";
            return false;
        }
        computer.Call(subscription.eventMember, &handler, 1, &status);
        if (FAILED(status)) {
            failureToken = "EventSubscriptionRejected";
            return false;
        }
    }
    return true;
}

void LibreHardwareMonitorSession::Impl::DrainHardwareEvents() {
    struct Drain {
        clr::Object* queue;
        bool added;
    };
    const Drain drains[] = {{&addedQueue, true}, {&removedQueue, false}};
    for (const Drain& drain : drains) {
        if (!drain.queue->Valid())
            continue;
        for (int guard = 0; guard < 256; ++guard) {
            int32_t count = 0;
            if (!drain.queue->Get(L"Count").AsInt32(count) || count <= 0)
                break;
            HRESULT status = E_FAIL;
            clr::Value item = drain.queue->Call(L"Dequeue", nullptr, 0, &status);
            if (FAILED(status) || !item.IsObject())
                break;
            clr::Object hardware(std::move(item));
            if (!hardware.Valid())
                continue;
            const std::string identifier = clr::ToText(hardware.Get(L"Identifier"));
            if (identifier.empty())
                continue;
            const auto existing = std::find_if(roots.begin(), roots.end(), [&identifier](const HardwareRoot& root) {
                return root.identifier == identifier;
            });
            if (!drain.added) {
                if (existing != roots.end())
                    roots.erase(existing);
                continue;
            }
            if (existing != roots.end())
                continue;
            HardwareRoot root;
            root.identifier = identifier;
            if (!hardware.Get(L"HardwareType").AsInt32(root.hardwareType))
                continue;
            root.isCpu = cpuHardwareTypeKnown && root.hardwareType == cpuHardwareType;
            root.isGpu = std::find(gpuHardwareTypes.begin(), gpuHardwareTypes.end(), root.hardwareType) !=
                         gpuHardwareTypes.end();
            root.object = std::move(hardware);
            roots.push_back(std::move(root));
        }
    }
}

void LibreHardwareMonitorSession::Impl::UpdateTree(const clr::Object& hardware, int depth) {
    if (!hardware.Valid() || depth > kMaximumHardwareDepth)
        return;
    hardware.Call(L"Update");
    const clr::Value children = hardware.Get(L"SubHardware");
    for (size_t index = 0; index < children.ArraySize(); ++index) {
        const clr::Object child(children.ArrayElement(index));
        UpdateTree(child, depth + 1);
    }
}

bool LibreHardwareMonitorSession::Impl::NeedsSensorType(int32_t sensorType) const {
    if (loadSensorTypeKnown && sensorType == loadSensorType)
        return true;
    for (size_t metric = 0; metric < kMetricCount; ++metric) {
        if (metricRequested[metric] && sensorTypeValues[metric] == sensorType)
            return true;
    }
    return false;
}

void LibreHardwareMonitorSession::Impl::CollectSensors(const clr::Object& hardware, int depth,
                                                       std::vector<TypedSensor>& sensors) {
    if (!hardware.Valid() || depth > kMaximumHardwareDepth)
        return;
    const clr::Value hardwareSensors = hardware.Get(L"Sensors");
    for (size_t index = 0; index < hardwareSensors.ArraySize(); ++index) {
        const clr::Object sensor(hardwareSensors.ArrayElement(index));
        TypedSensor typed;
        int32_t sensorType = 0;
        HRESULT status = E_FAIL;
        const clr::Value typeValue = sensor.Valid() ? sensor.Get(L"SensorType", &status) : clr::Value();
        // The type is read first so the common sensors this bridge never
        // reports - load, level, data, throughput - cost one call instead of
        // four. A busy system exposes several hundred of them.
        if (FAILED(status) || !typeValue.AsInt32(sensorType) || !NeedsSensorType(sensorType))
            continue;
        if (!ReadSensor(sensor, typed.candidate))
            continue;
        typed.sensorType = sensorType;
        sensors.push_back(std::move(typed));
    }
    const clr::Value children = hardware.Get(L"SubHardware");
    for (size_t index = 0; index < children.ArraySize(); ++index) {
        const clr::Object child(children.ArrayElement(index));
        CollectSensors(child, depth + 1, sensors);
    }
}

LibreHardwareMonitorSession::LibreHardwareMonitorSession() : impl_(std::make_unique<Impl>()) {}

LibreHardwareMonitorSession::~LibreHardwareMonitorSession() {
    Close();
}

const std::string& LibreHardwareMonitorSession::LibraryVersion() const {
    return impl_->libraryVersion;
}

bool LibreHardwareMonitorSession::Start(const std::filesystem::path& pluginDirectory,
                                        const BridgeSelectors& selectors, std::string& failureToken) {
    impl_->selectors = selectors;
    bool cpuRequested = false;
    bool gpuRequested = false;
    for (size_t metric = 0; metric < kMetricCount; ++metric) {
        impl_->metricRequested[metric] = selectors.values[metric] != "off";
        if (!impl_->metricRequested[metric])
            continue;
        if (kMetrics[metric].scope == MetricScope::Cpu)
            cpuRequested = true;
        else
            gpuRequested = true;
    }
    if (!cpuRequested && !gpuRequested) {
        failureToken = "NoMetricRequested";
        return false;
    }

    if (!impl_->host.Start(failureToken))
        return false;

    const std::filesystem::path libraryPath = pluginDirectory / kLibraryFileName;
    impl_->computer = impl_->host.CreateFromFile(libraryPath.wstring(), kComputerTypeName);
    if (!impl_->computer.Valid()) {
        failureToken = "LibraryLoadFailed";
        return false;
    }

    impl_->assemblyType = impl_->host.FindType(L"System.Reflection.Assembly");
    HRESULT status = E_FAIL;
    impl_->libraryAssembly =
        impl_->host.Reflection().Invoke(L"Assembly", clr::kBindGetProperty | clr::kBindPublic | clr::kBindInstance,
                                        impl_->computer.ObjectType().AsValue(), nullptr, 0, &status);
    if (!impl_->assemblyType.Valid() || FAILED(status) || !impl_->libraryAssembly.IsObject()) {
        failureToken = "LibraryMetadataUnavailable";
        return false;
    }
    const clr::Object assembly(impl_->libraryAssembly.Clone());
    const clr::Object assemblyName(assembly.Call(L"GetName"));
    impl_->libraryVersion = assemblyName.Valid() ? clr::ToText(assemblyName.Get(L"Version")) : std::string();
    if (impl_->libraryVersion.empty())
        impl_->libraryVersion = "unknown";

    clr::Type sensorTypeEnum;
    clr::Value sensorTypeEnumValue;
    clr::Type hardwareTypeEnum;
    clr::Value hardwareTypeEnumValue;
    if (!impl_->ResolveEnum(kSensorTypeEnumName, sensorTypeEnum, sensorTypeEnumValue) ||
        !impl_->ResolveEnum(kHardwareTypeEnumName, hardwareTypeEnum, hardwareTypeEnumValue)) {
        failureToken = "SensorMetadataMissing";
        return false;
    }
    for (size_t metric = 0; metric < kMetricCount; ++metric) {
        if (!impl_->metricRequested[metric])
            continue;
        if (!impl_->ResolveEnumMember(sensorTypeEnum, sensorTypeEnumValue, Utf8ToWide(kMetrics[metric].sensorType),
                                      impl_->sensorTypeValues[metric])) {
            failureToken = "SensorTypeUnknown";
            return false;
        }
    }
    impl_->loadSensorTypeKnown =
        gpuRequested && impl_->ResolveEnumMember(sensorTypeEnum, sensorTypeEnumValue, L"Load",
                                                 impl_->loadSensorType);
    impl_->cpuHardwareTypeKnown =
        impl_->ResolveEnumMember(hardwareTypeEnum, hardwareTypeEnumValue, L"Cpu", impl_->cpuHardwareType);
    std::vector<std::string> hardwareTypeNames;
    impl_->ResolveEnumNames(hardwareTypeEnum, hardwareTypeEnumValue, hardwareTypeNames);
    // Every GPU vendor gets its own HardwareType member, and the set grows with
    // new backends, so the prefix decides rather than a fixed list.
    for (const std::string& name : hardwareTypeNames) {
        if (name.rfind("Gpu", 0) != 0)
            continue;
        int32_t value = 0;
        if (impl_->ResolveEnumMember(hardwareTypeEnum, hardwareTypeEnumValue, Utf8ToWide(name), value))
            impl_->gpuHardwareTypes.push_back(value);
    }

    if (!impl_->SubscribeHardwareEvents(failureToken))
        return false;

    if (!impl_->computer.Set(L"IsCpuEnabled", clr::Value::FromBoolean(cpuRequested)) ||
        !impl_->computer.Set(L"IsGpuEnabled", clr::Value::FromBoolean(gpuRequested))) {
        failureToken = "VisitorConfigurationRejected";
        return false;
    }
    impl_->computer.Call(L"Open", nullptr, 0, &status);
    if (FAILED(status)) {
        failureToken = "ComputerOpenFailed";
        return false;
    }
    impl_->opened = true;
    impl_->DrainHardwareEvents();
    if (impl_->roots.empty()) {
        failureToken = "NoHardwareDiscovered";
        return false;
    }
    return true;
}

bool LibreHardwareMonitorSession::Sample(MetricReading* readings, std::string& failureToken) {
    if (!impl_->opened || !readings) {
        failureToken = "SessionNotOpen";
        return false;
    }
    impl_->DrainHardwareEvents();
    for (const HardwareRoot& root : impl_->roots)
        impl_->UpdateTree(root.object, 0);

    // Exact selectors may name a sensor on any GPU; automatic selectors follow
    // the active one, which keeps a multi-GPU system from reporting an idle
    // adapter while the game renders on the other.
    std::vector<TypedSensor> cpuSensors;
    std::vector<TypedSensor> allGpuSensors;
    std::vector<GpuLoadCandidate> gpuCandidates;
    std::vector<std::vector<TypedSensor>> perGpuSensors;
    for (const HardwareRoot& root : impl_->roots) {
        if (root.isCpu) {
            impl_->CollectSensors(root.object, 0, cpuSensors);
            continue;
        }
        if (!root.isGpu)
            continue;
        std::vector<TypedSensor> sensors;
        impl_->CollectSensors(root.object, 0, sensors);
        GpuLoadCandidate candidate;
        candidate.identifier = root.identifier;
        for (const TypedSensor& sensor : sensors) {
            if (impl_->loadSensorTypeKnown && sensor.sensorType == impl_->loadSensorType &&
                policy::EqualsIgnoreCase(sensor.candidate.name, kGpuCoreLoadSensorName) &&
                sensor.candidate.hasValue) {
                candidate.coreLoad = sensor.candidate.value;
                candidate.hasCoreLoad = true;
                break;
            }
        }
        allGpuSensors.insert(allGpuSensors.end(), sensors.begin(), sensors.end());
        gpuCandidates.push_back(std::move(candidate));
        perGpuSensors.push_back(std::move(sensors));
    }

    const size_t activeGpu = policy::SelectActiveGpu(gpuCandidates, impl_->activeGpuIdentifier);
    if (activeGpu != kNoSelection)
        impl_->activeGpuIdentifier = gpuCandidates[activeGpu].identifier;
    const std::vector<TypedSensor> emptySensors;
    const std::vector<TypedSensor>& activeGpuSensors =
        activeGpu != kNoSelection ? perGpuSensors[activeGpu] : emptySensors;

    for (size_t metric = 0; metric < kMetricCount; ++metric) {
        readings[metric] = MetricReading();
        if (!impl_->metricRequested[metric])
            continue;
        const bool isCpuMetric = kMetrics[metric].scope == MetricScope::Cpu;
        const std::string& selector = impl_->selectors.values[metric];
        const bool automatic = selector == "auto";
        const std::vector<TypedSensor>& scope =
            isCpuMetric ? cpuSensors : (automatic ? activeGpuSensors : allGpuSensors);
        const std::vector<SensorCandidate> candidates = FilterByType(scope, impl_->sensorTypeValues[metric]);
        const size_t selected =
            automatic ? policy::SelectAutomatic(candidates, kMetrics[metric].preferredNames,
                                                kMetrics[metric].preferredNameCount, kMetrics[metric].rejectZero,
                                                impl_->previousIdentifiers[metric])
                      : policy::SelectExact(candidates, selector);
        if (selected == kNoSelection || !policy::IsReportableReading(candidates[selected], kMetrics[metric])) {
            impl_->previousIdentifiers[metric].clear();
            continue;
        }
        readings[metric].available = true;
        readings[metric].value = candidates[selected].value;
        readings[metric].identifier = candidates[selected].identifier;
        impl_->previousIdentifiers[metric] = candidates[selected].identifier;
    }
    failureToken.clear();
    return true;
}

void LibreHardwareMonitorSession::Close() {
    if (!impl_ || !impl_->opened)
        return;
    impl_->computer.Call(L"Close");
    impl_->opened = false;
    impl_->roots.clear();
}

}  // namespace ce::hardware_sensors
