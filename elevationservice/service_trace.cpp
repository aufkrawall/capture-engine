#include "service_internal.h"
#include "common/platform/display_timing_etw.h"
#include "common/logging/logging.h"

namespace ce::elevation {

// Opens the shared session with only the process provider. Display providers
// are added by the first display lease (EnableDisplayLocked) and removed with the
// last one, so an idle machine is not traced for display timing just because
// process discovery is watching for game starts.
DWORD TraceManager::StartSessionLocked() {
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
        const dte::DisplayTimingProviderStatus providers =
            dte::EnableDisplayTimingProviders(session_, dte::ProviderGroup::ProcessStart);
        // A missing process provider leaves the session usable for display
        // timing; process discovery reads the capability and falls back.
        processStartAvailable_ = providers.kernelProcess == ERROR_SUCCESS;
        if (!processStartAvailable_)
            LogWarn("[ElevationService] Process-start provider unavailable (kernelProcess=%lu)", providers.kernelProcess);
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
    LogInfo("[ElevationService] Privileged trace started (process provider=%d); consumers have read-only access",
            processStartAvailable_ ? 1 : 0);
    return ERROR_SUCCESS;
}

DWORD TraceManager::EnableDisplayLocked() {
    namespace dte = display_timing_etw;
    const dte::DisplayTimingProviderStatus providers =
        dte::EnableDisplayTimingProviders(session_, dte::ProviderGroup::Display);
    if (providers.required != ERROR_SUCCESS) {
        dte::DisableDisplayTimingProviders(session_);
        LogWarn("[ElevationService] Display timing providers could not be enabled: %lu", providers.required);
        return providers.required;
    }
    if (providers.generatedFrames || providers.nvidiaSchedule || providers.inputRetrieval)
        LogWarn("[ElevationService] Optional timing providers (generated=%lu NVIDIA=%lu input=%lu)",
                providers.generatedFrames, providers.nvidiaSchedule, providers.inputRetrieval);
    LogInfo("[ElevationService] Display timing providers enabled for the first display lease");
    return ERROR_SUCCESS;
}

DWORD TraceManager::Acquire(TraceKind kind) {
    std::lock_guard<std::mutex> guard(mutex_);
    const bool display = kind == TraceKind::Display;
    const TraceLeaseCounter::AcquirePlan plan = leases_.PlanAcquire(display);
    if (plan.startSession) {
        const DWORD status = StartSessionLocked();
        if (status != ERROR_SUCCESS)
            return status;
    }
    if (plan.enableDisplay) {
        const DWORD status = EnableDisplayLocked();
        if (status != ERROR_SUCCESS) {
            if (plan.startSession)
                Stop();
            return status;
        }
    }
    leases_.CommitAcquire(display);
    return ERROR_SUCCESS;
}

void TraceManager::Stop() {
    processStartAvailable_ = false;
    leases_.Reset();
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

void TraceManager::Release(TraceKind kind) {
    std::lock_guard<std::mutex> guard(mutex_);
    const TraceLeaseCounter::ReleasePlan plan = leases_.Release(kind == TraceKind::Display);
    if (plan.disableDisplay) {
        display_timing_etw::DisableDisplayTimingProviders(session_);
        LogInfo("[ElevationService] Display timing providers disabled; the process provider stays on");
    }
    if (plan.stopSession)
        Stop();
}
bool TraceManager::ProcessStartAvailable() {
    std::lock_guard<std::mutex> guard(mutex_);
    return session_ != 0 && processStartAvailable_;
}
TraceManager::~TraceManager() {
    Stop();
}
}  // namespace ce::elevation
