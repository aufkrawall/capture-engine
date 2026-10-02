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

// The other redirect mode: repointing a `TAutoConsoleVariable` wrapper's `Ref`
// away from the game's `TConsoleVariableData<T>` {game, render} shadow pair.
//
// Repointing alone only reaches readers that go back through the wrapper. UE's
// renderer commonly resolves `IConsoleManager::FindTConsoleVariableDataInt` once
// into a static and reads the pair directly from then on; those readers never
// consult the wrapper again and keep seeing the game's value. Worse, they are
// invisible to verification, which reads CE's own shadow and therefore always
// agrees with CE's own configured value. So the pair is mirrored too, and both
// of its slots have to be recorded for the undo to be complete.
struct ObservedReference {
    const void* pair = nullptr;
    uint32_t gameBits = 0;
    uint32_t renderBits = 0;
    bool pairWritable = false;
};

struct ReferencePlan {
    bool valid = false;
    const void* restorePair = nullptr;
    uint32_t restoreGameBits = 0;
    uint32_t restoreRenderBits = 0;
    bool mirrorPair = false;
};

constexpr ReferencePlan MakeReferencePlan(const ObservedReference& observed) noexcept {
    ReferencePlan plan;
    if (!observed.pair)
        return plan;
    plan.valid = true;
    plan.restorePair = observed.pair;
    plan.restoreGameBits = observed.gameBits;
    plan.restoreRenderBits = observed.renderBits;
    plan.mirrorPair = observed.pairWritable;
    return plan;
}

// Mirroring is only allowed when both slots can be handed back afterwards.
constexpr bool CanMirror(const ReferencePlan& plan) noexcept {
    return plan.valid && plan.restorePair != nullptr && plan.mirrorPair;
}

}  // namespace ce::ue5_redirect
