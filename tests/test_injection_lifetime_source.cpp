#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "source_fragment_reader.h"

TEST(InjectionLifetimeSourceTest, DelayedWorkersRemainOwnedUntilJoin) {
    const std::filesystem::path source = std::filesystem::current_path() / "captureengine" / "injection.cpp";
    const std::string text = ce::test_source::ReadLogicalSource(source);
    ASSERT_FALSE(text.empty());

    EXPECT_EQ(text.find("t.detach()"), std::string::npos);
    EXPECT_EQ(text.find("detaching to avoid indefinite block"), std::string::npos);
    EXPECT_NE(text.find("t.join()"), std::string::npos);
    EXPECT_NE(text.find("to preserve manager lifetime"), std::string::npos);
    EXPECT_EQ(text.find("shared_from_this()"), std::string::npos);
    EXPECT_NE(text.find("MarkDoneAndDrain()"), std::string::npos);
    EXPECT_EQ(text.find("Brief delay to let thread pool drain"), std::string::npos);
    const size_t shutdownWmi = text.find("ShutdownWMI();");
    const size_t waitWorkers = text.find("WaitForInjectionThreads(5000);", shutdownWmi);
    ASSERT_NE(shutdownWmi, std::string::npos);
    EXPECT_NE(waitWorkers, std::string::npos);
}

TEST(InjectionLifetimeSourceTest, PublicationCallbackPrecedesRealtimeProcessMonitoring) {
    const std::filesystem::path source = std::filesystem::current_path() / "captureengine" / "inject_main.cpp";
    const std::string text = ce::test_source::ReadFile(source);
    ASSERT_FALSE(text.empty());

    const size_t callback = text.find("manager->SetOnInjectCallback(");
    const size_t monitoring = text.find("manager->StartMonitoring();", callback);
    const size_t construction = text.find("std::make_shared<InjectionManager>(injectorState.config)", monitoring);
    const size_t configure = text.find("configureInjector(injector);", construction);
    ASSERT_NE(callback, std::string::npos);
    ASSERT_NE(monitoring, std::string::npos);
    ASSERT_NE(construction, std::string::npos);
    ASSERT_NE(configure, std::string::npos);
    EXPECT_LT(callback, monitoring);
    EXPECT_LT(construction, configure);
}

TEST(InjectionLifetimeSourceTest, AsyncWmiFailureFallsBackOnlyFromTheManagerThread) {
    const std::string sink = ce::test_source::ReadFile(std::filesystem::current_path() / "captureengine" /
                                                       "injection_wmi_events.cpp");
    const std::string manager = ce::test_source::ReadFile(std::filesystem::current_path() / "captureengine" /
                                                          "injection_manager.cpp");
    ASSERT_FALSE(sink.empty());
    ASSERT_FALSE(manager.empty());

    const size_t setStatus = sink.find("ProcessEventSink::SetStatus");
    const size_t enterCallback = sink.find("EnterCallback()", setStatus);
    const size_t request = sink.find("pManager->RequestWmiFallback(result)", setStatus);
    const size_t expectedCancellation = sink.find("result == WBEM_E_CALL_CANCELLED && !fallbackQueued", request);
    const size_t update = manager.find("void InjectionManager::Update()");
    const size_t service = manager.find("ServiceWmiFallbackRequest();", update);
    const size_t injectLock = manager.find("std::lock_guard<std::mutex> lock(injectMutex);", update);
    const size_t fallback = manager.find("StartPolledWmiFallback(reason, \"asynchronously\")", update);
    const size_t catchupScan = manager.find("ScanExistingProcesses();", fallback);
    ASSERT_NE(setStatus, std::string::npos);
    ASSERT_NE(enterCallback, std::string::npos);
    ASSERT_NE(request, std::string::npos);
    ASSERT_NE(expectedCancellation, std::string::npos);
    ASSERT_NE(service, std::string::npos);
    ASSERT_NE(injectLock, std::string::npos);
    ASSERT_NE(fallback, std::string::npos);
    ASSERT_NE(catchupScan, std::string::npos);
    EXPECT_EQ(sink.find("ExecNotificationQueryAsync"), std::string::npos);
    EXPECT_LT(enterCallback, request);
    EXPECT_LT(request, expectedCancellation);
    EXPECT_LT(service, injectLock);
    EXPECT_LT(fallback, catchupScan);

    const size_t immediateFailure = manager.find("if (FAILED(realtimeHr))");
    const size_t claimFallback = manager.find("WmiSubscriptionState::kFallbackActive", immediateFailure);
    const size_t immediateFallback = manager.find("StartPolledWmiFallback(fallbackReason, \"immediately\")",
                                                  claimFallback);
    ASSERT_NE(immediateFailure, std::string::npos);
    ASSERT_NE(claimFallback, std::string::npos);
    ASSERT_NE(immediateFallback, std::string::npos);
    EXPECT_LT(claimFallback, immediateFallback);
    EXPECT_EQ(manager.find("wmiRealtimeFallbackArmed"), std::string::npos);
    EXPECT_EQ(manager.find("wmiFallbackRequested"), std::string::npos);
}

TEST(InjectionLifetimeSourceTest, DuplicateProcessDiscoverySharesOneInjectionWorker) {
    const std::string manager = ce::test_source::ReadFile(std::filesystem::current_path() / "captureengine" /
                                                          "injection_manager.cpp");
    ASSERT_FALSE(manager.empty());

    EXPECT_NE(manager.find("delayedInjectionPids.insert(pid).second"), std::string::npos);
    EXPECT_NE(manager.find("delayedInjectionPids.erase(pid);"), std::string::npos);
    EXPECT_NE(manager.find("coalescing duplicate discovery"), std::string::npos);
}
