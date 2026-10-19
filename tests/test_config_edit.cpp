#include "test_config_shared.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "common/config/config_ini_edit.h"
#include "common/config/config_ini_reader.h"
#include "tests/fuzz/settings_edit_checker.h"

// In-place edits of an IniDocument: only the edited line changes, everything else (comments,
// order, unknown lines, line endings, BOM) stays byte for byte. The scenarios pin exact bytes;
// the shared property checker (the one tests/fuzz/fuzz_settings_edit.cpp drives) is replayed over
// the committed seed corpus and a deterministic sweep, so the properties run in every unit gate.

namespace {

namespace text = ce::config_text;
using text::IniEditStatus;

std::string Set(const std::string& source, const char* section, const char* key, const char* value,
                IniEditStatus expected = IniEditStatus::kChanged) {
    text::IniDocument document = text::ParseUtf8Ini(source);
    EXPECT_EQ(text::SetIniValue(&document, section, key, value), expected);
    return text::SerializeIni(document);
}

std::string Remove(const std::string& source, const char* section, const char* key,
                   IniEditStatus expected = IniEditStatus::kChanged) {
    text::IniDocument document = text::ParseUtf8Ini(source);
    EXPECT_EQ(text::RemoveIniValue(&document, section, key), expected);
    return text::SerializeIni(document);
}

std::filesystem::path CorpusDir() {
    const std::filesystem::path relative = std::filesystem::path("tests") / "fuzz" / "corpus" / "settings_edit";
    std::filesystem::path base = std::filesystem::current_path();
    for (int depth = 0; depth < 4; ++depth) {
        const std::filesystem::path candidate = base / relative;
        if (std::filesystem::is_directory(candidate))
            return candidate;
        if (!base.has_parent_path() || base.parent_path() == base)
            break;
        base = base.parent_path();
    }
    return {};
}

}  // namespace

TEST(ConfigIniEditTest, SetExistingKeyRewritesOnlyThatLine) {
    const std::string source = "[Video]\r\n; the frame rate\r\nfps = 60 ; inline\r\ncodec=hevc\r\n[Output]\r\nfps=1\r\n";
    EXPECT_EQ(Set(source, "Video", "fps", "90"),
              "[Video]\r\n; the frame rate\r\nfps = 90\r\ncodec=hevc\r\n[Output]\r\nfps=1\r\n");
    // Names match like a lookup does: case and the blanks around them do not matter; the key keeps its spelling.
    EXPECT_EQ(Set(source, "  video ", " FPS ", "90"),
              "[Video]\r\n; the frame rate\r\nfps = 90\r\ncodec=hevc\r\n[Output]\r\nfps=1\r\n");
    // Clearing a value keeps "key=" (no stray blanks).
    EXPECT_EQ(Set("[A]\nk = old\n", "A", "k", ""), "[A]\nk =\n");
}

TEST(ConfigIniEditTest, SettingTheValueThatIsAlreadyThereChangesNothing) {
    const std::string source = "[A]\nk =  60  \n";
    EXPECT_EQ(Set(source, "A", "k", "60", IniEditStatus::kUnchanged), source);
    EXPECT_EQ(Set(source, "A", "k", " 60", IniEditStatus::kUnchanged), source);
}

TEST(ConfigIniEditTest, MissingKeyIsInsertedAfterTheLastKeyLineOfItsSection) {
    EXPECT_EQ(Set("[Video]\nfps=60\n\n; about output\n[Output]\nx=1\n", "Video", "new", "1"),
              "[Video]\nfps=60\nnew=1\n\n; about output\n[Output]\nx=1\n");
    // The terminator of the line before it is used (CRLF here), and an empty section gets the key after its header.
    EXPECT_EQ(Set("[A]\r\nx=1\r\n[B]\r\n[C]\r\ny=2\r\n", "B", "k", "v"), "[A]\r\nx=1\r\n[B]\r\nk=v\r\n[C]\r\ny=2\r\n");
    // A list block (lines without '=') counts as part of the section's content.
    EXPECT_EQ(Set("[I]\nwl=(\n a.exe\n)\n\n[J]\n", "I", "k", "v"), "[I]\nwl=(\n a.exe\n)\nk=v\n\n[J]\n");
}

TEST(ConfigIniEditTest, InsertAfterAnUnterminatedLastLineKeepsTheTextUnterminated) {
    EXPECT_EQ(Set("[A]\nx=1", "A", "y", "2"), "[A]\nx=1\ny=2");
    EXPECT_EQ(Set("[A]\r\nx=1", "A", "y", "2"), "[A]\r\nx=1\r\ny=2");
}

