// Discovery half of the UE5 CVar overrides: find each requested name's
// compiled-in literal, score the static console-variable objects that
// reference it, and hand the winner to the installer. Memory primitives live
// in main_ue5_memory.cpp, installation in main_ue5_install.cpp.
#include "main_ue5_internal.h"

#include "../common/module_enumeration.h"

namespace UE5::detail {
namespace {

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
      if (first != 'r' && first != 's' && first != 't')
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
  if (candidate.evidence.targetObjectCallable && targetObject) {
    const uintptr_t refBase = reinterpret_cast<uintptr_t>(targetObject);
    uintptr_t dataPtr = 0;
    uint32_t value = 0;
    uint32_t valueFlags = 0;
    if (ReadValue(reinterpret_cast<const void*>(refBase + kRefDataPointerOffset), dataPtr) &&
        ReadValue(reinterpret_cast<const void*>(dataPtr), value) &&
        ReadValue(reinterpret_cast<const void*>(dataPtr + sizeof(uint32_t)), valueFlags) &&
        ce::ue5_cvar::IsPlausibleShadowValue(candidate.specIndex, value)) {
      // UE 5.6 FConsoleVariable exposes the value through a data pointer at
      // +0x50 (render-thread reads dereference it; game-thread reads use the
      // local fallback pair at +0x58). Redirect the pointer to CE's shadow and
      // mirror the value into the local pair so both read paths see it.
      candidate.dataShadowAddress = refBase + kRefDataPointerOffset;
      candidate.dataShadowGameBits = value;
      candidate.dataShadowRenderBits = valueFlags;
      candidate.originalReference = reinterpret_cast<void*>(dataPtr);
      candidate.dataShadowPointerRedirect = true;
      candidate.dataShadowUsable = true;
    }
  }
  candidate.evidence.instructionDistance = candidate.instructionDistance;
}

int CandidateScore(const Candidate& candidate) noexcept {
  const int pointerScore = ce::ue5_rr::ScoreCandidate(candidate.evidence);
  if (pointerScore >= 0 || !candidate.dataShadowUsable)
    return pointerScore;
  int score = ce::ue5_rr::kMinimumAcceptableScore;
  if (candidate.instructionDistance < 64)
    score += static_cast<int>((64 - candidate.instructionDistance) / 4);
  return score;
}

void LogUnvalidatedCandidate(const ModuleView& image, const Candidate& raw, std::size_t specIndex,
                             bool hasRawCandidate, std::size_t matchCount, int bestScore,
                             int secondBestScore) {
  char modulePath[MAX_PATH];
  const char* baseName = ModuleBaseName(image.module, modulePath);
  if (!hasRawCandidate) {
    HookLogImportant(
        "UE5 overrides: found %s literal in %s but no validated TAutoConsoleVariable layout "
        "(survivors=%zu best=%d second=%d); leaving game memory unchanged",
        ce::ue5_cvar::kSpecs[specIndex].name, baseName, matchCount, bestScore, secondBestScore);
    return;
  }
  void* fieldVtable = nullptr;
  void* fieldTarget = nullptr;
  void* fieldReference = nullptr;
  ReadValue(reinterpret_cast<void*>(raw.object), fieldVtable);
  ReadValue(reinterpret_cast<void*>(raw.object + sizeof(void*)), fieldTarget);
  ReadValue(reinterpret_cast<void*>(raw.object + sizeof(void*) * 2), fieldReference);
  HookLogImportant(
      "UE5 overrides: %s literal in %s: closest raw candidate score=%d "
      "(aligned=%d writableSec=%d pageWr=%d objVtable=%d targetCall=%d refRead=%d shadowOk=%d "
      "refs=%d/%d/%d objRva=0x%llX fields=0x%p/0x%p/0x%p); survivors=%zu best=%d second=%d",
      ce::ue5_cvar::kSpecs[specIndex].name, baseName,
      ce::ue5_rr::ScoreCandidate(raw.evidence), raw.evidence.objectAligned ? 1 : 0,
      raw.evidence.objectInWritableSection ? 1 : 0, raw.evidence.objectPageWritable ? 1 : 0,
      raw.evidence.objectVtableCallable ? 1 : 0, raw.evidence.targetObjectCallable ? 1 : 0,
      raw.evidence.referenceDataReadable ? 1 : 0, raw.evidence.shadowValuesPlausible ? 1 : 0,
      raw.evidence.baseReferenceCount, raw.evidence.targetFieldReferenceCount,
      raw.evidence.referenceFieldReferenceCount,
      static_cast<unsigned long long>(raw.object - image.base), fieldVtable, fieldTarget,
      fieldReference, matchCount, bestScore, secondBestScore);
  if (raw.dataShadowUsable)
    return;
  static std::atomic<uint32_t> memoryDumpLogs{0};
  if (memoryDumpLogs.fetch_add(1, std::memory_order_relaxed) >= 4)
    return;
  uintptr_t objectQwords[12] = {};
  uintptr_t refQwords[16] = {};
  for (std::size_t qwordIndex = 0; qwordIndex < 12; ++qwordIndex)
    ReadValue(reinterpret_cast<void*>(raw.object + qwordIndex * sizeof(uintptr_t)),
              objectQwords[qwordIndex]);
  const uintptr_t refBase = reinterpret_cast<uintptr_t>(fieldTarget);
  if (refBase) {
    for (std::size_t qwordIndex = 0; qwordIndex < 16; ++qwordIndex)
      ReadValue(reinterpret_cast<void*>(refBase + qwordIndex * sizeof(uintptr_t)), refQwords[qwordIndex]);
  }
  HookLogImportant(
      "UE5 overrides: %s raw candidate memory base=0x%llX obj[0x00..0x60]=0x%llX 0x%llX "
      "0x%llX 0x%llX 0x%llX 0x%llX 0x%llX 0x%llX 0x%llX 0x%llX 0x%llX 0x%llX",
      ce::ue5_cvar::kSpecs[specIndex].name, static_cast<unsigned long long>(image.base),
      objectQwords[0], objectQwords[1], objectQwords[2], objectQwords[3], objectQwords[4],
      objectQwords[5], objectQwords[6], objectQwords[7], objectQwords[8], objectQwords[9],
      objectQwords[10], objectQwords[11]);
  HookLogImportant(
      "UE5 overrides: %s raw candidate ref[0x00..0x80]=0x%llX 0x%llX 0x%llX 0x%llX 0x%llX "
      "0x%llX 0x%llX 0x%llX 0x%llX 0x%llX 0x%llX 0x%llX 0x%llX 0x%llX 0x%llX 0x%llX",
      ce::ue5_cvar::kSpecs[specIndex].name, refQwords[0], refQwords[1], refQwords[2], refQwords[3],
      refQwords[4], refQwords[5], refQwords[6], refQwords[7], refQwords[8], refQwords[9],
      refQwords[10], refQwords[11], refQwords[12], refQwords[13], refQwords[14], refQwords[15]);
}

bool ScanModule(HMODULE module, std::vector<uint8_t>* seenLiterals) {
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
  if (seenLiterals) {
    for (const LiteralReference& literal : literals)
      (*seenLiterals)[literal.specIndex] = 1;
  }

  std::vector<Candidate> candidates = FindCandidates(image, literals);
  const std::size_t discoveredCandidateCount = candidates.size();
  for (Candidate& candidate : candidates)
    ValidateCandidateLayout(image, candidate);
  std::array<Candidate, kCVarCount> bestRawCandidates{};
  std::array<bool, kCVarCount> hasRawCandidate{};
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    const int rawScore = CandidateScore(candidates[index]);
    const std::size_t specIndex = candidates[index].specIndex;
    if (!hasRawCandidate[specIndex] || rawScore > bestRawCandidates[specIndex].score) {
      bestRawCandidates[specIndex] = candidates[index];
      bestRawCandidates[specIndex].score = rawScore;
      hasRawCandidate[specIndex] = true;
    }
  }
  candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [](const Candidate& candidate) {
                     return CandidateScore(candidate) < 0;
                   }),
                   candidates.end());
  CountCandidateReferences(image, candidates);
  for (Candidate& candidate : candidates)
    candidate.score = CandidateScore(candidate);
  // UE 5.6 registers some Lumen/rendering CVars from several sites that share
  // the same Ref data pointer. Keep the highest-scoring representative so the
  // duplicates cannot make a single override target look ambiguous.
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    if (!candidates[index].dataShadowAddress)
      continue;
    for (std::size_t other = candidates.size(); other-- > index + 1;) {
      if (candidates[other].originalReference != candidates[index].originalReference ||
          candidates[other].specIndex != candidates[index].specIndex)
        continue;
      if (candidates[other].score > candidates[index].score)
        candidates[index] = candidates[other];
      candidates.erase(candidates.begin() + static_cast<std::ptrdiff_t>(other));
    }
  }

  bool installedAny = false;
  for (const LiteralReference& literal : literals) {
    const std::size_t specIndex = literal.specIndex;
    if (g_activeModules[specIndex].load(std::memory_order_acquire))
      continue;
    Candidate* bestCandidate = nullptr;
    int bestScore = -1;
    int secondBestScore = -1;
    std::size_t matchCount = 0;
    Candidate* bestPointerCandidate = nullptr;
    int pointerBest = -1;
    int pointerSecond = -1;
    Candidate* bestDataShadowCandidate = nullptr;
    int dataShadowBest = -1;
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
      if (candidate.dataShadowAddress) {
        if (candidate.score > dataShadowBest) {
          dataShadowBest = candidate.score;
          bestDataShadowCandidate = &candidate;
        }
      } else if (candidate.score > pointerBest) {
        pointerSecond = pointerBest;
        pointerBest = candidate.score;
        bestPointerCandidate = &candidate;
      } else if (candidate.score > pointerSecond) {
        pointerSecond = candidate.score;
      }
    }
    // A validated pointer-model variable is the proven high-confidence target;
    // data-pointer duplicates of the same name must not make it look ambiguous.
    // UE 5.6 can register one CVar from several sites that each own a separate
    // value record, so when no pointer-model candidate exists, overriding the
    // best-correlated data-pointer variable is a safe best effort: the hard
    // layout and plausibility checks already passed, and an ineffective
    // duplicate simply means the engine reads another copy of the same name.
    const bool accepted = pointerBest >= 0
                              ? ce::ue5_rr::ShouldAcceptCandidate(pointerBest, pointerSecond)
                              : dataShadowBest >= ce::ue5_rr::kMinimumAcceptableScore;
    if (pointerBest >= 0 && accepted)
      bestCandidate = bestPointerCandidate;
    else if (pointerBest < 0 && accepted)
      bestCandidate = bestDataShadowCandidate;
    if (!accepted) {
      static std::atomic<uint32_t> validationLogs{0};
      if (validationLogs.fetch_add(1, std::memory_order_relaxed) < 20) {
        LogUnvalidatedCandidate(image, bestRawCandidates[specIndex], specIndex,
                                hasRawCandidate[specIndex], matchCount, bestScore, secondBestScore);
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

void ClearPendingModules() {
  for (auto& slot : g_pendingModules)
    slot.store(nullptr, std::memory_order_release);
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
  std::vector<uint8_t> seenLiterals(kCVarCount, 0);
  HMODULE mainModule = GetModuleHandleW(nullptr);
  if (mainModule)
    installedAny |= ScanModule(mainModule, &seenLiterals);
  for (HMODULE module : modules) {
    if (module != mainModule)
      installedAny |= ScanModule(module, &seenLiterals);
  }
  ClearPendingModules();

  // Names UE composes at runtime have no literal to find, and a per-title
  // object layout can leave a found literal unresolved. Both get one more
  // chance through the engine's own registry before the summary is written.
  installedAny |= ResolveMissingThroughConsoleRegistry();

  const std::size_t desired = DesiredCount();
  const std::size_t active = ActiveCount();
  if (active < desired && !g_missingSummaryLogged) {
    g_missingSummaryLogged = true;
    std::string missingLiterals;
    std::string missingCandidates;
    for (std::size_t index = 0; index < kCVarCount; ++index) {
      if (!g_desired[index].enabled || g_activeModules[index].load(std::memory_order_acquire))
        continue;
      std::string& missing = seenLiterals[index] ? missingCandidates : missingLiterals;
      if (missing.size() >= 480) {
        if (missing.size() < 484)
          missing += "...";
      } else {
        if (!missing.empty())
          missing += ", ";
        missing += ce::ue5_cvar::kSpecs[index].name;
      }
    }
    std::string detail;
    if (!missingLiterals.empty())
      detail += "; literals not found in loaded modules: " + missingLiterals;
    if (!missingCandidates.empty())
      detail += "; found but no validated CVar object: " + missingCandidates;
    HookLogImportant(
        "UE5 overrides: installed %zu/%zu requested persistent CVars across %zu modules in %llums%s",
        active, desired, modules.size(), static_cast<unsigned long long>(GetTickCount64() - start),
        detail.c_str());
  }
  return installedAny;
}

bool ScanPendingModules() {
  bool installedAny = false;
  for (auto& slot : g_pendingModules) {
    HMODULE module = slot.exchange(nullptr, std::memory_order_acq_rel);
    if (module)
      installedAny |= ScanModule(module, nullptr);
  }
  return installedAny;
}

}  // namespace UE5::detail
