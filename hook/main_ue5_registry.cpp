// Resolving UE console variables through UE's own console-object registry.
//
// The module scanner can only reach a CVar whose exact name is a compiled-in
// UTF-16 literal. UE 5.4/5.6 composes `ShowFlag.<Name>` at runtime, so those
// variables have no literal to match and stay unreachable no matter how the
// candidate scoring is tuned; per-title object layouts can also defeat
// candidate validation for a name that does have a literal.
//
// Both are discovery problems, and UE already keeps the answer in memory:
// `FConsoleManager` maps every registered name to its `IConsoleObject*`. This
// unit locates that map without calling into the engine and without assuming
// a UE map layout - it takes CVars CE already installed as anchors, finds an
// element that pairs a known name with a known object, derives the key-to-
// value distance from it, and only then resolves the missing names from the
// same allocation.
#include "main_ue5_internal.h"

#include "common/ue5_console_registry.h"

namespace UE5::detail {
namespace {

constexpr std::size_t kChunkBytes = 64 * 1024;
// Every element CE reads spans at most a key header plus the largest accepted
// value distance, so chunks overlap by that much and no element is missed.
constexpr std::size_t kChunkOverlap = 64;
constexpr ULONGLONG kScanBudgetMs = 400;
constexpr uint64_t kScanBudgetBytes = 768ull << 20;
// Locating the map walks the heap and is bounded hard. Re-reading a map that
// is already located is cheap, and it has to be retried for a while: UE
// registers the ShowFlag variables during world/graphics init, long after the
// first module scan.
constexpr uint32_t kMaxSearchAttempts = 4;
constexpr uint32_t kMaxResolveAttempts = 90;

uint32_t g_searchAttempts = 0;
uint32_t g_resolveAttempts = 0;
bool g_exhausted = false;

struct Anchor {
  uintptr_t object = 0;
  const char* name = nullptr;
};

struct Region {
  uintptr_t base = 0;
  std::size_t size = 0;
};

// Every read goes through ReadProcessMemory rather than a raw dereference: a
// live game frees heap regions while this walk runs, and the kernel-checked
// copy fails cleanly instead of raising an access violation.
bool ReadBytes(uintptr_t address, void* buffer, std::size_t size) {
  SIZE_T copied = 0;
  return address != 0 &&
         ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), buffer, size, &copied) &&
         copied == size;
}

std::vector<Anchor> CollectAnchors() {
  std::vector<Anchor> anchors;
  for (std::size_t index = 0; index < kCVarCount; ++index) {
    if (!g_activeModules[index].load(std::memory_order_acquire))
      continue;
    const OverrideState& state = g_overrides[index];
    if (state.registryResolved)
      continue;
    uintptr_t consoleObject = 0;
    if (state.dataShadowAddress && state.dataShadowPointerRedirect) {
      consoleObject = state.dataShadowAddress - kRefDataPointerOffset;
    } else if (state.object) {
      void* target = nullptr;
      if (ReadValue(reinterpret_cast<const void*>(state.object + sizeof(void*)), target))
        consoleObject = reinterpret_cast<uintptr_t>(target);
    }
    if (ce::ue5_registry::IsPlausibleConsoleObject(consoleObject))
      anchors.push_back({consoleObject, ce::ue5_cvar::kSpecs[index].name});
  }
  std::sort(anchors.begin(), anchors.end(),
            [](const Anchor& left, const Anchor& right) { return left.object < right.object; });
  return anchors;
}

std::vector<std::size_t> CollectMissingSpecs() {
  std::vector<std::size_t> missing;
  for (std::size_t index = 0; index < kCVarCount; ++index) {
    if (g_desired[index].enabled && !g_activeModules[index].load(std::memory_order_acquire))
      missing.push_back(index);
  }
  return missing;
}

std::vector<Region> CollectHeapRegions() {
  std::vector<Region> regions;
  MEMORY_BASIC_INFORMATION memory{};
  uintptr_t cursor = 0;
  while (VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &memory, sizeof(memory)) == sizeof(memory)) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(memory.BaseAddress);
    const std::size_t size = static_cast<std::size_t>(memory.RegionSize);
    if (size == 0 || base > (std::numeric_limits<uintptr_t>::max)() - size)
      break;
    if (memory.State == MEM_COMMIT && memory.Type == MEM_PRIVATE &&
        (memory.Protect & 0xFF) == PAGE_READWRITE && !(memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
      regions.push_back({base, size});
    }
    cursor = base + size;
  }
  return regions;
}

// A one-byte presence table over anchor addresses turns the hot compare into
// a single indexed load, so the bulk walk stays linear in practice.
struct AnchorFilter {
  uintptr_t low = 0;
  uintptr_t high = 0;
  std::array<bool, 256> present{};

  explicit AnchorFilter(const std::vector<Anchor>& anchors) {
    low = anchors.front().object;
    high = anchors.back().object;
    for (const Anchor& anchor : anchors)
      present[static_cast<uint8_t>((anchor.object >> 4) & 0xFF)] = true;
  }

