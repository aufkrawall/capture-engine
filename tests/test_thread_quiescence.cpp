#include <gtest/gtest.h>

#include <windows.h>

#include "hook/hooking/hook_patch_transaction.h"
#include "hook/runtime/process_thread_walk.h"
#include "tests/exact_start_thread_support.h"

namespace {

using ce::hook_patch::PeerThreadOps;
using ce::hook_patch::QuiesceFailure;
using ce::hook_patch::ThreadQuiescence;
using ce::hook_patch::UnstableSnapshotPolicy;
using ce::process_threads::ThreadExitState;

DWORD WINAPI WaitForRelease(LPVOID event) {
    WaitForSingleObject(static_cast<HANDLE>(event), INFINITE);
    return 0;
}

// A peer thread parked on an event; joined on destruction.
class ParkedPeer {
public:
    ParkedPeer() : release_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        // Not CreateThread: the tests compare the kernel's start address with WaitForRelease, which ASan's
        // thread thunk would replace (see exact_start_thread_support.h).
        thread_ = ce::test_support::CreateThreadWithExactStart(&WaitForRelease, release_);
        if (thread_)
            id_ = GetThreadId(thread_);
        else
            ADD_FAILURE() << "CreateThreadWithExactStart failed";
    }
    ~ParkedPeer() {
        Release();
        CloseHandle(thread_);
        CloseHandle(release_);
    }
    void Release() {
        SetEvent(release_);
        WaitForSingleObject(thread_, INFINITE);
    }
    HANDLE handle() const {
        return thread_;
    }
    DWORD id() const {
        return id_;
    }

private:
    HANDLE release_ = nullptr;
    HANDLE thread_ = nullptr;
    DWORD id_ = 0;
};

// The kernel's answer for the peer under test; every other thread gets the real OS calls. These run
// while peers are suspended, so they only read globals.
DWORD g_refusingPeer = 0;
ThreadExitState g_refusingPeerState = ThreadExitState::kRunning;

DWORD SuspendUnlessRefusingPeer(HANDLE thread) {
    if (GetThreadId(thread) == g_refusingPeer) {
        SetLastError(ERROR_ACCESS_DENIED);  // how SuspendThread reports STATUS_THREAD_IS_TERMINATING
        return static_cast<DWORD>(-1);
    }
    return SuspendThread(thread);
}

ThreadExitState ExitStateOfRefusingPeer(HANDLE thread) {
    if (GetThreadId(thread) == g_refusingPeer)
        return g_refusingPeerState;
    return ce::process_threads::QueryThreadExitState(thread);
}

constexpr PeerThreadOps kRefusingPeerOps{&SuspendUnlessRefusingPeer, &ExitStateOfRefusingPeer,
                                         &ce::process_threads::QueryThreadSuspendCount};

// Holds a peer at MAXIMUM_SUSPEND_COUNT the way another owner would, and releases it on destruction.
class HeldAtSuspendMaximum {
public:
    explicit HeldAtSuspendMaximum(HANDLE thread) : thread_(thread) {
        while (SuspendThread(thread_) != static_cast<DWORD>(-1))
            ++held_;
        refusal_ = GetLastError();
    }
    ~HeldAtSuspendMaximum() {
        while (held_ > 0 && ResumeThread(thread_) != static_cast<DWORD>(-1))
            --held_;
    }
    DWORD held() const {
        return held_;
    }
    DWORD refusal() const {
        return refusal_;
    }

private:
    HANDLE thread_ = nullptr;
    DWORD held_ = 0;
    DWORD refusal_ = 0;
};

// An owner that releases its held peer while the transaction is still walking: the first refusal sees a
// held count, the confirmation after every other peer is suspended finds the peer suspendable again.
DWORD g_releasedPeer = 0;
bool g_releasedPeerRefused = false;

DWORD SuspendRefusingReleasedPeerOnce(HANDLE thread) {
    if (GetThreadId(thread) == g_releasedPeer && !g_releasedPeerRefused) {
        g_releasedPeerRefused = true;
        SetLastError(ERROR_SIGNAL_REFUSED);
        return static_cast<DWORD>(-1);
    }
    return SuspendThread(thread);
}

bool SuspendCountOfReleasedPeer(HANDLE thread, DWORD* count) {
    if (GetThreadId(thread) == g_releasedPeer) {
        *count = 127;
        return true;
    }
    return ce::process_threads::QueryThreadSuspendCount(thread, count);
}

constexpr PeerThreadOps kReleasedPeerOps{&SuspendRefusingReleasedPeerOnce, &ce::process_threads::QueryThreadExitState,
                                         &SuspendCountOfReleasedPeer};

DWORD SuspendCountOf(HANDLE thread) {
    DWORD count = 0;
    EXPECT_TRUE(ce::process_threads::QueryThreadSuspendCount(thread, &count));
    return count;
}

