// Reading the payload appended to the setup executable.
//
// Files are decoded one block at a time (kBlockSize bytes of content), so memory
// stays bounded no matter how large the release is. LZMS comes from the Windows
// Compression API in cabinet.dll, resolved at run time so the setup file carries
// no import for it and the uninstaller - which never decodes - does not link it.

#include "setup.h"

#include <algorithm>

namespace ce::setup {
namespace {

constexpr DWORD kAlgorithmLzms = 5;  // COMPRESS_ALGORITHM_LZMS
using DecompressorHandle = void*;
using CreateDecompressorFn = BOOL(WINAPI*)(DWORD, void*, DecompressorHandle*);
using DecompressFn = BOOL(WINAPI*)(DecompressorHandle, const void*, SIZE_T, void*, SIZE_T, SIZE_T*);
using CloseDecompressorFn = BOOL(WINAPI*)(DecompressorHandle);

class Decompressor {
public:
    ~Decompressor() {
        if (handle_ && close_)
            close_(handle_);
        if (module_)
            FreeLibrary(module_);
    }
    bool Open() {
        module_ = LoadLibraryExW(L"cabinet.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_)
            return false;
        create_ = reinterpret_cast<CreateDecompressorFn>(GetProcAddress(module_, "CreateDecompressor"));
        decompress_ = reinterpret_cast<DecompressFn>(GetProcAddress(module_, "Decompress"));
        close_ = reinterpret_cast<CloseDecompressorFn>(GetProcAddress(module_, "CloseDecompressor"));
        return create_ && decompress_ && close_ && create_(kAlgorithmLzms, nullptr, &handle_);
    }
    bool Decode(const void* input, size_t inputSize, void* output, size_t outputSize, size_t* produced) {
        SIZE_T written = 0;
        if (!decompress_(handle_, input, inputSize, output, outputSize, &written))
            return false;
        *produced = written;
        return true;
    }

private:
    HMODULE module_ = nullptr;
    DecompressorHandle handle_ = nullptr;
    CreateDecompressorFn create_ = nullptr;
    DecompressFn decompress_ = nullptr;
    CloseDecompressorFn close_ = nullptr;
};

bool SeekTo(HANDLE file, uint64_t offset) {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    return SetFilePointerEx(file, position, nullptr, FILE_BEGIN) != FALSE;
}

bool ReadExact(HANDLE file, void* buffer, size_t size) {
    auto* bytes = static_cast<uint8_t*>(buffer);
    size_t done = 0;
    while (done < size) {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size - done, 0x10000000));
        if (!::ReadFile(file, bytes + done, chunk, &read, nullptr) || read == 0)
            return false;
        done += read;
    }
    return true;
}

}  // namespace

PayloadStatus PayloadReader::Open(const std::wstring& exePath) {
    file_.Reset(CreateFileW(exePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr));
    if (!file_.Valid()) {
        Log("payload: cannot open the setup file (error %lu)", GetLastError());
        return PayloadStatus::IoError;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file_.Get(), &size) || size.QuadPart <= 0)
        return PayloadStatus::IoError;
    const uint64_t fileSize = static_cast<uint64_t>(size.QuadPart);
    if (fileSize < kFooterSize)
        return PayloadStatus::TooSmall;
    uint8_t footerBytes[kFooterSize];
    if (!SeekTo(file_.Get(), fileSize - kFooterSize) || !ReadExact(file_.Get(), footerBytes, kFooterSize))
        return PayloadStatus::IoError;
    PayloadStatus status = ParseFooter(footerBytes, fileSize, &footer_);
    if (status != PayloadStatus::Ok)
        return status;
    std::vector<uint8_t> index(static_cast<size_t>(footer_.indexSize));
    if (!SeekTo(file_.Get(), footer_.indexOffset) || !ReadExact(file_.Get(), index.data(), index.size()))
        return PayloadStatus::IoError;
    status = ParseIndex(index.data(), index.size(), footer_, &files_);
    if (status == PayloadStatus::Ok)
        Log("payload: %zu files, %llu bytes of content", files_.size(),
            static_cast<unsigned long long>(footer_.totalSize));
    return status;
}

const FileEntry* PayloadReader::Find(std::string_view path) const {
    const std::string wanted = AsciiLowerCopy(path);
    for (const FileEntry& entry : files_) {
        if (AsciiLowerCopy(entry.path) == wanted)
            return &entry;
    }
    return nullptr;
}

