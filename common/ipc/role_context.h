#pragma once

#include "common/ipc/process_ipc.h"

#include <cstdint>
#include <memory>
#include <string>

namespace ce::log {
class Sink;
}

namespace ce::role {
// Everything a role (inject, media, logger) takes from its host (DR-38). The process host
// (`<Role>ProcessMain`) fills it from the command line and inherited handles; the thread host
// (runtime/children) from the runtime. A role never reads argv, GetModuleFileName(NULL) or the
// executable's directory: in-process the executable is the client's.
struct RoleContext {
    ProcessMode mode = ProcessMode::Controller;
    // The role owns its process (an isolated helper). Only then does it apply process-wide effects:
    // console handlers, working-set trims, crash handlers, Log_Init/Log_SetLevel, CoInitializeSecurity,
    // the default DLL search order (DR-39).
    bool ownsProcess = true;
    // The settings' process priority class and process GPU scheduling priority may be applied to this
    // process: always in a helper process, in-process only with CE_FEATURE_PROCESS_TUNING.
    bool processTuning = true;
    // Command channel to the runtime (inject, media); nullptr for the logger. Not owned.
    RoleChannel* channel = nullptr;
    // Manual-reset event the host sets to stop a role: the thread host, and a helper's process host from its
    // console control handler. The roles also stop on their other signals (Shutdown command, pipe break, the
    // logger's named shutdown event).
    HANDLE stopEvent = nullptr;
    // The runtime's process: the controller of a helper process, the own process in-process.
    uint32_t controllerPid = 0;
    // Runtime package directory without a trailing separator: role host, mediaengine.dll, hooks, layers.
    std::wstring packageDir;
    // The role host executable of that package. Workers a role starts (per-app audio, DR-43) run it.
    std::wstring roleHostPath;
    // Active-code-page settings file path, as the INI loader takes it.
    std::string configPath;
    // Active-code-page logs root (`<package>\logs`, like every log path): the logger's fallback session
    // directory when discovery names none. The process host passes its executable's `logs` folder.
    std::string logsRoot;
    // Media: the diagnostic recording serial used in log and manifest names; empty when unscoped.
    std::string recordingId;
    // Media: handle to the session A/V latency channel, owned by the role (inherited in a helper, a
    // duplicate in-process); see common/ipc/av_sync_latency_channel.h. nullptr when absent.
    void* latencyChannelHandle = nullptr;
    // Media: handle to this session's finalization result slot, owned by the role (inherited in a helper,
    // a duplicate in-process); see common/ipc/media_finalization.h. nullptr when absent.
    void* resultSlotHandle = nullptr;
    // Thread host: the role's log sink, bound to the role thread before the entry runs and inherited by
    // every thread the role starts (DR-40). nullptr in a helper process: its threads write the process log.
    std::shared_ptr<ce::log::Sink> logSink;
};
}  // namespace ce::role
