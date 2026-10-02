#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

// Who holds a back buffer that makes ResizeBuffers fail.
//
// DXGI refuses a resize with DXGI_ERROR_INVALID_CALL while anybody references a
// back buffer, and neither the error nor a dump says who. Talos Reawakened with
// FSR frame generation died of that three times on 2026-09-26 with three
// foreign references on each of its three buffers (resize_reference_probe.h),
// the last time with CE's capture not even bound to the chain; the stack-only
// dump had no heap and the external minidump no private memory, so the holder
// stayed anonymous.
//
// A holder keeps the buffer's interface pointer somewhere in memory. Scanning
// the process's writable memory for those pointer values finds each slot, and
// the object around a slot is recognized by the nearest code-image pointer in
// front of it - its vtable, which lies in the module that owns the object. A
// slot inside an image's own writable data is a global of that module. That
// names AMD's frame-generation proxy, Streamline, the engine, a driver or CE
// itself without symbols or a full-memory dump.
//
// This header is the pure part: one block of copied memory against a sorted
// image table. resize_reference_holders.cpp walks the address space.

namespace ce::resize_reference_holders {

// How far in front of a slot the owning object's vtable is looked for.
inline constexpr uint32_t kOwnerLookBackBytes = 512;
inline constexpr size_t kMaxTargets = 16;

struct ImageRange {
    uintptr_t base = 0;
    uintptr_t end = 0;  // exclusive
    uint32_t index = 0;  // caller's module index
};

struct Hit {
    uintptr_t address = 0;  // where the pointer value is stored
    uint32_t target = 0;    // index into the searched pointer values
    bool ownerFound = false;
    uint32_t ownerImage = 0;     // ImageRange::index of the nearest preceding code-image pointer
    uintptr_t ownerRva = 0;      // that pointer's value relative to its image
    uint32_t ownerDistance = 0;  // bytes from that pointer to the slot
};

// `images` sorted by base, non-overlapping.
inline const ImageRange* FindImage(uintptr_t value, const ImageRange* images, size_t imageCount) {
    size_t low = 0;
    size_t high = imageCount;
    while (low < high) {
        const size_t mid = low + (high - low) / 2;
        if (value < images[mid].base) {
            high = mid;
        } else if (value >= images[mid].end) {
            low = mid + 1;
        } else {
            return &images[mid];
        }
    }
    return nullptr;
}

// Scans `bytes`, a copy of [blockBase, blockBase + size), for pointer-aligned
// slots holding any of `targets`. Slots below `reportFrom` are only context for
// the owner search (the overlap a chunked caller re-reads). Returns the number
// of hits found, which can exceed `outCapacity`; only the first `outCapacity`
// are written.
inline size_t ScanBlock(const uint8_t* bytes, size_t size, uintptr_t blockBase, uintptr_t reportFrom,
                        const uintptr_t* targets, size_t targetCount, const ImageRange* images, size_t imageCount,
                        Hit* out, size_t outCapacity) {
    if (!bytes || !targets || targetCount == 0 || size < sizeof(uintptr_t)) {
        return 0;
    }
    size_t found = 0;
    const size_t firstSlot = reportFrom > blockBase ? static_cast<size_t>(reportFrom - blockBase) : 0;
    const size_t alignedFirst = (firstSlot + sizeof(uintptr_t) - 1) & ~(sizeof(uintptr_t) - 1);
    for (size_t offset = alignedFirst; offset + sizeof(uintptr_t) <= size; offset += sizeof(uintptr_t)) {
        uintptr_t value = 0;
        memcpy(&value, bytes + offset, sizeof(value));
        size_t target = targetCount;
        for (size_t t = 0; t < targetCount; ++t) {
            if (value == targets[t] && value != 0) {
                target = t;
                break;
            }
        }
        if (target == targetCount) {
            continue;
        }
        if (found < outCapacity && out) {
            Hit& hit = out[found];
            hit = Hit{};
            hit.address = blockBase + offset;
            hit.target = static_cast<uint32_t>(target);
            const size_t lookBackLimit = offset < kOwnerLookBackBytes ? offset : kOwnerLookBackBytes;
            for (size_t back = sizeof(uintptr_t); back <= lookBackLimit; back += sizeof(uintptr_t)) {
                uintptr_t candidate = 0;
                memcpy(&candidate, bytes + offset - back, sizeof(candidate));
                if (const ImageRange* image = FindImage(candidate, images, imageCount)) {
                    hit.ownerFound = true;
                    hit.ownerImage = image->index;
                    hit.ownerRva = candidate - image->base;
                    hit.ownerDistance = static_cast<uint32_t>(back);
                    break;
                }
            }
        }
        ++found;
    }
    return found;
}

// Memory the scan reads: committed, readable, writable (a holder writes its
// slot), not a guard page, and never uncached or write-combined - reading a
// write-combined GPU upload mapping from the CPU crawls and proves nothing.
inline bool IsScannableRegion(DWORD state, DWORD protect, DWORD type) {
    if (state != MEM_COMMIT || (type != MEM_PRIVATE && type != MEM_IMAGE)) {
        return false;
    }
    if ((protect & (PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE)) != 0) {
        return false;
    }
    const DWORD access = protect & 0xFF;
    return access == PAGE_READWRITE || access == PAGE_WRITECOPY || access == PAGE_EXECUTE_READWRITE ||
           access == PAGE_EXECUTE_WRITECOPY;
}

// The scan runs on the thread of a game that is already failing its resize, and
// its cost grows with the process's committed writable memory. A second or two
// is normal; a process with tens of gigabytes must not be held for minutes for
// a diagnostic, so the walk stops at this budget and reports that it was cut.
inline constexpr int64_t kScanBudgetUs = 3'000'000;

inline bool IsScanBudgetExhausted(int64_t elapsedUs) {
    return elapsedUs >= kScanBudgetUs;
}

// One-shot per process: the scan reads every writable page once, which costs
// the failing resize a second or two, and only the first failure needs naming.
inline constexpr HRESULT kDxgiErrorInvalidCall = static_cast<HRESULT>(0x887A0001L);

inline bool ShouldScanFailedResize(HRESULT hr, bool anyForeignReference, bool alreadyScanned) {
    return hr == kDxgiErrorInvalidCall && anyForeignReference && !alreadyScanned;
}

}  // namespace ce::resize_reference_holders

// Runs the scan for these buffers and logs every slot with its owner; see the
// comment at the top. Returns the number of slots found. Call only on a failed
// resize: it reads all writable memory of the process.
size_t LogBackBufferReferenceHolders(void* const* buffers, UINT bufferCount, const char* source);
