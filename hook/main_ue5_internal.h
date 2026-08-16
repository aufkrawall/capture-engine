#pragma once

#include "main_internal.h"

#include "common/ue5_console_layout.h"
#include "common/ue5_cvar_override_policy.h"
#include "common/ue5_redirect_plan.h"
#include "common/ue5_rr_override_policy.h"

#include <array>
#include <cstring>
#include <limits>

namespace UE5::detail {

// Loader bursts in a UE title (112 modules in Talos, more with third-party
// overlays) overflowed a 32-slot queue 8-11 times per launch, and every
// overflow costs a full ~700 ms rescan of every loaded module. Sized so an
// ordinary launch never falls back.
constexpr std::size_t kPendingModuleCapacity = 128;
constexpr std::size_t kCandidateWindowBytes = 96;
constexpr std::size_t kAutoConsoleVariablePointerCount = 3;
// UE 5.4/5.6 FConsoleVariable exposes its value through a data pointer here,
// with the local fallback pair eight bytes further on (verified against
// IConsoleVariable::GetValueOnGameThread).
constexpr std::size_t kRefDataPointerOffset = 0x50;
constexpr std::size_t kCVarCount = ce::ue5_cvar::kSpecs.size();

struct SectionView {
  uintptr_t begin = 0;
  std::size_t size = 0;
  DWORD characteristics = 0;

