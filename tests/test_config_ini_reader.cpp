#include <gtest/gtest.h>

#include <windows.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../common/config_ini_reader.h"
#include "../common/config_text_encoding.h"

namespace text = ce::config_text;

namespace {

// ASCII-only, so the real profile API reads it exactly on any code page. Every
// line shape the grammar note in config_ini_reader.h describes appears here.
const char kGrammarIni[] =
    "orphan=o\r\n"
    "  [ Spaced ]  trailing junk\r\n"
    "spkey = spval  \r\n"
    "[Quotes]\r\n"
    "dq=\"quoted value\"\r\n"
    "sq='single'\r\n"
    "half=\"open\r\n"
    "mid=a\"b\"c\r\n"
    "dqsp= \"x\" \r\n"
    "inner=  \"  spaced  \"  \r\n"
    "one=\"\r\n"
    "mixed='x\"\r\n"
    "pair=\"\"\r\n"
    "empty=\r\n"
    "[Dup]\r\n"
    "a=first\r\n"
    "a=second\r\n"
    "[dup]\r\n"
    "a=third\r\n"
    "b=fromsecond\r\n"
    "[Misc]\r\n"
    ";comment=c\r\n"
    "  ;indented=ic\r\n"
    "#hash=h\r\n"
    "noequals\r\n"
    " key with space = v1\r\n"
    "eq=a=b\r\n"
    "inline=v ; comment\r\n"
    "\tTAB\t=\ttabval\t\r\n"
    "lfonly=1\n"
    "lfnext=2\n"
    "cr=v\rstill\r\n"
    "[Broken\r\n"
    "k=broken\r\n"
    "[B]x=y\r\n"
    "k=b\r\n"
    "[a]b]\r\n"
    "k=ab\r\n"
    "[Case]\r\n"
    "MiXeD=mixed\r\n"
    "mixed=second\r\n"
    "[Trail]\r\n"
    "trailing=val";

std::filesystem::path WriteTempIni(const char* tag, const std::string& bytes) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                       ("ce_ini_" + std::string(tag) + "_" + std::to_string(GetCurrentProcessId()) +
                                        ".ini");
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << bytes;
    return path;
}

std::string ProfileApiValue(const std::filesystem::path& path, const char* section, const char* key) {
    // kernel32 trims a requested name by writing into the caller's string, so
    // a literal with a trailing blank would fault in read-only data.
    std::string mutableSection = section;
    std::string mutableKey = key;
    char buffer[4096] = {};
    GetPrivateProfileStringA(mutableSection.c_str(), mutableKey.c_str(), "<missing>", buffer, sizeof(buffer),
                             path.string().c_str());
    return buffer;
}

std::string ReaderValue(const text::IniDocument& document, const char* section, const char* key) {
    std::string value;
    return text::LookupIniValue(document, section, key, 1252, &value) ? value : std::string("<missing>");
}

// What kernel32's A profile functions do to a file without a UTF-16 BOM: decode
// it with the code page, and encode the answer back with it.
std::string ProfileApiRoundTrip(const std::string& bytes, unsigned codePage) {
    return text::WideToCodePage(text::CodePageToWide(bytes, codePage), codePage);
}

}  // namespace