  bool MayContain(uintptr_t value) const {
    return value >= low && value <= high && present[static_cast<uint8_t>((value >> 4) & 0xFF)];
  }
};

const Anchor* FindAnchor(const std::vector<Anchor>& anchors, uintptr_t value) {
  auto found = std::lower_bound(anchors.begin(), anchors.end(), value,
                                [](const Anchor& item, uintptr_t key) { return item.object < key; });
  return found != anchors.end() && found->object == value ? &*found : nullptr;
}

bool ReadStringHeader(uintptr_t address, ce::ue5_registry::StringHeader& header) {
  struct RawHeader {
    uint64_t data;
    int32_t num;
    int32_t max;
  } raw{};
  if (!ReadBytes(address, &raw, sizeof(raw)))
    return false;
  header.data = static_cast<uintptr_t>(raw.data);
  header.num = raw.num;
  header.max = raw.max;
  return ce::ue5_registry::IsPlausibleStringHeader(header);
}

bool KeyMatchesName(const ce::ue5_registry::StringHeader& header, const char* name) {
  uint8_t text[(ce::ue5_registry::kMaxNameLength + 1) * 2];
  const std::size_t bytes = static_cast<std::size_t>(header.num) * 2;
  if (bytes > sizeof(text) || !ReadBytes(header.data, text, bytes))
    return false;
  return ce::ue5_registry::MatchesName(text, bytes, header, name);
}

// Confirms the element layout: an element whose key is a name CE already
// resolved and whose value is that CVar's object proves the key-to-value
// distance for every other element in the same allocation.
bool ConfirmAnchorElement(uintptr_t valueAddress, const Anchor& anchor, std::size_t& valueOffset) {
  for (std::size_t offset : ce::ue5_registry::kCandidateValueOffsets) {
    if (valueAddress < offset)
      continue;
    ce::ue5_registry::StringHeader header;
    if (!ReadStringHeader(valueAddress - offset, header))
      continue;
    if (!KeyMatchesName(header, anchor.name))
      continue;
    valueOffset = offset;
    return true;
  }
  return false;
}

struct AnchorHit {
  bool valid = false;
  Region region;
  std::size_t valueOffset = 0;
  const char* name = nullptr;
  uintptr_t valueAddress = 0;
  uintptr_t object = 0;
};

AnchorHit g_anchor;

// UE reallocates the map's element storage when it grows, which moves every
// element. Re-reading the element the search confirmed distinguishes "the map
// moved, search again" from "these names are simply not registered yet".
bool AnchorStillHolds(const AnchorHit& hit) {
  uintptr_t object = 0;
  if (!ReadBytes(hit.valueAddress, &object, sizeof(object)) || object != hit.object)
    return false;
  ce::ue5_registry::StringHeader header;
  return ReadStringHeader(hit.valueAddress - hit.valueOffset, header) && KeyMatchesName(header, hit.name);
}

bool FindRegistryRegion(const std::vector<Anchor>& anchors, AnchorHit& hit, uint64_t& scannedBytes,
                        ULONGLONG deadline) {
  const AnchorFilter filter(anchors);
  std::vector<uint8_t> buffer(kChunkBytes);
  for (const Region& region : CollectHeapRegions()) {
    for (std::size_t offset = 0; offset < region.size; offset += kChunkBytes - kChunkOverlap) {
      if (scannedBytes >= kScanBudgetBytes || GetTickCount64() >= deadline)
        return false;
      const std::size_t bytes = (std::min)(kChunkBytes, region.size - offset);
      if (bytes < sizeof(uintptr_t))
        break;
      if (!ReadBytes(region.base + offset, buffer.data(), bytes))
        continue;
      scannedBytes += bytes;
      const std::size_t slots = bytes / sizeof(uintptr_t);
      const auto* words = reinterpret_cast<const uintptr_t*>(buffer.data());
      for (std::size_t slot = 0; slot < slots; ++slot) {
        if (!filter.MayContain(words[slot]))
          continue;
        const Anchor* anchor = FindAnchor(anchors, words[slot]);
        if (!anchor)
          continue;
        const uintptr_t valueAddress = region.base + offset + slot * sizeof(uintptr_t);
        std::size_t valueOffset = 0;
        if (!ConfirmAnchorElement(valueAddress, *anchor, valueOffset))
          continue;
        hit.valid = true;
        hit.region = region;
        hit.valueOffset = valueOffset;
        hit.name = anchor->name;
        hit.valueAddress = valueAddress;
        hit.object = anchor->object;
        return true;
      }
    }
  }
  return false;
}

