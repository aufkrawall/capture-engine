// Probing a UE console object for the layout it actually has, and driving it
// through that layout. Used by the console-registry resolver, where no static
// `FAutoConsoleVariable` wrapper exists to identify the object.
//
// Before this unit existed, the registry path assumed one shape - a data
// pointer at `object+0x50` - and wrote through it whenever the pointed dword
// happened to be a plausible value. UE's `ShowFlag.*` variables are
// `FConsoleVariableBitRef`, whose first two qwords are the two force masks
// every show flag shares, so the assumption held for exactly as long as a mask
// read zero. All four ShowFlag overrides in the 20260816_161158 Talos session
// installed that way and none of them did anything: the redirect replaced the
// engine's mask pointer with CE's eight-byte shadow, which the engine's
// `GetInt()` then read as an all-clear mask.
#include "main_ue5_internal.h"

namespace UE5::detail {
namespace {

// A bit reference is only committed once a second show flag reports the same
// mask pair. UE registers every show flag against the same two masks, so
// agreement across two independently resolved objects is proof of the shape;
// one object alone is just two adjacent pointers.
constexpr std::size_t kBitReferenceConfirmations = 2;

struct BitReferenceCandidate {
  bool present = false;
  bool reported = false;
  uintptr_t object = 0;
  uintptr_t forceZeroMask = 0;
  uintptr_t forceOneMask = 0;
  uint32_t bitNumber = 0;
};

std::array<BitReferenceCandidate, kCVarCount> g_bitReferenceCandidates{};
std::atomic<uint32_t> g_layoutDumpLogs{0};

HMODULE OwningModule(uintptr_t address) {
  HMODULE module = nullptr;
  if (!address ||
      !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(address), &module)) {
    return nullptr;
  }
  return module;
}

ce::ue5_layout::ObjectProbe ReadProbe(uintptr_t consoleObject, std::size_t offset) {
  ce::ue5_layout::ObjectProbe probe;
  probe.offset = offset;
  ReadValue(reinterpret_cast<const void*>(consoleObject + offset), probe.firstQword);
  ReadValue(reinterpret_cast<const void*>(consoleObject + offset + sizeof(uint64_t)), probe.secondQword);
  probe.bitNumberRead = ReadValue(
      reinterpret_cast<const void*>(consoleObject + offset + sizeof(uint64_t) * 2), probe.bitNumber);

  const auto first = static_cast<uintptr_t>(probe.firstQword);
  const auto second = static_cast<uintptr_t>(probe.secondQword);
  probe.firstTargetWritable = IsWritableRange(reinterpret_cast<const void*>(first), sizeof(uint32_t));
  probe.secondTargetWritable = IsWritableRange(reinterpret_cast<const void*>(second), sizeof(uint32_t));
  if (probe.firstTargetWritable)
    probe.pointedValueRead = ReadValue(reinterpret_cast<const void*>(first), probe.pointedValue);
  if (probe.firstTargetWritable && probe.secondTargetWritable) {
    HMODULE firstModule = OwningModule(first);
    probe.pointersShareModuleData = firstModule != nullptr && firstModule == OwningModule(second);
  }
  return probe;
}

// Logged when nothing matched, because the bytes are the only thing that can
// explain a per-title layout CE has not seen. Rate-limited: the resolver retries
// once a second, and an unrecognised object stays unrecognised.
void LogUnrecognisedObject(const char* name, uintptr_t consoleObject, const char* origin,
                           bool ambiguous) {
  if (g_layoutDumpLogs.fetch_add(1, std::memory_order_relaxed) >= 6)
    return;
  uintptr_t words[16] = {};
  for (std::size_t index = 0; index < std::size(words); ++index)
    ReadValue(reinterpret_cast<const void*>(consoleObject + index * sizeof(uintptr_t)), words[index]);
  HookLogImportant(
      "UE5 overrides: %s found via %s at %p carries %s console-variable layout; leaving game memory "
      "unchanged. obj[0x00..0x78]=0x%llX 0x%llX 0x%llX 0x%llX 0x%llX 0x%llX 0x%llX 0x%llX 0x%llX "
      "0x%llX 0x%llX 0x%llX 0x%llX 0x%llX 0x%llX 0x%llX",
      name, origin, reinterpret_cast<void*>(consoleObject),
      ambiguous ? "an ambiguous" : "no recognised", static_cast<unsigned long long>(words[0]),
      static_cast<unsigned long long>(words[1]), static_cast<unsigned long long>(words[2]),
      static_cast<unsigned long long>(words[3]), static_cast<unsigned long long>(words[4]),
      static_cast<unsigned long long>(words[5]), static_cast<unsigned long long>(words[6]),
      static_cast<unsigned long long>(words[7]), static_cast<unsigned long long>(words[8]),
      static_cast<unsigned long long>(words[9]), static_cast<unsigned long long>(words[10]),
      static_cast<unsigned long long>(words[11]), static_cast<unsigned long long>(words[12]),
      static_cast<unsigned long long>(words[13]), static_cast<unsigned long long>(words[14]),
      static_cast<unsigned long long>(words[15]));
}

void LogConsoleObjectInstall(std::size_t specIndex, const char* mode, const char* origin,
                             uintptr_t consoleObject, std::size_t valueOffset, uint32_t previousBits,
                             bool writeThrough) {
  const ce::ue5_cvar::Spec& spec = ce::ue5_cvar::kSpecs[specIndex];
  const uint32_t bits = g_desired[specIndex].bits;
  if (spec.type == ce::ue5_cvar::ValueType::Float) {
    HookLogImportant("UE5 overrides: persistent %s=%.3f installed via %s (%s) "
                     "(object=%p valueOffset=0x%zX prevValue=%.3f writeThrough=%d)",
                     spec.name, std::bit_cast<float>(bits), origin, mode,
                     reinterpret_cast<void*>(consoleObject), valueOffset,
                     std::bit_cast<float>(previousBits), writeThrough ? 1 : 0);
    return;
  }
  HookLogImportant("UE5 overrides: persistent %s=%d installed via %s (%s) "
                   "(object=%p valueOffset=0x%zX prevValue=%d writeThrough=%d)",
                   spec.name, static_cast<int32_t>(bits), origin, mode,
                   reinterpret_cast<void*>(consoleObject), valueOffset,
                   static_cast<int32_t>(previousBits), writeThrough ? 1 : 0);
}

bool InstallReferencePointer(std::size_t specIndex, HMODULE owner, uintptr_t consoleObject,
                             std::size_t valueOffset, const char* origin) {
  const ce::ue5_cvar::Spec& spec = ce::ue5_cvar::kSpecs[specIndex];
  const uintptr_t pointerSlot = consoleObject + valueOffset;
  uintptr_t dataPointer = 0;
  uint32_t value = 0;
  if (!ReadValue(reinterpret_cast<const void*>(pointerSlot), dataPointer) ||
      !ReadValue(reinterpret_cast<const void*>(dataPointer), value)) {
    return false;
  }
  if (!IsWritableRange(reinterpret_cast<void*>(pointerSlot), sizeof(uint32_t) * 4)) {
    HookLogImportant("UE5 overrides: %s value storage found via %s is not writable; leaving it unchanged",
                     spec.name, origin);
    return false;
  }
  ForcedConsoleVariableData* forcedData = GetOrCreateForcedData(specIndex);
  if (!forcedData) {
    HookLogImportant("UE5 overrides: unable to allocate process-lifetime storage for %s (error=%lu)",
                     spec.name, GetLastError());
    return false;
  }
  ce::ue5_redirect::Observed observed{};
  observed.dataPointer = reinterpret_cast<const void*>(dataPointer);
  observed.pointedValue = value;
  ReadValue(reinterpret_cast<const void*>(pointerSlot + sizeof(void*)), observed.localGame);
  ReadValue(reinterpret_cast<const void*>(pointerSlot + sizeof(void*) + sizeof(uint32_t)),
            observed.localRender);
  observed.pointedStorageWritable = IsWritableRange(reinterpret_cast<void*>(dataPointer), sizeof(uint32_t));
  const ce::ue5_redirect::Plan plan = ce::ue5_redirect::MakePlan(observed);
  if (!ce::ue5_redirect::CanInstall(plan)) {
    HookLogImportant("UE5 overrides: %s data pointer from %s cannot be recorded for restore; "
                     "leaving game memory unchanged",
                     spec.name, origin);
    return false;
  }

  void* previous = InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(pointerSlot),
                                                     forcedData, reinterpret_cast<void*>(dataPointer));
  if (previous != reinterpret_cast<void*>(dataPointer)) {
    HookLogImportant("UE5 overrides: refused changed %s data pointer from %s (expected=%p observed=%p)",
                     spec.name, origin, reinterpret_cast<void*>(dataPointer), previous);
    return false;
  }

