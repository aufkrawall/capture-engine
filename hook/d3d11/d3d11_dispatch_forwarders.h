#pragma once

// The forwarders of a CE dispatch table (d3d11_context_dispatch.h): machine code that jumps
// through the slot of whatever function table the runtime holds right now. Pure byte
// encoding, so the unit tests check both architectures (tests/test_d3d11_dispatch_forwarders.cpp).
//
// A forwarder reads the table's `runtime` field at an absolute address baked into it, never
// the object's vtable pointer: a tool that copies CE's table into a table of its own still
// reaches the runtime through it (one reading [this] would jump back into the copy forever).
//
//   x64: 48 A1 <imm64 &runtime>   mov rax, qword ptr [&runtime]
//        FF A0 <disp32 slot*8>    jmp qword ptr [rax + slot*8]
//   x86: A1 <imm32 &runtime>      mov eax, dword ptr [&runtime]
//        FF A0 <disp32 slot*4>    jmp dword ptr [eax + slot*4]
//
// rax/eax are scratch at a call boundary in both conventions; the arguments, `this` included,
// stay untouched. Encodings checked against llvm-mc (2026-10-17).

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ce::d3d11_dispatch {

// Bytes per forwarder: a multiple of 16, so each one is a valid Control Flow Guard target.
inline constexpr size_t kForwarderStride = 16;

// Writes one x64 forwarder (16 bytes, int3-padded) into `out`.
inline void EncodeForwarderX64(uint8_t* out, uint64_t runtimeFieldAddress, unsigned slot) {
    std::memset(out, 0xCC, kForwarderStride);
    out[0] = 0x48;
    out[1] = 0xA1;
    std::memcpy(out + 2, &runtimeFieldAddress, sizeof(runtimeFieldAddress));
    out[10] = 0xFF;
    out[11] = 0xA0;
    const uint32_t displacement = slot * 8u;
    std::memcpy(out + 12, &displacement, sizeof(displacement));
}

// Writes one x86 forwarder (16 bytes, int3-padded) into `out`.
inline void EncodeForwarderX86(uint8_t* out, uint32_t runtimeFieldAddress, unsigned slot) {
    std::memset(out, 0xCC, kForwarderStride);
    out[0] = 0xA1;
    std::memcpy(out + 1, &runtimeFieldAddress, sizeof(runtimeFieldAddress));
    out[5] = 0xFF;
    out[6] = 0xA0;
    const uint32_t displacement = slot * 4u;
    std::memcpy(out + 7, &displacement, sizeof(displacement));
}

// This process's architecture. `runtimeField` is the address of the pointer to the table.
inline void EncodeForwarder(uint8_t* out, const void* runtimeField, unsigned slot) {
#ifdef _WIN64
    EncodeForwarderX64(out, reinterpret_cast<uint64_t>(runtimeField), slot);
#else
    EncodeForwarderX86(out, reinterpret_cast<uint32_t>(runtimeField), slot);
#endif
}

}  // namespace ce::d3d11_dispatch
