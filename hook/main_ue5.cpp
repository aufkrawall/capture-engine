#include "main_internal.h"

#include "../common/module_enumeration.h"
#include "common/ue5_rr_override_policy.h"

#include <array>
#include <limits>

namespace UE5 {
namespace {

std::atomic<bool> g_policyRequested{false};

#ifdef _WIN64

constexpr std::size_t kPendingModuleCapacity = 32;
constexpr std::size_t kCandidateWindowBytes = 96;
constexpr std::size_t kAutoConsoleVariablePointerCount = 3;

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
  uintptr_t object = 0;
  std::size_t instructionDistance = (std::numeric_limits<std::size_t>::max)();
  ce::ue5_rr::CandidateEvidence evidence;
  void* originalReference = nullptr;
  int32_t gameThreadValue = 0;
  int32_t renderThreadValue = 0;
  int score = -1;
};

struct OverrideState {
  HMODULE module = nullptr;
  void* volatile* referenceField = nullptr;
  void* originalReference = nullptr;
  uintptr_t object = 0;
};

struct ForcedConsoleVariableData {
  volatile LONG gameThreadValue = 1;
  volatile LONG renderThreadValue = 1;
};
static_assert(sizeof(ForcedConsoleVariableData) == sizeof(int32_t) * 2,
              "forced UE CVar shadow must contain exactly the game/render int32 values");

class ScopedModuleReference {
 public:
  explicit ScopedModuleReference(HMODULE module) {
    if (module) {
      GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCSTR>(module), &module_);
    }
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

std::array<std::atomic<HMODULE>, kPendingModuleCapacity> g_pendingModules{};
std::atomic<bool> g_fullRescanRequested{true};
std::atomic<HMODULE> g_activeModule{nullptr};
std::atomic<bool> g_activeModuleUnloaded{false};
std::atomic<uint32_t> g_queueOverflowCount{0};
std::atomic<ForcedConsoleVariableData*> g_forcedData{nullptr};
OverrideState g_override;
bool g_wasEnabled = false;
bool g_noCandidateLogged = false;

bool IsReadableProtection(DWORD protection) {
  if (protection & (PAGE_GUARD | PAGE_NOACCESS))
    return false;
  switch (protection & 0xFF) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
      return true;
    default:
      return false;
  }
}

bool IsWritableProtection(DWORD protection) {
  if (protection & (PAGE_GUARD | PAGE_NOACCESS))
    return false;
  switch (protection & 0xFF) {
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
      return true;
    default:
      return false;
  }
}

bool IsExecutableProtection(DWORD protection) {
  if (protection & (PAGE_GUARD | PAGE_NOACCESS))
    return false;
  switch (protection & 0xFF) {
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
      return true;
    default:
      return false;
  }
}

template <typename Predicate>
bool IsRangeProtected(const void* pointer, std::size_t size, Predicate predicate) {
  if (!pointer || size == 0)
    return false;
  uintptr_t cursor = reinterpret_cast<uintptr_t>(pointer);
  if (cursor > (std::numeric_limits<uintptr_t>::max)() - size)
    return false;
  const uintptr_t end = cursor + size;
  while (cursor < end) {
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT || !predicate(memory.Protect)) {
      return false;
    }
    const uintptr_t regionBase = reinterpret_cast<uintptr_t>(memory.BaseAddress);
    if (regionBase > (std::numeric_limits<uintptr_t>::max)() - memory.RegionSize)
      return false;
    const uintptr_t regionEnd = regionBase + memory.RegionSize;
    if (regionEnd <= cursor)
      return false;
    cursor = (std::min)(regionEnd, end);
  }
  return true;
}

bool IsReadableRange(const void* pointer, std::size_t size) {
  return IsRangeProtected(pointer, size, IsReadableProtection);
}

bool IsWritableRange(const void* pointer, std::size_t size) {
  return IsRangeProtected(pointer, size, IsWritableProtection);
}

bool IsExecutableAddress(const void* pointer) {
  MEMORY_BASIC_INFORMATION memory{};
  return pointer && VirtualQuery(pointer, &memory, sizeof(memory)) == sizeof(memory) &&
         memory.State == MEM_COMMIT && IsExecutableProtection(memory.Protect);
}

