#pragma once

#include <windows.h>

// One bounded wait of a controller-owned service (logger, sensors) on the things
// that end it: the controller's shutdown event, the controller process itself,
// and the stop event of the role's host (a runtime thread host, or a helper's
// console handler; DR-38). Waiting on the event alone let a service outlive a controller
// that exited hard (crash, TerminateProcess): nothing ever set the event, so an
// orphaned logger kept draining the shared log rings and competed with the next
// controller's logger for them.

namespace ce::service_lifetime {

enum class WaitOutcome {
    kTimeout,           // keep servicing (timeout or optional activity)
    kStopRequested,     // the role's host asked it to stop
    kShutdownSignaled,  // orderly stop requested by the controller
    kControllerExited,  // the controller is gone without saying so
    kFailed,            // the wait itself failed; caller keeps bounded polling
};

// Any handle may be null. With none, this is a plain bounded sleep. An activity event wakes servicing without
// ending the role; lifecycle handles precede it so shutdown wins when both are signaled.
inline WaitOutcome WaitForServiceLifetime(HANDLE stopEvent, HANDLE shutdownEvent, HANDLE controllerProcess,
                                          DWORD waitMs, HANDLE activityEvent = nullptr) {
    HANDLE handles[4] = {};
    DWORD count = 0;
    DWORD stopIndex = MAXDWORD;
    DWORD shutdownIndex = MAXDWORD;
    DWORD controllerIndex = MAXDWORD;
    DWORD activityIndex = MAXDWORD;
    if (stopEvent) {
        stopIndex = count;
        handles[count++] = stopEvent;
    }
    if (shutdownEvent) {
        shutdownIndex = count;
        handles[count++] = shutdownEvent;
    }
    if (controllerProcess) {
        controllerIndex = count;
        handles[count++] = controllerProcess;
    }
    if (activityEvent) {
        activityIndex = count;
        handles[count++] = activityEvent;
    }
    if (count == 0) {
        Sleep(waitMs);
        return WaitOutcome::kTimeout;
    }
    const DWORD result = WaitForMultipleObjects(count, handles, FALSE, waitMs);
    if (result == WAIT_TIMEOUT)
        return WaitOutcome::kTimeout;
    if (result < WAIT_OBJECT_0 + count) {
        const DWORD index = result - WAIT_OBJECT_0;
        if (index == stopIndex)
            return WaitOutcome::kStopRequested;
        if (index == shutdownIndex)
            return WaitOutcome::kShutdownSignaled;
        if (index == controllerIndex)
            return WaitOutcome::kControllerExited;
        if (index == activityIndex)
            return WaitOutcome::kTimeout;  // service work is ready; lifecycle remains active
    }
    return WaitOutcome::kFailed;
}

inline WaitOutcome WaitForServiceLifetime(HANDLE shutdownEvent, HANDLE controllerProcess, DWORD waitMs) {
    return WaitForServiceLifetime(nullptr, shutdownEvent, controllerProcess, waitMs);
}

}  // namespace ce::service_lifetime
