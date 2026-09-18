#pragma once

/*
 * Unelevated process-start notification.
 *
 * `Win32_ProcessStartTrace` is the cheap, event-driven source, but subscribing
 * to it requires membership of the Administrators group; an ordinary CE run gets
 * WBEM_E_ACCESS_DENIED (0x80041003) and has to fall back. The fallback used to
 * be `SELECT * FROM __InstanceCreationEvent WITHIN 0.5 WHERE TargetInstance ISA
 * 'Win32_Process'`, which does not poll inside CE at all: it asks the WMI
 * service to enumerate and fully materialise every Win32_Process instance twice
 * a second, for the whole session, and diff them. Session 20260918_162809 ran
 * that way for about four hours.
 *
 * This is the same job done in CE's own process against the native API the WMI
 * provider itself sits on. One NtQuerySystemInformation call returns every
 * process's id and image name in a single buffer, with none of the per-instance
 * property materialisation (command line, owner, paths) the WMI class performs
 * and CE never reads.
 *
 * What this is NOT a fix for: injection latency. The WMI fallback notified CE
 * 419 ms after Alan Wake 2 started in that session, and it cost nothing - the
 * game did not create its D3D12 swapchain until 4.95 s after CE's hooks were
 * fully installed. Detection speed has margin to spare in a real title; the
 * reason to stop asking WMI is the load, not the lateness.
 */

#include <windows.h>

#include <functional>
#include <string>

namespace ce::process_start {

// Called once per newly observed process, on the poller's own thread.
using StartCallback = std::function<void(DWORD pid, const std::string& imageName)>;

// Polls for process starts until Stop(). The first sweep establishes the
// baseline and reports nothing: every process alive when CE started is the
// existing-process scan's business, not a "start".
class Poller {
public:
    Poller() = default;
    ~Poller();

    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    // `intervalMs` is clamped to a sane range. Returns false when the thread
    // could not be created, in which case nothing is running.
    bool Start(StartCallback callback, unsigned intervalMs);

    // Stops and joins. Safe to call when never started, and safe to call twice.
    void Stop();

    bool IsRunning() const;

private:
    void Run();

    StartCallback callback_;
    unsigned intervalMs_ = 0;
    HANDLE thread_ = nullptr;
    HANDLE stopEvent_ = nullptr;
};

// The cadence CE polls at when it has to. Chosen against what the interval
// actually buys: the previous WMI fallback cost a full 500 ms of detection lag
// and still left seconds of margin before a real game's first swapchain, so
// there is nothing to win by polling aggressively - and a system-wide sweep is
// exactly the kind of work that should stay rare.
inline constexpr unsigned kDefaultPollIntervalMs = 250;
inline constexpr unsigned kMinPollIntervalMs = 50;
inline constexpr unsigned kMaxPollIntervalMs = 2000;

inline constexpr unsigned ClampPollIntervalMs(unsigned requested) {
    return requested < kMinPollIntervalMs   ? kMinPollIntervalMs
           : requested > kMaxPollIntervalMs ? kMaxPollIntervalMs
                                            : requested;
}

}  // namespace ce::process_start
