#include "test_dxgi_shared_shared.h"

// ---------------------------------------------------------------------------
// A failed replacement CreateSwapChainForHwnd is documented-fatal to the game, yet the failure path
// exits CLEANLY (no exception, exit code 0) so the normal crash-dump pipeline never fires
// (dx12_fg_switch_test session 20260813_211734: the second OFF->FSR switch failed E_ACCESSDENIED with
// foreign refs still pinning the old chain, and no .dmp was produced). Every exhausted recovery arm
// must write a session-local diagnostic minidump, the pin probes must bracket the cleanup and the
// live-entry retry so the next run attributes the residual refs, the wrapper destructor must report
// the post-destruction refcount, and the test app must dump plus exit nonzero on its fatal switch
// failure.
// ---------------------------------------------------------------------------
TEST(DXGISharedSourceTest, AccessDeniedExhaustionWritesDiagnosticDumpAndBracketedPinProbes) {
    namespace fs = std::filesystem;
    auto readFile = [](const fs::path& p) {
        EXPECT_TRUE(fs::exists(p)) << p.string();
        const std::string text = ce::test_source::ReadLogicalSource(p);
        EXPECT_FALSE(text.empty()) << p.string();
        return text;
    };

    const std::string tracking =
        readFile(fs::current_path() / "hook" / "apis" / "dx12_hook_swapchain_tracking.cpp");
    ASSERT_FALSE(tracking.empty());
    size_t exhaustionDumps = 0;
    for (size_t pos = tracking.find("CaptureCreateSwapchainAccessDeniedExhaustedDump(hWnd");
         pos != std::string::npos;
         pos = tracking.find("CaptureCreateSwapchainAccessDeniedExhaustedDump(hWnd", pos + 1)) {
        ++exhaustionDumps;
    }
    EXPECT_GE(exhaustionDumps, 2u) << "both deep recovery arms must write the exhaustion dump";
    EXPECT_NE(tracking.find("LogAccessDeniedSwapchainPinDiagnostics(hWnd, \"post-cleanup\")"),
              std::string::npos);
    EXPECT_NE(tracking.find("LogAccessDeniedSwapchainPinDiagnostics(hWnd, \"post-entry-retry\")"),
              std::string::npos);

    const std::string fatalDump =
        readFile(fs::current_path() / "hook" / "main_fatal_dump.cpp");
    ASSERT_FALSE(fatalDump.empty());
    EXPECT_NE(fatalDump.find("CaptureCreateSwapchainAccessDeniedExhaustedDump(HWND hWnd, const char* context)"),
              std::string::npos);
    EXPECT_NE(fatalDump.find("TryCapturePreTerminationDumpWithExternalHelper("), std::string::npos)
        << "the exhaustion dump must prefer the external helper so the game thread is not suspended";
    EXPECT_NE(fatalDump.find("swapchain_access_denied_exhausted.dmp"), std::string::npos);
    EXPECT_NE(fatalDump.find("kMinimalDumpType"), std::string::npos)
        << "the in-process fallback must stay minimal (session 20260813_220022 freeze: 36 s full-memory dump)";

    const std::string inlineCreate =
        readFile(fs::current_path() / "hook" / "apis" / "dx12_hook_swapchain_create.cpp");
    ASSERT_FALSE(inlineCreate.empty());
    EXPECT_NE(inlineCreate.find("CaptureCreateSwapchainAccessDeniedExhaustedDump(hWnd"), std::string::npos)
        << "the INLINE recovery arms must also dump on exhaustion";

    const std::string wrapper =
        readFile(fs::current_path() / "hook" / "wrappers" / "dxgi_swapchain_wrap_lifetime.cpp");
    ASSERT_FALSE(wrapper.empty());
    EXPECT_NE(wrapper.find("post-destruction real refcount=%u"), std::string::npos);

    const std::string appSwitch =
        readFile(fs::current_path() / "testapp" / "dx12_fg_switch_render_switch.cpp");
    ASSERT_FALSE(appSwitch.empty());
    EXPECT_NE(appSwitch.find("WriteFatalSwitchDump(L\"dx12_fg_switch_test_fatal_switch\", 0xE000EACC)"),
              std::string::npos);
    const std::string appCommon = readFile(fs::current_path() / "testapp" / "testapp_common.h");
    ASSERT_FALSE(appCommon.empty());
    EXPECT_EQ(appCommon.find("MiniDumpWithDataSegs"), std::string::npos)
        << "the app-side fatal dump must stay light so it cannot freeze the render thread";
    const std::string appMain = readFile(fs::current_path() / "testapp" / "dx12_fg_switch_test.cpp");
    ASSERT_FALSE(appMain.empty());
    EXPECT_NE(appMain.find("return dx12_fg_switch_test_g_ProcessExitCode;"), std::string::npos);
}
