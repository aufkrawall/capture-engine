#pragma once

// What CE must remember before redirecting a UE console variable's data
// pointer, and what it must write back to undo it.
//
// This is separated from the memory-touching code so the undo contract is a
// checkable invariant rather than an assumption: a redirect is only allowed
// when the pointer the game owned has been captured, because restoring writes
// exactly that value back into the live console object. Recording nothing and
// restoring a default-initialised pointer stores null into the object, and the
// engine dereferences that pointer on its next read.

#include <cstdint>

namespace ce::ue5_redirect {

// The state of a `{ref+0x50 data pointer, ref+0x58 local pair}` console
// variable as observed immediately before the redirect.
struct Observed {
    const void* dataPointer = nullptr;
    uint32_t pointedValue = 0;
    uint32_t localGame = 0;
    uint32_t localRender = 0;
    // Whether the storage the data pointer addresses can be written. Engine
    // code generated for `FAutoConsoleVariableRef` reads that storage directly
    // instead of going through the console object, so CE mirrors its value
    // there as well when it can.
    bool pointedStorageWritable = false;
};

struct Plan {
    bool valid = false;
    const void* restorePointer = nullptr;
    uint32_t restorePointedValue = 0;
    uint32_t restoreLocalGame = 0;
    uint32_t restoreLocalRender = 0;
    bool writeThrough = false;
};

constexpr Plan MakePlan(const Observed& observed) noexcept {
    Plan plan;
    if (!observed.dataPointer)
        return plan;
    plan.valid = true;
    plan.restorePointer = observed.dataPointer;
    plan.restorePointedValue = observed.pointedValue;
    plan.restoreLocalGame = observed.localGame;
    plan.restoreLocalRender = observed.localRender;
    plan.writeThrough = observed.pointedStorageWritable;
    return plan;
}

// A plan may only drive a redirect when undoing it is fully determined.
constexpr bool CanInstall(const Plan& plan) noexcept {
    return plan.valid && plan.restorePointer != nullptr;
}

}  // namespace ce::ue5_redirect
