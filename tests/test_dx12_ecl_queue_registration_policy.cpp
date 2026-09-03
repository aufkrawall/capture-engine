#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../hook/common/dx12_overlay_policy/ecl_queue_registration.h"

#include "source_fragment_reader.h"

namespace {

using ce::dx12_overlay_policy::ShouldRegisterCommandQueueFromExecuteCommandLists;

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

// Discovery: without frame generation the ExecuteCommandLists hook is how CE finds
// the game's render queue, so registration must still run on every submission.
TEST(Dx12EclQueueRegistrationPolicyTest, RegistersWhileTheGameQueueIsStillBeingDiscovered) {
    EXPECT_TRUE(ShouldRegisterCommandQueueFromExecuteCommandLists(/*frameGenerationActive=*/false,
                                                                  /*hasPrimaryGameQueue=*/false));
    EXPECT_TRUE(ShouldRegisterCommandQueueFromExecuteCommandLists(/*frameGenerationActive=*/false,
                                                                  /*hasPrimaryGameQueue=*/true));
    // Frame generation running before CE ever saw a DIRECT queue is still discovery.
    EXPECT_TRUE(ShouldRegisterCommandQueueFromExecuteCommandLists(/*frameGenerationActive=*/true,
                                                                  /*hasPrimaryGameQueue=*/false));
}

// The regression this exists for: a frame-generation runtime submits from its own
// internal queues, which CE never recognises. Registering them re-points
// g_CommandQueue under the global command-queue mutex on the runtime's submission
// threads — measured at 1290 of 1290 submissions per second under 2x FSR FG, worth
// 1.9 ms per base frame.
TEST(Dx12EclQueueRegistrationPolicyTest, NeverRegistersWhileFrameGenerationOwnsSubmission) {
    EXPECT_FALSE(ShouldRegisterCommandQueueFromExecuteCommandLists(/*frameGenerationActive=*/true,
                                                                   /*hasPrimaryGameQueue=*/true));
}

// The decision must not depend on anything per-submission, or the hot path pays for
// evaluating it. Same inputs, same answer, every time.
TEST(Dx12EclQueueRegistrationPolicyTest, IsAPureFunctionOfTheTwoInputs) {
    for (int frameGeneration = 0; frameGeneration <= 1; ++frameGeneration) {
        for (int primaryQueue = 0; primaryQueue <= 1; ++primaryQueue) {
            const bool first =
                ShouldRegisterCommandQueueFromExecuteCommandLists(frameGeneration != 0, primaryQueue != 0);
            for (int repeat = 0; repeat < 4; ++repeat) {
                EXPECT_EQ(ShouldRegisterCommandQueueFromExecuteCommandLists(frameGeneration != 0, primaryQueue != 0),
                          first);
            }
            EXPECT_EQ(first, frameGeneration == 0 || primaryQueue == 0);
        }
    }
}

// The call site must ask the policy rather than re-deriving the old "unknown queue"
// test, and it must still report registrations so a return of the regression is
// visible in the log rather than only in the frame rate.
TEST(Dx12EclQueueRegistrationPolicyTest, ExecuteCommandListsDetourUsesThePolicyAndCountsRegistrations) {
    const std::string ecl = ReadSource("hook/apis/dx12_hook_ecl.cpp");
    EXPECT_NE(ecl.find("ShouldRegisterCommandQueueFromExecuteCommandLists"), std::string::npos);
    EXPECT_NE(ecl.find("g_EclQueueRegistrationsThisWindow"), std::string::npos);
    EXPECT_NE(ecl.find("registrations=%u"), std::string::npos);
    // The old condition must be gone: it is what re-registered a runtime queue on
    // every submission.
    EXPECT_EQ(ecl.find("if (!anyFGActive || !primaryQ || !isKnownQueue)"), std::string::npos);
}

}  // namespace
