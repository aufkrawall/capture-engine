#include <gtest/gtest.h>

#include <windows.h>

#include <array>
#include <cstdint>

#include "hook/d3d11/d3d11_dispatch_forwarders.h"

// The forwarders of CE's D3D11 dispatch tables (hook/d3d11/d3d11_context_dispatch.h): the
// bytes of both architectures against llvm-mc's encodings, and x64 forwarders executed.
namespace {
using ce::d3d11_dispatch::EncodeForwarderX64;
using ce::d3d11_dispatch::EncodeForwarderX86;
using ce::d3d11_dispatch::kForwarderStride;

TEST(D3D11DispatchForwarders, X64BytesMatchTheAssembler) {
    // movabs rax, qword ptr [0x1122334455667788]; jmp qword ptr [rax + 0x7f8]; int3 padding
    const std::array<uint8_t, kForwarderStride> expected = {0x48, 0xA1, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33,
                                                            0x22, 0x11, 0xFF, 0xA0, 0xF8, 0x07, 0x00, 0x00};
    std::array<uint8_t, kForwarderStride> bytes = {};
    EncodeForwarderX64(bytes.data(), 0x1122334455667788ull, 255);
    EXPECT_EQ(bytes, expected);
    EncodeForwarderX64(bytes.data(), 0x1122334455667788ull, 0);
    EXPECT_EQ(bytes[12] | bytes[13] | bytes[14] | bytes[15], 0) << "slot 0: displacement 0";
}

TEST(D3D11DispatchForwarders, X86BytesMatchTheAssembler) {
    // mov eax, dword ptr [0x11223344]; jmp dword ptr [eax + 0x3fc]; int3 padding
    const std::array<uint8_t, kForwarderStride> expected = {0xA1, 0x44, 0x33, 0x22, 0x11, 0xFF, 0xA0, 0xFC,
                                                            0x03, 0x00, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
    std::array<uint8_t, kForwarderStride> bytes = {};
    EncodeForwarderX86(bytes.data(), 0x11223344u, 255);
    EXPECT_EQ(bytes, expected);
}

#ifdef _WIN64
struct FakeObject {
    void* const* vtable;
};

int STDMETHODCALLTYPE First(FakeObject* self, int a, int b) {
    return (self ? 1000 : 0) + a * 10 + b;
}
int STDMETHODCALLTYPE Second(FakeObject* self, int a, int b) {
    return (self ? 2000 : 0) + a * 10 + b;
}

// A forwarder jumps through the slot of whatever table its field names at the call, with the
// caller's arguments, and never looks at the object's vtable pointer.
TEST(D3D11DispatchForwarders, X64ForwardersFollowTheRuntimeField) {
    void* firstTable[8] = {};
    void* secondTable[8] = {};
    firstTable[5] = reinterpret_cast<void*>(&First);
    secondTable[5] = reinterpret_cast<void*>(&Second);
    void** runtimeField = firstTable;

    auto* code = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    ASSERT_NE(code, nullptr);
    ce::d3d11_dispatch::EncodeForwarder(code, static_cast<const void*>(&runtimeField), 5);
    DWORD old = 0;
    ASSERT_TRUE(VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &old));
    FlushInstructionCache(GetCurrentProcess(), code, kForwarderStride);
    using Fn = int(STDMETHODCALLTYPE*)(FakeObject*, int, int);
    const Fn forward = reinterpret_cast<Fn>(code);

    FakeObject object = {reinterpret_cast<void* const*>(uintptr_t{0x10})};  // not a readable table
    EXPECT_EQ(forward(&object, 4, 2), 1042);
    runtimeField = secondTable;
    EXPECT_EQ(forward(&object, 7, 3), 2073) << "the next call follows the field";
    EXPECT_EQ(forward(nullptr, 1, 1), 11);
    VirtualFree(code, 0, MEM_RELEASE);
}
#endif

}  // namespace
