#pragma once

#include <windows.h>

// One bounded wait of a controller-owned service child (logger, sensors) on the
// two things that end it: the controller's shutdown event and the controller
// process itself. Waiting on the event alone let a service outlive a controller
// that exited hard (crash, TerminateProcess): nothing ever set the event, so an
// orphaned logger kept draining the shared log rings and competed with the next
// controller's logger for them.

namespace ce::service_lifetime {

enum class WaitOutcome {
    kTimeout,           // keep servicing
    kShutdownSignaled,  // orderly stop requested by the controller
    kControllerExited,  // the controller is gone without saying so
    kFailed,            // the wait itself failed; caller keeps bounded polling
};

// Either handle may be null. With neither, this is a plain bounded sleep.
inline WaitOutcome WaitForServiceLifetime(HANDLE shutdownEvent, HANDLE controllerProcess, DWORD waitMs) {
    HANDLE handles[2] = {};
    DWORD count = 0;
    DWORD shutdownIndex = MAXDWORD;
    DWORD controllerIndex = MAXDWORD;
    if (shutdownEvent) {
        shutdownIndex = count;
        handles[count++] = shutdownEvent;
    }
    if (controllerProcess) {
        controllerIndex = count;
        handles[count++] = controllerProcess;
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
        if (index == shutdownIndex)
            return WaitOutcome::kShutdownSignaled;
        if (index == controllerIndex)
            return WaitOutcome::kControllerExited;
    }
    return WaitOutcome::kFailed;
}

}  // namespace ce::service_lifetime
