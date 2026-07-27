#pragma once

// Private surface shared between screenshot_encoding.cpp (raw-file handling,
// the SDR path, the PQ/ST-2084 maths and the public entry points) and
// screenshot_hdr_encoding.cpp (parallel HDR conversion and AVIF encoding).
// Include screenshot_encoding.h for the public API.

#include "screenshot_encoding.h"

#include "../common/reserved_capture_output.h"

#include <filesystem>
#include <string>

namespace ce::screenshot {

using ce::capture_output::ReservedCaptureOutput;

// Defined in screenshot_encoding.cpp.
std::string WideToUtf8(const std::wstring& text);
bool FlushPath(const std::filesystem::path& path);
ReservedCaptureOutput ReserveScreenshotStaging(const std::filesystem::path& outputDirectory);

// Defined in screenshot_hdr_encoding.cpp.
bool ConvertHdrToSdrScreenshot(const RawScreenshot& screenshot, float sdrWhiteNits, RawScreenshot& converted);
bool SaveHdrAvif(const std::filesystem::path& outputDirectory, const RawScreenshot& screenshot,
                 std::filesystem::path& publishedPath);

}  // namespace ce::screenshot
