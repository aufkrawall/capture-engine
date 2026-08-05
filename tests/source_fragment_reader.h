#pragma once

// Source-policy tests read the logical translation unit, not only the small
// forwarding file that includes its ordered .inl fragments (legacy split) or
// its generated internal header plus sibling units (semantic .cpp split).

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace ce::test_source {

inline std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.good())
        return {};
    std::string contents{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    for (size_t position = 0; (position = contents.find("\r\n", position)) != std::string::npos;) {
        contents.erase(position, 1);
        ++position;
    }
    return contents;
}

inline std::string ReadLogicalSource(const std::filesystem::path& path) {
    const std::string wrapper = ReadFile(path);
    if (wrapper.empty())
        return {};

    if (path.extension() == ".py") {
        const std::string marker = "_SOURCE_PARTS = (";
        const size_t tupleBegin = wrapper.find(marker);
        if (tupleBegin != std::string::npos) {
            const size_t bodyMarker = wrapper.find("_SOURCE_BODY_PARTS", tupleBegin + marker.size());
            const std::string partTuple = wrapper.substr(
                tupleBegin + marker.size(), bodyMarker == std::string::npos ? std::string::npos : bodyMarker - tupleBegin - marker.size());
            std::vector<std::string> bodyNames;
            if (bodyMarker != std::string::npos) {
                const size_t bodyBegin = wrapper.find('(', bodyMarker);
                const size_t bodyEnd = wrapper.find(')', bodyBegin);
                if (bodyBegin != std::string::npos && bodyEnd != std::string::npos) {
                    const std::string bodyTuple = wrapper.substr(bodyBegin + 1, bodyEnd - bodyBegin - 1);
                    size_t bodyCursor = 0;
                    while (bodyCursor < bodyTuple.size()) {
                        const size_t nameBegin = bodyTuple.find('\'', bodyCursor);
                        if (nameBegin == std::string::npos)
                            break;
                        const size_t nameEnd = bodyTuple.find('\'', nameBegin + 1);
                        if (nameEnd == std::string::npos)
                            break;
                        bodyNames.push_back(bodyTuple.substr(nameBegin + 1, nameEnd - nameBegin - 1));
                        bodyCursor = nameEnd + 1;
                    }
                }
            }
            std::ostringstream logical;
            size_t cursor = 0;
            while (cursor < partTuple.size()) {
                const size_t nameBegin = partTuple.find('\'', cursor);
                if (nameBegin == std::string::npos)
                    break;
                const size_t nameEnd = partTuple.find('\'', nameBegin + 1);
                if (nameEnd == std::string::npos)
                    break;
                const std::string name = partTuple.substr(nameBegin + 1, nameEnd - nameBegin - 1);
                std::string part = ReadFile(path.parent_path() / name);
                for (const std::string& bodyName : bodyNames) {
                    if (bodyName == name) {
                        const size_t sentinelEnd = part.find('\n');
                        if (sentinelEnd != std::string::npos)
                            part.erase(0, sentinelEnd + 1);
                        break;
                    }
                }
                logical << part;
                cursor = nameEnd + 1;
            }
            return logical.str();
        }
    }

    const std::string stem = path.stem().string();
    const std::string includePrefix = "#include \"" + stem + "_part_";
    std::istringstream lines(wrapper);
    std::ostringstream logical;
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind(includePrefix, 0) == 0 && (line.ends_with(".inl\"") || line.ends_with(".py\""))) {
            const size_t nameBegin = std::string("#include \"").size();
            const size_t nameEnd = line.find('"', nameBegin);
            if (nameEnd != std::string::npos) {
                logical << ReadFile(path.parent_path() / line.substr(nameBegin, nameEnd - nameBegin));
                continue;
            }
        }
        logical << line << '\n';
    }
    if (path.extension() == ".cpp") {
        const std::string internalName = stem + "_internal.h";
        const auto internalPath = path.parent_path() / internalName;
        if (std::filesystem::exists(internalPath)) {
            std::ostringstream combined;
            combined << ReadFile(internalPath) << '\n' << logical.str();
            std::vector<std::filesystem::path> siblings;
            for (const auto& entry : std::filesystem::directory_iterator(path.parent_path())) {
                const std::string name = entry.path().filename().string();
                if (name.size() > stem.size() + 1 && name.rfind(stem + "_", 0) == 0 &&
                    name.ends_with(".cpp") && name != internalName) {
                    siblings.push_back(entry.path());
                }
            }
            std::sort(siblings.begin(), siblings.end());
            for (const auto& sibling : siblings) {
                combined << ReadFile(sibling) << '\n';
            }
            return combined.str();
        }
    }
    return logical.str();
}

}  // namespace ce::test_source
