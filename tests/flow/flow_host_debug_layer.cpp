// The D3D12 debug layer in the flow game, as a developer build runs it: enabled before the game's device (CE
// is already injected), every message logged into logs/<Suite.Test>/d3d12_debug.log with repeats metered per
// message ID, and counted by severity for the scenario invariant (flow_test_support.h). Messages arrive on
// any thread - the game's, CE's, the fake runtimes' presenter threads - and the tally is process-wide: one
// scenario runs per process.

#include "tests/flow/flow_host.h"

#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>

namespace ce::flow {
namespace {

struct MessageTally {
    uint64_t count = 0;
    D3D12_MESSAGE_SEVERITY severity = D3D12_MESSAGE_SEVERITY_MESSAGE;
    D3D12_MESSAGE_CATEGORY category = D3D12_MESSAGE_CATEGORY_MISCELLANEOUS;
};

struct DebugLayerLog {
    std::mutex mutex;
    FILE* file = nullptr;
    std::map<int, MessageTally> byId;
    D3D12DebugMessages messages;
    std::atomic<int> frame{0};
};

DebugLayerLog& TheLog() {
    static DebugLayerLog log;
    return log;
}

// Each message ID's first occurrences, then every 500th: a per-frame warning cannot flood the log.
constexpr uint64_t kLoggedOccurrencesPerId = 20;
constexpr uint64_t kOccurrenceStride = 500;

const char* SeverityName(D3D12_MESSAGE_SEVERITY severity) {
    switch (severity) {
        case D3D12_MESSAGE_SEVERITY_CORRUPTION:
            return "CORRUPTION";
        case D3D12_MESSAGE_SEVERITY_ERROR:
            return "ERROR";
        case D3D12_MESSAGE_SEVERITY_WARNING:
            return "WARNING";
        case D3D12_MESSAGE_SEVERITY_INFO:
            return "INFO";
        default:
            return "MESSAGE";
    }
}

void __stdcall OnDebugMessage(D3D12_MESSAGE_CATEGORY category, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id,
                              const char* description, void* /*context*/) {
    DebugLayerLog& log = TheLog();
    const int frame = log.frame.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(log.mutex);
    MessageTally& tally = log.byId[static_cast<int>(id)];
    ++tally.count;
    tally.severity = severity;
    tally.category = category;
    ++log.messages.total;
    const bool failure = severity == D3D12_MESSAGE_SEVERITY_CORRUPTION || severity == D3D12_MESSAGE_SEVERITY_ERROR;
    if (severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
        ++log.messages.corruptions;
    else if (severity == D3D12_MESSAGE_SEVERITY_ERROR)
        ++log.messages.errors;
    else if (severity == D3D12_MESSAGE_SEVERITY_WARNING)
        ++log.messages.warnings;
    if (failure && log.messages.firstFailure.empty()) {
        char text[96];
        std::snprintf(text, sizeof(text), "frame %d %s id=%d: ", frame, SeverityName(severity), static_cast<int>(id));
        log.messages.firstFailure = text + std::string(description ? description : "");
    }
    if (log.file && (tally.count <= kLoggedOccurrencesPerId || tally.count % kOccurrenceStride == 0)) {
        std::fprintf(log.file, "frame %d T%lu %s id=%d category=%d #%llu: %s\n", frame, GetCurrentThreadId(),
                     SeverityName(severity), static_cast<int>(id), static_cast<int>(category),
                     static_cast<unsigned long long>(tally.count), description ? description : "");
        if (failure)
            std::fflush(log.file);
    }
}

}  // namespace

D3D12DebugMessages D3D12DebugMessagesSoFar() {
    DebugLayerLog& log = TheLog();
    std::lock_guard<std::mutex> lock(log.mutex);
    return log.messages;
}

bool FlowGame::EnableD3D12DebugLayer() {
    ComPtr<ID3D12Debug> debug;
    const HRESULT hr = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
    if (FAILED(hr))
        return Fail("D3D12GetDebugInterface(ID3D12Debug) - the debug layer needs the Graphics Tools feature", hr);
    debug->EnableDebugLayer();
    return true;
}

bool FlowGame::WatchD3D12DebugMessages() {
    DebugLayerLog& log = TheLog();
    {
        std::lock_guard<std::mutex> lock(log.mutex);
        if (!log.file) {
            CreateDirectoryA(logDirectory_.c_str(), nullptr);
            log.messages.logPath = logDirectory_ + "\\d3d12_debug.log";
            fopen_s(&log.file, log.messages.logPath.c_str(), "w");
        }
    }
    HRESULT hr = device_.As(&infoQueue_);
    if (FAILED(hr))
        return Fail("QueryInterface(ID3D12InfoQueue1)", hr);
    hr = infoQueue_->RegisterMessageCallback(OnDebugMessage, D3D12_MESSAGE_CALLBACK_IGNORE_FILTERS, nullptr,
                                             &debugMessageCookie_);
    if (FAILED(hr))
        return Fail("ID3D12InfoQueue1::RegisterMessageCallback", hr);
    std::lock_guard<std::mutex> lock(log.mutex);
    log.messages.watched = true;
    return true;
}

void FlowGame::NoteD3D12DebugFrame() {
    TheLog().frame.store(frame_, std::memory_order_relaxed);
}

void FlowGame::StopWatchingD3D12DebugMessages() {
    if (infoQueue_ && debugMessageCookie_)
        infoQueue_->UnregisterMessageCallback(debugMessageCookie_);
    debugMessageCookie_ = 0;
    infoQueue_.Reset();
    DebugLayerLog& log = TheLog();
    std::lock_guard<std::mutex> lock(log.mutex);
    if (!log.file)
        return;
    std::fprintf(log.file, "--- %llu messages: %llu CORRUPTION, %llu ERROR, %llu WARNING; per ID:\n",
                 static_cast<unsigned long long>(log.messages.total),
                 static_cast<unsigned long long>(log.messages.corruptions),
                 static_cast<unsigned long long>(log.messages.errors),
                 static_cast<unsigned long long>(log.messages.warnings));
    for (const auto& [id, tally] : log.byId) {
        std::fprintf(log.file, "id=%d %s category=%d x%llu\n", id, SeverityName(tally.severity),
                     static_cast<int>(tally.category), static_cast<unsigned long long>(tally.count));
    }
    std::fclose(log.file);
    log.file = nullptr;
}

}  // namespace ce::flow
