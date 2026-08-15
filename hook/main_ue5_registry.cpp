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
// Per pass. The sweep resumes at the chunk it did not reach, so running out of
// time here costs latency, never coverage.
constexpr ULONGLONG kScanBudgetMs = 400;
constexpr uint64_t kScanBudgetBytes = 768ull << 20;
// Cumulative bounds for the whole sweep. It runs at roughly 1 Hz from the hook
// service thread, so a title whose heap cannot be covered within these gives up
// and says so, rather than sweeping for the lifetime of the session.
constexpr uint32_t kMaxSweepPasses = 32;
constexpr uint64_t kMaxSweepBytes = 8ull << 30;
// A discrete TMap element allocation is far below this. Exceeding it means the
// AllocationBase is a shared pool rather than the map's own block, which proves
// nothing about the map, so the expansion is abandoned instead of scanned.
constexpr uint64_t kMaxAllocationExpansionBytes = 256ull << 20;
// Re-reading the recorded regions is the cheap half, but the region set grows
// as the sweep proceeds, so it is time-boxed too.
constexpr ULONGLONG kResolveBudgetMs = 100;
// Retried for a while: UE registers the ShowFlag variables during world and
// graphics init, long after the first module scan.
constexpr uint32_t kMaxResolveAttempts = 90;

uint32_t g_resolveAttempts = 0;
bool g_exhausted = false;
ce::ue5_registry::SweepProgress g_sweep;
// Set once the owning allocation refused to be bounded, so the expensive
// enumeration is not retried every second for the rest of the session.
bool g_allocationExpansionRefused = false;
// Confirmed anchors are tracked by object address rather than by index into the
// anchor list: that list is rebuilt on every pass and grows as more CVars
// install, so an index would name a different anchor after the sweep resumes.
std::vector<uintptr_t> g_confirmedAnchors;
// Region the next resolve pass starts at, so a resolve budget that expires in
// the same place cannot starve every region behind it.
std::size_t g_resolveRegionCursor = 0;
// A CVar the registry located but could not install is a permanent per-title
// layout fact, not a transient miss. Without this it is re-found, re-refused
// and re-logged on every retry: the 20260815_210850 session carried 91 copies
// of the same r.Lumen.ScreenProbeGather.Temporal.MaxFramesAccumulated line.
std::array<bool, kCVarCount> g_registryRefused{};

struct Anchor {
  uintptr_t object = 0;
  const char* name = nullptr;
};

