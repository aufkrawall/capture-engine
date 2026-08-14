#include "main_ue5_internal.h"

#include "../common/module_enumeration.h"

namespace UE5::detail {
namespace {

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
    const std::size_t sectionSize =
        (std::min)(requestedSize, image.size - static_cast<std::size_t>(header.VirtualAddress));
    if (sectionSize == 0)
      continue;
    const uintptr_t sectionBegin = image.base + header.VirtualAddress;
    if (IsReadableRange(reinterpret_cast<const void*>(sectionBegin), sectionSize))
      image.sections.push_back({sectionBegin, sectionSize, static_cast<DWORD>(header.Characteristics)});
  }
  return !image.sections.empty();
}

std::vector<LiteralReference> FindRequestedLiterals(const ModuleView& image) {
  std::vector<std::size_t> requested;
  for (std::size_t index = 0; index < kCVarCount; ++index) {
    if (g_desired[index].enabled && !g_activeModules[index].load(std::memory_order_acquire))
      requested.push_back(index);
  }
  std::vector<LiteralReference> literals;
  if (requested.empty())
    return literals;

  for (const SectionView& section : image.sections) {
    if (!(section.characteristics & IMAGE_SCN_MEM_READ) || (section.characteristics & IMAGE_SCN_MEM_EXECUTE) ||
        section.size < 4) {
      continue;
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(section.begin);
    for (std::size_t offset = 0; offset + 4 <= section.size; ++offset) {
      if (bytes[offset + 1] != 0)
        continue;
      const uint8_t first = ce::ue5_rr::FoldAscii(bytes[offset]);
      if (first != 'r' && first != 's')
        continue;
      for (std::size_t specIndex : requested) {
        const char* name = ce::ue5_cvar::kSpecs[specIndex].name;
        if (first == ce::ue5_rr::FoldAscii(static_cast<uint8_t>(name[0])) &&
            ce::ue5_rr::MatchesUtf16LeAsciiInsensitive(bytes + offset, section.size - offset, name)) {
          literals.push_back({section.begin + offset, specIndex});
        }
      }
    }
  }
  std::sort(literals.begin(), literals.end(), [](const LiteralReference& left, const LiteralReference& right) {
    return left.address < right.address;
  });
  return literals;
}

void AddNearbyCandidates(const ModuleView& image, const SectionView& code, uintptr_t instruction,
                         std::size_t specIndex, std::vector<Candidate>& candidates) {
  const uintptr_t windowBegin = instruction > code.begin + kCandidateWindowBytes
                                    ? instruction - kCandidateWindowBytes
                                    : code.begin;
  const uintptr_t codeEnd = code.begin + code.size;
  const uintptr_t windowEnd = instruction < codeEnd - (std::min)(kCandidateWindowBytes, code.size)
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
      if (!FindSection(image, object, sizeof(void*) * kAutoConsoleVariablePointerCount, IMAGE_SCN_MEM_WRITE))
        continue;
      const std::size_t distance = cursor > instruction ? cursor - instruction : instruction - cursor;
      auto existing = std::find_if(candidates.begin(), candidates.end(), [specIndex, object](const Candidate& item) {
        return item.specIndex == specIndex && item.object == object;
      });
      const bool loadedIntoThis = fieldOffset == 0 && objectReference.TakesAddress() &&
                                  objectReference.registerIndex == 1;
      if (existing == candidates.end()) {
        Candidate candidate;
        candidate.specIndex = specIndex;
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

std::vector<Candidate> FindCandidates(const ModuleView& image, const std::vector<LiteralReference>& literals) {
  std::vector<Candidate> candidates;
  for (const SectionView& code : image.sections) {
    if (!(code.characteristics & IMAGE_SCN_MEM_EXECUTE) || code.size < 6)
      continue;
    const auto* bytes = reinterpret_cast<const uint8_t*>(code.begin);
    for (std::size_t offset = 0; offset + 6 <= code.size; ++offset) {
      const uintptr_t instruction = code.begin + offset;
      const auto nameReference =
          ce::ue5_rr::DecodeRipRelativeReference(bytes + offset, code.size - offset, instruction);
      if (!nameReference.valid || !nameReference.TakesAddress() || nameReference.registerIndex != 2)
        continue;
      auto literal = std::lower_bound(literals.begin(), literals.end(), nameReference.target,
                                      [](const LiteralReference& item, uintptr_t address) {
                                        return item.address < address;
                                      });
      while (literal != literals.end() && literal->address == nameReference.target) {
        AddNearbyCandidates(image, code, instruction, literal->specIndex, candidates);
        ++literal;
      }
    }
  }
  return candidates;
}

void CountCandidateReferences(const ModuleView& image, std::vector<Candidate>& candidates) {
  std::vector<CandidateReferenceTarget> targets;
  targets.reserve(candidates.size() * kAutoConsoleVariablePointerCount);
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    const uintptr_t object = candidates[index].object;
    targets.push_back({object, index, CandidateReferenceField::Base});
    targets.push_back({object + sizeof(void*), index, CandidateReferenceField::Target});
    targets.push_back({object + sizeof(void*) * 2, index, CandidateReferenceField::Reference});
  }
  std::sort(targets.begin(), targets.end(), [](const CandidateReferenceTarget& left,
                                                const CandidateReferenceTarget& right) {
    return left.address < right.address;
  });
  for (const SectionView& section : image.sections) {
    if (!(section.characteristics & IMAGE_SCN_MEM_EXECUTE) || section.size < 6)
      continue;
    const auto* bytes = reinterpret_cast<const uint8_t*>(section.begin);
    for (std::size_t offset = 0; offset + 6 <= section.size; ++offset) {
      const auto reference = ce::ue5_rr::DecodeRipRelativeReference(
          bytes + offset, section.size - offset, section.begin + offset);
      if (!reference.valid)
        continue;
      auto target = std::lower_bound(targets.begin(), targets.end(), reference.target,
                                     [](const CandidateReferenceTarget& item, uintptr_t address) {
                                       return item.address < address;
                                     });
      while (target != targets.end() && target->address == reference.target) {
        Candidate& candidate = candidates[target->candidateIndex];
        switch (target->field) {
          case CandidateReferenceField::Base:
            ++candidate.evidence.baseReferenceCount;
            break;
          case CandidateReferenceField::Target:
            ++candidate.evidence.targetFieldReferenceCount;
            break;
          case CandidateReferenceField::Reference:
            ++candidate.evidence.referenceFieldReferenceCount;
            break;
        }
        ++target;
      }
    }
  }
}

void ValidateCandidateLayout(const ModuleView& image, Candidate& candidate) {
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
      ReadValue(referenceField, referenceData) && IsReadableRange(referenceData, sizeof(uint32_t) * 2);
  if (candidate.evidence.referenceDataReadable) {
    const auto* values = static_cast<const uint32_t*>(referenceData);
    std::memcpy(&candidate.gameThreadBits, values, sizeof(candidate.gameThreadBits));
    std::memcpy(&candidate.renderThreadBits, values + 1, sizeof(candidate.renderThreadBits));
    candidate.evidence.shadowValuesPlausible =
        ce::ue5_cvar::IsPlausibleShadowValue(candidate.specIndex, candidate.gameThreadBits) &&
        ce::ue5_cvar::IsPlausibleShadowValue(candidate.specIndex, candidate.renderThreadBits);
    candidate.originalReference = referenceData;
  }
  candidate.evidence.instructionDistance = candidate.instructionDistance;
}

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

bool ApplyCandidate(const ModuleView& image, const Candidate& candidate, std::size_t discoveredCandidates,
                    std::size_t validatedCandidates, ULONGLONG scanElapsedMs) {
  const std::size_t specIndex = candidate.specIndex;
  ForcedConsoleVariableData* forcedData = GetOrCreateForcedData(specIndex);
  if (!forcedData) {
    HookLogImportant("UE5 overrides: unable to allocate process-lifetime storage for %s (error=%lu)",
                     ce::ue5_cvar::kSpecs[specIndex].name, GetLastError());
    return false;
  }
  UpdateForcedData(specIndex, g_desired[specIndex].bits);
  auto* referenceField = reinterpret_cast<void* volatile*>(candidate.object + sizeof(void*) * 2);
  void* observed = InterlockedCompareExchangePointer(referenceField, forcedData, candidate.originalReference);
  if (observed != candidate.originalReference) {
    char modulePath[MAX_PATH];
    HookLogImportant(
        "UE5 overrides: refused changed TAutoConsoleVariable reference for %s in %s "
        "(object=%p expected=%p observed=%p)",
        ce::ue5_cvar::kSpecs[specIndex].name, ModuleBaseName(image.module, modulePath),
        reinterpret_cast<void*>(candidate.object), candidate.originalReference, observed);
    return false;
  }

  g_overrides[specIndex] = {image.module, referenceField, candidate.originalReference, candidate.object};
  g_activeModules[specIndex].store(image.module, std::memory_order_release);
  g_activeModuleUnloaded[specIndex].store(false, std::memory_order_release);
  char modulePath[MAX_PATH];
  const ce::ue5_cvar::Spec& spec = ce::ue5_cvar::kSpecs[specIndex];
  if (spec.type == ce::ue5_cvar::ValueType::Float) {
    HookLogImportant(
        "UE5 overrides: persistent %s=%.3f installed in %s "
        "(objectRva=0x%llX oldGame=%.3f oldRender=%.3f score=%d scanMs=%llu candidates=%zu/%zu)",
        spec.name, std::bit_cast<float>(g_desired[specIndex].bits), ModuleBaseName(image.module, modulePath),
        static_cast<unsigned long long>(candidate.object - image.base),
        std::bit_cast<float>(candidate.gameThreadBits), std::bit_cast<float>(candidate.renderThreadBits),
        candidate.score, static_cast<unsigned long long>(scanElapsedMs), validatedCandidates,
        discoveredCandidates);
  } else {
    HookLogImportant(
        "UE5 overrides: persistent %s=%d installed in %s "
        "(objectRva=0x%llX oldGame=%d oldRender=%d score=%d scanMs=%llu candidates=%zu/%zu)",
        spec.name, static_cast<int32_t>(g_desired[specIndex].bits), ModuleBaseName(image.module, modulePath),
        static_cast<unsigned long long>(candidate.object - image.base),
        static_cast<int32_t>(candidate.gameThreadBits), static_cast<int32_t>(candidate.renderThreadBits),
        candidate.score, static_cast<unsigned long long>(scanElapsedMs), validatedCandidates,
        discoveredCandidates);
  }
  return true;
}

bool ScanModule(HMODULE module) {
  const ULONGLONG scanStart = GetTickCount64();
  ScopedModuleReference reference(module);
  if (!reference.Get())
    return false;
  ModuleView image;
  if (!BuildModuleView(reference.Get(), image))
    return false;
  const std::vector<LiteralReference> literals = FindRequestedLiterals(image);
  if (literals.empty())
    return false;

  std::vector<Candidate> candidates = FindCandidates(image, literals);
  const std::size_t discoveredCandidateCount = candidates.size();
  for (Candidate& candidate : candidates)
    ValidateCandidateLayout(image, candidate);
  candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [](const Candidate& candidate) {
                     return ce::ue5_rr::ScoreCandidate(candidate.evidence) < 0;
                   }),
                   candidates.end());
  CountCandidateReferences(image, candidates);
  for (Candidate& candidate : candidates)
    candidate.score = ce::ue5_rr::ScoreCandidate(candidate.evidence);

  bool installedAny = false;
  for (const LiteralReference& literal : literals) {
    const std::size_t specIndex = literal.specIndex;
    if (g_activeModules[specIndex].load(std::memory_order_acquire))
      continue;
    Candidate* bestCandidate = nullptr;
    int bestScore = -1;
    int secondBestScore = -1;
    std::size_t matchCount = 0;
    for (Candidate& candidate : candidates) {
      if (candidate.specIndex != specIndex)
        continue;
      ++matchCount;
      if (candidate.score > bestScore) {
        secondBestScore = bestScore;
        bestScore = candidate.score;
        bestCandidate = &candidate;
      } else if (candidate.score > secondBestScore) {
        secondBestScore = candidate.score;
      }
    }
    if (!bestCandidate || !ce::ue5_rr::IsUniquelyStrongCandidate(bestScore, secondBestScore)) {
      static std::atomic<uint32_t> validationLogs{0};
      if (validationLogs.fetch_add(1, std::memory_order_relaxed) < 12) {
        char modulePath[MAX_PATH];
        HookLogImportant(
            "UE5 overrides: found %s literal in %s but no unique validated TAutoConsoleVariable "
            "(candidates=%zu best=%d second=%d); leaving game memory unchanged",
            ce::ue5_cvar::kSpecs[specIndex].name, ModuleBaseName(image.module, modulePath), matchCount,
            bestScore, secondBestScore);
      }
      continue;
    }
    installedAny |= ApplyCandidate(image, *bestCandidate, discoveredCandidateCount, matchCount,
                                   GetTickCount64() - scanStart);
  }
  return installedAny;
}

