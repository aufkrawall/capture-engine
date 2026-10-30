#include "window_heartbeat.h"

#include <algorithm>
#include "common/logging/logging.h"

// The recovery policy: which windows are watched, when a stall starts an
// attempt, and the move -> resize ladder with its restore and outcome logging.
namespace ce::window_heartbeat {

namespace {

bool SameRect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

int64_t Area(const RECT& rect) {
    return static_cast<int64_t>(rect.right - rect.left) * static_cast<int64_t>(rect.bottom - rect.top);
}

const char* StepName(Step step) {
    return step == Step::Move ? "move" : "resize";
}

unsigned long long Ms(uint64_t value) {
    return static_cast<unsigned long long>(value);
}

}  // namespace

void Pump::Configure(const std::vector<ApplicationProfile>& profiles) {
    processProfiles_.clear();
    enabled_ = false;
    for (const auto& profile : profiles) {
        if (profile.target.HasProcess()) {
            processProfiles_.push_back(profile);
            enabled_ = enabled_ || profile.windowHeartbeatEnabled;
        }
    }
    // Keep outstanding probes and recovery attempts across disable/re-enable: a
    // late completion must never release a newer request, and a nudged window
    // still needs its geometry restored.
}

DWORD Pump::WaitMs(uint64_t nowMs) const {
    if (!Enabled())
        return INFINITE;
    const uint64_t elapsedMs = nowMs - lastTickMs_;
    return !hasTicked_ || elapsedMs >= kIntervalMs ? 0 : static_cast<DWORD>(kIntervalMs - elapsedMs);
}

std::map<DWORD, Window> Pump::SelectTargets(Backend& backend, const std::vector<Window>& windows) {
    std::map<DWORD, std::string> names;
    std::map<DWORD, std::string> paths;
    std::map<DWORD, Window> targets;
    const bool needsPaths = std::any_of(processProfiles_.begin(), processProfiles_.end(),
                                        [](const ApplicationProfile& profile) {
                                            return IsProcessPathSelector(profile.target);
                                        });
    for (const auto& window : windows) {
        if (!window.handle || !window.processId || !window.threadId || window.processId == ownerProcessId_ ||
            !window.visible || window.minimized || window.cloaked || !window.frame || Area(window.rect) <= 0)
            continue;
        auto name = names.find(window.processId);
        if (name == names.end()) {
            const auto cached = names_.find(window.processId);
            name = names
                       .emplace(window.processId,
                                cached != names_.end() ? cached->second : backend.ProcessName(window.processId))
                       .first;
            if (needsPaths) {
                const auto cachedPath = paths_.find(window.processId);
                paths.emplace(window.processId,
                              cachedPath != paths_.end() ? cachedPath->second : backend.ProcessPath(window.processId));
            }
        }
        const std::string& path = paths[window.processId];
        const ApplicationProfile* profile = FindApplicationProfileForProcessIdentity(
            processProfiles_, ProcessTargetIdentity{name->second, path}, true);
        if (!profile || !profile->windowHeartbeatEnabled)
            continue;
        // A game presents into its largest frame window; launchers and splash windows are smaller.
        Window& best = targets[window.processId];
        if (!best.handle || Area(window.rect) > Area(best.rect)) {
            best = window;
            best.processName = name->second;
        }
    }
    names_ = std::move(names);
    paths_ = std::move(paths);
    return targets;
}

TickResult Pump::Tick(uint64_t nowMs, int64_t nowQpc, Backend& backend, present_progress::Tracker& progress,
                      SENDASYNCPROC callback) {
    TickResult result;
    if (!Enabled() || (hasTicked_ && nowMs - lastTickMs_ < kIntervalMs))
        return result;
    lastTickMs_ = nowMs;
    hasTicked_ = true;
    result.ran = true;
    std::vector<Window> windows;
    if (!backend.Enumerate(windows)) {
        result.failed = 1;
        result.lastError = ERROR_GEN_FAILURE;
        result.episodes = static_cast<uint32_t>(episodes_.size());
        return result;
    }
    result.enumerated = true;
    for (auto it = probes_.begin(); it != probes_.end();) {
        const bool alive = std::any_of(windows.begin(), windows.end(), [&](const Window& window) {
            return window.handle == it->first && window.processId == it->second.processId &&
                   window.threadId == it->second.threadId;
        });
        it = alive ? std::next(it) : probes_.erase(it);
    }
    const DWORD foregroundPid = backend.ForegroundProcessId();
    const std::map<DWORD, Window> targets = SelectTargets(backend, windows);
    result.windows = static_cast<uint32_t>(targets.size());
    // Unknown foreground state fails closed: nothing is watched or nudged.
    for (const auto& [processId, window] : targets) {
        if (foregroundPid != 0 && processId != foregroundPid)
            result.background.push_back(processId);
    }
    progress.Track(result.background);

    for (auto it = episodes_.begin(); it != episodes_.end();) {
        Episode& episode = it->second;
        const auto target = targets.find(it->first);
        bool keep = true;
        if (target == targets.end() || target->second.handle != episode.window.handle ||
            target->second.threadId != episode.window.threadId) {
            LogInfo("[WindowHeartbeat] Recovery ended pid=%lu: the window closed, was hidden or minimized", it->first);
            keep = false;
        } else if (foregroundPid == it->first) {
            // The user took over the window; only the 1 px change is undone.
            if (SameRect(target->second.rect, episode.target))
                backend.Place(target->second, episode.window.rect);
            LogInfo("[WindowHeartbeat] Recovery ended pid=%lu: the window became foreground (phase=%u)", it->first,
                    static_cast<unsigned>(episode.phase));
            keep = false;
        } else if (foregroundPid != 0) {
            keep = Advance(episode, target->second, nowMs, backend, progress, callback, result);
        }
        it = keep ? std::next(it) : episodes_.erase(it);
    }
    for (auto it = attempted_.begin(); it != attempted_.end();)
        it = targets.contains(it->first) ? std::next(it) : attempted_.erase(it);
    for (const DWORD processId : result.background) {
        if (episodes_.contains(processId))
            continue;
        const present_progress::Stall stall = progress.Inspect(processId, nowQpc, stallThresholdUs_);
        const auto attempted = attempted_.find(processId);
        if (!stall.stalled ||
            (attempted != attempted_.end() && attempted->second == std::make_pair(stall.threadId, stall.sinceQpc)))
            continue;
        attempted_[processId] = {stall.threadId, stall.sinceQpc};
        Begin(targets.at(processId), stall, nowMs, backend, progress, callback, result);
    }
    result.episodes = static_cast<uint32_t>(episodes_.size());
    return result;
}

void Pump::Begin(const Window& window, const present_progress::Stall& stall, uint64_t nowMs, Backend& backend,
                 present_progress::Tracker& progress, SENDASYNCPROC callback, TickResult& result) {
    Episode episode;
    episode.window = window;
    episode.presentThread = stall.threadId;
    episode.presentSinceQpc = stall.sinceQpc;
    episode.detectedMs = nowMs;
    episode.returnsAtDetection = progress.Returns(window.processId);
    const RECT& rect = window.rect;
    LogWarn("[WindowHeartbeat] Present stall pid=%lu (%s) presentThread=%lu openMs=%llu thresholdMs=%llu "
            "lastIntervalMs=%llu mpo=%d window=%p rect=%ld,%ld %ldx%ld - nudging its geometry", window.processId,
            window.processName.c_str(), stall.threadId, Ms(stall.openUs / 1000), Ms(stallThresholdUs_ / 1000),
            Ms(stall.lastIntervalUs / 1000), stall.multiplane ? 1 : 0, static_cast<void*>(window.handle), rect.left,
            rect.top, rect.right - rect.left, rect.bottom - rect.top);
    Episode& stored = episodes_[window.processId] = episode;
    if (Nudge(stored, Step::Move, nowMs, backend, result))
        SendProbe(stored, nowMs, backend, callback, result);
}

bool Pump::Nudge(Episode& episode, Step step, uint64_t nowMs, Backend& backend, TickResult& result) {
    const RECT& original = episode.window.rect;
    episode.step = step;
    // Moving keeps the swapchain's size, so the game needs no ResizeBuffers; a
    // 1 px narrower window is the manual resize that is known to unstick it.
    episode.target = step == Step::Move ? RECT{original.left + 1, original.top, original.right + 1, original.bottom}
                                        : RECT{original.left, original.top, original.right - 1, original.bottom};
    episode.phaseMs = nowMs;
    const DWORD error = backend.Place(episode.window, episode.target);
    if (error != ERROR_SUCCESS) {
        ++result.failed;
        result.lastError = error;
        LogWarn("[WindowHeartbeat] %s nudge pid=%lu could not be queued (error=%lu); recovery gives up",
                StepName(step), episode.window.processId, error);
        episode.phase = Phase::Exhausted;
        return false;
    }
    LogInfo("[WindowHeartbeat] %s nudge queued pid=%lu sinceStallMs=%llu", StepName(step), episode.window.processId,
            Ms(nowMs - episode.detectedMs));
    episode.phase = Phase::Nudged;
    return true;
}

void Pump::SendProbe(Episode& episode, uint64_t nowMs, Backend& backend, SENDASYNCPROC callback,
                     TickResult& result) {
    if (probes_.contains(episode.window.handle))
        return;
    episode.probeSentMs = nowMs;
    episode.probeAnsweredMs = UINT64_MAX;
    // Never reuse a token during this worker's lifetime, even after HWND reuse.
    if (nextToken_ == UINTPTR_MAX) {
        ++result.failed;
        result.lastError = ERROR_ARITHMETIC_OVERFLOW;
        episode.probeAnsweredMs = nowMs;  // judged without the window thread's answer
        return;
    }
    const ULONG_PTR token = ++nextToken_;
    probes_[episode.window.handle] = {episode.window.processId, episode.window.threadId, token};
    episode.probe = token;
    const DWORD error = backend.SendNull(episode.window, token, callback);
    if (error != ERROR_SUCCESS) {
        probes_.erase(episode.window.handle);
        ++result.failed;
        result.lastError = error;
        episode.probeAnsweredMs = nowMs;
    }
}

void Pump::Complete(HWND window, ULONG_PTR token, uint64_t nowMs) {
    const auto it = probes_.find(window);
    if (it == probes_.end() || it->second.token != token)
        return;
    const DWORD processId = it->second.processId;
    probes_.erase(it);
    const auto episode = episodes_.find(processId);
    if (episode == episodes_.end() || episode->second.probe != token)
        return;
    Episode& current = episode->second;
    current.probeAnsweredMs = nowMs;
    if (current.firstReplyMs == UINT64_MAX)
        current.firstReplyMs = nowMs - current.probeSentMs;
}

bool Pump::Advance(Episode& episode, const Window& current, uint64_t nowMs, Backend& backend,
                   present_progress::Tracker& progress, SENDASYNCPROC callback, TickResult& result) {
    const DWORD processId = episode.window.processId;
    const RECT& original = episode.window.rect;
    // The stalled call returned. A loss or a new trace that only hides it changes no return count.
    const bool resumed =
        !progress.StillOpen(processId, episode.presentThread, episode.presentSinceQpc) &&
        progress.Returns(processId) != episode.returnsAtDetection;
    const long long reply = episode.firstReplyMs == UINT64_MAX ? -1 : static_cast<long long>(episode.firstReplyMs);
    switch (episode.phase) {
        case Phase::Nudged:
            if (SameRect(current.rect, episode.target)) {
                const DWORD error = backend.Place(current, original);
                if (error != ERROR_SUCCESS) {
                    ++result.failed;
                    result.lastError = error;
                    LogWarn("[WindowHeartbeat] Restore after the %s nudge pid=%lu could not be queued (error=%lu); "
                            "the window keeps its 1 px change", StepName(episode.step), processId, error);
                    return false;
                }
                episode.phase = Phase::Restoring;
                return true;
            }
            if (!SameRect(current.rect, original)) {
                LogInfo("[WindowHeartbeat] Recovery ended pid=%lu: something else changed the window's geometry",
                        processId);
                return false;
            }
            // Unchanged: the window thread has not run the queued change yet, or the
            // game put its geometry straight back. Once the thread answered a probe
            // sent after the nudge and a tick passed, it is the latter.
            if (episode.probeAnsweredMs != UINT64_MAX && episode.probeSentMs >= episode.phaseMs &&
                nowMs - episode.phaseMs >= kIntervalMs) {
                LogInfo("[WindowHeartbeat] %s nudge pid=%lu left the geometry unchanged although the window thread "
                        "answered", StepName(episode.step), processId);
                episode.phase = Phase::Observing;
                episode.phaseMs = nowMs;
            } else if (episode.probeSentMs < episode.phaseMs) {
                SendProbe(episode, nowMs, backend, callback, result);
            }
            return true;
        case Phase::Restoring:
            if (SameRect(current.rect, original)) {
                episode.phase = Phase::Observing;
                episode.phaseMs = nowMs;
                return true;
            }
            if (SameRect(current.rect, episode.target))
                return true;
            LogInfo("[WindowHeartbeat] Recovery ended pid=%lu: something else changed the window's geometry",
                    processId);
            return false;
        case Phase::Observing:
            if (resumed) {
                LogInfo("[WindowHeartbeat] Present resumed pid=%lu after the %s nudge (sinceStallMs=%llu "
                        "probeReplyMs=%lld)", processId, StepName(episode.step), Ms(nowMs - episode.detectedMs), reply);
                return false;
            }
            if (nowMs - episode.phaseMs < kIntervalMs)
                return true;
            if (episode.step == Step::Move) {
                if (Nudge(episode, Step::Resize, nowMs, backend, result))
                    SendProbe(episode, nowMs, backend, callback, result);
                return true;
            }
            LogWarn("[WindowHeartbeat] Present still blocked pid=%lu after move and resize nudges (sinceStallMs=%llu "
                    "probeReplyMs=%lld); no further attempt for this stall", processId,
                    Ms(nowMs - episode.detectedMs), reply);
            episode.phase = Phase::Exhausted;
            return true;
        case Phase::Exhausted:
            if (!resumed)
                return true;
            LogInfo("[WindowHeartbeat] Present resumed pid=%lu without a nudge's help (sinceStallMs=%llu)", processId,
                    Ms(nowMs - episode.detectedMs));
            return false;
    }
    return false;
}

}  // namespace ce::window_heartbeat