template <typename T>
bool ReadValue(const void* pointer, T& value) {
  if (!IsReadableRange(pointer, sizeof(T)))
    return false;
  std::memcpy(static_cast<void*>(&value), pointer, sizeof(T));
  return true;
}

bool HasCallableVtable(const void* object) {
  void* vtable = nullptr;
  void* firstMethod = nullptr;
  return ReadValue(object, vtable) && ReadValue(vtable, firstMethod) && IsExecutableAddress(firstMethod);
}

const SectionView* FindSection(const ModuleView& image, uintptr_t address, std::size_t bytes,
                               DWORD requiredCharacteristics) {
  for (const SectionView& section : image.sections) {
    if ((section.characteristics & requiredCharacteristics) == requiredCharacteristics &&
        section.Contains(address, bytes)) {
      return &section;
    }
  }
  return nullptr;
}

bool BuildModuleView(HMODULE module, ModuleView& image) {
  MODULEINFO info{};
  if (!GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)) || !info.lpBaseOfDll ||
      info.SizeOfImage < sizeof(IMAGE_DOS_HEADER) + sizeof(IMAGE_NT_HEADERS)) {
    return false;
  }

  image.module = module;
  image.base = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
  image.size = info.SizeOfImage;
  if (image.base > (std::numeric_limits<uintptr_t>::max)() - image.size)
    return false;
  const uintptr_t imageEnd = image.base + image.size;
  if (!IsReadableRange(info.lpBaseOfDll, sizeof(IMAGE_DOS_HEADER)))
    return false;

  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
      static_cast<std::size_t>(dos->e_lfanew) > image.size - sizeof(IMAGE_NT_HEADERS)) {
    return false;
  }

  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image.base + dos->e_lfanew);
  if (!IsReadableRange(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE ||
      nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
      nt->FileHeader.NumberOfSections == 0 || nt->FileHeader.NumberOfSections > 96) {
    return false;
  }

  const uintptr_t optionalHeaderAddress = reinterpret_cast<uintptr_t>(&nt->OptionalHeader);
  if (optionalHeaderAddress < image.base || optionalHeaderAddress > imageEnd ||
      nt->FileHeader.SizeOfOptionalHeader > imageEnd - optionalHeaderAddress) {
    return false;
  }
  const uintptr_t sectionHeaderAddress = optionalHeaderAddress + nt->FileHeader.SizeOfOptionalHeader;
  const std::size_t sectionHeaderBytes =
      static_cast<std::size_t>(nt->FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
  if (sectionHeaderAddress < image.base || sectionHeaderAddress > imageEnd ||
      sectionHeaderBytes > imageEnd - sectionHeaderAddress ||
      !IsReadableRange(reinterpret_cast<const void*>(sectionHeaderAddress), sectionHeaderBytes)) {
    return false;
  }
  const auto* sectionHeaders = reinterpret_cast<const IMAGE_SECTION_HEADER*>(sectionHeaderAddress);

  image.sections.reserve(nt->FileHeader.NumberOfSections);
  for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
    const IMAGE_SECTION_HEADER& header = sectionHeaders[index];
    if (header.VirtualAddress >= image.size)
      continue;
    const std::size_t requestedSize = header.Misc.VirtualSize != 0
                                          ? static_cast<std::size_t>(header.Misc.VirtualSize)
                                          : static_cast<std::size_t>(header.SizeOfRawData);
    const std::size_t sectionSize = (std::min)(requestedSize, image.size - header.VirtualAddress);
    if (sectionSize == 0)
      continue;
    const uintptr_t sectionBegin = image.base + header.VirtualAddress;
    if (!IsReadableRange(reinterpret_cast<const void*>(sectionBegin), sectionSize)) {
      continue;
    }
    image.sections.push_back(
        {sectionBegin, sectionSize, static_cast<DWORD>(header.Characteristics)});
  }
  return !image.sections.empty();
}

const char* ModuleBaseName(HMODULE module, char (&path)[MAX_PATH]) {
  std::memset(path, 0, sizeof(path));
  if (!GetModuleFileNameA(module, path, static_cast<DWORD>(std::size(path))))
    return "<unknown>";
  path[std::size(path) - 1] = '\0';
  const char* backslash = std::strrchr(path, '\\');
  const char* slash = std::strrchr(path, '/');
  const char* base = backslash && slash ? (std::max)(backslash, slash) : (backslash ? backslash : slash);
  return base ? base + 1 : path;
}

