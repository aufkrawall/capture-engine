// Process-wide checks after a flow scenario's test body. Teardown is part of the scenario: the game releases
// its swapchain and the runtime contexts and CE shuts down after the body's own checks, and one scenario runs
// per process, so the process-wide tally checked here belongs to that scenario.

#include "tests/flow/flow_test_support.h"
#include "tests/flow/fakes/ngx/ngx_fake_api.h"

namespace {

class FlowTeardownChecks : public ::testing::Environment {
public:
    void TearDown() override {
        if (const HMODULE module = GetModuleHandleA("nvngx.dll")) {
            const auto get =
                reinterpret_cast<ce::flow::ngx::GetCounters>(GetProcAddress(module, "CEFlowNGX_GetCounters"));
            ASSERT_NE(get, nullptr);
            ce::flow::ngx::Counters counts;
            get(&counts);
            EXPECT_EQ(counts.liveFeatures, 0u) << "NGX features survived scenario cleanup";
            EXPECT_EQ(counts.liveParameters, 0u) << "NGX parameters survived scenario cleanup";
        }
        ce::flow::ExpectNoDebugLayerErrors("teardown", false);
    }
};

// GoogleTest requires registration before its library-provided main.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
[[maybe_unused]] ::testing::Environment* const g_flowTeardownChecks =
    ::testing::AddGlobalTestEnvironment(new FlowTeardownChecks);

}  // namespace
