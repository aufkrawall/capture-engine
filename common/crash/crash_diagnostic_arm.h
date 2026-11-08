#pragma once

#include <windows.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ce::crash_dump_policy {

inline constexpr const char* kDiagnosticArmMarkerFileName = "diagnostic_arm.txt";
inline constexpr const char* kDiagnosticArmAltMarkerFileName = "ce_diagnostic_arm.txt";
inline constexpr const char* kDiagnosticArmEnvVar = "CE_DIAGNOSTIC_ARM";

inline bool TrimWhitespaceAscii(const char* input, char* output, size_t outputCapacity) {
    if (!input || !output || outputCapacity == 0) {
        return false;
    }
    while (*input != '\0' && (static_cast<unsigned char>(*input) <= ' ' || *input == '\r' || *input == '\n')) {
        ++input;
    }
    size_t length = std::strlen(input);
    while (length > 0 && (static_cast<unsigned char>(input[length - 1]) <= ' ' || input[length - 1] == '\r' ||
                          input[length - 1] == '\n')) {
        --length;
    }
    if (length == 0 || length >= outputCapacity) {
        output[0] = '\0';
        return false;
    }
    std::memcpy(output, input, length);
    output[length] = '\0';
    return true;
}

inline const char* ExtractFileNameAscii(const char* path) {
    if (!path || path[0] == '\0') {
        return "";
    }
    const char* cursor = path;
    const char* lastSlash = nullptr;
    while (*cursor != '\0') {
        if (*cursor == '\\' || *cursor == '/') {
            lastSlash = cursor;
        }
        ++cursor;
    }
    return lastSlash ? (lastSlash + 1) : path;
}

inline bool EqualsAsciiCaseInsensitive(const char* a, const char* b) {
    if (!a || !b) {
        return false;
    }
    while (*a != '\0' && *b != '\0') {
        const char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(*a)));
        const char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(*b)));
        if (ca != cb) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

inline bool IsDiagnosticArmMatch(const char* targetProcess, const char* candidateProcessName) {
    if (!targetProcess || !candidateProcessName) {
        return false;
    }
    char trimmedTarget[MAX_PATH] = {};
    if (!TrimWhitespaceAscii(targetProcess, trimmedTarget, sizeof(trimmedTarget))) {
        return false;
    }
    const char* targetBaseName = ExtractFileNameAscii(trimmedTarget);
    const char* candidateBaseName = ExtractFileNameAscii(candidateProcessName);
    return EqualsAsciiCaseInsensitive(targetBaseName, candidateBaseName);
}

inline bool ReadDiagnosticArmMarkerFile(const char* filePath, char* outTarget, size_t outCapacity) {
    if (!filePath || !outTarget || outCapacity == 0) {
        return false;
    }
    FILE* file = std::fopen(filePath, "r");
    if (!file) {
        return false;
    }
    char lineBuffer[MAX_PATH] = {};
    const char* readResult = std::fgets(lineBuffer, static_cast<int>(sizeof(lineBuffer)), file);
    std::fclose(file);
    if (!readResult) {
        return false;
    }
    return TrimWhitespaceAscii(lineBuffer, outTarget, outCapacity);
}

inline bool ReadDiagnosticArmMarker(const char* sessionDir, char* outTarget, size_t outCapacity) {
    if (!outTarget || outCapacity == 0) {
        return false;
    }
    outTarget[0] = '\0';

    char envBuffer[MAX_PATH] = {};
    const DWORD envLength = GetEnvironmentVariableA(kDiagnosticArmEnvVar, envBuffer, sizeof(envBuffer));
    if (envLength > 0 && envLength < sizeof(envBuffer) && TrimWhitespaceAscii(envBuffer, outTarget, outCapacity)) {
        return true;
    }

    if (!sessionDir || sessionDir[0] == '\0') {
        return false;
    }

    char markerPath[MAX_PATH] = {};
    std::snprintf(markerPath, sizeof(markerPath), "%s\\%s", sessionDir, kDiagnosticArmMarkerFileName);
    if (ReadDiagnosticArmMarkerFile(markerPath, outTarget, outCapacity)) {
        return true;
    }

    std::snprintf(markerPath, sizeof(markerPath), "%s\\%s", sessionDir, kDiagnosticArmAltMarkerFileName);
    if (ReadDiagnosticArmMarkerFile(markerPath, outTarget, outCapacity)) {
        return true;
    }

    // Also check parent directory (e.g. logs root where marker is prepared before a session starts)
    std::snprintf(markerPath, sizeof(markerPath), "%s\\..\\%s", sessionDir, kDiagnosticArmMarkerFileName);
    if (ReadDiagnosticArmMarkerFile(markerPath, outTarget, outCapacity)) {
        return true;
    }

    std::snprintf(markerPath, sizeof(markerPath), "%s\\..\\%s", sessionDir, kDiagnosticArmAltMarkerFileName);
    if (ReadDiagnosticArmMarkerFile(markerPath, outTarget, outCapacity)) {
        return true;
    }

    // Also check install root (e.g. "C:\Program Files\Capture Engine\diagnostic_arm.txt")
    std::snprintf(markerPath, sizeof(markerPath), "%s\\..\\..\\%s", sessionDir, kDiagnosticArmMarkerFileName);
    if (ReadDiagnosticArmMarkerFile(markerPath, outTarget, outCapacity)) {
        return true;
    }

    std::snprintf(markerPath, sizeof(markerPath), "%s\\..\\..\\%s", sessionDir, kDiagnosticArmAltMarkerFileName);
    return ReadDiagnosticArmMarkerFile(markerPath, outTarget, outCapacity);
}

inline bool ShouldCaptureArmedTermination(bool diagnosticArmActive, DWORD exitCode) {
    return diagnosticArmActive && exitCode != 0;
}

}  // namespace ce::crash_dump_policy