  OverrideState& state = g_overrides[specIndex];
  state = {};
  state.module = owner;
  state.object = consoleObject;
  state.dataShadowAddress = pointerSlot;
  state.dataShadowPointerRedirect = true;
  state.registryResolved = true;
  ApplyRestorePlan(state, plan, g_desired[specIndex].bits);
  UpdateForcedData(specIndex, g_desired[specIndex].bits);
  g_activeModules[specIndex].store(owner, std::memory_order_release);
  g_activeModuleUnloaded[specIndex].store(false, std::memory_order_release);
  LogConsoleObjectInstall(specIndex, "reference redirect", origin, consoleObject, valueOffset, value,
                          state.dataPointerValueWritten);
  return true;
}

bool InstallInlinePair(std::size_t specIndex, HMODULE owner, uintptr_t consoleObject,
                       std::size_t valueOffset, const char* origin) {
  const ce::ue5_cvar::Spec& spec = ce::ue5_cvar::kSpecs[specIndex];
  const uintptr_t pairAddress = consoleObject + valueOffset;
  if (!IsWritableRange(reinterpret_cast<void*>(pairAddress), sizeof(uint32_t) * 2)) {
    HookLogImportant("UE5 overrides: %s inline value pair found via %s is not writable; leaving it unchanged",
                     spec.name, origin);
    return false;
  }
  uint32_t game = 0;
  uint32_t render = 0;
  if (!ReadValue(reinterpret_cast<const void*>(pairAddress), game) ||
      !ReadValue(reinterpret_cast<const void*>(pairAddress + sizeof(uint32_t)), render)) {
    return false;
  }
  if (!GetOrCreateForcedData(specIndex)) {
    HookLogImportant("UE5 overrides: unable to allocate process-lifetime storage for %s (error=%lu)",
                     spec.name, GetLastError());
    return false;
  }
  OverrideState& state = g_overrides[specIndex];
  state = {};
  state.module = owner;
  state.object = consoleObject;
  state.dataShadowAddress = pairAddress;
  state.dataShadowPointerRedirect = false;
  state.originalDataShadowGameBits = game;
  state.originalDataShadowRenderBits = render;
  state.registryResolved = true;
  UpdateForcedData(specIndex, g_desired[specIndex].bits);
  g_activeModules[specIndex].store(owner, std::memory_order_release);
  g_activeModuleUnloaded[specIndex].store(false, std::memory_order_release);
  LogConsoleObjectInstall(specIndex, "inline pair write", origin, consoleObject, valueOffset, game, true);
  return true;
}

