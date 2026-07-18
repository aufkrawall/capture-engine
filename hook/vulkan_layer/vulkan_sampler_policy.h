#pragma once

namespace ce::vulkan_sampler_policy {

enum class RejectReason {
    None,
    NoMipRange,
    Comparison,
    SpecialReduction,
    BorderAddress,
    UnnormalizedCoordinates,
    NonstandardFilter,
    SafeModeMaterialPolicy,
};

struct Input {
    bool mipmapped = false;
    bool comparison = false;
    bool specialReduction = false;
    bool borderAddress = false;
    bool unnormalizedCoordinates = false;
    bool standardMinMag = false;
    bool linearMinMag = false;
    bool materialAddress = false;
    bool aggressive = false;
};

struct Decision {
    bool allowMipMapping = false;
    bool allowAnisotropy = false;
    RejectReason mipMappingReason = RejectReason::None;
    RejectReason anisotropyReason = RejectReason::None;
};

inline RejectReason ClassifyStructuralSafety(const Input& input) {
    if (!input.mipmapped)
        return RejectReason::NoMipRange;
    if (input.comparison)
        return RejectReason::Comparison;
    if (input.specialReduction)
        return RejectReason::SpecialReduction;
    if (input.borderAddress)
        return RejectReason::BorderAddress;
    if (input.unnormalizedCoordinates)
        return RejectReason::UnnormalizedCoordinates;
    if (!input.standardMinMag)
        return RejectReason::NonstandardFilter;
    return RejectReason::None;
}

inline Decision Classify(const Input& input) {
    Decision decision = {};
    const RejectReason structural = ClassifyStructuralSafety(input);
    decision.mipMappingReason = structural;
    decision.anisotropyReason = structural;
    decision.allowMipMapping = structural == RejectReason::None;
    if (structural != RejectReason::None)
        return decision;

    decision.allowAnisotropy = input.aggressive || (input.materialAddress && input.linearMinMag);
    if (!decision.allowAnisotropy)
        decision.anisotropyReason = RejectReason::SafeModeMaterialPolicy;
    return decision;
}

inline const char* ReasonName(RejectReason reason) {
    switch (reason) {
        case RejectReason::None:
            return "allowed";
        case RejectReason::NoMipRange:
            return "no-mip-range";
        case RejectReason::Comparison:
            return "comparison";
        case RejectReason::SpecialReduction:
            return "special-reduction";
        case RejectReason::BorderAddress:
            return "border-address";
        case RejectReason::UnnormalizedCoordinates:
            return "unnormalized-coordinates";
        case RejectReason::NonstandardFilter:
            return "nonstandard-filter";
        case RejectReason::SafeModeMaterialPolicy:
            return "safe-mode-material-policy";
    }
    return "unknown";
}

}  // namespace ce::vulkan_sampler_policy
