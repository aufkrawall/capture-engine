#include <gtest/gtest.h>

#include <windows.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include "../hook/common/dxgi_shared_internal.h"
#include "source_fragment_reader.h"

namespace {

// A `call rax` through NULL, exactly the shape the recovery exists for: RIP=0,
// RAX=0 and a return address on the stack.
struct NullCallFault {
    EXCEPTION_RECORD record{};
    CONTEXT context{};
    EXCEPTION_POINTERS pointers{};
    uintptr_t stack[4] = {};

    NullCallFault() {
        record.ExceptionCode = STATUS_ACCESS_VIOLATION;
        record.NumberParameters = 2;
        record.ExceptionInformation[0] = 8;
        record.ExceptionInformation[1] = 0;
        stack[0] = reinterpret_cast<uintptr_t>(&::GetTickCount);
#ifdef _WIN64
        context.Rsp = reinterpret_cast<DWORD64>(&stack[0]);
#else
        context.Esp = reinterpret_cast<DWORD>(&stack[0]);
#endif
        pointers.ExceptionRecord = &record;
        pointers.ContextRecord = &context;
    }
};

}  // namespace

// The recovery handler is registered once for the whole process, so it must
// ignore every fault that is not raised under an armed guard on the faulting
// thread. Before, any thread's NULL call with a Steam return address would
// have had Steam's memory patched.
TEST(SteamNullCallbackRecoveryTest, HandlerIgnoresFaultsOutsideAnArmedGuard) {
    NullCallFault fault;
    // CONTEXT has padding and no unique object representation, so snapshot and
    // compare raw bytes instead of the struct; the fixture value-initializes it
    // whole, padding included, which makes the comparison deterministic.
    std::array<unsigned char, sizeof(CONTEXT)> before{};
    std::memcpy(before.data(), &fault.context, before.size());
    EXPECT_EQ(DXGIShared::SteamOverlayInitVehHandler(&fault.pointers), EXCEPTION_CONTINUE_SEARCH);
    std::array<unsigned char, sizeof(CONTEXT)> after{};
    std::memcpy(after.data(), &fault.context, after.size());
    EXPECT_EQ(before, after) << "an unarmed thread's context must not change";
}

TEST(SteamNullCallbackRecoveryTest, GuardArmsOnlyItsOwnThreadAndDisarmsOnExit) {
    EXPECT_FALSE(DXGIShared::dxgi_shared_s_steamNullCallbackRecoveryContext.active);
    {
        int hook = 0;
        int bypass = 0;
        DXGIShared::ScopedSteamNullCallbackRecoveryGuard guard(true, "test", "Present", &hook, &bypass, false, false);
        ASSERT_TRUE(guard.IsInstalled());
        EXPECT_TRUE(DXGIShared::dxgi_shared_s_steamNullCallbackRecoveryContext.active);

        bool otherThreadArmed = true;
        std::thread other([&]() {
            otherThreadArmed = DXGIShared::dxgi_shared_s_steamNullCallbackRecoveryContext.active;
            NullCallFault fault;
            EXPECT_EQ(DXGIShared::SteamOverlayInitVehHandler(&fault.pointers), EXCEPTION_CONTINUE_SEARCH);
        });
        other.join();
        EXPECT_FALSE(otherThreadArmed);
    }
    EXPECT_FALSE(DXGIShared::dxgi_shared_s_steamNullCallbackRecoveryContext.active);

    DXGIShared::ScopedSteamNullCallbackRecoveryGuard disabled(false, "test", "Present", nullptr, nullptr, false,
                                                              false);
    EXPECT_FALSE(disabled.IsInstalled());
    EXPECT_FALSE(DXGIShared::dxgi_shared_s_steamNullCallbackRecoveryContext.active);
}

TEST(SteamNullCallbackRecoveryTest, HandlerRegistrationIsProcessLifetimeAndIdempotent) {
    EXPECT_TRUE(DXGIShared::EnsureSteamNullCallbackRecoveryHandlerRegistered());
    EXPECT_TRUE(DXGIShared::EnsureSteamNullCallbackRecoveryHandlerRegistered());

    namespace fs = std::filesystem;
    const std::string guard = ce::test_source::ReadFile(fs::current_path() / "hook" / "common" /
                                                        "dxgi_shared_internal.h");
    ASSERT_FALSE(guard.empty());
    EXPECT_EQ(guard.find("AddVectoredExceptionHandler"), std::string::npos)
        << "the per-Present guard must not add and remove a process-wide handler";
    EXPECT_EQ(guard.find("RemoveVectoredExceptionHandler"), std::string::npos);
}

// A fixed Steam RVA (0x1621d8, from one 2025 x64 build, and wrong for x86)
// is no longer a write target: only a slot proven by the faulting code is.
TEST(SteamNullCallbackRecoveryTest, HandlerNeverWritesAFixedSteamRva) {
    namespace fs = std::filesystem;
    const std::string handler =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "common" / "dxgi_shared_steam_veh.cpp");
    ASSERT_FALSE(handler.empty());
    EXPECT_EQ(handler.find("kSteamCallbackRva"), std::string::npos);
    EXPECT_EQ(handler.find("steamStart + 0x"), std::string::npos);
    EXPECT_EQ(handler.find("steamStart + kSteam"), std::string::npos);
    const size_t entry = handler.find("LONG CALLBACK SteamOverlayInitVehHandler(");
    const size_t activeGate = handler.find("if (!dxgi_shared_s_steamNullCallbackRecoveryContext.active)", entry);
    const size_t firstRecovery = handler.find("TryRecoverForeignOverlayInvokeCrash(ep)", entry);
    ASSERT_NE(entry, std::string::npos);
    ASSERT_NE(activeGate, std::string::npos);
    EXPECT_LT(activeGate, firstRecovery);
}