class ThreadQuiescenceRefusalTest : public ::testing::Test {
protected:
    void TearDown() override {
        g_refusingPeer = 0;
        g_refusingPeerState = ThreadExitState::kRunning;
    }
    ParkedPeer peer;
};

}  // namespace

// Heroes of the Storm late injection (20261025_211938): every patch failed with kSuspend for 1.2 s and the
// Present hook was never installed. SuspendThread refuses a thread that has started exiting until its handle
// is signaled; such a thread never runs user code again and must not fail the transaction.
TEST_F(ThreadQuiescenceRefusalTest, ExitingPeerThatRefusesSuspensionIsSkipped) {
    g_refusingPeer = peer.id();
    g_refusingPeerState = ThreadExitState::kExiting;
    static int patchTarget = 0;
    {
        ThreadQuiescence quiescence(&patchTarget, sizeof(patchTarget), UnstableSnapshotPolicy::kAcceptSuspendedSet,
                                    kRefusingPeerOps);
        EXPECT_TRUE(quiescence.IsReady());
        EXPECT_EQ(quiescence.ExitingPeersSkipped(), 1u);
        EXPECT_EQ(quiescence.Refused().threadId, 0u);
        EXPECT_TRUE(quiescence.IsRangeSafe(&patchTarget, sizeof(patchTarget)));
    }
    // Every peer the transaction suspended was resumed; the parked peer still exits normally.
    peer.Release();
    EXPECT_EQ(WaitForSingleObject(peer.handle(), 0), WAIT_OBJECT_0);
}

// A live peer that refuses suspension can be anywhere, including inside the patch range: the transaction
// must still fail closed, and name that thread for the log.
TEST_F(ThreadQuiescenceRefusalTest, LivePeerThatRefusesSuspensionFailsClosedAndIsNamed) {
    g_refusingPeer = peer.id();
    g_refusingPeerState = ThreadExitState::kRunning;
    static int patchTarget = 0;
    ThreadQuiescence quiescence(&patchTarget, sizeof(patchTarget), UnstableSnapshotPolicy::kAcceptSuspendedSet,
                                kRefusingPeerOps);
    EXPECT_FALSE(quiescence.IsReady());
    EXPECT_EQ(quiescence.FailureReason(), QuiesceFailure::kSuspend);
    EXPECT_EQ(quiescence.Refused().threadId, peer.id());
    EXPECT_EQ(quiescence.Refused().error, static_cast<DWORD>(ERROR_ACCESS_DENIED));
    EXPECT_EQ(quiescence.Refused().startAddress, reinterpret_cast<const void*>(&WaitForRelease));
    EXPECT_EQ(quiescence.ExitingPeersSkipped(), 0u);
}

// Heroes of the Storm keeps one of its own threads suspended 127 times, so SuspendThread refuses it with
// ERROR_SIGNAL_REFUSED (20261025_230718: tid 8976, error 156) and every late-inject patch was refused for the
// game's lifetime. Such a peer is stopped; the transaction must range-check it and leave its owner's count alone.
// Real kernel calls throughout.
TEST(ThreadQuiescenceHeldPeerTest, PeerHeldAtTheSuspendMaximumIsStoppedNotRefused) {
    ParkedPeer peer;
    HeldAtSuspendMaximum owner(peer.handle());
    ASSERT_EQ(owner.held(), static_cast<DWORD>(MAXIMUM_SUSPEND_COUNT));
    ASSERT_EQ(owner.refusal(), static_cast<DWORD>(ERROR_SIGNAL_REFUSED));
    CONTEXT parked = {};
    parked.ContextFlags = CONTEXT_CONTROL;
    ASSERT_TRUE(GetThreadContext(peer.handle(), &parked));
#ifdef _WIN64
    const auto* parkedAt = reinterpret_cast<const void*>(parked.Rip);
#else
    const auto* parkedAt = reinterpret_cast<const void*>(parked.Eip);
#endif
    static int patchTarget = 0;
    {
        ThreadQuiescence quiescence(&patchTarget, sizeof(patchTarget), UnstableSnapshotPolicy::kAcceptSuspendedSet);
        ASSERT_TRUE(quiescence.IsReady()) << ce::hook_patch::GetQuiesceFailureName(quiescence.FailureReason());
        EXPECT_EQ(quiescence.HeldPeers(), 1u);
        EXPECT_EQ(quiescence.Refused().threadId, 0u);
        EXPECT_TRUE(quiescence.IsRangeSafe(&patchTarget, sizeof(patchTarget)));
        // Its context was captured: a patch over the instruction it is parked at is refused.
        EXPECT_FALSE(quiescence.IsRangeSafe(parkedAt, 1));
    }
    // Neither suspended nor resumed by the transaction: the owner's 127 suspensions are intact.
    EXPECT_EQ(SuspendCountOf(peer.handle()), static_cast<DWORD>(MAXIMUM_SUSPEND_COUNT));
}

