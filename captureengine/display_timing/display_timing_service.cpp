#include "display_timing_service_internal.h"

using namespace display_timing_etw;

namespace {
constexpr int64_t kTimestampReorderWindowUs = 24'000;
constexpr uint64_t kHealthLogPeriodMs = 10'000;
constexpr DWORD kTraceFlushPeriodMs = 8;
// Display modes change rarely; a stale period only disables or narrows the
// refresh bound, because every bound is also limited by an observed blank.
constexpr uint64_t kRefreshPeriodQueryMs = 2'000;

}  // namespace

void DisplayTimingService::Impl::Start() {
    if (started_.exchange(true, std::memory_order_acq_rel))
        return;

    LARGE_INTEGER frequency = {};
    QueryPerformanceFrequency(&frequency);
    qpcFrequency_ = frequency.QuadPart;
    submissions_.SetMaxCompletionAge(kMaxSubmitToCompletionUs * qpcFrequency_ / 1'000'000);
    inputBursts_.SetMaximumGap(kInputRetrievalBurstGapUs * qpcFrequency_ / 1'000'000);
    nvidiaSchedule_.SetQpcFrequency(qpcFrequency_);
    outputs_.SetQpcFrequency(qpcFrequency_);
    RefreshDisplayPeriods();
    brokerConfigured_ = ce::elevation::ServiceEnabled();
    attemptedServicePid_ = ce::elevation::ServiceProcessId();
    // A service instance whose trace this process was not allowed to consume is not retried; the
    // local backend takes over until a new service instance appears.
    brokerTrace_ = brokerConfigured_ && attemptedServicePid_ != deniedServicePid_.load() &&
                   broker_.Connect(ce::elevation::ControllerPid()) && broker_.AcquireTrace();
    ULONG status = ERROR_SUCCESS;
    if (brokerTrace_) {
        wcscpy_s(sessionName_, ce::elevation::kTraceName);
        LogInfo("[DisplayTiming] Consuming the service-owned trace without controlling its session");
    } else {
        broker_.Disconnect();
        swprintf(sessionName_, std::size(sessionName_), L"CE_DisplayTiming_%08X", GetCurrentProcessId());
        status = ce::display_timing_startup::OpenSessionAndEnableProviders(&session_, sessionName_);
    }
    if (status != ERROR_SUCCESS) {
        SetStartupFailure(status);
        return;
    }

    EVENT_TRACE_LOGFILEW trace = {};
    trace.LoggerName = sessionName_;
    trace.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD |
                             PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    trace.EventRecordCallback = &EventRecordThunk;
    trace.BufferCallback = &BufferThunk;
    trace.Context = this;
    traceHandle_ = OpenTraceW(&trace);
    if (traceHandle_ == INVALID_PROCESSTRACE_HANDLE && brokerTrace_) {
        const DWORD consumerError = GetLastError();
        broker_.ReleaseTrace();
        broker_.Disconnect();
        brokerTrace_ = false;
        LogWarn("[DisplayTiming] Service trace could not be consumed (error=%lu); trying the local backend",
                consumerError);
        swprintf(sessionName_, std::size(sessionName_), L"CE_DisplayTiming_%08X", GetCurrentProcessId());
        const ULONG localStatus = ce::display_timing_startup::OpenSessionAndEnableProviders(&session_, sessionName_);
        if (localStatus != ERROR_SUCCESS) {
            SetStartupFailure(localStatus);
            return;
        }
        traceHandle_ = OpenTraceW(&trace);
    }
    if (traceHandle_ == INVALID_PROCESSTRACE_HANDLE) {
        SetStartupFailure(GetLastError());
        StopTraceSession();
        return;
    }

    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) {
        SetStartupFailure(GetLastError());
        CloseTrace(traceHandle_);
        traceHandle_ = INVALID_PROCESSTRACE_HANDLE;
        StopTraceSession();
        return;
    }

    startupStatus_.store(DisplayTimingStatus::Starting, std::memory_order_release);
    consuming_.store(true, std::memory_order_release);
    processThread_ = std::thread([this] {
        const ULONG traceStatus = ProcessTrace(&traceHandle_, 1, nullptr, nullptr);
        // ERROR_CANCELLED is the ordinary result of stopping the session.
        if (traceStatus != ERROR_SUCCESS && traceStatus != ERROR_CANCELLED) {
            startupStatus_.store(DisplayTimingStatus::Failed, std::memory_order_release);
            if (brokerTrace_)
                deniedServicePid_.store(attemptedServicePid_);
            LogWarn("[DisplayTiming] Event consumption stopped: %lu%s", traceStatus,
                    brokerTrace_ ? " (service trace; using the local backend until the service restarts)" : "");
        }
        consuming_.store(false, std::memory_order_release);
    });
    flushThread_ = std::thread([this] { FlushLoop(); });
    LogInfo("[DisplayTiming] Screen-change timing service started (flush=%lums reorder=%lldus "
            "timestampPolicy=event/no-grid graphTime=refresh-bounded)",
            kTraceFlushPeriodMs, static_cast<long long>(kTimestampReorderWindowUs));
}