  bool Contains(uintptr_t address, std::size_t bytes = 1) const {
    if (bytes == 0 || begin > (std::numeric_limits<uintptr_t>::max)() - size || address < begin ||
        address > (std::numeric_limits<uintptr_t>::max)() - bytes)
      return false;
    return address + bytes <= begin + size;
  }
};

struct ModuleView {
  HMODULE module = nullptr;
  uintptr_t base = 0;
  std::size_t size = 0;
  std::vector<SectionView> sections;
};

struct Candidate {
  std::size_t specIndex = 0;
  uintptr_t object = 0;
  std::size_t instructionDistance = (std::numeric_limits<std::size_t>::max)();
  ce::ue5_rr::CandidateEvidence evidence;
  void* originalReference = nullptr;
  uint32_t gameThreadBits = 0;
  uint32_t renderThreadBits = 0;
  int score = -1;
  // Ref-only layout (UE 5.6 Lumen/rendering CVars): the {game, render} value
  // pair lives inside the Ref object instead of behind the third object
  // pointer. When dataShadowPointerRedirect is set, the Ref exposes a data
  // pointer at dataShadowAddress (expected original in originalReference) that
  // the engine dereferences for the value; the local fallback pair sits eight
  // bytes after it.
  uintptr_t dataShadowAddress = 0;
  uint32_t dataShadowGameBits = 0;
  uint32_t dataShadowRenderBits = 0;
  bool dataShadowUsable = false;
  bool dataShadowPointerRedirect = false;
};

enum class CandidateReferenceField : uint8_t {
  Base,
  Target,
  Reference,
};

struct CandidateReferenceTarget {
  uintptr_t address = 0;
  std::size_t candidateIndex = 0;
  CandidateReferenceField field = CandidateReferenceField::Base;
};

struct LiteralReference {
  uintptr_t address = 0;
  std::size_t specIndex = 0;
};

struct OverrideState {
  HMODULE module = nullptr;
  void* volatile* referenceField = nullptr;
  // The pointer value the game owned before the redirect: the third object
  // pointer for the pointer model, the `ref+0x50` data pointer for the
  // data-pointer model. Restoring writes exactly this back, so it must never
  // be left null while dataShadowPointerRedirect is set.
  void* originalReference = nullptr;
  uintptr_t object = 0;
  // Non-zero when this override writes the {game, render} pair directly into
  // the Ref object (no redirectable pointer exists for that layout).
  uintptr_t dataShadowAddress = 0;
  uint32_t originalDataShadowGameBits = 0;
  uint32_t originalDataShadowRenderBits = 0;
  // Value the game kept in the storage the original data pointer addressed,
  // plus whether CE mirrored its own value into it. Engine code generated for
  // `FAutoConsoleVariableRef` CVars reads that storage directly instead of
  // going through the console object, so redirecting the pointer alone would
  // leave those reads on the game's value.
  uint32_t originalDataPointerBits = 0;
  bool dataPointerValueWritten = false;
  bool dataShadowPointerRedirect = false;
  bool registryResolved = false;
  // Bit-reference mode (`FConsoleVariableBitRef`, UE's `ShowFlag.*`). Deliberately
  // NOT part of `IsOverrideInstalled`: nothing verifies or re-asserts a force bit,
  // it is written once and handed back on teardown.
  bool bitReference = false;
  uintptr_t forceZeroByte = 0;
  uintptr_t forceOneByte = 0;
  uint8_t bitMask = 0;
  bool originalForceZeroBit = false;
  bool originalForceOneBit = false;
  // Ref-redirect mode only. Repointing the wrapper's `Ref` covers reads that go
  // back through the wrapper (`TAutoConsoleVariable::GetValueOnRenderThread`),
  // but UE's renderer routinely caches the `TConsoleVariableData<T>*` itself
  // (`IConsoleManager::FindTConsoleVariableDataInt`, resolved once into a
  // static) and then never consults the wrapper again. Such a reader keeps
  // seeing the game's original pair, so CE mirrors its value into that pair as
  // well - the same both-read-paths contract the data-pointer mode already has.
  bool referencePairWritten = false;
  uint32_t originalReferenceGameBits = 0;
  uint32_t originalReferenceRenderBits = 0;
  // Verification bookkeeping. Drift reporting is capped per override, and the
  // cap only lifts after a long clean run, so a variable the game rewrites
  // constantly stays quiet while a rare regression is still reported.
  uint32_t driftReports = 0;
  uint32_t cleanVerifications = 0;
  // Lifetime re-assert count. The per-override drift report is capped at three
  // and the summary line only prints when the counts change, so a CVar the game
  // rewrites on every pass would fall silent after three lines and look
  // settled - while the setting is in fact flickering between CE's value and
  // the game's for the whole session. Reported on a widening scale instead.
  uint32_t reassertCount = 0;
};

// Consecutive clean verifications (at roughly one per second) before an
// override may report drift again.
constexpr uint32_t kDriftReportResetPasses = 60;

struct VerificationCounts {
  std::size_t checked = 0;
  std::size_t verified = 0;
  std::size_t reasserted = 0;
  std::size_t lost = 0;
};

struct ForcedConsoleVariableData {
  volatile LONG gameThreadBits = 0;
  volatile LONG renderThreadBits = 0;
};
static_assert(sizeof(ForcedConsoleVariableData) == sizeof(uint32_t) * 2,
              "forced UE CVar shadow must contain exactly the game/render 32-bit values");

class ScopedModuleReference {
 public:
  explicit ScopedModuleReference(HMODULE module) {
    if (module)
      GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCSTR>(module), &module_);
  }

  ~ScopedModuleReference() {
    if (module_)
      FreeLibrary(module_);
  }

  ScopedModuleReference(const ScopedModuleReference&) = delete;
  ScopedModuleReference& operator=(const ScopedModuleReference&) = delete;

  HMODULE Get() const { return module_; }

 private:
  HMODULE module_ = nullptr;
};

extern std::array<std::atomic<HMODULE>, kPendingModuleCapacity> g_pendingModules;
extern std::atomic<bool> g_fullRescanRequested;
extern std::atomic<uint32_t> g_queueOverflowCount;
extern std::array<std::atomic<HMODULE>, kCVarCount> g_activeModules;
extern std::array<std::atomic<bool>, kCVarCount> g_activeModuleUnloaded;
extern std::array<std::atomic<ForcedConsoleVariableData*>, kCVarCount> g_forcedData;
extern std::array<OverrideState, kCVarCount> g_overrides;
extern std::array<ce::ue5_cvar::ResolvedValue, kCVarCount> g_desired;
extern bool g_missingSummaryLogged;