// The handler runs first for every exception on every thread, including loader worker threads that have
// no thread-local block for the module. Asking whether the thread is armed must therefore not read
// thread-local storage (the sanitizer pass died of exactly that: the handler faulted inside itself and
// re-entered until the stack was gone).
TEST(SteamNullCallbackRecoveryTest, ArmedThreadSetAnswersByThreadIdWithoutThreadLocalStorage) {
    ce::steam_recovery::ArmedThreadSet<2> armed;
    size_t first = 0;
    size_t second = 0;
    size_t third = 0;
    EXPECT_FALSE(armed.Contains(11));
    ASSERT_EQ(armed.Arm(11, &first), ce::steam_recovery::ArmResult::kArmed);
    EXPECT_TRUE(armed.Contains(11));
    EXPECT_FALSE(armed.Contains(12));
    EXPECT_EQ(armed.Arm(11, &third), ce::steam_recovery::ArmResult::kAlreadyArmed) << "a nested guard";
    EXPECT_EQ(third, armed.kNoSlot);
    ASSERT_EQ(armed.Arm(12, &second), ce::steam_recovery::ArmResult::kArmed);
    EXPECT_EQ(armed.Arm(13, &third), ce::steam_recovery::ArmResult::kFull);
    EXPECT_FALSE(armed.Contains(13));

    armed.Disarm(first);
    EXPECT_FALSE(armed.Contains(11));
    EXPECT_TRUE(armed.Contains(12));
    EXPECT_EQ(armed.Arm(13, &third), ce::steam_recovery::ArmResult::kArmed) << "a freed slot is reused";
    EXPECT_FALSE(armed.Contains(0));
    size_t none = 0;
    EXPECT_EQ(armed.Arm(0, &none), ce::steam_recovery::ArmResult::kFull) << "0 is the empty marker";
}

TEST(SteamNullCallbackRecoveryTest, GuardRegistersItsThreadAndAnOuterGuardKeepsTheRegistration) {
    const DWORD self = GetCurrentThreadId();
    EXPECT_FALSE(ce::steam_recovery::g_armedThreads.Contains(self));
    int hook = 0;
    {
        DXGIShared::ScopedSteamNullCallbackRecoveryGuard outer(true, "outer", "Present", &hook, nullptr, false, false);
        ASSERT_TRUE(outer.IsInstalled());
        EXPECT_TRUE(ce::steam_recovery::g_armedThreads.Contains(self));
        {
            DXGIShared::ScopedSteamNullCallbackRecoveryGuard inner(true, "inner", "Present", &hook, nullptr, false,
                                                                   false);
            ASSERT_TRUE(inner.IsInstalled());
            EXPECT_TRUE(ce::steam_recovery::g_armedThreads.Contains(self));
        }
        EXPECT_TRUE(ce::steam_recovery::g_armedThreads.Contains(self)) << "the inner guard must not disarm the outer";
        EXPECT_TRUE(DXGIShared::dxgi_shared_s_steamNullCallbackRecoveryContext.active);
    }
    EXPECT_FALSE(ce::steam_recovery::g_armedThreads.Contains(self));
    EXPECT_FALSE(DXGIShared::dxgi_shared_s_steamNullCallbackRecoveryContext.active);

    bool otherThreadArmed = true;
    {
        DXGIShared::ScopedSteamNullCallbackRecoveryGuard guard(true, "test", "Present", &hook, nullptr, false, false);
        std::thread other([&]() { otherThreadArmed = ce::steam_recovery::g_armedThreads.Contains(GetCurrentThreadId()); });
        other.join();
    }
    EXPECT_FALSE(otherThreadArmed);
}

TEST(SteamNullCallbackRecoveryTest, HandlerAsksTheArmedThreadSetBeforeTouchingThreadLocalState) {
    namespace fs = std::filesystem;
    const std::string handler =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "common" / "dxgi_shared_steam_veh.cpp");
    ASSERT_FALSE(handler.empty());
    const size_t entry = handler.find("LONG CALLBACK SteamOverlayInitVehHandler(");
    ASSERT_NE(entry, std::string::npos);
    const size_t armedSet = handler.find("g_armedThreads.Contains(GetCurrentThreadId())", entry);
    const size_t threadLocal = handler.find("dxgi_shared_s_steamNullCallbackRecoveryContext", entry);
    ASSERT_NE(armedSet, std::string::npos);
    ASSERT_NE(threadLocal, std::string::npos);
    EXPECT_LT(armedSet, threadLocal) << "a thread-local read here faults on loader worker threads";
    EXPECT_NE(handler.substr(entry > 120 ? entry - 120 : 0, 140).find("no_sanitize(\"address\")"), std::string::npos)
        << "the process-wide handler must not be AddressSanitizer-instrumented";
}
