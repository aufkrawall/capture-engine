#pragma once

// Synthetic AV1 headers for the show-existing reader/ledger tests. Written in
// spec syntax order (5.5 sequence header, 5.9.2 uncompressed header); the
// libaom cross-checks in test_av1_show_existing_roundtrip.cpp pin the same
// reader against a real encoder.

#include <cstdint>
#include <vector>

namespace av1_test {

class Bits {
public:
    void Put(uint32_t value, int count) {
        for (int i = count - 1; i >= 0; --i) {
            if ((position_ & 7) == 0)
                bytes_.push_back(0);
            if ((value >> i) & 1u)
                bytes_.back() |= static_cast<uint8_t>(0x80u >> (position_ & 7));
            ++position_;
        }
    }
    void Uvlc(uint32_t value) {
        int leadingZeros = 0;
        while ((value + 1) >> (leadingZeros + 1))
            ++leadingZeros;
        Put(0, leadingZeros);
        Put(1, 1);
        Put(value + 1 - (1u << leadingZeros), leadingZeros);
    }
    std::vector<uint8_t> Finish(size_t paddingBytes = 0) {
        Put(1, 1);
        while (position_ & 7)
            Put(0, 1);
        bytes_.insert(bytes_.end(), paddingBytes, 0x5a);  // stands in for tile data
        return bytes_;
    }

private:
    std::vector<uint8_t> bytes_;
    size_t position_ = 0;
};

struct Sequence {
    bool reducedStill = false;
    bool timing = false;
    bool equalInterval = false;
    bool decoderModel = false;
    uint8_t bufferDelayLength = 10;
    uint8_t removalLength = 9;
    uint8_t presentationLength = 7;
    bool initialDisplayDelay = false;
    std::vector<uint16_t> operatingPointIdc = {0};
    std::vector<bool> decoderModelForOp = {true};
    uint8_t levelIdx = 12;  // > 7 carries a tier bit
    bool frameIds = false;
    uint8_t deltaLengthMinus2 = 12;
    uint8_t additionalLengthMinus1 = 2;
    uint8_t screenContent = 2;  // 0, 1 or 2 (SELECT)
    uint8_t integerMv = 2;      // 0, 1 or 2 (SELECT), when screenContent > 0
    bool orderHint = true;
    uint8_t orderHintBits = 7;
};

struct Frame {
    bool showExisting = false;
    uint8_t mapIdx = 0;
    uint8_t type = 1;  // KEY 0, INTER 1, INTRA_ONLY 2, SWITCH 3
    bool show = true;
    bool showable = true;  // written only for hidden frames
    bool errorResilient = false;
    bool allowScreenContent = false;
    uint32_t frameId = 0;
    uint8_t orderHint = 0;
    bool bufferRemoval = false;
    uint8_t refresh = 0x01;
};

inline uint8_t FrameIdLength(const Sequence& s) {
    return static_cast<uint8_t>(s.additionalLengthMinus1 + s.deltaLengthMinus2 + 3);
}

inline std::vector<uint8_t> SequencePayload(const Sequence& s) {
    Bits b;
    b.Put(0, 3);  // seq_profile
    b.Put(0, 1);  // still_picture
    b.Put(s.reducedStill, 1);
    if (s.reducedStill) {
        b.Put(s.levelIdx, 5);
    } else {
        b.Put(s.timing, 1);
        if (s.timing) {
            b.Put(1, 32);
            b.Put(120, 32);
            b.Put(s.equalInterval, 1);
            if (s.equalInterval)
                b.Uvlc(0);
            b.Put(s.decoderModel, 1);
            if (s.decoderModel) {
                b.Put(s.bufferDelayLength - 1, 5);
                b.Put(1, 32);
                b.Put(s.removalLength - 1, 5);
                b.Put(s.presentationLength - 1, 5);
            }
        }
        b.Put(s.initialDisplayDelay, 1);
        b.Put(static_cast<uint32_t>(s.operatingPointIdc.size() - 1), 5);
        for (size_t op = 0; op < s.operatingPointIdc.size(); ++op) {
            b.Put(s.operatingPointIdc[op], 12);
            b.Put(s.levelIdx, 5);
            if (s.levelIdx > 7)
                b.Put(0, 1);
            if (s.timing && s.decoderModel) {
                b.Put(s.decoderModelForOp[op], 1);
                if (s.decoderModelForOp[op]) {
                    b.Put(3, s.bufferDelayLength);
                    b.Put(5, s.bufferDelayLength);
                    b.Put(0, 1);
                }
            }
            if (s.initialDisplayDelay) {
                b.Put(1, 1);
                b.Put(9, 4);
            }
        }
    }
    b.Put(11, 4);  // frame_width_bits_minus_1
    b.Put(11, 4);  // frame_height_bits_minus_1
    b.Put(3839, 12);
    b.Put(2159, 12);
    if (!s.reducedStill) {
        b.Put(s.frameIds, 1);
        if (s.frameIds) {
            b.Put(s.deltaLengthMinus2, 4);
            b.Put(s.additionalLengthMinus1, 3);
        }
    }
    b.Put(0, 3);  // 128x128 sb, filter intra, intra edge
    if (!s.reducedStill) {
        b.Put(0, 4);
        b.Put(s.orderHint, 1);
        if (s.orderHint)
            b.Put(3, 2);
        b.Put(s.screenContent == 2, 1);
        if (s.screenContent != 2)
            b.Put(s.screenContent, 1);
        if (s.screenContent > 0) {
            b.Put(s.integerMv == 2, 1);
            if (s.integerMv != 2)
                b.Put(s.integerMv, 1);
        }
        if (s.orderHint)
            b.Put(s.orderHintBits - 1, 3);
    }
    b.Put(0x2a5, 10);  // superres/cdef/restoration/color config (not parsed)
    return b.Finish();
}

inline std::vector<uint8_t> FramePayload(const Sequence& s, const Frame& f, size_t tileBytes = 6) {
    Bits b;
    b.Put(f.showExisting, 1);
    if (f.showExisting) {
        b.Put(f.mapIdx, 3);
        if (s.decoderModel && !s.equalInterval)
            b.Put(1, s.presentationLength);
        if (s.frameIds)
            b.Put(f.frameId, FrameIdLength(s));
        return b.Finish();
    }
    b.Put(f.type, 2);
    b.Put(f.show, 1);
    if (f.show && s.decoderModel && !s.equalInterval)
        b.Put(1, s.presentationLength);
    if (!f.show)
        b.Put(f.showable, 1);
    const bool shownKey = f.type == 0 && f.show;
    if (!(f.type == 3 || shownKey))
        b.Put(f.errorResilient, 1);
    b.Put(0, 1);  // disable_cdf_update
    bool allowScreenContent = s.screenContent == 1;
    if (s.screenContent == 2) {
        b.Put(f.allowScreenContent, 1);
        allowScreenContent = f.allowScreenContent;
    }
    if (allowScreenContent && s.integerMv == 2)
        b.Put(1, 1);
    if (s.frameIds)
        b.Put(f.frameId, FrameIdLength(s));
    if (f.type != 3)
        b.Put(0, 1);  // frame_size_override_flag
    if (s.orderHint)
        b.Put(f.orderHint, s.orderHintBits);
    const bool intra = f.type == 0 || f.type == 2;
    const bool errorResilient = f.type == 3 || shownKey || f.errorResilient;
    if (!intra && !errorResilient)
        b.Put(5, 3);  // primary_ref_frame
    if (s.decoderModel) {
        b.Put(f.bufferRemoval, 1);
        if (f.bufferRemoval) {
            for (size_t op = 0; op < s.operatingPointIdc.size(); ++op) {
                const uint16_t idc = s.operatingPointIdc[op];
                if (s.decoderModelForOp[op] && (idc == 0 || ((idc & 1) && ((idc >> 8) & 1))))
                    b.Put(77, s.removalLength);
            }
        }
    }
    if (!(f.type == 3 || shownKey))
        b.Put(f.refresh, 8);
    b.Put(0x1f, 5);  // rest of the header (not parsed)
    return b.Finish(tileBytes);
}

inline std::vector<uint8_t> Obu(uint8_t type, const std::vector<uint8_t>& payload, bool extension = false) {
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>(type << 3 | (extension ? 0x04 : 0) | 0x02));
    if (extension)
        out.push_back(0);
    size_t size = payload.size();
    do {
        uint8_t byte = size & 0x7f;
        size >>= 7;
        out.push_back(static_cast<uint8_t>(byte | (size ? 0x80 : 0)));
    } while (size);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// TD [+ sequence header] + OBU_FRAME, as NVENC emits one temporal unit.
inline std::vector<uint8_t> TemporalUnit(const Sequence& s, const Frame& f, bool withSequence) {
    std::vector<uint8_t> unit = {0x12, 0x00};
    if (withSequence) {
        const auto seq = Obu(1, SequencePayload(s));
        unit.insert(unit.end(), seq.begin(), seq.end());
    }
    const auto frame = Obu(6, FramePayload(s, f));
    unit.insert(unit.end(), frame.begin(), frame.end());
    return unit;
}

}  // namespace av1_test
