#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>

#include "common/ipc/media_finalization.h"

// The media finalization result is the only way the spawner of a media child learns whether a
// recording was saved and where, and it must work at every log level. These cover the pure contract
// (exit codes, slot encoding, the exit-code/slot resolution) and the Win32 slot round trip through a
// real inherited-handle mapping.

namespace {

using namespace ce::media_result;

std::unique_ptr<Slot> MakeSlot() {
    auto slot = std::make_unique<Slot>();
    InitSlot(*slot);
    return slot;
}

Result Saved(const std::string& path, uint32_t count = 1, uint32_t flags = 0) {
    Result result;
    result.status = Status::kSaved;
    result.flags = flags;
    result.finalizationCount = count;
    result.paths.push_back(path);
    return result;
}

}  // namespace

TEST(MediaFinalizationExitCodeTest, EveryEncodableStatusRoundTripsWithItsFlags) {
    const uint32_t flagSets[] = {0u, kFlagVideoDegraded, kFlagAudioDegraded,
                                 kFlagVideoDegraded | kFlagAudioDegraded, kFlagLiveStream, kFlagPathOmitted};
    for (const Status status : {Status::kSaved, Status::kFailed, Status::kCanceled}) {
        for (const uint32_t flags : flagSets) {
            const uint32_t code = EncodeExitCode(status, flags);
            const DecodedExit decoded = DecodeExitCode(code);
            EXPECT_EQ(decoded.kind, ExitKind::kResult) << std::hex << code;
            EXPECT_EQ(decoded.status, status) << std::hex << code;
            EXPECT_EQ(decoded.flags, flags) << std::hex << code;
        }
    }
}

TEST(MediaFinalizationExitCodeTest, LegacyCodesKeepTheirMeaning) {
    // A child that ends its loop without finalizing a recording has always exited 0, and an init
    // failure has always exited 1. Neither may become a coded result.
    EXPECT_EQ(EncodeExitCode(Status::kNone, 0), 0u);
    EXPECT_EQ(DecodeExitCode(0u).kind, ExitKind::kClean);
    EXPECT_EQ(DecodeExitCode(0u).status, Status::kNone);

    EXPECT_EQ(DecodeExitCode(kExitCodeInitFailed).kind, ExitKind::kAbnormal);
    EXPECT_EQ(DecodeExitCode(kExitCodeInitFailed).status, Status::kLost);
    // kLost is the spawner's verdict, never something a child can say about itself.
    EXPECT_EQ(EncodeExitCode(Status::kLost, 0), kExitCodeInitFailed);
}

TEST(MediaFinalizationExitCodeTest, CrashAndKillCodesNeverDecodeAsResults) {
    // Crash codes are NTSTATUS values (0xC...), a forced TerminateProcess uses 1, and a controller that
    // restarts itself terminates with 0 or small Win32 codes. None may look like a coded result.
    for (const uint32_t code : {0xC0000005u, 0xC000013Au, 0x80000003u, 0xE06D7363u, 0x40010005u, 1u, 2u, 5u,
                                0x00000103u, 0xFFFFFFFFu, 0x4345FFFFu, 0x43450000u, 0x43450005u}) {
        const DecodedExit decoded = DecodeExitCode(code);
        EXPECT_EQ(decoded.kind, ExitKind::kAbnormal) << std::hex << code;
        EXPECT_EQ(decoded.status, Status::kLost) << std::hex << code;
    }
}

TEST(MediaFinalizationBuildTest, SavedFileCarriesItsPathAndHealth) {
    FinalizationInputs inputs;
    inputs.outputSaved = true;
    inputs.outputPath = "D:\\Captures\\capture_1.mkv";
    inputs.videoDegraded = true;
    const Result result = BuildResult(inputs, 0);
    EXPECT_EQ(result.status, Status::kSaved);
    EXPECT_EQ(result.flags, kFlagVideoDegraded);
    ASSERT_EQ(result.paths.size(), 1u);
    EXPECT_EQ(result.paths[0], inputs.outputPath);
    EXPECT_EQ(result.finalizationCount, 1u);
}

TEST(MediaFinalizationBuildTest, SavedFileWithoutAPathIsMarkedNotSilentlyEmpty) {
    FinalizationInputs inputs;
    inputs.outputSaved = true;
    const Result result = BuildResult(inputs, 2);
    EXPECT_EQ(result.status, Status::kSaved);
    EXPECT_TRUE(result.paths.empty());
    EXPECT_NE(result.flags & kFlagPathOmitted, 0u);
    EXPECT_EQ(result.finalizationCount, 3u);
}

