#include "monitor_selection.h"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <sstream>

namespace ce::monitor_selection {
namespace {

std::string TrimSelector(std::string_view value) {
    size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])))
        ++first;
    size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])))
        --last;
    if (last - first >= 2 && value[first] == '"' && value[last - 1] == '"') {
        ++first;
        --last;
    }
    return std::string(value.substr(first, last - first));
}

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

std::wstring LowerWide(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
    return value;
}

std::string WideToUtf8(const wchar_t* value) {
    if (!value || !*value)
        return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1)
        return {};
    std::string result(static_cast<size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, result.data(), bytes, nullptr, nullptr) == 0)
        return {};
    result.resize(static_cast<size_t>(bytes - 1));
    return result;
}

std::wstring Utf8ToWide(std::string_view value) {
    if (value.empty())
        return {};
    const int characters =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (characters <= 0)
        return {};
    std::wstring result(static_cast<size_t>(characters), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(),
                            characters) == 0) {
        return {};
    }
    return result;
}

struct DisplayConfigIdentity {
    std::wstring gdiDeviceName;
    std::wstring monitorDevicePath;
    std::wstring friendlyName;
    LUID adapterLuid = {};
    uint32_t sourceId = 0;
    uint32_t targetId = 0;
};

std::vector<DisplayConfigIdentity> QueryActiveDisplayIdentities() {
    std::vector<DisplayConfigIdentity> identities;
    for (int attempt = 0; attempt < 3; ++attempt) {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        LONG result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
        if (result != ERROR_SUCCESS)
            return identities;

        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        if (result == ERROR_INSUFFICIENT_BUFFER)
            continue;
        if (result != ERROR_SUCCESS)
            return identities;
        paths.resize(pathCount);

        for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
            source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            source.header.size = sizeof(source);
            source.header.adapterId = path.sourceInfo.adapterId;
            source.header.id = path.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || !source.viewGdiDeviceName[0])
                continue;

            DISPLAYCONFIG_TARGET_DEVICE_NAME target = {};
            target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            target.header.size = sizeof(target);
            target.header.adapterId = path.targetInfo.adapterId;
            target.header.id = path.targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS)
                continue;

            DisplayConfigIdentity identity;
            identity.gdiDeviceName = source.viewGdiDeviceName;
            identity.monitorDevicePath = target.monitorDevicePath;
            identity.friendlyName = target.monitorFriendlyDeviceName;
            identity.adapterLuid = path.targetInfo.adapterId;
            identity.sourceId = path.sourceInfo.id;
            identity.targetId = path.targetInfo.id;
            identities.push_back(std::move(identity));
        }
        break;
    }
    return identities;
}

BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM parameter) {
    auto* handles = reinterpret_cast<std::vector<HMONITOR>*>(parameter);
    handles->push_back(monitor);
    return TRUE;
}

MonitorDescriptor DescribeMonitor(HMONITOR monitor, const std::vector<DisplayConfigIdentity>& identities) {
    MonitorDescriptor descriptor;
    descriptor.handle = monitor;
    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info))
        return descriptor;

    descriptor.deviceName = WideToUtf8(info.szDevice);
    descriptor.desktopRect = info.rcMonitor;
    descriptor.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    const std::wstring deviceLower = LowerWide(info.szDevice);
    auto identity = std::find_if(identities.begin(), identities.end(), [&](const DisplayConfigIdentity& candidate) {
        return LowerWide(candidate.gdiDeviceName) == deviceLower;
    });
    if (identity != identities.end()) {
        descriptor.stableId = WideToUtf8(identity->monitorDevicePath.c_str());
        descriptor.friendlyName = WideToUtf8(identity->friendlyName.c_str());
        descriptor.adapterLuid = identity->adapterLuid;
        descriptor.sourceId = identity->sourceId;
        descriptor.targetId = identity->targetId;
        descriptor.hasDisplayConfigIdentity = !descriptor.stableId.empty();
    }
    if (descriptor.friendlyName.empty())
        descriptor.friendlyName = descriptor.deviceName;
    if (descriptor.stableId.empty())
        descriptor.stableId = "gdi:" + descriptor.deviceName;
    return descriptor;
}

MonitorDescriptor DescribeMonitor(HMONITOR monitor) {
    return DescribeMonitor(monitor, QueryActiveDisplayIdentities());
}

