#include "resize_reference_holders.h"

#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "../../common/module_enumeration.h"
#include "hook_common.h"

namespace {

namespace holders = ce::resize_reference_holders;

constexpr size_t kChunkBytes = 1u << 20;
constexpr size_t kRecordedHits = 64;
constexpr size_t kLoggedHits = 32;

struct ModuleName {
    char text[64] = "?";
};

// Every image currently mapped, sorted by base, and its base name for the log.
void SnapshotImages(std::vector<holders::ImageRange>& images, std::vector<ModuleName>& names) {
    std::vector<HMODULE> modules;
    if (!ce::EnumerateProcessModules(GetCurrentProcess(), modules)) {
        return;
    }
    for (HMODULE module : modules) {
        MODULEINFO info = {};
        if (!GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)) || info.SizeOfImage == 0) {
            continue;
        }
        ModuleName name;
        wchar_t path[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
        if (length != 0 && length < MAX_PATH) {
            const wchar_t* base = wcsrchr(path, L'\\');
            base = base ? base + 1 : path;
            WideCharToMultiByte(CP_UTF8, 0, base, -1, name.text, static_cast<int>(sizeof(name.text)), nullptr,
                                nullptr);
            name.text[sizeof(name.text) - 1] = '\0';
        }
        holders::ImageRange range;
        range.base = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
        range.end = range.base + info.SizeOfImage;
        range.index = static_cast<uint32_t>(names.size());
        images.push_back(range);
        names.push_back(name);
    }
    std::sort(images.begin(), images.end(),
              [](const holders::ImageRange& left, const holders::ImageRange& right) { return left.base < right.base; });
}

bool Overlaps(uintptr_t begin, uintptr_t end, uintptr_t otherBegin, uintptr_t otherEnd) {
    return begin < otherEnd && otherBegin < end;
}

}  // namespace