TEST(MediaFinalizationBuildTest, LiveStreamNeverReportsAPath) {
    // The engine's output target for a live stream is the endpoint, which carries the stream key.
    FinalizationInputs inputs;
    inputs.outputSaved = true;
    inputs.liveStream = true;
    inputs.outputPath = "rtmps://live.example/app/SECRET_KEY";
    const Result result = BuildResult(inputs, 0);
    EXPECT_EQ(result.status, Status::kSaved);
    EXPECT_TRUE(result.paths.empty());
    EXPECT_NE(result.flags & kFlagLiveStream, 0u);
    EXPECT_EQ(result.flags & kFlagPathOmitted, 0u) << "a stream has no file; that is not an omission";
}

TEST(MediaFinalizationBuildTest, FailureAndCancellationPublishNoPathAndNoHealth) {
    FinalizationInputs failed;
    failed.outputPath = "D:\\Captures\\staging.mkv";  // whatever the engine still reports is not an output
    failed.videoDegraded = true;
    const Result failure = BuildResult(failed, 0);
    EXPECT_EQ(failure.status, Status::kFailed);
    EXPECT_TRUE(failure.paths.empty());
    EXPECT_EQ(failure.flags, 0u);

    FinalizationInputs canceled = failed;
    canceled.canceled = true;
    const Result cancellation = BuildResult(canceled, 0);
    EXPECT_EQ(cancellation.status, Status::kCanceled);
    EXPECT_TRUE(cancellation.paths.empty());
}

TEST(MediaFinalizationBuildTest, PublishedFileWinsOverACancelRequest) {
    // A stop that arrives after the first frame committed keeps the recording; the file on disk decides.
    FinalizationInputs inputs;
    inputs.canceled = true;
    inputs.outputSaved = true;
    inputs.outputPath = "D:\\Captures\\capture_2.mkv";
    const Result result = BuildResult(inputs, 0);
    EXPECT_EQ(result.status, Status::kSaved);
    ASSERT_EQ(result.paths.size(), 1u);
}

TEST(MediaFinalizationSlotTest, FreshSlotHoldsNoResult) {
    const auto slot = MakeSlot();
    EXPECT_TRUE(IsSlotCompatible(*slot));
    Result out;
    EXPECT_FALSE(ReadResult(*slot, out)) << "an unpublished slot must read as 'not published', not as a result";
    EXPECT_EQ(slot->sequence.load() % 2, 0u);
}

TEST(MediaFinalizationSlotTest, PublishedResultRoundTripsWithUnicodePath) {
    const auto slot = MakeSlot();
    const std::string path = "D:\\Aufnahmen\\\xC3\xA4\xC3\xB6\xC3\xBC \xE6\x97\xA5\xE6\x9C\xAC\\capture_3.mkv";
    ASSERT_TRUE(PublishResult(*slot, Saved(path, 1, kFlagAudioDegraded)));
    EXPECT_EQ(slot->sequence.load() % 2, 0u) << "the slot must be left in a stable (even) state";

    Result out;
    ASSERT_TRUE(ReadResult(*slot, out));
    EXPECT_EQ(out.status, Status::kSaved);
    EXPECT_EQ(out.flags, kFlagAudioDegraded);
    EXPECT_EQ(out.finalizationCount, 1u);
    ASSERT_EQ(out.paths.size(), 1u);
    EXPECT_EQ(out.paths[0], path);
}

TEST(MediaFinalizationSlotTest, SegmentedOutputsKeepEveryPathInOrder) {
    const auto slot = MakeSlot();
    Result result;
    result.status = Status::kSaved;
    result.finalizationCount = 1;
    for (size_t i = 0; i < kMaxPaths; ++i)
        result.paths.push_back("E:\\out\\segment_" + std::to_string(i) + ".mkv");
    ASSERT_TRUE(PublishResult(*slot, result));

    Result out;
    ASSERT_TRUE(ReadResult(*slot, out));
    EXPECT_EQ(out.paths, result.paths);
}

TEST(MediaFinalizationSlotTest, ALaterFinalizationReplacesTheEarlierOneWholly) {
    const auto slot = MakeSlot();
    ASSERT_TRUE(PublishResult(*slot, Saved("D:\\Captures\\a_much_longer_first_name_than_the_second.mkv", 1)));
    ASSERT_TRUE(PublishResult(*slot, Saved("D:\\b.mkv", 2)));

    Result out;
    ASSERT_TRUE(ReadResult(*slot, out));
    EXPECT_EQ(out.finalizationCount, 2u);
    ASSERT_EQ(out.paths.size(), 1u);
    EXPECT_EQ(out.paths[0], "D:\\b.mkv") << "no remainder of the longer earlier path may survive";
}

