#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <latch>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

#include <windows.h>

#include "captureengine/media/media_session_state.h"
#include "common/logging/logging.h"
#include "source_fragment_reader.h"

namespace {

std::filesystem::path UniqueLogPath(const char* tag) {
    static std::atomic<uint32_t> sequence{1};
    return std::filesystem::temp_directory_path() /
           (std::string("ce_media_session_") + tag + "_" + std::to_string(GetCurrentProcessId()) + "_" +
            std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".log");
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST(MediaSessionStateTest, BindingsNestAndRestoreTheOuterSession) {
    auto processSession = std::make_unique<MediaSessionState>();
    auto outer = std::make_unique<MediaSessionState>();
    auto inner = std::make_unique<MediaSessionState>();
    MediaProcessDefaultScope processDefault(*processSession);

    {
        MediaSessionBinding outerBinding(*outer);
        EXPECT_EQ(&MediaState(), outer.get());
        {
            MediaSessionBinding innerBinding(*inner);
            EXPECT_EQ(&MediaState(), inner.get());
        }
        EXPECT_EQ(&MediaState(), outer.get());
    }
    EXPECT_EQ(&MediaState(), processSession.get());
}

TEST(MediaSessionStateTest, UnboundThreadsUseTheProcessSession) {
    auto processSession = std::make_unique<MediaSessionState>();
    MediaProcessDefaultScope processDefault(*processSession);

    MediaSessionState* seen = nullptr;
    std::thread unbound([&seen] { seen = &MediaState(); });
    unbound.join();
    EXPECT_EQ(seen, processSession.get());
}

TEST(MediaSessionStateTest, SessionsBoundOnTwoThreadsSeeOnlyTheirOwnState) {
    auto first = std::make_unique<MediaSessionState>();
    auto second = std::make_unique<MediaSessionState>();
    first->recordingId = "first";
    second->recordingId = "second";

    // Both threads hold their binding at the same time before either reads state.
    std::latch bothBound(2);
    std::string firstSeen;
    std::string secondSeen;
    std::thread firstThread([&] {
        MediaSessionBinding binding(*first);
        bothBound.arrive_and_wait();
        MediaState().recordingHealthFlags.store(1, std::memory_order_relaxed);
        firstSeen = MediaState().recordingId;
    });
    std::thread secondThread([&] {
        MediaSessionBinding binding(*second);
        bothBound.arrive_and_wait();
        MediaState().recordingHealthFlags.store(2, std::memory_order_relaxed);
        secondSeen = MediaState().recordingId;
    });
    firstThread.join();
    secondThread.join();

    EXPECT_EQ(firstSeen, "first");
    EXPECT_EQ(secondSeen, "second");
    EXPECT_EQ(first->recordingHealthFlags.load(), 1u);
    EXPECT_EQ(second->recordingHealthFlags.load(), 2u);
}

TEST(MediaSessionStateTest, SessionScopeBindsItsSessionOnlyAroundTheBody) {
    auto session = std::make_unique<MediaSessionState>();
    auto processSession = std::make_unique<MediaSessionState>();
    MediaProcessDefaultScope processDefault(*processSession);

    const ce::ThreadScope scope = MediaSessionScope(*session);
    MediaSessionState* seen = nullptr;
    scope.Run([&seen] { seen = &MediaState(); });
    EXPECT_EQ(seen, session.get());
    EXPECT_EQ(&MediaState(), processSession.get());
}

TEST(ThreadScopeTest, UnsetScopeRunsTheBodyInPlace) {
    const ce::ThreadScope unset;
    bool ran = false;
    unset.Run([&ran] { ran = true; });
    EXPECT_TRUE(ran);
}

TEST(MediaSessionStateTest, BoundSessionSinkReceivesOnlyItsThreadsLines) {
    const std::filesystem::path path = UniqueLogPath("sink");
    auto session = std::make_unique<MediaSessionState>();
    session->logSink = ce::log::OpenSink(path.string());
    ASSERT_TRUE(session->logSink);

    std::thread worker([&session] {
        MediaSessionBinding binding(*session);
        LogInfo("[MediaSessionTest] bound-thread line");
    });
    worker.join();
    // This thread is unbound and no process log is open, so the line goes nowhere.
    LogInfo("[MediaSessionTest] unbound-thread line");

    session->logSink.reset();
    const std::string text = ReadText(path);
    EXPECT_NE(text.find("[MediaSessionTest] bound-thread line"), std::string::npos) << text;
    EXPECT_EQ(text.find("[MediaSessionTest] unbound-thread line"), std::string::npos) << text;

    std::error_code error;
    std::filesystem::remove(path, error);
}

// The shared capture units are also linked into the runtime (cengine.dll), where screenshots run on the engine
// thread with no media session. They reach the session only through the ThreadScope their owner passes in.
TEST(MediaSessionStateTest, SharedCaptureUnitsNeverNameTheMediaSession) {
    const std::filesystem::path media = std::filesystem::current_path() / "captureengine" / "media";
    size_t checked = 0;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(media, error)) {
        const std::string name = entry.path().filename().string();
        const bool shared = name.rfind("wgc_capture", 0) == 0 || name.rfind("dxgi_dup_capture", 0) == 0 ||
                            name.rfind("screenshot", 0) == 0 || name.rfind("media_main_window", 0) == 0;
        if (!shared) {
            continue;
        }
        ++checked;
        const std::string text = ce::test_source::ReadFile(entry.path());
        EXPECT_EQ(text.find("MediaState()"), std::string::npos) << name;
        EXPECT_EQ(text.find("media_session_state.h"), std::string::npos) << name;
    }
    EXPECT_GT(checked, 0u);
}