bool WriteAll(HANDLE output, const std::string& text) {
    size_t offset = 0;
    while (offset < text.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(text.size() - offset, MAXDWORD));
        DWORD written = 0;
        if (!WriteFile(output, text.data() + offset, chunk, &written, nullptr) || written == 0)
            return false;
        offset += written;
    }
    return true;
}

bool WriteConsoleAll(HANDLE output, const std::wstring& text) {
    size_t offset = 0;
    while (offset < text.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(text.size() - offset, MAXDWORD));
        DWORD written = 0;
        if (!WriteConsoleW(output, text.data() + offset, chunk, &written, nullptr) || written == 0)
            return false;
        offset += written;
    }
    return true;
}

}  // namespace

bool TryParseSelector(std::string_view value, Selector& selector) {
    const std::string trimmed = TrimSelector(value);
    const std::string lowered = LowerAscii(trimmed);
    selector = {};
    if (lowered.empty() || lowered == "auto") {
        return true;
    }
    if (lowered == "primary") {
        selector.kind = SelectorKind::kPrimary;
        selector.canonical = "primary";
        return true;
    }
    if (lowered == "window") {
        selector.kind = SelectorKind::kWindow;
        selector.canonical = "window";
        return true;
    }
    if (lowered == "cursor") {
        selector.kind = SelectorKind::kCursor;
        selector.canonical = "cursor";
        return true;
    }
    if (lowered.rfind("id:", 0) == 0 && trimmed.size() > 3) {
        selector.kind = SelectorKind::kStableId;
        selector.stableId = TrimSelector(std::string_view(trimmed).substr(3));
        if (selector.stableId.empty())
            return false;
        selector.canonical = "id:" + selector.stableId;
        return true;
    }
    return false;
}

bool IsExplicitSelector(const Selector& selector) {
    return selector.kind != SelectorKind::kAuto;
}

CandidateChoice ChooseCandidate(SelectorKind kind, const CandidateHandles& candidates) {
    switch (kind) {
        case SelectorKind::kAuto:
            if (candidates.targetWindow)
                return {candidates.targetWindow, "target-window"};
            if (candidates.hint)
                return {candidates.hint, "target-hint"};
            if (candidates.foregroundWindow)
                return {candidates.foregroundWindow, "foreground-window"};
            if (candidates.primary)
                return {candidates.primary, "primary-fallback"};
            return {};
        case SelectorKind::kPrimary:
            return {candidates.primary, "configured-primary"};
        case SelectorKind::kWindow:
            return {candidates.targetWindow, "configured-window"};
        case SelectorKind::kCursor:
            return {candidates.cursor, "configured-cursor"};
        case SelectorKind::kStableId:
            return {};
    }
    return {};
}

std::vector<MonitorDescriptor> EnumerateActiveMonitors() {
    std::vector<HMONITOR> handles;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&handles));
    const std::vector<DisplayConfigIdentity> identities = QueryActiveDisplayIdentities();
    std::vector<MonitorDescriptor> monitors;
    monitors.reserve(handles.size());
    for (HMONITOR handle : handles)
        monitors.push_back(DescribeMonitor(handle, identities));
    std::sort(monitors.begin(), monitors.end(), [](const MonitorDescriptor& left, const MonitorDescriptor& right) {
        if (left.primary != right.primary)
            return left.primary;
        if (left.desktopRect.left != right.desktopRect.left)
            return left.desktopRect.left < right.desktopRect.left;
        return left.desktopRect.top < right.desktopRect.top;
    });
    return monitors;
}

const MonitorDescriptor* FindByStableId(const std::vector<MonitorDescriptor>& monitors, std::string_view stableId) {
    const std::string wanted = LowerAscii(TrimSelector(stableId));
    auto found = std::find_if(monitors.begin(), monitors.end(), [&](const MonitorDescriptor& monitor) {
        return LowerAscii(monitor.stableId) == wanted;
    });
    return found == monitors.end() ? nullptr : &*found;
}

