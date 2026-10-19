#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "common/platform/display_timing_etw.h"
#include "captureengine/injection/process_start_etw.h"
#include "tests/source_fragment_reader.h"

namespace {

TEST(ProcessStartEtw, ExtractsBaseNameFromVariousPathFormats) {
    using ce::process_start::ExtractFileName;

    EXPECT_EQ(ExtractFileName(L"\\Device\\HarddiskVolume3\\Games\\Witcher 3\\bin\\x64\\witcher3.exe"), "witcher3.exe");
    EXPECT_EQ(ExtractFileName(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\The Witcher 3\\bin\\x64\\witcher3.exe"),
              "witcher3.exe");
    EXPECT_EQ(ExtractFileName(L"witcher3.exe"), "witcher3.exe");
    EXPECT_EQ(ExtractFileName(L"D:/Games/witcher3.exe"), "witcher3.exe");
    EXPECT_EQ(ExtractFileName(L""), "");
    EXPECT_EQ(ExtractFileName(nullptr), "");
}

TEST(ProcessStartEtw, KernelProcessProviderConstantsAreCorrect) {
    const GUID expectedGuid = {0x22fb2cd6, 0x0e7b, 0x422b, {0xa0, 0xc7, 0x2f, 0xad, 0x1f, 0xd0, 0xe7, 0x16}};
    EXPECT_TRUE(IsEqualGUID(display_timing_etw::kKernelProcessProvider, expectedGuid));
    EXPECT_EQ(display_timing_etw::kKernelProcessKeyword, 0x10ull);
    EXPECT_EQ(display_timing_etw::kKernelProcessStartEventId, 1u);
}

TEST(ProcessStartEtw, SessionsEnableKernelProcessProvider) {
    namespace fs = std::filesystem;
    const auto root = fs::current_path();

    const std::string etw = ce::test_source::ReadFile(root / "common" / "platform" / "display_timing_etw.h");
    ASSERT_FALSE(etw.empty());
    const size_t providerSet = etw.find("inline DisplayTimingProviderStatus EnableDisplayTimingProviders(");
    ASSERT_NE(providerSet, std::string::npos);
    EXPECT_NE(etw.find("kKernelProcessProvider", providerSet), std::string::npos);
    EXPECT_NE(etw.find("kKernelProcessStartEventId", providerSet), std::string::npos);

    const std::string service = ce::test_source::ReadFile(root / "elevationservice" / "service_trace.cpp");
    ASSERT_FALSE(service.empty());
    EXPECT_NE(service.find("providers.kernelProcess"), std::string::npos);

    const std::string startup =
        ce::test_source::ReadFile(root / "captureengine" / "display_timing" / "display_timing_startup.cpp");
    ASSERT_FALSE(startup.empty());
    EXPECT_NE(startup.find("providers.kernelProcess"), std::string::npos);
}

TEST(ProcessStartEtw, PartialCopyEnablesImmediateInjectionWithoutDelay) {
    namespace fs = std::filesystem;
    const auto root = fs::current_path();

    const std::string manager =
        ce::test_source::ReadFile(root / "captureengine" / "injection" / "injection_manager.cpp");
    ASSERT_FALSE(manager.empty());

    const size_t partialCopyPos = manager.find("err == ERROR_PARTIAL_COPY");
    ASSERT_NE(partialCopyPos, std::string::npos);

    const size_t readyPos = manager.find("ready = true;", partialCopyPos);
    ASSERT_NE(readyPos, std::string::npos);
    EXPECT_LT(readyPos - partialCopyPos, 120u) << "ready = true must be set immediately for ERROR_PARTIAL_COPY";

    // Verify injection_manager.cpp line count remains strictly <= 800 lines
    std::ifstream stream(root / "captureengine" / "injection" / "injection_manager.cpp");
    size_t lineCount = 0;
    std::string line;
    while (std::getline(stream, line)) {
        ++lineCount;
    }
    EXPECT_LE(lineCount, 800u) << "injection_manager.cpp must not exceed the 800-line ceiling";
}

TEST(ProcessStartEtw, EtwProcessStartEventRoutesToInjectionThread) {
    namespace fs = std::filesystem;
    const auto root = fs::current_path();

    const std::string wmiEvents =
        ce::test_source::ReadFile(root / "captureengine" / "injection" / "injection_wmi_events.cpp");
    ASSERT_FALSE(wmiEvents.empty());

    const size_t handlerPos = wmiEvents.find("InjectionManager::HandleEtwProcessStart");
    ASSERT_NE(handlerPos, std::string::npos);
    EXPECT_NE(wmiEvents.find("LaunchDelayedInjectionThread(pid, imageName, \"ETWStartTrace\")", handlerPos),
              std::string::npos);
    EXPECT_NE(wmiEvents.find("TerminateNgxUpdaterIfDisabled(pid, imageName, \"ETWStartTrace\")", handlerPos),
              std::string::npos);
}

}  // namespace