// Reports a confirmed bit reference without touching the masks.
//
// CE drove these bits for exactly one build (0.1.6128) and the result settles
// the question the previous version could not answer: the masks *are* live in a
// Shipping build. Forcing the four post-processing flags off removed all
// lighting from Talos (session 20260816_165501), which the reported numbers
// explain: `ShowFlag.Vignette` is bit 13 and `ShowFlag.Grain` bit 14, and the
// engine's own name table places `GlobalIllumination` immediately before them
// at bit 12. Any off-by-one between the index the console object carries and
// the index the renderer reads the mask by lands on global illumination.
//
// The bit numbers are self-consistent (distinct, ordered, matching the table),
// so the defect is in the mapping between them and the runtime mask - plausibly
// the `SHOWFLAG_FIXED_IN_SHIPPING` flags being compiled out of one side and not
// the other. Until a run proves that mapping, writing a bit means guessing
// which flag is being turned off, and the guess already cost a broken frame.
// The classification stays: it is what stops the old redirect from replacing
// the engine's mask pointer, which is the defect that made these overrides
// inert in the first place.
// Exactly one flag is driven, and it is the only one with no alternative: every
// other show flag CE targets is already covered by a value CVar
// (`r.FilmGrain`, `r.MotionBlurQuality`, `r.SceneColorFringeQuality`), while UE
// ships no vignette CVar at all - the whole binary contains only the show flag's
// own name and its localization key.
//
// One bit rather than four because the four-at-once write in 0.1.6128 cost a
// broken frame and taught nothing about which bit did it. The engine reads these
// masks through the console object's own pointer, so no static reference to them
// exists to disassemble; the convention has to be measured. Vignette is bit 13
// and `GlobalIllumination` is bit 12, so a uniform off-by-one is visible as
// exactly the 0.1.6128 symptom, and any other outcome is equally diagnostic.
constexpr const char* kDrivenBitReference = "ShowFlag.Vignette";

