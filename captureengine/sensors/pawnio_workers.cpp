#include "pawnio_workers.h"
#include "pawnio_setup_core.h"
#include <mutex>

namespace ce::pawnio {
namespace {
// Admission is decided under `mutex`: SetupWorker::Start is not safe against a concurrent start.
std::mutex mutex;
ce::startup::SetupWorker silentRequest;
}  // namespace

unsigned long StartSilentSetupRequest(std::function<unsigned long()> work) {
    std::lock_guard<std::mutex> guard(mutex);
    return StartSilentRequest(silentRequest, std::move(work));
}

// No lock here: collecting joins the request thread, and holding `mutex` across that join would stall the caller. The
// SetupWorker stays busy until the result is collected, so no start can overlap the join.
std::optional<unsigned long> TakeSilentSetupResult() {
    return silentRequest.TakeResult();
}

std::optional<unsigned long> WaitSilentSetupResult() {
    return silentRequest.WaitForResult();
}
}  // namespace ce::pawnio
