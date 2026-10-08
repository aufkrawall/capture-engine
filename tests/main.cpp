#include <gtest/gtest.h>
#include <optional>

std::optional<int> RunRuntimePackagePathProbe(int argc, char** argv);

int main(int argc, char** argv) {
    if (const auto probe = RunRuntimePackagePathProbe(argc, argv))
        return *probe;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
