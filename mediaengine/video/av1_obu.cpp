#include "av1_obu.h"

namespace ce::media::av1 {
namespace {

enum ObuType : uint8_t {
    kObuSequenceHeader = 1,
    kObuTemporalDelimiter = 2,
    kObuFrameHeader = 3,
    kObuFrame = 6,
};

class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), bits_(size * 8) {}

    // f(n), n <= 32. Reading past the end latches overrun() and returns 0.
    uint32_t Read(int count) {
        uint32_t value = 0;
        for (int i = 0; i < count; ++i) {
            if (position_ >= bits_) {
                overrun_ = true;
                return 0;
            }
            const uint32_t bit = (data_[position_ >> 3] >> (7 - (position_ & 7))) & 1u;
            value = (value << 1) | bit;
            ++position_;
        }
        return value;
    }
    bool Flag() {
        return Read(1) != 0;
    }
    // uvlc(), spec 4.10.3.
    uint32_t Uvlc() {
        int leadingZeros = 0;
        while (!overrun_ && !Flag()) {
            if (++leadingZeros >= 32)
                return 0;
        }
        return leadingZeros == 0 ? 0 : Read(leadingZeros) + ((1u << leadingZeros) - 1);
    }
    bool overrun() const {
        return overrun_;
    }

private:
    const uint8_t* data_;
    size_t bits_;
    size_t position_ = 0;
    bool overrun_ = false;
};

class BitWriter {
public:
    BitWriter(uint8_t* data, size_t capacity) : data_(data), capacity_(capacity) {}
    void Write(uint32_t value, int count) {
        for (int i = count - 1; i >= 0; --i) {
            const size_t byte = position_ >> 3;
            if (byte >= capacity_) {
                overflow_ = true;
                return;
            }
            if ((position_ & 7) == 0)
                data_[byte] = 0;
            if ((value >> i) & 1u)
                data_[byte] |= static_cast<uint8_t>(0x80u >> (position_ & 7));
            ++position_;
        }
    }
    // trailing_bits(): a one bit, then zero bits up to the byte boundary.
    void TrailingBits() {
        Write(1, 1);
        while ((position_ & 7) != 0)
            Write(0, 1);
    }
    size_t Bytes() const {
        return (position_ + 7) >> 3;
    }
    bool overflow() const {
        return overflow_;
    }

private:
    uint8_t* data_;
    size_t capacity_;
    size_t position_ = 0;
    bool overflow_ = false;
};

ParseResult Fail(ParseStatus status, const char* reason) {
    return ParseResult{status, reason};
}

bool ReadLeb128(const uint8_t* data, size_t size, size_t* offset, uint64_t* value) {
    *value = 0;
    for (int i = 0; i < 8; ++i) {
        if (*offset >= size)
            return false;
        const uint8_t byte = data[(*offset)++];
        *value |= static_cast<uint64_t>(byte & 0x7f) << (i * 7);
        if (!(byte & 0x80))
            return true;
    }
    return false;
}

// uncompressed_header() up to refresh_frame_flags (spec 5.9.2).
ParseResult ParseFrameHeaderPrefix(BitReader& bits, const SequenceHeader& seq, uint8_t temporalId, uint8_t spatialId,
                                   FrameHeader* out) {
    if (seq.reducedStillPicture)
        return Fail(ParseStatus::Unsupported, "reduced_still_picture_header");
    FrameHeader frame;
    frame.showExistingFrame = bits.Flag();
    if (frame.showExistingFrame) {
        frame.frameToShowMapIdx = static_cast<uint8_t>(bits.Read(3));
        if (seq.decoderModelInfoPresent && !seq.equalPictureInterval)
            bits.Read(seq.framePresentationTimeLength);
        if (seq.frameIdNumbersPresent)
            frame.currentFrameId = bits.Read(seq.frameIdLength);
        frame.showFrame = true;
        if (bits.overrun())
            return Fail(ParseStatus::Truncated, "show_existing_header");
        *out = frame;
        return {};
    }
    frame.frameType = static_cast<FrameType>(bits.Read(2));
    const bool frameIsIntra = frame.frameType == FrameType::Key || frame.frameType == FrameType::IntraOnly;
    frame.showFrame = bits.Flag();
    if (frame.showFrame && seq.decoderModelInfoPresent && !seq.equalPictureInterval)
        bits.Read(seq.framePresentationTimeLength);
    frame.showableFrame = frame.showFrame ? frame.frameType != FrameType::Key : bits.Flag();
    const bool shownKey = frame.frameType == FrameType::Key && frame.showFrame;
    const bool errorResilient = (frame.frameType == FrameType::Switch || shownKey) ? true : bits.Flag();
    bits.Read(1);  // disable_cdf_update
    bool allowScreenContentTools = seq.forceScreenContentTools != 0;
    if (seq.forceScreenContentTools == kSelectScreenContentTools)
        allowScreenContentTools = bits.Flag();
    if (allowScreenContentTools && seq.forceIntegerMv == kSelectIntegerMv)
        bits.Read(1);  // force_integer_mv
    if (seq.frameIdNumbersPresent)
        frame.currentFrameId = bits.Read(seq.frameIdLength);
    if (frame.frameType != FrameType::Switch)
        bits.Read(1);  // frame_size_override_flag
    bits.Read(seq.orderHintBits);
    if (!frameIsIntra && !errorResilient)
        bits.Read(3);                                  // primary_ref_frame
    if (seq.decoderModelInfoPresent && bits.Flag()) {  // buffer_removal_time_present_flag
        for (int op = 0; op < seq.operatingPointCount; ++op) {
            if (!seq.decoderModelPresentForOp[op])
                continue;
            const uint16_t idc = seq.operatingPointIdc[op];
            const bool inTemporalLayer = (idc >> temporalId) & 1u;
            const bool inSpatialLayer = (idc >> (spatialId + 8)) & 1u;
            if (idc == 0 || (inTemporalLayer && inSpatialLayer))
                bits.Read(seq.bufferRemovalTimeLength);
        }
    }
    frame.refreshFrameFlags =
        (frame.frameType == FrameType::Switch || shownKey) ? 0xff : static_cast<uint8_t>(bits.Read(8));
    if (bits.overrun())
        return Fail(ParseStatus::Truncated, "frame_header");
    *out = frame;
    return {};
}

}  // namespace