void DisplayTimingService::Impl::UpdateTargets(const std::vector<DisplayTimingTarget>& targets) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (const auto& oldTarget : targets_) {
        const bool retained = std::any_of(targets.begin(), targets.end(), [&](const DisplayTimingTarget& target) {
            return target.output == oldTarget.output && target.sourcePid == oldTarget.sourcePid &&
                   target.rendererPid == oldTarget.rendererPid;
        });
        if (!retained && oldTarget.output) {
            oldTarget.output->Reset(0, 0, DisplayTimingStatus::Unavailable);
            outputs_.Forget(oldTarget.output);
        }
    }

    for (const auto& target : targets) {
        const bool unchanged = std::any_of(targets_.begin(), targets_.end(), [&](const DisplayTimingTarget& old) {
            return target.output == old.output && target.sourcePid == old.sourcePid &&
                   target.rendererPid == old.rendererPid;
        });
        if (!unchanged && target.output) {
            target.output->Reset(target.sourcePid, target.rendererPid,
                                 startupStatus_.load(std::memory_order_acquire));
            outputs_.Track(target.output);
        }
    }
    targets_ = targets;
}

ULONG WINAPI DisplayTimingService::Impl::BufferThunk(EVENT_TRACE_LOGFILEW* trace) {
    auto* self = static_cast<Impl*>(trace->Context);
    self->ObserveTraceLosses(trace->EventsLost);
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    self->DrainReady(now.QuadPart, false);
    return TRUE;
}

void DisplayTimingService::Impl::SetStartupFailure(ULONG error) {
    const DisplayTimingStatus status =
        error == ERROR_ACCESS_DENIED ? DisplayTimingStatus::AccessDenied : DisplayTimingStatus::Failed;
    startupStatus_.store(status, std::memory_order_release);
    ce::display_timing_startup::LogStartupFailure(error);
    broker_.ReleaseTrace();
    broker_.Disconnect();
    brokerTrace_ = false;
}

void DisplayTimingService::Impl::ObserveTraceLosses(ULONG eventsLost) {
    ULONG loggedTotal = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (eventsLost > observedTraceEventsLost_) {
            const ULONG newlyLost = eventsLost - observedTraceEventsLost_;
            observedTraceEventsLost_ = eventsLost;
            for (const auto& target : targets_) {
                if (target.output)
                    target.output->droppedTimestampCount.fetch_add(newlyLost, std::memory_order_relaxed);
            }
        }
        const uint64_t now = GetTickCount64();
        if (observedTraceEventsLost_ > loggedTraceEventsLost_ &&
            (lastTraceLossLogTime_ == 0 || now - lastTraceLossLogTime_ >= 10000)) {
            loggedTraceEventsLost_ = observedTraceEventsLost_;
            lastTraceLossLogTime_ = now;
            loggedTotal = loggedTraceEventsLost_;
        }
    }
    if (loggedTotal != 0)
        LogWarn("[DisplayTiming] Graphics event loss detected: total=%lu", loggedTotal);
}

void DisplayTimingService::Impl::SortPending() noexcept {
    std::sort(pendingTimestamps_.begin(), pendingTimestamps_.end(), [](const auto& a, const auto& b) {
        return a.timestamp != b.timestamp ? a.timestamp < b.timestamp : a.arrivalOrder < b.arrivalOrder;
    });
}

void DisplayTimingService::Impl::DrainReady(int64_t nowQpc, bool force) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (lastPruneQpc_ == 0 || nowQpc - lastPruneQpc_ >= qpcFrequency_ * 5) {
        PruneAssociations(nowQpc - qpcFrequency_ * 10);
        lastPruneQpc_ = nowQpc;
    }
    if (nowQpc - lastSubmissionPruneQpc_ >= qpcFrequency_ / 4)
        PruneSubmissions(nowQpc);
    if (pendingTimestamps_.empty())
        return;
    SortPending();
    const int64_t cutoff = nowQpc - (kTimestampReorderWindowUs * qpcFrequency_) / 1'000'000;
    const int64_t publishUs = DisplayTimingQpcToUs(nowQpc, qpcFrequency_);
    std::size_t consumed = 0;
    for (auto& pending : pendingTimestamps_) {
        if (!force && pending.timestamp > cutoff)
            break;
        if (ShouldPublish(pending)) {
            PublishPending(pending, publishUs);
            if (pending.completionKind != DisplayCompletionKind::Unconditional) {
                ++fallbackPublished_;
                correlation_.CommitFallback(pending);
            }
        } else {
            ++suppressedTimestamps_;
            if (pending.completionKind != DisplayCompletionKind::Unconditional)
                ++fallbackSuppressed_;
        }
        ++consumed;
    }
    pendingTimestamps_.erase(
        pendingTimestamps_.begin(),
        pendingTimestamps_.begin() + static_cast<std::vector<PendingTimestamp>::difference_type>(consumed));
}

