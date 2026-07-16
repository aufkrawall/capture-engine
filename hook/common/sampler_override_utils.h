#pragma once

#include <d3d10.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3d9.h>
#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../../common/config.h"

namespace ce::sampler_override {

inline bool IsAnisotropicOverrideEnabled(const GraphicsConfig& gfx) {
    return !gfx.anisotropicFiltering.empty() && gfx.anisotropicFiltering != "default" &&
           gfx.anisotropicFiltering != "off";
}

inline UINT GetConfiguredMaxAnisotropy(const GraphicsConfig& gfx) {
    if (gfx.anisotropicFiltering == "2x")
        return 2;
    if (gfx.anisotropicFiltering == "4x")
        return 4;
    if (gfx.anisotropicFiltering == "8x")
        return 8;
    return 16;
}

inline bool IsD3D10ComparisonFilter(D3D10_FILTER filter) {
    return filter >= D3D10_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
}

inline bool IsD3D10AnisotropicFilter(D3D10_FILTER filter) {
    return filter == D3D10_FILTER_ANISOTROPIC || filter == D3D10_FILTER_COMPARISON_ANISOTROPIC;
}

inline D3D10_FILTER GetForcedAnisotropicFilter(D3D10_FILTER originalFilter) {
    return IsD3D10ComparisonFilter(originalFilter) ? D3D10_FILTER_COMPARISON_ANISOTROPIC : D3D10_FILTER_ANISOTROPIC;
}

inline bool IsD3D10SamplerOverrideEligible(const D3D10_SAMPLER_DESC& desc, const GraphicsConfig& gfx) {
    if (desc.MaxLOD <= 0.0f || desc.MinLOD >= desc.MaxLOD || IsD3D10ComparisonFilter(desc.Filter))
        return false;
    if (desc.AddressU == D3D10_TEXTURE_ADDRESS_BORDER || desc.AddressV == D3D10_TEXTURE_ADDRESS_BORDER ||
        desc.AddressW == D3D10_TEXTURE_ADDRESS_BORDER)
        return false;
    if (gfx.samplerOverrideMode == "aggressive")
        return true;
    return D3D11_DECODE_MIN_FILTER(static_cast<D3D11_FILTER>(desc.Filter)) == D3D11_FILTER_TYPE_LINEAR &&
           D3D11_DECODE_MAG_FILTER(static_cast<D3D11_FILTER>(desc.Filter)) == D3D11_FILTER_TYPE_LINEAR;
}

inline bool D3D10SamplerAllowsCreationTimeForcedAF(const D3D10_SAMPLER_DESC& desc, const GraphicsConfig& gfx) {
    return IsAnisotropicOverrideEnabled(gfx) && IsD3D10SamplerOverrideEligible(desc, gfx);
}

struct D3D9SamplerForcedAFInfo {
    DWORD minFilter = D3DTEXF_POINT;
    DWORD magFilter = D3DTEXF_POINT;
    DWORD mipFilter = D3DTEXF_NONE;
    DWORD addressU = D3DTADDRESS_WRAP;
    DWORD addressV = D3DTADDRESS_WRAP;
    DWORD addressW = D3DTADDRESS_WRAP;
    UINT textureMipLevels = 0;
    UINT deviceMaxAnisotropy = 1;
    bool textureBound = false;
    bool usesAddressW = false;
};

enum class D3D9ForcedAFDecision {
    Allow,
    OverrideDisabled,
    MissingTexture,
    SingleMipTexture,
    MipFilterDisabled,
    BorderAddress,
    NonMaterialAddress,
    PointMinMag,
    Unsupported,
};

inline D3D9ForcedAFDecision ClassifyD3D9SamplerForForcedAF(const D3D9SamplerForcedAFInfo& info,
                                                           const GraphicsConfig& gfx) {
    if (!IsAnisotropicOverrideEnabled(gfx)) {
        return D3D9ForcedAFDecision::OverrideDisabled;
    }
    if (!info.textureBound) {
        return D3D9ForcedAFDecision::MissingTexture;
    }
    if (info.textureMipLevels <= 1) {
        return D3D9ForcedAFDecision::SingleMipTexture;
    }
    if (info.mipFilter == D3DTEXF_NONE) {
        return D3D9ForcedAFDecision::MipFilterDisabled;
    }
    if (info.addressU == D3DTADDRESS_BORDER || info.addressV == D3DTADDRESS_BORDER ||
        (info.usesAddressW && info.addressW == D3DTADDRESS_BORDER)) {
        return D3D9ForcedAFDecision::BorderAddress;
    }
    if (info.deviceMaxAnisotropy < 2) {
        return D3D9ForcedAFDecision::Unsupported;
    }
    if (gfx.samplerOverrideMode != "aggressive") {
        const auto materialAddress = [](DWORD address) {
            return address == D3DTADDRESS_WRAP || address == D3DTADDRESS_MIRROR || address == D3DTADDRESS_CLAMP;
        };
        if (!materialAddress(info.addressU) || !materialAddress(info.addressV) ||
            (info.usesAddressW && !materialAddress(info.addressW))) {
            return D3D9ForcedAFDecision::NonMaterialAddress;
        }
        const auto linearFilter = [](DWORD filter) {
            return filter == D3DTEXF_LINEAR || filter == D3DTEXF_ANISOTROPIC;
        };
        if (!linearFilter(info.minFilter) || !linearFilter(info.magFilter)) {
            return D3D9ForcedAFDecision::PointMinMag;
        }
    }
    return D3D9ForcedAFDecision::Allow;
}

inline UINT ResolveD3D9ForcedAnisotropy(const D3D9SamplerForcedAFInfo& info, const GraphicsConfig& gfx) {
    return std::max(1u, std::min(GetConfiguredMaxAnisotropy(gfx), info.deviceMaxAnisotropy));
}

struct LegacyD3DSamplerTraits {
    DWORD pointMag = 1;
    DWORD linearMag = 2;
    DWORD anisotropicMag = 3;
    DWORD pointMin = 1;
    DWORD linearMin = 2;
    DWORD anisotropicMin = 3;
    DWORD mipNone = 0;
    DWORD mipPoint = 1;
    DWORD mipLinear = 2;
};

struct LegacyD3DSamplerForcedAFInfo {
    DWORD minFilter = 1;
    DWORD magFilter = 1;
    DWORD mipFilter = 0;
    DWORD addressU = 1;
    DWORD addressV = 1;
    DWORD addressW = 1;
    UINT deviceMaxAnisotropy = 1;
};

enum class LegacyD3DForcedAFDecision {
    Allow,
    OverrideDisabled,
    MipFilterDisabled,
    BorderAddress,
    NonMaterialAddress,
    PointMinMag,
    Unsupported,
};

inline LegacyD3DForcedAFDecision ClassifyLegacyD3DSamplerForForcedAF(const LegacyD3DSamplerForcedAFInfo& info,
                                                                     const LegacyD3DSamplerTraits& traits,
                                                                     const GraphicsConfig& gfx) {
    if (!IsAnisotropicOverrideEnabled(gfx)) {
        return LegacyD3DForcedAFDecision::OverrideDisabled;
    }
    if (info.mipFilter == traits.mipNone) {
        return LegacyD3DForcedAFDecision::MipFilterDisabled;
    }
    if (info.addressU == 4 || info.addressV == 4 || info.addressW == 4) {
        return LegacyD3DForcedAFDecision::BorderAddress;
    }
    if (info.deviceMaxAnisotropy < 2) {
        return LegacyD3DForcedAFDecision::Unsupported;
    }
    if (gfx.samplerOverrideMode != "aggressive") {
        const auto materialAddress = [](DWORD address) { return address >= 1 && address <= 3; };
        if (!materialAddress(info.addressU) || !materialAddress(info.addressV) || !materialAddress(info.addressW)) {
            return LegacyD3DForcedAFDecision::NonMaterialAddress;
        }
        const bool linearMin = info.minFilter == traits.linearMin || info.minFilter == traits.anisotropicMin;
        const bool linearMag = info.magFilter == traits.linearMag || info.magFilter == traits.anisotropicMag;
        if (!linearMin || !linearMag) {
            return LegacyD3DForcedAFDecision::PointMinMag;
        }
    }
    return LegacyD3DForcedAFDecision::Allow;
}

inline UINT ResolveLegacyD3DForcedAnisotropy(const LegacyD3DSamplerForcedAFInfo& info, const GraphicsConfig& gfx) {
    return std::max(1u, std::min(GetConfiguredMaxAnisotropy(gfx), info.deviceMaxAnisotropy));
}

struct OpenGLSamplerForcedAFInfo {
    int minFilter = 0x2703;  // GL_LINEAR_MIPMAP_LINEAR
    int magFilter = 0x2601;  // GL_LINEAR
    int wrapS = 0x2901;      // GL_REPEAT
    int wrapT = 0x2901;
    int wrapR = 0x2901;
    int compareMode = 0;  // GL_NONE
    int baseLevel = 0;
    int maxLevel = 1000;
    float deviceMaxAnisotropy = 1.0f;
    float currentAnisotropy = 1.0f;
    bool extensionSupported = false;
    bool usesWrapT = true;
    bool usesWrapR = true;
};

enum class OpenGLForcedAFDecision {
    Allow,
    OverrideDisabled,
    ExtensionUnsupported,
    NoMipRange,
    NonMipFilter,
    CompareSampler,
    BorderAddress,
    NonMaterialAddress,
    PointMinMag,
};

inline OpenGLForcedAFDecision ClassifyOpenGLSamplerForForcedAF(const OpenGLSamplerForcedAFInfo& info,
                                                               const GraphicsConfig& gfx) {
    if (!IsAnisotropicOverrideEnabled(gfx)) {
        return OpenGLForcedAFDecision::OverrideDisabled;
    }
    if (!info.extensionSupported || info.deviceMaxAnisotropy < 2.0f) {
        return OpenGLForcedAFDecision::ExtensionUnsupported;
    }
    if (info.maxLevel <= info.baseLevel) {
        return OpenGLForcedAFDecision::NoMipRange;
    }
    if (info.minFilter < 0x2700 || info.minFilter > 0x2703) {
        return OpenGLForcedAFDecision::NonMipFilter;
    }
    if (info.compareMode != 0) {
        return OpenGLForcedAFDecision::CompareSampler;
    }
    if (info.wrapS == 0x812D || info.wrapS == 0x2900 ||
        (info.usesWrapT && (info.wrapT == 0x812D || info.wrapT == 0x2900)) ||
        (info.usesWrapR && (info.wrapR == 0x812D || info.wrapR == 0x2900))) {
        return OpenGLForcedAFDecision::BorderAddress;
    }
    if (gfx.samplerOverrideMode != "aggressive") {
        const auto materialAddress = [](int address) {
            return address == 0x2901 || address == 0x8370 || address == 0x812F || address == 0x8743;
        };
        if (!materialAddress(info.wrapS) || (info.usesWrapT && !materialAddress(info.wrapT)) ||
            (info.usesWrapR && !materialAddress(info.wrapR))) {
            return OpenGLForcedAFDecision::NonMaterialAddress;
        }
        if ((info.minFilter != 0x2701 && info.minFilter != 0x2703) || info.magFilter != 0x2601) {
            return OpenGLForcedAFDecision::PointMinMag;
        }
    }
    return OpenGLForcedAFDecision::Allow;
}

inline float ResolveOpenGLForcedAnisotropy(const OpenGLSamplerForcedAFInfo& info, const GraphicsConfig& gfx) {
    return std::max(1.0f, std::min(static_cast<float>(GetConfiguredMaxAnisotropy(gfx)), info.deviceMaxAnisotropy));
}

inline bool IsD3D11ReductionFilter(D3D11_FILTER filter) {
    return filter >= D3D11_FILTER_MINIMUM_MIN_MAG_MIP_POINT;
}

inline bool IsD3D11ComparisonFilter(D3D11_FILTER filter) {
    return filter >= D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT && filter < D3D11_FILTER_MINIMUM_MIN_MAG_MIP_POINT;
}

inline bool IsD3D11AnisotropicFilter(D3D11_FILTER filter) {
    return filter == D3D11_FILTER_ANISOTROPIC || filter == D3D11_FILTER_COMPARISON_ANISOTROPIC;
}

inline D3D11_FILTER GetForcedAnisotropicFilter(D3D11_FILTER originalFilter) {
    return IsD3D11ComparisonFilter(originalFilter) ? D3D11_FILTER_COMPARISON_ANISOTROPIC : D3D11_FILTER_ANISOTROPIC;
}

inline UINT ResolveFullMipCount2D(UINT width, UINT height, UINT mipLevels);
inline UINT ResolveVisibleMipCount(UINT totalMipLevels, UINT mostDetailedMip, UINT viewMipLevels);

enum class D3D11ShaderSampleOpcodeKind {
    Implicit,
    Bias,
    Lod,
    Gradient,
    Comparison,
    OtherExplicit,
};

struct D3D11ShaderSamplerUsage {
    std::array<std::array<bool, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT>, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT>
        samplerTextures = {};
    std::array<bool, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> samplerUsesImplicitSample = {};
    std::array<bool, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> samplerUsesBiasSample = {};
    std::array<bool, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> samplerUsesLodSample = {};
    std::array<bool, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> samplerUsesGradientSample = {};
    std::array<bool, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> samplerUsesComparisonSample = {};
    std::array<bool, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> samplerUsesOtherExplicitSample = {};
    std::array<bool, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> samplerUsesExplicitSample = {};
    bool sawSampleInstruction = false;
    bool sawUnsupportedRegister = false;
};

inline char LowerAscii(char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

inline bool IsD3D11AsmIdentifierChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

inline bool IsD3D11AsmWhitespace(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

inline bool D3D11AsmLineStartsWithSample(const char* line, size_t length) {
    size_t pos = 0;
    while (pos < length && IsD3D11AsmWhitespace(line[pos])) {
        ++pos;
    }
    static constexpr char kSample[] = "sample";
    for (size_t i = 0; i < 6; ++i) {
        if (pos + i >= length || LowerAscii(line[pos + i]) != kSample[i]) {
            return false;
        }
    }
    const size_t after = pos + 6;
    if (after >= length) {
        return true;
    }
    const char next = line[after];
    return IsD3D11AsmWhitespace(next) || next == '_';
}

inline bool D3D11AsmLineIsImplicitSample(const char* line, size_t length) {
    size_t pos = 0;
    while (pos < length && IsD3D11AsmWhitespace(line[pos])) {
        ++pos;
    }
    const size_t after = pos + 6;
    return after < length && IsD3D11AsmWhitespace(line[after]);
}

inline bool D3D11AsmOpcodeSuffixMatches(const char* line, size_t length, size_t suffixPos, const char* suffix) {
    size_t i = 0;
    while (suffix[i] != '\0') {
        if (suffixPos + i >= length || LowerAscii(line[suffixPos + i]) != suffix[i]) {
            return false;
        }
        ++i;
    }
    const size_t after = suffixPos + i;
    return after >= length || IsD3D11AsmWhitespace(line[after]) || line[after] == '_' || line[after] == '(';
}

inline D3D11ShaderSampleOpcodeKind GetD3D11AsmSampleOpcodeKind(const char* line, size_t length) {
    size_t pos = 0;
    while (pos < length && IsD3D11AsmWhitespace(line[pos])) {
        ++pos;
    }
    const size_t afterSample = pos + 6;
    if (afterSample < length && IsD3D11AsmWhitespace(line[afterSample])) {
        return D3D11ShaderSampleOpcodeKind::Implicit;
    }
    if (afterSample >= length || line[afterSample] != '_') {
        return D3D11ShaderSampleOpcodeKind::OtherExplicit;
    }

    const size_t suffixPos = afterSample + 1;
    if (D3D11AsmOpcodeSuffixMatches(line, length, suffixPos, "indexable")) {
        return D3D11ShaderSampleOpcodeKind::Implicit;
    }
    if (D3D11AsmOpcodeSuffixMatches(line, length, suffixPos, "b")) {
        return D3D11ShaderSampleOpcodeKind::Bias;
    }
    if (D3D11AsmOpcodeSuffixMatches(line, length, suffixPos, "l")) {
        return D3D11ShaderSampleOpcodeKind::Lod;
    }
    if (D3D11AsmOpcodeSuffixMatches(line, length, suffixPos, "d")) {
        return D3D11ShaderSampleOpcodeKind::Gradient;
    }
    if (D3D11AsmOpcodeSuffixMatches(line, length, suffixPos, "c")) {
        return D3D11ShaderSampleOpcodeKind::Comparison;
    }
    return D3D11ShaderSampleOpcodeKind::OtherExplicit;
}

inline bool ParseD3D11AsmRegisterAt(const char* text, size_t length, size_t pos, char prefix, UINT* outRegister) {
    if (!text || !outRegister || pos >= length || LowerAscii(text[pos]) != prefix) {
        return false;
    }
    if (pos > 0 && IsD3D11AsmIdentifierChar(text[pos - 1])) {
        return false;
    }
    size_t digitPos = pos + 1;
    if (digitPos >= length || text[digitPos] < '0' || text[digitPos] > '9') {
        return false;
    }

    UINT value = 0;
    while (digitPos < length && text[digitPos] >= '0' && text[digitPos] <= '9') {
        value = value * 10u + static_cast<UINT>(text[digitPos] - '0');
        ++digitPos;
    }
    if (digitPos < length && IsD3D11AsmIdentifierChar(text[digitPos])) {
        return false;
    }
    *outRegister = value;
    return true;
}

inline void AddD3D11ShaderSamplerTextureUse(D3D11ShaderSamplerUsage& usage, UINT samplerSlot, UINT textureSlot) {
    usage.sawSampleInstruction = true;
    if (samplerSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT ||
        textureSlot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
        usage.sawUnsupportedRegister = true;
        return;
    }
    usage.samplerTextures[samplerSlot][textureSlot] = true;
}

inline void MarkD3D11ShaderSamplerSampleOpcode(D3D11ShaderSamplerUsage& usage, UINT samplerSlot,
                                               D3D11ShaderSampleOpcodeKind opcodeKind) {
    usage.sawSampleInstruction = true;
    if (samplerSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        usage.sawUnsupportedRegister = true;
        return;
    }

    switch (opcodeKind) {
        case D3D11ShaderSampleOpcodeKind::Implicit:
            usage.samplerUsesImplicitSample[samplerSlot] = true;
            break;
        case D3D11ShaderSampleOpcodeKind::Bias:
            usage.samplerUsesBiasSample[samplerSlot] = true;
            usage.samplerUsesExplicitSample[samplerSlot] = true;
            break;
        case D3D11ShaderSampleOpcodeKind::Lod:
            usage.samplerUsesLodSample[samplerSlot] = true;
            usage.samplerUsesExplicitSample[samplerSlot] = true;
            break;
        case D3D11ShaderSampleOpcodeKind::Gradient:
            usage.samplerUsesGradientSample[samplerSlot] = true;
            usage.samplerUsesExplicitSample[samplerSlot] = true;
            break;
        case D3D11ShaderSampleOpcodeKind::Comparison:
            usage.samplerUsesComparisonSample[samplerSlot] = true;
            usage.samplerUsesExplicitSample[samplerSlot] = true;
            break;
        case D3D11ShaderSampleOpcodeKind::OtherExplicit:
            usage.samplerUsesOtherExplicitSample[samplerSlot] = true;
            usage.samplerUsesExplicitSample[samplerSlot] = true;
            break;
    }
}

inline D3D11ShaderSamplerUsage ParseD3D11ShaderSamplerUsage(const char* disassembly, size_t length) {
    D3D11ShaderSamplerUsage usage = {};
    if (!disassembly || length == 0) {
        return usage;
    }

    size_t lineStart = 0;
    while (lineStart < length) {
        size_t lineEnd = lineStart;
        while (lineEnd < length && disassembly[lineEnd] != '\n') {
            ++lineEnd;
        }
        const size_t lineLength = lineEnd - lineStart;
        const char* line = disassembly + lineStart;

        if (D3D11AsmLineStartsWithSample(line, lineLength)) {
            const D3D11ShaderSampleOpcodeKind opcodeKind = GetD3D11AsmSampleOpcodeKind(line, lineLength);
            bool textureSlots[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
            bool samplerSlots[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
            bool sawTexture = false;
            bool sawSampler = false;

            for (size_t pos = 0; pos < lineLength; ++pos) {
                UINT reg = 0;
                if (ParseD3D11AsmRegisterAt(line, lineLength, pos, 't', &reg)) {
                    sawTexture = true;
                    if (reg < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
                        textureSlots[reg] = true;
                    } else {
                        usage.sawUnsupportedRegister = true;
                    }
                } else if (ParseD3D11AsmRegisterAt(line, lineLength, pos, 's', &reg)) {
                    sawSampler = true;
                    if (reg < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
                        samplerSlots[reg] = true;
                    } else {
                        usage.sawUnsupportedRegister = true;
                    }
                }
            }

            if (sawTexture && sawSampler) {
                usage.sawSampleInstruction = true;
                for (UINT sampler = 0; sampler < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++sampler) {
                    if (!samplerSlots[sampler]) {
                        continue;
                    }
                    MarkD3D11ShaderSamplerSampleOpcode(usage, sampler, opcodeKind);
                    for (UINT texture = 0; texture < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++texture) {
                        if (textureSlots[texture]) {
                            AddD3D11ShaderSamplerTextureUse(usage, sampler, texture);
                        }
                    }
                }
            }
        }

        lineStart = (lineEnd < length) ? lineEnd + 1 : length;
    }
    return usage;
}

inline bool D3D11ShaderSamplerUsesExplicitSample(const D3D11ShaderSamplerUsage& usage, UINT samplerSlot) {
    return samplerSlot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT && usage.samplerUsesExplicitSample[samplerSlot];
}

inline bool D3D11ShaderSamplerUsesBiasSample(const D3D11ShaderSamplerUsage& usage, UINT samplerSlot) {
    return samplerSlot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT && usage.samplerUsesBiasSample[samplerSlot];
}

inline bool D3D11ShaderSamplerUsesLodSample(const D3D11ShaderSamplerUsage& usage, UINT samplerSlot) {
    return samplerSlot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT && usage.samplerUsesLodSample[samplerSlot];
}

inline bool D3D11ShaderSamplerUsesGradientSample(const D3D11ShaderSamplerUsage& usage, UINT samplerSlot) {
    return samplerSlot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT && usage.samplerUsesGradientSample[samplerSlot];
}

inline bool D3D11ShaderSamplerUsesComparisonSample(const D3D11ShaderSamplerUsage& usage, UINT samplerSlot) {
    return samplerSlot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT && usage.samplerUsesComparisonSample[samplerSlot];
}

inline bool D3D11ShaderSamplerUsesOtherExplicitSample(const D3D11ShaderSamplerUsage& usage, UINT samplerSlot) {
    return samplerSlot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT && usage.samplerUsesOtherExplicitSample[samplerSlot];
}

inline bool D3D11ShaderSamplerUsesUnsafeExplicitSample(const D3D11ShaderSamplerUsage& usage, UINT samplerSlot) {
    return samplerSlot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT &&
           (usage.samplerUsesLodSample[samplerSlot] || usage.samplerUsesComparisonSample[samplerSlot] ||
            usage.samplerUsesOtherExplicitSample[samplerSlot]);
}

inline bool D3D11ShaderSamplerUsesAFSafeSample(const D3D11ShaderSamplerUsage& usage, UINT samplerSlot) {
    if (samplerSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        return false;
    }
    const bool hasDerivativeFootprint = usage.samplerUsesImplicitSample[samplerSlot] ||
                                        usage.samplerUsesBiasSample[samplerSlot] ||
                                        usage.samplerUsesGradientSample[samplerSlot];
    return hasDerivativeFootprint && !D3D11ShaderSamplerUsesUnsafeExplicitSample(usage, samplerSlot);
}

inline bool D3D11ShaderSamplerUsesOnlyImplicitSample(const D3D11ShaderSamplerUsage& usage, UINT samplerSlot) {
    return samplerSlot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT && usage.samplerUsesImplicitSample[samplerSlot] &&
           !usage.samplerUsesExplicitSample[samplerSlot];
}

inline bool D3D11ShaderSamplerUsesTexture(const D3D11ShaderSamplerUsage& usage, UINT samplerSlot, UINT textureSlot) {
    return samplerSlot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT &&
           textureSlot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT &&
           usage.samplerTextures[samplerSlot][textureSlot];
}

inline bool D3D11ShaderSamplerUsesAnyTexture(const D3D11ShaderSamplerUsage& usage, UINT samplerSlot) {
    if (samplerSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        return false;
    }
    for (bool usesTexture : usage.samplerTextures[samplerSlot]) {
        if (usesTexture) {
            return true;
        }
    }
    return false;
}

inline bool D3D11ShaderSamplerIsAFCandidate(const D3D11ShaderSamplerUsage& usage, UINT samplerSlot) {
    return D3D11ShaderSamplerUsesAnyTexture(usage, samplerSlot) &&
           D3D11ShaderSamplerUsesAFSafeSample(usage, samplerSlot);
}

inline uint32_t D3D11ShaderSamplerMaskForTextureSlot(const D3D11ShaderSamplerUsage& usage, UINT textureSlot) {
    if (textureSlot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
        return 0;
    }

    uint32_t mask = 0;
    for (UINT sampler = 0; sampler < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++sampler) {
        if (usage.samplerTextures[sampler][textureSlot]) {
            mask |= (1u << sampler);
        }
    }
    return mask;
}

inline uint32_t D3D11ShaderAFSafeSamplerMaskForTextureSlot(const D3D11ShaderSamplerUsage& usage, UINT textureSlot) {
    if (textureSlot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
        return 0;
    }

    uint32_t mask = 0;
    for (UINT sampler = 0; sampler < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++sampler) {
        if (usage.samplerTextures[sampler][textureSlot] && D3D11ShaderSamplerUsesAFSafeSample(usage, sampler)) {
            mask |= (1u << sampler);
        }
    }
    return mask;
}

inline uint32_t D3D11ShaderSamplerMaskForAnyTexture(const D3D11ShaderSamplerUsage& usage) {
    uint32_t mask = 0;
    for (UINT sampler = 0; sampler < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++sampler) {
        if (D3D11ShaderSamplerUsesAnyTexture(usage, sampler)) {
            mask |= (1u << sampler);
        }
    }
    return mask;
}

inline uint32_t D3D11ShaderAFSafeSamplerMaskForAnyTexture(const D3D11ShaderSamplerUsage& usage) {
    uint32_t mask = 0;
    for (UINT sampler = 0; sampler < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++sampler) {
        if (D3D11ShaderSamplerIsAFCandidate(usage, sampler)) {
            mask |= (1u << sampler);
        }
    }
    return mask;
}

inline UINT CountD3D11ShaderSamplerTextureUses(const D3D11ShaderSamplerUsage& usage, UINT samplerSlot,
                                               UINT* firstTextureSlot = nullptr, UINT* lastTextureSlot = nullptr) {
    if (firstTextureSlot) {
        *firstTextureSlot = UINT_MAX;
    }
    if (lastTextureSlot) {
        *lastTextureSlot = UINT_MAX;
    }
    if (samplerSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        return 0;
    }

    UINT count = 0;
    for (UINT texture = 0; texture < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++texture) {
        if (!usage.samplerTextures[samplerSlot][texture]) {
            continue;
        }
        if (firstTextureSlot && *firstTextureSlot == UINT_MAX) {
            *firstTextureSlot = texture;
        }
        if (lastTextureSlot) {
            *lastTextureSlot = texture;
        }
        ++count;
    }
    return count;
}

struct D3D11ShaderSamplerUsageSummary {
    UINT samplerCount = 0;
    UINT texturePairCount = 0;
    UINT implicitSamplers = 0;
    UINT biasSamplers = 0;
    UINT lodSamplers = 0;
    UINT gradientSamplers = 0;
    UINT comparisonSamplers = 0;
    UINT otherExplicitSamplers = 0;
    UINT afSafeSamplers = 0;
    UINT unsafeExplicitSamplers = 0;
};

inline D3D11ShaderSamplerUsageSummary SummarizeD3D11ShaderSamplerUsage(const D3D11ShaderSamplerUsage& usage) {
    D3D11ShaderSamplerUsageSummary summary = {};
    for (UINT sampler = 0; sampler < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++sampler) {
        const UINT textureCount = CountD3D11ShaderSamplerTextureUses(usage, sampler);
        if (textureCount == 0) {
            continue;
        }
        ++summary.samplerCount;
        summary.texturePairCount += textureCount;
        if (usage.samplerUsesImplicitSample[sampler]) {
            ++summary.implicitSamplers;
        }
        if (usage.samplerUsesBiasSample[sampler]) {
            ++summary.biasSamplers;
        }
        if (usage.samplerUsesLodSample[sampler]) {
            ++summary.lodSamplers;
        }
        if (usage.samplerUsesGradientSample[sampler]) {
            ++summary.gradientSamplers;
        }
        if (usage.samplerUsesComparisonSample[sampler]) {
            ++summary.comparisonSamplers;
        }
        if (usage.samplerUsesOtherExplicitSample[sampler]) {
            ++summary.otherExplicitSamplers;
        }
        if (D3D11ShaderSamplerUsesAFSafeSample(usage, sampler)) {
            ++summary.afSafeSamplers;
        }
        if (D3D11ShaderSamplerUsesUnsafeExplicitSample(usage, sampler)) {
            ++summary.unsafeExplicitSamplers;
        }
    }
    return summary;
}

enum class D3D11ForcedAFSamplerDecision {
    Allow,
    OverrideDisabled,
    FixedLOD,
    BorderAddress,
    ReductionFilter,
    ComparisonFilter,
    PointMinMag,
};

inline D3D11ForcedAFSamplerDecision ClassifyD3D11SamplerForForcedAF(const D3D11_SAMPLER_DESC& desc,
                                                                    const GraphicsConfig& gfx) {
    if (!IsAnisotropicOverrideEnabled(gfx)) {
        return D3D11ForcedAFSamplerDecision::OverrideDisabled;
    }
    if (desc.MaxLOD <= 0.0f || desc.MinLOD >= desc.MaxLOD) {
        return D3D11ForcedAFSamplerDecision::FixedLOD;
    }
    if (desc.AddressU == D3D11_TEXTURE_ADDRESS_BORDER || desc.AddressV == D3D11_TEXTURE_ADDRESS_BORDER ||
        desc.AddressW == D3D11_TEXTURE_ADDRESS_BORDER) {
        return D3D11ForcedAFSamplerDecision::BorderAddress;
    }
    if (IsD3D11ReductionFilter(desc.Filter)) {
        return D3D11ForcedAFSamplerDecision::ReductionFilter;
    }
    if (IsD3D11ComparisonFilter(desc.Filter)) {
        return D3D11ForcedAFSamplerDecision::ComparisonFilter;
    }
    if (gfx.samplerOverrideMode != "aggressive" && (D3D11_DECODE_MIN_FILTER(desc.Filter) != D3D11_FILTER_TYPE_LINEAR ||
                                                    D3D11_DECODE_MAG_FILTER(desc.Filter) != D3D11_FILTER_TYPE_LINEAR)) {
        return D3D11ForcedAFSamplerDecision::PointMinMag;
    }
    return D3D11ForcedAFSamplerDecision::Allow;
}

inline bool D3D11SamplerAllowsForcedAF(const D3D11_SAMPLER_DESC& desc, const GraphicsConfig& gfx) {
    return ClassifyD3D11SamplerForForcedAF(desc, gfx) == D3D11ForcedAFSamplerDecision::Allow;
}

inline bool IsPotentiallyProblematicD3D11AFFormat(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_UNKNOWN:
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R32_SINT:
        case DXGI_FORMAT_R32G32_UINT:
        case DXGI_FORMAT_R32G32_SINT:
        case DXGI_FORMAT_R32G32B32_UINT:
        case DXGI_FORMAT_R32G32B32_SINT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_SINT:
        case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R16_SINT:
        case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R16G16_SINT:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SINT:
        case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_R8_SINT:
        case DXGI_FORMAT_R8G8_UINT:
        case DXGI_FORMAT_R8G8_SINT:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SINT:
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC6H_TYPELESS:
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R32G32_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return true;
        default:
            return false;
    }
}

inline bool IsLikelyColorD3D11AFFormat(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16_UNORM:
        case DXGI_FORMAT_R16G16_SNORM:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R8G8_SNORM:
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R16_SNORM:
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8_SNORM:
        case DXGI_FORMAT_A8_UNORM:
        case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
        case DXGI_FORMAT_B5G6R5_UNORM:
        case DXGI_FORMAT_B5G5R5A1_UNORM:
        case DXGI_FORMAT_B4G4R4A4_UNORM:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return true;
        default:
            return false;
    }
}

enum class D3D11ForcedAFResourceDecision {
    Allow,
    UnsupportedFormat,
    UnsupportedViewDimension,
    Multisampled,
    DepthStencilResource,
    ProblematicFormat,
    NonColorFormat,
    SingleVisibleMip,
};

inline const char* D3D11ForcedAFResourceDecisionName(D3D11ForcedAFResourceDecision decision) {
    switch (decision) {
        case D3D11ForcedAFResourceDecision::Allow:
            return "allow";
        case D3D11ForcedAFResourceDecision::UnsupportedFormat:
            return "unsupported-format";
        case D3D11ForcedAFResourceDecision::UnsupportedViewDimension:
            return "unsupported-view-dimension";
        case D3D11ForcedAFResourceDecision::Multisampled:
            return "multisampled";
        case D3D11ForcedAFResourceDecision::DepthStencilResource:
            return "depth-stencil-resource";
        case D3D11ForcedAFResourceDecision::ProblematicFormat:
            return "problematic-format";
        case D3D11ForcedAFResourceDecision::NonColorFormat:
            return "non-color-format";
        case D3D11ForcedAFResourceDecision::SingleVisibleMip:
            return "single-visible-mip";
    }
    return "unknown";
}

struct D3D11Texture2DForcedAFInfo {
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT textureFormat = DXGI_FORMAT_UNKNOWN;
    D3D11_SRV_DIMENSION viewDimension = D3D11_SRV_DIMENSION_UNKNOWN;
    UINT width = 0;
    UINT height = 0;
    UINT mipLevels = 0;
    UINT mostDetailedMip = 0;
    UINT viewMipLevels = UINT_MAX;
    UINT arraySize = 1;
    UINT sampleCount = 1;
    UINT bindFlags = 0;
    UINT miscFlags = 0;
    bool formatSupported = false;
};

inline D3D11ForcedAFResourceDecision ClassifyD3D11Texture2DForForcedAF(const D3D11Texture2DForcedAFInfo& info) {
    if (!info.formatSupported) {
        return D3D11ForcedAFResourceDecision::UnsupportedFormat;
    }
    switch (info.viewDimension) {
        case D3D11_SRV_DIMENSION_TEXTURE2D:
        case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
        case D3D11_SRV_DIMENSION_TEXTURECUBE:
        case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY:
            break;
        case D3D11_SRV_DIMENSION_TEXTURE2DMS:
        case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY:
            return D3D11ForcedAFResourceDecision::Multisampled;
        default:
            return D3D11ForcedAFResourceDecision::UnsupportedViewDimension;
    }
    if (info.sampleCount > 1) {
        return D3D11ForcedAFResourceDecision::Multisampled;
    }
    if ((info.bindFlags & D3D11_BIND_DEPTH_STENCIL) != 0) {
        return D3D11ForcedAFResourceDecision::DepthStencilResource;
    }
    if (IsPotentiallyProblematicD3D11AFFormat(info.format)) {
        return D3D11ForcedAFResourceDecision::ProblematicFormat;
    }
    if (!IsLikelyColorD3D11AFFormat(info.format)) {
        return D3D11ForcedAFResourceDecision::NonColorFormat;
    }

    const UINT totalMipLevels = ResolveFullMipCount2D(info.width, info.height, info.mipLevels);
    const UINT visibleMipLevels = ResolveVisibleMipCount(totalMipLevels, info.mostDetailedMip, info.viewMipLevels);
    if (visibleMipLevels <= 1) {
        return D3D11ForcedAFResourceDecision::SingleVisibleMip;
    }

    return D3D11ForcedAFResourceDecision::Allow;
}

inline bool D3D11Texture2DAllowsForcedAF(const D3D11Texture2DForcedAFInfo& info) {
    return ClassifyD3D11Texture2DForForcedAF(info) == D3D11ForcedAFResourceDecision::Allow;
}

inline bool IsD3D12ReductionFilter(D3D12_FILTER filter) {
    return filter >= D3D12_FILTER_MINIMUM_MIN_MAG_MIP_POINT;
}

inline bool IsD3D12ComparisonFilter(D3D12_FILTER filter) {
    return filter >= D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT && filter < D3D12_FILTER_MINIMUM_MIN_MAG_MIP_POINT;
}

inline bool IsD3D12AnisotropicFilter(D3D12_FILTER filter) {
    return filter == D3D12_FILTER_ANISOTROPIC || filter == D3D12_FILTER_COMPARISON_ANISOTROPIC;
}

inline D3D12_FILTER GetForcedAnisotropicFilter(D3D12_FILTER originalFilter) {
    return IsD3D12ComparisonFilter(originalFilter) ? D3D12_FILTER_COMPARISON_ANISOTROPIC : D3D12_FILTER_ANISOTROPIC;
}

inline UINT ResolveFullMipCount2D(UINT width, UINT height, UINT mipLevels) {
    if (mipLevels != 0) {
        return mipLevels;
    }

    UINT maxDimension = std::max(width, height);
    UINT resolvedMipLevels = 0;
    do {
        ++resolvedMipLevels;
        maxDimension >>= 1;
    } while (maxDimension > 0);

    return resolvedMipLevels;
}

inline UINT ResolveVisibleMipCount(UINT totalMipLevels, UINT mostDetailedMip, UINT viewMipLevels) {
    if (viewMipLevels == UINT_MAX) {
        return totalMipLevels > mostDetailedMip ? totalMipLevels - mostDetailedMip : 0;
    }
    return viewMipLevels;
}

inline uint64_t HashSamplerOverrideConfig(const GraphicsConfig& gfx) {
    uint64_t hash = 1469598103934665603ull;
    auto mixByte = [&hash](uint8_t value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    auto mixString = [&mixByte](const std::string& value) {
        for (unsigned char ch : value) {
            mixByte(ch);
        }
        mixByte(0xff);
    };

    mixString(gfx.anisotropicFiltering);
    mixString(gfx.samplerOverrideMode);
    mixString(gfx.mipMapping);
    mixString(gfx.mipBias);
    mixString(gfx.mipBiasMode);
    mixString(gfx.msaaSamples);
    mixByte(gfx.forceMipBiasClamp ? 1 : 0);
    mixByte(gfx.sgssaa ? 1 : 0);
    mixByte(gfx.disableAutoMipBias ? 1 : 0);
    return hash;
}

}  // namespace ce::sampler_override
