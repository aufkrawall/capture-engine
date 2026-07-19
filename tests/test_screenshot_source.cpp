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
