#include "config_text_encoding.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "config_ini_reader.h"
#include "logging.h"

namespace ce::config_text {

bool HasUtf8Bom(std::string_view bytes) {
    return bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
           static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF;
}

bool ContainsNonAscii(std::string_view bytes) {
    for (const char ch : bytes) {
        if (static_cast<unsigned char>(ch) >= 0x80) {
            return true;
        }
    }
    return false;
}

bool IsValidUtf8(std::string_view bytes) {
    size_t i = 0;
    while (i < bytes.size()) {
        const unsigned char lead = static_cast<unsigned char>(bytes[i]);
        if (lead < 0x80) {
            ++i;
            continue;
        }
        size_t length = 0;
        uint32_t codePoint = 0;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
            codePoint = lead & 0x1Fu;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            codePoint = lead & 0x0Fu;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            codePoint = lead & 0x07u;
        } else {
            return false;  // continuation byte, overlong 2-byte lead, or out of range
        }
        if (i + length > bytes.size()) {
            return false;
        }
        for (size_t k = 1; k < length; ++k) {
            const unsigned char next = static_cast<unsigned char>(bytes[i + k]);
            if ((next & 0xC0u) != 0x80u) {
                return false;
            }
            codePoint = (codePoint << 6) | (next & 0x3Fu);
        }
        if ((length == 3 && codePoint < 0x800) || (length == 4 && codePoint < 0x10000) ||
            (codePoint >= 0xD800 && codePoint <= 0xDFFF) || codePoint > 0x10FFFF) {
            return false;
        }
        i += length;
    }
    return true;
}

bool IsUtf8ConfigText(std::string_view fileBytes) {
    if (HasUtf8Bom(fileBytes)) {
        return true;
    }
    return ContainsNonAscii(fileBytes) && IsValidUtf8(fileBytes);
}

std::string Utf8ToCodePage(std::string_view utf8, unsigned codePage, bool* lossy) {
    if (lossy) {
        *lossy = false;
    }
    if (utf8.empty()) {
        return {};
    }
    const int wideLength =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wideLength <= 0) {
        return std::string(utf8);
    }
    std::wstring wide(static_cast<size_t>(wideLength), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                        wideLength);
    if (codePage == CP_UTF8) {
        return std::string(utf8);
    }
    BOOL usedDefault = FALSE;
    const int narrowLength = WideCharToMultiByte(codePage, WC_NO_BEST_FIT_CHARS, wide.data(), wideLength, nullptr, 0,
                                                 "?", &usedDefault);
    if (narrowLength <= 0) {
        return std::string(utf8);
    }
    std::string narrow(static_cast<size_t>(narrowLength), '\0');
    usedDefault = FALSE;
    WideCharToMultiByte(codePage, WC_NO_BEST_FIT_CHARS, wide.data(), wideLength, narrow.data(), narrowLength, "?",
                        &usedDefault);
    if (lossy) {
        *lossy = usedDefault != FALSE;
    }
    return narrow;
}

namespace {

// The parsed form of the last config file read, keyed by its path, write time
// and size. A config load asks for a few hundred values; the file is read and
// parsed once per change, not once per value.
struct ConfigDocumentCache {
    std::mutex mutex;
    std::string path;
    FILETIME lastWrite = {};
    uint64_t size = 0;
    bool valid = false;
    std::shared_ptr<const IniDocument> utf8Document;  // null for an ANSI (or unreadable) file
};

ConfigDocumentCache& Cache() {
    static ConfigDocumentCache cache;
    return cache;
}

bool ReadWholeFile(const std::string& path, std::string* bytes) {
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size = {};
    // A config file is a few tens of kilobytes; refuse anything absurd.
    constexpr LONGLONG kMaximumConfigBytes = 16LL * 1024 * 1024;
    const bool sized = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0 && size.QuadPart <= kMaximumConfigBytes;
    bool ok = sized;
    if (sized) {
        bytes->assign(static_cast<size_t>(size.QuadPart), '\0');
        DWORD read = 0;
        ok = size.QuadPart == 0 ||
             (ReadFile(file, bytes->data(), static_cast<DWORD>(size.QuadPart), &read, nullptr) != FALSE &&
              read == static_cast<DWORD>(size.QuadPart));
    }
    CloseHandle(file);
    return ok;
}

std::atomic<uint64_t> g_configReadFailures{0};

void NoteConfigReadFailure(const std::string& path) {
    const uint64_t failures = g_configReadFailures.fetch_add(1, std::memory_order_acq_rel) + 1;
    // Rate-limited: a locked file is re-tried by every key lookup of a load.
    if (failures <= 4 || (failures % 256) == 0) {
        LogWarn("Config: could not read %s (error %lu, failure #%llu); values fall back to defaults for this read",
                path.c_str(), static_cast<unsigned long>(GetLastError()), static_cast<unsigned long long>(failures));
    }
}

// The parsed document when `path` is a UTF-8 file, null when it is ANSI text
// (which keeps the profile API and its exact legacy semantics). *readOk reports
// whether the file's bytes were available (cached or read now).
std::shared_ptr<const IniDocument> Utf8DocumentFor(const std::string& path, bool* readOk = nullptr) {
    if (readOk) {
        *readOk = false;
    }
    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &attributes)) {
        NoteConfigReadFailure(path);
        return nullptr;
    }
    const uint64_t size = (static_cast<uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
    ConfigDocumentCache& cache = Cache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    if (cache.valid && cache.path == path && cache.size == size &&
        CompareFileTime(&cache.lastWrite, &attributes.ftLastWriteTime) == 0) {
        if (readOk) {
            *readOk = true;
        }
        return cache.utf8Document;
    }
    std::string bytes;
    if (!ReadWholeFile(path, &bytes)) {
        // A UTF-8 file then falls through to the profile API (lossy on DBCS, or all
        // defaults when the file is locked). Counted so a reload can refuse to publish it.
        NoteConfigReadFailure(path);
        return nullptr;
    }
    if (readOk) {
        *readOk = true;
    }
    cache.path = path;
    cache.lastWrite = attributes.ftLastWriteTime;
    cache.size = size;
    cache.utf8Document =
        IsUtf8ConfigText(bytes) ? std::make_shared<const IniDocument>(ParseUtf8Ini(bytes)) : nullptr;
    cache.valid = true;
    return cache.utf8Document;
}