TEST(MediaFinalizationSlotTest, APathThatCannotFitIsOmittedNeverTruncated) {
    const auto slot = MakeSlot();
    const std::string tooLong = "D:\\" + std::string(kPathCapacity, 'x') + ".mkv";
    EXPECT_FALSE(PublishResult(*slot, Saved(tooLong))) << "the caller must learn the path was not stored";

    Result out;
    ASSERT_TRUE(ReadResult(*slot, out)) << "status and flags still arrive";
    EXPECT_EQ(out.status, Status::kSaved);
    EXPECT_TRUE(out.paths.empty());
    EXPECT_NE(out.flags & kFlagPathOmitted, 0u);
}

TEST(MediaFinalizationSlotTest, TooManyOrMalformedPathsAreOmittedWhole) {
    Result tooMany;
    tooMany.status = Status::kSaved;
    tooMany.finalizationCount = 1;
    for (size_t i = 0; i <= kMaxPaths; ++i)
        tooMany.paths.push_back("a" + std::to_string(i));
    Result emptyEntry = Saved("");
    Result embeddedNul = Saved(std::string("D:\\a\0b.mkv", 10));

    for (const Result& candidate : {tooMany, emptyEntry, embeddedNul}) {
        const auto slot = MakeSlot();
        EXPECT_FALSE(PublishResult(*slot, candidate));
        Result out;
        ASSERT_TRUE(ReadResult(*slot, out));
        EXPECT_TRUE(out.paths.empty());
        EXPECT_NE(out.flags & kFlagPathOmitted, 0u);
    }
}

TEST(MediaFinalizationSlotTest, TornAndForeignSlotsAreNeverTrusted) {
    auto slot = MakeSlot();
    ASSERT_TRUE(PublishResult(*slot, Saved("D:\\c.mkv")));
    Result out;

    slot->sequence.fetch_add(1);  // a write in progress
    EXPECT_FALSE(ReadResult(*slot, out));
    slot->sequence.fetch_add(1);
    EXPECT_TRUE(ReadResult(*slot, out));

    slot->magic = 0;
    EXPECT_FALSE(IsSlotCompatible(*slot));
    EXPECT_FALSE(ReadResult(*slot, out));
    EXPECT_FALSE(PublishResult(*slot, Saved("D:\\d.mkv"))) << "an incompatible slot is never written";

    slot->magic = kSlotMagic;
    slot->pathCount = 99;  // corrupt: more paths than the contract allows
    EXPECT_FALSE(ReadResult(*slot, out));
    slot->pathCount = 1;
    slot->pathBytes = 3;  // corrupt: the byte count disagrees with the packed paths
    EXPECT_FALSE(ReadResult(*slot, out));
}

TEST(MediaFinalizationResolveTest, SlotIsAuthoritativeEvenWhenTheChildThenCrashed) {
    // The file was published and the result written; the crash came later, during teardown. The
    // recording is saved, and the crash is reported alongside rather than instead.
    Result published = Saved("D:\\e.mkv");
    const Resolution resolution = Resolve(0xC0000005u, &published);
    EXPECT_TRUE(resolution.fromSlot);
    EXPECT_TRUE(resolution.abnormalExit);
    EXPECT_EQ(resolution.result.status, Status::kSaved);
    ASSERT_EQ(resolution.result.paths.size(), 1u);
}

TEST(MediaFinalizationResolveTest, ExitCodeCarriesTheStatusWhenThereIsNoSlotResult) {
    const uint32_t code = EncodeExitCode(Status::kSaved, kFlagVideoDegraded);
    const Resolution noSlot = Resolve(code, nullptr);
    EXPECT_FALSE(noSlot.fromSlot);
    EXPECT_FALSE(noSlot.abnormalExit);
    EXPECT_EQ(noSlot.result.status, Status::kSaved);
    EXPECT_NE(noSlot.result.flags & kFlagVideoDegraded, 0u);
    EXPECT_NE(noSlot.result.flags & kFlagPathOmitted, 0u) << "saved, but this channel cannot name the file";
    EXPECT_TRUE(noSlot.result.paths.empty());

    Result unpublished;  // a slot the child never wrote
    const Resolution emptySlot = Resolve(code, &unpublished);
    EXPECT_FALSE(emptySlot.fromSlot);
    EXPECT_EQ(emptySlot.result.status, Status::kSaved);
}

TEST(MediaFinalizationResolveTest, LiveStreamOutputIsNotAPathOmission) {
    const Resolution resolution = Resolve(EncodeExitCode(Status::kSaved, kFlagLiveStream), nullptr);
    EXPECT_EQ(resolution.result.status, Status::kSaved);
    EXPECT_EQ(resolution.result.flags & kFlagPathOmitted, 0u);
}

