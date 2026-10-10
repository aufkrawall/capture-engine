#include "tests/flow/flow_test_support.h"

#include <filesystem>

TEST(FlowLoaderBootstrap, NativeStreamlineCoreLoadBeforeFullHooksUsesOverride) {
    char executable[MAX_PATH] = {};
    ASSERT_NE(GetModuleFileNameA(nullptr, executable, MAX_PATH), 0u);
    const auto directory = std::filesystem::path(executable).parent_path();
    const auto vendorDirectory = directory / "logs" / ce::flow::CurrentTestName() / "vendor";
    std::filesystem::create_directories(vendorDirectory);
    const auto requested = vendorDirectory / "sl.common.dll";
    std::filesystem::copy_file(directory / "sl.common.dll", requested,
                               std::filesystem::copy_options::overwrite_existing);
    const auto requestedPath = requested.string();
    const auto overridePath = directory.string();
    CEFlowEarlyLoaderProbe probe{requestedPath.c_str(), overridePath.c_str(), nullptr};
    {
        ce::flow::FlowGame game(ce::flow::CurrentTestName(), &probe);
        ASSERT_TRUE(game.Error().empty()) << game.Error();
        ASSERT_NE(probe.loadedModule, nullptr);
        char loadedPath[MAX_PATH] = {};
        ASSERT_NE(GetModuleFileNameA(probe.loadedModule, loadedPath, MAX_PATH), 0u);
        EXPECT_TRUE(std::filesystem::equivalent(loadedPath, directory / "sl.common.dll"));
        EXPECT_FALSE(std::filesystem::equivalent(loadedPath, requested));
        EXPECT_TRUE(FreeLibrary(probe.loadedModule));
    }
    std::filesystem::remove(requested);
}
