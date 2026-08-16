// Installing, refreshing, verifying, and restoring the persistent UE5 CVar
// overrides. Split out of main_ue5_scan.cpp, which owns discovery only.
#include "main_ue5_internal.h"

namespace UE5::detail {

ForcedConsoleVariableData* GetOrCreateForcedData(std::size_t specIndex) {
  ForcedConsoleVariableData* existing = g_forcedData[specIndex].load(std::memory_order_acquire);
  if (existing)
    return existing;
  auto* allocated = static_cast<ForcedConsoleVariableData*>(
      VirtualAlloc(nullptr, sizeof(ForcedConsoleVariableData), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (!allocated)
    return nullptr;
  ForcedConsoleVariableData* expected = nullptr;
  if (!g_forcedData[specIndex].compare_exchange_strong(expected, allocated, std::memory_order_acq_rel)) {
    VirtualFree(allocated, 0, MEM_RELEASE);
    return expected;
  }
  // These tiny allocations are deliberately process-lifetime. A render thread
  // may have loaded Ref immediately before restoration; retaining the shadow
  // prevents a stale-read UAF when the injected hook shuts down.
  return allocated;
}

namespace {

// The dword next to a data-pointer value is not a second shadow: only the
// value itself is layout-validated, so it is logged as raw neighbouring
// memory rather than as a render-thread value.
void LogInstall(const ce::ue5_cvar::Spec& spec, const char* baseName, const char* installMode,
                const Candidate& candidate, bool installedAsDataShadow, bool writeThrough,
                std::size_t validatedCandidates, std::size_t discoveredCandidates, uintptr_t objectRva,
                ULONGLONG scanElapsedMs, uint32_t forcedBits) {
  const uint32_t previous =
      installedAsDataShadow ? candidate.dataShadowGameBits : candidate.gameThreadBits;
  const uint32_t neighbour =
      installedAsDataShadow ? candidate.dataShadowRenderBits : candidate.renderThreadBits;
  const char* neighbourLabel = installedAsDataShadow ? "neighborDword" : "prevRender";
  if (spec.type == ce::ue5_cvar::ValueType::Float) {
    HookLogImportant(
        "UE5 overrides: persistent %s=%.3f installed in %s (%s) "
        "(objectRva=0x%llX prevValue=%.3f %s=0x%08X writeThrough=%d score=%d scanMs=%llu candidates=%zu/%zu)",
        spec.name, std::bit_cast<float>(forcedBits), baseName, installMode,
        static_cast<unsigned long long>(objectRva), std::bit_cast<float>(previous), neighbourLabel,
        neighbour, writeThrough ? 1 : 0, candidate.score,
        static_cast<unsigned long long>(scanElapsedMs), validatedCandidates, discoveredCandidates);
    return;
  }
  HookLogImportant(
      "UE5 overrides: persistent %s=%d installed in %s (%s) "
      "(objectRva=0x%llX prevValue=%d %s=0x%08X writeThrough=%d score=%d scanMs=%llu candidates=%zu/%zu)",
      spec.name, static_cast<int32_t>(forcedBits), baseName, installMode,
      static_cast<unsigned long long>(objectRva), static_cast<int32_t>(previous), neighbourLabel,
      neighbour, writeThrough ? 1 : 0, candidate.score,
      static_cast<unsigned long long>(scanElapsedMs), validatedCandidates, discoveredCandidates);
}

// Engine code compiled for an `FAutoConsoleVariableRef` CVar reads the global
// the data pointer addresses without going through the console object, so the
// redirect alone would be invisible to it. Mirroring CE's value into that
// storage covers both read paths; the original is kept for restore.
bool WriteThroughOriginalStorage(OverrideState& state, uint32_t bits) {
  if (!state.originalReference || !IsWritableRange(state.originalReference, sizeof(uint32_t)))
    return false;
  InterlockedExchange(reinterpret_cast<volatile LONG*>(state.originalReference), static_cast<LONG>(bits));
  return true;
}

// Ref-redirect counterpart. `originalReference` is the `TConsoleVariableData<T>`
// the wrapper pointed at, i.e. the {game, render} shadow pair, so both slots are
// written. Repointing the wrapper alone leaves this pair holding the game's
// value, and every reader that cached the pair directly keeps reading it - the
// redirect is invisible to them, and so is CE's own verification, which reads
// CE's shadow rather than anything the engine consults.
bool WriteThroughReferencePair(OverrideState& state, uint32_t bits) {
  if (!state.referencePairWritten)
    return false;
  auto* pair = reinterpret_cast<volatile LONG*>(state.originalReference);
  InterlockedExchange(pair, static_cast<LONG>(bits));
  InterlockedExchange(pair + 1, static_cast<LONG>(bits));
  return true;
}

// Records the undo information first, then mirrors, so a mirrored pair can
// never exist without the values needed to put it back.
bool ApplyReferencePlan(OverrideState& state, const ce::ue5_redirect::ReferencePlan& plan, uint32_t bits) {
  if (!ce::ue5_redirect::CanMirror(plan))
    return false;
  state.originalReferenceGameBits = plan.restoreGameBits;
  state.originalReferenceRenderBits = plan.restoreRenderBits;
  state.referencePairWritten = true;
  return WriteThroughReferencePair(state, bits);
}

// Hands the game's own pair back. Ordering matters exactly as it does for the
// data-pointer path: the values go back before the pointer does, so no reader
// can observe the game's pointer addressing CE's value.
void RestoreReferencePair(OverrideState& state) {
  if (!state.referencePairWritten || !IsWritableRange(state.originalReference, sizeof(uint32_t) * 2))
    return;
  auto* pair = reinterpret_cast<volatile LONG*>(state.originalReference);
  InterlockedExchange(pair, static_cast<LONG>(state.originalReferenceGameBits));
  InterlockedExchange(pair + 1, static_cast<LONG>(state.originalReferenceRenderBits));
}

}  // namespace

// Commits the recorded undo information, then mirrors CE's value into the
// storage the data pointer addressed. Shared by both redirect install paths so
// neither can forget part of the contract.
void ApplyRestorePlan(OverrideState& state, const ce::ue5_redirect::Plan& plan, uint32_t bits) {
  state.originalReference = const_cast<void*>(plan.restorePointer);
  state.originalDataPointerBits = plan.restorePointedValue;
  state.originalDataShadowGameBits = plan.restoreLocalGame;
  state.originalDataShadowRenderBits = plan.restoreLocalRender;
  state.dataPointerValueWritten = plan.writeThrough && WriteThroughOriginalStorage(state, bits);
}

void UpdateForcedData(std::size_t specIndex, uint32_t bits) {
  ForcedConsoleVariableData* data = g_forcedData[specIndex].load(std::memory_order_acquire);
  if (!data)
    return;
  InterlockedExchange(&data->gameThreadBits, static_cast<LONG>(bits));
  InterlockedExchange(&data->renderThreadBits, static_cast<LONG>(bits));
  OverrideState& state = g_overrides[specIndex];
  if (state.dataShadowAddress) {
    if (state.dataShadowPointerRedirect) {
      // Game-thread reads use the local fallback pair; keep it mirrored.
      auto* localSlot = reinterpret_cast<volatile LONG*>(state.dataShadowAddress + sizeof(void*));
      InterlockedExchange(localSlot, static_cast<LONG>(bits));
      InterlockedExchange(localSlot + 1, static_cast<LONG>(bits));
    } else {
      auto* gameSlot = reinterpret_cast<volatile LONG*>(state.dataShadowAddress);
      InterlockedExchange(gameSlot, static_cast<LONG>(bits));
      InterlockedExchange(gameSlot + 1, static_cast<LONG>(bits));
    }
  }
  if (state.dataPointerValueWritten)
    WriteThroughOriginalStorage(state, bits);
  if (state.referencePairWritten)
    WriteThroughReferencePair(state, bits);
}

bool ApplyCandidate(const ModuleView& image, const Candidate& candidate, std::size_t discoveredCandidates,
                    std::size_t validatedCandidates, ULONGLONG scanElapsedMs) {
  const std::size_t specIndex = candidate.specIndex;
  ForcedConsoleVariableData* forcedData = GetOrCreateForcedData(specIndex);
  if (!forcedData) {
    HookLogImportant("UE5 overrides: unable to allocate process-lifetime storage for %s (error=%lu)",
                     ce::ue5_cvar::kSpecs[specIndex].name, GetLastError());
    return false;
  }
  char modulePath[MAX_PATH];
  const char* baseName = ModuleBaseName(image.module, modulePath);
  const ce::ue5_cvar::Spec& spec = ce::ue5_cvar::kSpecs[specIndex];
  // Some overrides are only safe in a particular engine state, and the value the
  // game currently holds is the evidence. Checked before anything is written, so
  // a refusal leaves game memory untouched rather than being undone after.
  const uint32_t observedBits = candidate.originalReference && candidate.evidence.shadowValuesPlausible
                                    ? candidate.gameThreadBits
                                    : candidate.dataShadowGameBits;
  if (!ce::ue5_cvar::MayApplyOverObservedValue(spec, observedBits)) {
    HookLogImportant(
        "UE5 overrides: %s not applied in %s - the game's current value (%d) puts it outside the "
        "state this override is safe in; leaving game memory unchanged",
        spec.name, baseName, static_cast<int32_t>(observedBits));
    return false;
  }
  bool writeThrough = false;
  if (candidate.originalReference && candidate.evidence.shadowValuesPlausible) {
    auto* referenceField = reinterpret_cast<void* volatile*>(candidate.object + sizeof(void*) * 2);
    void* observed = InterlockedCompareExchangePointer(referenceField, forcedData, candidate.originalReference);
    if (observed != candidate.originalReference) {
      HookLogImportant(
          "UE5 overrides: refused changed TAutoConsoleVariable reference for %s in %s "
          "(object=%p expected=%p observed=%p)",
          spec.name, baseName, reinterpret_cast<void*>(candidate.object), candidate.originalReference,
          observed);
      return false;
    }
    g_overrides[specIndex] = {};
    g_overrides[specIndex].module = image.module;
    g_overrides[specIndex].referenceField = referenceField;
    g_overrides[specIndex].originalReference = candidate.originalReference;
    g_overrides[specIndex].object = candidate.object;
    // `shadowValuesPlausible` is a hard gate on this path, so the {game, render}
    // pair behind the old Ref is layout-proven - mirroring into it is exactly as
    // well-founded as the redirect itself, and it is what makes the override
    // visible to readers that cached the pair instead of the wrapper.
    ce::ue5_redirect::ObservedReference observedPair{};
    observedPair.pair = candidate.originalReference;
    observedPair.gameBits = candidate.gameThreadBits;
    observedPair.renderBits = candidate.renderThreadBits;
    observedPair.pairWritable = IsWritableRange(candidate.originalReference, sizeof(uint32_t) * 2);
    writeThrough = ApplyReferencePlan(g_overrides[specIndex],
                                      ce::ue5_redirect::MakeReferencePlan(observedPair),
                                      g_desired[specIndex].bits);
  } else if (candidate.dataShadowUsable) {
    if (!IsWritableRange(reinterpret_cast<void*>(candidate.dataShadowAddress),
                         sizeof(uint32_t) * (candidate.dataShadowPointerRedirect ? 4 : 2))) {
      HookLogImportant(
          "UE5 overrides: %s data-shadow storage not writable in %s; leaving game memory unchanged",
          spec.name, baseName);
      return false;
    }
    g_overrides[specIndex] = {};
    g_overrides[specIndex].module = image.module;
    g_overrides[specIndex].object = candidate.object;
    g_overrides[specIndex].dataShadowAddress = candidate.dataShadowAddress;
    g_overrides[specIndex].originalDataShadowGameBits = candidate.dataShadowGameBits;
    g_overrides[specIndex].originalDataShadowRenderBits = candidate.dataShadowRenderBits;
    g_overrides[specIndex].dataShadowPointerRedirect = candidate.dataShadowPointerRedirect;
    if (candidate.dataShadowPointerRedirect) {
      ce::ue5_redirect::Observed observed{};
      observed.dataPointer = candidate.originalReference;
      observed.pointedValue = candidate.dataShadowGameBits;
      ReadValue(reinterpret_cast<const void*>(candidate.dataShadowAddress + sizeof(void*)),
                observed.localGame);
      ReadValue(
          reinterpret_cast<const void*>(candidate.dataShadowAddress + sizeof(void*) + sizeof(uint32_t)),
          observed.localRender);
      observed.pointedStorageWritable =
          IsWritableRange(candidate.originalReference, sizeof(uint32_t));
      // Refusing an unrecordable redirect is what keeps restore from writing a
      // default-initialised (null) pointer back into the live console object.
      const ce::ue5_redirect::Plan plan = ce::ue5_redirect::MakePlan(observed);
      if (!ce::ue5_redirect::CanInstall(plan)) {
        HookLogImportant("UE5 overrides: %s data pointer cannot be recorded for restore; "
                         "leaving game memory unchanged",
                         spec.name);
        g_overrides[specIndex] = {};
        return false;
      }
      auto* pointerSlot = reinterpret_cast<void* volatile*>(candidate.dataShadowAddress);
      void* previous =
          InterlockedCompareExchangePointer(pointerSlot, forcedData, candidate.originalReference);
      if (previous != candidate.originalReference) {
        HookLogImportant("UE5 overrides: refused changed %s data pointer (expected=%p observed=%p)",
                         spec.name, candidate.originalReference, previous);
        g_overrides[specIndex] = {};
        return false;
      }
      ApplyRestorePlan(g_overrides[specIndex], plan, g_desired[specIndex].bits);
      writeThrough = g_overrides[specIndex].dataPointerValueWritten;
    }
  } else {
    HookLogImportant("UE5 overrides: %s candidate scored without a usable override path in %s",
                     spec.name, baseName);
    return false;
  }
  const bool installedAsDataShadow =
      !(candidate.originalReference && candidate.evidence.shadowValuesPlausible);
  const char* installMode = installedAsDataShadow ? "data-pointer redirect" : "Ref redirect";
  UpdateForcedData(specIndex, g_desired[specIndex].bits);
  g_activeModules[specIndex].store(image.module, std::memory_order_release);
  g_activeModuleUnloaded[specIndex].store(false, std::memory_order_release);
  LogInstall(spec, baseName, installMode, candidate, installedAsDataShadow, writeThrough,
             validatedCandidates, discoveredCandidates,
             candidate.object > image.base ? candidate.object - image.base : 0, scanElapsedMs,
             g_desired[specIndex].bits);
  return true;
}

VerificationCounts VerifyOverrides() {
  VerificationCounts counts;
  for (std::size_t index = 0; index < kCVarCount; ++index) {
    OverrideState& state = g_overrides[index];
    if (!g_desired[index].enabled || !IsOverrideInstalled(state))
      continue;
    if (!g_activeModules[index].load(std::memory_order_acquire))
      continue;
    ++counts.checked;
    const ForcedConsoleVariableData* forced = g_forcedData[index].load(std::memory_order_acquire);
    const uint32_t expected = g_desired[index].bits;
    const char* failure = nullptr;
    bool lostRedirect = false;

    if (state.referenceField || state.dataShadowPointerRedirect) {
      const void* slot = state.dataShadowAddress
                             ? reinterpret_cast<const void*>(state.dataShadowAddress)
                             : static_cast<const void*>(const_cast<void**>(state.referenceField));
      void* observed = nullptr;
      if (!ReadValue(slot, observed))
        failure = "redirect slot unreadable";
      else if (observed != forced)
        lostRedirect = true;
    }

    uint32_t gameBits = 0;
    uint32_t renderBits = 0;
    if (!failure && !lostRedirect) {
      if (state.dataShadowAddress && !state.dataShadowPointerRedirect) {
        if (!ReadValue(reinterpret_cast<const void*>(state.dataShadowAddress), gameBits) ||
            !ReadValue(reinterpret_cast<const void*>(state.dataShadowAddress + sizeof(uint32_t)),
                       renderBits)) {
          failure = "value pair unreadable";
        }
      } else if (!forced) {
        failure = "shadow missing";
      } else {
        // CE's own process-lifetime storage: always mapped, so a plain
        // volatile load is both sufficient and cheaper than a range probe.
        gameBits = static_cast<uint32_t>(forced->gameThreadBits);
        renderBits = static_cast<uint32_t>(forced->renderThreadBits);
      }
    }

    uint32_t throughBits = expected;
    if (!failure && !lostRedirect && state.dataPointerValueWritten &&
        !ReadValue(state.originalReference, throughBits)) {
      failure = "write-through storage unreadable";
    }
    // Without this the Ref-redirect mode verifies CE's own shadow against CE's
    // own configured value and always agrees, whatever the engine is reading.
    if (!failure && !lostRedirect && state.referencePairWritten) {
      uint32_t pairGame = expected;
      uint32_t pairRender = expected;
      const auto* pair = static_cast<const uint8_t*>(state.originalReference);
      if (!ReadValue(pair, pairGame) || !ReadValue(pair + sizeof(uint32_t), pairRender))
        failure = "reference value pair unreadable";
      else
        throughBits = pairGame != expected ? pairGame : pairRender;
    }

    if (lostRedirect) {
      // The game re-registered or replaced the variable: retire the stale
      // record and let the next scan reinstall against the new object.
      if (state.driftReports < 3) {
        ++state.driftReports;
        HookLogImportant(
            "UE5 overrides: %s redirect no longer owned by CE (the game replaced the pointer); "
            "retiring the record and rescanning",
            ce::ue5_cvar::kSpecs[index].name);
      }
      ++counts.lost;
      // The pointer is the game's again, but the storage it used to address
      // still holds CE's mirrored value; hand that back before forgetting it.
      if (state.dataPointerValueWritten && IsWritableRange(state.originalReference, sizeof(uint32_t))) {
        InterlockedExchange(reinterpret_cast<volatile LONG*>(state.originalReference),
                            static_cast<LONG>(state.originalDataPointerBits));
      }
      RestoreReferencePair(state);
      state = {};
      g_activeModules[index].store(nullptr, std::memory_order_release);
      g_fullRescanRequested.store(true, std::memory_order_release);
      continue;
    }
    if (failure) {
      if (state.driftReports < 3) {
        ++state.driftReports;
        HookLogImportant("UE5 overrides: %s verification could not read back its value (%s)",
                         ce::ue5_cvar::kSpecs[index].name, failure);
      }
      state.cleanVerifications = 0;
      continue;
    }

    const bool matches = gameBits == expected && renderBits == expected && throughBits == expected;
    if (!matches) {
      // A game-side Set() reached the storage CE owns. Re-assert rather than
      // reinstall: the redirect itself is still ours.
      if (state.driftReports < 3) {
        ++state.driftReports;
        HookLogImportant(
            "UE5 overrides: %s drifted (game=0x%08X render=0x%08X through=0x%08X expected=0x%08X); "
            "re-asserting the configured value",
            ce::ue5_cvar::kSpecs[index].name, gameBits, renderBits, throughBits, expected);
      }
      // A one-off drift after level load is ordinary; the same CVar drifting
      // over and over is the game continuously winning the storage back, which
      // means the setting is not actually holding. That has to stay visible
      // after the drift cap without turning into a per-second log.
      ++state.reassertCount;
      if (state.reassertCount == 10 || state.reassertCount == 100 || state.reassertCount == 1000 ||
          (state.reassertCount % 5000) == 0) {
        HookLogImportant(
            "UE5 overrides: %s re-asserted %u time(s) — the game keeps rewriting this value, so it is "
            "contested rather than held (last observed 0x%08X, expected 0x%08X)",
            ce::ue5_cvar::kSpecs[index].name, state.reassertCount, throughBits, expected);
      }
      UpdateForcedData(index, expected);
      ++counts.reasserted;
      state.cleanVerifications = 0;
      continue;
    }

    ++counts.verified;
    if (++state.cleanVerifications >= kDriftReportResetPasses) {
      state.cleanVerifications = 0;
      state.driftReports = 0;
    }
  }
  return counts;
}

void ForgetUnloadedOverrides() {
  for (std::size_t index = 0; index < kCVarCount; ++index) {
    if (!g_activeModuleUnloaded[index].exchange(false, std::memory_order_acq_rel))
      continue;
    if (IsOverrideInstalled(g_overrides[index])) {
      HookLogImportant("UE5 overrides: module owning %s unloaded; retired stale override and awaiting reload",
                       ce::ue5_cvar::kSpecs[index].name);
      g_overrides[index] = {};
    }
    if (g_desired[index].enabled)
      g_fullRescanRequested.store(true, std::memory_order_release);
  }
}

void RestoreOverride(std::size_t specIndex, const char* reason) {
  OverrideState& state = g_overrides[specIndex];
  if (state.bitReference) {
    // Only CE's own bit goes back. Writing the whole byte would undo whatever
    // the game did to the other show flags sharing it.
    UpdateForceMaskBit(state.forceZeroByte, state.bitMask, state.originalForceZeroBit);
    UpdateForceMaskBit(state.forceOneByte, state.bitMask, state.originalForceOneBit);
    HookLogImportant("UE5 overrides: restored the game's %s show flag force bit (%s)",
                     ce::ue5_cvar::kSpecs[specIndex].name, reason);
    state = {};
    return;
  }
  if (!IsOverrideInstalled(state))
    return;
  if (state.dataShadowAddress) {
    if (state.dataShadowPointerRedirect) {
      // Put the game's value back before the pointer, so no read observes the
      // original storage still holding CE's value through the game's pointer.
      if (state.dataPointerValueWritten &&
          IsWritableRange(state.originalReference, sizeof(uint32_t))) {
        InterlockedExchange(reinterpret_cast<volatile LONG*>(state.originalReference),
                            static_cast<LONG>(state.originalDataPointerBits));
      }
      ForcedConsoleVariableData* forcedData = g_forcedData[specIndex].load(std::memory_order_acquire);
      void* observed = InterlockedCompareExchangePointer(
          reinterpret_cast<void* volatile*>(state.dataShadowAddress), state.originalReference, forcedData);
      if (observed == forcedData) {
        HookLogImportant("UE5 overrides: restored the game's %s data pointer (%s)",
                         ce::ue5_cvar::kSpecs[specIndex].name, reason);
      } else {
        HookLogImportant("UE5 overrides: %s data pointer changed before restore (%s, observed=%p); "
                         "left newer owner untouched",
                         ce::ue5_cvar::kSpecs[specIndex].name, reason, observed);
      }
      auto* localSlot = reinterpret_cast<volatile LONG*>(state.dataShadowAddress + sizeof(void*));
      InterlockedExchange(localSlot, static_cast<LONG>(state.originalDataShadowGameBits));
      InterlockedExchange(localSlot + 1, static_cast<LONG>(state.originalDataShadowRenderBits));
      HookLogImportant("UE5 overrides: restored the game's %s local value pair (%s)",
                       ce::ue5_cvar::kSpecs[specIndex].name, reason);
    } else {
      auto* gameSlot = reinterpret_cast<volatile LONG*>(state.dataShadowAddress);
      InterlockedExchange(gameSlot, static_cast<LONG>(state.originalDataShadowGameBits));
      InterlockedExchange(gameSlot + 1, static_cast<LONG>(state.originalDataShadowRenderBits));
      HookLogImportant("UE5 overrides: restored the game's %s data-shadow pair (%s)",
                       ce::ue5_cvar::kSpecs[specIndex].name, reason);
    }
  }
  if (!state.referenceField) {
    g_activeModules[specIndex].store(nullptr, std::memory_order_release);
    g_activeModuleUnloaded[specIndex].store(false, std::memory_order_release);
    state = {};
    return;
  }
  HMODULE activeModule = g_activeModules[specIndex].load(std::memory_order_acquire);
  if (!activeModule) {
    state = {};
    g_activeModuleUnloaded[specIndex].store(false, std::memory_order_release);
    return;
  }
  ScopedModuleReference reference(activeModule);
  const void* referenceFieldAddress = static_cast<const void*>(const_cast<void**>(state.referenceField));
  if (!reference.Get() || !IsWritableRange(referenceFieldAddress, sizeof(void*))) {
    g_activeModules[specIndex].store(nullptr, std::memory_order_release);
    state = {};
    return;
  }
  // Values first, then the pointer: the same ordering the data-pointer path
  // uses, so no reader observes the game's Ref addressing CE's value.
  RestoreReferencePair(state);
  ForcedConsoleVariableData* forcedData = g_forcedData[specIndex].load(std::memory_order_acquire);
  void* observed = InterlockedCompareExchangePointer(state.referenceField, state.originalReference, forcedData);
  if (observed == forcedData) {
    HookLogImportant("UE5 overrides: restored the game's %s reference and value pair (%s)",
                     ce::ue5_cvar::kSpecs[specIndex].name, reason);
  } else {
    HookLogImportant("UE5 overrides: %s reference changed before restore (%s, observed=%p); "
                     "left newer owner untouched",
                     ce::ue5_cvar::kSpecs[specIndex].name, reason, observed);
  }
  g_activeModules[specIndex].store(nullptr, std::memory_order_release);
  state = {};
}

void RestoreAllOverrides(const char* reason) {
  for (std::size_t index = 0; index < kCVarCount; ++index)
    RestoreOverride(index, reason);
}

}  // namespace UE5::detail
