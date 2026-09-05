#include "custom_overlay_dx12.h"

#include <mutex>
#include "hook_common.h"

namespace CustomOverlay {
namespace {
struct RetiredBackends {
    std::mutex mutex;
    DX12Backend* first = nullptr;
};

RetiredBackends& Retirements() {
    // The hook service owns reclamation. Static destruction must never release
    // GPU objects after the driver has begun process-exit teardown.
    static auto* state = new RetiredBackends;
    return *state;
}
}  // namespace

void RetireDX12Backend(DX12Backend* backend) {
    if (!backend)
        return;
    backend->retirementDevice = backend->device;
    auto& retired = Retirements();
    std::lock_guard<std::mutex> lock(retired.mutex);
    backend->retiredNext = retired.first;
    retired.first = backend;
    HookLogImportant("DX12 Overlay: retaining callback resources until inline GPU completion across backend change");
}

void CollectRetiredDX12Backends() {
    if (IsProcessTerminating())
        return;
    auto& retired = Retirements();
    DX12Backend* ready = nullptr;
    {
        std::lock_guard<std::mutex> lock(retired.mutex);
        auto** link = &retired.first;
        while (*link) {
            auto* backend = *link;
            if (backend->HasInlineUploadsInFlight()) {
                link = &backend->retiredNext;
            } else {
                *link = backend->retiredNext;
                backend->retiredNext = ready;
                ready = backend;
            }
        }
    }
    // Driver releases run on the service thread, outside the registry lock.
    while (ready) {
        auto* backend = ready;
        ready = backend->retiredNext;
        delete backend;
    }
}

}  // namespace CustomOverlay
