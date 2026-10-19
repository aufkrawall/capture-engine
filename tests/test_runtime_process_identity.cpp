#include <gtest/gtest.h>

#include <windows.h>

#include <map>
#include <string>

#include "common/platform/process_parent.h"
#include "common/platform/runtime_process_identity.h"

namespace ri = ce::runtime_identity;

namespace {

const std::wstring kRoleHost = L"C:\\App\\host.exe";

// A fake process table: pid -> facts and image. A pid that is absent is a process that is not running.
class ProcessTable {
public:
    void Add(uint32_t pid, uint32_t parentPid, uint64_t creationTime, const std::wstring& image = kRoleHost) {
        facts_[pid] = ri::ProcessFacts{pid, creationTime, parentPid};
        images_[pid] = image;
    }
    ri::Relation Classify(const ri::HostIdentity& host, uint32_t pid) const {
        return ri::Classify(
            host, pid,
            [this](uint32_t id, ri::ProcessFacts* out) {
                ++lookups;
                const auto it = facts_.find(id);
                if (it == facts_.end())
                    return false;
                *out = it->second;
                return true;
            },
            [this](uint32_t id) {
                ++imageReads;
                const auto it = images_.find(id);
                return it == images_.end() ? std::wstring() : it->second;
            });
    }
    mutable int lookups = 0;
    mutable int imageReads = 0;

private:
    std::map<uint32_t, ri::ProcessFacts> facts_;
    std::map<uint32_t, std::wstring> images_;
};

const ri::HostIdentity kHost{100, 1000, kRoleHost};

// The registered host is process-global state: leave none behind, also when an assertion returns early.
struct HostGuard {
    HostGuard() { ri::ClearHostForTests(); }
    ~HostGuard() { ri::ClearHostForTests(); }
};

}  // namespace

TEST(RuntimeProcessIdentityTest, WithoutAHostNothingBelongsToTheRuntime) {
    ProcessTable table;
    table.Add(200, 100, 2000);
    EXPECT_EQ(table.Classify(ri::HostIdentity{}, 200), ri::Relation::Unrelated);
    EXPECT_EQ(table.Classify(ri::HostIdentity{}, 0), ri::Relation::Unrelated);
    EXPECT_EQ(table.lookups, 0) << "no host: not a single process is looked at";
}

TEST(RuntimeProcessIdentityTest, TheHostIsRecognisedWhateverItsExecutableIsCalled) {
    ProcessTable table;
    table.Add(100, 50, 1000, L"C:\\Games\\SomeClient.exe");
    EXPECT_EQ(table.Classify(kHost, 100), ri::Relation::Host);
    // The host exited while its helper was shutting down: its PID is still the host's.
    EXPECT_EQ(ProcessTable().Classify(kHost, 100), ri::Relation::Host);
    EXPECT_EQ(table.Classify(ri::HostIdentity{100, 1000, L""}, 100), ri::Relation::Host)
        << "a host needs no role host image";
}

TEST(RuntimeProcessIdentityTest, ReusedHostPidIsNotTheHost) {
    ProcessTable table;
    table.Add(100, 50, 9000);  // another process got the PID after the host exited
    EXPECT_EQ(table.Classify(kHost, 100), ri::Relation::Unrelated);
    // Unknown creation time on either side keeps the exclusion.
    EXPECT_EQ(table.Classify(ri::HostIdentity{100, 0, kRoleHost}, 100), ri::Relation::Host);
    ProcessTable noTime;
    noTime.Add(100, 50, 0);
    EXPECT_EQ(noTime.Classify(kHost, 100), ri::Relation::Host);
}

TEST(RuntimeProcessIdentityTest, HelpersRunTheRoleHostAndDescendFromTheHostThroughUpToThreeParentLinks) {
    ProcessTable table;
    table.Add(100, 50, 1000, L"C:\\Games\\SomeClient.exe");  // host
    table.Add(200, 100, 2000);                               // helper role
    table.Add(300, 200, 3000);                               // its worker
    table.Add(400, 300, 4000);                               // worker's child
    table.Add(500, 400, 5000);                               // one link too many
    EXPECT_EQ(table.Classify(kHost, 200), ri::Relation::Helper);
    EXPECT_EQ(table.Classify(kHost, 300), ri::Relation::Helper);
    EXPECT_EQ(table.Classify(kHost, 400), ri::Relation::Helper);
    EXPECT_EQ(table.Classify(kHost, 500), ri::Relation::Unrelated);
}

