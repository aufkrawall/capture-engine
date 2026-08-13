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
    EXPECT_NE(fatalDump.find("ShouldUseInProcessMiniDumpFallbackAfterExternalHelperFailure("),
              std::string::npos)
        << "the in-process fallback must be refused while foreign overlay modules are loaded "
           "(session 20260813_222058 freeze: the game's own dump deadlocked inside Steam's hooked version APIs)";

    const std::string inlineCreate =
        readFile(fs::current_path() / "hook" / "apis" / "dx12_hook_swapchain_create.cpp");
    ASSERT_FALSE(inlineCreate.empty());
    EXPECT_NE(inlineCreate.find("CaptureCreateSwapchainAccessDeniedExhaustedDump(hWnd"), std::string::npos)
        << "the INLINE recovery arms must also dump on exhaustion";

    const std::string externalDump = readFile(fs::current_path() / "hook" / "main_external_dump.cpp");
    ASSERT_FALSE(externalDump.empty());
    EXPECT_NE(externalDump.find("CopyCompletedDumpFile(hProcess"), std::string::npos)
        << "the session mirror must be a plain copy of the game's completed dump, never a nested "
           "MiniDumpWriteDump re-entry (session 20260813_222058 freeze)";
    EXPECT_EQ(externalDump.find("WriteSupplementalCrashDump(sourcePath"), std::string::npos)
        << "the rich supplemental dump must not run in-process inside the game's dump call";

    const std::string wrapper =
        readFile(fs::current_path() / "hook" / "wrappers" / "dxgi_swapchain_wrap_lifetime.cpp");
    ASSERT_FALSE(wrapper.empty());
    EXPECT_NE(wrapper.find("post-destruction real refcount=%u"), std::string::npos);
    EXPECT_NE(wrapper.find("LogSwapChainLifetimeDiagnostics(pRealToFree"), std::string::npos)
        << "the post-destruction probe must attribute the remaining refs and the vtable-slot owners";

    const std::string attribution =
        readFile(fs::current_path() / "hook" / "wrappers" / "dxgi_swapchain_wrap_attribution.cpp");
    ASSERT_FALSE(attribution.empty());
    EXPECT_NE(attribution.find("AttributionAddRefHook"), std::string::npos);
    EXPECT_NE(attribution.find("AttributionReleaseHook"), std::string::npos);
    EXPECT_NE(attribution.find("return original(self)"), std::string::npos)
        << "the attribution hooks must ALWAYS forward to the saved original — the patched vtable is "
           "shared by every chain the factory creates, and a dropped AddRef/Release corrupts dxgi's "
           "internal refcounts (session 20260813_233025 telemetry heap crash)";
    EXPECT_NE(attribution.find("IsThirdPartyOverlayLoaded()"), std::string::npos)
        << "the attribution vtable hooks must stay out of foreign-overlay processes so Steam/RTSS "
           "vtable state is never disturbed";

    const std::string appSwitch =
        readFile(fs::current_path() / "testapp" / "dx12_fg_switch_render_switch.cpp");
    ASSERT_FALSE(appSwitch.empty());
    EXPECT_NE(appSwitch.find("WriteFatalSwitchDump(L\"dx12_fg_switch_test_fatal_switch\", 0xE000EACC)"),
              std::string::npos);
    const std::string appCommon = readFile(fs::current_path() / "testapp" / "testapp_common.h");
    ASSERT_FALSE(appCommon.empty());
    EXPECT_EQ(appCommon.find("MiniDumpWithDataSegs"), std::string::npos)
        << "the app-side fatal dump must stay light so it cannot freeze the render thread";
    EXPECT_NE(appCommon.find("TryCaptureFatalSwitchDumpWithExternalHelper("), std::string::npos)
        << "the app-side fatal dump must prefer the external helper; the in-process dbghelp path "
           "deadlocks with foreign overlay hooks loaded";
    EXPECT_NE(appCommon.find("IsForeignOverlayModuleLoaded"), std::string::npos)
        << "the in-process app-side fallback must be refused while foreign overlay modules are loaded";
    EXPECT_NE(appSwitch.find("OFF mode keeps the FSR swapchain/context alive"), std::string::npos)
        << "OFF-after-FSR must keep the FFX chain alive instead of recreating a foreign-pinned native chain";
    const std::string appMain = readFile(fs::current_path() / "testapp" / "dx12_fg_switch_test.cpp");
    ASSERT_FALSE(appMain.empty());
    EXPECT_NE(appMain.find("return dx12_fg_switch_test_g_ProcessExitCode;"), std::string::npos);
}
