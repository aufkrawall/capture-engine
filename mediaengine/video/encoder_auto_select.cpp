#include "encoder_auto_select.h"

#include "mediaengine/engine/screen_grab_adapter.h"

namespace ce::media {

namespace {

constexpr size_t kMaxTrailEntries = 8;

const char* VendorWrapperSuffix(uint32_t vendorId) {
    switch (vendorId) {
        case kVendorNvidia:
            return "_nvenc";
        case kVendorAmd:
            return "_amf";
        case kVendorIntel:
            return "_qsv";
        default:
            return nullptr;
    }
}

void AppendTrail(AutoEncoderChoice* choice, size_t* entries, const std::string& encoder, bool tenBit,
                 const AutoEncoderProbeResult& result) {
    ++*entries;
    if (*entries > kMaxTrailEntries)
        return;
    if (!choice->trail.empty())
        choice->trail += ' ';
    choice->trail += encoder + (tenBit ? ":10=" : ":8=") + (result.usable ? "ok" : result.detail);
}

}  // namespace

std::vector<std::string> AutoEncoderCandidates(const std::vector<encoder_auto::CodecFamily>& families,
                                               uint32_t vendorId) {
    std::vector<std::string> candidates;
    if (const char* suffix = VendorWrapperSuffix(vendorId)) {
        for (const encoder_auto::CodecFamily family : families)
            candidates.push_back(std::string(encoder_auto::CodecFamilyPrefix(family)) + suffix);
    }
    for (const encoder_auto::CodecFamily family : families)
        candidates.push_back(std::string(encoder_auto::CodecFamilyPrefix(family)) + "_mf");
    return candidates;
}

AutoEncoderChoice ChooseAutoEncoder(const std::vector<std::string>& candidates, bool wantTenBit,
                                    const AutoEncoderProbe& probe) {
    AutoEncoderChoice choice;
    size_t entries = 0;
    auto firstUsable = [&](bool tenBit) -> bool {
        for (const std::string& candidate : candidates) {
            const AutoEncoderProbeResult result = probe(candidate, tenBit);
            ++choice.probes;
            AppendTrail(&choice, &entries, candidate, tenBit, result);
            if (result.usable) {
                choice.encoder = candidate;
                choice.tenBit = tenBit;
                return true;
            }
        }
        return false;
    };
    if (!(wantTenBit && firstUsable(true)))
        firstUsable(false);
    if (entries > kMaxTrailEntries)
        choice.trail += " +" + std::to_string(entries - kMaxTrailEntries) + " more";
    return choice;
}

}  // namespace ce::media
