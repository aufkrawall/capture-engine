#pragma once

/**
 * D3DKMT ABI mirror (from `d3dkmthk.h` / `d3dukmdt.h`).
 *
 * CE hooks four of the Windows display kernel-mode thunks -
 * `D3DKMTQueryVideoMemoryInfo`, `D3DKMTQueryAdapterInfo`, `D3DKMTEnumAdapters`
 * and `D3DKMTEnumAdapters2` - and both reads and (under the VRAM override)
 * writes fields of the caller's structures. The Windows SDK headers that
 * declare them are not part of the MSYS2 toolchain this project builds with, so
 * the layouts are mirrored here rather than included, the same way
 * `ce::vulkan_present_metering_policy` mirrors `VK_NV_present_metering`.
 *
 * A mirror that does not match is not a cosmetic problem: every field is
 * addressed by offset, and the structures belong to the application, not to CE.
 * Until 2026-09-13 `D3DKMT_HANDLE` was typed here as `UINT64` where the SDK
 * defines `UINT32`, which shifted every member after the first handle:
 *  - `D3DKMT_QUERYADAPTERINFO::Type` read the low dword of
 *    `pPrivateDriverData`, and `PrivateDriverDataSize` read past the structure.
 *    DOOM Eternal session `20260913_180809` logged
 *    `QueryAdapterInfo - Type=3086977864, Size=52413`, where 52413 is 0xccbd -
 *    the low half of the adapter LUID the Vulkan layer reported for the same
 *    GPU in the same session.
 *  - `D3DKMT_QUERYVIDEOMEMORYINFO` was off by eight bytes from
 *    `MemorySegmentGroup` onwards, so the override branch would have written
 *    its budget over `CurrentUsage` and read `Budget`'s low dword as the
 *    segment group. Only the override being off by default kept that latent.
 *
 * The static_asserts below pin every offset and size, so a future edit that
 * changes a member type fails the build instead of shifting a write.
 */

#include <windows.h>

#include <cstddef>

