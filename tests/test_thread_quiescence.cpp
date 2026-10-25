#include <gtest/gtest.h>

#include <windows.h>

#include "hook/hooking/hook_patch_transaction.h"
#include "hook/runtime/process_thread_walk.h"

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
        thread_ = CreateThread(nullptr, 0, &WaitForRelease, release_, 0, &id_);
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

constexpr PeerThreadOps kRefusingPeerOps{&SuspendUnlessRefusingPeer, &ExitStateOfRefusingPeer};

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

TEST(ProcessThreadStateTest, ExitStateAndStartAddressOfARealThread) {
    ASSERT_TRUE(ce::process_threads::PrepareThreadStateQueries());
    ParkedPeer peer;
    EXPECT_EQ(ce::process_threads::QueryThreadExitState(peer.handle()), ThreadExitState::kRunning);
    EXPECT_EQ(ce::process_threads::QueryThreadStartAddress(peer.handle()),
              reinterpret_cast<const void*>(&WaitForRelease));
    peer.Release();
    EXPECT_EQ(ce::process_threads::QueryThreadExitState(peer.handle()), ThreadExitState::kExited);
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