TEST(RuntimeProcessIdentityTest, AGameTheRuntimeLaunchedIsNotAHelper) {
    ProcessTable table;
    table.Add(100, 50, 1000);
    table.Add(200, 100, 2000, L"C:\\Games\\Witcher3\\bin\\x64\\witcher3.exe");  // launched by the host
    table.Add(300, 200, 2500, L"C:\\Games\\Witcher3\\bin\\x64\\helper.exe");    // and its child
    EXPECT_EQ(table.Classify(kHost, 200), ri::Relation::Unrelated);
    EXPECT_EQ(table.Classify(kHost, 300), ri::Relation::Unrelated);
    // Paths compare like Windows paths: case does not matter, a different directory does.
    ProcessTable lower;
    lower.Add(200, 100, 2000, L"c:\\app\\HOST.EXE");
    EXPECT_EQ(lower.Classify(kHost, 200), ri::Relation::Helper);
    ProcessTable elsewhere;
    elsewhere.Add(200, 100, 2000, L"C:\\Other\\host.exe");
    EXPECT_EQ(elsewhere.Classify(kHost, 200), ri::Relation::Unrelated);
    // Without a known role host image no descendant can be proven a helper.
    ProcessTable unknownImage;
    unknownImage.Add(200, 100, 2000, L"");
    EXPECT_EQ(unknownImage.Classify(kHost, 200), ri::Relation::Unrelated);
    EXPECT_EQ(table.Classify(ri::HostIdentity{100, 1000, L""}, 200), ri::Relation::Unrelated);
}

TEST(RuntimeProcessIdentityTest, UnrelatedProcessesAndTheirAncestorsAreNotRuntimeProcesses) {
    ProcessTable table;
    table.Add(100, 50, 1000);
    table.Add(50, 4, 500);     // the host's own parent is not part of the runtime
    table.Add(600, 50, 1500);  // sibling of the host
    table.Add(700, 600, 1600);
    table.Add(800, 999, 1700);  // parent no longer exists
    EXPECT_EQ(table.Classify(kHost, 50), ri::Relation::Unrelated);
    EXPECT_EQ(table.Classify(kHost, 600), ri::Relation::Unrelated);
    EXPECT_EQ(table.Classify(kHost, 700), ri::Relation::Unrelated);
    EXPECT_EQ(table.Classify(kHost, 800), ri::Relation::Unrelated);
    EXPECT_EQ(table.Classify(kHost, 12345), ri::Relation::Unrelated) << "not running";
}

TEST(RuntimeProcessIdentityTest, ParentLinkToAReusedPidDoesNotCount) {
    ProcessTable table;
    // Created before the host (1000) existed: its parent PID belonged to an earlier process.
    table.Add(200, 100, 500);
    EXPECT_EQ(table.Classify(kHost, 200), ri::Relation::Unrelated);
    table.Add(200, 100, 1000);  // created in the host's own tick is still a child of it
    EXPECT_EQ(table.Classify(kHost, 200), ri::Relation::Helper);

    // The same for a link in the middle of the chain.
    ProcessTable chain;
    chain.Add(100, 50, 1000);
    chain.Add(300, 250, 3000);
    chain.Add(250, 100, 9000);  // reused: created after its "child" 300
    EXPECT_EQ(chain.Classify(kHost, 300), ri::Relation::Unrelated);
}

TEST(RuntimeProcessIdentityTest, UnknownCreationTimesAcceptTheLink) {
    ProcessTable table;
    table.Add(200, 100, 0);
    EXPECT_EQ(table.Classify(kHost, 200), ri::Relation::Helper);
    EXPECT_EQ(table.Classify(ri::HostIdentity{100, 0, kRoleHost}, 200), ri::Relation::Helper);
}

