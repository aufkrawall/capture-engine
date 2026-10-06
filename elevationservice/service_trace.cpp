#include "service_internal.h"
#include "captureengine/display_timing/display_timing_etw.h"
#include "common/logging/logging.h"

namespace ce::elevation {

DWORD TraceManager::Acquire() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (users_) {
        ++users_;
        return ERROR_SUCCESS;
    }
    GUID guid = kTraceGuid;
    PSID owner = nullptr;
    PSID system = nullptr;
    if (!ConvertStringSidToSidW(owner_.c_str(), &owner) || !ConvertStringSidToSidW(L"S-1-5-18", &system)) {
        if (owner)
            LocalFree(owner);
        return ERROR_INVALID_SID;
    }
    ULONG status = EventAccessControl(&guid, EventSecuritySetDACL, system, WMIGUID_ALL_ACCESS, TRUE);
    if (status == ERROR_SUCCESS)
        status = EventAccessControl(&guid, EventSecurityAddDACL, owner, kConsumerTraceRights, TRUE);
    LocalFree(owner);
    LocalFree(system);
    if (status != ERROR_SUCCESS) {
        EventAccessRemove(&guid);
        return status;
    }
    auto properties = display_timing_etw::MakeProperties(kTraceName);
    properties.Get()->Wnode.Guid = kTraceGuid;
    status = StartTraceW(&session_, kTraceName, properties.Get());
    if (status == ERROR_ALREADY_EXISTS) {
        auto existing = display_timing_etw::MakeProperties(kTraceName);
        if (ControlTraceW(0, kTraceName, existing.Get(), EVENT_TRACE_CONTROL_QUERY) == ERROR_SUCCESS &&
            IsEqualGUID(existing.Get()->Wnode.Guid, kTraceGuid)) {
            ControlTraceW(0, kTraceName, existing.Get(), EVENT_TRACE_CONTROL_STOP);
            status = StartTraceW(&session_, kTraceName, properties.Get());
        }
    }
    namespace dte = display_timing_etw;
    if (status == ERROR_SUCCESS) {
        // Shared with CE's own session so the two can never enable different sets.
        const dte::DisplayTimingProviderStatus providers = dte::EnableDisplayTimingProviders(session_);
        status = providers.required;
        const bool optionalMissing =
            providers.generatedFrames || providers.nvidiaSchedule || providers.inputRetrieval;
        if (status == ERROR_SUCCESS && optionalMissing)
            LogWarn("[ElevationService] Optional timing providers (generated=%lu NVIDIA=%lu input=%lu)",
                    providers.generatedFrames, providers.nvidiaSchedule, providers.inputRetrieval);
    }
    if (status == ERROR_SUCCESS) {
        stop_.Reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!stop_)
            status = GetLastError();
    }
    if (status != ERROR_SUCCESS) {
        Stop();
        return status;
    }
    try {
        flush_ = std::thread([this] {
            while (WaitForSingleObject(stop_.Get(), 8) == WAIT_TIMEOUT) {
                auto propertiesToFlush = dte::MakeProperties(kTraceName);
                FlushTraceW(session_, kTraceName, propertiesToFlush.Get());
            }
        });
    } catch (...) {
        Stop();
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    users_ = 1;
    LogInfo("[ElevationService] Privileged display trace started; consumer has read-only access");
    return ERROR_SUCCESS;
}

void TraceManager::Stop() {
    if (stop_)
        SetEvent(stop_.Get());
    if (flush_.joinable())
        flush_.join();
    if (session_) {
        auto properties = display_timing_etw::MakeProperties(kTraceName);
        ControlTraceW(session_, kTraceName, properties.Get(), EVENT_TRACE_CONTROL_STOP);
        session_ = 0;
    }
    GUID guid = kTraceGuid;
    EventAccessRemove(&guid);
    stop_.Reset();
}

void TraceManager::Release() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (users_ && --users_ == 0)
        Stop();
}
TraceManager::~TraceManager() {
    Stop();
}
}  // namespace ce::elevation
