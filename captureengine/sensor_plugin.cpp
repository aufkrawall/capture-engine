#include "sensor_plugin.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "../common/config.h"
#include "../common/logging.h"
#include "../common/strict_float_parse.h"
#include "../common/strict_integer_parse.h"

namespace ce::hardware_sensors {
namespace {

constexpr std::string_view kReadyPrefix = "CE_LHM_READY";
constexpr std::string_view kSamplePrefix = "CE_LHM_SAMPLE";
constexpr std::string_view kErrorPrefix = "CE_LHM_ERROR";
constexpr size_t kMaximumBufferedOutput = 64 * 1024;

bool IsAsciiAlphaNumeric(unsigned char ch) {
    return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

std::vector<std::string_view> SplitTabs(std::string_view line) {
    std::vector<std::string_view> fields;
    size_t start = 0;
    while (start <= line.size()) {
        const size_t tab = line.find('\t', start);
        fields.push_back(line.substr(start, tab == std::string_view::npos ? line.size() - start : tab - start));
        if (tab == std::string_view::npos)
            break;
        start = tab + 1;
    }
    return fields;
}

bool IsSafeProtocolToken(std::string_view value, size_t maximumLength, bool sensorIdentifier) {
    if (value.empty() || value.size() > maximumLength ||
        (sensorIdentifier && (value.size() < 2 || value.front() != '/'))) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [sensorIdentifier](unsigned char ch) {
        if (IsAsciiAlphaNumeric(ch) || ch == '_' || ch == '-' || ch == '.')
            return true;
        return sensorIdentifier && ch == '/';
    });
}

bool ParseSensorValue(std::string_view valueField, std::string_view identifierField, float minimum, float maximum,
                      bool minimumExclusive, SensorValue& output) {
    output = {};
    if (valueField == "-" && identifierField == "-")
        return true;
    if (valueField == "-" || identifierField == "-" || !IsSafeProtocolToken(identifierField, 255, true))
        return false;
    float value = 0.0f;
    if (!ce::TryParseFiniteFloat(std::string(valueField), value) || value > maximum ||
        (minimumExclusive ? value <= minimum : value < minimum)) {
        return false;
    }
    output.value = value;
    output.valid = true;
    output.identifier.assign(identifierField);
    return true;
}

std::wstring QuoteWindowsArgument(std::wstring_view argument) {
    std::wstring quoted;
    quoted.push_back(L'"');
    size_t backslashes = 0;
    for (wchar_t ch : argument) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(ch);
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(ch);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

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

std::filesystem::path ExecutableDirectory() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return {};
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

std::wstring CreateShutdownEventName() {
    std::array<unsigned char, 8> nonce{};
    if (BCryptGenRandom(nullptr, nonce.data(), static_cast<ULONG>(nonce.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        const uint64_t fallback = GetTickCount64();
        memcpy(nonce.data(), &fallback, sizeof(fallback));
    }
    wchar_t name[96] = {};
    swprintf(name, std::size(name), L"Local\\CE_LHM_Shutdown_%08X_%02X%02X%02X%02X%02X%02X%02X%02X",
             GetCurrentProcessId(), nonce[0], nonce[1], nonce[2], nonce[3], nonce[4], nonce[5], nonce[6], nonce[7]);
    return name;
}

void CloseIfValid(HANDLE& handle) {
    if (handle && handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
    handle = nullptr;
}

}  // namespace

bool ParseBridgeMessage(std::string_view line, BridgeMessage& message) {
    message = {};
    if (line.empty() || line.size() > 4096)
        return false;
    const std::vector<std::string_view> fields = SplitTabs(line);
    if (fields.size() == 2 && fields[0] == kReadyPrefix && IsSafeProtocolToken(fields[1], 63, false)) {
        message.kind = BridgeMessageKind::Ready;
        message.detail.assign(fields[1]);
        return true;
    }
    if (fields.size() == 2 && fields[0] == kErrorPrefix && IsSafeProtocolToken(fields[1], 127, false)) {
        message.kind = BridgeMessageKind::Error;
        message.detail.assign(fields[1]);
        return true;
    }
    if (fields.size() != 12 || fields[0] != kSamplePrefix)
        return false;

    uint32_t sequence = 0;
    if (!ce::TryParseUInt32(fields[1], sequence) || sequence == 0)
        return false;
    HardwareSensorSnapshot snapshot;
    if (!ParseSensorValue(fields[2], fields[3], 0.0f, 250.0f, true, snapshot.cpuTemperature) ||
        !ParseSensorValue(fields[4], fields[5], 0.0f, 250.0f, true, snapshot.gpuTemperature) ||
        !ParseSensorValue(fields[6], fields[7], 0.0f, 5000.0f, false, snapshot.cpuPackagePower) ||
        !ParseSensorValue(fields[8], fields[9], 0.0f, 5000.0f, false, snapshot.gpuPackagePower) ||
        !ParseSensorValue(fields[10], fields[11], 0.0f, 100000.0f, false, snapshot.gpuFan)) {
        return false;
    }
    snapshot.sequence = sequence;
    message.kind = BridgeMessageKind::Sample;
    message.snapshot = std::move(snapshot);
    return true;
}

bool IsSnapshotFresh(const HardwareSensorSnapshot& snapshot, uint64_t nowTickMs, uint32_t pollIntervalMs) {
    if (snapshot.receivedTickMs == 0 || nowTickMs < snapshot.receivedTickMs)
        return false;
    const uint64_t staleAfterMs = (std::max)(5000ull, static_cast<uint64_t>(pollIntervalMs) * 3ull);
    return nowTickMs - snapshot.receivedTickMs <= staleAfterMs;
}

struct LibreHardwareMonitorPlugin::Impl {
    explicit Impl(const HardwareSensorsConfig& sourceConfig) : config(sourceConfig) {}

    ~Impl() {
        Shutdown();
    }

    bool AnySensorRequested() const {
        return config.cpuTemperature != "off" || config.gpuTemperature != "off" ||
               config.cpuPackagePower != "off" || config.gpuPackagePower != "off" || config.gpuFan != "off";
    }

    void Shutdown() {
        if (shutdownEvent)
            SetEvent(shutdownEvent);
        if (process) {
            const DWORD wait = WaitForSingleObject(process, 3000);
            if (wait == WAIT_TIMEOUT) {
                LogWarn("[Sensors:LHM] Bridge did not stop within 3000 ms; terminating only the bridge process");
                TerminateProcess(process, 1);
                WaitForSingleObject(process, 1000);
            }
        }
        CloseIfValid(process);
        CloseIfValid(stdoutRead);
        CloseIfValid(shutdownEvent);
        CloseIfValid(job);
        running = false;
    }

    bool Start() {
        if (running)
            return true;
        if (process || stdoutRead || shutdownEvent || job)
            Shutdown();
        snapshot = {};
        outputBuffer.clear();
        rejectedMessages = 0;
        readyLogged = false;
        exitLogged = false;
        staleLogged = false;
        launchTickMs = 0;

        if (config.enabled == "off" || !AnySensorRequested()) {
            LogDebug("[Sensors:LHM] Optional hardware sensors disabled by configuration");
            return false;
        }

        const std::filesystem::path executableDirectory = ExecutableDirectory();
        if (executableDirectory.empty()) {
            LogWarn("[Sensors:LHM] Cannot resolve the CaptureEngine executable directory");
            return false;
        }
        const std::filesystem::path pluginDirectory = executableDirectory / L"plugins" / L"LibreHardwareMonitor";
        const std::array<const wchar_t*, 5> requiredFiles = {
            L"CaptureEngine.LibreHardwareMonitor.ps1",
            L"LibreHardwareMonitorLib.dll",
            L"System.Memory.dll",
            L"System.Numerics.Vectors.dll",
            L"System.Runtime.CompilerServices.Unsafe.dll",
        };
        std::vector<std::wstring> missingFiles;
        for (const wchar_t* filename : requiredFiles) {
            const DWORD attributes = GetFileAttributesW((pluginDirectory / filename).c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                missingFiles.emplace_back(filename);
        }
        if (!missingFiles.empty()) {
            if (config.enabled == "on") {
                std::string names;
                for (const std::wstring& filename : missingFiles) {
                    if (!names.empty())
                        names += ", ";
                    names.append(filename.begin(), filename.end());
                }
                LogWarn("[Sensors:LHM] Optional plugin requested but required file(s) are missing: %s", names.c_str());
            } else {
                LogDebug("[Sensors:LHM] Optional plugin files are absent; native usage metrics remain active");
            }
            return false;
        }

        wchar_t systemDirectory[32768] = {};
        const UINT systemLength = GetSystemDirectoryW(systemDirectory, static_cast<UINT>(std::size(systemDirectory)));
        if (systemLength == 0 || systemLength >= std::size(systemDirectory)) {
            LogWarn("[Sensors:LHM] Cannot resolve Windows PowerShell");
            return false;
        }
        const std::filesystem::path powerShell =
            std::filesystem::path(std::wstring(systemDirectory, systemLength)) / L"WindowsPowerShell" / L"v1.0" /
            L"powershell.exe";
        if (GetFileAttributesW(powerShell.c_str()) == INVALID_FILE_ATTRIBUTES) {
            LogWarn("[Sensors:LHM] Windows PowerShell 5.1 is unavailable");
            return false;
        }

        const std::wstring eventName = CreateShutdownEventName();
        shutdownEvent = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
        if (!shutdownEvent) {
            LogWarn("[Sensors:LHM] Cannot create the bridge shutdown event (error=%lu)", GetLastError());
            return false;
        }
        job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits = {};
        jobLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jobLimits, sizeof(jobLimits))) {
            LogWarn("[Sensors:LHM] Cannot create the bridge lifetime job (error=%lu)", GetLastError());
            Shutdown();
            return false;
        }

        SECURITY_ATTRIBUTES inheritable = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE stdoutWrite = nullptr;
        if (!CreatePipe(&stdoutRead, &stdoutWrite, &inheritable, 0) ||
            !SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0)) {
            LogWarn("[Sensors:LHM] Cannot create the bridge output pipe (error=%lu)", GetLastError());
            CloseIfValid(stdoutWrite);
            Shutdown();
            return false;
        }
        HANDLE nullHandle = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (nullHandle == INVALID_HANDLE_VALUE) {
            LogWarn("[Sensors:LHM] Cannot open the null stream for the bridge (error=%lu)", GetLastError());
            nullHandle = nullptr;
            CloseIfValid(stdoutWrite);
            Shutdown();
            return false;
        }

        std::vector<std::wstring> arguments = {
            L"-NoLogo", L"-NoProfile", L"-NonInteractive", L"-ExecutionPolicy", L"Bypass", L"-File",
            (pluginDirectory / L"CaptureEngine.LibreHardwareMonitor.ps1").wstring(), L"-ShutdownEventName",
            eventName, L"-PollIntervalMs", std::to_wstring(config.pollIntervalMs), L"-CpuTemperature",
            Utf8ToWide(config.cpuTemperature), L"-GpuTemperature", Utf8ToWide(config.gpuTemperature),
            L"-CpuPackagePower", Utf8ToWide(config.cpuPackagePower), L"-GpuPackagePower",
            Utf8ToWide(config.gpuPackagePower), L"-GpuFan", Utf8ToWide(config.gpuFan),
        };
        std::wstring commandLine = QuoteWindowsArgument(powerShell.wstring());
        for (const std::wstring& argument : arguments) {
            if (argument.empty()) {
                LogWarn("[Sensors:LHM] Refusing an invalid non-UTF-8 selector");
                CloseIfValid(stdoutWrite);
                CloseIfValid(nullHandle);
                Shutdown();
                return false;
            }
            commandLine.push_back(L' ');
            commandLine += QuoteWindowsArgument(argument);
        }

        SIZE_T attributeBytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
        std::vector<unsigned char> attributeStorage(attributeBytes);
        auto* attributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
        if (!InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeBytes)) {
            LogWarn("[Sensors:LHM] Cannot initialize the bridge handle allowlist (error=%lu)", GetLastError());
            CloseIfValid(stdoutWrite);
            CloseIfValid(nullHandle);
            Shutdown();
            return false;
        }
        HANDLE inheritedHandles[] = {stdoutWrite, nullHandle};
        const bool attributesReady = UpdateProcThreadAttribute(
            attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, static_cast<void*>(inheritedHandles),
            sizeof(inheritedHandles), nullptr, nullptr) != FALSE;

        STARTUPINFOEXW startup = {};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_FORCEOFFFEEDBACK;
        startup.StartupInfo.hStdInput = nullHandle;
        startup.StartupInfo.hStdOutput = stdoutWrite;
        startup.StartupInfo.hStdError = nullHandle;
        startup.lpAttributeList = attributeList;
        PROCESS_INFORMATION processInfo = {};
        const BOOL created = attributesReady &&
                             CreateProcessW(powerShell.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                                            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                            pluginDirectory.c_str(), &startup.StartupInfo, &processInfo);
        const DWORD createError = created ? ERROR_SUCCESS : GetLastError();
        DeleteProcThreadAttributeList(attributeList);
        CloseIfValid(stdoutWrite);
        CloseIfValid(nullHandle);
        if (!created) {
            LogWarn("[Sensors:LHM] Cannot launch the bridge (error=%lu)", createError);
            Shutdown();
            return false;
        }
        if (!AssignProcessToJobObject(job, processInfo.hProcess) ||
            ResumeThread(processInfo.hThread) == static_cast<DWORD>(-1)) {
            const DWORD assignmentError = GetLastError();
            TerminateProcess(processInfo.hProcess, 1);
            WaitForSingleObject(processInfo.hProcess, 1000);
            CloseHandle(processInfo.hThread);
            CloseHandle(processInfo.hProcess);
            LogWarn("[Sensors:LHM] Cannot bind the bridge lifetime to the sensor service (error=%lu)",
                    assignmentError);
            Shutdown();
            return false;
        }
        CloseHandle(processInfo.hThread);
        process = processInfo.hProcess;
        running = true;
        launchTickMs = GetTickCount64();
        LogInfo("[Sensors:LHM] Bridge launched out of process (poll=%u ms)", config.pollIntervalMs);
        return true;
    }

    void LogSelectedSensors(const HardwareSensorSnapshot& next) {
        const std::array<std::pair<const char*, const SensorValue*>, 5> values = {{
            {"cpu_temperature", &next.cpuTemperature},
            {"gpu_temperature", &next.gpuTemperature},
            {"cpu_package_power", &next.cpuPackagePower},
            {"gpu_package_power", &next.gpuPackagePower},
            {"gpu_fan", &next.gpuFan},
        }};
        const std::array<const SensorValue*, 5> previous = {
            &snapshot.cpuTemperature, &snapshot.gpuTemperature, &snapshot.cpuPackagePower,
            &snapshot.gpuPackagePower, &snapshot.gpuFan,
        };
        for (size_t index = 0; index < values.size(); ++index) {
            if (values[index].second->identifier == previous[index]->identifier)
                continue;
            if (values[index].second->valid) {
                LogInfo("[Sensors:LHM] Selected %s=%s", values[index].first,
                        values[index].second->identifier.c_str());
            } else if (previous[index]->valid) {
                LogInfo("[Sensors:LHM] %s became unavailable", values[index].first);
            }
        }
    }

    void Poll() {
        if (!running || !process || !stdoutRead)
            return;

        for (int readAttempt = 0; readAttempt < 16; ++readAttempt) {
            DWORD available = 0;
            if (!PeekNamedPipe(stdoutRead, nullptr, 0, nullptr, &available, nullptr) || available == 0)
                break;
            std::array<char, 4096> chunk{};
            DWORD bytesRead = 0;
            if (!ReadFile(stdoutRead, chunk.data(), (std::min)(available, static_cast<DWORD>(chunk.size())),
                          &bytesRead, nullptr) || bytesRead == 0) {
                break;
            }
            outputBuffer.append(chunk.data(), bytesRead);
            if (outputBuffer.size() > kMaximumBufferedOutput) {
                outputBuffer.clear();
                LogWarn("[Sensors:LHM] Discarded oversized bridge output");
                break;
            }
        }

        size_t newline = std::string::npos;
        while ((newline = outputBuffer.find('\n')) != std::string::npos) {
            std::string line = outputBuffer.substr(0, newline);
            outputBuffer.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            BridgeMessage message;
            if (!ParseBridgeMessage(line, message)) {
                ++rejectedMessages;
                if (rejectedMessages <= 4 || rejectedMessages % 100 == 0)
                    LogWarn("[Sensors:LHM] Rejected malformed bridge message (count=%u)", rejectedMessages);
                continue;
            }
            if (message.kind == BridgeMessageKind::Ready) {
                if (!readyLogged) {
                    LogInfo("[Sensors:LHM] LibreHardwareMonitor ready (version=%s)", message.detail.c_str());
                    readyLogged = true;
                }
            } else if (message.kind == BridgeMessageKind::Error) {
                LogWarn("[Sensors:LHM] Bridge reported %s", message.detail.c_str());
                snapshot = {};
            } else if (message.kind == BridgeMessageKind::Sample) {
                if (!readyLogged) {
                    ++rejectedMessages;
                    if (rejectedMessages <= 4 || rejectedMessages % 100 == 0)
                        LogWarn("[Sensors:LHM] Rejected sample before bridge readiness (count=%u)",
                                rejectedMessages);
                    continue;
                }
                message.snapshot.receivedTickMs = GetTickCount64();
                LogSelectedSensors(message.snapshot);
                snapshot = std::move(message.snapshot);
                staleLogged = false;
            }
        }

        DWORD exitCode = STILL_ACTIVE;
        if (!GetExitCodeProcess(process, &exitCode) || exitCode != STILL_ACTIVE) {
            if (!exitLogged) {
                LogWarn("[Sensors:LHM] Bridge exited (code=%lu); values are unavailable until service restart",
                        exitCode);
                exitLogged = true;
            }
            running = false;
            snapshot = {};
        } else if (!readyLogged && launchTickMs != 0 && GetTickCount64() - launchTickMs > 30000) {
            LogWarn("[Sensors:LHM] Bridge did not become ready within 30000 ms; terminating it");
            TerminateProcess(process, 1);
            WaitForSingleObject(process, 1000);
            running = false;
            snapshot = {};
        } else if (snapshot.receivedTickMs != 0 &&
                   !IsSnapshotFresh(snapshot, GetTickCount64(), config.pollIntervalMs)) {
            if (!staleLogged) {
                LogWarn("[Sensors:LHM] Bridge samples became stale; hiding optional hardware values");
                staleLogged = true;
            }
            snapshot = {};
        }
    }

    HardwareSensorsConfig config;
    HANDLE process = nullptr;
    HANDLE stdoutRead = nullptr;
    HANDLE shutdownEvent = nullptr;
    HANDLE job = nullptr;
    HardwareSensorSnapshot snapshot;
    std::string outputBuffer;
    uint32_t rejectedMessages = 0;
    uint64_t launchTickMs = 0;
    bool running = false;
    bool readyLogged = false;
    bool exitLogged = false;
    bool staleLogged = false;
};

LibreHardwareMonitorPlugin::LibreHardwareMonitorPlugin(const HardwareSensorsConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

LibreHardwareMonitorPlugin::~LibreHardwareMonitorPlugin() = default;

bool LibreHardwareMonitorPlugin::Start() {
    return impl_->Start();
}

void LibreHardwareMonitorPlugin::Poll() {
    impl_->Poll();
}

HardwareSensorSnapshot LibreHardwareMonitorPlugin::GetSnapshot() const {
    if (!IsSnapshotFresh(impl_->snapshot, GetTickCount64(), impl_->config.pollIntervalMs))
        return {};
    return impl_->snapshot;
}

}  // namespace ce::hardware_sensors
