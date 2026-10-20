#include "media_main_internal.h"

#include <memory>

#include "captureengine/roles/role_entries.h"
#include "media_role_context.h"

namespace ce::role {
namespace {

// Runs the session on the calling thread, bound to `session` (DR-41). The context supplies its recording id, log sink
// and the process effects it may apply (DR-39).
int RunBoundSession(RoleContext& context, const AppConfig& config, MediaSessionState& session) {
    session.recordingId = context.recordingId;
    session.logSink = context.logSink;
    session.ownsProcess = context.ownsProcess;
    session.processTuning = context.processTuning;
    MediaSessionBinding binding(session);
    return MediaProcessSession(context).Run(config);
}

}  // namespace

// The media role (DR-38). A role that owns its process installs its session as the process default, which every unbound
// thread of the process uses, and keeps that session static: its threads may still run when the role returns. A role on
// a client's thread gets a session of its own and never the process default (DR-41).
int RunMediaRole(RoleContext& context, const AppConfig& config) {
    // A refusal still owns the handles the context handed over.
    if (!context.channel) {
        LogError("[Media] No command channel; the media role cannot run");
        ce::media_role::ReleaseRoleOwnedHandles(context.resultSlotHandle, context.latencyChannelHandle);
        return 1;
    }
    if (context.packageDir.empty()) {
        LogError("[Media] No package directory; refusing to run the media role");
        ce::media_role::ReleaseRoleOwnedHandles(context.resultSlotHandle, context.latencyChannelHandle);
        return 1;
    }
    if (context.ownsProcess) {
        static MediaSessionState processSession;
        static MediaProcessDefaultScope processDefault(processSession);
        return RunBoundSession(context, config, processSession);
    }
    auto session = std::make_unique<MediaSessionState>();
    return RunBoundSession(context, config, *session);
}

}  // namespace ce::role