std::size_t DesiredCount() {
  return static_cast<std::size_t>(std::count_if(g_desired.begin(), g_desired.end(),
                                                [](const auto& value) { return value.enabled; }));
}

std::size_t ActiveCount() {
  std::size_t count = 0;
  for (const auto& module : g_activeModules) {
    if (module.load(std::memory_order_acquire))
      ++count;
  }
  return count;
}

}  // namespace

bool IsReadableRange(const void* pointer, std::size_t size) {
  return IsRangeProtected(pointer, size, IsReadableProtection);
}

bool IsWritableRange(const void* pointer, std::size_t size) {
  return IsRangeProtected(pointer, size, IsWritableProtection);
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

void ClearPendingModules() {
  for (auto& slot : g_pendingModules)
    slot.store(nullptr, std::memory_order_release);
}

void UpdateForcedData(std::size_t specIndex, uint32_t bits) {
  ForcedConsoleVariableData* data = g_forcedData[specIndex].load(std::memory_order_acquire);
  if (!data)
    return;
  InterlockedExchange(&data->gameThreadBits, static_cast<LONG>(bits));
  InterlockedExchange(&data->renderThreadBits, static_cast<LONG>(bits));
}

void ForgetUnloadedOverrides() {
  for (std::size_t index = 0; index < kCVarCount; ++index) {
    if (!g_activeModuleUnloaded[index].exchange(false, std::memory_order_acq_rel))
      continue;
    if (g_overrides[index].referenceField) {
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
  if (!state.referenceField)
    return;
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
  ForcedConsoleVariableData* forcedData = g_forcedData[specIndex].load(std::memory_order_acquire);
  void* observed = InterlockedCompareExchangePointer(state.referenceField, state.originalReference, forcedData);
  if (observed == forcedData) {
    HookLogImportant("UE5 overrides: restored the game's %s reference (%s)",
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

bool ScanAllLoadedModules() {
  std::vector<HMODULE> modules;
  if (!ce::EnumerateProcessModules(GetCurrentProcess(), modules)) {
    HookLogImportant("UE5 overrides: unable to enumerate loaded modules for initial CVar scan (error=%lu)",
                     GetLastError());
    return false;
  }

  const ULONGLONG start = GetTickCount64();
  bool installedAny = false;
  HMODULE mainModule = GetModuleHandleW(nullptr);
  if (mainModule)
    installedAny |= ScanModule(mainModule);
  for (HMODULE module : modules) {
    if (module != mainModule)
      installedAny |= ScanModule(module);
  }
  ClearPendingModules();

  const std::size_t desired = DesiredCount();
  const std::size_t active = ActiveCount();
  if (active < desired && !g_missingSummaryLogged) {
    g_missingSummaryLogged = true;
    std::string missing;
    for (std::size_t index = 0; index < kCVarCount; ++index) {
      if (!g_desired[index].enabled || g_activeModules[index].load(std::memory_order_acquire))
        continue;
      if (!missing.empty())
        missing += ", ";
      if (missing.size() > 480) {
        missing += "...";
        break;
      }
      missing += ce::ue5_cvar::kSpecs[index].name;
    }
    HookLogImportant(
        "UE5 overrides: installed %zu/%zu requested persistent CVars across %zu modules in %llums; "
        "unresolved variables may be absent from this UE/plugin build: %s",
        active, desired, modules.size(), static_cast<unsigned long long>(GetTickCount64() - start),
        missing.c_str());
  }
  return installedAny;
}

bool ScanPendingModules() {
  bool installedAny = false;
  for (auto& slot : g_pendingModules) {
    HMODULE module = slot.exchange(nullptr, std::memory_order_acq_rel);
    if (module)
      installedAny |= ScanModule(module);
  }
  return installedAny;
}

}  // namespace UE5::detail
