#pragma once

#include <cstdint>

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

// The reference counts behind the service's one shared trace session. Any lease
// keeps the session (and its process provider) open; display leases alone keep
// the display-timing providers on. The service performs what a plan names and
// only then commits an acquire, so a failed step leaves the counts untouched.
class TraceLeaseCounter {
public:
    struct AcquirePlan {
        bool startSession = false;
        bool enableDisplay = false;
    };
    struct ReleasePlan {
        bool disableDisplay = false;  // never together with stopSession: stopping drops every provider
        bool stopSession = false;
    };

    AcquirePlan PlanAcquire(bool display) const {
        return AcquirePlan{users_ == 0, display && displayUsers_ == 0};
    }
    void CommitAcquire(bool display) {
        ++users_;
        if (display)
            ++displayUsers_;
    }
    ReleasePlan Release(bool display) {
        ReleasePlan plan;
        // A release without its matching lease changes nothing.
        if (display ? displayUsers_ == 0 : users_ == displayUsers_)
            return plan;
        const bool lastDisplay = display && --displayUsers_ == 0;
        plan.stopSession = --users_ == 0;
        plan.disableDisplay = lastDisplay && !plan.stopSession;
        return plan;
    }
    void Reset() {
        users_ = 0;
        displayUsers_ = 0;
    }
    uint32_t users() const { return users_; }
    uint32_t displayUsers() const { return displayUsers_; }

private:
    uint32_t users_ = 0;
    uint32_t displayUsers_ = 0;
};

}  // namespace ce::elevation
