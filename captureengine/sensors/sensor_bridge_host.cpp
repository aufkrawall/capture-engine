#include "sensor_bridge_host.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "sensor_bridge_lhm.h"
#include "sensor_selection_policy.h"

namespace ce::hardware_sensors {
namespace {

using policy::kMetricCount;
using policy::kMetrics;

constexpr uint32_t kMinimumPollIntervalMs = 250;
constexpr uint32_t kMaximumPollIntervalMs = 10000;
constexpr wchar_t kShutdownEventPrefix[] = L"Local\\CE_LHM_Shutdown_";

struct BridgeArguments {
    std::wstring shutdownEventName;
    uint32_t pollIntervalMs = 1000;
    BridgeSelectors selectors;
};

void WriteProtocolLine(const std::string& line) {
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!output || output == INVALID_HANDLE_VALUE)
        return;
    const std::string payload = line + "\n";
    DWORD written = 0;
    WriteFile(output, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr);
}

void ReportError(const std::string& token) {
    WriteProtocolLine("CE_LHM_ERROR\t" + (token.empty() ? std::string("BridgeFailure") : token));
}

bool MatchOption(const std::wstring& argument, const std::wstring& option, std::wstring& value) {
    if (argument.size() <= option.size() || argument.compare(0, option.size(), option) != 0)
        return false;
    value = argument.substr(option.size());
    return true;
}

// The shutdown event name arrives from the sensor service, but the bridge
// validates it anyway: it is the only cross-process name this role opens.
bool IsValidShutdownEventName(const std::wstring& name) {
    const size_t prefixLength = wcslen(kShutdownEventPrefix);
    if (name.size() <= prefixLength || name.size() > 96 || name.compare(0, prefixLength, kShutdownEventPrefix) != 0)
        return false;
    for (size_t index = prefixLength; index < name.size(); ++index) {
        const wchar_t character = name[index];
        const bool allowed = (character >= L'0' && character <= L'9') ||
                             (character >= L'A' && character <= L'F') ||
                             (character >= L'a' && character <= L'f') || character == L'_';
        if (!allowed)
            return false;
    }
    return true;
}

bool ParseUnsigned(const std::wstring& text, uint32_t& value) {
    if (text.empty() || text.size() > 9)
        return false;
    uint32_t parsed = 0;
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9')
            return false;
        parsed = parsed * 10 + static_cast<uint32_t>(character - L'0');
    }
    value = parsed;
    return true;
}

std::string WideToUtf8Selector(const std::wstring& value) {
    if (value.empty() || value.size() > 255)
        return {};
    std::string result;
    result.reserve(value.size());
    for (const wchar_t character : value) {
        if (character < 32 || character > 126)
            return {};
        result.push_back(static_cast<char>(character));
    }
    return result;
}

bool IsValidSelector(const std::string& selector) {
    return selector == "off" || selector == "auto" || policy::IsValidSensorIdentifier(selector);
}

bool ParseArguments(int argumentCount, wchar_t** arguments, BridgeArguments& parsed) {
    for (size_t metric = 0; metric < kMetricCount; ++metric)
        parsed.selectors.values[metric] = "auto";

    for (int index = 2; index < argumentCount; ++index) {
        const std::wstring argument = arguments[index];
        std::wstring value;
        if (MatchOption(argument, kSensorBridgeShutdownEventOption, value)) {
            parsed.shutdownEventName = value;
            continue;
        }
        if (MatchOption(argument, kSensorBridgePollIntervalOption, value)) {
            if (!ParseUnsigned(value, parsed.pollIntervalMs))
                return false;
            continue;
        }
        bool matchedMetric = false;
        for (size_t metric = 0; metric < kMetricCount && !matchedMetric; ++metric) {
            if (!MatchOption(argument, MetricSelectorOption(metric), value))
                continue;
            matchedMetric = true;
            parsed.selectors.values[metric] = WideToUtf8Selector(value);
            if (!IsValidSelector(parsed.selectors.values[metric]))
                return false;
        }
        if (!matchedMetric)
            return false;
    }
    return IsValidShutdownEventName(parsed.shutdownEventName) &&
           parsed.pollIntervalMs >= kMinimumPollIntervalMs && parsed.pollIntervalMs <= kMaximumPollIntervalMs;
}

