#include "display_stall_trace_sealer.h"

#include <windows.h>

#include <cwchar>
#include <iterator>

#include "common/logging/logging.h"

namespace {

std::string Narrow(const std::wstring& value) {
    if (value.empty())
        return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0,
                                          nullptr, nullptr);
    std::string narrow(bytes > 0 ? static_cast<size_t>(bytes) : 0, '\0');
    if (bytes > 0)
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), narrow.data(), bytes, nullptr,
                            nullptr);
    return narrow;
}

unsigned long MoveSealedFile(const std::wstring& from, const std::wstring& to) {
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH) ? ERROR_SUCCESS
                                                                                                 : GetLastError();
}

}  // namespace

void DisplayStallTraceSealer::Start(Seal seal, std::wstring sessionFolder, Move move) {
    Stop();
    seal_ = std::move(seal);
    move_ = move ? std::move(move) : Move(&MoveSealedFile);
    folder_ = std::move(sessionFolder);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = false;
        pending_.reset();
    }
    thread_ = std::thread([this] { Run(); });
}

void DisplayStallTraceSealer::Request(const display_stall_trace::SealRequest& request) {
    if (!request)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!thread_.joinable() || stop_)
        return;
    if (pending_)
        ++replaced_;
    pending_ = request;
    wake_.notify_all();
}

void DisplayStallTraceSealer::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        pending_.reset();
    }
    wake_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

void DisplayStallTraceSealer::WaitIdle() {
    std::unique_lock<std::mutex> lock(mutex_);
    wake_.wait(lock, [this] { return (!pending_ && !busy_) || stop_; });
}

std::wstring DisplayStallTraceSealer::DestinationName(const display_stall_trace::SealRequest& request) {
    wchar_t name[96] = {};
    swprintf(name, std::size(name), L"display_stall_%u_ep%u_%hs.etl", request.processId, request.episode,
             display_stall_trace::SealReasonName(request.reason));
    return name;
}

void DisplayStallTraceSealer::Run() {
    for (;;) {
        display_stall_trace::SealRequest request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || pending_.has_value(); });
            if (stop_)
                return;
            request = *pending_;
            pending_.reset();
            busy_ = true;
        }
        SealOne(request);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            busy_ = false;
        }
        wake_.notify_all();
    }
}

void DisplayStallTraceSealer::SealOne(const display_stall_trace::SealRequest& request) {
    ce::elevation::SealedDisplayTrace sealed;
    const bool exchanged = seal_ && seal_(sealed);
    if (!exchanged || sealed.status != ERROR_SUCCESS || !ce::elevation::IsSealedDisplayTracePath(sealed.path)) {
        LogWarn("[DisplayStallTrace] seal failed ep=%u pid=%u reason=%s silenceMs=%llu exchanged=%d status=%u "
                "restart=%u validPath=%d",
                request.episode, request.processId, display_stall_trace::SealReasonName(request.reason),
                static_cast<unsigned long long>(request.silenceMs), exchanged ? 1 : 0, sealed.status,
                sealed.restartStatus, ce::elevation::IsSealedDisplayTracePath(sealed.path) ? 1 : 0);
        return;
    }
    const std::wstring source(sealed.path.data());
    const std::wstring destination = folder_ + L"\\" + DestinationName(request);
    const unsigned long moved = move_(source, destination);
    if (moved != ERROR_SUCCESS) {
        LogWarn("[DisplayStallTrace] move failed ep=%u error=%lu from=%s to=%s (the service removes it after 10 min)",
                request.episode, moved, Narrow(source).c_str(), Narrow(destination).c_str());
        return;
    }
    LogInfo("[DisplayStallTrace] sealed ep=%u pid=%u reason=%s silenceMs=%llu silenceStartQpcUs=%lld presents=%u "
            "file=%s bytes=%llu eventsLost=%u buffersLost=%u restart=%u replaced=%u",
            request.episode, request.processId, display_stall_trace::SealReasonName(request.reason),
            static_cast<unsigned long long>(request.silenceMs), static_cast<long long>(request.silenceStartUs),
            request.presents,
            Narrow(DestinationName(request)).c_str(), static_cast<unsigned long long>(sealed.bytes), sealed.eventsLost,
            sealed.buffersLost, sealed.restartStatus, replaced_.load());
}