TEST(ConfigIniEditTest, MissingSectionIsAppendedAtTheEnd) {
    EXPECT_EQ(Set("[A]\r\nx=1\r\n", "B", "y", "2"), "[A]\r\nx=1\r\n\r\n[B]\r\ny=2\r\n");
    EXPECT_EQ(Set("[A]\r\nx=1\r\n\r\n", "B", "y", "2"), "[A]\r\nx=1\r\n\r\n[B]\r\ny=2\r\n");  // already ends in a blank line
    EXPECT_EQ(Set("[A]\nx=1", "B", "y", "2"), "[A]\nx=1\n\n[B]\ny=2");
    EXPECT_EQ(Set("", "A", "x", "1"), "[A]\r\nx=1\r\n");
    EXPECT_EQ(Set("\xEF\xBB\xBF", "A", "x", "1"), "\xEF\xBB\xBF[A]\r\nx=1\r\n");
}

TEST(ConfigIniEditTest, RemoveDeletesEveryMatchingLineOfTheFirstSectionOnly) {
    EXPECT_EQ(Remove("[A]\nk=1\n; keep\nk=2\nj=3\n[A]\nk=9\n[B]\nk=8\n", "a", "K"),
              "[A]\n; keep\nj=3\n[A]\nk=9\n[B]\nk=8\n");
    Remove("[A]\nk=1\n", "A", "nope", IniEditStatus::kNotFound);
    Remove("[A]\nk=1\n", "Nope", "k", IniEditStatus::kNotFound);
    // A final unterminated line can be removed; the line before keeps its terminator.
    EXPECT_EQ(Remove("[A]\nx=1\nk=2", "A", "k"), "[A]\nx=1\n");
}

TEST(ConfigIniEditTest, ByteOrderMarkAndMixedLineEndingsSurviveEdits) {
    const std::string source = "\xEF\xBB\xBF[A]\r\nx=1\ny=2\rz=3\n\r[B]\nw=4\n";
    const std::string edited = Set(source, "A", "y", "\xE6\x97\xA5\xE6\x9C\xAC");
    EXPECT_EQ(edited, "\xEF\xBB\xBF[A]\r\nx=1\ny=\xE6\x97\xA5\xE6\x9C\xAC\rz=3\n\r[B]\nw=4\n");
    const text::IniDocument reparsed = text::ParseUtf8Ini(edited);
    std::string value;
    ASSERT_TRUE(text::LookupIniValue(reparsed, "A", "y", CP_UTF8, &value));
    EXPECT_EQ(value, "\xE6\x97\xA5\xE6\x9C\xAC");
    EXPECT_TRUE(reparsed.hasBom);
}

TEST(ConfigIniEditTest, InvalidNamesAndValuesAreRejectedWithoutTouchingTheText) {
    const std::string source = "[A]\nk=1\n";
    EXPECT_EQ(Set(source, "A]", "k", "v", IniEditStatus::kInvalidSection), source);
    EXPECT_EQ(Set(source, "A\n", "k", "v", IniEditStatus::kInvalidSection), source);
    EXPECT_EQ(Set(source, "A", "", "v", IniEditStatus::kInvalidKey), source);
    EXPECT_EQ(Set(source, "A", "a=b", "v", IniEditStatus::kInvalidKey), source);
    EXPECT_EQ(Set(source, "A", ";c", "v", IniEditStatus::kInvalidKey), source);
    EXPECT_EQ(Set(source, "A", "[c", "v", IniEditStatus::kInvalidKey), source);
    EXPECT_EQ(Set(source, "A", "k", "a\nb", IniEditStatus::kInvalidValue), source);
    EXPECT_EQ(Set(source, "A", "k", "a\rb", IniEditStatus::kInvalidValue), source);
    EXPECT_EQ(Set(source, "A", "k", "\xFF", IniEditStatus::kInvalidValue), source);
    EXPECT_EQ(Remove(source, "A", "a=b", IniEditStatus::kInvalidKey), source);
}

TEST(ConfigIniEditTest, SectionWithEmptyNameAddressesTheLinesBeforeTheFirstHeader) {
    EXPECT_EQ(Set("orphan=1\n[A]\nx=1\n", "", "orphan", "2"), "orphan=2\n[A]\nx=1\n");
    EXPECT_EQ(Set("orphan=1\n[A]\nx=1\n", "", "added", "2"), "orphan=1\nadded=2\n[A]\nx=1\n");
    EXPECT_EQ(Set("[A]\nx=1\n", "", "added", "2"), "added=2\n[A]\nx=1\n");
    EXPECT_EQ(Remove("orphan=1\n[A]\nx=1\n", "", "orphan"), "[A]\nx=1\n");
}

