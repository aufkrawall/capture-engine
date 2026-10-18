#pragma once

// encoder=auto: candidate ranking and the probe-driven choice
// (llm-wiki/encoder-auto-selection.md). Pure logic; the hardware probe that
// answers "does this encoder open on this GPU" is encoder_auto_probe.h.
//
// Ranking per adapter: the GPU vendor's own SDK wrapper (NVENC, AMF, Quick
// Sync) for AV1, HEVC, H.264, then Media Foundation's hardware MFTs for the
// same families (vendor-neutral, the fallback for any other GPU or an SDK the
// driver cannot serve). Whether an encoder exists is never inferred from a
// device table: every candidate is opened for real.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "common/config/encoder_auto_request.h"

namespace ce::media {

// Encoder names to try on an adapter of `vendorId`, best first. Unknown
// vendors get the Media Foundation candidates only.
std::vector<std::string> AutoEncoderCandidates(const std::vector<encoder_auto::CodecFamily>& families,
                                               uint32_t vendorId);

struct AutoEncoderProbeResult {
    bool usable = false;
    std::string detail;  // short reason, e.g. "ok", "open: Function not implemented"
};

// Whether `encoder` can encode on the adapter at 10-bit (`tenBit`) or 8-bit.
using AutoEncoderProbe = std::function<AutoEncoderProbeResult(const std::string& encoder, bool tenBit)>;

struct AutoEncoderChoice {
    std::string encoder;  // empty: no candidate is usable
    bool tenBit = false;  // the choice passed the 10-bit probe
    size_t probes = 0;
    std::string trail;    // "av1_nvenc:10=ok" entries, capped
};

// With `wantTenBit` (bit_depth auto or 10: a recording may need 10-bit or HDR
// output) the best candidate that also encodes 10-bit wins, and only when none
// does the best 8-bit one; otherwise the best 8-bit one. The probe is called
// lazily in rank order and never again once a candidate is chosen.
AutoEncoderChoice ChooseAutoEncoder(const std::vector<std::string>& candidates, bool wantTenBit,
                                    const AutoEncoderProbe& probe);

}  // namespace ce::media