bool IsReadableRange(const void* pointer, std::size_t size);
bool IsWritableRange(const void* pointer, std::size_t size);
bool IsExecutableAddress(const void* pointer);
bool HasCallableVtable(const void* object);
const char* ModuleBaseName(HMODULE module, char (&path)[MAX_PATH]);
bool BuildModuleView(HMODULE module, ModuleView& image);
const SectionView* FindSection(const ModuleView& image, uintptr_t address, std::size_t bytes,
                               DWORD requiredCharacteristics);

template <typename T>
bool ReadValue(const void* pointer, T& value) {
  if (!IsReadableRange(pointer, sizeof(T)))
    return false;
  std::memcpy(static_cast<void*>(&value), pointer, sizeof(T));
  return true;
}

void ClearPendingModules();
void UpdateForcedData(std::size_t specIndex, uint32_t bits);
// Process-lifetime shadow storage for one override, allocated on first use.
ForcedConsoleVariableData* GetOrCreateForcedData(std::size_t specIndex);
// Commits the recorded undo information for a data-pointer redirect, then
// mirrors CE's value into the storage the original pointer addressed.
void ApplyRestorePlan(OverrideState& state, const ce::ue5_redirect::Plan& plan, uint32_t bits);
void RestoreOverride(std::size_t specIndex, const char* reason);
void RestoreAllOverrides(const char* reason);
void ForgetUnloadedOverrides();
bool ApplyCandidate(const ModuleView& image, const Candidate& candidate, std::size_t discoveredCandidates,
                    std::size_t validatedCandidates, ULONGLONG scanElapsedMs);

// True once either value mode owns storage for this override.
inline bool IsOverrideInstalled(const OverrideState& state) {
  return state.referenceField != nullptr || state.dataShadowAddress != 0;
}

// Drives an IConsoleObject whose address came from somewhere other than the
// module scan (today: UE's console registry), where no static
// FAutoConsoleVariable wrapper exists to identify the layout. The object is
// probed first and only written through the layout it proves to have; an
// unrecognised object is left untouched and reported.
//
// A bit-reference object is never written. It is recorded, confirmed against a
// second show flag, and reported - see `ReportConfirmedBitReferences`.
enum class ConsoleObjectOutcome : uint8_t {
  Installed,
  BitReferenceNotDriven,
  Refused,
};

ConsoleObjectOutcome InstallConsoleObjectOverride(std::size_t specIndex, HMODULE owner,
                                                  uintptr_t consoleObject, const char* origin);
// Sets or clears one bit of a UE force mask without disturbing the flags sharing
// the byte, and reads one back.
void UpdateForceMaskBit(uintptr_t byteAddress, uint8_t mask, bool set);
bool ReadForceMaskBit(uintptr_t byteAddress, uint8_t mask, bool& set);

// Reports every recorded bit-reference candidate whose mask pair a second
// candidate independently confirms, once each. Returns how many were reported.
std::size_t ReportConfirmedBitReferences(const char* origin);
void ResetBitReferenceCandidates();
// Reads the `{Force0MaskPtr, Force1MaskPtr, BitNumber}` triple out of a console
// object that carries the bit-reference shape. Diagnostic only.
bool DescribeBitReference(uintptr_t consoleObject, uintptr_t& forceZeroMask, uintptr_t& forceOneMask,
                          uint32_t& bitNumber);
bool ScanAllLoadedModules();
bool ScanPendingModules();

// Reads every installed override back through the storage the engine reads,
// re-asserting drifted values and retiring redirects the game took back.
VerificationCounts VerifyOverrides();

// Resolves still-missing CVars through UE's own console-object registry. The
// literal scan cannot see names UE composes at runtime (`ShowFlag.%s`), and
// per-title object layouts can defeat candidate validation; both resolve
// exactly once an already-installed override anchors the registry.
bool ResolveMissingThroughConsoleRegistry();
void ResetConsoleRegistry();
// Lets a newly requested CVar be looked for again after the resolver had
// concluded, without discarding the map it already located.
void ReopenConsoleRegistry();

}  // namespace UE5::detail
