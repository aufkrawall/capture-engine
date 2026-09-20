// An `inline` variable at namespace scope is meant to be ONE object shared by
// every translation unit that includes the header. It silently stops being one
// when its type comes from an unnamed namespace in that same header: the type
// has internal linkage, the variable inherits it, and each translation unit
// gets its own copy. No compiler diagnostic fires, not even under -Wall
// -Wextra, so the only symptom is a writer and a reader disagreeing at runtime.
//
// That is exactly how `[StartupPerf]` came to report VulkanRegistration=0.000,
// TrayCreate=0.000 and TotalToReady=36055.158 ms for a startup that the log
// timestamps show took 142 ms: main_entry.cpp wrote its copy of
// main_g_ControllerStartupTiming, main_recording.cpp read a zero-initialized
// one, and TotalToReady degenerated into `Log_GetQpcUs() - 0`, an absolute
// QPC-since-boot reading.

#include <gtest/gtest.h>

#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "source_fragment_reader.h"

namespace {

// Directories build_tests.py already treats as first-party C++.
const char* const kFirstPartySourceDirectories[] = {"common",     "hook",    "captureengine",
                                                    "mediaengine", "testapp", "tests"};

bool IsHeaderExtension(const std::filesystem::path& path) {
    const std::string extension = path.extension().string();
    return extension == ".h" || extension == ".hpp" || extension == ".inl";
}

struct AnonymousNamespaceSpan {
    size_t begin = 0;
    size_t end = 0;
};

bool IsIdentifierCharacter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// Reads the identifier starting at `position`, or an empty string when the
// text there does not begin one.
std::string ReadIdentifier(const std::string& text, size_t position) {
    size_t end = position;
    while (end < text.size() && IsIdentifierCharacter(text[end]))
        ++end;
    return text.substr(position, end - position);
}

size_t SkipSpaces(const std::string& text, size_t position) {
    while (position < text.size() && (text[position] == ' ' || text[position] == '\t'))
        ++position;
    return position;
}

// Every `namespace {` block in `text`, with the type names it declares.
// Nesting is tracked by brace depth, which is enough because the scan only has
// to find declarations, not parse them.
std::vector<AnonymousNamespaceSpan> FindAnonymousNamespaces(const std::string& text,
                                                           std::set<std::string>& internalLinkageTypes) {
    std::vector<AnonymousNamespaceSpan> spans;
    const std::string keyword = "namespace";
    for (size_t search = text.find(keyword); search != std::string::npos; search = text.find(keyword, search + 1)) {
        if (search > 0 && IsIdentifierCharacter(text[search - 1]))
            continue;
        size_t cursor = SkipSpaces(text, search + keyword.size());
        if (cursor >= text.size() || text[cursor] != '{')
            continue;  // A named namespace, or `namespace x = y;`.

        const size_t bodyBegin = cursor + 1;
        size_t depth = 1;
        while (cursor + 1 < text.size() && depth > 0) {
            ++cursor;
            if (text[cursor] == '{')
                ++depth;
            else if (text[cursor] == '}')
                --depth;
        }
        if (depth != 0)
            continue;  // Unbalanced; nothing reliable to report.

        spans.push_back({search, cursor + 1});
        const std::string body = text.substr(bodyBegin, cursor - bodyBegin);
        for (const char* introducer : {"struct", "class"}) {
            const std::string token(introducer);
            for (size_t at = body.find(token); at != std::string::npos; at = body.find(token, at + 1)) {
                if (at > 0 && IsIdentifierCharacter(body[at - 1]))
                    continue;
                const size_t nameAt = SkipSpaces(body, at + token.size());
                const std::string name = ReadIdentifier(body, nameAt);
                if (!name.empty())
                    internalLinkageTypes.insert(name);
            }
        }
        for (size_t at = body.find("using"); at != std::string::npos; at = body.find("using", at + 1)) {
            if (at > 0 && IsIdentifierCharacter(body[at - 1]))
                continue;
            const size_t nameAt = SkipSpaces(body, at + 5);
            const std::string name = ReadIdentifier(body, nameAt);
            const size_t assignAt = SkipSpaces(body, nameAt + name.size());
            if (!name.empty() && assignAt < body.size() && body[assignAt] == '=')
                internalLinkageTypes.insert(name);
        }
    }
    return spans;
}

bool IsInsideAnySpan(const std::vector<AnonymousNamespaceSpan>& spans, size_t position) {
    for (const AnonymousNamespaceSpan& span : spans) {
        if (position >= span.begin && position < span.end)
            return true;
    }
    return false;
}

struct Violation {
    std::string file;
    std::string type;
    std::string variable;
};

// `inline <Type> <name>` declarations outside every unnamed namespace whose
// `<Type>` was declared inside one.
std::vector<Violation> FindSplitInlineVariables(const std::filesystem::path& header) {
    const std::string text = ce::test_source::ReadFile(header);
    std::vector<Violation> violations;
    if (text.empty())
        return violations;

    std::set<std::string> internalLinkageTypes;
    const std::vector<AnonymousNamespaceSpan> spans = FindAnonymousNamespaces(text, internalLinkageTypes);
    if (internalLinkageTypes.empty())
        return violations;

    const std::string keyword = "inline";
    for (size_t at = text.find(keyword); at != std::string::npos; at = text.find(keyword, at + 1)) {
        if (at > 0 && IsIdentifierCharacter(text[at - 1]))
            continue;
        if (IsInsideAnySpan(spans, at))
            continue;  // Already internal-linkage by intent, not by accident.

        size_t cursor = SkipSpaces(text, at + keyword.size());
        std::string typeName = ReadIdentifier(text, cursor);
        if (typeName == "constexpr" || typeName == "const" || typeName == "static") {
            cursor = SkipSpaces(text, cursor + typeName.size());
            typeName = ReadIdentifier(text, cursor);
        }
        if (typeName.empty() || internalLinkageTypes.find(typeName) == internalLinkageTypes.end())
            continue;

        const size_t nameAt = SkipSpaces(text, cursor + typeName.size());
        const std::string variableName = ReadIdentifier(text, nameAt);
        if (variableName.empty())
            continue;
        const size_t terminatorAt = SkipSpaces(text, nameAt + variableName.size());
        if (terminatorAt >= text.size())
            continue;
        // A declarator follows a variable; anything else is a function or a
        // template and carries no per-translation-unit object.
        const char terminator = text[terminatorAt];
        if (terminator != ';' && terminator != '=' && terminator != '{')
            continue;

        violations.push_back({header.generic_string(), typeName, variableName});
    }
    return violations;
}

}  // namespace

