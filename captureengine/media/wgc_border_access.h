#pragma once

// Whether WGC captures may hide the yellow capture border (GraphicsCaptureAccessKind::Borderless).
//
// Windows answers the access request on its own schedule and resumes it on the apartment that asked. A blocking
// wait on a single-threaded apartment therefore deadlocks whenever the answer is not immediate: the engine thread
// (STA) froze in its first desktop screenshot that way, and every later hotkey and the shutdown with it (user
// report 2026-10-24: an HDR desktop, where desktop screenshots capture through WGC on the engine thread). The
// request runs on a thread of its own in the multithreaded apartment, and no capture thread ever waits for it:
// Windows accepts IsBorderRequired(false) before it answered and ignores the setting unless access is granted.
// tools/check_winrt_blocking_waits.py keeps every other blocking WinRT wait off the product's threads.

#include <functional>
#include <memory>

namespace ce::wgc {

enum class BorderAccess : int { Pending = 0, Allowed, Denied, Unavailable };

const char* BorderAccessName(BorderAccess access);

class BorderAccessRequest {
public:
    // Asks the question. It runs once, on the request's own thread in the multithreaded apartment; an exception
    // counts as Unavailable.
    using Ask = std::function<BorderAccess()>;
    explicit BorderAccessRequest(Ask ask);
    ~BorderAccessRequest();
    BorderAccessRequest(const BorderAccessRequest&) = delete;
    BorderAccessRequest& operator=(const BorderAccessRequest&) = delete;

    // Starts the request unless it already started, and returns at once; any thread, any apartment.
    void Start();
    // Pending until the answer arrived.
    BorderAccess State() const;
    // Waits up to timeoutMs for the answer; true once it arrived. Capture threads never wait for it.
    bool WaitAnswered(unsigned long timeoutMs) const;

    struct Shared;

private:
    std::shared_ptr<Shared> shared_;  // the request thread holds its own reference
};

// The process-wide request, asking Windows.
BorderAccessRequest& SystemBorderAccess();

}  // namespace ce::wgc
