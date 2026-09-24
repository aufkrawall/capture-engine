#include "config_text_encoding.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

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

struct FileEncodingCache {
    std::mutex mutex;
    std::string path;
    FILETIME lastWrite = {};
    uint64_t size = 0;
    bool utf8 = false;
    bool valid = false;
};

FileEncodingCache& Cache() {
    static FileEncodingCache cache;
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

bool IsUtf8ConfigFile(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &attributes)) {
        return false;
    }
    const uint64_t size = (static_cast<uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
    FileEncodingCache& cache = Cache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    if (cache.valid && cache.path == path && cache.size == size &&
        CompareFileTime(&cache.lastWrite, &attributes.ftLastWriteTime) == 0) {
        return cache.utf8;
    }
    std::string bytes;
    if (!ReadWholeFile(path, &bytes)) {
        return false;
    }
    cache.path = path;
    cache.lastWrite = attributes.ftLastWriteTime;
    cache.size = size;
    cache.utf8 = IsUtf8ConfigText(bytes);
    cache.valid = true;
    return cache.utf8;
}

}  // namespace

std::string ConfigValueToNative(const std::string& path, std::string value) {
    if (!ContainsNonAscii(value) || !IsValidUtf8(value)) {
        return value;
    }
    const UINT activeCodePage = GetACP();
    if (activeCodePage == CP_UTF8 || !IsUtf8ConfigFile(path)) {
        return value;
    }
    bool lossy = false;
    std::string native = Utf8ToCodePage(value, activeCodePage, &lossy);
    if (lossy) {
        static std::atomic<uint32_t> s_lossyLogs{0};
        if (s_lossyLogs.fetch_add(1, std::memory_order_relaxed) < 8) {
            LogWarn(
                "Config: a setting contains characters the Windows code page (%u) cannot represent; they were "
                "replaced with '?'. Paths and names with such characters are not supported yet",
                static_cast<unsigned>(activeCodePage));
        }
    }
    return native;
}

std::string ReadIniValue(const std::string& path, const char* section, const char* key, const char* defaultValue) {
    char buffer[4096];
    GetPrivateProfileStringA(section, key, defaultValue, buffer, sizeof(buffer), path.c_str());
    return ConfigValueToNative(path, buffer);
}

}  // namespace ce::config_text
