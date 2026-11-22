#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace ce::media {

// Captures bounded libav AV_LOG_VERBOSE/AV_LOG_DEBUG lines emitted on the current
// thread during an avcodec_open2 call. FFmpeg's AMF trace writer routes AMF's own
// AMF_TRACE_WARNING/ERROR lines through av_log(..., AV_LOG_DEBUG, ...), which
// normal AV_LOG_INFO logging discards. Keeping the most recent lines in a thread-
// local scope costs one TLS check on the hot path, emits zero noise on success,
// and lets open failures log the exact AMF/driver property rejection.
class ScopedAvOpenDebugCapture {
public:
    explicit ScopedAvOpenDebugCapture(size_t maxLines = 32);
    ~ScopedAvOpenDebugCapture();

    ScopedAvOpenDebugCapture(const ScopedAvOpenDebugCapture&) = delete;
    ScopedAvOpenDebugCapture& operator=(const ScopedAvOpenDebugCapture&) = delete;

    const std::vector<std::string>& Lines() const { return lines_; }
    size_t DroppedCount() const { return droppedCount_; }
    bool Empty() const { return lines_.empty(); }

    // Concise single-line tail summary (for probe rejection strings), truncated to maxChars.
    std::string LastLineTruncated(size_t maxChars = 160) const;

    static bool HasActiveCapture();
    static bool TryRecordForCurrentThread(const std::string& redactedLine);

private:
    ScopedAvOpenDebugCapture* previous_ = nullptr;
    size_t maxLines_ = 32;
    size_t droppedCount_ = 0;
    std::vector<std::string> lines_;
};

}  // namespace ce::media