TEST(MediaFinalizationResolveTest, ChildThatSaidNothingIsLostOrCleanNotSaved) {
    const Resolution killed = Resolve(1u, nullptr);
    EXPECT_EQ(killed.result.status, Status::kLost);
    EXPECT_TRUE(killed.abnormalExit);

    const Resolution crashed = Resolve(0xC0000005u, nullptr);
    EXPECT_EQ(crashed.result.status, Status::kLost);

    const Resolution idle = Resolve(0u, nullptr);
    EXPECT_EQ(idle.result.status, Status::kNone);
    EXPECT_FALSE(idle.abnormalExit);
}

TEST(MediaFinalizationResolveTest, DisagreementBetweenTheChannelsIsReportedNotHidden) {
    Result slotResult = Saved("D:\\f.mkv");
    const Resolution resolution = Resolve(EncodeExitCode(Status::kFailed, 0), &slotResult);
    EXPECT_TRUE(resolution.fromSlot);
    EXPECT_TRUE(resolution.statusDisagreed);
    EXPECT_EQ(resolution.result.status, Status::kSaved);

    EXPECT_FALSE(Resolve(EncodeExitCode(Status::kSaved, 0), &slotResult).statusDisagreed);
}

TEST(MediaFinalizationResolveTest, ChildOutcomeRoundTripsThroughBothChannels) {
    // End to end through the contract: what the child builds is what the spawner resolves.
    FinalizationInputs inputs;
    inputs.outputSaved = true;
    inputs.audioDegraded = true;
    inputs.outputPath = "D:\\Captures\\capture_4.mkv";
    const Result built = BuildResult(inputs, 0);

    auto slot = MakeSlot();
    ASSERT_TRUE(PublishResult(*slot, built));
    Result read;
    ASSERT_TRUE(ReadResult(*slot, read));
    const Resolution resolution = Resolve(EncodeExitCode(built.status, built.flags), &read);
    EXPECT_TRUE(resolution.fromSlot);
    EXPECT_FALSE(resolution.statusDisagreed);
    EXPECT_EQ(resolution.result.status, Status::kSaved);
    EXPECT_EQ(resolution.result.flags, kFlagAudioDegraded);
    EXPECT_EQ(resolution.result.paths, built.paths);
}

// The Win32 wrapper: the spawner creates the slot, an inheriting child maps it through a duplicate of
// the handle (the same-process stand-in for handle inheritance), publishes, and the spawner reads.
TEST(MediaFinalizationChannelTest, ChildPublicationReachesTheSpawnerThroughTheMapping) {
    const auto spawnerSlot = ResultSlot::Create();
    ASSERT_NE(spawnerSlot, nullptr);
    ASSERT_NE(spawnerSlot->ChildHandle(), nullptr);

    Result before;
    EXPECT_FALSE(spawnerSlot->Read(before)) << "nothing published yet";

    HANDLE childCopy = nullptr;
    ASSERT_TRUE(DuplicateHandle(GetCurrentProcess(), static_cast<HANDLE>(spawnerSlot->ChildHandle()),
                                GetCurrentProcess(), &childCopy, 0, FALSE, DUPLICATE_SAME_ACCESS));
    Slot* childView = MapInheritedSlot(childCopy);  // takes ownership of childCopy
    ASSERT_NE(childView, nullptr);

    ASSERT_TRUE(PublishResult(*childView, Saved("D:\\Captures\\capture_5.mkv", 1, kFlagVideoDegraded)));
    UnmapInheritedSlot(childView);

    Result after;
    ASSERT_TRUE(spawnerSlot->Read(after));
    EXPECT_EQ(after.status, Status::kSaved);
    EXPECT_EQ(after.flags, kFlagVideoDegraded);
    ASSERT_EQ(after.paths.size(), 1u);
    EXPECT_EQ(after.paths[0], "D:\\Captures\\capture_5.mkv");
}

TEST(MediaFinalizationChannelTest, AbsentOrForeignHandlesMapToNothing) {
    EXPECT_EQ(MapInheritedSlot(nullptr), nullptr);
    EXPECT_EQ(MapInheritedSlot(INVALID_HANDLE_VALUE), nullptr);

    // A mapping that is not a result slot (wrong magic) is refused and its handle is still consumed.
    HANDLE foreign = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Slot), nullptr);
    ASSERT_NE(foreign, nullptr);
    EXPECT_EQ(MapInheritedSlot(foreign), nullptr);
    DWORD handleFlags = 0;
    EXPECT_FALSE(GetHandleInformation(foreign, &handleFlags)) << "the foreign handle must have been closed";
}
