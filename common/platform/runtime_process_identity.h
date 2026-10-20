#pragma once

// Which processes belong to the CaptureEngine runtime, by process identity instead of executable name.
//
// The runtime is hosted by whatever process owns it (the CaptureEngine app today, any client process
// once it lives in cengine.dll) and it spawns helper roles from the role-host executable. Name rules
// such as "captureengine.exe" recognise neither a third-party host nor a renamed role host
// (llm-wiki/library/08 Q10). The identity here is structural and race-free:
//   - the host is a PID, learned by a helper from the authenticated IPC endpoint it inherits
//     (ProcessIPCServer::Init registers it before any consumer runs);
//   - a helper is a process that runs the role-host image and descends from the host through at most
//     kMaxAncestorHops parent links. Parent links are fixed by the kernel at creation, so a helper is
//     recognised from its first instruction, with no list to publish and no window in which it is
//     unknown. The image test is what keeps a game the runtime launched for the user (also a child of
//     the host) out of it.
// Every PID is paired with its creation time, so a PID that was reused after the host or an ancestor
// exited never counts.

#include <cstdint>
#include <string>

namespace ce::runtime_identity {

inline constexpr int kMaxAncestorHops = 3;  // host -> helper role -> its worker -> that worker's child

struct ProcessFacts {
    uint32_t pid = 0;
    uint64_t creationTime = 0;  // FILETIME ticks; 0 = unknown
    uint32_t parentPid = 0;
};

struct HostIdentity {
    uint32_t pid = 0;           // 0 = no host known
    uint64_t creationTime = 0;  // 0 = unknown
    std::wstring roleHostImage;  // full path of the helper executable; empty = helpers cannot be told
};

enum class Relation : uint8_t {
    Unrelated,
    Host,    // the host process itself
    Helper,  // a role-host process the host (or one of its helpers) spawned
};

// Whether two image paths name the same executable (Windows paths compare case-insensitively).
bool SameImage(const std::wstring& lhs, const std::wstring& rhs);

// Pure classification of `pid` against `host`. `lookup(pid, &facts)` returns the facts of a live
// process and `imageOf(pid)` its full image path (empty when unreadable); they are the only OS access,
// so tests drive them with a fake process table. An unknown creation time (0) on either side of a
// link accepts the link: the exclusions built on this fail toward treating a process as the runtime's.
template <typename Lookup, typename ImageOf>
Relation Classify(const HostIdentity& host, uint32_t pid, Lookup&& lookup, ImageOf&& imageOf) {
    if (host.pid == 0 || pid == 0)
        return Relation::Unrelated;
    ProcessFacts current;
    const bool known = lookup(pid, &current);
    if (pid == host.pid) {
        // A host PID that now has another creation time was reused after the host exited.
        const bool reused = known && host.creationTime != 0 && current.creationTime != 0 &&
                            current.creationTime != host.creationTime;
        return reused ? Relation::Unrelated : Relation::Host;
    }
    if (!known || host.roleHostImage.empty())
        return Relation::Unrelated;
    for (int hop = 0; hop < kMaxAncestorHops && current.parentPid != 0; ++hop) {
        if (current.parentPid == host.pid) {
            // The host must predate the process it spawned, or its PID was reused.
            const bool hostPredates = host.creationTime == 0 || current.creationTime == 0 ||
                                      host.creationTime <= current.creationTime;
            // A descendant that runs anything but the role host is the user's (a launched game).
            return hostPredates && SameImage(imageOf(pid), host.roleHostImage) ? Relation::Helper
                                                                                : Relation::Unrelated;
        }
        ProcessFacts parent;
        if (!lookup(current.parentPid, &parent))
            return Relation::Unrelated;
        // An ancestor created after its child is a reused PID: the real parent is gone.
        if (parent.creationTime != 0 && current.creationTime != 0 && parent.creationTime > current.creationTime)
            return Relation::Unrelated;
        current = parent;
    }
    return Relation::Unrelated;
}

// Whether the windows of `pid` are CE's own, for a screen-capture rule that must not take them for the user's
// application. A helper always is. The host is only when it is the package's own application (its image is the
// role host image, or unknown, which fails toward the runtime's). A client that embeds the runtime is the host
// too, but the windows it owns are its own: a client recording its own fullscreen window must not be blacked out.
template <typename Lookup, typename ImageOf>
bool OwnsWindows(const HostIdentity& host, uint32_t pid, Lookup&& lookup, ImageOf&& imageOf) {
    const Relation relation = Classify(host, pid, lookup, imageOf);
    if (relation == Relation::Helper)
        return true;
    if (relation == Relation::Host)
        return host.roleHostImage.empty() || SameImage(imageOf(pid), host.roleHostImage);
    return false;
}

// A helper role records its host from the authenticated controller PID of its IPC endpoint; the role
// host image is the helper's own executable. The host's creation time is read now, while the host is
// certain to be alive. Re-registering the same host is a no-op; another one replaces it. The explicit
// form lets a runtime that knows its package's role host register itself.
void RegisterHost(uint32_t hostPid);
void RegisterHost(uint32_t hostPid, const std::wstring& roleHostImage);
HostIdentity RegisteredHost();
void ClearHostForTests();

// Relation of `pid` to the registered host (Unrelated while none is registered).
Relation ClassifyAgainstRegisteredHost(uint32_t pid);
// True when `pid` is the registered host or one of its helpers.
bool IsRegisteredRuntimeProcess(uint32_t pid);
// OwnsWindows against the registered host: the windows CE itself owns (see OwnsWindows).
bool OwnsRuntimeWindows(uint32_t pid);

}  // namespace ce::runtime_identity
