#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

#include "../common/installer_setup_policy.h"
#include "../installer/install_policy.h"
#include "../installer/payload_format.h"

namespace {

using namespace ce::setup;

// ---------------------------------------------------------------------------
// Container builder mirroring tools/installer_payload.py
// ---------------------------------------------------------------------------

struct TestFile {
    std::string path;
    std::string content;
    Method method = Method::Store;
};

struct Container {
    std::vector<uint8_t> bytes;
    Footer footer;
};

std::vector<uint8_t> BuildIndex(const std::vector<FileEntry>& entries) {
    std::vector<uint8_t> index;
    for (const FileEntry& entry : entries) {
        WriteLe16(index, static_cast<uint16_t>(entry.path.size()));
        index.insert(index.end(), entry.path.begin(), entry.path.end());
        WriteLe32(index, static_cast<uint32_t>(entry.method));
        WriteLe32(index, entry.flags);
        WriteLe64(index, entry.offset);
        WriteLe64(index, entry.storedSize);
        WriteLe64(index, entry.size);
        WriteLe32(index, entry.crc);
        WriteLe32(index, 0);
    }
    return index;
}

std::vector<uint8_t> BuildFooter(uint64_t payloadOffset, uint64_t indexOffset, const std::vector<uint8_t>& index,
                                 uint64_t total, uint32_t count) {
    std::vector<uint8_t> footer(kFooterMagic, kFooterMagic + sizeof(kFooterMagic));
    WriteLe32(footer, kFormatVersion);
    WriteLe32(footer, 0);
    WriteLe64(footer, payloadOffset);
    WriteLe64(footer, indexOffset);
    WriteLe64(footer, index.size());
    WriteLe64(footer, total);
    WriteLe32(footer, count);
    WriteLe32(footer, Crc32Update(0, index.data(), index.size()));
    WriteLe32(footer, 0);
    WriteLe32(footer, Crc32Update(0, footer.data(), footer.size()));
    return footer;
}

std::vector<FileEntry> StoredEntries(const std::vector<TestFile>& files, uint64_t base, std::vector<uint8_t>* area) {
    std::vector<FileEntry> entries;
    for (const TestFile& file : files) {
        FileEntry entry;
        entry.path = file.path;
        entry.method = Method::Store;
        entry.offset = base + area->size();
        entry.storedSize = file.content.size();
        entry.size = file.content.size();
        entry.crc = Crc32Update(0, file.content.data(), file.content.size());
        area->insert(area->end(), file.content.begin(), file.content.end());
        entries.push_back(entry);
    }
    return entries;
}

// Returns the finished bytes; `mutate` may edit the entries before indexing.
std::vector<uint8_t> BuildFile(const std::vector<TestFile>& files,
                               const std::function<void(std::vector<FileEntry>&)>& mutate = {},
                               uint64_t totalDelta = 0) {
    std::vector<uint8_t> file(128, 0);  // stand-in stub
    file[0] = 'M';
    file[1] = 'Z';
    std::vector<uint8_t> area;
    std::vector<FileEntry> entries = StoredEntries(files, file.size(), &area);
    if (mutate)
        mutate(entries);
    file.insert(file.end(), area.begin(), area.end());
    const uint64_t indexOffset = file.size();
    const std::vector<uint8_t> index = BuildIndex(entries);
    file.insert(file.end(), index.begin(), index.end());
    uint64_t total = 0;
    for (const FileEntry& entry : entries)
        total += entry.size;
    const std::vector<uint8_t> footer = BuildFooter(128, indexOffset, index, total + totalDelta, static_cast<uint32_t>(entries.size()));
    file.insert(file.end(), footer.begin(), footer.end());
    return file;
}

PayloadStatus Parse(const std::vector<uint8_t>& file, std::vector<FileEntry>* entries = nullptr) {
    Footer footer;
    const PayloadStatus footerStatus = ParseFooter(file.data() + file.size() - kFooterSize, file.size(), &footer);
    if (footerStatus != PayloadStatus::Ok)
        return footerStatus;
    std::vector<FileEntry> local;
    return ParseIndex(file.data() + footer.indexOffset, static_cast<size_t>(footer.indexSize), footer,
                      entries ? entries : &local);
}

// ---------------------------------------------------------------------------
// payload_format.h
// ---------------------------------------------------------------------------

TEST(InstallerPayloadTest, Crc32MatchesTheStandardCheckValue) {
    EXPECT_EQ(Crc32Update(0, "123456789", 9), 0xCBF43926u);
    // Streaming in two parts equals one pass.
    EXPECT_EQ(Crc32Update(Crc32Update(0, "1234", 4), "56789", 5), 0xCBF43926u);
    EXPECT_EQ(Crc32Update(0, "", 0), 0u);
}

TEST(InstallerPayloadTest, ParsesAValidContainer) {
    const auto file = BuildFile({{"captureengine.exe", "MZ-exe"}, {"ffmpeg/avcodec-63.dll", "dll"}, {"config.ini", ""}});
    std::vector<FileEntry> entries;
    ASSERT_EQ(Parse(file, &entries), PayloadStatus::Ok);
    ASSERT_EQ(entries.size(), 3u);
    EXPECT_EQ(entries[1].path, "ffmpeg/avcodec-63.dll");
    EXPECT_EQ(entries[1].size, 3u);
    EXPECT_EQ(entries[2].size, 0u);
}

TEST(InstallerPayloadTest, RejectsFootersThatAreMissingOrDamaged) {
    auto file = BuildFile({{"a.txt", "hello"}});
    Footer footer;
    EXPECT_EQ(ParseFooter(file.data(), kFooterSize - 1, &footer), PayloadStatus::TooSmall);

    auto noMagic = file;
    noMagic[noMagic.size() - kFooterSize] ^= 0xFF;
    EXPECT_EQ(Parse(noMagic), PayloadStatus::BadMagic);

    auto otherVersion = file;
    otherVersion[otherVersion.size() - kFooterSize + 8] = 9;
    EXPECT_EQ(Parse(otherVersion), PayloadStatus::BadVersion);

    auto flipped = file;
    flipped[flipped.size() - 30] ^= 0x01;
    EXPECT_EQ(Parse(flipped), PayloadStatus::BadFooterChecksum);
}

TEST(InstallerPayloadTest, RejectsABadIndexBeforeAnythingIsExtracted) {
    auto file = BuildFile({{"a.txt", "hello"}});
    Footer footer;
    ASSERT_EQ(ParseFooter(file.data() + file.size() - kFooterSize, file.size(), &footer), PayloadStatus::Ok);
    file[static_cast<size_t>(footer.indexOffset) + 3] ^= 0x01;
    EXPECT_EQ(Parse(file), PayloadStatus::BadIndexChecksum);
}

TEST(InstallerPayloadTest, RejectsPathsThatCouldLeaveTheInstallationFolder) {
    for (const char* hostile : {"../evil.dll", "a/../../evil.dll", "/abs.dll", "C:/x.dll", "a\\b.dll", "a//b.dll", ".", "..",
                                "nul", "dir/CON.txt", "name.", "name ", "a:stream"}) {
        const auto file = BuildFile({{"a.txt", "x"}}, [&](std::vector<FileEntry>& entries) { entries[0].path = hostile; });
        EXPECT_EQ(Parse(file), PayloadStatus::BadPath) << hostile;
    }
}

TEST(InstallerPayloadTest, RejectsDuplicatePathsIgnoringCase) {
    const auto file = BuildFile({{"A.txt", "x"}, {"a.TXT", "y"}});
    EXPECT_EQ(Parse(file), PayloadStatus::DuplicatePath);
}

TEST(InstallerPayloadTest, RejectsRangesOutsideOrOverlappingTheDataArea) {
    auto outside = BuildFile({{"a.txt", "hello"}}, [](std::vector<FileEntry>& entries) { entries[0].offset = 1u << 30; });
    EXPECT_EQ(Parse(outside), PayloadStatus::BadFileRange);

    auto beforeData = BuildFile({{"a.txt", "hello"}}, [](std::vector<FileEntry>& entries) { entries[0].offset = 4; });
    EXPECT_EQ(Parse(beforeData), PayloadStatus::BadFileRange);

    auto overlap = BuildFile({{"a.txt", "hello"}, {"b.txt", "world"}},
                             [](std::vector<FileEntry>& entries) { entries[1].offset = entries[0].offset + 2; });
    EXPECT_EQ(Parse(overlap), PayloadStatus::BadFileRange);

    auto wrongStoredSize = BuildFile({{"a.txt", "hello"}}, [](std::vector<FileEntry>& entries) { entries[0].storedSize = 4; });
    EXPECT_EQ(Parse(wrongStoredSize), PayloadStatus::BadFileRange);
}

TEST(InstallerPayloadTest, RejectsUnknownMethodsAndInconsistentTotals) {
    auto method = BuildFile({{"a.txt", "hello"}},
                            [](std::vector<FileEntry>& entries) { entries[0].method = static_cast<Method>(7); });
    EXPECT_EQ(Parse(method), PayloadStatus::BadMethod);

    // The footer's total must equal the sum of the files, and stay under the format limit.
    EXPECT_EQ(Parse(BuildFile({{"a.txt", "hello"}}, {}, 1)), PayloadStatus::BadIndex);
    EXPECT_EQ(Parse(BuildFile({{"a.txt", "hello"}}, {}, 9ull << 30)), PayloadStatus::TooLarge);
}

TEST(InstallerPayloadTest, LzmsStoredSizeIsBoundedByTheBlockFraming) {
    // 1 byte of content needs at least a 4-byte block header.
    auto tooSmall = BuildFile({{"a.txt", "x"}}, [](std::vector<FileEntry>& entries) {
        entries[0].method = Method::Lzms;
        entries[0].storedSize = 1;
    });
    EXPECT_EQ(Parse(tooSmall), PayloadStatus::BadFileRange);
}

TEST(InstallerPayloadTest, BlockFramingRules) {
    EXPECT_EQ(BlockCount(0), 0u);
    EXPECT_EQ(BlockCount(1), 1u);
    EXPECT_EQ(BlockCount(kBlockSize), 1u);
    EXPECT_EQ(BlockCount(kBlockSize + 1), 2u);
    EXPECT_TRUE(ValidBlockLength(10, kBlockSize));
    EXPECT_TRUE(ValidBlockLength(kBlockSize, kBlockSize));
    EXPECT_FALSE(ValidBlockLength(0, kBlockSize));
    EXPECT_FALSE(ValidBlockLength(kBlockSize + 1, kBlockSize));
}

TEST(InstallerPayloadTest, SafeRelativePathAcceptsOrdinaryNames) {
    for (const char* name : {"captureengine.exe", "ffmpeg/avcodec-63.dll", "plugins/LibreHardwareMonitor/PawnIO_setup.exe",
                             "licenses/MIT_CaptureEngine.txt", "caf\xC3\xA9.txt"})
        EXPECT_TRUE(IsSafeRelativePath(name)) << name;
    EXPECT_FALSE(IsSafeRelativePath(std::string(kMaxPathChars + 1, 'a')));
    EXPECT_FALSE(IsSafeRelativePath("bad\xFF.txt"));
    EXPECT_FALSE(IsSafeRelativePath("a/\x01/b"));
    EXPECT_FALSE(IsSafeRelativePath("a/lpt9.txt"));
}

// ---------------------------------------------------------------------------
// Command line and options
// ---------------------------------------------------------------------------

CommandLine Command(std::initializer_list<const wchar_t*> arguments) {
    std::vector<std::wstring> list;
    for (const wchar_t* argument : arguments)
        list.emplace_back(argument);
    return ParseCommandLine(list);
}

TEST(InstallerCommandLineTest, ConventionalSwitchesAreRecognised) {
    const CommandLine command = Command({L"/S", L"/D=C:\\Apps\\Capture Engine"});
    EXPECT_TRUE(command.valid);
    EXPECT_TRUE(command.silent);
    EXPECT_EQ(command.mode, Mode::Install);
    EXPECT_EQ(command.directory, L"C:\\Apps\\Capture Engine");
    EXPECT_TRUE(Command({L"--silent", L"--dir=D:\\x"}).silent);
}

TEST(InstallerCommandLineTest, OptionTogglesOverrideTheDefaults) {
    const CommandLine command = Command({L"--no-desktop", L"--no-autostart", L"--admin", L"--no-launch"});
    ASSERT_TRUE(command.valid);
    const uint32_t options = ApplyOverrides(kFreshDefaults, command);
    EXPECT_FALSE(options & kOptDesktopShortcut);
    EXPECT_FALSE(options & kOptAutostart);
    EXPECT_TRUE(options & kOptElevated);
    EXPECT_FALSE(options & kOptLaunch);
    EXPECT_TRUE(options & kOptStartMenuShortcut);
    EXPECT_TRUE(options & kOptService);
}

TEST(InstallerCommandLineTest, TheLastToggleWins) {
    const uint32_t options = ApplyOverrides(0, Command({L"--desktop", L"--no-desktop", L"--service"}));
    EXPECT_FALSE(options & kOptDesktopShortcut);
    EXPECT_TRUE(options & kOptService);
}

TEST(InstallerCommandLineTest, CloseTimeoutIsBoundedAndDefaultsToThirtySeconds) {
    EXPECT_EQ(Command({L"/S"}).closeTimeoutSeconds, 30u);
    EXPECT_EQ(Command({L"--close-timeout=1"}).closeTimeoutSeconds, 1u);
    EXPECT_EQ(Command({L"--close-timeout=300"}).closeTimeoutSeconds, 300u);
    EXPECT_FALSE(Command({L"--close-timeout=0"}).valid);
    EXPECT_FALSE(Command({L"--close-timeout=301"}).valid);
    EXPECT_FALSE(Command({L"--close-timeout="}).valid);
    EXPECT_FALSE(Command({L"--close-timeout=abc"}).valid);
    EXPECT_FALSE(Command({L"--close-timeout=-5"}).valid);
    EXPECT_FALSE(Command({L"--close-timeout=1000"}).valid);
}

TEST(InstallerCommandLineTest, FilesOnlyIsAnExplicitSwitch) {
    EXPECT_FALSE(Command({L"/S"}).filesOnly);
    EXPECT_TRUE(Command({L"--files-only", L"/S"}).filesOnly);
}

TEST(InstallerCommandLineTest, ModesAndErrors) {
    EXPECT_EQ(Command({L"--uninstall"}).mode, Mode::Uninstall);
    EXPECT_EQ(Command({L"/?"}).mode, Mode::Help);
    EXPECT_EQ(Command({L"--verify-payload"}).mode, Mode::VerifyPayload);
    EXPECT_EQ(Command({L"--extract=C:\\x"}).extractDirectory, L"C:\\x");
    EXPECT_TRUE(Command({L"--uninstall", L"--remove-data"}).removeUserData);
    EXPECT_FALSE(Command({L"--bogus"}).valid);
    EXPECT_FALSE(Command({L"--extract="}).valid);
    EXPECT_FALSE(Command({L"--preview="}).valid);
}

TEST(InstallerOptionsTest, FreshInstallOffersEverythingExceptAdministratorMode) {
    ExistingState state;
    const uint32_t options = DefaultOptions(state);
    EXPECT_TRUE(options & kOptDesktopShortcut);
    EXPECT_TRUE(options & kOptStartMenuShortcut);
    EXPECT_TRUE(options & kOptService);
    EXPECT_TRUE(options & kOptAutostart);
    EXPECT_TRUE(options & kOptPawnIo);
    EXPECT_FALSE(options & kOptElevated);
}

TEST(InstallerOptionsTest, AnInstalledDriverIsNotOfferedAgain) {
    ExistingState state;
    state.pawnIoInstalled = true;
    EXPECT_FALSE(DefaultOptions(state) & kOptPawnIo);
}

TEST(InstallerOptionsTest, AnUpdateKeepsEveryEarlierChoice) {
    ExistingState state;
    state.installed = true;
    state.hasPreferences = true;
    state.service = false;
    state.autostart = true;
    state.elevated = true;
    state.desktopShortcut = false;
    state.startMenuShortcut = true;
    state.pawnIoInstalled = true;
    const uint32_t options = DefaultOptions(state);
    EXPECT_FALSE(options & kOptService);
    EXPECT_TRUE(options & kOptAutostart);
    EXPECT_TRUE(options & kOptElevated);
    EXPECT_FALSE(options & kOptDesktopShortcut);
    EXPECT_TRUE(options & kOptStartMenuShortcut);
    EXPECT_FALSE(options & kOptPawnIo);
}

TEST(InstallerOptionsTest, AnUpdateWithoutStoredPreferencesFallsBackToTheDefaults) {
    ExistingState state;
    state.installed = true;
    const uint32_t options = DefaultOptions(state);
    EXPECT_TRUE(options & kOptService);
    EXPECT_TRUE(options & kOptAutostart);
}

// ---------------------------------------------------------------------------
// Install directory
// ---------------------------------------------------------------------------

TEST(InstallerDirectoryTest, NormalizationTrimsQuotesSlashesAndSeparators) {
    EXPECT_EQ(NormalizeDirectory(L"  \"C:\\Program Files\\Capture Engine\\\"  "), L"C:\\Program Files\\Capture Engine");
    EXPECT_EQ(NormalizeDirectory(L"C:/Program Files//Capture Engine/"), L"C:\\Program Files\\Capture Engine");
    EXPECT_EQ(NormalizeDirectory(L"C:\\"), L"C:\\");
    EXPECT_EQ(NormalizeDirectory(L"\\\\server\\share"), L"\\\\server\\share");
}

TEST(InstallerDirectoryTest, PathContainmentIsCaseInsensitiveAndRespectsBoundaries) {
    EXPECT_TRUE(IsPathInside(L"C:\\Program Files\\Capture Engine\\captureengine.exe", L"c:\\program files\\capture engine"));
    EXPECT_TRUE(IsPathInside(L"C:\\Program Files\\Capture Engine", L"C:\\Program Files\\Capture Engine"));
    // A sibling that merely shares a prefix is not inside.
    EXPECT_FALSE(IsPathInside(L"C:\\Program Files\\Capture Engine2\\x.exe", L"C:\\Program Files\\Capture Engine"));
    EXPECT_FALSE(IsPathInside(L"C:\\Other\\x.exe", L"C:\\Program Files\\Capture Engine"));
    EXPECT_FALSE(IsPathInside(L"C:\\x", L""));
}

TEST(InstallerDirectoryTest, ValidationAcceptsOrdinaryLocalFolders) {
    const std::wstring windows = L"C:\\Windows";
    EXPECT_EQ(ValidateInstallDirectory(L"C:\\Program Files\\Capture Engine", windows), DirectoryIssue::Ok);
    EXPECT_EQ(ValidateInstallDirectory(L"D:\\Games\\Capture Engine\\", windows), DirectoryIssue::Ok);
    EXPECT_EQ(ValidateInstallDirectory(L"\"E:\\Tools\\CE\"", windows), DirectoryIssue::Ok);
}

TEST(InstallerDirectoryTest, ValidationRejectsUnsafeOrUnusableTargets) {
    const std::wstring windows = L"C:\\Windows";
    EXPECT_EQ(ValidateInstallDirectory(L"", windows), DirectoryIssue::Empty);
    EXPECT_EQ(ValidateInstallDirectory(L"Capture Engine", windows), DirectoryIssue::NotAbsolute);
    EXPECT_EQ(ValidateInstallDirectory(L"\\Program Files\\x", windows), DirectoryIssue::NotAbsolute);
    EXPECT_EQ(ValidateInstallDirectory(L"\\\\server\\share\\CE", windows), DirectoryIssue::Unc);
    EXPECT_EQ(ValidateInstallDirectory(L"\\\\?\\C:\\CE", windows), DirectoryIssue::DevicePath);
    EXPECT_EQ(ValidateInstallDirectory(L"C:\\", windows), DirectoryIssue::DriveRoot);
    EXPECT_EQ(ValidateInstallDirectory(L"C:", windows), DirectoryIssue::DriveRoot);
    EXPECT_EQ(ValidateInstallDirectory(L"C:\\a\\..\\b", windows), DirectoryIssue::InvalidComponent);
    EXPECT_EQ(ValidateInstallDirectory(L"C:\\a\\b.", windows), DirectoryIssue::InvalidComponent);
    EXPECT_EQ(ValidateInstallDirectory(L"C:\\bad|name", windows), DirectoryIssue::InvalidCharacter);
    EXPECT_EQ(ValidateInstallDirectory(L"C:\\Windows\\System32\\CE", windows), DirectoryIssue::InsideWindows);
    EXPECT_EQ(ValidateInstallDirectory(L"C:\\" + std::wstring(250, L'a'), windows), DirectoryIssue::TooLong);
}

TEST(InstallerDirectoryTest, EveryIssueHasAnExplanation) {
    for (DirectoryIssue issue : {DirectoryIssue::Empty, DirectoryIssue::NotAbsolute, DirectoryIssue::Unc,
                                 DirectoryIssue::DevicePath, DirectoryIssue::DriveRoot, DirectoryIssue::TooLong,
                                 DirectoryIssue::InvalidCharacter, DirectoryIssue::InvalidComponent,
                                 DirectoryIssue::InsideWindows})
        EXPECT_NE(std::string(DirectoryIssueText(issue)), "");
    EXPECT_EQ(std::string(DirectoryIssueText(DirectoryIssue::Ok)), "");
}

// ---------------------------------------------------------------------------
// config.ini and user data
// ---------------------------------------------------------------------------

TEST(InstallerConfigTest, AMissingConfigIsCreated) {
    const ConfigPlan plan = PlanConfig(false);
    EXPECT_TRUE(plan.writeConfig);
    EXPECT_FALSE(plan.writeTemplate);
}

TEST(InstallerConfigTest, AnExistingConfigIsNeverOverwrittenAndGetsANewFileBesideIt) {
    const ConfigPlan plan = PlanConfig(true);
    EXPECT_FALSE(plan.writeConfig);
    EXPECT_TRUE(plan.writeTemplate);
    EXPECT_STREQ(kConfigName, "config.ini");
    EXPECT_STREQ(kConfigTemplateName, "config.ini.new");
}

TEST(InstallerUserDataTest, ConfigAndRuntimeFoldersAreUserData) {
    EXPECT_TRUE(IsUserDataPath("config.ini"));
    EXPECT_TRUE(IsUserDataPath("CONFIG.INI"));
    EXPECT_TRUE(IsUserDataPath("logs"));
    EXPECT_TRUE(IsUserDataPath("logs/20261001_063906/hook_debug.log"));
    EXPECT_TRUE(IsUserDataPath("Captures/clip.mkv"));
    EXPECT_TRUE(IsUserDataPath("screenshots/a.png"));
    EXPECT_TRUE(IsUserDataPath("benchmarks/report.html"));
    EXPECT_FALSE(IsUserDataPath("captureengine.exe"));
    EXPECT_FALSE(IsUserDataPath("config.ini.new"));
    EXPECT_FALSE(IsUserDataPath("logsmith.dll"));
    EXPECT_FALSE(IsUserDataPath("ffmpeg/avcodec-63.dll"));
}

TEST(InstallerUserDataTest, TemporaryNamesAreRecognised) {
    EXPECT_TRUE(IsInstallerTemporaryName("captureengine.exe.cenew"));
    EXPECT_TRUE(IsInstallerTemporaryName("mediaengine.dll.CEBAK"));
    EXPECT_TRUE(IsInstallerTemporaryName("x.dll.123456.cebak"));
    EXPECT_FALSE(IsInstallerTemporaryName("captureengine.exe"));
    EXPECT_FALSE(IsInstallerTemporaryName(".cebak"));
    EXPECT_TRUE(IsInstallerOwnedLeaf("config.ini.new"));
    EXPECT_FALSE(IsInstallerOwnedLeaf("config.ini"));
}

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------

TEST(InstallerManifestTest, RoundTrips) {
    const std::vector<std::string> files = {"captureengine.exe", "ffmpeg/avcodec-63.dll"};
    const Manifest parsed = ParseManifest(SerializeManifest("0.1.6917", files));
    ASSERT_TRUE(parsed.valid);
    EXPECT_EQ(parsed.version, "0.1.6917");
    EXPECT_EQ(parsed.files, files);
}

TEST(InstallerManifestTest, ToleratesCrLfAndBlankLines) {
    const Manifest parsed = ParseManifest("CEINSTALL1\r\nversion=1\r\n\r\nfile=a.exe\r\n");
    ASSERT_TRUE(parsed.valid);
    EXPECT_EQ(parsed.files, std::vector<std::string>{"a.exe"});
}

TEST(InstallerManifestTest, RejectsForeignOrEmptyFiles) {
    EXPECT_FALSE(ParseManifest("").valid);
    EXPECT_FALSE(ParseManifest("something else\nfile=a.exe\n").valid);
}

TEST(InstallerManifestTest, AHostileManifestCannotNameAFileOutsideTheFolder) {
    const Manifest parsed = ParseManifest(
        "CEINSTALL1\nfile=../../Windows/System32/x.dll\nfile=C:/Windows/x.dll\nfile=/etc/passwd\nfile=ok.dll\n");
    ASSERT_TRUE(parsed.valid);
    EXPECT_EQ(parsed.files, std::vector<std::string>{"ok.dll"});
}

TEST(InstallerManifestTest, StaleFilesAreWhatTheNewPayloadNoLongerShips) {
    Manifest previous;
    previous.valid = true;
    previous.files = {"captureengine.exe", "old_helper.dll", "ffmpeg/old.dll", "ffmpeg/keep.dll"};
    const auto stale = StaleFiles(previous, {"captureengine.exe", "FFMPEG/KEEP.DLL"});
    EXPECT_EQ(stale, (std::vector<std::string>{"old_helper.dll", "ffmpeg/old.dll"}));
}

TEST(InstallerManifestTest, StaleFilesNeverIncludeUserData) {
    Manifest previous;
    previous.valid = true;
    previous.files = {"config.ini", "logs/x.log", "captures/a.mkv", "gone.dll"};
    EXPECT_EQ(StaleFiles(previous, {}), std::vector<std::string>{"gone.dll"});
}

TEST(InstallerManifestTest, ParentDirectoriesAreOrderedDeepestFirst) {
    const auto directories = ParentDirectories({"a.exe", "ffmpeg/x.dll", "plugins/Lib/y.dll", "plugins/Lib/z.dll"});
    EXPECT_EQ(directories, (std::vector<std::string>{"plugins/Lib", "plugins", "ffmpeg"}));
}

TEST(InstallerShortcutTest, OnlyALinkToTheInstalledExecutableIsOurs) {
    const std::wstring directory = L"C:\\Program Files\\Capture Engine";
    EXPECT_TRUE(ShortcutTargetsInstallation(L"C:\\Program Files\\Capture Engine\\captureengine.exe", directory,
                                            L"captureengine.exe"));
    EXPECT_TRUE(ShortcutTargetsInstallation(L"c:\\program files\\capture engine\\CaptureEngine.exe", directory,
                                            L"captureengine.exe"));
    EXPECT_FALSE(ShortcutTargetsInstallation(L"D:\\dev\\installed\\captureengine\\captureengine.exe", directory,
                                             L"captureengine.exe"));
    EXPECT_FALSE(ShortcutTargetsInstallation(L"C:\\Program Files\\Capture Engine\\other.exe", directory,
                                             L"captureengine.exe"));
    EXPECT_FALSE(ShortcutTargetsInstallation(L"", directory, L"captureengine.exe"));
}

// ---------------------------------------------------------------------------
// Elevated role contract (common/installer_setup_policy.h)
// ---------------------------------------------------------------------------

std::vector<std::wstring> RoleArguments(const std::wstring& sid, const std::wstring& admin, const std::wstring& prefs,
                                        const std::wstring& service, const std::wstring& autostart) {
    return {L"--owner-sid=" + sid, L"--owner-admin=" + admin, L"--prefs=" + prefs, L"--service=" + service,
            L"--autostart=" + autostart};
}

TEST(InstallerRolePolicyTest, ParsesAValidRequest) {
    ce::startup::InstallerSetupRequest request;
    ASSERT_TRUE(ce::startup::ParseInstallerSetupArguments(
        RoleArguments(L"S-1-5-21-1-2-3-1001", L"1", L"5", L"install", L"apply"), &request));
    EXPECT_EQ(request.ownerSid, L"S-1-5-21-1-2-3-1001");
    EXPECT_TRUE(request.ownerAdministrator);
    EXPECT_TRUE(request.preferences.service);
    EXPECT_FALSE(request.preferences.elevated);
    EXPECT_TRUE(request.preferences.autostart);
    EXPECT_EQ(request.service, ce::startup::ServiceStep::Install);
    EXPECT_TRUE(request.applyAutostart);
}

TEST(InstallerRolePolicyTest, RejectsMalformedOrIncompleteRequests) {
    ce::startup::InstallerSetupRequest request;
    const auto valid = RoleArguments(L"S-1-5-21-1-2-3-1001", L"0", L"0", L"keep", L"apply");
    ASSERT_TRUE(ce::startup::ParseInstallerSetupArguments(valid, &request));

    for (size_t missing = 0; missing < valid.size(); ++missing) {
        auto fewer = valid;
        fewer.erase(fewer.begin() + static_cast<long>(missing));
        EXPECT_FALSE(ce::startup::ParseInstallerSetupArguments(fewer, &request)) << missing;
    }
    auto repeated = valid;
    repeated.push_back(valid[2]);
    EXPECT_FALSE(ce::startup::ParseInstallerSetupArguments(repeated, &request));
    auto unknown = valid;
    unknown.push_back(L"--extra=1");
    EXPECT_FALSE(ce::startup::ParseInstallerSetupArguments(unknown, &request));

    for (const auto& bad : {RoleArguments(L"not-a-sid", L"0", L"0", L"keep", L"apply"),
                            RoleArguments(L"S-1-5-21-", L"0", L"0", L"keep", L"apply"),
                            RoleArguments(L"S-1-5-21-1", L"2", L"0", L"keep", L"apply"),
                            RoleArguments(L"S-1-5-21-1", L"0", L"8", L"keep", L"apply"),
                            RoleArguments(L"S-1-5-21-1", L"0", L"-1", L"keep", L"apply"),
                            RoleArguments(L"S-1-5-21-1", L"0", L"0", L"restart", L"apply"),
                            RoleArguments(L"S-1-5-21-1", L"0", L"0", L"keep", L"maybe")})
        EXPECT_FALSE(ce::startup::ParseInstallerSetupArguments(bad, &request));
}

TEST(InstallerRolePolicyTest, ServiceStepMustAgreeWithThePreference) {
    ce::startup::InstallerSetupRequest request;
    // Installing the service while saving "service off" (or the reverse) would leave
    // the tray showing a state that contradicts the machine.
    EXPECT_FALSE(ce::startup::ParseInstallerSetupArguments(
        RoleArguments(L"S-1-5-21-1", L"0", L"0", L"install", L"apply"), &request));
    EXPECT_FALSE(ce::startup::ParseInstallerSetupArguments(
        RoleArguments(L"S-1-5-21-1", L"0", L"1", L"remove", L"apply"), &request));
    EXPECT_TRUE(ce::startup::ParseInstallerSetupArguments(
        RoleArguments(L"S-1-5-21-1", L"0", L"0", L"remove", L"apply"), &request));
}

TEST(InstallerRolePolicyTest, SidShapeCheck) {
    EXPECT_TRUE(ce::startup::LooksLikeSidString(L"S-1-5-18"));
    EXPECT_TRUE(ce::startup::LooksLikeSidString(L"S-1-5-21-3623811015-3361044348-30300820-1013"));
    EXPECT_FALSE(ce::startup::LooksLikeSidString(L"S-1-"));
    EXPECT_FALSE(ce::startup::LooksLikeSidString(L"S-1-5--18"));
    EXPECT_FALSE(ce::startup::LooksLikeSidString(L"S-1-5-18-"));
    EXPECT_FALSE(ce::startup::LooksLikeSidString(L"s-1-5-18"));
    EXPECT_FALSE(ce::startup::LooksLikeSidString(L"S-1-5-1 8"));
    EXPECT_FALSE(ce::startup::LooksLikeSidString(std::wstring(L"S-1-5-") + std::wstring(200, L'1')));
}

}  // namespace
