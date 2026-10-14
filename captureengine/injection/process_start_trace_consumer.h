#pragma once

#include <windows.h>
#include <evntrace.h>

#include <atomic>
#include <functional>
#include <thread>
#include <utility>

#include "common/logging/logging.h"

namespace ce::process_start {

// Own the consumer separately from event decoding. Controlled operations let
// regression tests exercise the same lifetime without a privileged session.
struct TraceOperations {
    std::function<bool()> acquire;
    std::function<TRACEHANDLE(EVENT_TRACE_LOGFILEW*)> open;
    std::function<ULONG(TRACEHANDLE)> consume;
    std::function<void(TRACEHANDLE)> close;
    std::function<void()> release;
};

class TraceConsumer {
public:
    explicit TraceConsumer(TraceOperations operations) : operations_(std::move(operations)) {}
    ~TraceConsumer() { Stop(); }
    TraceConsumer(const TraceConsumer&) = delete;
    TraceConsumer& operator=(const TraceConsumer&) = delete;

    bool Start(EVENT_TRACE_LOGFILEW* trace) {
        if (thread_.joinable())
            return false;
        if (!operations_.acquire()) {
            operations_.release();
            return false;
        }
        handle_ = operations_.open(trace);
        if (handle_ == INVALID_PROCESSTRACE_HANDLE) {
            LogWarn("[Inject] OpenTraceW failed for process discovery (error=%lu)", GetLastError());
            operations_.release();
            return false;
        }
        running_.store(true, std::memory_order_release);
        try {
            // Capture by value: Stop may run before the worker first enters ProcessTrace.
            thread_ = std::thread([this, handle = handle_] {
                const ULONG status = operations_.consume(handle);
                if (status != ERROR_SUCCESS && status != ERROR_CANCELLED)
                    LogWarn("[Inject] ETW process start trace stopped: %lu", status);
                running_.store(false, std::memory_order_release);
            });
        } catch (const std::system_error& error) {
            LogWarn("[Inject] Cannot start ETW consumer: %s", error.what());
            Stop();
            return false;
        }
        return true;
    }

    void Stop() {
        if (handle_ == INVALID_PROCESSTRACE_HANDLE)
            return;
        operations_.close(handle_);
        if (thread_.joinable())
            thread_.join();
        handle_ = INVALID_PROCESSTRACE_HANDLE;
        running_.store(false, std::memory_order_release);
        operations_.release();
    }

    bool IsRunning() const { return running_.load(std::memory_order_acquire); }

private:
    TraceOperations operations_;
    TRACEHANDLE handle_ = INVALID_PROCESSTRACE_HANDLE;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

}  // namespace ce::process_start
