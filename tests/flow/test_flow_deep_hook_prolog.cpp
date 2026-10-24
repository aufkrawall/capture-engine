// A deep hook's private trampoline replays the prolog a foreign entry jump skipped, then resumes in the body. For
// short native signatures the prolog may load a security cookie RIP-relative (AMD's ffxDestroyContext); copied
// verbatim, that load read the same distance past the trampoline instead of the cookie, so a forwarded native call
// read foreign memory or failed its cookie check.

#include "tests/flow/flow_test_support.h"

#include <cstdint>
#include <cstring>

extern "C" {
__attribute__((used)) volatile uint64_t g_flowDeepHookCookie = 0x1122334455667788ull;

// A cookie prolog that returns the cookie it loaded (rax ^ rsp ^ rsp), with room after it for CE's resume patch.
__attribute__((naked, noinline)) uint64_t FlowDeepHookCookieFunction(void*, void*) {
    __asm__(
        "sub $0x38, %rsp\n"
        "mov g_flowDeepHookCookie(%rip), %rax\n"
        "xor %rsp, %rax\n"
        "mov %rax, 0x30(%rsp)\n"
        ".rept 40\n nop\n .endr\n"
        "mov 0x30(%rsp), %rax\n"
        "xor %rsp, %rax\n"
        "add $0x38, %rsp\n"
        "ret\n");
}
}

namespace {

using namespace ce::flow;

uint64_t ForeignEntryHandler(void*, void*) {
    return 0;
}

uint64_t DeepHookWrapper(void*, void*) {
    return 0;
}

// The end of the prolog's `mov rax, [rip+disp32]`, or 0.
size_t CookieLoadEnd(const unsigned char* code) {
    for (size_t i = 0; i + 7 <= 24; ++i) {
        if (code[i] == 0x48 && code[i + 1] == 0x8B && code[i + 2] == 0x05) {
            return i + 7;
        }
    }
    return 0;
}

bool WriteCode(void* target, const void* bytes, size_t size) {
    DWORD protection = 0;
    if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &protection)) {
        return false;
    }
    std::memcpy(target, bytes, size);
    DWORD ignored = 0;
    const bool restored = VirtualProtect(target, size, protection, &ignored) != FALSE;
    return FlushInstructionCache(GetCurrentProcess(), target, size) != FALSE && restored;
}

TEST(FlowDeepHookProlog, ARipRelativeCookieLoadInTheReplayedPrologStillReadsTheCookie) {
    FlowGame game(CurrentTestName());
    const HMODULE hook = GetModuleHandleW(L"capture_hook_x64.dll");
    const auto install =
        reinterpret_cast<void* (*)(void*, void*, unsigned)>(GetProcAddress(hook, "CEFlow_InstallDeepHook"));
    const auto remove = reinterpret_cast<bool (*)(void*)>(GetProcAddress(hook, "CEFlow_RemoveDeepHook"));
    ASSERT_NE(install, nullptr);
    ASSERT_NE(remove, nullptr);

    auto* target = reinterpret_cast<unsigned char*>(&FlowDeepHookCookieFunction);
    const size_t prologEnd = CookieLoadEnd(target);
    ASSERT_GE(prologEnd, 7u) << "the probe's prolog must contain the cookie load";
    unsigned char saved[24] = {};
    std::memcpy(saved, target, prologEnd);

    // A foreign five-byte entry jump, NOP-filled to the end of the cookie load: CE resumes right after the load, so
    // the load is part of the prolog its trampoline replays.
    unsigned char patch[24];
    std::memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xE9;
    const auto rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(&ForeignEntryHandler) -
                                          reinterpret_cast<intptr_t>(target + 5));
    std::memcpy(patch + 1, &rel, sizeof(rel));
    ASSERT_TRUE(WriteCode(target, patch, prologEnd));

    auto* trampoline =
        static_cast<unsigned char*>(install(target, reinterpret_cast<void*>(&DeepHookWrapper), /*integerArguments=*/2));
    ASSERT_NE(trampoline, nullptr) << "the cookie prolog of a two-argument signature must be accepted";

    // Checked before executing it: the replayed load must address the cookie, not memory past the trampoline.
    const size_t replayedEnd = CookieLoadEnd(trampoline);
    ASSERT_EQ(replayedEnd, prologEnd);
    int32_t disp = 0;
    std::memcpy(&disp, trampoline + replayedEnd - 4, sizeof(disp));
    EXPECT_EQ(reinterpret_cast<uintptr_t>(trampoline + replayedEnd) + disp,
              reinterpret_cast<uintptr_t>(&g_flowDeepHookCookie));
    if (!HasFailure()) {
        const auto original = reinterpret_cast<uint64_t (*)(void*, void*)>(trampoline);
        EXPECT_EQ(original(nullptr, nullptr), g_flowDeepHookCookie) << "the native call through the trampoline";
    }

    EXPECT_TRUE(remove(target));
    ASSERT_TRUE(WriteCode(target, saved, prologEnd));
}

}  // namespace
