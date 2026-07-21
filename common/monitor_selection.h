#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ce::monitor_selection {

enum class SelectorKind : uint8_t {
    kAuto = 0,
    kPrimary,
    kWindow,
    kCursor,
    kStableId,
};

struct Selector {
    SelectorKind kind = SelectorKind::kAuto;
    std::string stableId;
    std::string canonical = "auto";
};

struct MonitorDescriptor {
    HMONITOR handle = nullptr;
    std::string stableId;
    std::string deviceName;
    std::string friendlyName;
    RECT desktopRect = {};
    bool primary = false;
    LUID adapterLuid = {};
    uint32_t sourceId = 0;
    uint32_t targetId = 0;
    bool hasDisplayConfigIdentity = false;
};

struct CandidateHandles {
    HMONITOR targetWindow = nullptr;
    HMONITOR hint = nullptr;
    HMONITOR foregroundWindow = nullptr;
    HMONITOR cursor = nullptr;
    HMONITOR primary = nullptr;
};

struct CandidateChoice {
    HMONITOR monitor = nullptr;
    const char* reason = "unresolved";
};

struct ResolveRequest {
    Selector selector;
    HWND targetWindow = nullptr;
    HMONITOR hint = nullptr;
    HWND foregroundWindow = nullptr;
};

struct ResolveResult {
    HMONITOR monitor = nullptr;
    MonitorDescriptor descriptor;
    std::string reason;
    std::string error;

    explicit operator bool() const {
        return monitor != nullptr;
    }
};

bool TryParseSelector(std::string_view value, Selector& selector);
bool IsExplicitSelector(const Selector& selector);
CandidateChoice ChooseCandidate(SelectorKind kind, const CandidateHandles& candidates);
std::vector<MonitorDescriptor> EnumerateActiveMonitors();
const MonitorDescriptor* FindByStableId(const std::vector<MonitorDescriptor>& monitors, std::string_view stableId);
ResolveResult Resolve(const ResolveRequest& request);
std::string FormatMonitorList(const std::vector<MonitorDescriptor>& monitors);
int WriteMonitorListToStandardOutput();

}  // namespace ce::monitor_selection
