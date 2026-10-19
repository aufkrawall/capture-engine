#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "runtime/screenshot/screenshotter.h"

// Screenshotter orchestrates one screenshot around a capture it does not own. These drive it with fakes:
// the capture, the desktop overlay and the in-game notifier record into one shared event log, so the
// ORDER of the steps is asserted, not just that they happened.

namespace {

using namespace ce::runtime;

struct Notified {
    OverlayNotificationType type;
    uint64_t expiry;
};

class Fixture {
public:
    std::vector<std::string> events;  // begin, capture, end(...), notify(...), in the order they happened
    std::vector<Notified> notified;
    std::string capturedDirectory;
    std::string capturedColorSpace;
    ScreenshotCapture result;
    std::function<void()> beforeReturn;  // runs inside the capture, e.g. to throw
    uint64_t clock = 100000;

    class Overlay final : public ScreenshotOverlay {
    public:
        explicit Overlay(Fixture& owner) : owner_(owner) {}
        void BeginCapture() override { owner_.events.push_back("begin"); }
        void EndCapture(bool saved) override { owner_.events.push_back(saved ? "end(saved)" : "end(failed)"); }

    private:
        Fixture& owner_;
    };

    Overlay overlay{*this};

    ScreenshotHooks Hooks(bool withOverlay = true, bool withNotifier = true) {
        ScreenshotHooks hooks;
        hooks.capture = [this](const std::string& directory, const std::string& colorSpace) {
            events.push_back("capture");
            capturedDirectory = directory;
            capturedColorSpace = colorSpace;
            if (beforeReturn)
                beforeReturn();
            return result;
        };
        if (withOverlay)
            hooks.overlay = &overlay;
        if (withNotifier) {
            hooks.notify = [this](OverlayNotificationType type, uint64_t expiry) {
                events.push_back(type == OverlayNotificationType::ScreenshotSaved ? "notify(saved)" : "notify(failed)");
                notified.push_back({type, expiry});
            };
        }
        hooks.nowMs = [this]() { return clock; };
        return hooks;
    }
};

AppConfig ConfigWith(const std::string& directory, const std::string& colorSpace) {
    AppConfig config;
    config.screenshotDir = directory;
    config.screenshotColorSpace = colorSpace;
    return config;
}

}  // namespace

TEST(ScreenshotterTest, ASavedCombinedCapturePassesEveryPathThroughInOrder) {
    Fixture fixture;
    fixture.result = {true, {"D:\\Shots\\capture_1.avif", "D:\\Shots\\capture_1.png"}};
    Screenshotter screenshotter(fixture.Hooks());

    const ScreenshotOutcome outcome = screenshotter.Take(ConfigWith("D:\\Shots", "both"));

    EXPECT_EQ(outcome.status, ScreenshotStatus::Saved);
    ASSERT_EQ(outcome.paths.size(), 2u);
    EXPECT_EQ(outcome.paths[0], "D:\\Shots\\capture_1.avif");
    EXPECT_EQ(outcome.paths[1], "D:\\Shots\\capture_1.png");
    EXPECT_EQ(fixture.capturedDirectory, "D:\\Shots") << "the capture gets the snapshot's directory";
    EXPECT_EQ(fixture.capturedColorSpace, "both") << "and its color-space policy";
}

TEST(ScreenshotterTest, TheOverlayIsHiddenBeforeCaptureRestoredAfterAndTheGameToldLast) {
    Fixture fixture;
    fixture.result = {true, {"D:\\Shots\\a.png"}};
    Screenshotter screenshotter(fixture.Hooks());
    screenshotter.Take(ConfigWith("", "auto"));

    const std::vector<std::string> expected{"begin", "capture", "end(saved)", "notify(saved)"};
    EXPECT_EQ(fixture.events, expected);
}

TEST(ScreenshotterTest, TheInGameNotificationMatchesTheResultAndExpiresAfterTwoSeconds) {
    Fixture fixture;
    fixture.result = {true, {"D:\\Shots\\a.png"}};
    Screenshotter screenshotter(fixture.Hooks());
    screenshotter.Take(ConfigWith("", "both"));
    ASSERT_EQ(fixture.notified.size(), 1u);
    EXPECT_EQ(fixture.notified[0].type, OverlayNotificationType::ScreenshotSaved);
    EXPECT_EQ(fixture.notified[0].expiry, fixture.clock + 2000u);
    EXPECT_EQ(Screenshotter::kNotificationMs, 2000u);

    fixture.clock = 555;
    fixture.result = {};
    screenshotter.Take(ConfigWith("", "both"));
    ASSERT_EQ(fixture.notified.size(), 2u);
    EXPECT_EQ(fixture.notified[1].type, OverlayNotificationType::ScreenshotFailed);
    EXPECT_EQ(fixture.notified[1].expiry, 555u + 2000u);
}

