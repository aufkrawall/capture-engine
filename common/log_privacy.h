#pragma once

#include <cstring>
#include <string>

// Log privacy redaction shared by every CaptureEngine component (controller,
// inject, media, limiter, logger/sensors services, injected hook DLL, and the
// Vulkan overlay layer). Users share their logs with us for diagnostics; the
// logs must not expose the Windows account name or the user's private output
// directory layout while keeping every other diagnostic property of a path.
//
// Two orthogonal transforms:
//
//   RedactUserAccountComponents - masks the account component of Windows
//     user-profile paths (for example, the `TestUser` component in
//     "C:\Users\TestUser\..."). Length-preserving by design: log funnels format
//     into fixed-capacity buffers, so redaction must never grow a message,
//     and in-place masking needs no shifting (it cannot corrupt adjacent
//     bytes). Applied centrally by the logging funnels so no call site can
//     forget it.
//
//   CollapsePathForLog - collapses all directory components of a path except
//     its root prefix and final leaf ("H:\private\clips\capture.mkv" becomes
//     "H:\...\capture.mkv"). For user-configured capture/screenshot output
//     paths, where the drive and the timestamped filename carry the diagnostic
//     value but the intermediate directories are private disk layout.
namespace ce::privacy {

inline bool IsLogPathSeparator(char c) {
    return c == '\\' || c == '/';
}

// Case-insensitive match of a path separator + "users" + separator at text[i],
// i.e. the boundary that introduces a user-profile account component.
inline bool MatchesUserDirectoryComponent(const char* text, size_t length, size_t i) {
    if (i + 7 > length || !IsLogPathSeparator(text[i]) || !IsLogPathSeparator(text[i + 6]))
        return false;
    static const char kUsers[5] = {'u', 's', 'e', 'r', 's'};
    for (size_t k = 0; k < 5; ++k) {
        const char c = text[i + 1 + k];
        if (c != kUsers[k] && c != static_cast<char>(kUsers[k] - 'a' + 'A'))
            return false;
    }
    return true;
}

// Masks account components of "\users\<account>" path prefixes in place with
// '*' fills. Operates on the first `length` bytes of `text`; bytes beyond that
// are never read or written. Returns `length`: the transform never changes the
// message length, so callers keep their previously computed sizes.
inline size_t RedactUserAccountComponents(char* text, size_t length) {
    if (!text || length == 0)
        return length;
    constexpr size_t kMarkerLen = 7;  // separator + "users" + separator

    size_t read = 0;
    while (read + kMarkerLen <= length) {
        if (MatchesUserDirectoryComponent(text, length, read)) {
            const size_t nameStart = read + kMarkerLen;
            size_t nameEnd = nameStart;
            while (nameEnd < length && !IsLogPathSeparator(text[nameEnd]))
                ++nameEnd;
            // An empty token is not an account ("\...\Users\" at the end of a
            // path); anything else is masked without being copied anywhere.
            for (size_t i = nameStart; i < nameEnd; ++i)
                text[i] = '*';
            if (nameEnd == length)
                break;
        }
        ++read;
    }
    return length;
}

// Convenience overload for NUL-terminated buffers. Returns strlen(text).
inline size_t RedactUserAccountComponents(char* text) {
    if (!text)
        return 0;
    return RedactUserAccountComponents(text, strlen(text));
}

// Copy-based overload for std::string log fragments and manifests.
inline std::string RedactUserAccountComponents(const std::string& text) {
    std::string buffer = text;
    RedactUserAccountComponents(buffer.data(), buffer.size());
    return buffer;
}

// Root-prefix classification for CollapsePathForLog. Returns the number of
// leading bytes that form the immutable root ("H:\", "\\?\", "\\" for UNC) and
// whether any root was recognized at all.
inline size_t PathRootPrefixLength(const char* text, size_t length, bool& hasRoot) {
    hasRoot = false;
    if (length >= 4 && IsLogPathSeparator(text[0]) && IsLogPathSeparator(text[1]) && text[2] == '?' &&
        IsLogPathSeparator(text[3])) {
        // Extended-length prefix "\\?\": skip it, then classify what follows.
        const size_t inner = PathRootPrefixLength(text + 4, length - 4, hasRoot);
        return inner == 0 ? 4 : 4 + inner;
    }
    // Drive root: "X:\" or "X:/"
    if (length >= 3 && text[1] == ':' && IsLogPathSeparator(text[2])) {
        hasRoot = true;
        return 3;
    }
    // UNC root: collapse server and share into the placeholder as well, since
    // both can identify private infrastructure.
    if (length >= 2 && IsLogPathSeparator(text[0]) && IsLogPathSeparator(text[1])) {
        hasRoot = true;
        return 2;
    }
    return 0;
}

// Collapses everything between the root prefix and the final leaf so logs keep
// the storage location class (drive/UNC) and the correlatable filename without
// exposing the user's directory names. Relative paths keep only their leaf.
// Examples:
//   H:\captures\capture_stage_...mkv -> H:\...\capture_stage_...mkv
//   \\nas\media\capture.mkv       -> \\...\capture.mkv
//   relative\dir\file.png         -> ...\file.png
//   file.png                      -> file.png (unchanged)
inline std::string CollapsePathForLog(const std::string& path) {
    const size_t length = path.size();
    if (length == 0)
        return path;
    const char* text = path.c_str();
    bool hasRoot = false;
    size_t root = PathRootPrefixLength(text, length, hasRoot);

    size_t lastSeparator = std::string::npos;
    for (size_t i = length; i-- > root;) {
        if (IsLogPathSeparator(text[i])) {
            lastSeparator = i;
            break;
        }
    }
    if (!hasRoot && lastSeparator == std::string::npos)
        return path;  // Bare leaf: nothing private to remove.
    if (lastSeparator == std::string::npos || lastSeparator + 1 >= length) {
        // A root/directory without a leaf still loses its private components.
        return hasRoot ? path.substr(0, root) + "..." : "...";
    }
    const char* joiner = IsLogPathSeparator(text[lastSeparator]) ? (text[lastSeparator] == '/' ? "/" : "\\") : "\\";
    return path.substr(0, root) + "..." + joiner + path.substr(lastSeparator + 1);
}

// RedactStreamEndpointsForLog - strips the playpath from any RTMP(S) URL in a message.
//
// A live-streaming destination is `rtmp://host/app/<stream key>`: the host says which
// service and whether it resolved, and the trailing component is a credential equivalent to
// a password. FFmpeg composes its own diagnostics from the URL it was given, and that text
// is not under our control, so any message forwarded from libav* has to be filtered rather
// than trusted.
//
// Keeps the scheme and host, drops everything from the first path separator:
//   "rtmp://live.example/app/secret123 failed" -> "rtmp://live.example/<redacted> failed"
// A URL with no path is already free of a key and is left alone.
inline std::string RedactStreamEndpointsForLog(const std::string& message) {
    static constexpr const char* kSchemes[] = {"rtmps://", "rtmp://"};
    std::string result = message;
    for (const char* scheme : kSchemes) {
        const size_t schemeLength = std::strlen(scheme);
        size_t search = 0;
        for (;;) {
            size_t start = std::string::npos;
            for (size_t i = search; i + schemeLength <= result.size(); ++i) {
                size_t k = 0;
                for (; k < schemeLength; ++k) {
                    const char left = result[i + k];
                    const char right = scheme[k];
                    const char lowered =
                        (left >= 'A' && left <= 'Z') ? static_cast<char>(left - 'A' + 'a') : left;
                    if (lowered != right)
                        break;
                }
                if (k == schemeLength) {
                    start = i;
                    break;
                }
            }
            if (start == std::string::npos)
                break;

            // The URL ends at the first character that cannot appear in one.
            size_t end = start + schemeLength;
            while (end < result.size()) {
                const unsigned char c = static_cast<unsigned char>(result[end]);
                if (c <= 0x20 || c == '"' || c == '\'' || c == 0x7f)
                    break;
                ++end;
            }
            const size_t pathSeparator = result.find('/', start + schemeLength);
            if (pathSeparator == std::string::npos || pathSeparator >= end) {
                search = end;  // Host only: nothing secret to remove.
                continue;
            }
            static constexpr const char kMask[] = "<redacted>";
            result.replace(pathSeparator + 1, end - (pathSeparator + 1), kMask);
            search = pathSeparator + 1 + (sizeof(kMask) - 1);
        }
    }
    return result;
}

}  // namespace ce::privacy
