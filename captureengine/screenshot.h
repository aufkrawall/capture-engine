#pragma once

#include <string>

// Take a screenshot using DXGI Desktop Duplication and save as PNG.
// Captures the composed desktop output (includes fullscreen games, windowed games, desktop).
// Uses WIC (Windows Imaging Component) for PNG encoding.
// screenshotDir: output directory. Empty = "captures" subfolder next to exe.
// Returns true on success.
bool TakeScreenshot(const std::string& screenshotDir);