TEST(ScreenshotterTest, AFailedCaptureReportsFailedWithNoPathsAndStillEndsTheBracket) {
    Fixture fixture;
    fixture.result = {false, {}};
    Screenshotter screenshotter(fixture.Hooks());

    const ScreenshotOutcome outcome = screenshotter.Take(ConfigWith("", "both"));

    EXPECT_EQ(outcome.status, ScreenshotStatus::Failed);
    EXPECT_TRUE(outcome.paths.empty());
    const std::vector<std::string> expected{"begin", "capture", "end(failed)", "notify(failed)"};
    EXPECT_EQ(fixture.events, expected);
}

TEST(ScreenshotterTest, APartiallyPublishedCaptureFailsButStillNamesTheFileThatExists) {
    // SaveHdrAndSdrScreenshots keeps a variant that published when the other failed: the request failed (the
    // public CE_E_IO), yet the complete file is on disk and the caller must be able to find it.
    Fixture fixture;
    fixture.result = {false, {"D:\\Shots\\capture_2.avif"}};
    Screenshotter screenshotter(fixture.Hooks());

    const ScreenshotOutcome outcome = screenshotter.Take(ConfigWith("", "both"));

    EXPECT_EQ(outcome.status, ScreenshotStatus::Failed);
    ASSERT_EQ(outcome.paths.size(), 1u);
    EXPECT_EQ(outcome.paths[0], "D:\\Shots\\capture_2.avif");
    ASSERT_EQ(fixture.notified.size(), 1u);
    EXPECT_EQ(fixture.notified[0].type, OverlayNotificationType::ScreenshotFailed)
        << "the user is told it failed, not that it saved";
}

TEST(ScreenshotterTest, ACaptureThatThrowsIsAFailureAndTheBracketStillEnds) {
    Fixture fixture;
    fixture.beforeReturn = []() { throw std::runtime_error("device lost"); };
    Screenshotter screenshotter(fixture.Hooks());

    ScreenshotOutcome outcome;
    EXPECT_NO_THROW(outcome = screenshotter.Take(ConfigWith("", "both")));

    EXPECT_EQ(outcome.status, ScreenshotStatus::Failed);
    EXPECT_TRUE(outcome.paths.empty());
    const std::vector<std::string> expected{"begin", "capture", "end(failed)", "notify(failed)"};
    EXPECT_EQ(fixture.events, expected);
}

TEST(ScreenshotterTest, ANonStandardExceptionIsHandledTheSameWay) {
    Fixture fixture;
    fixture.beforeReturn = []() { throw 42; };
    Screenshotter screenshotter(fixture.Hooks());

    ScreenshotOutcome outcome;
    EXPECT_NO_THROW(outcome = screenshotter.Take(ConfigWith("", "both")));
    EXPECT_EQ(outcome.status, ScreenshotStatus::Failed);
    EXPECT_EQ(fixture.events.back(), "notify(failed)");
    EXPECT_EQ(fixture.events[fixture.events.size() - 2], "end(failed)");
}

TEST(ScreenshotterTest, AThrowDoesNotWedgeTheNextScreenshot) {
    Fixture fixture;
    fixture.beforeReturn = []() { throw std::runtime_error("first one fails"); };
    Screenshotter screenshotter(fixture.Hooks());
    EXPECT_EQ(screenshotter.Take(ConfigWith("", "both")).status, ScreenshotStatus::Failed);

    fixture.beforeReturn = nullptr;
    fixture.result = {true, {"D:\\Shots\\c.png"}};
    fixture.events.clear();
    const ScreenshotOutcome next = screenshotter.Take(ConfigWith("", "both"));
    EXPECT_EQ(next.status, ScreenshotStatus::Saved);
    const std::vector<std::string> expected{"begin", "capture", "end(saved)", "notify(saved)"};
    EXPECT_EQ(fixture.events, expected) << "a fresh bracket, begun and ended exactly once";
}

TEST(ScreenshotterTest, WithoutADesktopOverlayOrNotifierOnlyTheCaptureRuns) {
    Fixture fixture;
    fixture.result = {true, {"D:\\Shots\\d.png"}};
    Screenshotter screenshotter(fixture.Hooks(/*withOverlay=*/false, /*withNotifier=*/false));

    const ScreenshotOutcome outcome = screenshotter.Take(ConfigWith("", "both"));
    EXPECT_EQ(outcome.status, ScreenshotStatus::Saved);
    const std::vector<std::string> expected{"capture"};
    EXPECT_EQ(fixture.events, expected);
    EXPECT_TRUE(fixture.notified.empty());
}

TEST(ScreenshotterTest, AMissingCaptureIsAFailureNotACrash) {
    Fixture fixture;
    ScreenshotHooks hooks = fixture.Hooks();
    hooks.capture = nullptr;  // a core that forgot to bind the capture
    Screenshotter screenshotter(std::move(hooks));

    ScreenshotOutcome outcome;
    EXPECT_NO_THROW(outcome = screenshotter.Take(ConfigWith("", "both")));
    EXPECT_EQ(outcome.status, ScreenshotStatus::Failed);
    const std::vector<std::string> expected{"begin", "end(failed)", "notify(failed)"};
    EXPECT_EQ(fixture.events, expected) << "even then the overlay is restored";
}