ParseResult ParseSequenceHeaderObu(const uint8_t* payload, size_t size, SequenceHeader* out) {
    BitReader bits(payload, size);
    SequenceHeader seq;
    seq.profile = static_cast<uint8_t>(bits.Read(3));
    bits.Read(1);  // still_picture
    seq.reducedStillPicture = bits.Flag();
    uint8_t bufferDelayLength = 0;
    if (seq.reducedStillPicture) {
        seq.operatingPointCount = 1;
        bits.Read(5);  // seq_level_idx[0]
    } else {
        seq.timingInfoPresent = bits.Flag();
        if (seq.timingInfoPresent) {
            bits.Read(32);  // num_units_in_display_tick
            bits.Read(32);  // time_scale
            seq.equalPictureInterval = bits.Flag();
            if (seq.equalPictureInterval)
                bits.Uvlc();  // num_ticks_per_picture_minus_1
            seq.decoderModelInfoPresent = bits.Flag();
            if (seq.decoderModelInfoPresent) {
                bufferDelayLength = static_cast<uint8_t>(bits.Read(5) + 1);
                bits.Read(32);  // num_units_in_decoding_tick
                seq.bufferRemovalTimeLength = static_cast<uint8_t>(bits.Read(5) + 1);
                seq.framePresentationTimeLength = static_cast<uint8_t>(bits.Read(5) + 1);
            }
        }
        const bool initialDisplayDelayPresent = bits.Flag();
        seq.operatingPointCount = static_cast<uint8_t>(bits.Read(5) + 1);
        for (int op = 0; op < seq.operatingPointCount; ++op) {
            seq.operatingPointIdc[op] = static_cast<uint16_t>(bits.Read(12));
            if (bits.Read(5) > 7)  // seq_level_idx
                bits.Read(1);      // seq_tier
            if (seq.decoderModelInfoPresent) {
                seq.decoderModelPresentForOp[op] = bits.Flag();
                if (seq.decoderModelPresentForOp[op]) {
                    bits.Read(bufferDelayLength);  // decoder_buffer_delay
                    bits.Read(bufferDelayLength);  // encoder_buffer_delay
                    bits.Read(1);                  // low_delay_mode_flag
                }
            }
            if (initialDisplayDelayPresent && bits.Flag())
                bits.Read(4);  // initial_display_delay_minus_1
        }
    }
    const int widthBits = static_cast<int>(bits.Read(4)) + 1;
    const int heightBits = static_cast<int>(bits.Read(4)) + 1;
    bits.Read(widthBits);
    bits.Read(heightBits);
    if (!seq.reducedStillPicture)
        seq.frameIdNumbersPresent = bits.Flag();
    if (seq.frameIdNumbersPresent) {
        const uint32_t deltaLengthMinus2 = bits.Read(4);
        const uint32_t additionalLengthMinus1 = bits.Read(3);
        seq.frameIdLength = static_cast<uint8_t>(additionalLengthMinus1 + deltaLengthMinus2 + 3);
    }
    bits.Read(3);  // use_128x128_superblock, enable_filter_intra, enable_intra_edge_filter
    seq.forceScreenContentTools = kSelectScreenContentTools;
    seq.forceIntegerMv = kSelectIntegerMv;
    if (!seq.reducedStillPicture) {
        bits.Read(4);  // interintra, masked compound, warped motion, dual filter
        const bool enableOrderHint = bits.Flag();
        if (enableOrderHint)
            bits.Read(2);  // enable_jnt_comp, enable_ref_frame_mvs
        if (!bits.Flag())  // seq_choose_screen_content_tools
            seq.forceScreenContentTools = static_cast<uint8_t>(bits.Read(1));
        if (seq.forceScreenContentTools > 0) {
            if (!bits.Flag())  // seq_choose_integer_mv
                seq.forceIntegerMv = static_cast<uint8_t>(bits.Read(1));
        }
        if (enableOrderHint)
            seq.orderHintBits = static_cast<uint8_t>(bits.Read(3) + 1);
    }
    if (bits.overrun())
        return Fail(ParseStatus::Truncated, "sequence_header");
    seq.valid = true;
    *out = seq;
    return {};
}

