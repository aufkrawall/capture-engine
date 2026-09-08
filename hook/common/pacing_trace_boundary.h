#pragma once
#include "pacing_trace.h"

namespace ce::pacing_trace {
enum class PresentStage : uint32_t { Proxy, Proxy1, Detour, Detour1, Forward, Forward1 };

// Backend injection lets tests exercise early returns and nested boundaries with
// a deterministic clock. IDs are thread-local; pair by (thread, id), not pointer.
template <typename Backend> class BoundaryScope {
public:
    BoundaryScope(PresentStage stage, const void* object, uint32_t sync, uint32_t flags)
        : active_(Backend::Enabled()), object_(object), stage_(static_cast<uint32_t>(stage)) {
        if (!active_) return;
        id_ = Backend::NextId();
        begin_ = Backend::Now();
        Emit(Kind::PresentBegin, begin_, sync, flags, 0);
    }
    ~BoundaryScope() { Close(); }
    BoundaryScope(const BoundaryScope&) = delete;
    BoundaryScope& operator=(const BoundaryScope&) = delete;
    void Forward(uint32_t sync, uint32_t flags) {
        if (active_) Emit(Kind::PresentForward, Backend::Now(), sync, flags, 0);
    }
    void Finish(uint32_t result) { result_ = result; hasResult_ = true; Close(); }
private:
    void Close() {
        if (!active_) return;
        const auto end = Backend::Now();
        Emit(Kind::PresentEnd, end, static_cast<uint64_t>(end - begin_), result_, hasResult_ ? 1 : 0);
        active_ = false;
    }
    void Emit(Kind kind, int64_t time, uint64_t a, uint64_t b, uint64_t c) {
        Backend::Write(kind, id_, object_, a, b, c, stage_, time);
    }
    bool active_ = false, hasResult_ = false;
    const void* object_ = nullptr;
    uint32_t stage_ = 0, result_ = 0;
    uint64_t id_ = 0;
    int64_t begin_ = 0;
};

struct TraceBoundaryBackend {
    static bool Enabled() { return ce::pacing_trace::Enabled(); }
    static uint64_t NextId() { static thread_local uint64_t next = 0; return ++next; }
    static int64_t Now();
    static void Write(Kind kind, uint64_t id, const void* object, uint64_t a, uint64_t b,
                      uint64_t c, uint32_t flags, int64_t time) {
        Record(kind, id, object, a, b, c, flags, time);
    }
};
#ifdef VK_LAYER_CE_OVERLAY
inline int64_t TraceBoundaryBackend::Now() { return 0; }
#endif
using PresentScope = BoundaryScope<TraceBoundaryBackend>;
}  // namespace ce::pacing_trace
