#pragma once

#include <cstddef>
#include <cstdint>

// Which Streamline API generation a loaded interposer speaks, and what CE is allowed to do
// with that answer.
//
// Streamline 1.x and 2.x share export NAMES but not signatures. The two that matter:
//
//   1.x  bool slSetTag(const Resource* resource, BufferType tag, uint32_t id, const Extent* extent)
//   2.x  Result slSetTag(const ViewportHandle& viewport, const ResourceTag* tags, uint32_t numTags,
//                        CommandBuffer* cmdBuffer)
//
//   1.x  bool slEvaluateFeature(CommandBuffer* cmdBuffer, Feature feature, uint32_t frameIndex, uint32_t id)
//   2.x  Result slEvaluateFeature(Feature feature, const FrameToken& frame, const BaseStructure** inputs,
//                                 uint32_t numInputs, CommandBuffer* cmdBuffer)
//
// CE mirrored the 2.x shapes and installed them unconditionally, so on a 1.x interposer the
// evaluate hook took the game's `ID3D12GraphicsCommandList*` out of RCX into a `uint32_t
// feature` parameter and handed the truncated 32-bit value straight back to Streamline.
// The Witcher 3 (sl.interposer 1.5.6, session 20260820_221409) died on it: sl.common
// dereferenced `0x11C5F6F0`, the low half of a live command list, one save-game load in.
// The 1.x slSetTag hook was the same bug waiting for DLSS-G to arm the UI-tag path - it
// reads `tags[i]` out of what 1.x passes as a small `BufferType` enum.
//
// Generation is therefore a precondition for touching either export, and this header owns
// the decision so both the hook installer and the `streamline_dll_path` redirect answer it
// the same way.
namespace ce::streamline_api {

enum class Generation {
    Unknown,
    V1,
    V2,
};

// Exports that exist in exactly one generation. These are load-bearing: a wrong entry here
// installs an ABI-mismatched hook, so each one is present in the public headers of its own
// generation and absent from the other's.
//
// 2.x introduced frame tokens, the viewport/tag structures, and the plugin-function
// indirection; 1.x drove features through per-feature constants and enable flags.
inline constexpr const char* kV2OnlyExports[] = {
    "slSetTagForFrame", "slGetNewFrameToken",  "slGetFeatureRequirements",
    "slSetD3DDevice",   "slGetFeatureFunction", "slIsFeatureLoaded",
};

inline constexpr const char* kV1OnlyExports[] = {
    "slSetFeatureConstants", "slGetFeatureSettings", "slSetFeatureEnabled",
    "slIsFeatureEnabled",    "slGetFeatureConfiguration",
};

inline constexpr size_t kV2OnlyExportCount = sizeof(kV2OnlyExports) / sizeof(kV2OnlyExports[0]);
inline constexpr size_t kV1OnlyExportCount = sizeof(kV1OnlyExports) / sizeof(kV1OnlyExports[0]);

// A module that shows markers of both generations is not a Streamline interposer CE
// understands - a repack, a shim, or a future generation. Unknown is the safe answer and
// suppresses every ABI-sensitive hook; it never suppresses the generation-independent ones.
inline Generation Classify(bool anyV2OnlyExport, bool anyV1OnlyExport) {
    if (anyV2OnlyExport && !anyV1OnlyExport) {
        return Generation::V2;
    }
    if (anyV1OnlyExport && !anyV2OnlyExport) {
        return Generation::V1;
    }
    return Generation::Unknown;
}

// Streamline stamps its API generation into every module's file version - interposer and
// plugins alike - which is the only property a plugin shares with the interposer that owns
// its ABI. Export markers can only classify the interposer; this classifies the whole set.
inline Generation GenerationFromMajorVersion(uint32_t majorVersion) {
    switch (majorVersion) {
        case 1:
            return Generation::V1;
        case 2:
            return Generation::V2;
        default:
            return Generation::Unknown;
    }
}

inline const char* Describe(Generation generation) {
    switch (generation) {
        case Generation::V1:
            return "Streamline 1.x";
        case Generation::V2:
            return "Streamline 2.x";
        case Generation::Unknown:
            return "unrecognized Streamline generation";
    }
    return "unrecognized Streamline generation";
}

// An ABI-sensitive hook may only be installed when the module's generation is known AND
// matches the signature CE compiled the hook against.
inline bool MayInstallAbiSensitiveHook(Generation moduleGeneration, Generation hookGeneration) {
    if (moduleGeneration == Generation::Unknown || hookGeneration == Generation::Unknown) {
        return false;
    }
    return moduleGeneration == hookGeneration;
}

// Substituting a Streamline DLL across generations cannot work and must not be attempted.
//
// A game linked against 1.x imports `slSetFeatureConstants`, `slGetFeatureSettings`,
// `slSetFeatureEnabled`, `slIsFeatureEnabled` and `slGetFeatureConfiguration`; a 2.x
// interposer exports none of them, so the loader fails the import and the process dies
// before its first frame. Where the names do survive the signatures do not, which is the
// same truncation that killed The Witcher 3. The NGX runtimes (`nvngx_dlss.dll`,
// `nvngx_dlssg.dll`) are the generation-independent upgrade path and keep their own
// override keys.
inline bool MayRedirectStreamlineModuleAcrossGenerations(Generation processGeneration,
                                                         Generation replacementGeneration) {
    if (processGeneration == Generation::Unknown || replacementGeneration == Generation::Unknown) {
        // Nothing is known well enough to prove a mismatch. Historical behavior (redirect)
        // is preserved so an unrecognized-but-working setup is not broken by this check.
        return true;
    }
    return processGeneration == replacementGeneration;
}

// ---------------------------------------------------------------------------
// Streamline 1.x wire constants and structure validation
// ---------------------------------------------------------------------------

// `eBufferTypeUIColorAndAlpha`. The 1.x value is read off sl.common.dll 1.5.6's own
// buffer-type name table, where it is the 24th entry (index 23); 2.x spells the same
// constant `kBufferTypeUIColorAndAlpha = 23`. The two generations agree, so CE's existing
// constant carries over unchanged.
inline constexpr uint32_t kV1BufferTypeUIColorAndAlpha = 23;

// `eResourceTypeTex2d`, the only resource type a UI colour tag can be.
inline constexpr uint32_t kV1ResourceTypeTex2d = 0;

// Field offsets in 1.x `sl::Resource` (no BaseStructure header, unlike 2.x).
inline constexpr size_t kV1ResourceOffsetType = 0;
inline constexpr size_t kV1ResourceOffsetNative = 8;
inline constexpr size_t kV1ResourceOffsetState = 32;
inline constexpr size_t kV1ResourceProbeBytes = 48;

// Every D3D12 resource state bit a tagged UI colour buffer can legally be in. A value
// outside this mask means the offsets above did not land on a real `sl::Resource`, and CE
// must not emit a barrier from it - a wrong before-state is a GPU fault, not a bad log line.
inline constexpr uint32_t kV1PlausibleResourceStateMask =
    0x1u |     // VERTEX_AND_CONSTANT_BUFFER
    0x2u |     // INDEX_BUFFER
    0x4u |     // RENDER_TARGET
    0x8u |     // UNORDERED_ACCESS
    0x10u |    // DEPTH_WRITE
    0x20u |    // DEPTH_READ
    0x40u |    // NON_PIXEL_SHADER_RESOURCE
    0x80u |    // PIXEL_SHADER_RESOURCE
    0x200u |   // INDIRECT_ARGUMENT
    0x400u |   // COPY_DEST
    0x800u |   // COPY_SOURCE
    0x1000u |  // RESOLVE_DEST
    0x2000u;   // RESOLVE_SOURCE

// COMMON/PRESENT is zero, so a zero state is legal and the mask test must accept it.
inline bool IsPlausibleV1ResourceState(uint32_t state) {
    return (state & ~kV1PlausibleResourceStateMask) == 0;
}

// The 1.x layout above is a claim about somebody else's ABI, so CE proves it per resource
// before acting on it rather than trusting it. `native` still has to survive a real
// QueryInterface to `ID3D12Resource` at the call site; these are the cheap checks that come
// first and keep a wrong offset from ever reaching a dereference.
inline bool LooksLikeV1UiResource(uint32_t type, const void* native, uint32_t state) {
    if (type != kV1ResourceTypeTex2d) {
        return false;
    }
    if (native == nullptr) {
        return false;
    }
    // Every COM object is at least pointer-aligned; a misread field almost never is.
    if ((reinterpret_cast<uintptr_t>(native) & 0x7u) != 0) {
        return false;
    }
    return IsPlausibleV1ResourceState(state);
}

}  // namespace ce::streamline_api
