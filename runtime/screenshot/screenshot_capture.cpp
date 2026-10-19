#include "screenshot_capture.h"

#include <utility>

#include "captureengine/media/screenshot.h"

namespace ce::runtime {

ScreenshotCapture CaptureDesktopScreenshot(const std::string& directory, const std::string& colorSpace) {
    ScreenshotResult result = TakeScreenshot(directory, colorSpace);
    return {result.saved, std::move(result.paths)};
}

}  // namespace ce::runtime
