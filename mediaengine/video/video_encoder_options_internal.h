#pragma once

namespace ce::video {
struct EncoderKind;
}

namespace ce::video {
struct ProfileDecision;
}

#include "video_encoder_options.h"

#include "video_encoder_backend_options.h"

#include <algorithm>

#include <cctype>

#include <charconv>

#include <limits>

#include <string_view>

namespace ce::video {
enum class CodecFamily {
    kUnknown,
    kH264,
    kHEVC,
    kAV1,
};
}

namespace ce::video {
enum class EncoderBackend {
    kUnknown,
    kNVENC,
    kAMF,
    kQSV,
    kMF,
};
}

using BitrateUsage = ce::video::detail::BackendBitrateUsage;

namespace ce::video {
BitrateUsage MakeBitrateUsage(bool applyBitrate, bool applyMaxBitrate, bool applyBufferSize = true);
}

namespace ce::video {
std::string TrimAscii(std::string_view input);
}

namespace ce::video {
std::string ToLowerAscii(std::string_view input);
}

namespace ce::video {
bool EndsWithInsensitive(std::string_view input, std::string_view suffix);
}

namespace ce::video {
EncoderKind ClassifyEncoder(std::string_view encoderName);
}

namespace ce::video {
bool SupportsProfileOption(const EncoderKind& kind);
}

namespace ce::video {
void AddWarning(EncoderOptionPlan* plan, std::string message);
}

namespace ce::video {
void AddError(EncoderOptionPlan* plan, std::string message);
}

namespace ce::video {
void AddGeneratedOption(EncoderOptionPlan* plan, std::string key, std::string value);
}

namespace ce::video {
void AddCustomOption(EncoderOptionPlan* plan, std::string key, std::string value);
}

namespace ce::video {
void AddRequiredOption(EncoderOptionPlan* plan, std::string key, std::string value);
}

namespace ce::video {
bool IsDisabledBooleanValue(std::string_view value);
}

namespace ce::video {
std::string GetAutoProfile(const EncoderKind& kind, bool use10Bit, std::string_view resolvedChroma, std::vector<std::string>* warnings);
}

namespace ce::video {
std::string CanonicalizeRequestedProfile(const EncoderKind& kind, std::string_view requested, bool* recognized);
}

namespace ce::video {
bool IsProfileCompatible(const EncoderKind& kind, std::string_view profile, bool use10Bit, std::string_view chroma);
}

namespace ce::video {
ProfileDecision ResolveProfile(const VideoConfig& config, const EncoderKind& kind, bool use10Bit, std::string_view resolvedChroma);
}

namespace ce::video {
int ClampBFrames(int requested, EncoderOptionPlan* plan);
}

namespace ce::video {
std::string CanonicalizeEnumValue(const std::string& value);
}

namespace ce::video {
std::optional<std::string> CanonicalizeNvencBRefMode(const std::string& value);
}

namespace ce::video {
std::optional<std::string> CanonicalizeNvencMultipass(const std::string& value);
}

namespace ce::video {
std::optional<std::string> CanonicalizeNvencSplitEncode(const std::string& value);
}

namespace ce::video {
bool SupportsNvencSplitEncoding(const EncoderKind& kind);
}

namespace ce::video {
bool IsNvencSplitEncodingDisabled(std::string_view value);
}

namespace ce::video {
bool IsNvencSplitEncodingForced(std::string_view value);
}

namespace ce::video {
std::optional<int> ResolveNvencLookaheadDepth(const std::string& value, int bFrames, EncoderOptionPlan* plan);
}

namespace ce::video {
std::optional<std::string> CanonicalizeNvencTune(const std::string& value);
}

namespace ce::video {
std::optional<std::string> CanonicalizeNvencPreset(const std::string& value);
}

namespace ce::video {
BitrateUsage AddRateControlOptions(const VideoConfig& config, const EncoderKind& kind, EncoderOptionPlan* plan);
}

namespace ce::video {
void ParseConfiguredBitrate(const std::string& label, const std::string& input, std::optional<int64_t>* output, EncoderOptionPlan* plan);
}

namespace ce::video {
bool ParseBitrateString(const std::string& input, int64_t* bitsPerSecond, std::string* error);
}

namespace ce::video {
bool ParseCustomOptions(const std::string& input, std::vector<EncoderOption>* options, std::string* error);
}

namespace ce::video {
EncoderOptionPlan BuildEncoderOptionPlan(const VideoConfig& config, bool use10Bit, const std::string& resolvedChroma, bool outputIsHDR);
}

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace ce::video {
struct EncoderKind {
    CodecFamily family = CodecFamily::kUnknown;
    EncoderBackend backend = EncoderBackend::kUnknown;
};
}

namespace ce::video {
struct ProfileDecision {
    std::string profile;
    bool apply = false;
    std::vector<std::string> warnings;
};
}