ParseResult ParseTemporalUnit(const uint8_t* data, size_t size, SequenceHeader* sequence, TemporalUnitInfo* out) {
    TemporalUnitInfo info;
    size_t offset = 0;
    while (offset < size) {
        const uint8_t header = data[offset++];
        if (header & 0x80)
            return Fail(ParseStatus::Truncated, "obu_forbidden_bit");
        const uint8_t type = (header >> 3) & 0x0f;
        const bool extension = (header & 0x04) != 0;
        const bool hasSize = (header & 0x02) != 0;
        uint8_t temporalId = 0;
        uint8_t spatialId = 0;
        if (extension) {
            if (offset >= size)
                return Fail(ParseStatus::Truncated, "obu_extension");
            temporalId = static_cast<uint8_t>(data[offset] >> 5);
            spatialId = static_cast<uint8_t>((data[offset] >> 3) & 0x03);
            ++offset;
            info.extensionSeen = true;
        }
        uint64_t payloadSize = size - offset;
        if (hasSize && !ReadLeb128(data, size, &offset, &payloadSize))
            return Fail(ParseStatus::Truncated, "obu_size");
        if (payloadSize > size - offset)
            return Fail(ParseStatus::Truncated, "obu_payload");
        const uint8_t* payload = data + offset;
        const size_t payloadBytes = static_cast<size_t>(payloadSize);
        offset += payloadBytes;
        if (type == kObuSequenceHeader) {
            const ParseResult parsed = ParseSequenceHeaderObu(payload, payloadBytes, sequence);
            if (parsed.status != ParseStatus::Ok)
                return parsed;
            info.sequenceHeaderSeen = true;
        } else if (type == kObuFrameHeader || type == kObuFrame) {
            if (!sequence->valid)
                return Fail(ParseStatus::NoSequence, "frame_before_sequence_header");
            ++info.frameHeaders;
            if (info.frameHeaders > 1)
                continue;
            BitReader bits(payload, payloadBytes);
            const ParseResult parsed = ParseFrameHeaderPrefix(bits, *sequence, temporalId, spatialId, &info.frame);
            if (parsed.status != ParseStatus::Ok)
                return parsed;
        }
    }
    *out = info;
    return {};
}

ParseResult CanWriteShowExisting(const SequenceHeader& sequence) {
    if (!sequence.valid)
        return Fail(ParseStatus::NoSequence, "no_sequence_header");
    if (sequence.reducedStillPicture)
        return Fail(ParseStatus::Unsupported, "reduced_still_picture_header");
    if (sequence.decoderModelInfoPresent && !sequence.equalPictureInterval)
        return Fail(ParseStatus::Unsupported, "decoder_model_without_equal_picture_interval");
    return {};
}

size_t WriteShowExistingTemporalUnit(const SequenceHeader& sequence, uint8_t mapIdx, uint32_t displayFrameId,
                                     uint8_t* out, size_t capacity) {
    if (CanWriteShowExisting(sequence).status != ParseStatus::Ok || mapIdx >= kNumRefFrames || capacity < 4)
        return 0;
    uint8_t payload[8] = {};
    BitWriter bits(payload, sizeof(payload));
    bits.Write(1, 1);  // show_existing_frame
    bits.Write(mapIdx, 3);
    if (sequence.frameIdNumbersPresent)
        bits.Write(displayFrameId, sequence.frameIdLength);
    bits.TrailingBits();
    if (bits.overflow() || bits.Bytes() > 127 || capacity < 4 + bits.Bytes())
        return 0;
    size_t written = 0;
    out[written++] = kObuTemporalDelimiter << 3 | 0x02;  // has_size_field
    out[written++] = 0;
    out[written++] = kObuFrameHeader << 3 | 0x02;
    out[written++] = static_cast<uint8_t>(bits.Bytes());  // leb128, < 128
    for (size_t i = 0; i < bits.Bytes(); ++i)
        out[written++] = payload[i];
    return written;
}

}  // namespace ce::media::av1