void CountCandidateReferences(const ModuleView& image, Candidate& candidate) {
  for (const SectionView& section : image.sections) {
    if (!(section.characteristics & IMAGE_SCN_MEM_EXECUTE) || section.size < 6)
      continue;
    const auto* bytes = reinterpret_cast<const uint8_t*>(section.begin);
    for (std::size_t offset = 0; offset + 6 <= section.size; ++offset) {
      const auto reference = ce::ue5_rr::DecodeRipRelativeReference(
          bytes + offset, section.size - offset, section.begin + offset);
      if (!reference.valid)
        continue;
      if (reference.target == candidate.object)
        ++candidate.evidence.baseReferenceCount;
      else if (reference.target == candidate.object + sizeof(void*))
        ++candidate.evidence.targetFieldReferenceCount;
      else if (reference.target == candidate.object + sizeof(void*) * 2)
        ++candidate.evidence.referenceFieldReferenceCount;
    }
  }
}

void ValidateCandidate(const ModuleView& image, Candidate& candidate) {
  candidate.evidence.objectAligned = (candidate.object % alignof(void*)) == 0;
  candidate.evidence.objectInWritableSection =
      FindSection(image, candidate.object, sizeof(void*) * kAutoConsoleVariablePointerCount,
                  IMAGE_SCN_MEM_WRITE) != nullptr;
  candidate.evidence.objectPageWritable =
      IsWritableRange(reinterpret_cast<void*>(candidate.object), sizeof(void*) * kAutoConsoleVariablePointerCount);
  candidate.evidence.objectVtableCallable = HasCallableVtable(reinterpret_cast<void*>(candidate.object));

  void* targetObject = nullptr;
  void* referenceData = nullptr;
  const void* targetField = reinterpret_cast<void*>(candidate.object + sizeof(void*));
  const void* referenceField = reinterpret_cast<void*>(candidate.object + sizeof(void*) * 2);
  candidate.evidence.targetObjectCallable = ReadValue(targetField, targetObject) && HasCallableVtable(targetObject);
  candidate.evidence.referenceDataReadable =
      ReadValue(referenceField, referenceData) &&
      IsReadableRange(referenceData, sizeof(int32_t) * 2);
  if (candidate.evidence.referenceDataReadable) {
    const auto* values = static_cast<const int32_t*>(referenceData);
    std::memcpy(&candidate.gameThreadValue, values, sizeof(candidate.gameThreadValue));
    std::memcpy(&candidate.renderThreadValue, values + 1, sizeof(candidate.renderThreadValue));
    candidate.evidence.shadowValuesPlausible =
        ce::ue5_rr::IsPlausibleDenoiserModeShadow(candidate.gameThreadValue) &&
        ce::ue5_rr::IsPlausibleDenoiserModeShadow(candidate.renderThreadValue);
    candidate.originalReference = referenceData;
  }
  candidate.evidence.instructionDistance = candidate.instructionDistance;
  CountCandidateReferences(image, candidate);
  candidate.score = ce::ue5_rr::ScoreCandidate(candidate.evidence);
}

