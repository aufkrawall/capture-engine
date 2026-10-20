#pragma once

#include <type_traits>

namespace ce {

// Runs the body of a thread or foreign callback inside a scope the owner installed. The media role installs
// one that binds its session (MediaSessionScope in media_session_state.h). Shared units (WGC capture, DXGI
// duplication) hold only this hook and never name a media session: an unset scope runs the body as it is,
// which is how the runtime's engine thread uses those units for screenshots.
class ThreadScope {
public:
    using Body = void (*)(void* arg);
    using RunFn = void (*)(void* context, Body body, void* arg);

    ThreadScope() = default;
    ThreadScope(RunFn run, void* context) : run_(run), context_(context) {}

    template <typename Callable>
    void Run(Callable&& body) const {
        if (!run_) {
            body();
            return;
        }
        run_(context_, [](void* arg) { (*static_cast<std::remove_reference_t<Callable>*>(arg))(); }, &body);
    }

private:
    RunFn run_ = nullptr;
    void* context_ = nullptr;
};

}  // namespace ce
