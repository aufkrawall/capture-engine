#pragma once

#include <string>

#include "screenshotter.h"

namespace ce::runtime {

// ScreenshotHooks::capture bound to the media role's TakeScreenshot (hook-assisted, WGC or GDI desktop
// capture, published atomically as PNG and/or AVIF). Kept apart from the Screenshotter so the orchestration
// is testable without capturing a desktop, and so the one role-host dependency has one place to be listed.
ScreenshotCapture CaptureDesktopScreenshot(const std::string& directory, const std::string& colorSpace);

}  // namespace ce::runtime
