#pragma once

#include <string>
#include <vector>

// Take a screenshot using DXGI Desktop Duplication and save as PNG.
// Captures the composed desktop output (includes fullscreen games, windowed games, desktop).
// Uses WIC (Windows Imaging Component) for PNG encoding.
// screenshotDir: output directory. Empty = "captures" subfolder next to exe.
// colorSpace: "auto" preserves HDR as AVIF; "bt709" tone-maps HDR to an SDR PNG;
// "both" publishes an HDR source as both variants of the same capture. An SDR
// source has no HDR variant and always yields a single PNG.
// Returns what the capture published. `saved` is true only when every requested variant was
// published, so the combined default yields two paths (HDR AVIF first, then the SDR PNG) for an HDR
// source and one for an SDR source. `paths` lists the files that exist after the call, UTF-8: empty
// when nothing was published, and non-empty with saved == false when a combined capture published only
// one of its variants (the published file is complete and is kept).
struct ScreenshotResult {
    bool saved = false;
    std::vector<std::string> paths;
};

ScreenshotResult TakeScreenshot(const std::string& screenshotDir, const std::string& colorSpace = "both");
