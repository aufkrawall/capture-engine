#include "screenshotter.h"

#include <windows.h>

#include <exception>
#include <utility>

#include "common/logging/log_privacy.h"
#include "common/logging/logging.h"

namespace ce::runtime {

Screenshotter::Screenshotter(ScreenshotHooks hooks) : hooks_(std::move(hooks)) {}

ScreenshotOutcome Screenshotter::Take(const AppConfig& config) {
    if (hooks_.overlay)
        hooks_.overlay->BeginCapture();

    // A capture that throws is a failed screenshot, not a dead engine thread: the bracket below must end and
    // the user must see the failure.
    ScreenshotCapture capture;
    try {
        capture = hooks_.capture(config.screenshotDir, config.screenshotColorSpace);
    } catch (const std::exception& error) {
        LogError("[Controller] Screenshot capture threw: %s", error.what());
        capture = {};
    } catch (...) {
        LogError("[Controller] Screenshot capture threw a non-standard exception");
        capture = {};
    }

    // The desktop overlay is restored (and told the result) before the in-game notification, as it always was.
    if (hooks_.overlay)
        hooks_.overlay->EndCapture(capture.saved);

    ScreenshotOutcome outcome;
    outcome.status = capture.saved ? ScreenshotStatus::Saved : ScreenshotStatus::Failed;
    outcome.paths = std::move(capture.paths);

    LogInfo("[Controller] Screenshot %s (%zu file%s)", capture.saved ? "saved" : "failed", outcome.paths.size(),
            outcome.paths.size() == 1 ? "" : "s");
    for (const std::string& path : outcome.paths)
        LogInfo("[Controller] Screenshot file: %s", ce::privacy::CollapsePathForLog(path).c_str());

    if (hooks_.notify) {
        const uint64_t now = hooks_.nowMs ? hooks_.nowMs() : GetTickCount64();
        hooks_.notify(capture.saved ? OverlayNotificationType::ScreenshotSaved : OverlayNotificationType::ScreenshotFailed,
                      now + kNotificationMs);
    }
    return outcome;
}

}  // namespace ce::runtime
