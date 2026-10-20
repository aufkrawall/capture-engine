#pragma once
#include <functional>
#include <optional>
namespace ce::pawnio {
void LaunchSetupWorker(std::function<void()> task);
void ShutdownSetupWorkers();
bool SetupShuttingDown();
// Silent requests run on their own worker (SetupWorker: busy until the result is collected). Admission is shared with
// LaunchSetupWorker: neither starts while the other is in flight. Results are collected by one thread.
unsigned long StartSilentSetupRequest(std::function<unsigned long()> work);
std::optional<unsigned long> TakeSilentSetupResult();
std::optional<unsigned long> WaitSilentSetupResult();
}  // namespace ce::pawnio
