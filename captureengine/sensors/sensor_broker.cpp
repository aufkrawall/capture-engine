#include "sensor_broker.h"
#include "common/logging/logging.h"

namespace ce::hardware_sensors {
bool SensorBrokerBackend::Start() {
    const bool enabled = ce::elevation::ServiceEnabled() && config_.enabled != "off";
    HKEY driver = nullptr;
    const bool driverPresent = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\PawnIO", 0,
                                             KEY_READ, &driver) == ERROR_SUCCESS;
    if (driver)
        RegCloseKey(driver);
    if (driverPresent != driverPresent_) {
        driverPresent_ = driverPresent;
        client_.Disconnect();
        snapshot_.Clear();
        nextAttempt_ = 0;
        LogInfo("[Sensors:LHM] PawnIO installation changed; renewing the service sensor subscription");
    }
    if (enabled != enabled_) {
        nextAttempt_ = 0;
        enabled_ = enabled;
    }
    if (!enabled) {
        client_.Disconnect();
        snapshot_.Clear();
        return false;
    }
    if (client_.Connected())
        return true;
    if (GetTickCount64() < nextAttempt_)
        return false;
    nextAttempt_ = GetTickCount64() + 5000;
    ce::elevation::SensorRequest request;
    request.pollIntervalMs = config_.pollIntervalMs;
    const std::string* selectors[] = {&config_.cpuTemperature,  &config_.gpuTemperature, &config_.cpuPackagePower,
                                      &config_.gpuPackagePower, &config_.gpuFan,         &config_.cpuCoreClock,
                                      &config_.gpuCoreClock,    &config_.gpuMemoryClock, &config_.gpuVoltage};
    bool requested = false;
    for (size_t index = 0; index < ce::elevation::kMetricCount; ++index) {
        if (selectors[index]->size() >= request.selectors[index].size())
            return false;
        std::memcpy(request.selectors[index].data(), selectors[index]->data(), selectors[index]->size());
        requested = requested || *selectors[index] != "off";
    }
    if (!requested)
        return false;
    const bool connected = client_.Connect(ce::elevation::ControllerPid());
    if (connected)
        stopLocal_();
    if (!connected || !client_.Subscribe(request)) {
        client_.Disconnect();
        if (!failureLogged_)
            LogWarn("[Sensors:LHM] Elevation service unavailable; using the local sensor backend");
        failureLogged_ = true;
        return false;
    }
    failureLogged_ = false;
    snapshot_.Clear();
    LogInfo(
        "[Sensors:LHM] Switched to the privileged service backend; sensor worker remains at its existing privilege "
        "level");
    return true;
}

void SensorBrokerBackend::Poll() {
    PollSample();
    snapshotDiagnostics_.Report(snapshot_, client_.Connected(), GetTickCount64(), config_.pollIntervalMs,
                                 "privileged");
}

void SensorBrokerBackend::PollSample() {
    if (!Start())
        return;
    ce::elevation::SensorSample sample;
    if (!client_.Sample(sample)) {
        snapshot_.Clear();
        return;
    }
    const char* end = static_cast<const char*>(std::memchr(sample.line.data(), 0, sample.line.size()));
    if (!end || sample.sampledTickMs > GetTickCount64()) {
        client_.Disconnect();
        snapshot_.Clear();
        return;
    }
    if (end == sample.line.data()) {
        snapshot_.Clear();
        return;
    }
    BridgeMessage message;
    if (!ParseBridgeMessage(std::string_view(sample.line.data(), static_cast<size_t>(end - sample.line.data())),
                            message) ||
        message.kind != BridgeMessageKind::Sample) {
        LogWarn("[Sensors:LHM] Rejected malformed service sample");
        client_.Disconnect();
        snapshot_.Clear();
        return;
    }
    message.snapshot.receivedTickMs = sample.sampledTickMs;
    snapshot_.Store(std::move(message.snapshot));
}

HardwareSensorSnapshot SensorBrokerBackend::Snapshot() const {
    return snapshot_.Read(client_.Connected(), GetTickCount64());
}
}  // namespace ce::hardware_sensors