PayloadStatus PayloadReader::DecodeFile(const FileEntry& entry, const Sink& sink, DWORD* win32Error,
                                        const std::function<void(uint64_t)>& progress) {
    const auto ioFailure = [&]() {
        if (win32Error)
            *win32Error = GetLastError();
        return PayloadStatus::IoError;
    };
    if (!SeekTo(file_.Get(), entry.offset))
        return ioFailure();
    uint32_t crc = 0;
    uint64_t produced = 0;
    uint64_t consumed = 0;
    std::vector<uint8_t> raw;
    std::vector<uint8_t> decoded;
    Decompressor decompressor;
    if (entry.method == Method::Lzms && !decompressor.Open()) {
        Log("payload: the Windows compression library is unavailable (error %lu)", GetLastError());
        return ioFailure();
    }
    while (produced < entry.size) {
        const uint32_t blockRaw = static_cast<uint32_t>(std::min<uint64_t>(kBlockSize, entry.size - produced));
        const uint8_t* content = nullptr;
        if (entry.method == Method::Store) {
            raw.resize(blockRaw);
            if (!ReadExact(file_.Get(), raw.data(), blockRaw))
                return ioFailure();
            consumed += blockRaw;
            content = raw.data();
        } else {
            uint8_t header[4];
            if (!ReadExact(file_.Get(), header, sizeof(header)))
                return ioFailure();
            const uint32_t storedLength = ReadLe32(header);
            if (!ValidBlockLength(storedLength, blockRaw) || consumed + 4 + storedLength > entry.storedSize)
                return PayloadStatus::BadBlock;
            raw.resize(storedLength);
            if (!ReadExact(file_.Get(), raw.data(), storedLength))
                return ioFailure();
            consumed += 4 + storedLength;
            if (storedLength == blockRaw) {
                content = raw.data();
            } else {
                decoded.resize(blockRaw);
                size_t written = 0;
                if (!decompressor.Decode(raw.data(), raw.size(), decoded.data(), decoded.size(), &written)) {
                    Log("payload: block decode failed for %s (error %lu)", entry.path.c_str(), GetLastError());
                    return PayloadStatus::BadBlock;
                }
                if (written != blockRaw)
                    return PayloadStatus::BadBlock;
                content = decoded.data();
            }
        }
        crc = Crc32Update(crc, content, blockRaw);
        if (!sink(content, blockRaw))
            return ioFailure();
        produced += blockRaw;
        if (progress)
            progress(produced);
    }
    if (consumed != entry.storedSize)
        return PayloadStatus::BadBlock;
    return crc == entry.crc ? PayloadStatus::Ok : PayloadStatus::BadFileChecksum;
}

PayloadStatus PayloadReader::ExtractFile(const FileEntry& entry, const std::wstring& destination, DWORD* win32Error,
                                         const std::function<void(uint64_t)>& progress) {
    Handle output(CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!output.Valid()) {
        if (win32Error)
            *win32Error = GetLastError();
        return PayloadStatus::IoError;
    }
    const auto sink = [&](const uint8_t* data, size_t size) {
        size_t done = 0;
        while (done < size) {
            DWORD written = 0;
            if (!::WriteFile(output.Get(), data + done, static_cast<DWORD>(std::min<size_t>(size - done, 0x10000000)),
                             &written, nullptr) ||
                written == 0)
                return false;
            done += written;
        }
        return true;
    };
    PayloadStatus status = DecodeFile(entry, sink, win32Error, progress);
    if (status == PayloadStatus::Ok && !FlushFileBuffers(output.Get())) {
        if (win32Error)
            *win32Error = GetLastError();
        status = PayloadStatus::IoError;
    }
    output.Reset();
    if (status != PayloadStatus::Ok)
        DeleteFileW(destination.c_str());
    return status;
}

PayloadStatus PayloadReader::ReadContents(const FileEntry& entry, std::string* contents, DWORD* win32Error) {
    constexpr uint64_t kMemoryLimit = 16u << 20;
    if (entry.size > kMemoryLimit)
        return PayloadStatus::TooLarge;
    contents->clear();
    contents->reserve(static_cast<size_t>(entry.size));
    const auto sink = [&](const uint8_t* data, size_t size) {
        contents->append(reinterpret_cast<const char*>(data), size);
        return true;
    };
    return DecodeFile(entry, sink, win32Error, {});
}

PayloadStatus PayloadReader::VerifyAll(DWORD* win32Error) {
    for (const FileEntry& entry : files_) {
        const PayloadStatus status = DecodeFile(entry, [](const uint8_t*, size_t) { return true; }, win32Error, {});
        if (status != PayloadStatus::Ok) {
            Log("payload: %s failed verification: %s", entry.path.c_str(), PayloadStatusText(status));
            return status;
        }
    }
    return PayloadStatus::Ok;
}

}  // namespace ce::setup