// The owner resumed its held peer before the owner itself was suspended. Once every other peer is suspended
// the transaction re-checks it, suspends it like any peer, and resumes it afterwards.
TEST(ThreadQuiescenceHeldPeerTest, HeldPeerReleasedDuringTheWalkIsSuspendedAndResumed) {
    ParkedPeer peer;
    g_releasedPeer = peer.id();
    g_releasedPeerRefused = false;
    static int patchTarget = 0;
    {
        ThreadQuiescence quiescence(&patchTarget, sizeof(patchTarget), UnstableSnapshotPolicy::kAcceptSuspendedSet,
                                    kReleasedPeerOps);
        ASSERT_TRUE(g_releasedPeerRefused);
        ASSERT_TRUE(quiescence.IsReady()) << ce::hook_patch::GetQuiesceFailureName(quiescence.FailureReason());
        EXPECT_EQ(quiescence.HeldPeers(), 0u);
        EXPECT_EQ(SuspendCountOf(peer.handle()), 1u);
    }
    EXPECT_EQ(SuspendCountOf(peer.handle()), 0u);
    g_releasedPeer = 0;
    peer.Release();
    EXPECT_EQ(WaitForSingleObject(peer.handle(), 0), WAIT_OBJECT_0);
}

// The suspend-count query that separates a held peer from a live refusal, against the real kernel. The
// third-party preload opens tool threads with limited query rights only, so those must suffice.
TEST(ProcessThreadStateTest, SuspendCountOfARealThread) {
    ASSERT_TRUE(ce::process_threads::PrepareThreadStateQueries());
    ParkedPeer peer;
    EXPECT_EQ(SuspendCountOf(peer.handle()), 0u);
    ASSERT_NE(SuspendThread(peer.handle()), static_cast<DWORD>(-1));
    ASSERT_NE(SuspendThread(peer.handle()), static_cast<DWORD>(-1));
    EXPECT_EQ(SuspendCountOf(peer.handle()), 2u);
    HANDLE limited = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, peer.id());
    ASSERT_NE(limited, nullptr);
    DWORD limitedCount = 0;
    EXPECT_TRUE(ce::process_threads::QueryThreadSuspendCount(limited, &limitedCount));
    EXPECT_EQ(limitedCount, 2u);
    CloseHandle(limited);
    ResumeThread(peer.handle());
    ResumeThread(peer.handle());
    EXPECT_EQ(SuspendCountOf(peer.handle()), 0u);
}

TEST(ProcessThreadStateTest, ExitStateAndStartAddressOfARealThread) {
    ASSERT_TRUE(ce::process_threads::PrepareThreadStateQueries());
    ParkedPeer peer;
    EXPECT_EQ(ce::process_threads::QueryThreadExitState(peer.handle()), ThreadExitState::kRunning);
    EXPECT_EQ(ce::process_threads::QueryThreadStartAddress(peer.handle()),
              reinterpret_cast<const void*>(&WaitForRelease));
    peer.Release();
    EXPECT_EQ(ce::process_threads::QueryThreadExitState(peer.handle()), ThreadExitState::kExited);
}

// The start-address query rejects a limited handle. The third-party preload opened tool threads that way from
// 2026-08-13 on, recognized none of them, and suspended nothing.
TEST(ProcessThreadStateTest, StartAddressNeedsQueryInformationRights) {
    ASSERT_TRUE(ce::process_threads::PrepareThreadStateQueries());
    ParkedPeer peer;
    HANDLE limited = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, peer.id());
    HANDLE full = OpenThread(THREAD_QUERY_INFORMATION, FALSE, peer.id());
    ASSERT_NE(limited, nullptr);
    ASSERT_NE(full, nullptr);
    EXPECT_EQ(ce::process_threads::QueryThreadStartAddress(limited), nullptr);
    EXPECT_EQ(ce::process_threads::QueryThreadStartAddress(full), reinterpret_cast<const void*>(&WaitForRelease));
    CloseHandle(limited);
    CloseHandle(full);
}

// The kernel's terminated flag itself (ThreadIsTerminated). A handle without SYNCHRONIZE cannot see the
// signaled state, so the answer comes from the query alone: running while the thread lives, terminated
// once it has exited. A wrong information class would read "running" in both cases.
TEST(ProcessThreadStateTest, TerminatedFlagComesFromTheKernelQuery) {
    ASSERT_TRUE(ce::process_threads::PrepareThreadStateQueries());
    ParkedPeer peer;
    HANDLE queryOnly = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, peer.id());
    ASSERT_NE(queryOnly, nullptr);
    EXPECT_EQ(ce::process_threads::QueryThreadExitState(queryOnly), ThreadExitState::kRunning);
    peer.Release();
    EXPECT_EQ(ce::process_threads::QueryThreadExitState(queryOnly), ThreadExitState::kExiting);
    CloseHandle(queryOnly);
}