ResolveResult Resolve(const ResolveRequest& request) {
    ResolveResult result;
    if (request.selector.kind == SelectorKind::kStableId) {
        const std::vector<MonitorDescriptor> monitors = EnumerateActiveMonitors();
        const MonitorDescriptor* monitor = FindByStableId(monitors, request.selector.stableId);
        if (!monitor) {
            result.error = "configured monitor ID is not active";
            return result;
        }
        result.monitor = monitor->handle;
        result.descriptor = *monitor;
        result.reason = "configured-stable-id";
        return result;
    }

    CandidateHandles candidates;
    if (request.targetWindow)
        candidates.targetWindow = MonitorFromWindow(request.targetWindow, MONITOR_DEFAULTTONULL);
    candidates.hint = request.hint;
    if (request.foregroundWindow)
        candidates.foregroundWindow = MonitorFromWindow(request.foregroundWindow, MONITOR_DEFAULTTONULL);
    POINT cursorPosition = {};
    if (GetCursorPos(&cursorPosition))
        candidates.cursor = MonitorFromPoint(cursorPosition, MONITOR_DEFAULTTONULL);
    candidates.primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);

    const CandidateChoice choice = ChooseCandidate(request.selector.kind, candidates);
    if (!choice.monitor) {
        switch (request.selector.kind) {
            case SelectorKind::kWindow:
                result.error = "monitor=window requires a valid target window";
                break;
            case SelectorKind::kCursor:
                result.error = "the cursor monitor could not be resolved";
                break;
            case SelectorKind::kPrimary:
                result.error = "the Windows primary monitor could not be resolved";
                break;
            default:
                result.error = "no active monitor candidate could be resolved";
                break;
        }
        return result;
    }

    result.monitor = choice.monitor;
    result.descriptor = DescribeMonitor(choice.monitor);
    result.reason = choice.reason;
    return result;
}

std::string FormatMonitorList(const std::vector<MonitorDescriptor>& monitors) {
    std::ostringstream output;
    output << "CaptureEngine active monitors: " << monitors.size() << "\r\n";
    for (size_t index = 0; index < monitors.size(); ++index) {
        const MonitorDescriptor& monitor = monitors[index];
        output << "\r\n[" << (index + 1) << "] " << monitor.friendlyName;
        if (monitor.primary)
            output << " (primary)";
        output << "\r\n"
               << "  device=" << monitor.deviceName << "\r\n"
               << "  bounds=" << monitor.desktopRect.left << ',' << monitor.desktopRect.top << ' '
               << (monitor.desktopRect.right - monitor.desktopRect.left) << 'x'
               << (monitor.desktopRect.bottom - monitor.desktopRect.top) << "\r\n"
               << "  monitor=id:" << monitor.stableId << "\r\n";
        if (monitor.hasDisplayConfigIdentity) {
            output << "  adapter_luid=" << std::hex << static_cast<uint32_t>(monitor.adapterLuid.HighPart) << ':'
                   << monitor.adapterLuid.LowPart << std::dec << " source=" << monitor.sourceId
                   << " target=" << monitor.targetId << "\r\n";
        } else {
            output << "  identity_warning=DisplayConfig path unavailable; this GDI fallback ID may change\r\n";
        }
    }
    if (monitors.empty())
        output << "No active monitors were found.\r\n";
    return output.str();
}

int WriteMonitorListToStandardOutput() {
    const std::string output = FormatMonitorList(EnumerateActiveMonitors());
    HANDLE standardOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!standardOutput || standardOutput == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS))
            standardOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    if (!standardOutput || standardOutput == INVALID_HANDLE_VALUE) {
        if (AllocConsole())
            standardOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    DWORD consoleMode = 0;
    const bool isConsole = standardOutput && standardOutput != INVALID_HANDLE_VALUE &&
                           GetConsoleMode(standardOutput, &consoleMode);
    const std::wstring wideOutput = isConsole ? Utf8ToWide(output) : std::wstring{};
    const bool wroteOutput = isConsole ? (!wideOutput.empty() && WriteConsoleAll(standardOutput, wideOutput))
                                       : (standardOutput && standardOutput != INVALID_HANDLE_VALUE &&
                                          WriteAll(standardOutput, output));
    if (!wroteOutput) {
        if (!wideOutput.empty())
            OutputDebugStringW(wideOutput.c_str());
        else
            OutputDebugStringA(output.c_str());
        return 1;
    }
    return 0;
}

}  // namespace ce::monitor_selection