size_t LogBackBufferReferenceHolders(void* const* buffers, UINT bufferCount, const char* source) {
    if (!buffers || bufferCount == 0) {
        return 0;
    }
    const size_t targetCount = (std::min)(static_cast<size_t>(bufferCount), holders::kMaxTargets);

    LARGE_INTEGER frequency = {};
    LARGE_INTEGER started = {};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&started);

    std::vector<holders::ImageRange> images;
    std::vector<ModuleName> names;
    SnapshotImages(images, names);

    // The searched values and the copy buffer share one allocation that the
    // walk skips, so the scan never finds its own working set.
    std::vector<uintptr_t> storage(holders::kMaxTargets + (kChunkBytes + holders::kOwnerLookBackBytes) /
                                                              sizeof(uintptr_t));
    uintptr_t* targets = storage.data();
    for (size_t i = 0; i < targetCount; ++i) {
        targets[i] = reinterpret_cast<uintptr_t>(buffers[i]);
    }
    auto* chunk = reinterpret_cast<uint8_t*>(storage.data() + holders::kMaxTargets);
    const uintptr_t storageBegin = reinterpret_cast<uintptr_t>(storage.data());
    const uintptr_t storageEnd = storageBegin + storage.size() * sizeof(uintptr_t);

    ULONG_PTR stackLow = 0;
    ULONG_PTR stackHigh = 0;
    GetCurrentThreadStackLimits(&stackLow, &stackHigh);

    SYSTEM_INFO system = {};
    GetSystemInfo(&system);
    const uintptr_t lowest = reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress);
    const uintptr_t highest = reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress);

    holders::Hit hits[kRecordedHits];
    size_t recorded = 0;
    size_t total = 0;
    unsigned long long bytesScanned = 0;
    unsigned long regionsScanned = 0;
    bool budgetExhausted = false;
    const auto elapsedUs = [&]() {
        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);
        return frequency.QuadPart > 0 ? (now.QuadPart - started.QuadPart) * 1'000'000 / frequency.QuadPart : 0;
    };
    for (uintptr_t address = lowest; address < highest && !budgetExhausted;) {
        MEMORY_BASIC_INFORMATION region = {};
        if (VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) == 0) {
            break;
        }
        const uintptr_t regionBase = reinterpret_cast<uintptr_t>(region.BaseAddress);
        const uintptr_t regionEnd = regionBase + region.RegionSize;
        address = regionEnd > address ? regionEnd : address + system.dwPageSize;
        if (!holders::IsScannableRegion(region.State, region.Protect, region.Type)) {
            continue;
        }
        ++regionsScanned;
        for (uintptr_t chunkStart = regionBase; chunkStart < regionEnd; chunkStart += kChunkBytes) {
            if (holders::IsScanBudgetExhausted(elapsedUs())) {
                budgetExhausted = true;
                break;
            }
            const uintptr_t readStart = (std::max)(regionBase, chunkStart >= holders::kOwnerLookBackBytes
                                                                   ? chunkStart - holders::kOwnerLookBackBytes
                                                                   : regionBase);
            const uintptr_t readEnd = (std::min)(regionEnd, chunkStart + kChunkBytes);
            if (Overlaps(readStart, readEnd, storageBegin, storageEnd)) {
                continue;
            }
            // ReadProcessMemory on this process returns a partial copy instead
            // of faulting when another thread decommits the range meanwhile.
            SIZE_T read = 0;
            ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(readStart), chunk,
                              static_cast<SIZE_T>(readEnd - readStart), &read);
            if (read == 0) {
                continue;
            }
            bytesScanned += read;
            const size_t found =
                holders::ScanBlock(chunk, static_cast<size_t>(read), readStart, chunkStart, targets, targetCount,
                                   images.data(), images.size(), hits + recorded, kRecordedHits - recorded);
            recorded += (std::min)(found, kRecordedHits - recorded);
            total += found;
        }
    }

    LARGE_INTEGER finished = {};
    QueryPerformanceCounter(&finished);
    const double elapsedMs =
        frequency.QuadPart > 0 ? 1000.0 * static_cast<double>(finished.QuadPart - started.QuadPart) /
                                     static_cast<double>(frequency.QuadPart)
                               : 0.0;

    char bufferList[256] = {};
    size_t used = 0;
    for (size_t i = 0; i < targetCount && used < sizeof(bufferList); ++i) {
        const int written = snprintf(bufferList + used, sizeof(bufferList) - used, i == 0 ? "%p" : ",%p",
                                     reinterpret_cast<void*>(targets[i]));
        if (written <= 0) {
            break;
        }
        used += static_cast<size_t>(written);
    }
    HookLogImportant(
        "ResizeReferenceHolders: %s scanned %.1f MB in %lu writable region(s) in %.0f ms for bb=[%s]%s - "
        "slots=%zu (listed %zu). Owner = nearest code-image pointer in front of the slot (the object's vtable, "
        "or a return address on a stack); dxgi/d3d12 owners include the chain's own bookkeeping",
        source ? source : "resize", static_cast<double>(bytesScanned) / (1024.0 * 1024.0), regionsScanned, elapsedMs,
        bufferList, budgetExhausted ? " (CUT at the time budget; later addresses were not scanned)" : "", total,
        (std::min)(recorded, kLoggedHits));

    std::map<std::string, unsigned> owners;
    for (size_t i = 0; i < recorded; ++i) {
        const holders::Hit& hit = hits[i];
        const holders::ImageRange* slotImage = holders::FindImage(hit.address, images.data(), images.size());
        const bool onThisStack = hit.address >= stackLow && hit.address < stackHigh;
        const char* ownerName = hit.ownerFound ? names[hit.ownerImage].text : "none";
        std::string ownerKey = slotImage ? std::string("global:") + names[slotImage->index].text
                                         : (onThisStack ? std::string("resizing-thread-stack") : ownerName);
        ++owners[ownerKey];
        if (i >= kLoggedHits) {
            continue;
        }
        if (slotImage) {
            HookLogImportant("ResizeReferenceHolders: bb%u slot=%p global %s+0x%llX", hit.target,
                             reinterpret_cast<void*>(hit.address), names[slotImage->index].text,
                             static_cast<unsigned long long>(hit.address - slotImage->base));
        } else {
            HookLogImportant("ResizeReferenceHolders: bb%u slot=%p %s owner=%s+0x%llX at -%u", hit.target,
                             reinterpret_cast<void*>(hit.address), onThisStack ? "resizing-thread-stack" : "private",
                             ownerName, static_cast<unsigned long long>(hit.ownerRva), hit.ownerDistance);
        }
    }
    std::string summary;
    for (const auto& [owner, count] : owners) {
        char entry[112];
        snprintf(entry, sizeof(entry), "%s%s=%u", summary.empty() ? "" : " ", owner.c_str(), count);
        summary += entry;
    }
    HookLogImportant("ResizeReferenceHolders: slots by owner: %s", summary.empty() ? "none" : summary.c_str());
    return total;
}
