#pragma once

#include "main_internal.h"

#include "common/ue5_cvar_override_policy.h"
#include "common/ue5_rr_override_policy.h"

#include <array>
#include <limits>

namespace UE5::detail {

constexpr std::size_t kPendingModuleCapacity = 32;
constexpr std::size_t kCandidateWindowBytes = 96;
constexpr std::size_t kAutoConsoleVariablePointerCount = 3;
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
  void* originalReference = nullptr;
  uintptr_t object = 0;
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
const char* ModuleBaseName(HMODULE module, char (&path)[MAX_PATH]);
void ClearPendingModules();
void UpdateForcedData(std::size_t specIndex, uint32_t bits);
void RestoreOverride(std::size_t specIndex, const char* reason);
void RestoreAllOverrides(const char* reason);
void ForgetUnloadedOverrides();
bool ScanAllLoadedModules();
bool ScanPendingModules();

}  // namespace UE5::detail
