#pragma once
#include <functional>
#include <optional>
namespace ce::pawnio {
// Silent requests run on their own worker (SetupWorker: busy until the result is collected). One request is admitted
// at a time. Results are collected by one thread.
unsigned long StartSilentSetupRequest(std::function<unsigned long()> work);
std::optional<unsigned long> TakeSilentSetupResult();
std::optional<unsigned long> WaitSilentSetupResult();
}  // namespace ce::pawnio