std::filesystem::path PluginDirectory() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return {};
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path() / L"plugins" / L"LibreHardwareMonitor";
}

// Round-trip formatting for a single-precision reading. The process never calls
// setlocale, so the C locale keeps the decimal point the protocol requires.
std::string FormatReading(float value) {
    char buffer[64] = {};
    const int written = snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
    if (written <= 0 || written >= static_cast<int>(sizeof(buffer)))
        return "-";
    return buffer;
}

std::string BuildSampleLine(uint32_t sequence, const MetricReading* readings) {
    std::string line = "CE_LHM_SAMPLE\t" + std::to_string(sequence);
    for (size_t metric = 0; metric < kMetricCount; ++metric) {
        if (!readings[metric].available || readings[metric].identifier.empty()) {
            line += "\t-\t-";
            continue;
        }
        line += "\t" + FormatReading(readings[metric].value) + "\t" + readings[metric].identifier;
    }
    return line;
}

int RunBridge(const BridgeArguments& arguments) {
    const HANDLE shutdownEvent = OpenEventW(SYNCHRONIZE, FALSE, arguments.shutdownEventName.c_str());
    if (!shutdownEvent) {
        ReportError("ShutdownEventUnavailable");
        return 2;
    }
    const std::filesystem::path pluginDirectory = PluginDirectory();
    if (pluginDirectory.empty()) {
        CloseHandle(shutdownEvent);
        ReportError("PluginDirectoryUnresolved");
        return 2;
    }

    LibreHardwareMonitorSession session;
    std::string failureToken;
    if (!session.Start(pluginDirectory, arguments.selectors, failureToken)) {
        CloseHandle(shutdownEvent);
        ReportError(failureToken);
        return 2;
    }
    WriteProtocolLine("CE_LHM_READY\t" + session.LibraryVersion());

    uint32_t sequence = 0;
    int exitCode = 0;
    while (WaitForSingleObject(shutdownEvent, 0) != WAIT_OBJECT_0) {
        MetricReading readings[kMetricCount];
        if (!session.Sample(readings, failureToken)) {
            ReportError(failureToken);
            exitCode = 2;
            break;
        }
        // Sequence zero marks "no sample yet" in the protocol, so a wrap skips it.
        if (++sequence == 0)
            sequence = 1;
        WriteProtocolLine(BuildSampleLine(sequence, readings));
        if (WaitForSingleObject(shutdownEvent, arguments.pollIntervalMs) == WAIT_OBJECT_0)
            break;
    }
    session.Close();
    CloseHandle(shutdownEvent);
    return exitCode;
}

}  // namespace

std::optional<int> TryRunSensorBridgeHost() {
    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (!arguments)
        return std::nullopt;
    if (argumentCount < 2 || wcscmp(arguments[1], kSensorBridgeCommand) != 0) {
        LocalFree(reinterpret_cast<HLOCAL>(arguments));
        return std::nullopt;
    }
    BridgeArguments parsed;
    const bool valid = ParseArguments(argumentCount, arguments, parsed);
    LocalFree(reinterpret_cast<HLOCAL>(arguments));
    if (!valid) {
        ReportError("BridgeArgumentsRejected");
        return 2;
    }
    // The bridge only ever calls into the CLR and the sensor library from this
    // thread, so the multithreaded apartment avoids needing a message pump.
    const HRESULT comStatus = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const int result = RunBridge(parsed);
    if (SUCCEEDED(comStatus))
        CoUninitialize();
    return result;
}

}  // namespace ce::hardware_sensors
