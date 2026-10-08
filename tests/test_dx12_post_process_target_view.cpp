#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "source_fragment_reader.h"

namespace {

std::string ReadHookSource(const char* relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

TEST(DX12PostProcessTargetViewTest, PassWritesItsRenderTargetViewForEveryRecordingInsteadOfTrustingASamePointer) {
    const std::string pass = ReadHookSource("hook/sharpen/sharpen_d3d12.cpp");
    const std::string header = ReadHookSource("hook/sharpen/sharpen_d3d12.h");
    ASSERT_FALSE(pass.empty());
    ASSERT_FALSE(header.empty());
    // A view cached by resource pointer is stale once the resource was destroyed and a new back buffer took its
    // address (flow FlowDLSS.NativeReturn..., ~1 in 12 under load: OMSetRenderTargets error 1042, device removed).
    EXPECT_EQ(header.find("viewedTarget_"), std::string::npos);
    EXPECT_EQ(pass.find("viewedTarget_"), std::string::npos);
    const size_t begin = pass.find("bool D3D12Pass::EnsureTargetView(");
    ASSERT_NE(begin, std::string::npos);
    const size_t end = pass.find("int D3D12Pass::AcquireAllocatorSlot()", begin);
    ASSERT_NE(end, std::string::npos);
    const std::string body = pass.substr(begin, end - begin);
    const size_t write = body.find("CreateRenderTargetView(target");
    ASSERT_NE(write, std::string::npos);
    // No early return ahead of the write, and the only return follows it.
    EXPECT_EQ(body.find("return"), body.rfind("return"));
    EXPECT_LT(write, body.find("return"));
}

}  // namespace