ForcedConsoleVariableData* GetOrCreateForcedData() {
  ForcedConsoleVariableData* existing = g_forcedData.load(std::memory_order_acquire);
  if (existing)
    return existing;

  auto* allocated = static_cast<ForcedConsoleVariableData*>(
      VirtualAlloc(nullptr, sizeof(ForcedConsoleVariableData), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (!allocated)
    return nullptr;
  allocated->gameThreadValue = 1;
  allocated->renderThreadValue = 1;

  if (!g_forcedData.compare_exchange_strong(existing, allocated, std::memory_order_acq_rel)) {
    VirtualFree(allocated, 0, MEM_RELEASE);
    return existing;
  }
  // This tiny process-lifetime allocation is deliberately retained. A render
  // thread may have loaded the Ref pointer immediately before hook shutdown;
  // keeping the two values mapped avoids a stale-read UAF after this DLL exits.
  return allocated;
}

bool ApplyCandidate(const ModuleView& image, const Candidate& candidate) {
  ForcedConsoleVariableData* forcedData = GetOrCreateForcedData();
  if (!forcedData) {
    HookLogImportant("UE5 RR: unable to allocate process-lifetime DenoiserMode storage (error=%lu)",
                     GetLastError());
    return false;
  }
  auto* referenceField = reinterpret_cast<void* volatile*>(candidate.object + sizeof(void*) * 2);
  InterlockedExchange(&forcedData->gameThreadValue, 1);
  InterlockedExchange(&forcedData->renderThreadValue, 1);
  void* observed = InterlockedCompareExchangePointer(referenceField, forcedData, candidate.originalReference);
  if (observed != candidate.originalReference) {
    char modulePath[MAX_PATH];
    HookLogImportant(
        "UE5 RR: refused changed TAutoConsoleVariable reference in %s (object=%p expected=%p observed=%p)",
        ModuleBaseName(image.module, modulePath), reinterpret_cast<void*>(candidate.object),
        candidate.originalReference, observed);
    return false;
  }

  g_override = {image.module, referenceField, candidate.originalReference, candidate.object};
  g_activeModule.store(image.module, std::memory_order_release);
  g_activeModuleUnloaded.store(false, std::memory_order_release);
  char modulePath[MAX_PATH];
  HookLogImportant(
      "UE5 RR: persistent r.NGX.DLSS.DenoiserMode=1 override installed in %s "
      "(module=%p objectRva=0x%llX oldGame=%d oldRender=%d score=%d)",
      ModuleBaseName(image.module, modulePath), image.module,
      static_cast<unsigned long long>(candidate.object - image.base), candidate.gameThreadValue,
      candidate.renderThreadValue, candidate.score);
  return true;
}

bool ScanModule(HMODULE module) {
  ScopedModuleReference reference(module);
  if (!reference.Get())
    return false;

  ModuleView image;
  if (!BuildModuleView(reference.Get(), image))
    return false;

  std::vector<uintptr_t> strings;
  for (const SectionView& section : image.sections) {
    if (!(section.characteristics & IMAGE_SCN_MEM_READ) || (section.characteristics & IMAGE_SCN_MEM_EXECUTE))
      continue;
    const auto* bytes = reinterpret_cast<const uint8_t*>(section.begin);
    std::size_t cursor = 0;
    while (cursor < section.size) {
      const std::size_t found =
          ce::ue5_rr::FindUtf16LeAsciiInsensitive(bytes + cursor, section.size - cursor,
                                                  ce::ue5_rr::kDenoiserModeCVar);
      if (found == ce::ue5_rr::kNotFound)
        break;
      strings.push_back(section.begin + cursor + found);
      cursor += found + 2;
    }
  }
  if (strings.empty())
    return false;

  std::vector<Candidate> candidates;
  for (const SectionView& code : image.sections) {
    if (!(code.characteristics & IMAGE_SCN_MEM_EXECUTE) || code.size < 6)
      continue;
    const auto* bytes = reinterpret_cast<const uint8_t*>(code.begin);
    for (std::size_t offset = 0; offset + 6 <= code.size; ++offset) {
      const uintptr_t instruction = code.begin + offset;
      const auto nameReference =
          ce::ue5_rr::DecodeRipRelativeReference(bytes + offset, code.size - offset, instruction);
      if (!nameReference.valid || !nameReference.TakesAddress() || nameReference.registerIndex != 2 ||
          std::find(strings.begin(), strings.end(), nameReference.target) == strings.end()) {
        continue;
      }

      const uintptr_t windowBegin = instruction > code.begin + kCandidateWindowBytes
                                        ? instruction - kCandidateWindowBytes
                                        : code.begin;
      const uintptr_t codeEnd = code.begin + code.size;
      const uintptr_t windowEnd =
          instruction < codeEnd - (std::min)(kCandidateWindowBytes, code.size)
              ? instruction + kCandidateWindowBytes
              : codeEnd;
      for (uintptr_t cursor = windowBegin; cursor + 6 <= windowEnd; ++cursor) {
        const auto objectReference = ce::ue5_rr::DecodeRipRelativeReference(
            reinterpret_cast<const uint8_t*>(cursor), windowEnd - cursor, cursor);
        if (!objectReference.valid)
          continue;

        for (std::size_t fieldOffset : {std::size_t{0}, sizeof(void*), sizeof(void*) * 2}) {
          if (objectReference.target < fieldOffset)
            continue;
          const uintptr_t object = objectReference.target - fieldOffset;
          if (!FindSection(image, object, sizeof(void*) * kAutoConsoleVariablePointerCount,
                           IMAGE_SCN_MEM_WRITE)) {
            continue;
          }

          const std::size_t distance = cursor > instruction ? cursor - instruction : instruction - cursor;
          auto existing = std::find_if(candidates.begin(), candidates.end(),
                                       [object](const Candidate& candidate) { return candidate.object == object; });
          const bool loadedIntoThis = fieldOffset == 0 && objectReference.TakesAddress() &&
                                      objectReference.registerIndex == 1;
          if (existing == candidates.end()) {
            Candidate candidate;
            candidate.object = object;
            candidate.instructionDistance = distance;
            candidate.evidence.nameLoadedIntoSecondArgument = true;
            candidate.evidence.objectLoadedIntoThis = loadedIntoThis;
            candidates.push_back(candidate);
          } else {
            existing->instructionDistance = (std::min)(existing->instructionDistance, distance);
            existing->evidence.objectLoadedIntoThis |= loadedIntoThis;
          }
        }
      }
    }
  }

  for (Candidate& candidate : candidates)
    ValidateCandidate(image, candidate);
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& left, const Candidate& right) { return left.score > right.score; });

  const int bestScore = candidates.empty() ? -1 : candidates[0].score;
  const int secondBestScore = candidates.size() < 2 ? -1 : candidates[1].score;
  if (!ce::ue5_rr::IsUniquelyStrongCandidate(bestScore, secondBestScore)) {
    static std::atomic<uint32_t> validationLogs{0};
    if (validationLogs.fetch_add(1, std::memory_order_relaxed) < 8) {
      char modulePath[MAX_PATH];
      HookLogImportant(
          "UE5 RR: found DenoiserMode literal in %s but no unique validated TAutoConsoleVariable "
          "(strings=%zu candidates=%zu best=%d second=%d); leaving game memory unchanged",
          ModuleBaseName(image.module, modulePath), strings.size(), candidates.size(), bestScore, secondBestScore);
    }
    return false;
  }
  return ApplyCandidate(image, candidates.front());
}