struct Region {
  uintptr_t base = 0;
  std::size_t size = 0;
  // VirtualQuery splits one allocation wherever protection or state changes, so
  // this is what ties the pieces of the map's storage back together.
  uintptr_t allocationBase = 0;
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

// Enumerates from `fromAddress` rather than from zero. A resumed pass has no
// use for anything below its cursor, and re-walking it is not free: VirtualQuery
// visits every region in the address space, reserved ones included, and a loaded
// UE5 title has a great many. Industria 2 (20260816_011313) covered 211 MB in
// the first 400 ms pass and then only 4-8 MB per pass once the level had loaded
// and the address space had grown - the scan budget was being spent re-reaching
// the cursor instead of advancing past it.
std::vector<Region> CollectHeapRegions(uintptr_t fromAddress) {
  std::vector<Region> regions;
  MEMORY_BASIC_INFORMATION memory{};
  uintptr_t cursor = fromAddress;
  while (VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &memory, sizeof(memory)) == sizeof(memory)) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(memory.BaseAddress);
    const std::size_t size = static_cast<std::size_t>(memory.RegionSize);
    if (size == 0 || base > (std::numeric_limits<uintptr_t>::max)() - size)
      break;
    if (memory.State == MEM_COMMIT && memory.Type == MEM_PRIVATE &&
        (memory.Protect & 0xFF) == PAGE_READWRITE && !(memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
      regions.push_back({base, size, reinterpret_cast<uintptr_t>(memory.AllocationBase)});
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

// Every region proven to hold map elements, not just the first one found. A
// UE console map does not fit in one 64 KB heap region, and stopping at the
// first hit searched a fraction of it: the 20260815_202743 Talos session
// anchored on r.SceneColorFringeQuality and then failed to find
// r.Lumen.ScreenProbeGather.Temporal.MaxFramesAccumulated, which is certainly
// registered - the element simply lived outside that one region.
struct RegistryMap {
  bool valid = false;
  std::size_t valueOffset = 0;
  std::vector<Region> regions;
  // One representative element, re-checked each pass so a TMap growth that
  // relocates the storage triggers a fresh search instead of silent misses.
  const char* name = nullptr;
  uintptr_t valueAddress = 0;
  uintptr_t object = 0;
};

RegistryMap g_map;

// Distinct anchors only: a CVar registered from several sites would otherwise
// inflate the confirmed count, and that count is what says whether the element
// set the resolve pass reads is the whole map or a corner of it.
void MarkAnchorConfirmed(uintptr_t object) {
  const auto at = std::lower_bound(g_confirmedAnchors.begin(), g_confirmedAnchors.end(), object);
  if (at == g_confirmedAnchors.end() || *at != object)
    g_confirmedAnchors.insert(at, object);
}

std::size_t CountConfirmedAnchors(const std::vector<Anchor>& anchors) {
  std::size_t confirmed = 0;
  for (const Anchor& anchor : anchors) {
    const auto at =
        std::lower_bound(g_confirmedAnchors.begin(), g_confirmedAnchors.end(), anchor.object);
    if (at != g_confirmedAnchors.end() && *at == anchor.object)
      ++confirmed;
  }
  return confirmed;
}

// UE reallocates the map's element storage when it grows, which moves every
// element. Re-reading the element the search confirmed distinguishes "the map
// moved, search again" from "these names are simply not registered yet".
bool AnchorStillHolds(const RegistryMap& map) {
  uintptr_t object = 0;
  if (!ReadBytes(map.valueAddress, &object, sizeof(object)) || object != map.object)
    return false;
  ce::ue5_registry::StringHeader header;
  return ReadStringHeader(map.valueAddress - map.valueOffset, header) && KeyMatchesName(header, map.name);
}

// One time-boxed pass of the sweep. It never restarts and never stops at the
// first hit: it walks on to the end of the heap enumeration across as many
// passes as that takes, because a region it did not read is a region that could
// hold the element the resolve pass is looking for - and because the closing
// verdict distinguishes "swept everywhere, absent" from "stopped early".
void SweepForRegistryMap(const std::vector<Anchor>& anchors, RegistryMap& map,
                         ce::ue5_registry::SweepProgress& progress, ULONGLONG deadline,
                         ULONGLONG& enumerateMs) {
  const AnchorFilter filter(anchors);
  std::vector<uint8_t> buffer(kChunkBytes);
  uint64_t passBytes = 0;
  ++progress.passes;
  const ULONGLONG enumerateStart = GetTickCount64();
  const std::vector<Region> regions = CollectHeapRegions(progress.cursor);
  enumerateMs = GetTickCount64() - enumerateStart;
  for (const Region& region : regions) {
    const ce::ue5_registry::RegionSpan span{region.base, region.size};
    if (ce::ue5_registry::RegionAlreadySwept(span, progress.cursor))
      continue;
    for (std::size_t offset = ce::ue5_registry::SweepResumeOffset(span, progress.cursor);
         offset < region.size; offset += kChunkBytes - kChunkOverlap) {
      if (passBytes >= kScanBudgetBytes || GetTickCount64() >= deadline) {
        // Park on the chunk that was not read, not on the one that was: the
        // next pass re-reads it whole, so nothing falls through the pause.
        progress.cursor = region.base + offset;
        return;
      }
      const std::size_t bytes = (std::min)(kChunkBytes, region.size - offset);
      if (bytes < sizeof(uintptr_t))
        break;
      if (!ReadBytes(region.base + offset, buffer.data(), bytes))
        continue;
      passBytes += bytes;
      progress.sweptBytes += bytes;
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
        if (!map.valid) {
          map.valid = true;
          map.valueOffset = valueOffset;
          map.name = anchor->name;
          map.valueAddress = valueAddress;
          map.object = anchor->object;
        } else if (valueOffset != map.valueOffset) {
          continue;  // A different layout is a different structure, not ours.
        }
        MarkAnchorConfirmed(anchor->object);
        if (map.regions.empty() || map.regions.back().base != region.base)
          map.regions.push_back(region);
      }
    }
    progress.cursor = region.base + region.size;
  }
  progress.complete = true;
}

// Second pass over every proven allocation: find the key of each missing name
// and read its object from the distance the anchors proved.
std::size_t ResolveMissingInRegion(const Region& region, std::size_t valueOffset,
                                   const std::vector<std::size_t>& missing, HMODULE owner) {
  std::array<int32_t, kCVarCount> wantedNum{};
  for (std::size_t index : missing)
    wantedNum[index] = static_cast<int32_t>(std::strlen(ce::ue5_cvar::kSpecs[index].name)) + 1;

  std::vector<uint8_t> buffer(kChunkBytes);
  std::size_t installed = 0;
  for (std::size_t offset = 0; offset < region.size; offset += kChunkBytes - kChunkOverlap) {
    const std::size_t bytes = (std::min)(kChunkBytes, region.size - offset);
    if (bytes < sizeof(uintptr_t) * 2 + valueOffset)
      break;
    if (!ReadBytes(region.base + offset, buffer.data(), bytes))
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
        if (g_activeModules[index].load(std::memory_order_acquire) || g_registryRefused[index] ||
            header.num != wantedNum[index])
          continue;
        if (!KeyMatchesName(header, ce::ue5_cvar::kSpecs[index].name))
          continue;
        uintptr_t object = 0;
        const uintptr_t keyAddress = region.base + offset + position;
        if (!ReadBytes(keyAddress + valueOffset, &object, sizeof(object)))
          continue;
        if (!ce::ue5_registry::IsPlausibleConsoleObject(object) ||
            !HasCallableVtable(reinterpret_cast<const void*>(object))) {
          g_registryRefused[index] = true;
          HookLogImportant(
              "UE5 overrides: console registry holds %s but its object slot is not a console object "
              "(key=%p object=%p); leaving game memory unchanged and not retrying it",
              ce::ue5_cvar::kSpecs[index].name, reinterpret_cast<void*>(keyAddress),
              reinterpret_cast<void*>(object));
          continue;
        }
        if (InstallConsoleObjectRedirect(index, owner, object, "console registry")) {
          ++installed;
        } else {
          // The object was located; its layout is simply not one CE can drive.
          // That verdict cannot change later, so stop re-attempting it.
          g_registryRefused[index] = true;
        }
      }
    }
  }
  return installed;
}

