#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "../captureengine/logger_service_policy.h"

TEST(LoggerServicePolicyTest, SessionDiscoveryPathOverridesExecutableLogsFallback) {
    char discoveryPath[64] = {};
    std::strcpy(discoveryPath, "C:\\captureengine\\logs\\20260717_133007");

    EXPECT_EQ(logger_service_policy::SelectSessionLogsDirectory(discoveryPath, sizeof(discoveryPath),
                                                                "C:\\captureengine\\logs"),
              "C:\\captureengine\\logs\\20260717_133007");
}

TEST(LoggerServicePolicyTest, MissingOrUnterminatedDiscoveryPathUsesFallback) {
    char emptyPath[8] = {};
    char unterminatedPath[8];
    std::memset(unterminatedPath, 'x', sizeof(unterminatedPath));

    EXPECT_EQ(logger_service_policy::SelectSessionLogsDirectory(emptyPath, sizeof(emptyPath), "fallback"),
              "fallback");
    EXPECT_EQ(logger_service_policy::SelectSessionLogsDirectory(unterminatedPath, sizeof(unterminatedPath),
                                                                "fallback"),
              "fallback");
}

TEST(LoggerServicePolicyTest, LogFilenameCannotEscapeSessionDirectory) {
    EXPECT_TRUE(logger_service_policy::IsSafeLogFilename("hook_debug.log"));
    EXPECT_TRUE(logger_service_policy::IsSafeLogFilename("vulkan-layer.log"));
    EXPECT_FALSE(logger_service_policy::IsSafeLogFilename("..\\outside.log"));
    EXPECT_FALSE(logger_service_policy::IsSafeLogFilename("C:outside.log"));
    EXPECT_FALSE(logger_service_policy::IsSafeLogFilename("../outside.log"));
}