void ClearPendingModules() {
  for (auto& slot : g_pendingModules)
    slot.store(nullptr, std::memory_order_release);
}

bool ScanAllLoadedModules() {
  std::vector<HMODULE> modules;
  if (!ce::EnumerateProcessModules(GetCurrentProcess(), modules)) {
    HookLogImportant("UE5 RR: unable to enumerate loaded modules for initial CVar scan (error=%lu)",
                     GetLastError());
    return false;
  }

  const ULONGLONG start = GetTickCount64();
  for (HMODULE module : modules) {
    if (ScanModule(module)) {
      ClearPendingModules();
      return true;
    }
  }
  if (!g_noCandidateLogged) {
    g_noCandidateLogged = true;
    HookLogImportant(
        "UE5 RR: no validated r.NGX.DLSS.DenoiserMode owner found in %zu loaded modules (%llums); "
        "waiting for the UE NVIDIA plugin to load",
        modules.size(), static_cast<unsigned long long>(GetTickCount64() - start));
  }
  return false;
}

bool ScanPendingModules() {
  for (auto& slot : g_pendingModules) {
    HMODULE module = slot.exchange(nullptr, std::memory_order_acq_rel);
    if (module && ScanModule(module)) {
      ClearPendingModules();
      return true;
    }
  }
  return false;
}

void ForgetUnloadedOverride() {
  if (!g_override.referenceField)
    return;
  HookLogImportant("UE5 RR: owning UE module unloaded; retired the stale CVar override and awaiting reload");
  g_override = {};
  g_activeModuleUnloaded.store(false, std::memory_order_release);
  g_fullRescanRequested.store(true, std::memory_order_release);
}