// Pulls in every remaining region of the allocation(s) the map's elements live
// in, which is what makes "this name is not registered" answerable cheaply.
//
// A TMap keeps its elements in one allocation; VirtualQuery merely reports it in
// pieces wherever protection or state changes, and all those pieces share
// AllocationBase. Covering that allocation therefore settles the question the
// whole-heap sweep was being asked to settle - at the cost of one enumeration
// instead of gigabytes. Fails closed: if the owning allocation turns out to be a
// large shared pool rather than a discrete block, nothing is claimed and the
// whole-heap sweep stays the fallback.
bool ExpandToMapAllocations(RegistryMap& map, uint64_t& addedBytes, std::size_t& addedRegions) {
  addedBytes = 0;
  addedRegions = 0;
  if (map.regions.empty())
    return false;
  std::vector<uintptr_t> bases;
  for (const Region& region : map.regions) {
    if (region.allocationBase == 0)
      return false;  // Nothing to reason about; leave the verdict unproven.
    const auto at = std::lower_bound(bases.begin(), bases.end(), region.allocationBase);
    if (at == bases.end() || *at != region.allocationBase)
      bases.insert(at, region.allocationBase);
  }
  std::vector<Region> added;
  for (const Region& region : CollectHeapRegions(0)) {
    const auto at = std::lower_bound(bases.begin(), bases.end(), region.allocationBase);
    if (at == bases.end() || *at != region.allocationBase)
      continue;
    bool known = false;
    for (const Region& existing : map.regions) {
      if (existing.base == region.base) {
        known = true;
        break;
      }
    }
    if (known)
      continue;
    addedBytes += region.size;
    if (addedBytes > kMaxAllocationExpansionBytes)
      return false;
    added.push_back(region);
  }
  addedRegions = added.size();
  map.regions.insert(map.regions.end(), added.begin(), added.end());
  return true;
}

