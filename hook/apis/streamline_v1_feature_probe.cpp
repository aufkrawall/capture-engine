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

constexpr uint32_t kSlotCount = 64;
constexpr uint32_t kMaxRecordsPerSlot = 24;

uint32_t Fnv1a(const void* data, size_t length, uint32_t seed = 2166136261u) {
    uint32_t hash = seed;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < length; ++i) {
        hash = (hash ^ bytes[i]) * 16777619u;
    }
    return hash;
}

// Which bucket a (call site, feature) pair records under.
//
// The call site is folded in by hash rather than by any single character: "slGetFeatureSettings"
// and "slGetFeatureSettings:out" differ only in a suffix, and treating them as one slot would
// suppress the OUT struct - the half carrying sl::DLSSSettings / sl::DLSSGSettings, which is
// the whole reason the OUT side is recorded at all.
uint32_t SlotFor(const char* call, uint32_t feature) {
    const uint32_t callHash = Fnv1a(call, call ? strlen(call) : 0);
    // Feature ids are small, or UINT32_MAX for Common; fold both into a compact index.
    const uint32_t featureSlot = (feature == UINT32_MAX) ? 7u : (feature & 7u);
    return ((callHash & 7u) * 8u + featureSlot) % kSlotCount;
}

// Record a payload the first time it is seen AND every time its contents change.
//
// Recording only the first sighting was wrong, and a real session proved it: The Witcher 3
// set the DLSS-G constants during setup with the mode field at 0, then activated frame
// generation twenty seconds later - and every call carrying the enabled mode was thrown away
// by the throttle. The captured struct said "off" for a session that ran 4x frame generation
// for 1800 frames.
//
// A layout is static but its VALUES are the evidence: a field that changes between captures
// is a live field, and the mode transition is what identifies the mode field at all. So the
// trigger is a content change, which stays quiet for a struct the game sets once per frame
// with identical contents and speaks up exactly when something real happens. The per-slot cap
// keeps a struct with a genuinely per-frame field (a frame index, a timestamp) from flooding,
// and the heartbeat still covers anything a slot collision hid.
bool ShouldRecord(const char* call, uint32_t feature, const void* payload, size_t length) {
    static std::atomic<uint32_t> lastDigest[kSlotCount] = {};
    static std::atomic<uint32_t> records[kSlotCount] = {};
    static std::atomic<uint32_t> beats{0};

    const uint32_t slot = SlotFor(call, feature);
    // Mix the call/feature identity in so a zeroed payload is still distinct per slot, and
    // never let the digest be 0 - that is the "nothing recorded yet" sentinel.
    uint32_t digest = Fnv1a(payload, length, Fnv1a(call, call ? strlen(call) : 0) ^ feature);
    if (digest == 0) {
        digest = 1;
    }

    const uint32_t previous = lastDigest[slot].exchange(digest, std::memory_order_relaxed);
    if (previous != digest && records[slot].fetch_add(1, std::memory_order_relaxed) < kMaxRecordsPerSlot) {
        return true;
    }
    return (beats.fetch_add(1, std::memory_order_relaxed) % 16384u) == 0;
}

}  // namespace

void RecordOpaqueFeaturePayload(const char* call, uint32_t v1Feature, const void* payload) {
    const char* featureName = ce::streamline_bridge::DescribeV1Feature(v1Feature);
    constexpr size_t kDumpBytes = 96;

    // Readability is proven before the change-detector reads a byte, not after.
    if (!ReadableBytes(payload, kDumpBytes)) {
        static std::atomic<uint32_t> unreadable{0};
        if (unreadable.fetch_add(1, std::memory_order_relaxed) < 4) {
            HookLogImportant("Streamline 1.x probe: %s(%s) payload=%p is not %zu readable bytes - nothing recorded",
                             call, featureName, payload, kDumpBytes);
        }
        return;
    }
    if (!ShouldRecord(call, v1Feature, payload, kDumpBytes)) {
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
