#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string ReadScreenshotSource() {
    const std::filesystem::path source = std::filesystem::current_path() / "captureengine" / "screenshot.cpp";
    std::ifstream file(source, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

}  // namespace

TEST(ScreenshotSourceTest, WgcReadbackUsesTheTextureOwningDevice) {
    const std::string source = ReadScreenshotSource();
    ASSERT_FALSE(source.empty());

    const size_t function = source.find("bool CaptureD3D11Texture(ID3D11Texture2D* texture");
    const size_t functionEnd = source.find("std::filesystem::path ReadyPathForPart", function);
    ASSERT_NE(function, std::string::npos);
    ASSERT_NE(functionEnd, std::string::npos);
    const std::string body = source.substr(function, functionEnd - function);

    const size_t getDevice = body.find("texture->GetDevice(textureDevice.addressof())");
    const size_t getContext = body.find("textureDevice->GetImmediateContext(textureContext.addressof())");
    const size_t createStaging = body.find("textureDevice->CreateTexture2D");
    const size_t queryMutex = body.find("texture->QueryInterface(IID_PPV_ARGS(&keyedMutex.mutex))");
    const size_t acquireMutex = body.find("keyedMutex.mutex->AcquireSync(1, 1000)");
    const size_t copyResource = body.find("textureContext->CopyResource(staging.get(), texture)");
    const size_t mapStaging = body.find("textureContext->Map(staging.get()");
    const size_t releaseMutex = body.find("mutex->ReleaseSync(0)");
    ASSERT_NE(getDevice, std::string::npos);
    ASSERT_NE(getContext, std::string::npos);
    ASSERT_NE(createStaging, std::string::npos);
    ASSERT_NE(queryMutex, std::string::npos);
    ASSERT_NE(acquireMutex, std::string::npos);
    ASSERT_NE(copyResource, std::string::npos);
    ASSERT_NE(mapStaging, std::string::npos);
    ASSERT_NE(releaseMutex, std::string::npos);
    EXPECT_LT(queryMutex, acquireMutex);
    EXPECT_LT(acquireMutex, copyResource);
    EXPECT_LT(getDevice, createStaging);
    EXPECT_LT(getContext, copyResource);
    EXPECT_LT(copyResource, mapStaging);
    EXPECT_EQ(body.find("ID3D11Device* device"), std::string::npos);
    EXPECT_EQ(body.find("ID3D11DeviceContext* context"), std::string::npos);
}

TEST(ScreenshotSourceTest, DesktopScreenshotSkipsAnIdleInjectServiceWithoutWaiting) {
    const std::string source = ReadScreenshotSource();
    ASSERT_FALSE(source.empty());

    const size_t function = source.find("bool TryHookScreenshot(");
    const size_t functionEnd = source.find("bool IsHdrDesktop()", function);
    ASSERT_NE(function, std::string::npos);
    ASSERT_NE(functionEnd, std::string::npos);
    const std::string body = source.substr(function, functionEnd - function);

    const size_t sourcePid = body.find("sharedMemory->GetSourcePid()");
    const size_t idleCheck = body.find("if (sourcePid == 0)", sourcePid);
    const size_t requestPublish = body.find("screenshotStatus.store", idleCheck);
    const size_t wait = body.find("WaitForSingleObject", requestPublish);
    ASSERT_NE(sourcePid, std::string::npos);
    ASSERT_NE(idleCheck, std::string::npos);
    ASSERT_NE(requestPublish, std::string::npos);
    ASSERT_NE(wait, std::string::npos);
    EXPECT_LT(idleCheck, requestPublish);
    EXPECT_LT(requestPublish, wait);
}

TEST(ScreenshotSourceTest, DesktopScreenshotSkipsDeadInjectedSourceWithoutWaiting) {
    // After the injected game exits, sourcePid stays stale because the hook
    // dies with its process. The host must detect that before publishing a
    // request; otherwise the desktop fallback blocks for the full hook
    // timeout on an event no one will ever signal.
    const std::string source = ReadScreenshotSource();
    ASSERT_FALSE(source.empty());

    const size_t function = source.find("bool TryHookScreenshot(");
    const size_t functionEnd = source.find("bool IsHdrDesktop()", function);
    ASSERT_NE(function, std::string::npos);
    ASSERT_NE(functionEnd, std::string::npos);
    const std::string body = source.substr(function, functionEnd - function);

    const size_t sourcePid = body.find("sharedMemory->GetSourcePid()");
    const size_t idleCheck = body.find("if (sourcePid == 0)", sourcePid);
    const size_t livenessCheck = body.find("IsSourceProcessAlive(sourcePid)", idleCheck);
    const size_t deadSourceReset = body.find("ResetScreenshotRequestState(sharedMemory)", livenessCheck);
    const size_t requestPublish = body.find("screenshotStatus.store", deadSourceReset);
    const size_t wait = body.find("WaitForSingleObject", requestPublish);
    ASSERT_NE(sourcePid, std::string::npos);
    ASSERT_NE(idleCheck, std::string::npos);
    ASSERT_NE(livenessCheck, std::string::npos);
    ASSERT_NE(deadSourceReset, std::string::npos);
    ASSERT_NE(requestPublish, std::string::npos);
    ASSERT_NE(wait, std::string::npos);
    EXPECT_LT(idleCheck, livenessCheck);
    EXPECT_LT(livenessCheck, deadSourceReset);
    EXPECT_LT(deadSourceReset, requestPublish);
    EXPECT_LT(requestPublish, wait);
}

TEST(ScreenshotSourceTest, SourceLivenessHelperOnlyTreatsProvenDeadProcessesAsDead) {
    const std::string source = ReadScreenshotSource();
    ASSERT_FALSE(source.empty());

    const size_t helper = source.find("bool IsSourceProcessAlive(uint32_t processId)");
    const size_t helperEnd = source.find("bool TryHookScreenshot(", helper);
    ASSERT_NE(helper, std::string::npos);
    ASSERT_NE(helperEnd, std::string::npos);
    const std::string body = source.substr(helper, helperEnd - helper);

    EXPECT_NE(body.find("OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION"), std::string::npos);
    EXPECT_NE(body.find("GetExitCodeProcess"), std::string::npos);
    EXPECT_NE(body.find("STILL_ACTIVE"), std::string::npos);
    // Protected sources may deny even the limited query; only a positive dead
    // result may skip the hook request.
    EXPECT_NE(body.find("ERROR_ACCESS_DENIED"), std::string::npos);
}

TEST(ScreenshotSourceTest, FailedHookRequestLeavesCleanProtocolState) {
    // A request that times out or fails must not leave a stale Pending/Writing
    // status behind: it would make the next screenshot of a freshly injected
    // game skip the hook path and capture the desktop instead.
    const std::string source = ReadScreenshotSource();
    ASSERT_FALSE(source.empty());

    const size_t function = source.find("bool TryHookScreenshot(");
    const size_t functionEnd = source.find("bool IsHdrDesktop()", function);
    ASSERT_NE(function, std::string::npos);
    ASSERT_NE(functionEnd, std::string::npos);
    const std::string body = source.substr(function, functionEnd - function);

    const size_t failureLog = body.find("Hook request %llu failed");
    const size_t cleanup = body.find("CleanupHookPayload(partPath)", failureLog);
    const size_t failureReset = body.find("ResetScreenshotRequestState(sharedMemory)", cleanup);
    ASSERT_NE(failureLog, std::string::npos);
    ASSERT_NE(cleanup, std::string::npos);
    ASSERT_NE(failureReset, std::string::npos);
    EXPECT_LT(cleanup, failureReset);
}
