#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <windows.h>

#include "captureengine/media/recording_manifest.h"
#include "common/ipc/media_finalization.h"
#include "common/ipc/process_ipc.h"
#include "source_fragment_reader.h"

// How the media role reports a recording's outcome (common/ipc/media_finalization.h) once it is wired
// into the media process: the manifest fields, the inherited-handle argument, and the order and
// log-level independence of the publication. The pure contract is covered by test_media_finalization.cpp.

namespace {

using namespace ce::media_result;

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

std::filesystem::path FreshDirectory() {
    static std::atomic<uint32_t> sequence{1};
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / (L"ce_media_result_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                                                  std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed)));
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory);
    return directory;
}

std::string ReadFile(const std::filesystem::path& path) {
    // The manifest is written in text mode, so lines end in CR LF on Windows; compare LF-only text.
    std::ifstream input(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

bool HasLine(const std::string& text, const std::string& line) {
    return text.find("\n" + line + "\n") != std::string::npos || text.rfind(line + "\n", 0) == 0;
}

// The diagnostic recording id the media process carries (--recording-id); the manifest names files with it.
constexpr const char* kRecordingId = "r0042";

std::filesystem::path ManifestPath(const std::filesystem::path& directory) {
    return directory / ("recording_r0042_" + std::to_string(GetCurrentProcessId()) + ".manifest");
}

}  // namespace

TEST(MediaResultManifestTest, FinalizationFieldsFollowTheStartRecordInOneManifest) {
    const std::filesystem::path directory = FreshDirectory();
    const std::string logName = "media_r0042_" + std::to_string(GetCurrentProcessId()) + ".log";
    AppConfig config;
    WriteRecordingManifest(directory.string(), config, logName, kRecordingId);

    Result result;
    result.status = Status::kSaved;
    result.flags = kFlagAudioDegraded;
    result.finalizationCount = 1;
    result.paths.push_back("D:\\Captures\\capture_1.mkv");
    FinalizeRecordingManifest((directory / logName).string(), kRecordingId, false, true, "degraded", "audio", "audio",
                              2, 5, 9, 7, result);

    const std::string manifest = ReadFile(ManifestPath(directory));
    EXPECT_TRUE(HasLine(manifest, "status=media_process_started")) << manifest;
    EXPECT_TRUE(HasLine(manifest, "status=media_finalized")) << manifest;
    EXPECT_TRUE(HasLine(manifest, "finalization_result=saved")) << manifest;
    EXPECT_TRUE(HasLine(manifest, "finalization_flags=2")) << manifest;
    EXPECT_TRUE(HasLine(manifest, "media_exit_code=0x43450201")) << manifest;
    EXPECT_TRUE(HasLine(manifest, "output_files=1")) << manifest;
    EXPECT_TRUE(HasLine(manifest, "output_path=D:\\...\\capture_1.mkv")) << manifest;
    EXPECT_TRUE(HasLine(manifest, "output_saved=1")) << manifest;
    EXPECT_TRUE(HasLine(manifest, "finalization_complete=1")) << manifest;

    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

TEST(MediaResultManifestTest, EveryPathOfASegmentedOutputIsListed) {
    const std::filesystem::path directory = FreshDirectory();
    const std::string logName = "media_r0042_" + std::to_string(GetCurrentProcessId()) + ".log";
    Result result;
    result.status = Status::kSaved;
    result.finalizationCount = 1;
    result.paths = {"D:\\out\\part1.mkv", "D:\\out\\part2.mkv", "D:\\out\\part3.mkv"};
    FinalizeRecordingManifest((directory / logName).string(), kRecordingId, false, true, "healthy", "none", "none",
                              0, 0, 0, 0, result);

    const std::string manifest = ReadFile(ManifestPath(directory));
    EXPECT_TRUE(HasLine(manifest, "output_files=3")) << manifest;
    EXPECT_TRUE(HasLine(manifest, "output_path=D:\\...\\part1.mkv")) << manifest;
    EXPECT_TRUE(HasLine(manifest, "output_path_2=D:\\...\\part2.mkv")) << manifest;
    EXPECT_TRUE(HasLine(manifest, "output_path_3=D:\\...\\part3.mkv")) << manifest;

    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

TEST(MediaResultManifestTest, FailureAndCancellationNameNoOutput) {
    for (const Status status : {Status::kFailed, Status::kCanceled}) {
            const std::filesystem::path directory = FreshDirectory();
        const std::string logName = "media_r0042_" + std::to_string(GetCurrentProcessId()) + ".log";
        Result result;
        result.status = status;
        result.finalizationCount = 1;
        FinalizeRecordingManifest((directory / logName).string(), kRecordingId, status == Status::kCanceled, false, "healthy",
                                  "none", "none", 0, 0, 0, 0, result);

        const std::string manifest = ReadFile(ManifestPath(directory));
        EXPECT_TRUE(HasLine(manifest, std::string("finalization_result=") + StatusName(status))) << manifest;
        EXPECT_TRUE(HasLine(manifest, "output_files=0")) << manifest;
        EXPECT_EQ(manifest.find("output_path"), std::string::npos) << manifest;
        EXPECT_TRUE(HasLine(manifest, "output_saved=0")) << manifest;

        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
}

TEST(MediaResultManifestTest, UserProfileDirectoriesNeverReachTheManifest) {
    const std::filesystem::path directory = FreshDirectory();
    const std::string logName = "media_r0042_" + std::to_string(GetCurrentProcessId()) + ".log";
    Result result;
    result.status = Status::kSaved;
    result.finalizationCount = 1;
    result.paths.push_back("C:\\Users\\SomeAccountName\\Videos\\Private Folder\\capture_9.mkv");
    FinalizeRecordingManifest((directory / logName).string(), kRecordingId, false, true, "healthy", "none", "none",
                              0, 0, 0, 0, result);

    const std::string manifest = ReadFile(ManifestPath(directory));
    EXPECT_EQ(manifest.find("SomeAccountName"), std::string::npos) << manifest;
    EXPECT_EQ(manifest.find("Private Folder"), std::string::npos) << manifest;
    EXPECT_TRUE(HasLine(manifest, "output_path=C:\\...\\capture_9.mkv")) << manifest;

    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

TEST(MediaResultManifestTest, NoLogPathMeansNoManifestButTheResultStillExists) {
    // log_level=none: the media process has no log path, so no manifest and no logs directory exist.
    // The outcome must not depend on that; it is built and encoded from the same inputs regardless.
    const std::filesystem::path directory = FreshDirectory();
    FinalizationInputs inputs;
    inputs.outputSaved = true;
    inputs.outputPath = "D:\\Captures\\capture_2.mkv";
    const Result result = BuildResult(inputs, 0);
    FinalizeRecordingManifest(std::string(), kRecordingId, false, true, "healthy", "none", "none", 0, 0, 0, 0, result);

    EXPECT_TRUE(std::filesystem::is_empty(directory)) << "no log path: nothing may be written";
    EXPECT_EQ(DecodeExitCode(EncodeExitCode(result.status, result.flags)).status, Status::kSaved);
    ASSERT_EQ(result.paths.size(), 1u);

    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

TEST(MediaResultHandleArgumentTest, ParsesTheOneNamedHandle) {
    const wchar_t* arguments[] = {L"captureengine.exe", L"--mode=media", L"--avsync-latency-handle=0x10",
                                  L"--media-result-handle=0x1A4"};
    EXPECT_EQ(ParseInheritedHandleArgument(arguments, 4, L"--media-result-handle="),
              reinterpret_cast<void*>(static_cast<uintptr_t>(0x1A4)));
    EXPECT_EQ(ParseInheritedHandleArgument(arguments, 4, L"--avsync-latency-handle="),
              reinterpret_cast<void*>(static_cast<uintptr_t>(0x10)))
        << "the two inherited-handle arguments never read each other's value";

    const wchar_t* decimal[] = {L"captureengine.exe", L"--media-result-handle=420"};
    EXPECT_EQ(ParseInheritedHandleArgument(decimal, 2, L"--media-result-handle="),
              reinterpret_cast<void*>(static_cast<uintptr_t>(420)));
}

TEST(MediaResultHandleArgumentTest, AbsentRepeatedOrMalformedArgumentsYieldNoHandle) {
    const wchar_t* absent[] = {L"captureengine.exe", L"--mode=media"};
    EXPECT_EQ(ParseInheritedHandleArgument(absent, 2, L"--media-result-handle="), nullptr);

    const wchar_t* repeated[] = {L"captureengine.exe", L"--media-result-handle=0x1", L"--media-result-handle=0x2"};
    EXPECT_EQ(ParseInheritedHandleArgument(repeated, 3, L"--media-result-handle="), nullptr)
        << "two values are ambiguous; the child must not guess which slot is its own";

    for (const wchar_t* malformed : {L"--media-result-handle=", L"--media-result-handle=0xZZ",
                                     L"--media-result-handle=12abc", L"--media-result-handle=-1",
                                     L"--media-result-handle=0", L"--media-result-handle=0xFFFFFFFFFFFFFFFF",
                                     L"--media-result-handle=99999999999999999999999"}) {
        const wchar_t* arguments[] = {L"captureengine.exe", malformed};
        EXPECT_EQ(ParseInheritedHandleArgument(arguments, 2, L"--media-result-handle="), nullptr) << malformed;
    }

    // Index 0 is the program name and is never an argument.
    const wchar_t* programOnly[] = {L"--media-result-handle=0x5"};
    EXPECT_EQ(ParseInheritedHandleArgument(programOnly, 1, L"--media-result-handle="), nullptr);
}

TEST(MediaResultOutputPathCopyTest, FollowsTheSizeThenFillContract) {
    const std::string path = "D:\\Captures\\capture_3.mkv";
    EXPECT_EQ(CopyOutputPath(path, nullptr, 0), path.size());

    char buffer[64];
    memset(buffer, 'x', sizeof(buffer));
    EXPECT_EQ(CopyOutputPath(path, buffer, static_cast<uint32_t>(path.size() + 1)), path.size());
    EXPECT_STREQ(buffer, path.c_str());

    // One byte short of path plus NUL: the length is reported and nothing is written.
    memset(buffer, 'x', sizeof(buffer));
    EXPECT_EQ(CopyOutputPath(path, buffer, static_cast<uint32_t>(path.size())), path.size());
    EXPECT_EQ(buffer[0], 'x') << "a too-small buffer must never be partially filled";

    EXPECT_EQ(CopyOutputPath(std::string(), buffer, sizeof(buffer)), 0u);
}

TEST(MediaResultWiringSourceTest, TheSpawnerHandsTheSlotOverForOneLaunchOnly) {
    const std::string spawn = ReadSource("common/ipc/process_ipc_client.cpp");
    ASSERT_FALSE(spawn.empty());
    EXPECT_NE(spawn.find("--media-result-handle=0x%llX"), std::string::npos);
    EXPECT_NE(spawn.find("mediaResult->ChildHandle()"), std::string::npos);
    // The inherit flag is scoped to the launch attempt, whichever way it ends.
    EXPECT_NE(spawn.find("class ScopedInheritableHandle"), std::string::npos);
    EXPECT_NE(spawn.find("SetHandleInformation(handle_, HANDLE_FLAG_INHERIT, 0)"), std::string::npos);
    // Only a media child is given the slot.
    EXPECT_NE(spawn.find("mode == ProcessMode::Media && mediaResult"), std::string::npos);
}

TEST(MediaResultWiringSourceTest, TheMediaProcessAttachesTheSlotAndExitsWithTheCodedResult) {
    const std::string media = ReadSource("captureengine/media/media_main.cpp");
    ASSERT_FALSE(media.empty());
    EXPECT_NE(media.find("ce::media_result::MapInheritedSlot(ParseInheritedMediaResultHandle())"), std::string::npos);

    const size_t run = media.find("int MediaProcessSession::Run(const AppConfig& initialConfig)");
    ASSERT_NE(run, std::string::npos);
    const size_t shutdown = media.find("Shutdown();", run);
    const size_t encode = media.find("ce::media_result::EncodeExitCode(MediaState().lastFinalization.status", run);
    ASSERT_NE(shutdown, std::string::npos);
    ASSERT_NE(encode, std::string::npos);
    EXPECT_LT(shutdown, encode) << "the exit code is the outcome after teardown, not a guess before it";
    EXPECT_NE(media.find("return static_cast<int>(exitCode);", encode), std::string::npos);
}

TEST(MediaResultWiringSourceTest, FinalizationPublishesTheResultBeforeAnyLogDependentArtifact) {
    const std::string media = ReadSource("captureengine/media/media_main.cpp");
    ASSERT_FALSE(media.empty());

    const size_t helper = media.find("ce::media_result::Result PublishFinalizationResult(");
    ASSERT_NE(helper, std::string::npos);
    const size_t helperEnd = media.find("}  // namespace", helper);
    ASSERT_NE(helperEnd, std::string::npos);
    const std::string helperBody = media.substr(helper, helperEnd - helper);
    EXPECT_EQ(helperBody.find("IsAnyLoggingEnabled"), std::string::npos)
        << "the slot and the exit code must not depend on the log level";
    EXPECT_NE(helperBody.find("PublishResult(*MediaState().resultSlot, result)"), std::string::npos);
    EXPECT_NE(helperBody.find("MediaState().lastFinalization = result;"), std::string::npos);

    const size_t funnel = media.find("void CompleteRecordingFinalization(bool canceled, bool outputSaved)");
    ASSERT_NE(funnel, std::string::npos);
    const size_t publish = media.find("PublishFinalizationResult(finalizationInputs)", funnel);
    const size_t manifest = media.find("FinalizeRecordingManifest(", funnel);
    ASSERT_NE(publish, std::string::npos);
    ASSERT_NE(manifest, std::string::npos);
    EXPECT_LT(publish, manifest);
    // A live stream never asks the engine for a path: the endpoint carries the stream key.
    EXPECT_NE(media.find("if (outputSaved && !liveStream)", funnel), std::string::npos);
}

TEST(MediaResultWiringSourceTest, TheEnginePublishesAPathOnlyForAFinalizedFileAndNeverForAStream) {
    const std::string stop = ReadSource("mediaengine/engine/mediaengine_recording_stop.cpp");
    ASSERT_FALSE(stop.empty());
    const size_t clearAtStart = stop.find("lastOutputPath.clear();");
    const size_t firstReturn = stop.find("return ");
    ASSERT_NE(clearAtStart, std::string::npos);
    EXPECT_LT(clearAtStart, firstReturn) << "a stop that publishes nothing must not report the previous file";
    EXPECT_NE(stop.find("if (videoOutputPublished)\n                lastOutputPath = videoEnc->PublishedOutputPath();"),
              std::string::npos);
    EXPECT_NE(stop.find("if (audioOutputPublished)\n                    lastOutputPath = audioOnlyPath;"),
              std::string::npos);

    const std::string encoder = ReadSource("mediaengine/video/video_encoder_lifecycle.cpp");
    ASSERT_FALSE(encoder.empty());
    const size_t accessor = encoder.find("std::string VideoEncoder::PublishedOutputPath() const");
    ASSERT_NE(accessor, std::string::npos);
    EXPECT_NE(encoder.find("outputPublished.load(std::memory_order_acquire) && !liveOutput", accessor),
              std::string::npos);

    const std::string api = ReadSource("mediaengine/engine/mediaengine.h");
    EXPECT_NE(api.find("MEDIAENGINE_API uint32_t MediaEngine_GetLastOutputPath(char* buffer, uint32_t capacity);"),
              std::string::npos);
}