// The anchor for the [StartupPerf] regression: this is the declaration that
// split, and the shape the sweep below generalizes.
TEST(HeaderInlineVariableLinkage, ControllerStartupTimingIsOneObjectForEveryTranslationUnit) {
    const std::filesystem::path header = std::filesystem::current_path() / "captureengine" / "main_internal.h";
    const std::string text = ce::test_source::ReadFile(header);
    ASSERT_FALSE(text.empty()) << "captureengine/main_internal.h could not be read";

    EXPECT_NE(text.find("inline ControllerStartupTimingState main_g_ControllerStartupTiming;"), std::string::npos)
        << "main_g_ControllerStartupTiming must stay an inline variable so one object serves main_entry.cpp and "
           "main_recording.cpp";

    const std::vector<Violation> violations = FindSplitInlineVariables(header);
    for (const Violation& violation : violations) {
        ADD_FAILURE() << violation.file << " declares `inline " << violation.type << " " << violation.variable
                      << "`, but " << violation.type
                      << " comes from an unnamed namespace in that header. Every translation unit then gets its own "
                         "copy, so a writer's measurements never reach a reader in another unit.";
    }
}

// The same mistake anywhere else is just as silent, so sweep every first-party
// header rather than waiting for the next subsystem to hit it.
TEST(HeaderInlineVariableLinkage, NoFirstPartyHeaderGivesAnInlineVariableInternalLinkage) {
    const std::filesystem::path root = std::filesystem::current_path();
    std::vector<Violation> violations;
    size_t headersScanned = 0;

    for (const char* directory : kFirstPartySourceDirectories) {
        const std::filesystem::path base = root / directory;
        if (!std::filesystem::exists(base))
            continue;
        for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(base)) {
            if (!entry.is_regular_file() || !IsHeaderExtension(entry.path()))
                continue;
            ++headersScanned;
            const std::vector<Violation> found = FindSplitInlineVariables(entry.path());
            violations.insert(violations.end(), found.begin(), found.end());
        }
    }

    EXPECT_GT(headersScanned, 100u) << "the header sweep found almost nothing to scan; check the working directory";
    for (const Violation& violation : violations) {
        ADD_FAILURE() << violation.file << ": `inline " << violation.type << " " << violation.variable
                      << "` has internal linkage because " << violation.type
                      << " is declared in an unnamed namespace in the same header. Move the type out of the unnamed "
                         "namespace, or make the variable's per-translation-unit copy explicit.";
    }
}
