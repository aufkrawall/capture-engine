#include <gtest/gtest.h>

#include <windows.h>

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
    const CONTEXT before = fault.context;
    EXPECT_EQ(DXGIShared::SteamOverlayInitVehHandler(&fault.pointers), EXCEPTION_CONTINUE_SEARCH);
    EXPECT_EQ(memcmp(&before, &fault.context, sizeof(CONTEXT)), 0) << "an unarmed thread's context must not change";
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
