#pragma once

// E9 rel32 displacement policy for CE's jump emission.
//
// The displacement is always computed in 64-bit signed arithmetic and verified
// to land on the target through the exact landing rule its consumer uses. This
// replaces the previous `int32_t rel = (int32_t)((uintptr_t)target - (uintptr_t)(dest + 5))`
// cast, which relied on unsigned wraparound landing correctly and whose
// round-trip "Verification" log could not detect a problem it did not act on.

#include <cstdint>

namespace ce::hook_jump_policy {

// How the consumer's CPU derives the jump target from the 5-byte E9's rel32.
enum class Rel32Semantics : uint8_t {
    // target = dest + 5 + sign_extend(rel32), computed in address-width
    // arithmetic with no wrap. This is the reach every 64-bit rel32 consumer
    // has (RIP-relative fixups, near relays, x64 short-branch rewrites) and the
    // only one a +/-2GB placement policy can guarantee.
    kSignExtended,
    // target = low32(dest + 5 + rel32). Native x86 E9 semantics: the EIP update
    // wraps modulo the 32-bit address space, so every in-space address is
    // reachable and an entry patch may legally jump from game code to a hook DLL
    // more than 2GB away. Refusing those displacements would fail x86 installs
    // that the ISA executes correctly, so they are emitted - explicitly and
    // round-trip-verified here - rather than refused or silently cast.
    kWrapAddress32,
};

// Displacement of a 5-byte E9 at `dest` landing on `target`, or false when no
// displacement lands there under `semantics` (only possible for kSignExtended,
// plus non-32-bit addresses for kWrapAddress32). A false result must fail the
// install; nothing may write a jump from it.
inline bool TryRel32Displacement(const void* dest, const void* target, Rel32Semantics semantics,
                                 int32_t* outDisplacement) {
    const int64_t displacement = static_cast<int64_t>(reinterpret_cast<uintptr_t>(target)) -
                                 (static_cast<int64_t>(reinterpret_cast<uintptr_t>(dest)) + 5);

    if (semantics == Rel32Semantics::kSignExtended) {
        if (displacement < INT32_MIN || displacement > INT32_MAX)
            return false;
        const int32_t displacement32 = static_cast<int32_t>(displacement);
        if (static_cast<int64_t>(displacement32) != displacement)
            return false;
        if (outDisplacement)
            *outDisplacement = displacement32;
        return true;
    }

    const uintptr_t destAddress = reinterpret_cast<uintptr_t>(dest);
    const uintptr_t targetAddress = reinterpret_cast<uintptr_t>(target);
    if (destAddress > 0xFFFFFFFFull || targetAddress > 0xFFFFFFFFull)
        return false;
    // C++20 conversions are modular: this is displacement (mod 2^32).
    const uint32_t displacement32 = static_cast<uint32_t>(static_cast<uint64_t>(displacement));
    const uint32_t landed = static_cast<uint32_t>(destAddress + 5) + displacement32;
    if (landed != static_cast<uint32_t>(targetAddress))
        return false;
    if (outDisplacement)
        *outDisplacement = static_cast<int32_t>(displacement32);
    return true;
}

}  // namespace ce::hook_jump_policy
