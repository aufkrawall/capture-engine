#pragma once

// [Video] encoder=auto (the default) and its codec-restricted forms
// (llm-wiki/encoder-auto-selection.md). The request is resolved to a concrete
// FFmpeg encoder by the media engine once the GPU that will encode is known;
// every other consumer of VideoConfig::encoder only ever sees that result.

#include <cctype>
#include <cstddef>
#include <iterator>
#include <string_view>
#include <vector>

namespace ce::encoder_auto {

enum class CodecFamily { kAv1, kHevc, kH264 };

// Best first: AV1, HEVC, H.264 (compression efficiency at equal bitrate).
inline constexpr CodecFamily kFamiliesBestFirst[] = {CodecFamily::kAv1, CodecFamily::kHevc, CodecFamily::kH264};

inline const char* CodecFamilyPrefix(CodecFamily family) {
    switch (family) {
        case CodecFamily::kAv1:
            return "av1";
        case CodecFamily::kHevc:
            return "hevc";
        case CodecFamily::kH264:
            return "h264";
    }
    return "";
}

namespace detail {
inline bool EqualsInsensitive(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size())
        return false;
    for (size_t index = 0; index < lhs.size(); ++index) {
        const int left = std::tolower(static_cast<unsigned char>(lhs[index]));
        if (left != std::tolower(static_cast<unsigned char>(rhs[index])))
            return false;
    }
    return true;
}
}  // namespace detail

// "auto" allows every family, best first; "av1_auto", "hevc_auto" and
// "h264_auto" allow only that family (live streaming uses "h264_auto").
// Case-insensitive. False for every concrete encoder name; `families` may be
// null.
inline bool ParseAutoEncoderRequest(std::string_view encoder, std::vector<CodecFamily>* families) {
    if (detail::EqualsInsensitive(encoder, "auto")) {
        if (families)
            families->assign(std::begin(kFamiliesBestFirst), std::end(kFamiliesBestFirst));
        return true;
    }
    constexpr std::string_view kSuffix = "_auto";
    if (encoder.size() <= kSuffix.size() ||
        !detail::EqualsInsensitive(encoder.substr(encoder.size() - kSuffix.size()), kSuffix))
        return false;
    const std::string_view prefix = encoder.substr(0, encoder.size() - kSuffix.size());
    for (const CodecFamily family : kFamiliesBestFirst) {
        if (detail::EqualsInsensitive(prefix, CodecFamilyPrefix(family))) {
            if (families)
                families->assign(1, family);
            return true;
        }
    }
    return false;
}

inline bool IsAutoEncoderRequest(std::string_view encoder) {
    return ParseAutoEncoderRequest(encoder, nullptr);
}

}  // namespace ce::encoder_auto
