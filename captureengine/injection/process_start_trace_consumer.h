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
    // `prefix` and `purpose` name the consumer in its log lines.
    explicit TraceConsumer(TraceOperations operations, const char* prefix = "[Inject]",
                           const char* purpose = "process discovery")
        : operations_(std::move(operations)), prefix_(prefix), purpose_(purpose) {}
    ~TraceConsumer() noexcept { Stop(); }
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
            LogWarn("%s OpenTraceW failed for %s (error=%lu)", prefix_, purpose_, GetLastError());
            operations_.release();
            return false;
        }
        running_.store(true, std::memory_order_release);
        try {
            // Capture by value: Stop may run before the worker first enters ProcessTrace. The sink is the
            // starting thread's, so the consumer's lines land in the role's log (DR-40).
            thread_ = std::thread([this, handle = handle_, logSink = ce::log::CurrentSink()] {
                ce::log::SinkScope threadLog(logSink);
                const ULONG status = operations_.consume(handle);
                if (status != ERROR_SUCCESS && status != ERROR_CANCELLED)
                    LogWarn("%s ETW trace for %s stopped: %lu", prefix_, purpose_, status);
                running_.store(false, std::memory_order_release);
            });
        } catch (const std::system_error& error) {
            LogWarn("%s Cannot start ETW consumer for %s: %s", prefix_, purpose_, error.what());
            Stop();
            return false;
        }
        return true;
    }

    void Stop() noexcept {
        if (handle_ == INVALID_PROCESSTRACE_HANDLE)
            return;
        try {
            if (operations_.close)
                operations_.close(handle_);
            if (thread_.joinable())
                thread_.join();
            handle_ = INVALID_PROCESSTRACE_HANDLE;
            running_.store(false, std::memory_order_release);
            if (operations_.release)
                operations_.release();
        } catch (...) {
            handle_ = INVALID_PROCESSTRACE_HANDLE;
            running_.store(false, std::memory_order_release);
        }
    }

    bool IsRunning() const { return running_.load(std::memory_order_acquire); }

private:
    TraceOperations operations_;
    const char* prefix_;
    const char* purpose_;
    TRACEHANDLE handle_ = INVALID_PROCESSTRACE_HANDLE;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

}  // namespace ce::process_start
