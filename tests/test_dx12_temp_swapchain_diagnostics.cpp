// CE's temp-swapchain Present-hook bootstrap used to abandon three first-order
// failures in complete silence, which made a dead bootstrap indistinguishable
// from one that never ran.
//
// Witcher 3 (DX12) session 20260820_142322 is the case that exposed it: the log
// says "DX12Hook: Installing Present hooks eagerly (no D3D12 wrapper)" and then
// nothing at all for 65 ms before "Present hooks deferred". Whether the factory,
// the device or the command queue had failed - and with what HRESULT - was
// unrecoverable from the artifacts.

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "source_fragment_reader.h"

namespace {

std::string FunctionBody(const std::string& source, const std::string& signature, const std::string& nextSignature) {
    const size_t begin = source.find(signature);
    if (begin == std::string::npos)
        return {};
    const size_t end = source.find(nextSignature, begin + signature.size());
    return source.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

}  // namespace

TEST(DX12TempSwapchainDiagnosticsTest, EveryAbandonedBootstrapStepReportsItsHResult) {
    const std::string source = ce::test_source::ReadLogicalSource(std::filesystem::current_path() /
                                                                  "hook/apis/dx12_hook_hook_install.cpp");
    ASSERT_FALSE(source.empty());
    const std::string body =
        FunctionBody(source, "void HookSwapchainVTableViaTempSwapchain(bool presentOnly, bool guardedSystemRouteOnly)",
                     "\nIDXGISwapChain1* pSwapChain = nullptr;");
    ASSERT_FALSE(body.empty());

    EXPECT_NE(body.find("CreateDXGIFactory1 failed (hr=0x%08X)"), std::string::npos);
    EXPECT_NE(body.find("D3D12CreateDevice failed (hr=0x%08X)"), std::string::npos);
    EXPECT_NE(body.find("CreateCommandQueue failed (hr=0x%08X)"), std::string::npos);

    // The HRESULT has to be captured, not re-derived: a bare FAILED(call(...))
    // condition is exactly what threw the value away.
    EXPECT_NE(body.find("HRESULT factoryHr = "), std::string::npos);
    EXPECT_NE(body.find("HRESULT deviceHr = "), std::string::npos);
    EXPECT_NE(body.find("HRESULT queueHr = "), std::string::npos);
}
