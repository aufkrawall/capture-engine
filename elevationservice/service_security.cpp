#include "service_internal.h"
#include <tlhelp32.h>
#include <cstdio>

namespace ce::elevation {

std::wstring InstalledOwner() {
    wchar_t sid[256]{};
    DWORD bytes = sizeof(sid);
    const LSTATUS status =
        RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\CaptureEngineElevation\\Parameters",
                     L"OwnerSid", RRF_RT_REG_SZ, nullptr, sid, &bytes);
    PSID parsed = nullptr;
    if (status != ERROR_SUCCESS || !ConvertStringSidToSidW(sid, &parsed))
        return {};
    LocalFree(parsed);
    return sid;
}

bool AuthenticateClient(HANDLE pipe, const Hello& hello, const std::wstring& owner, Handle& controller) {
    ULONG peerPid = 0;
    DWORD sessionId = 0;
    if (!GetNamedPipeClientProcessId(pipe, &peerPid) || !ProcessIdToSessionId(peerPid, &sessionId) ||
        sessionId != hello.sessionId)
        return false;
    Handle peer(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, peerPid));
    controller.Reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, hello.controllerPid));
    if (!peer || !controller || WaitForSingleObject(controller.Get(), 0) != WAIT_TIMEOUT)
        return false;
    std::wstring peerImage(32768, L'\0');
    std::wstring controllerImage(32768, L'\0');
    DWORD peerLength = static_cast<DWORD>(peerImage.size());
    DWORD controllerLength = static_cast<DWORD>(controllerImage.size());
    FILETIME peerCreated{}, controllerCreated{}, exited{}, kernel{}, user{};
    if (!QueryFullProcessImageNameW(peer.Get(), 0, peerImage.data(), &peerLength) ||
        !QueryFullProcessImageNameW(controller.Get(), 0, controllerImage.data(), &controllerLength) ||
        _wcsicmp(peerImage.substr(0, peerLength).c_str(), controllerImage.substr(0, controllerLength).c_str()) != 0 ||
        !GetProcessTimes(peer.Get(), &peerCreated, &exited, &kernel, &user) ||
        !GetProcessTimes(controller.Get(), &controllerCreated, &exited, &kernel, &user) ||
        CompareFileTime(&controllerCreated, &peerCreated) > 0)
        return false;
    const std::wstring peerSid = ProcessUserSid(peer.Get());
    if (peerSid.empty() || ProcessUserSid(controller.Get()) != peerSid)
        return false;
    if (peerSid != owner) {
        HANDLE rawToken = nullptr;
        if (!OpenProcessToken(peer.Get(), TOKEN_QUERY, &rawToken))
            return false;
        Handle token(rawToken);
        TOKEN_ELEVATION elevation{};
        DWORD bytes = 0;
        if (!GetTokenInformation(token.Get(), TokenElevation, &elevation, sizeof(elevation), &bytes) ||
            !elevation.TokenIsElevated)
            return false;
    }
    DWORD controllerSession = 0;
    if (!ProcessIdToSessionId(hello.controllerPid, &controllerSession) || controllerSession != sessionId)
        return false;
    if (peerPid == hello.controllerPid)
        return true;
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!snapshot || !Process32FirstW(snapshot.Get(), &entry))
        return false;
    do {
        if (entry.th32ProcessID == peerPid)
            return entry.th32ParentProcessID == hello.controllerPid;
    } while (Process32NextW(snapshot.Get(), &entry));
    return false;
}

HardwareSensorsConfig MakeSensorConfig(const SensorRequest& request) {
    HardwareSensorsConfig config;
    config.enabled = "on";
    config.pollIntervalMs = request.pollIntervalMs;
    std::string* values[] = {&config.cpuTemperature,  &config.gpuTemperature, &config.cpuPackagePower,
                             &config.gpuPackagePower, &config.gpuFan,         &config.cpuCoreClock,
                             &config.gpuCoreClock,    &config.gpuMemoryClock, &config.gpuVoltage};
    for (size_t index = 0; index < kMetricCount; ++index)
        *values[index] = request.selectors[index].data();
    return config;
}

SensorSample MakeSensorSample(const ce::hardware_sensors::HardwareSensorSnapshot& snapshot) {
    SensorSample sample;
    sample.sampledTickMs = snapshot.receivedTickMs;
    if (!snapshot.sequence)
        return sample;
    std::string line = "CE_LHM_SAMPLE\t" + std::to_string(snapshot.sequence);
    const ce::hardware_sensors::SensorValue* values[] = {
        &snapshot.cpuTemperature, &snapshot.gpuTemperature, &snapshot.cpuPackagePower, &snapshot.gpuPackagePower,
        &snapshot.gpuFan,         &snapshot.cpuCoreClock,   &snapshot.gpuCoreClock,    &snapshot.gpuMemoryClock,
        &snapshot.gpuVoltage,     &snapshot.cpuMaxCoreClock};
    for (const auto* value : values) {
        if (!value->valid) {
            line += "\t-\t-";
            continue;
        }
        char number[64]{};
        std::snprintf(number, sizeof(number), "%.9g", static_cast<double>(value->value));
        line += "\t" + std::string(number) + "\t" + value->identifier;
    }
    if (line.size() >= sample.line.size())
        return {};
    std::memcpy(sample.line.data(), line.data(), line.size());
    return sample;
}
}  // namespace ce::elevation
