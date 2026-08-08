#pragma once

#include <gtest/gtest.h>
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

class ConfigOverrideTest : public ::testing::Test {
protected:
    std::string tempConfigFile;

    void SetUp() override {
        // Include the process id, exactly like MakeTestPath() in test_config_shared.h: the
        // product suite and the isolated sanitizer suite run concurrently under --verify
        // from the same working directory, and a fixed name let them delete and overwrite
        // each other's config file mid-test.
        const std::string uniqueName = "test_config_override." + std::to_string(GetCurrentProcessId()) + ".ini";
        tempConfigFile = (std::filesystem::current_path() / uniqueName).string();
        remove(tempConfigFile.c_str());
    }

    void TearDown() override { remove(tempConfigFile.c_str()); }

    void WriteConfig(const std::string& content) {
        std::ofstream out(tempConfigFile);
        out << content;
        out.close();
    }
};
