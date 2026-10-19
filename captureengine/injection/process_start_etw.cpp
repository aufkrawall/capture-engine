#include "process_start_etw.h"

#include <cwchar>
#include <limits>
#include <vector>

#include "common/platform/display_timing_etw.h"
#include "common/logging/logging.h"

namespace ce::process_start {

namespace {

std::string QueryProcessImageName(DWORD pid) {
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess)
        return {};
    wchar_t path[MAX_PATH] = {};
    DWORD size = static_cast<DWORD>(std::size(path));
    std::string name;
    if (QueryFullProcessImageNameW(hProcess, 0, path, &size) && size > 0) {
        name = ExtractFileName(path);
    }
    CloseHandle(hProcess);
    return name;
}

}  // namespace

EtwMonitor::EtwMonitor()
    : consumer_(TraceOperations{
          [this] {
              return ce::elevation::ServiceEnabled() && broker_.Connect(ce::elevation::ControllerPid()) &&
                     broker_.AcquireProcessTrace();
          },
          [](EVENT_TRACE_LOGFILEW* trace) { return OpenTraceW(trace); },
          [](TRACEHANDLE handle) { return ProcessTrace(&handle, 1, nullptr, nullptr); },
          [](TRACEHANDLE handle) { CloseTrace(handle); },
          [this] { broker_.ReleaseTrace(); broker_.Disconnect(); }}) {}

EtwMonitor::~EtwMonitor() noexcept {
    Stop();
}

void WINAPI EtwMonitor::EventRecordCallback(EVENT_RECORD* event) {
    if (event && event->UserContext) {
        static_cast<EtwMonitor*>(event->UserContext)->OnEvent(event);
    }
}

ULONG WINAPI EtwMonitor::BufferCallback(EVENT_TRACE_LOGFILEW* logfile) {
    (void)logfile;
    return TRUE;
}

bool EtwMonitor::Start(StartCallback callback) {
    if (consumer_.IsRunning() || !callback)
        return false;
    callback_ = std::move(callback);

    wchar_t sessionName[display_timing_etw::kTraceSessionNameCapacity] = {};
    wcscpy_s(sessionName, ce::elevation::kTraceName);

    EVENT_TRACE_LOGFILEW trace = {};
    trace.LoggerName = sessionName;
    trace.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD |
                             PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    trace.EventRecordCallback = &EventRecordCallback;
    trace.BufferCallback = &BufferCallback;
    trace.Context = this;

    if (!consumer_.Start(&trace)) {
        callback_ = nullptr;
        return false;
    }
    return true;
}

void EtwMonitor::Stop() noexcept {
    consumer_.Stop();
    callback_ = nullptr;
}

bool EtwMonitor::IsRunning() const {
    return consumer_.IsRunning();
}

void EtwMonitor::OnEvent(const EVENT_RECORD* event) {
    if (!event)
        return;
    const auto& header = event->EventHeader;
    if (!IsEqualGUID(header.ProviderId, display_timing_etw::kKernelProcessProvider)) {
        return;
    }
    if (header.EventDescriptor.Id != display_timing_etw::kKernelProcessStartEventId) {
        return;
    }

    uint32_t pid = 0;
    if (!display_timing_etw::ReadProperty(event, L"ProcessID", pid) || pid == 0) {
        pid = header.ProcessId;
    }
    if (pid == 0) {
        return;
    }

    std::string imageName;
    PROPERTY_DATA_DESCRIPTOR desc = {};
    desc.PropertyName = reinterpret_cast<ULONGLONG>(L"ImageName");
    desc.ArrayIndex = std::numeric_limits<ULONG>::max();
    ULONG propSize = 0;
    if (TdhGetPropertySize(const_cast<EVENT_RECORD*>(event), 0, nullptr, 1, &desc, &propSize) == ERROR_SUCCESS &&
        propSize >= sizeof(wchar_t)) {
        std::vector<std::byte> propBuf(propSize + sizeof(wchar_t));
        if (TdhGetProperty(const_cast<EVENT_RECORD*>(event), 0, nullptr, 1, &desc, propSize,
                           reinterpret_cast<PBYTE>(propBuf.data())) == ERROR_SUCCESS) {
            const auto* wideStr = reinterpret_cast<const wchar_t*>(propBuf.data());
            imageName = ExtractFileName(wideStr);
        }
    }

    if (imageName.empty()) {
        imageName = QueryProcessImageName(pid);
    }

    if (!imageName.empty() && callback_) {
        callback_(pid, imageName);
    }
}

}  // namespace ce::process_start