// Second pass over the one confirmed allocation: find the key of each missing
// name and read its object from the distance the anchor proved.
std::size_t ResolveMissingInRegion(const AnchorHit& hit, const std::vector<std::size_t>& missing,
                                   HMODULE owner) {
  std::array<int32_t, kCVarCount> wantedNum{};
  for (std::size_t index : missing)
    wantedNum[index] = static_cast<int32_t>(std::strlen(ce::ue5_cvar::kSpecs[index].name)) + 1;

  std::vector<uint8_t> buffer(kChunkBytes);
  std::size_t installed = 0;
  for (std::size_t offset = 0; offset < hit.region.size; offset += kChunkBytes - kChunkOverlap) {
    const std::size_t bytes = (std::min)(kChunkBytes, hit.region.size - offset);
    if (bytes < sizeof(uintptr_t) * 2 + hit.valueOffset)
      break;
    if (!ReadBytes(hit.region.base + offset, buffer.data(), bytes))
      continue;
    const std::size_t limit = bytes - sizeof(uintptr_t) * 2;
    for (std::size_t position = 0; position <= limit; position += sizeof(uintptr_t)) {
      ce::ue5_registry::StringHeader header;
      std::memcpy(&header.data, buffer.data() + position, sizeof(uint64_t));
      std::memcpy(&header.num, buffer.data() + position + 8, sizeof(int32_t));
      std::memcpy(&header.max, buffer.data() + position + 12, sizeof(int32_t));
      if (!ce::ue5_registry::IsPlausibleStringHeader(header))
        continue;
      for (std::size_t index : missing) {
        if (g_activeModules[index].load(std::memory_order_acquire) || header.num != wantedNum[index])
          continue;
        if (!KeyMatchesName(header, ce::ue5_cvar::kSpecs[index].name))
          continue;
        uintptr_t object = 0;
        const uintptr_t keyAddress = hit.region.base + offset + position;
        if (!ReadBytes(keyAddress + hit.valueOffset, &object, sizeof(object)))
          continue;
        if (!ce::ue5_registry::IsPlausibleConsoleObject(object) ||
            !HasCallableVtable(reinterpret_cast<const void*>(object))) {
          HookLogImportant(
              "UE5 overrides: console registry holds %s but its object slot is not a console object "
              "(key=%p object=%p); leaving game memory unchanged",
              ce::ue5_cvar::kSpecs[index].name, reinterpret_cast<void*>(keyAddress),
              reinterpret_cast<void*>(object));
          continue;
        }
        if (InstallConsoleObjectRedirect(index, owner, object, "console registry"))
          ++installed;
      }
    }
  }
  return installed;
}

}  // namespace

void ResetConsoleRegistry() {
  g_searchAttempts = 0;
  g_resolveAttempts = 0;
  g_exhausted = false;
  g_anchor = {};
}

bool ResolveMissingThroughConsoleRegistry() {
  if (g_exhausted)
    return false;
  const std::vector<std::size_t> missing = CollectMissingSpecs();
  if (missing.empty())
    return false;
  const std::vector<Anchor> anchors = CollectAnchors();
  if (anchors.empty())
    return false;  // Nothing proven yet; the module scan has to land first.

  if (g_anchor.valid && !AnchorStillHolds(g_anchor)) {
    HookLogImportant("UE5 overrides: console-registry element storage moved; re-locating it");
    g_anchor = {};
  }

  if (!g_anchor.valid) {
    if (g_searchAttempts >= kMaxSearchAttempts) {
      g_exhausted = true;
      HookLogImportant("UE5 overrides: console registry could not be located in %u attempt(s); "
                       "%zu CVar(s) stay unresolved",
                       kMaxSearchAttempts, missing.size());
      return false;
    }
    ++g_searchAttempts;
    const ULONGLONG start = GetTickCount64();
    uint64_t scannedBytes = 0;
    AnchorHit hit;
    if (!FindRegistryRegion(anchors, hit, scannedBytes, start + kScanBudgetMs)) {
      HookLogImportant(
          "UE5 overrides: console registry not located (attempt %u/%u, %llu MB scanned in %llums, "
          "%zu anchor(s)); keeping the module-scan results",
          g_searchAttempts, kMaxSearchAttempts, static_cast<unsigned long long>(scannedBytes >> 20),
          static_cast<unsigned long long>(GetTickCount64() - start), anchors.size());
      return false;
    }
    g_anchor = hit;
    HookLogImportant(
        "UE5 overrides: console registry anchored on %s (valueOffset=%zu region=%p+0x%llX) after "
        "%llu MB in %llums",
        hit.name, hit.valueOffset, reinterpret_cast<void*>(hit.region.base),
        static_cast<unsigned long long>(hit.region.size),
        static_cast<unsigned long long>(scannedBytes >> 20),
        static_cast<unsigned long long>(GetTickCount64() - start));
  }

  const std::size_t installed = ResolveMissingInRegion(g_anchor, missing, GetModuleHandleW(nullptr));
  if (installed) {
    HookLogImportant("UE5 overrides: console registry resolved %zu of %zu remaining CVar(s)",
                     installed, missing.size());
    return true;
  }
  if (++g_resolveAttempts >= kMaxResolveAttempts) {
    g_exhausted = true;
    HookLogImportant(
        "UE5 overrides: %zu CVar(s) are still absent from the located console registry after %u "
        "attempt(s); the engine never registered them",
        missing.size(), kMaxResolveAttempts);
  }
  return false;
}

}  // namespace UE5::detail