// Every region the sweep proved, time-boxed and resumed by rotation. Regions
// here are the VirtualQuery spans holding confirmed elements, so this is the
// cheap half - but the sweep no longer stops at the first one, and a budget
// that always expired in the same region would starve the rest.
std::size_t ResolveMissingAcrossRegions(const std::vector<Region>& regions, std::size_t valueOffset,
                                        const std::vector<std::size_t>& missing, HMODULE owner,
                                        ULONGLONG deadline) {
  if (regions.empty())
    return 0;
  if (g_resolveRegionCursor >= regions.size())
    g_resolveRegionCursor = 0;
  std::size_t installed = 0;
  for (std::size_t visited = 0; visited < regions.size(); ++visited) {
    const std::size_t index = (g_resolveRegionCursor + visited) % regions.size();
    if (GetTickCount64() >= deadline) {
      g_resolveRegionCursor = index;
      return installed;
    }
    installed += ResolveMissingInRegion(regions[index], valueOffset, missing, owner);
  }
  g_resolveRegionCursor = 0;
  return installed;
}

}  // namespace

void ResetConsoleRegistry() {
  g_resolveAttempts = 0;
  g_exhausted = false;
  g_map = {};
  g_sweep = {};
  g_confirmedAnchors.clear();
  g_resolveRegionCursor = 0;
  g_allocationExpansionRefused = false;
  g_registryRefused.fill(false);
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

  if (g_map.valid && !AnchorStillHolds(g_map)) {
    HookLogImportant("UE5 overrides: console-registry element storage moved; re-locating it");
    g_map = {};
    g_sweep = {};
    g_confirmedAnchors.clear();
    g_resolveRegionCursor = 0;
    g_allocationExpansionRefused = false;
  }

  // Tried before the sweep: covering the map's own allocation answers the
  // absence question outright, and doing so stops the sweep from spending
  // further passes on heap that cannot contain the map.
  if (g_map.valid && !g_sweep.allocationCovered && !g_allocationExpansionRefused) {
    uint64_t addedBytes = 0;
    std::size_t addedRegions = 0;
    if (ExpandToMapAllocations(g_map, addedBytes, addedRegions)) {
      g_sweep.allocationCovered = true;
      HookLogImportant(
          "UE5 overrides: console-registry map allocation covered — %zu region(s) total (+%zu, %llu KB "
          "from the owning allocation); a name absent from these is genuinely unregistered, so the "
          "heap sweep stops here after %llu MB in %u pass(es)",
          g_map.regions.size(), addedRegions, static_cast<unsigned long long>(addedBytes >> 10),
          static_cast<unsigned long long>(g_sweep.sweptBytes >> 20), g_sweep.passes);
    } else {
      g_allocationExpansionRefused = true;
      HookLogImportant(
          "UE5 overrides: console-registry map allocation could not be bounded (%llu KB before the cap); "
          "falling back to the whole-heap sweep for coverage",
          static_cast<unsigned long long>(addedBytes >> 10));
    }
  }

  // The sweep keeps going after the first element is found. Locating the map
  // is only half of what the walk produces; the other half is the coverage
  // that lets the closing verdict below say "absent" instead of "not seen".
  if (ce::ue5_registry::SweepCanContinue(g_sweep, kMaxSweepPasses, kMaxSweepBytes)) {
    const bool wasValid = g_map.valid;
    const ULONGLONG start = GetTickCount64();
    const uint64_t before = g_sweep.sweptBytes;
    ULONGLONG enumerateMs = 0;
    SweepForRegistryMap(anchors, g_map, g_sweep, start + kScanBudgetMs, enumerateMs);
    const auto passMb = static_cast<unsigned long long>((g_sweep.sweptBytes - before) >> 20);
    const auto totalMb = static_cast<unsigned long long>(g_sweep.sweptBytes >> 20);
    if (!wasValid && g_map.valid) {
      HookLogImportant(
          "UE5 overrides: console registry anchored on %s (valueOffset=%zu, %zu/%zu anchor element(s) "
          "across %zu region(s), first=%p) after %llu MB in %llums",
          g_map.name, g_map.valueOffset, CountConfirmedAnchors(anchors), anchors.size(),
          g_map.regions.size(), reinterpret_cast<void*>(g_map.regions.front().base), passMb,
          static_cast<unsigned long long>(GetTickCount64() - start));
    }
    if (g_sweep.complete) {
      HookLogImportant(
          "UE5 overrides: console-registry sweep complete after %llu MB in %u pass(es) — "
          "%zu/%zu anchor element(s) across %zu region(s); a name absent from this sweep is "
          "genuinely unregistered",
          totalMb, g_sweep.passes, CountConfirmedAnchors(anchors), anchors.size(),
          g_map.regions.size());
    } else if (!ce::ue5_registry::SweepCanContinue(g_sweep, kMaxSweepPasses, kMaxSweepBytes)) {
      g_sweep.budgetExhausted = true;
      HookLogImportant(
          "UE5 overrides: console-registry sweep gave up after %llu MB in %u pass(es), parked at %p; "
          "coverage stays partial and cannot prove any name unregistered",
          totalMb, g_sweep.passes, reinterpret_cast<void*>(g_sweep.cursor));
    } else if (g_sweep.passes <= 4 || (g_sweep.passes % 8) == 0) {
      // enumerateMs separates "the heap is genuinely this big" from "the budget
      // went into re-reaching the cursor", which is what made passes 2-4 of
      // 20260816_011313 collapse to single-digit megabytes.
      HookLogImportant(
          "UE5 overrides: console-registry sweep paused at %p after pass %u (%llu MB this pass, "
          "%llu MB total, enumerate=%llums, %zu/%zu anchor element(s), %zu region(s)); resuming there",
          reinterpret_cast<void*>(g_sweep.cursor), g_sweep.passes, passMb, totalMb,
          static_cast<unsigned long long>(enumerateMs), CountConfirmedAnchors(anchors), anchors.size(),
          g_map.regions.size());
    }
  }

  if (!g_map.valid) {
    // Only a sweep that ran out of room to look has finished looking.
    if (!ce::ue5_registry::SweepCanContinue(g_sweep, kMaxSweepPasses, kMaxSweepBytes)) {
      g_exhausted = true;
      HookLogImportant("UE5 overrides: console registry could not be located in %llu MB across %u "
                       "pass(es); %zu CVar(s) stay unresolved",
                       static_cast<unsigned long long>(g_sweep.sweptBytes >> 20), g_sweep.passes,
                       missing.size());
    }
    return false;
  }

  const std::size_t installed =
      ResolveMissingAcrossRegions(g_map.regions, g_map.valueOffset, missing, GetModuleHandleW(nullptr),
                                  GetTickCount64() + kResolveBudgetMs);
  if (installed) {
    HookLogImportant("UE5 overrides: console registry resolved %zu of %zu remaining CVar(s)",
                     installed, missing.size());
    return true;
  }
  std::size_t refused = 0;
  for (std::size_t index : missing) {
    if (g_registryRefused[index])
      ++refused;
  }
  // Retrying only makes sense for names the engine might still register. Once
  // every remaining one has been located and refused, waiting cannot help.
  const bool nothingLeftToWaitFor = refused == missing.size();
  if (nothingLeftToWaitFor || ++g_resolveAttempts >= kMaxResolveAttempts) {
    g_exhausted = true;
    // "Not found" only becomes "not registered" behind a finished sweep. The
    // sweep is bounded and always terminal by the time the attempts run out, so
    // this reports which of the two it actually was instead of assuming.
    const std::size_t unseen = missing.size() - refused;
    if (ce::ue5_registry::SweepProvesAbsence(g_sweep)) {
      HookLogImportant(
          "UE5 overrides: console registry finished with %zu CVar(s) unresolved after %u attempt(s) — "
          "%zu located but carrying a layout CE cannot drive, %zu absent from the registry (%s, "
          "%zu region(s), %llu MB swept in %u pass(es))",
          missing.size(), g_resolveAttempts, refused, unseen,
          g_sweep.allocationCovered ? "owning allocation covered" : "whole heap swept",
          g_map.regions.size(), static_cast<unsigned long long>(g_sweep.sweptBytes >> 20),
          g_sweep.passes);
    } else {
      HookLogImportant(
          "UE5 overrides: console registry finished with %zu CVar(s) unresolved after %u attempt(s) — "
          "%zu located but carrying a layout CE cannot drive, %zu not found in the %llu MB swept "
          "before the sweep stopped at %p; those %zu are unproven, not known-absent",
          missing.size(), g_resolveAttempts, refused, unseen,
          static_cast<unsigned long long>(g_sweep.sweptBytes >> 20),
          reinterpret_cast<void*>(g_sweep.cursor), unseen);
    }
  }
  return false;
}

}  // namespace UE5::detail
