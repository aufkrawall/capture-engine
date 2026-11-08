#include <gtest/gtest.h>
#include <optional>

std::optional<int> RunRuntimePackagePathProbe(int argc, char** argv);
std::optional<int> RunFatalExitChildProbe(int argc, char** argv);

int main(int argc, char** argv) {
    if (const auto probe = RunRuntimePackagePathProbe(argc, argv))
        return *probe;
    if (const auto fatalProbe = RunFatalExitChildProbe(argc, argv))
        return *fatalProbe;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