// The byte-level reader must answer exactly what GetPrivateProfileString
// answers; the real API is the oracle here, on content it reads losslessly.
TEST(ConfigIniReaderTest, AnswersExactlyLikeTheProfileApi) {
    const std::filesystem::path path = WriteTempIni("grammar", kGrammarIni);
    const text::IniDocument document = text::ParseUtf8Ini(kGrammarIni);

    const std::pair<const char*, const char*> queries[] = {
        {"Spaced", "spkey"},   {" Spaced ", "spkey"}, {"Quotes", "dq"},     {"Quotes", "sq"},
        {"Quotes", "half"},    {"Quotes", "mid"},     {"Quotes", "dqsp"},   {"Quotes", "inner"},
        {"Quotes", "one"},     {"Quotes", "mixed"},   {"Quotes", "pair"},   {"Quotes", "empty"},
        {"Dup", "a"},          {"Dup", "b"},          {"dup", "a"},         {"Misc", ";comment"},
        {"Misc", ";indented"}, {"Misc", "#hash"},     {"Misc", "noequals"}, {"Misc", "key with space"},
        {"Misc", "eq"},        {"Misc", "inline"},    {"Misc", "TAB"},      {"Misc", "lfonly"},
        {"Misc", "lfnext"},    {"Misc", "cr"},        {"Misc", "still"},    {"Broken", "k"},
        {"B", "k"},            {"B", "x"},            {"a]b", "k"},         {"a", "k"},
        {"case", "mixed"},     {"Trail", "trailing"}, {"", "orphan"},       {"Nowhere", "k"},
        {"Misc", " eq "},
    };
    for (const auto& [section, key] : queries) {
        EXPECT_EQ(ReaderValue(document, section, key), ProfileApiValue(path, section, key))
            << "[" << section << "] " << key;
    }

    std::vector<char> names(4096, '\0');
    GetPrivateProfileSectionNamesA(names.data(), static_cast<DWORD>(names.size()), path.string().c_str());
    std::vector<std::string> apiNames;
    for (const char* name = names.data(); *name; name += strlen(name) + 1) {
        apiNames.emplace_back(name);
    }
    EXPECT_EQ(text::IniSectionNames(document, 1252), apiNames);

    for (const char* section : {"Misc", "Quotes", "dup", "Broken", "Nowhere"}) {
        std::vector<char> buffer(4096, '\0');
        const DWORD chars = GetPrivateProfileSectionA(section, buffer.data(), static_cast<DWORD>(buffer.size()),
                                                      path.string().c_str());
        std::vector<std::string> apiLines;
        for (const char* line = buffer.data(); *line; line += strlen(line) + 1) {
            apiLines.emplace_back(line);
        }
        std::vector<std::string> readerLines;
        const bool found = text::IniSectionLines(document, section, 1252, &readerLines);
        EXPECT_EQ(found, chars != 0) << section;
        EXPECT_EQ(readerLines, apiLines) << section;
    }
    std::filesystem::remove(path);
}

// The one deliberate difference: the profile API reads a UTF-8 BOM as part of
// the first line, so the first section of a BOM-carrying config was lost.
TEST(ConfigIniReaderTest, Utf8BomNoLongerHidesTheFirstSection) {
    const std::string bytes = "\xEF\xBB\xBF[Capture]\r\nfps=60\r\n[Output]\r\ncodec=hevc\r\n";
    const std::filesystem::path path = WriteTempIni("bom", bytes);
    EXPECT_EQ(ProfileApiValue(path, "Capture", "fps"), "<missing>") << "the API behaviour this reader corrects";
    EXPECT_EQ(text::ReadIniValue(path.string(), "Capture", "fps", "30"), "60");
    EXPECT_EQ(text::ReadIniValue(path.string(), "Output", "codec", ""), "hevc");
    const std::vector<std::string> names = text::ReadIniSectionNames(path.string());
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "Capture");
    std::filesystem::remove(path);
}

// The reason the reader exists: on every DBCS code page, the ACP round trip the
// profile API performs does not return a UTF-8 file's bytes.
TEST(ConfigIniReaderTest, ProfileApiRoundTripIsLossyOnDoubleByteCodePages) {
    const std::string japanese = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88";
    const std::string katakana = "\xE3\x82\xB2\xE3\x83\xBC\xE3\x83\xA0";
    const std::string korean = "\xEC\x8A\xA4\xED\x81\xAC\xEB\xA6\xB0\xEC\x83\xB7";
    EXPECT_NE(ProfileApiRoundTrip(japanese, 932), japanese);
    EXPECT_NE(ProfileApiRoundTrip(katakana, 936), katakana);
    EXPECT_NE(ProfileApiRoundTrip(korean, 949), korean);
    EXPECT_NE(ProfileApiRoundTrip(japanese, 950), japanese);
    // Single-byte code pages are the case that did work.
    EXPECT_EQ(ProfileApiRoundTrip("J\xC3\xBCrgen", 1252), "J\xC3\xBCrgen");
}

