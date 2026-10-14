#include "process_start_etw.h"

#include <cwchar>
#include <limits>
#include <vector>

#include "captureengine/display_timing/display_timing_etw.h"
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

EtwMonitor::EtwMonitor() = default;

EtwMonitor::~EtwMonitor() {
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
    if (running_.load(std::memory_order_acquire) || !callback) {
        return false;
    }
    if (!ce::elevation::ServiceEnabled()) {
        return false;
    }

    callback_ = std::move(callback);

    if (!broker_.Connect(ce::elevation::ControllerPid()) || !broker_.AcquireTrace()) {
        broker_.Disconnect();
        callback_ = nullptr;
        return false;
    }

    wchar_t sessionName[display_timing_etw::kTraceSessionNameCapacity] = {};
    wcscpy_s(sessionName, ce::elevation::kTraceName);

    EVENT_TRACE_LOGFILEW trace = {};
    trace.LoggerName = sessionName;
    trace.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD |
                             PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    trace.EventRecordCallback = &EventRecordCallback;
    trace.BufferCallback = &BufferCallback;
    trace.Context = this;

    traceHandle_ = OpenTraceW(&trace);
    if (traceHandle_ == INVALID_PROCESSTRACE_HANDLE) {
        const DWORD err = GetLastError();
        LogWarn("[Inject] OpenTraceW failed for elevation process trace (error=%lu)", err);
        broker_.ReleaseTrace();
        broker_.Disconnect();
        callback_ = nullptr;
        return false;
    }

    running_.store(true, std::memory_order_release);
    processThread_ = std::thread([this] {
        const ULONG status = ProcessTrace(&traceHandle_, 1, nullptr, nullptr);
        if (status != ERROR_SUCCESS && status != ERROR_CANCELLED) {
            LogWarn("[Inject] ETW process start trace stopped: %lu", status);
        }
        running_.store(false, std::memory_order_release);
    });

    return true;
}

void EtwMonitor::Stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel) && traceHandle_ == INVALID_PROCESSTRACE_HANDLE) {
        return;
    }
    if (traceHandle_ != INVALID_PROCESSTRACE_HANDLE) {
        CloseTrace(traceHandle_);
        traceHandle_ = INVALID_PROCESSTRACE_HANDLE;
    }
    if (processThread_.joinable()) {
        processThread_.join();
    }
    broker_.ReleaseTrace();
    broker_.Disconnect();
    callback_ = nullptr;
}

bool EtwMonitor::IsRunning() const {
    return running_.load(std::memory_order_acquire);
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
