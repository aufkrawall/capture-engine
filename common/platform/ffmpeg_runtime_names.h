#pragma once

// The names of CaptureEngine's FFmpeg DLLs (avcodec-ce-63.dll; tools/build/build_linux_msys2.py FFMPEG_BUILD_SUFFIX).
// The suffix keeps them apart from a client's own FFmpeg in the same process: Windows binds an import to an already
// loaded DLL of the same base name (libcengine 11, part 4). A Python self-test pins these to the build's suffix.

namespace ce::ffmpeg_runtime {

inline constexpr wchar_t kAvutilPrefix[] = L"avutil-ce-";
inline constexpr wchar_t kAvcodecPrefix[] = L"avcodec-ce-";
inline constexpr wchar_t kAvformatPrefix[] = L"avformat-ce-";

}  // namespace ce::ffmpeg_runtime
