#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "common/config/config.h"
#include "common/ipc/shared_defs.h"

namespace ce::runtime {

// The desktop status overlay while a screenshot is taken. The runtime core adapts its desktop overlay to
// this; a runtime without the overlay passes none. End maps to "restore the overlay, then show the
// saved/failed result on it" in that order.
class ScreenshotOverlay {
public:
    virtual ~ScreenshotOverlay() = default;
    virtual void BeginCapture() = 0;
    virtual void EndCapture(bool saved) = 0;
};

enum class ScreenshotStatus : uint8_t {
    Saved,   // every requested variant was published
    Failed,  // maps to the public CE_E_IO
};

struct ScreenshotOutcome {
    ScreenshotStatus status = ScreenshotStatus::Failed;
    // UTF-8 paths of the files the capture published, HDR variant first. Empty when nothing was published.
    // A failure can still list a file: a combined HDR+SDR capture whose second variant failed keeps the
    // first one, complete and atomically named. The file exists, so the caller is told where it is; the
    // failure status stays because the request as asked did not complete.
    std::vector<std::string> paths;
};

// What the capture itself returns (the media role's ScreenshotResult, translated by the adapter that binds
// it, so this header reaches no role-host code).
struct ScreenshotCapture {
    bool saved = false;
    std::vector<std::string> paths;
};

struct ScreenshotHooks {
    // Required. Takes the screenshot into `directory` (empty = default) with `colorSpace` (auto|bt709|both).
    // May block for a second or more and may throw; both are handled.
    std::function<ScreenshotCapture(const std::string& directory, const std::string& colorSpace)> capture;
    // Optional: the desktop overlay to hide for the capture.
    ScreenshotOverlay* overlay = nullptr;
    // Optional: the in-game result notification (the inject overlay). Called with the notification type and
    // the absolute expiry tick.
    std::function<void(OverlayNotificationType type, uint64_t expiryTick)> notify;
    // Optional: milliseconds on the clock the expiry tick uses. Defaults to GetTickCount64.
    std::function<uint64_t()> nowMs;
};

// Orchestrates one screenshot: hide the desktop overlay, capture, restore the overlay with the result, and
// show the result in game. The bracket always ends, whatever the capture does. Engine-thread object.
class Screenshotter {
public:
    // How long the in-game result stays up.
    static constexpr uint64_t kNotificationMs = 2000;

    explicit Screenshotter(ScreenshotHooks hooks);

    ScreenshotOutcome Take(const AppConfig& config);

private:
    ScreenshotHooks hooks_;
};

}  // namespace ce::runtime