TEST(RuntimeProcessIdentityTest, ClassificationIsCheapAndBounded) {
    ProcessTable table;
    table.Add(200, 100, 2000);
    EXPECT_EQ(table.Classify(kHost, 200), ri::Relation::Helper);
    EXPECT_EQ(table.lookups, 1) << "a direct child is decided by its own record alone";
    EXPECT_EQ(table.imageReads, 1);

    ProcessTable cycle;  // a corrupt table must not loop
    cycle.Add(10, 20, 100);
    cycle.Add(20, 10, 100);
    EXPECT_EQ(cycle.Classify(kHost, 10), ri::Relation::Unrelated);
    EXPECT_LE(cycle.lookups, 1 + ri::kMaxAncestorHops);
    EXPECT_EQ(cycle.imageReads, 0) << "the image is only read for a process that descends from the host";
}

TEST(RuntimeProcessIdentityTest, RegisteredHostGovernsTheOsBackedQueries) {
    HostGuard guard;
    const uint32_t self = GetCurrentProcessId();
    EXPECT_FALSE(ri::IsRegisteredRuntimeProcess(self));
    EXPECT_EQ(ri::RegisteredHost().pid, 0u);

    ri::RegisterHost(0);
    EXPECT_EQ(ri::RegisteredHost().pid, 0u) << "PID 0 is no host";

    ri::RegisterHost(self);
    const ri::HostIdentity host = ri::RegisteredHost();
    EXPECT_EQ(host.pid, self);
    EXPECT_NE(host.creationTime, 0u);
    EXPECT_FALSE(host.roleHostImage.empty()) << "a helper's own executable is the role host image";
    EXPECT_EQ(ri::ClassifyAgainstRegisteredHost(self), ri::Relation::Host);
    EXPECT_TRUE(ri::IsRegisteredRuntimeProcess(self));

    ri::RegisterHost(self);  // idempotent
    EXPECT_EQ(ri::RegisteredHost().roleHostImage, host.roleHostImage);

    ri::ClearHostForTests();
    EXPECT_FALSE(ri::IsRegisteredRuntimeProcess(self));
}

// The real OS queries on a real child: a copy of this very test executable stands in for a helper role
// (it runs the role host image), cmd.exe for a game the host launched.
TEST(RuntimeProcessIdentityTest, ARealChildIsAHelperOnlyWhenItRunsTheRoleHostImage) {
    wchar_t selfPath[MAX_PATH] = {};
    ASSERT_GT(GetModuleFileNameW(nullptr, selfPath, MAX_PATH), 0u);
    wchar_t systemDirectory[MAX_PATH] = {};
    ASSERT_GT(GetSystemDirectoryW(systemDirectory, MAX_PATH), 0u);
    const std::wstring cmd = std::wstring(systemDirectory) + L"\\cmd.exe";

    auto spawnSuspended = [](const std::wstring& image, PROCESS_INFORMATION* info) {
        std::wstring commandLine = L"\"" + image + L"\" /?";
        STARTUPINFOW startup = {sizeof(startup)};
        return CreateProcessW(image.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                              CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, info) != FALSE;
    };
    PROCESS_INFORMATION sameImage = {};
    PROCESS_INFORMATION otherImage = {};
    // Suspended: each exists, has a parent link and a creation time, and cannot exit under the query.
    ASSERT_TRUE(spawnSuspended(selfPath, &sameImage));
    ASSERT_TRUE(spawnSuspended(cmd, &otherImage));

    HostGuard guard;
    ri::RegisterHost(GetCurrentProcessId());  // this process is the host, its own image the role host
    EXPECT_EQ(ri::ClassifyAgainstRegisteredHost(sameImage.dwProcessId), ri::Relation::Helper);
    EXPECT_EQ(ri::ClassifyAgainstRegisteredHost(otherImage.dwProcessId), ri::Relation::Unrelated);
    DWORD parentPid = 0;
    ASSERT_TRUE(ce::process_parent::QueryParentProcessId(GetCurrentProcess(), &parentPid));
    EXPECT_EQ(ri::ClassifyAgainstRegisteredHost(parentPid), ri::Relation::Unrelated)
        << "an ancestor of the host is not a helper of it";
    EXPECT_EQ(ri::ClassifyAgainstRegisteredHost(4), ri::Relation::Unrelated) << "the System process";

    for (PROCESS_INFORMATION* child : {&sameImage, &otherImage}) {
        TerminateProcess(child->hProcess, 0);
        WaitForSingleObject(child->hProcess, INFINITE);
        CloseHandle(child->hThread);
        CloseHandle(child->hProcess);
    }
}