void DisplayTimingService::Impl::DrainReadyNoexcept(int64_t nowQpc) noexcept {
    if (pendingTimestamps_.empty())
        return;
    SortPending();
    const int64_t publishUs = DisplayTimingQpcToUs(nowQpc, qpcFrequency_);
    for (const auto& pending : pendingTimestamps_)
        if (ShouldPublish(pending))
            PublishPending(pending, publishUs);
    pendingTimestamps_.clear();
}

void DisplayTimingService::Impl::PruneSubmissions(int64_t nowQpc) {
    const int64_t boundQpc =
        (kMaxSubmitToCompletionUs + kTimestampReorderWindowUs) * qpcFrequency_ / 1'000'000;
    const uint64_t expired = submissions_.PruneBefore(nowQpc - boundQpc);
    composed_.PruneBefore(nowQpc - boundQpc);
    lastSubmissionPruneQpc_ = nowQpc;
    // Only the tracked processes' own flips end the composed state; the
    // compositor's flips are what measure it.
    const auto transition = expiryMonitor_.Observe(expired, ownCompletions_, GetTickCount64());
    if (transition == DisplaySubmissionExpiryMonitor::Transition::Started) {
        const uint32_t processId = submissions_.lastExpiredProcessId();
        composed_.Begin(processId, FindCompositorProcessId(processId));
    } else if (transition == DisplaySubmissionExpiryMonitor::Transition::Stopped) {
        submissions_.EraseProcess(composed_.compositorPid());
        composed_.End();
    }
    LogSubmissionExpiryTransition(transition, submissions_.lastExpiredProcessId(), composed_.compositorPid(),
                                  expired, expiryMonitor_.lastCompleted());
}

void DisplayTimingService::Impl::PruneAssociations(int64_t cutoff) {
    correlation_.Prune(cutoff);
    nvidiaSchedule_.PruneBefore(cutoff);
}

bool DisplayTimingService::Impl::IsScreenTime(const PendingTimestamp& pending) {
    return pending.screenTimeResolved;
}

void DisplayTimingService::Impl::PublishPending(const PendingTimestamp& pending, int64_t publishUs) {
    DisplayTimingPublication sample;
    sample.processId = pending.processId;
    sample.timestampQpc = pending.timestamp;
    sample.presentStartQpc = pending.presentStartTimestamp;
    sample.screenTimeResolved = IsScreenTime(pending);
    sample.synchronizedFlip =
        pending.completionKind == DisplayCompletionKind::Sync && pending.synchronizedFlip;
    sample.displaySource = pending.displaySource;
    outputs_.Publish(
        targets_, sample, publishUs, [this](uint32_t source) { return refreshPeriods_.PeriodUs(source); },
        [this](uint32_t source, int64_t from, int64_t until) {
            return verticalBlanks_.FirstBlankInRange(source, from, until);
        });
}

void DisplayTimingService::Impl::RefreshDisplayPeriods() {
    const DisplayRefreshPeriods periods = QueryDisplayRefreshPeriods();
    std::lock_guard<std::mutex> lock(mutex_);
    lastRefreshQueryTime_ = GetTickCount64();
    if (refreshPeriodsLogged_ && periods == refreshPeriods_)
        return;
    refreshPeriods_ = periods;
    refreshPeriodsLogged_ = true;
    LogDisplayRefreshPeriods(periods);
}

