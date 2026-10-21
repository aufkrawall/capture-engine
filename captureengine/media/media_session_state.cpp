#include "media_session_state.h"

#include "common/platform/thread_label.h"

#include <cstdio>
#include <exception>

namespace {

// Written only by the thread that binds a session; the process default is written once by the process host
// before any media thread exists.
thread_local MediaSessionState* t_boundSession = nullptr;
std::atomic<MediaSessionState*> g_processDefaultSession{nullptr};

void RunInBoundSession(void* context, ce::ThreadScope::Body body, void* arg) {
    MediaSessionBinding binding(*static_cast<MediaSessionState*>(context));
    body(arg);
}

}  // namespace

MediaSessionState& MediaState() {
    if (t_boundSession) {
        return *t_boundSession;
    }
    if (MediaSessionState* processSession = g_processDefaultSession.load(std::memory_order_acquire)) {
        return *processSession;
    }
    LogError("[MediaSession] MediaState() used on a thread with no bound media session and no process session");
    std::terminate();
}

MediaSessionBinding::MediaSessionBinding(MediaSessionState& state)
    : previous_(t_boundSession), sinkScope_(state.logSink) {
    t_boundSession = &state;
}

MediaSessionBinding::~MediaSessionBinding() {
    t_boundSession = previous_;
}

MediaProcessDefaultScope::MediaProcessDefaultScope(MediaSessionState& state)
    : previous_(g_processDefaultSession.exchange(&state, std::memory_order_acq_rel)) {}

MediaProcessDefaultScope::~MediaProcessDefaultScope() {
    g_processDefaultSession.store(previous_, std::memory_order_release);
}

ce::ThreadScope MediaSessionScope(MediaSessionState& state) {
    return ce::ThreadScope(&RunInBoundSession, &state);
}

void NameMediaThread(const char* purpose) {
    const std::string& recordingId = MediaState().recordingId;
    char name[64];
    snprintf(name, sizeof(name), "CE media %s %s", recordingId.empty() ? "-" : recordingId.c_str(),
             purpose ? purpose : "thread");
    ce::platform::NameCurrentThread(name);
}
