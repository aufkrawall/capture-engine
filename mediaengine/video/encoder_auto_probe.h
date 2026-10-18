#pragma once

// encoder=auto on real hardware (llm-wiki/encoder-auto-selection.md): which
// encoder of encoder_auto_select.h's ranking actually opens on a given GPU.
//
// A candidate is first checked against the configured output contract without
// touching the GPU (FFmpeg build, pixel format, container, the encoder option
// plan's own errors such as 10-bit H.264 on AMF), then opened for real with the
// option plan on a private D3D11 device of that adapter - the same hardware
// frames route (D3D11, or Quick Sync derived from D3D11) the encoder uses.
// Open results are cached per adapter for the process lifetime, so a GPU is
// probed once, not per recording.

#include <windows.h>

#include <string>
#include <vector>

#include "common/config/config.h"
#include "common/config/encoder_auto_request.h"

struct ID3D11Device;

namespace ce::media {

// One VideoEncoder's auto request and what it was resolved for.
struct AutoEncoderBinding {
    std::string request;                // "auto", "h264_auto", ...; empty for a concrete encoder
    bool resolved = false;              // `adapter` holds the GPU of the current resolution
    LUID adapter = {};
    std::vector<std::string> excluded;  // candidates whose real open failed on `adapter`
};

struct AutoEncoderResolution {
    std::string encoder;  // empty: no candidate is usable on the adapter
    std::string log;      // one line describing the decision
};

// Resolves `request` for the adapter `luid` and the output contract in
// `config` (bit depth, chroma, colour space, container, backend options),
// skipping `excluded` candidates.
AutoEncoderResolution ResolveAutoEncoderForAdapter(const std::string& request, const LUID& luid,
                                                   const VideoConfig& config,
                                                   const std::vector<std::string>& excluded = {});

// A concrete name for `request` before the encode GPU is known, without a
// probe: the last resolution of the same request in this process, else the
// best compiled-in candidate for the vendor of the first high-performance GPU.
std::string ProvisionalAutoEncoder(const std::string& request);

bool AdapterLuidOfDevice(ID3D11Device* device, LUID* luid);

inline bool SameAdapterLuid(const LUID& lhs, const LUID& rhs) {
    return lhs.LowPart == rhs.LowPart && lhs.HighPart == rhs.HighPart;
}

}  // namespace ce::media