bool DisplayTimingService::Impl::SnapshotHealth(DisplayTimingHealth& health) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t now = GetTickCount64();
    if (targets_.empty() || (lastHealthLogTime_ != 0 && now - lastHealthLogTime_ < kHealthLogPeriodMs))
        return false;
    const bool firstWindow = lastHealthLogTime_ == 0;
    lastHealthLogTime_ = now;
    if (firstWindow)
        return false;
    health.presents = submissions_.observedPresents();
    health.associations = submissions_.observedAssociations();
    health.expiredAssociations = submissions_.expiredAssociations();
    health.staleCompletions = submissions_.rejectedStaleCompletions();
    health.submissionsExpiring = expiryMonitor_.expiring();
    health.queued = queuedTimestamps_;
    health.published = outputs_.published();
    health.suppressed = suppressedTimestamps_;
    health.regressed = outputs_.regressed();
    health.payloadReceived = frameTypePayloadReceived_;
    health.payloadValid = frameTypePayloadValid_;
    health.payloadCorrelated = frameTypeCorrelated_;
    health.payloadPending = correlation_.pendingPayloads().size();
    health.payloadPendingObserved = frameTypePendingObserved_;
    health.authoritative = frameTypeAuthoritative_;
    health.payloadDuplicate = frameTypePayloadDuplicate_;
    health.payloadLate = frameTypePayloadLate_;
    health.fallbackPublished = fallbackPublished_;
    health.fallbackSuppressed = fallbackSuppressed_;
    health.inputRetrievals = inputBursts_.observed();
    health.inputBursts = inputBursts_.emitted();
    SetNvidiaFlipSchedule(health, nvidiaSchedule_, qpcFrequency_);
    SetComposedPresentation(health, composed_);
    health.completions = completionsBySource_;
    const uint32_t blankSource = verticalBlanks_.busiestSource();
    health.blankIntervalUs = DisplayTimingQpcToUs(verticalBlanks_.PeriodUs(blankSource), qpcFrequency_);
    health.blanksObserved = verticalBlanks_.observedBlanks(blankSource);
    health.blankClockPeriodic = verticalBlanks_.HasPeriodicCadence(blankSource);
    SetBlankIntervals(health, blankIntervals_);
    SetLatchIntervals(health, latchIntervals_);
    SnapshotIntervals(health);
    return true;
}

void DisplayTimingService::Impl::SnapshotIntervals(DisplayTimingHealth& health) {
    outputs_.Snapshot(health);
    SetRuntimeIntervals(health, runtimeIntervals_);
    runtimeIntervals_.StartWindow();
    blankIntervals_.StartWindow();
    latchIntervals_.StartWindow();
}

void DisplayTimingService::Impl::LogHealthIfDue() {
    DisplayTimingHealth health;
    if (SnapshotHealth(health))
        LogDisplayTimingHealth(health);
}

void DisplayTimingService::Impl::FlushLoop() {
    while (WaitForSingleObject(stopEvent_, kTraceFlushPeriodMs) == WAIT_TIMEOUT) {
        auto flushProperties = MakeProperties(sessionName_);
        if (!brokerTrace_)
            FlushTraceW(session_, sessionName_, flushProperties.Get());
        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);
        DrainReady(now.QuadPart, false);
        LogHealthIfDue();
        if (GetTickCount64() - lastRefreshQueryTime_ >= kRefreshPeriodQueryMs)
            RefreshDisplayPeriods();
    }
}

void DisplayTimingService::Impl::StopTraceSession() {
    if (session_ != 0) {
        auto stopProperties = MakeProperties(sessionName_);
        ControlTraceW(session_, sessionName_, stopProperties.Get(), EVENT_TRACE_CONTROL_STOP);
        session_ = 0;
    }
}

void DisplayTimingService::Impl::StopNoexcept() noexcept {
    if (stopEvent_)
        SetEvent(stopEvent_);
    if (flushThread_.joinable())
        flushThread_.join();
    StopTraceSession();
    if (brokerTrace_ && traceHandle_ != INVALID_PROCESSTRACE_HANDLE) {
        CloseTrace(traceHandle_);
    }
    if (processThread_.joinable())
        processThread_.join();
    if (traceHandle_ != INVALID_PROCESSTRACE_HANDLE) {
        if (!brokerTrace_) CloseTrace(traceHandle_);
        traceHandle_ = INVALID_PROCESSTRACE_HANDLE;
    }
    broker_.ReleaseTrace();
    broker_.Disconnect();
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    DrainReadyNoexcept(now.QuadPart);
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
    correlation_.Clear();
    nvidiaSchedule_.Clear();
    composed_.End();
    verticalBlanks_.Clear();
    submissions_.Clear();
}

DisplayTimingService::DisplayTimingService() : impl_(std::make_unique<Impl>()) {}

DisplayTimingService::~DisplayTimingService() = default;

void DisplayTimingService::Start() {
    impl_->Start();
}

void DisplayTimingService::UpdateTargets(const std::vector<DisplayTimingTarget>& targets) {
    impl_->UpdateTargets(targets);
}

bool DisplayTimingService::Impl::NeedsRestart() const {
    const bool configured = ce::elevation::ServiceEnabled();
    if (configured != brokerConfigured_) return true;
    if (brokerTrace_ && (!broker_.Connected() || !consuming_.load(std::memory_order_acquire))) return true;
    const uint32_t pid = configured ? ce::elevation::ServiceProcessId() : 0;
    return configured && !brokerTrace_ && pid != 0 && pid != attemptedServicePid_;
}

bool DisplayTimingService::NeedsRestart() const { return impl_->NeedsRestart(); }
