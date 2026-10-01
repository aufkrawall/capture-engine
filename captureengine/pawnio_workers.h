#pragma once
#include <functional>
namespace ce::pawnio {
void LaunchSetupWorker(std::function<void()> task);
void ShutdownSetupWorkers();
bool SetupShuttingDown();
}  // namespace ce::pawnio
