#pragma once

#include <windows.h>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "common/config/application_profile.h"
#include "common/platform/present_progress.h"

// Background presentation-stall recovery for enabled process profiles.
//
// With multi-plane overlays, a background window's presentation can stop
// until the window's geometry changes: its Present call never returns, the
// game's own hang detection may then kill it, and a manual resize unsticks it.
// The pump watches the profiled processes' Present calls through the shared
// display trace (present_progress::Tracker). When one stays open past the GPU
// timeout while the process is in the background, it changes the window's
// geometry the way a user would, from outside the process: first a 1 px move
// and back, then a 1 px narrower size and back, each queued asynchronously on
// the window's own thread without activation or z-order change. Every step and
// its outcome is logged, so each real occurrence shows which step helped.
namespace ce::window_heartbeat {

constexpr uint64_t kIntervalMs = 250;

struct Window {
    HWND handle = nullptr;
    DWORD processId = 0;
    DWORD threadId = 0;
    std::string processName;  // resolved by the pump, cached per process
    RECT rect = {};           // window rectangle, physical screen coordinates
    bool visible = false;
    bool minimized = false;
    bool cloaked = true;
    bool frame = false;  // top-level, not a child, tool or no-activate window
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual DWORD ForegroundProcessId() = 0;
    virtual bool Enumerate(std::vector<Window>& windows) = 0;
    virtual std::string ProcessName(DWORD processId) = 0;
    virtual DWORD SendNull(const Window& window, ULONG_PTR token, SENDASYNCPROC callback) = 0;
    // Queues the rectangle on the window's own thread; the window keeps its
    // activation and z-order. ERROR_SUCCESS means queued, not applied.
    virtual DWORD Place(const Window& window, const RECT& rect) = 0;
};

enum class Step : uint8_t { Move, Resize };

struct TickResult {
    bool ran = false;
    bool enumerated = false;        // `background` is current; a failed enumeration leaves the trace as it was
    std::vector<DWORD> background;  // profiled processes with a background window: the trace's subjects
    uint32_t windows = 0;           // profiled processes with an eligible window
    uint32_t episodes = 0;          // stalls under recovery after this tick
    uint32_t failed = 0;
    DWORD lastError = ERROR_SUCCESS;
};

// The worker owns this pump and its completion callbacks on one thread.
// Explicit time and platform inputs let tests drive every step without sleeps.
class Pump {
public:
    Pump(DWORD ownerProcessId, uint64_t stallThresholdUs)
        : ownerProcessId_(ownerProcessId), stallThresholdUs_(stallThresholdUs) {}
    void Configure(const std::vector<ApplicationProfile>& profiles);
    bool Enabled() const { return !processNames_.empty(); }
    TickResult Tick(uint64_t nowMs, int64_t nowQpc, Backend& backend, present_progress::Tracker& progress,
                    SENDASYNCPROC callback);
    // The probe's answer: the window thread processed its sent messages.
    void Complete(HWND window, ULONG_PTR token, uint64_t nowMs);
    DWORD WaitMs(uint64_t nowMs) const;

private:
    enum class Phase : uint8_t { Nudged, Restoring, Observing, Exhausted };
    struct Probe {
        DWORD processId;
        DWORD threadId;
        ULONG_PTR token;
    };
    struct Episode {
        Window window;  // as found at detection: its rectangle is what every step restores
        DWORD presentThread = 0;
        int64_t presentSinceQpc = 0;  // the unanswered Present call
        RECT target = {};
        Step step = Step::Move;
        Phase phase = Phase::Nudged;
        uint64_t detectedMs = 0;
        uint64_t phaseMs = 0;
        uint64_t returnsAtDetection = 0;
        ULONG_PTR probe = 0;
        uint64_t probeSentMs = 0;
        uint64_t probeAnsweredMs = UINT64_MAX;
        uint64_t firstReplyMs = UINT64_MAX;  // the first probe's round trip
    };

    std::map<DWORD, Window> SelectTargets(Backend& backend, const std::vector<Window>& windows);
    void Begin(const Window& window, const present_progress::Stall& stall, uint64_t nowMs, Backend& backend,
               present_progress::Tracker& progress, SENDASYNCPROC callback, TickResult& result);
    // False once the episode is over.
    bool Advance(Episode& episode, const Window& current, uint64_t nowMs, Backend& backend,
                 present_progress::Tracker& progress, SENDASYNCPROC callback, TickResult& result);
    bool Nudge(Episode& episode, Step step, uint64_t nowMs, Backend& backend, TickResult& result);
    // A WM_NULL whose answer shows the window thread pumped messages after the nudge was queued.
    void SendProbe(Episode& episode, uint64_t nowMs, Backend& backend, SENDASYNCPROC callback, TickResult& result);

    DWORD ownerProcessId_;
    uint64_t stallThresholdUs_;
    std::vector<std::string> processNames_;
    std::map<DWORD, std::string> names_;  // process identity cache, pruned to enumerated processes
    std::map<DWORD, Episode> episodes_;   // by process
    // The last stalled call each process got an attempt for: one attempt per
    // stall, also when it ended early (geometry changed by someone else).
    std::map<DWORD, std::pair<DWORD, int64_t>> attempted_;
    std::map<HWND, Probe> probes_;        // one outstanding WM_NULL per window
    ULONG_PTR nextToken_ = 0;
    uint64_t lastTickMs_ = 0;
    bool hasTicked_ = false;
};

// Asynchronous system-message delivery, with a fresh HWND/thread/process/focus
// check. A successful return means queued, not that the recipient made progress.
DWORD SendAsyncNull(const Window& window, ULONG_PTR token, SENDASYNCPROC callback, DWORD foregroundPid = 0);
// Queues a geometry change on the window's thread (SWP_ASYNCWINDOWPOS) after a fresh HWND/thread/process
// check. The foreground decision is the pump's: a restore must still reach a window the user activated.
DWORD PlaceAsync(const Window& window, const RECT& rect);
// The graphics kernel's GPU timeout (TdrDelay, 2 s when unset): no healthy Present stays open longer.
uint64_t GpuTimeoutUs();

// Holds the Present trace only while a profiled process has a background
// window. A refused start is retried when that set changes or after a running
// trace ended on its own (service restart), never on every tick.
class FeedControl {
public:
    explicit FeedControl(present_progress::Feed* feed) : feed_(feed) {}
    void Steer(const std::vector<DWORD>& background, present_progress::Tracker& tracker);
    bool Live() const { return live_; }

private:
    present_progress::Feed* feed_;
    std::vector<DWORD> attempted_;
    bool live_ = false;
};

class Service {
public:
    // `feed` delivers the Present events; without one, no stall can be seen.
    explicit Service(std::unique_ptr<present_progress::Feed> feed = nullptr);
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
    bool UpdateProfiles(const std::vector<ApplicationProfile>& profiles);
    void Stop();

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace ce::window_heartbeat
