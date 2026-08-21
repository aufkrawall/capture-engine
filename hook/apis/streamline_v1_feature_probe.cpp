#include "streamline_v1_feature_probe.h"

#include <windows.h>

#include <cstring>

#include "../common/hook_common.h"
#include "streamline_bridge_policy.h"

namespace ce::streamline_v1 {

PFN_slSetFeatureConstantsV1 g_Original_slSetFeatureConstantsV1 = nullptr;
std::atomic<bool> g_SetFeatureConstantsV1Hooked{false};
std::atomic<void*> g_SetFeatureConstantsV1Target{nullptr};

PFN_slGetFeatureSettingsV1 g_Original_slGetFeatureSettingsV1 = nullptr;
std::atomic<bool> g_GetFeatureSettingsV1Hooked{false};
std::atomic<void*> g_GetFeatureSettingsV1Target{nullptr};

namespace {

// A diagnostic that reads a foreign struct must never be the thing that faults. The page
// the payload lives on has to be committed and readable for the whole span before a single
// byte is touched.
bool ReadableBytes(const void* address, size_t wanted) {
    if (!address || wanted == 0) {
        return false;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info)) {
        return false;
    }
    if (info.State != MEM_COMMIT) {
        return false;
    }
    constexpr DWORD kNoRead = PAGE_NOACCESS | PAGE_GUARD;
    if (info.Protect == 0 || (info.Protect & kNoRead) != 0) {
        return false;
    }
    const auto base = static_cast<const uint8_t*>(info.BaseAddress);
    const auto start = static_cast<const uint8_t*>(address);
    const size_t consumed = static_cast<size_t>(start - base);
    if (consumed >= info.RegionSize) {
        return false;
    }
    return (info.RegionSize - consumed) >= wanted;
}

// One record per (call site, feature) pair, plus a rare heartbeat. These calls run every
// frame, so an unconditional dump would be exactly the hot-path noise the project forbids -
// and a struct layout is a constant, so the first sighting already carries it.
//
// The call site is folded in by hash rather than by any single character: "slGetFeatureSettings"
// and "slGetFeatureSettings:out" differ only in a suffix, and treating them as one slot would
// suppress the OUT struct - which is the half that carries sl::DLSSSettings / sl::DLSSGSettings
// and is the whole reason the OUT side is recorded at all.
bool ShouldRecord(const char* call, uint32_t feature) {
    static std::atomic<uint64_t> seen{0};
    static std::atomic<uint32_t> beats{0};

    uint32_t hash = 2166136261u;  // FNV-1a
    for (const char* cursor = call; cursor && *cursor; ++cursor) {
        hash = (hash ^ static_cast<uint8_t>(*cursor)) * 16777619u;
    }
    // Feature ids are small, or UINT32_MAX for Common; fold both into a compact index.
    const uint32_t featureSlot = (feature == UINT32_MAX) ? 7u : (feature & 7u);
    const uint32_t slot = ((hash & 7u) * 8u + featureSlot) & 63u;

    const uint64_t bit = 1ull << slot;
    if ((seen.fetch_or(bit, std::memory_order_relaxed) & bit) == 0) {
        return true;
    }
    // Two distinct pairs can still share a slot; the heartbeat is what guarantees a
    // collided one is eventually seen rather than silently lost.
    return (beats.fetch_add(1, std::memory_order_relaxed) % 16384u) == 0;
}

}  // namespace

void RecordOpaqueFeaturePayload(const char* call, uint32_t v1Feature, const void* payload) {
    if (!ShouldRecord(call, v1Feature)) {
        return;
    }

    const char* featureName = ce::streamline_bridge::DescribeV1Feature(v1Feature);
    constexpr size_t kDumpBytes = 96;
    if (!ReadableBytes(payload, kDumpBytes)) {
        HookLogImportant("Streamline 1.x probe: %s(%s) payload=%p is not %zu readable bytes - nothing recorded",
                         call, featureName, payload, kDumpBytes);
        return;
    }

    const auto* bytes = static_cast<const uint8_t*>(payload);
    static const char kDigits[] = "0123456789abcdef";
    char hex[kDumpBytes * 3 + 1] = {};
    for (size_t i = 0; i < kDumpBytes; ++i) {
        hex[i * 3 + 0] = kDigits[bytes[i] >> 4];
        hex[i * 3 + 1] = kDigits[bytes[i] & 0xF];
        hex[i * 3 + 2] = ' ';
    }
    // The leading dwords and floats are what identify the mode/enum fields and the first
    // real values, which is the part a layout reconstruction starts from.
    uint32_t head[4] = {};
    memcpy(head, bytes, sizeof(head));
    float headf[4] = {};
    memcpy(headf, bytes, sizeof(headf));
    HookLogImportant(
        "Streamline 1.x probe: %s(feature=%s/%u) payload=%p u32[0..3]=%u,%u,%u,%u f32[0..3]=%.4f,%.4f,%.4f,%.4f "
        "first %zu bytes: %s",
        call, featureName, v1Feature, payload, head[0], head[1], head[2], head[3], headf[0], headf[1], headf[2],
        headf[3], kDumpBytes, hex);
}

bool Hooked_slSetFeatureConstantsV1(uint32_t feature, const void* consts, uint32_t frameIndex, uint32_t id) {
    RecordOpaqueFeaturePayload("slSetFeatureConstants", feature, consts);
    // Pass through untouched. This hook exists to observe, never to change behaviour: the
    // game keeps driving its own 1.x runtime exactly as it would without CE.
    if (g_Original_slSetFeatureConstantsV1) {
        return g_Original_slSetFeatureConstantsV1(feature, consts, frameIndex, id);
    }
    return false;
}

bool Hooked_slGetFeatureSettingsV1(uint32_t feature, const void* consts, void* settings) {
    RecordOpaqueFeaturePayload("slGetFeatureSettings", feature, consts);
    if (!g_Original_slGetFeatureSettingsV1) {
        return false;
    }
    const bool ok = g_Original_slGetFeatureSettingsV1(feature, consts, settings);
    // The OUT struct is the other half of the unknown pair (`sl::DLSSSettings`,
    // `sl::DLSSGSettings`), and it is only meaningful once the callee has filled it.
    if (ok) {
        RecordOpaqueFeaturePayload("slGetFeatureSettings:out", feature, settings);
    }
    return ok;
}

}  // namespace ce::streamline_v1