// Values, section names and keys resolve in a forced DBCS code page, answering
// in that code page's own encoding.
TEST(ConfigIniReaderTest, Utf8ValuesResolveUnderAForcedDoubleByteCodePage) {
    const std::wstring folder = L"D:\\\u30D3\u30C7\u30AA\\\u65E5\u672C\u8A9E\u30C6\u30B9\u30C8";
    const std::wstring process = L"\u30B2\u30FC\u30E0.exe";
    const std::string utf8Folder = text::WideToCodePage(folder, CP_UTF8);
    const std::string utf8Process = text::WideToCodePage(process, CP_UTF8);
    const std::string bytes = "[Output]\r\noutput_dir=" + utf8Folder + "\r\n[" + utf8Process +
                              "]\r\nVideo.fps=144\r\n";
    const std::filesystem::path path = WriteTempIni("dbcs", bytes);

    bool lossy = true;
    const std::string expectedFolder = text::WideToCodePage(folder, 932, &lossy);
    ASSERT_FALSE(lossy) << "cp932 represents every character of the sample";
    EXPECT_EQ(text::ReadIniValueForCodePage(path.string(), "Output", "output_dir", "", 932), expectedFolder);

    // A profile section named after a Japanese executable, addressed by its
    // cp932 name the way ConfigReader receives it.
    const std::string processName932 = text::WideToCodePage(process, 932);
    EXPECT_EQ(text::ReadIniValueForCodePage(path.string(), processName932.c_str(), "Video.fps", "", 932), "144");

    const text::IniDocument document = text::ParseUtf8Ini(bytes);
    const std::vector<std::string> names = text::IniSectionNames(document, 932);
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[1], processName932) << "enumerated names must round-trip into lookups";

    // Korean and Simplified Chinese take the same path.
    const std::wstring korean = L"\uC2A4\uD06C\uB9B0\uC0F7";
    const text::IniDocument koreanDocument =
        text::ParseUtf8Ini("[Output]\r\nfolder=" + text::WideToCodePage(korean, CP_UTF8) + "\r\n");
    std::string value;
    ASSERT_TRUE(text::LookupIniValue(koreanDocument, "Output", "folder", 949, &value));
    EXPECT_EQ(value, text::WideToCodePage(korean, 949));
    const std::wstring chinese = L"\u5F55\u50CF";
    const text::IniDocument chineseDocument =
        text::ParseUtf8Ini("[Output]\r\nfolder=" + text::WideToCodePage(chinese, CP_UTF8) + "\r\n");
    ASSERT_TRUE(text::LookupIniValue(chineseDocument, "Output", "folder", 936, &value));
    EXPECT_EQ(value, text::WideToCodePage(chinese, 936));
    std::filesystem::remove(path);
}

TEST(ConfigIniReaderTest, UnrepresentableCharactersAreReportedAndNamesFoldCase) {
    const text::IniDocument document =
        text::ParseUtf8Ini("[\xC3\x9C" "BER]\r\nname=\xE3\x82\xB2\r\npath=J\xC3\xBCrgen\r\n");
    std::string value;
    bool lossy = false;
    // "über" (cp1252) finds "[ÜBER]": the comparison folds non-ASCII case too.
    ASSERT_TRUE(text::LookupIniValue(document, "\xFC" "ber", "name", 1252, &value, &lossy));
    EXPECT_EQ(value, "?");
    EXPECT_TRUE(lossy);
    ASSERT_TRUE(text::LookupIniValue(document, "\xFC" "ber", "PATH", 1252, &value, &lossy));
    EXPECT_EQ(value, "J\xFCrgen");
    EXPECT_FALSE(lossy);
    // CP_UTF8 (the Windows "beta: UTF-8" setting) returns the file's bytes.
    ASSERT_TRUE(text::LookupIniValue(document, "\xC3\xBC" "ber", "name", CP_UTF8, &value, &lossy));
    EXPECT_EQ(value, "\xE3\x82\xB2");
    EXPECT_FALSE(lossy);
}

// A missing key answers the default the way the profile API does (trailing
// blanks stripped), and an ANSI config keeps going through the API itself.
TEST(ConfigIniReaderTest, DefaultsAndAnsiFilesKeepProfileApiBehaviour) {
    const std::filesystem::path utf8 = WriteTempIni("default", "[Output]\r\nname=J\xC3\xBCrgen\r\n");
    EXPECT_EQ(text::ReadIniValueForCodePage(utf8.string(), "Output", "missing", "  def  ", 932), "  def");
    EXPECT_EQ(text::ReadIniValueForCodePage(utf8.string(), "Nowhere", "name", "", 932), "");
    std::filesystem::remove(utf8);

    const std::filesystem::path ansi = WriteTempIni("ansi", "[Output]\r\nname=J\xFCrgen\r\n");
    EXPECT_EQ(text::ReadIniValue(ansi.string(), "Output", "name", ""), ProfileApiValue(ansi, "Output", "name"));
    std::vector<std::string> lines;
    ASSERT_TRUE(text::ReadIniSectionLines(ansi.string(), "Output", &lines));
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0], "name=J\xFCrgen");
    std::filesystem::remove(ansi);
}