namespace ce::d3dkmt {

// d3dukmdt.h: `typedef UINT32 D3DKMT_HANDLE;`
using Handle = UINT32;

// d3dkmthk.h: `D3DKMT_MEMORY_SEGMENT_GROUP`, a plain enum (4 bytes).
enum SegmentGroup : UINT32 {
    kSegmentGroupLocal = 0,
    kSegmentGroupNonLocal = 1,
};

// d3dkmthk.h: `KMTQUERYADAPTERINFOTYPE`, a plain enum (4 bytes). Only the
// values CE names are listed; the field carries the driver's full range.
enum QueryAdapterInfoType : UINT32 {
    kQaiTypeUmDriverPrivate = 0,
    kQaiTypeUmDriverName = 1,
    kQaiTypeUmOpenGlInfo = 2,
    kQaiTypeGetSegmentSize = 3,
    kQaiTypeAdapterGuid = 4,
    kQaiTypeFlipQueueInfo = 5,
    kQaiTypeAdapterAddress = 6,
    kQaiTypeSetWorkingSetInfo = 7,
    kQaiTypeAdapterRegistryInfo = 8,
    kQaiTypeCurrentDisplayMode = 9,
    kQaiTypeModeList = 10,
    kQaiTypeCheckDriverUpdateStatus = 11,
    kQaiTypeVirtualAddressInfo = 12,
    kQaiTypeDriverVersion = 13,
    kQaiTypeAdapterType = 15,
    kQaiTypeOutputDuplContextsCount = 16,
    kQaiTypeWddm12CapsFeatureLevel = 17,
    kQaiTypeUmdDriverVersion = 18,
    kQaiTypeDirectFlipSupport = 19,
    kQaiTypeQueryVidPnSourceId = 20,
    kQaiTypePhysicalAdapterCount = 40,
};

// d3dkmthk.h: `D3DKMT_QUERYVIDEOMEMORYINFO`.
struct QueryVideoMemoryInfo {
    HANDLE hProcess;
    Handle hAdapter;
    SegmentGroup MemorySegmentGroup;
    UINT64 Budget;
    UINT64 CurrentUsage;
    UINT64 CurrentReservation;
    UINT64 AvailableForReservation;
    UINT32 PhysicalAdapterIndex;
};

// d3dkmthk.h: `D3DKMT_QUERYADAPTERINFO`.
struct QueryAdapterInfo {
    Handle hAdapter;
    QueryAdapterInfoType Type;
    VOID* pPrivateDriverData;
    UINT32 PrivateDriverDataSize;
};

// d3dkmthk.h: `D3DKMT_ADAPTERINFO`. `LUID` has 4-byte alignment, so it packs
// directly behind the 4-byte handle - there is no padding at offset 4.
struct AdapterInfo {
    Handle hAdapter;
    LUID AdapterLuid;
    ULONG NumOfSources;
    BOOL bPrecisePresentRegionsPreferred;
};

// d3dkmthk.h: `MAX_ENUM_ADAPTERS`.
inline constexpr size_t kMaxEnumAdapters = 16;

// d3dkmthk.h: `D3DKMT_ENUMADAPTERS`.
struct EnumAdapters {
    ULONG NumAdapters;
    AdapterInfo Adapters[kMaxEnumAdapters];
};

// d3dkmthk.h: `D3DKMT_ENUMADAPTERS2`.
struct EnumAdapters2 {
    ULONG NumAdapters;
    AdapterInfo* pAdapters;
};

static_assert(sizeof(Handle) == 4, "D3DKMT_HANDLE is UINT32, not a pointer-sized handle");
static_assert(sizeof(SegmentGroup) == 4, "D3DKMT_MEMORY_SEGMENT_GROUP is a 4-byte enum");
static_assert(sizeof(QueryAdapterInfoType) == 4, "KMTQUERYADAPTERINFOTYPE is a 4-byte enum");

// CE ships 32-bit and 64-bit hooks, and the two members that carry a pointer or
// a Win32 HANDLE are the only ones whose offset differs between them. Those are
// asserted relative to `sizeof(void*)` / `sizeof(HANDLE)`; everything else is
// absolute in both, and the 64-bit layout is additionally pinned exactly - it is
// the one the wrong mirror was caught on.
static_assert(offsetof(QueryVideoMemoryInfo, hProcess) == 0, "D3DKMT_QUERYVIDEOMEMORYINFO::hProcess");
static_assert(offsetof(QueryVideoMemoryInfo, hAdapter) == sizeof(HANDLE), "D3DKMT_QUERYVIDEOMEMORYINFO::hAdapter");
static_assert(offsetof(QueryVideoMemoryInfo, MemorySegmentGroup) == sizeof(HANDLE) + 4,
              "D3DKMT_QUERYVIDEOMEMORYINFO::MemorySegmentGroup");
static_assert(offsetof(QueryVideoMemoryInfo, Budget) == 16, "D3DKMT_QUERYVIDEOMEMORYINFO::Budget");
static_assert(offsetof(QueryVideoMemoryInfo, CurrentUsage) == 24, "D3DKMT_QUERYVIDEOMEMORYINFO::CurrentUsage");
static_assert(offsetof(QueryVideoMemoryInfo, CurrentReservation) == 32,
              "D3DKMT_QUERYVIDEOMEMORYINFO::CurrentReservation");
static_assert(offsetof(QueryVideoMemoryInfo, AvailableForReservation) == 40,
              "D3DKMT_QUERYVIDEOMEMORYINFO::AvailableForReservation");
static_assert(offsetof(QueryVideoMemoryInfo, PhysicalAdapterIndex) == 48,
              "D3DKMT_QUERYVIDEOMEMORYINFO::PhysicalAdapterIndex");
static_assert(sizeof(QueryVideoMemoryInfo) == 56, "D3DKMT_QUERYVIDEOMEMORYINFO size");

static_assert(offsetof(QueryAdapterInfo, hAdapter) == 0, "D3DKMT_QUERYADAPTERINFO::hAdapter");
static_assert(offsetof(QueryAdapterInfo, Type) == 4, "D3DKMT_QUERYADAPTERINFO::Type");
static_assert(offsetof(QueryAdapterInfo, pPrivateDriverData) == 8, "D3DKMT_QUERYADAPTERINFO::pPrivateDriverData");
static_assert(offsetof(QueryAdapterInfo, PrivateDriverDataSize) == 8 + sizeof(void*),
              "D3DKMT_QUERYADAPTERINFO::PrivateDriverDataSize");
static_assert(sizeof(QueryAdapterInfo) == (sizeof(void*) == 8 ? 24 : 16), "D3DKMT_QUERYADAPTERINFO size");

#if defined(_WIN64)
static_assert(offsetof(QueryVideoMemoryInfo, hAdapter) == 8, "64-bit D3DKMT_QUERYVIDEOMEMORYINFO::hAdapter");
static_assert(offsetof(QueryVideoMemoryInfo, MemorySegmentGroup) == 12,
              "64-bit D3DKMT_QUERYVIDEOMEMORYINFO::MemorySegmentGroup");
static_assert(offsetof(QueryAdapterInfo, PrivateDriverDataSize) == 16,
              "64-bit D3DKMT_QUERYADAPTERINFO::PrivateDriverDataSize");
static_assert(sizeof(QueryAdapterInfo) == 24, "64-bit D3DKMT_QUERYADAPTERINFO size");
#endif

static_assert(offsetof(AdapterInfo, hAdapter) == 0, "D3DKMT_ADAPTERINFO::hAdapter");
static_assert(offsetof(AdapterInfo, AdapterLuid) == 4, "D3DKMT_ADAPTERINFO::AdapterLuid");
static_assert(offsetof(AdapterInfo, NumOfSources) == 12, "D3DKMT_ADAPTERINFO::NumOfSources");
static_assert(offsetof(AdapterInfo, bPrecisePresentRegionsPreferred) == 16,
              "D3DKMT_ADAPTERINFO::bPrecisePresentRegionsPreferred");
static_assert(sizeof(AdapterInfo) == 20, "D3DKMT_ADAPTERINFO size");

static_assert(offsetof(EnumAdapters, NumAdapters) == 0, "D3DKMT_ENUMADAPTERS::NumAdapters");
static_assert(offsetof(EnumAdapters, Adapters) == 4, "D3DKMT_ENUMADAPTERS::Adapters");
static_assert(offsetof(EnumAdapters2, NumAdapters) == 0, "D3DKMT_ENUMADAPTERS2::NumAdapters");
static_assert(offsetof(EnumAdapters2, pAdapters) == sizeof(void*), "D3DKMT_ENUMADAPTERS2::pAdapters");

}  // namespace ce::d3dkmt
