#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "source_fragment_reader.h"

namespace {

std::string ReadProjectSource(const std::filesystem::path& relativePath) {
    const std::string source =
        ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
    EXPECT_FALSE(source.empty()) << relativePath.string();
    return source;
}

}  // namespace

TEST(InjectSourceLifecycleTest, InjectorClearsStaleHookIdentityWhenSourceProcessExits) {
    // The hook lives inside the game process and dies with it, so the injector
    // is the only component that can retire the hook-owned identity. A stale
    // sourcePid after a game exit makes the desktop screenshot path wait for
    // the full hook timeout on an event no one will ever signal.
    const std::string source = ReadProjectSource("captureengine/inject_main.cpp");

    const size_t monitor = source.find("// Monitor sourcePid for config reloads (CBT hook support)");
    const size_t liveness = source.find("!IsProcessAlive(currentSourcePid)", monitor);
    const size_t clear = source.find("ClearStaleHookSourceState(pSharedMem)", liveness);
    const size_t reload = source.find("GetProcessNameFromPID(currentSourcePid)", clear);
    ASSERT_NE(monitor, std::string::npos);
    ASSERT_NE(liveness, std::string::npos);
    ASSERT_NE(clear, std::string::npos);
    ASSERT_NE(reload, std::string::npos);
    EXPECT_LT(monitor, liveness);
    EXPECT_LT(liveness, clear);
    EXPECT_LT(clear, reload);
}

TEST(InjectSourceLifecycleTest, StaleIdentityClearRetiresHookOwnedState) {
    const std::string source = ReadProjectSource("captureengine/inject_main.cpp");

    const size_t helper = source.find("static void ClearStaleHookSourceState(SharedMemoryLayout* sharedMemory)");
    const size_t helperEnd = source.find("// Console control handler for graceful cleanup", helper);
    ASSERT_NE(helper, std::string::npos);
    ASSERT_NE(helperEnd, std::string::npos);
    const std::string body = source.substr(helper, helperEnd - helper);

    EXPECT_NE(body.find("SetSourcePid(0)"), std::string::npos);
    EXPECT_NE(body.find("SetLuidSourcePid(0)"), std::string::npos);
    // A request left behind by a dying hook must not make the next injected
    // game's screenshot skip the hook path.
    EXPECT_NE(body.find("ScreenshotRequestStatus::Idle"), std::string::npos);
    EXPECT_NE(body.find("screenshotRequestId.store(0"), std::string::npos);
    EXPECT_NE(body.find("screenshotCompletedRequestId.store(0"), std::string::npos);
    EXPECT_NE(body.find("screenshotError.store(ERROR_SUCCESS"), std::string::npos);
    EXPECT_NE(body.find("screenshotPayloadKind.store(static_cast<uint32_t>(ScreenshotPayloadKind::None)"),
              std::string::npos);
}

TEST(InjectSourceLifecycleTest, LivenessHelperTreatsProtectedProcessesAsAlive) {
    const std::string source = ReadProjectSource("captureengine/inject_main.cpp");

    const size_t helper = source.find("static bool IsProcessAlive(uint32_t processId)");
    const size_t helperEnd = source.find("static void ClearStaleHookSourceState", helper);
    ASSERT_NE(helper, std::string::npos);
    ASSERT_NE(helperEnd, std::string::npos);
    const std::string body = source.substr(helper, helperEnd - helper);

    EXPECT_NE(body.find("OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION"), std::string::npos);
    EXPECT_NE(body.find("GetExitCodeProcess"), std::string::npos);
    EXPECT_NE(body.find("STILL_ACTIVE"), std::string::npos);
    // Protected sources may deny even the limited query; only a positive dead
    // result may retire the identity.
    EXPECT_NE(body.find("ERROR_ACCESS_DENIED"), std::string::npos);
}
