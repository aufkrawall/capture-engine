#pragma once

// Minimal AV1 OBU reader/writer for show-existing CFR repeats.
//
// A repeat slot is written as a temporal unit that only re-shows a frame the
// decoder already holds (show_existing_frame=1, AV1 spec 5.9.2). Choosing the
// reference slot needs NVENC's refresh_frame_flags of that frame, and the
// bundled FFmpeg exposes no AV1 header parser (DLL build, --disable-parsers,
// CBS internal), so CE reads exactly the sequence-header fields and the
// uncompressed-header prefix that decide those bits. Anything outside the
// supported subset is reported as unsupported; callers then re-encode.
// Design and rules: llm-wiki/nvenc-encoding.md "AV1 show-existing repeats".

#include <cstddef>
#include <cstdint>

namespace ce::media::av1 {

inline constexpr int kNumRefFrames = 8;
inline constexpr int kMaxOperatingPoints = 32;
inline constexpr uint8_t kSelectScreenContentTools = 2;
inline constexpr uint8_t kSelectIntegerMv = 2;
// TD + frame-header OBU with the longest supported display_frame_id.
inline constexpr size_t kMaxShowExistingUnitBytes = 16;

enum class FrameType : uint8_t { Key = 0, Inter = 1, IntraOnly = 2, Switch = 3 };

enum class ParseStatus : uint8_t {
    Ok,
    Truncated,    // malformed or cut short
    NoSequence,   // frame header before any sequence header
    Unsupported,  // valid AV1 outside the supported subset (see reason)
};

struct SequenceHeader {
    bool valid = false;
    uint8_t profile = 0;
    bool reducedStillPicture = false;
    bool timingInfoPresent = false;
    bool equalPictureInterval = false;
    bool decoderModelInfoPresent = false;
    uint8_t bufferRemovalTimeLength = 0;
    uint8_t framePresentationTimeLength = 0;
    uint8_t operatingPointCount = 0;
    uint16_t operatingPointIdc[kMaxOperatingPoints] = {};
    bool decoderModelPresentForOp[kMaxOperatingPoints] = {};
    bool frameIdNumbersPresent = false;
    uint8_t frameIdLength = 0;
    uint8_t forceScreenContentTools = 0;
    uint8_t forceIntegerMv = 0;
    uint8_t orderHintBits = 0;
};

struct FrameHeader {
    bool showExistingFrame = false;
    uint8_t frameToShowMapIdx = 0;
    FrameType frameType = FrameType::Key;
    bool showFrame = false;
    bool showableFrame = false;
    uint8_t refreshFrameFlags = 0;
    uint32_t currentFrameId = 0;
};

// One encoder packet (one temporal unit). With b_frames=0 NVENC emits exactly
// one frame header per unit; more than one is reported (frameHeaders > 1).
struct TemporalUnitInfo {
    bool sequenceHeaderSeen = false;
    bool extensionSeen = false;
    int frameHeaders = 0;
    FrameHeader frame;  // the first frame header of the unit
};

struct ParseResult {
    ParseStatus status = ParseStatus::Ok;
    const char* reason = "";  // static string, stable for logs
};

ParseResult ParseSequenceHeaderObu(const uint8_t* payload, size_t size, SequenceHeader* out);

// Walks every OBU of `data`. A sequence header OBU replaces `sequence`; the
// frame header uses the sequence valid at that point. `extradata` style input
// (sequence header only) is accepted as well.
ParseResult ParseTemporalUnit(const uint8_t* data, size_t size, SequenceHeader* sequence, TemporalUnitInfo* out);

// Whether a show-existing unit can be written for this sequence: a decoder
// model without equal_picture_interval needs temporal_point_info, which CE
// does not synthesize.
ParseResult CanWriteShowExisting(const SequenceHeader& sequence);

// Writes TD + OBU_FRAME_HEADER(show_existing_frame=1, frame_to_show_map_idx).
// `displayFrameId` is only written when the sequence carries frame ids.
// Returns the byte count, or 0 when `capacity` is too small or the sequence is
// unsupported.
size_t WriteShowExistingTemporalUnit(const SequenceHeader& sequence, uint8_t mapIdx, uint32_t displayFrameId,
                                     uint8_t* out, size_t capacity);

}  // namespace ce::media::av1
