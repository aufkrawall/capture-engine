// Process-wide checks after a flow scenario's test body. Teardown is part of the scenario: the game releases
// its swapchain and the runtime contexts and CE shuts down after the body's own checks, and one scenario runs
// per process, so the process-wide tally checked here belongs to that scenario.

#include "tests/flow/flow_test_support.h"

namespace {

class FlowTeardownChecks : public ::testing::Environment {
public:
    void TearDown() override { ce::flow::ExpectNoDebugLayerErrors("teardown", false); }
};

// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - GoogleTest requires registration before its library-provided main
[[maybe_unused]] ::testing::Environment* const g_flowTeardownChecks =
    ::testing::AddGlobalTestEnvironment(new FlowTeardownChecks);

}  // namespace