TEST(ConfigIniEditTest, EditedDocumentIsWhatTheLoaderReads) {
    const std::string source =
        "; keep this comment\r\n[Video]\r\nfps=60\r\n; and this one\r\ncodec=hevc\r\n[Hotkeys]\r\nstart_stop=F9\r\n";
    text::IniDocument document = text::ParseUtf8Ini(source);
    ASSERT_EQ(text::SetIniValue(&document, "Video", "fps", "144"), IniEditStatus::kChanged);
    ASSERT_EQ(text::SetIniValue(&document, "Hotkeys", "screenshot", "F10"), IniEditStatus::kChanged);
    ASSERT_EQ(text::SetIniValue(&document, "Streaming", "enabled", "false"), IniEditStatus::kChanged);
    ASSERT_EQ(text::RemoveIniValue(&document, "Video", "codec"), IniEditStatus::kChanged);
    EXPECT_EQ(text::SerializeIni(document),
              "; keep this comment\r\n[Video]\r\nfps=144\r\n; and this one\r\n[Hotkeys]\r\nstart_stop=F9\r\n"
              "screenshot=F10\r\n\r\n[Streaming]\r\nenabled=false\r\n");

    AppConfig config;
    LoadConfigFromDocument(document, config, "");
    EXPECT_EQ(config.video.fps, 144);
    EXPECT_NE(config.hotkeyScreenshot.vkey, 0);
}

TEST(ConfigIniEditTest, EditingTheShippedTemplateChangesOnlyTheEditedLines) {
    const std::string source = ReadTextFile(DefaultTemplatePath());
    ASSERT_FALSE(source.empty());
    text::IniDocument document = text::ParseUtf8Ini(source);
    ASSERT_EQ(text::SerializeIni(document), source);
    const size_t lineCount = document.sourceLines.size();
    ASSERT_EQ(text::SetIniValue(&document, "Video", "fps", "75"), IniEditStatus::kChanged);
    ASSERT_EQ(text::SetIniValue(&document, "Logging", "log_level", "debug"), IniEditStatus::kChanged);

    size_t differing = 0;
    ASSERT_EQ(document.sourceLines.size(), lineCount);
    const text::IniDocument pristine = text::ParseUtf8Ini(source);
    for (size_t i = 0; i < lineCount; ++i) {
        if (document.sourceLines[i].text != pristine.sourceLines[i].text ||
            document.sourceLines[i].eol != pristine.sourceLines[i].eol)
            ++differing;
    }
    EXPECT_EQ(differing, 2u);
    AppConfig config;
    LoadConfigFromDocument(document, config, "");
    EXPECT_EQ(config.video.fps, 75);
}

TEST(ConfigIniEditTest, SeedCorpusReplaysThroughTheEditProperties) {
    const std::filesystem::path dir = CorpusDir();
    ASSERT_FALSE(dir.empty()) << "Missing settings-edit fuzz corpus (tests/fuzz/corpus/settings_edit) at or above "
                              << std::filesystem::current_path().string();
    size_t replayed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file())
            continue;
        std::ifstream file(entry.path(), std::ios::binary);
        ASSERT_TRUE(file) << entry.path().string();
        const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const std::string failure =
            ce::settings_edit_check::CheckEditSequence(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
        EXPECT_TRUE(failure.empty()) << entry.path().filename().string() << ": " << failure;
        ++replayed;
    }
    EXPECT_GT(replayed, 0u) << "settings-edit corpus contains no seeds";
}

TEST(ConfigIniEditTest, DeterministicSweepKeepsTheEditProperties) {
    // The same checker over generated documents (slices of the shipped template and of small
    // hand-made texts) and generated edit headers: no timing, no randomness beyond a fixed LCG.
    const std::string templateText = ReadTextFile(DefaultTemplatePath());
    const std::string small[] = {"", "[A]\nx=1\n", "\xEF\xBB\xBF[A]\r\nx=1\r\ny=2\r\n[B]\r\n", "[A]\rx=1\r[A]\ry=2",
                                 "orphan=1\n[Video]\nfps=1\nfps=2\n\n[video]\nfps=3", templateText.substr(0, 3000)};
    uint32_t state = 0xC0FFEEu;
    auto next = [&] {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    for (int round = 0; round < 1500; ++round) {
        std::string input;
        const size_t ops = 1 + next() % 4;
        input.push_back(static_cast<char>(ops - 1));
        for (size_t i = 0; i < ops * 4; ++i)
            input.push_back(static_cast<char>(next()));
        const std::string& document = small[next() % (sizeof(small) / sizeof(small[0]))];
        input += document;
        const std::string failure = ce::settings_edit_check::CheckEditSequence(
            reinterpret_cast<const uint8_t*>(input.data()), input.size());
        ASSERT_TRUE(failure.empty()) << "round " << round << ": " << failure;
    }
}
