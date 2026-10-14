#pragma once

#include "elevation_protocol.h"

namespace ce::elevation {

// Process discovery needs more than a working display trace. Query readiness
// after acquiring the lease, and release it on both legacy-service and provider
// failure paths. The ordinary display consumer keeps its existing contract.
template <typename Acquire, typename Query, typename Release>
bool AcquireProcessTraceLease(Acquire acquire, Query query, Release release) {
    if (!acquire())
        return false;
    Capabilities capabilities;
    if (query(capabilities) && (capabilities.features & kProcessStartTraceCapability) != 0)
        return true;
    release();
    return false;
}

}  // namespace ce::elevation