bool DriveBitReference(std::size_t specIndex, const BitReferenceCandidate& candidate,
                       const char* origin) {
  const uint32_t desired = g_desired[specIndex].bits;
  if (desired != 0)
    return false;  // Only "force off" is expressible without knowing the roles.
  const std::size_t byteIndex = ce::ue5_layout::BitByteIndex(candidate.bitNumber);
  const uint8_t mask = ce::ue5_layout::BitMask(candidate.bitNumber);
  const uintptr_t forceZeroByte = candidate.forceZeroMask + byteIndex;
  const uintptr_t forceOneByte = candidate.forceOneMask + byteIndex;
  bool zeroBit = false;
  bool oneBit = false;
  if (!ReadForceMaskBit(forceZeroByte, mask, zeroBit) || !ReadForceMaskBit(forceOneByte, mask, oneBit))
    return false;

  OverrideState& state = g_overrides[specIndex];
  state = {};
  state.object = candidate.object;
  state.bitReference = true;
  state.forceZeroByte = forceZeroByte;
  state.forceOneByte = forceOneByte;
  state.bitMask = mask;
  state.originalForceZeroBit = zeroBit;
  state.originalForceOneBit = oneBit;
  UpdateForceMaskBit(forceOneByte, mask, false);
  UpdateForceMaskBit(forceZeroByte, mask, true);
  HookLogImportant(
      "UE5 overrides: %s force bit SET via %s (object=%p force0=%p force1=%p bit=%u byte=%zu "
      "mask=0x%02X prevForce0=%d prevForce1=%d). This is the single-flag measurement of the mask "
      "convention: if vignette disappears the mapping is right, if global illumination disappears "
      "the index is off by one, if nothing changes the masks are inert in this build",
      ce::ue5_cvar::kSpecs[specIndex].name, origin, reinterpret_cast<void*>(candidate.object),
      reinterpret_cast<void*>(candidate.forceZeroMask), reinterpret_cast<void*>(candidate.forceOneMask),
      candidate.bitNumber, byteIndex, mask, zeroBit ? 1 : 0, oneBit ? 1 : 0);
  return true;
}

void ReportBitReference(std::size_t specIndex, const BitReferenceCandidate& candidate,
                        const char* origin) {
  HookLogImportant(
      "UE5 overrides: %s found via %s is a show flag force bit "
      "(object=%p force0=%p force1=%p bit=%u byte=%zu mask=0x%02X); CE does not drive it - the bit "
      "index the console object carries is not proven to be the index the renderer reads the mask "
      "by, and forcing it removed global illumination in 0.1.6128. Leaving game memory unchanged",
      ce::ue5_cvar::kSpecs[specIndex].name, origin, reinterpret_cast<void*>(candidate.object),
      reinterpret_cast<void*>(candidate.forceZeroMask), reinterpret_cast<void*>(candidate.forceOneMask),
      candidate.bitNumber, ce::ue5_layout::BitByteIndex(candidate.bitNumber),
      ce::ue5_layout::BitMask(candidate.bitNumber));
}

}  // namespace

void UpdateForceMaskBit(uintptr_t byteAddress, uint8_t mask, bool set) {
  // The masks are byte arrays, so CE's bit shares its 32-bit word with other
  // show flags. Compare-exchange on the containing word is what keeps a
  // concurrent engine-side update to a neighbouring flag from being lost.
  const uintptr_t wordAddress = byteAddress & ~uintptr_t{3};
  const unsigned shift = static_cast<unsigned>((byteAddress - wordAddress) * 8);
  const uint32_t shiftedMask = static_cast<uint32_t>(mask) << shift;
  const LONG bits = static_cast<LONG>(shiftedMask);
  if (!IsWritableRange(reinterpret_cast<void*>(wordAddress), sizeof(LONG)))
    return;
  auto* word = reinterpret_cast<volatile LONG*>(wordAddress);
  LONG observed = *word;
  for (;;) {
    const LONG wanted = set ? (observed | bits) : (observed & ~bits);
    if (wanted == observed)
      return;
    const LONG previous = InterlockedCompareExchange(word, wanted, observed);
    if (previous == observed)
      return;
    observed = previous;
  }
}

bool ReadForceMaskBit(uintptr_t byteAddress, uint8_t mask, bool& set) {
  const uintptr_t wordAddress = byteAddress & ~uintptr_t{3};
  uint8_t value = 0;
  if (!IsWritableRange(reinterpret_cast<void*>(wordAddress), sizeof(LONG)) ||
      !ReadValue(reinterpret_cast<const void*>(byteAddress), value)) {
    return false;
  }
  set = (value & mask) != 0;
  return true;
}