void RestoreOverride(const char* reason) {
  HMODULE activeModule = g_activeModule.load(std::memory_order_acquire);
  if (!g_override.referenceField || !activeModule) {
    if (g_override.referenceField)
      ForgetUnloadedOverride();
    return;
  }

  ScopedModuleReference reference(activeModule);
  const void* referenceFieldAddress =
      static_cast<const void*>(const_cast<void**>(g_override.referenceField));
  if (!reference.Get() || !IsWritableRange(referenceFieldAddress, sizeof(void*))) {
    g_activeModule.store(nullptr, std::memory_order_release);
    ForgetUnloadedOverride();
    return;
  }

  ForcedConsoleVariableData* forcedData = g_forcedData.load(std::memory_order_acquire);
  void* observed = InterlockedCompareExchangePointer(g_override.referenceField, g_override.originalReference,
                                                     forcedData);
  if (observed == forcedData) {
    HookLogImportant("UE5 RR: restored the game's DenoiserMode reference (%s)", reason);
  } else {
    HookLogImportant(
        "UE5 RR: DenoiserMode reference changed before restore (%s, observed=%p); left newer owner untouched",
        reason, observed);
  }
  g_activeModule.store(nullptr, std::memory_order_release);
  g_override = {};
}

#endif  // _WIN64

}  // namespace

void NotifyModuleLoaded(HMODULE module) {
#ifdef _WIN64
  if (!module || !g_policyRequested.load(std::memory_order_acquire))
    return;
  for (auto& slot : g_pendingModules) {
    HMODULE empty = nullptr;
    if (slot.compare_exchange_strong(empty, module, std::memory_order_acq_rel)) {
      if (g_hCheckHooksEvent)
        SetEvent(g_hCheckHooksEvent);
      return;
    }
    if (empty == module)
      return;
  }
  g_queueOverflowCount.fetch_add(1, std::memory_order_relaxed);
  g_fullRescanRequested.store(true, std::memory_order_release);
  if (g_hCheckHooksEvent)
    SetEvent(g_hCheckHooksEvent);
#else
  (void)module;
#endif
}

void NotifyModuleUnloaded(void* moduleBase, std::size_t moduleSize) {
#ifdef _WIN64
  (void)moduleSize;
  HMODULE module = static_cast<HMODULE>(moduleBase);
  if (!module)
    return;
  for (auto& slot : g_pendingModules) {
    HMODULE pending = slot.load(std::memory_order_acquire);
    if (pending == module)
      slot.compare_exchange_strong(pending, nullptr, std::memory_order_acq_rel);
  }
  HMODULE active = module;
  if (g_activeModule.compare_exchange_strong(active, nullptr, std::memory_order_acq_rel)) {
    g_activeModuleUnloaded.store(true, std::memory_order_release);
    if (g_hCheckHooksEvent)
      SetEvent(g_hCheckHooksEvent);
  }
#else
  (void)moduleBase;
  (void)moduleSize;
#endif
}

void RefreshRayReconstructionOverride(bool enabled) {
  g_policyRequested.store(enabled, std::memory_order_release);
#ifdef _WIN64
  if (!enabled) {
    if (g_wasEnabled)
      RestoreOverride("configuration disabled");
    g_wasEnabled = false;
    g_noCandidateLogged = false;
    g_fullRescanRequested.store(true, std::memory_order_release);
    ClearPendingModules();
    return;
  }

  if (!g_wasEnabled) {
    g_wasEnabled = true;
    g_noCandidateLogged = false;
    g_fullRescanRequested.store(true, std::memory_order_release);
    HookLogImportant(
        "UE5 RR: force policy enabled; selecting the existing NVIDIA UE plugin's DenoiserMode in memory "
        "without spoofing NGX support");
  }

  if (g_activeModuleUnloaded.load(std::memory_order_acquire))
    ForgetUnloadedOverride();
  if (g_activeModule.load(std::memory_order_acquire)) {
    ClearPendingModules();
    return;
  }

  if (g_fullRescanRequested.exchange(false, std::memory_order_acq_rel)) {
    const uint32_t overflow = g_queueOverflowCount.exchange(0, std::memory_order_acq_rel);
    if (overflow)
      HookLog("UE5 RR: module notification queue overflowed %u time(s); using a full safe rescan", overflow);
    if (ScanAllLoadedModules())
      return;
  }
  ScanPendingModules();
#else
  static bool logged = false;
  if (enabled && !logged) {
    logged = true;
    HookLogImportant("UE5 RR: force policy is unavailable in 32-bit processes");
  }
#endif
}

void ShutdownRayReconstructionOverride() {
  g_policyRequested.store(false, std::memory_order_release);
#ifdef _WIN64
  RestoreOverride("hook shutdown");
  ClearPendingModules();
#endif
}

}  // namespace UE5