void NoteLossyValue(unsigned codePage) {
    static std::atomic<uint32_t> s_lossyLogs{0};
    if (s_lossyLogs.fetch_add(1, std::memory_order_relaxed) < 8) {
        LogWarn(
            "Config: a setting contains characters the Windows code page (%u) cannot represent; they were "
            "replaced with '?'. Paths and names with such characters are not supported yet",
            codePage);
    }
}

// What GetPrivateProfileString does to a default: trailing blanks go.
std::string ProfileDefault(const char* defaultValue) {
    std::string value = defaultValue ? defaultValue : "";
    while (!value.empty() && value.back() == ' ') {
        value.pop_back();
    }
    return value;
}

}  // namespace

std::string ReadIniValueForCodePage(const std::string& path, const char* section, const char* key,
                                    const char* defaultValue, unsigned codePage) {
    if (const std::shared_ptr<const IniDocument> document = Utf8DocumentFor(path)) {
        std::string value;
        bool lossy = false;
        if (!LookupIniValue(*document, section ? section : "", key ? key : "", codePage, &value, &lossy)) {
            return ProfileDefault(defaultValue);
        }
        if (lossy) {
            NoteLossyValue(codePage);
        }
        // The profile API answers through a 4096-byte buffer; so did every caller.
        constexpr size_t kProfileValueLimit = 4095;
        if (value.size() > kProfileValueLimit) {
            value.resize(kProfileValueLimit);
        }
        return value;
    }
    char buffer[4096];
    GetPrivateProfileStringA(section, key, defaultValue, buffer, sizeof(buffer), path.c_str());
    return buffer;
}

uint64_t ConfigReadFailureCount() {
    return g_configReadFailures.load(std::memory_order_acquire);
}

bool PrimeConfigDocument(const std::string& path) {
    bool readOk = false;
    Utf8DocumentFor(path, &readOk);
    return readOk;
}

std::string ReadIniValue(const std::string& path, const char* section, const char* key, const char* defaultValue) {
    return ReadIniValueForCodePage(path, section, key, defaultValue, GetACP());
}

std::vector<std::string> ReadIniSectionNames(const std::string& path) {
    if (const std::shared_ptr<const IniDocument> document = Utf8DocumentFor(path)) {
        return IniSectionNames(*document, GetACP());
    }
    std::vector<char> names(4096, '\0');
    DWORD copied = 0;
    for (;;) {
        copied = GetPrivateProfileSectionNamesA(names.data(), static_cast<DWORD>(names.size()), path.c_str());
        if (copied < names.size() - 2 || names.size() >= 1024 * 1024)
            break;
        names.assign(names.size() * 2, '\0');
    }
    std::vector<std::string> sections;
    for (const char* current = names.data(); current && *current; current += strlen(current) + 1) {
        sections.emplace_back(current);
    }
    return sections;
}

bool ReadIniSectionLines(const std::string& path, const std::string& section, std::vector<std::string>* lines) {
    if (const std::shared_ptr<const IniDocument> document = Utf8DocumentFor(path)) {
        return IniSectionLines(*document, section, GetACP(), lines);
    }
    std::vector<char> buffer(4096, '\0');
    const DWORD chars =
        GetPrivateProfileSectionA(section.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()), path.c_str());
    if (chars == 0 || chars >= buffer.size() - 2) {
        return false;
    }
    if (lines) {
        lines->clear();
        for (const char* p = buffer.data(); *p; p += strlen(p) + 1) {
            lines->emplace_back(p);
        }
    }
    return true;
}

}  // namespace ce::config_text