bool DescribeBitReference(uintptr_t consoleObject, uintptr_t& forceZeroMask, uintptr_t& forceOneMask,
                          uint32_t& bitNumber) {
  for (std::size_t offset : ce::ue5_layout::kValueOffsets) {
    const ce::ue5_layout::ObjectProbe probe = ReadProbe(consoleObject, offset);
    if (!ce::ue5_layout::IsBitReference(probe))
      continue;
    forceZeroMask = static_cast<uintptr_t>(probe.firstQword);
    forceOneMask = static_cast<uintptr_t>(probe.secondQword);
    bitNumber = probe.bitNumber;
    return true;
  }
  return false;
}

void ResetBitReferenceCandidates() {
  g_bitReferenceCandidates.fill({});
  g_layoutDumpLogs.store(0, std::memory_order_relaxed);
}

ConsoleObjectOutcome InstallConsoleObjectOverride(std::size_t specIndex, HMODULE owner,
                                                  uintptr_t consoleObject, const char* origin) {
  const ce::ue5_cvar::Spec& spec = ce::ue5_cvar::kSpecs[specIndex];
  std::array<ce::ue5_layout::ObjectProbe, std::size(ce::ue5_layout::kValueOffsets)> probes{};
  for (std::size_t index = 0; index < probes.size(); ++index)
    probes[index] = ReadProbe(consoleObject, ce::ue5_layout::kValueOffsets[index]);

  const ce::ue5_layout::Selection selection = ce::ue5_layout::SelectLayout(
      probes.data(), probes.size(), specIndex, ce::ue5_cvar::IsShowFlagSpec(specIndex));
  switch (selection.kind) {
    case ce::ue5_layout::Kind::BitReference: {
      const ce::ue5_layout::ObjectProbe* probe = nullptr;
      for (const ce::ue5_layout::ObjectProbe& item : probes) {
        if (item.offset == selection.offset)
          probe = &item;
      }
      if (!probe)
        return ConsoleObjectOutcome::Refused;
      BitReferenceCandidate& candidate = g_bitReferenceCandidates[specIndex];
      candidate.present = true;
      candidate.object = consoleObject;
      // `FConsoleVariableBitRef` declares Force0MaskPtr before Force1MaskPtr, so
      // the member at the lower offset is the force-to-0 mask.
      candidate.forceZeroMask = static_cast<uintptr_t>(probe->firstQword);
      candidate.forceOneMask = static_cast<uintptr_t>(probe->secondQword);
      candidate.bitNumber = probe->bitNumber;
      return ConsoleObjectOutcome::BitReferenceNotDriven;
    }
    case ce::ue5_layout::Kind::ReferencePointer:
      return InstallReferencePointer(specIndex, owner, consoleObject, selection.offset, origin)
                 ? ConsoleObjectOutcome::Installed
                 : ConsoleObjectOutcome::Refused;
    case ce::ue5_layout::Kind::InlinePair:
      return InstallInlinePair(specIndex, owner, consoleObject, selection.offset, origin)
                 ? ConsoleObjectOutcome::Installed
                 : ConsoleObjectOutcome::Refused;
    case ce::ue5_layout::Kind::None:
      break;
  }
  LogUnrecognisedObject(spec.name, consoleObject, origin, selection.ambiguous);
  return ConsoleObjectOutcome::Refused;
}

std::size_t ReportConfirmedBitReferences(const char* origin) {
  std::size_t reported = 0;
  for (std::size_t index = 0; index < kCVarCount; ++index) {
    BitReferenceCandidate& candidate = g_bitReferenceCandidates[index];
    if (!candidate.present || candidate.reported ||
        g_activeModules[index].load(std::memory_order_acquire)) {
      continue;
    }
    std::size_t agreeing = 0;
    for (std::size_t other = 0; other < kCVarCount; ++other) {
      const BitReferenceCandidate& peer = g_bitReferenceCandidates[other];
      // Distinct bit numbers as well as distinct specs: two records of the same
      // flag would agree trivially and prove nothing about the mask pair.
      if (peer.present && peer.forceZeroMask == candidate.forceZeroMask &&
          peer.forceOneMask == candidate.forceOneMask &&
          (other == index || peer.bitNumber != candidate.bitNumber)) {
        ++agreeing;
      }
    }
    if (agreeing < kBitReferenceConfirmations)
      continue;
    candidate.reported = true;
    if (std::strcmp(ce::ue5_cvar::kSpecs[index].name, kDrivenBitReference) == 0 &&
        DriveBitReference(index, candidate, origin)) {
      ++reported;
      continue;
    }
    ReportBitReference(index, candidate, origin);
    ++reported;
  }
  return reported;
}

}  // namespace UE5::detail
