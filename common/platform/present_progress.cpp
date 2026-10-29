#include "present_progress.h"

#include <algorithm>

namespace ce::present_progress {

void Tracker::Track(const std::vector<DWORD>& processIds) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = processes_.begin(); it != processes_.end();) {
        if (std::find(processIds.begin(), processIds.end(), it->first) == processIds.end())
            it = processes_.erase(it);
        else
            ++it;
    }
    for (const DWORD processId : processIds) {
        if (processId != 0)
            processes_.try_emplace(processId);
    }
}

void Tracker::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Return counts stay monotonic: a recovery attempt compares them across a restart.
    for (auto& [processId, process] : processes_)
        process.threads.clear();
    totals_ = Totals{};
}

uint64_t Tracker::ToUs(int64_t qpcDelta) const {
    return qpcDelta <= 0 ? 0 : static_cast<uint64_t>(qpcDelta) * 1'000'000ull / static_cast<uint64_t>(frequency_);
}

Tracker::Thread& Tracker::ThreadOf(Process& process, DWORD threadId) {
    const auto found = process.threads.find(threadId);
    if (found != process.threads.end())
        return found->second;
    if (process.threads.size() >= kMaxThreadsPerProcess) {
        // Evict the quietest thread, preferring one without an open call.
        const auto quietest = std::min_element(process.threads.begin(), process.threads.end(),
                                               [](const auto& a, const auto& b) {
                                                   if (a.second.open != b.second.open)
                                                       return !a.second.open;
                                                   return a.second.lastEventQpc < b.second.lastEventQpc;
                                               });
        process.threads.erase(quietest);
    }
    return process.threads[threadId];
}

void Tracker::ObserveStart(DWORD processId, DWORD threadId, int64_t qpc, bool multiplane) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto process = processes_.find(processId);
    if (process == processes_.end())
        return;
    Thread& thread = ThreadOf(process->second, threadId);
    if (thread.lastStartQpc != 0)
        thread.lastIntervalUs = ToUs(qpc - thread.lastStartQpc);
    thread.lastStartQpc = qpc;
    thread.lastEventQpc = qpc;
    thread.open = true;
    thread.multiplane = multiplane;
    ++totals_.starts;
}

void Tracker::ObserveReturn(DWORD processId, DWORD threadId, int64_t qpc) {
    std::lock_guard<std::mutex> lock(mutex_);
    totals_.returnsDelivered = true;
    const auto process = processes_.find(processId);
    if (process == processes_.end())
        return;
    Thread& thread = ThreadOf(process->second, threadId);
    thread.lastEventQpc = qpc;
    thread.open = false;
    ++process->second.returns;
    ++totals_.returns;
}

void Tracker::ObserveLoss() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [processId, process] : processes_) {
        for (auto& [threadId, thread] : process.threads)
            thread.open = false;
    }
    ++totals_.losses;
}

Stall Tracker::Inspect(DWORD processId, int64_t nowQpc, uint64_t thresholdUs) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stall result;
    const auto process = processes_.find(processId);
    if (!totals_.returnsDelivered || process == processes_.end())
        return result;
    for (const auto& [threadId, thread] : process->second.threads) {
        if (!thread.open)
            continue;
        const uint64_t openUs = ToUs(nowQpc - thread.lastStartQpc);
        if (openUs < thresholdUs || (result.stalled && openUs <= result.openUs))
            continue;
        result.stalled = true;
        result.threadId = threadId;
        result.openUs = openUs;
        result.lastIntervalUs = thread.lastIntervalUs;
        result.multiplane = thread.multiplane;
    }
    return result;
}

uint64_t Tracker::Returns(DWORD processId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto process = processes_.find(processId);
    return process == processes_.end() ? 0 : process->second.returns;
}

Totals Tracker::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return totals_;
}

}  // namespace ce::present_progress
