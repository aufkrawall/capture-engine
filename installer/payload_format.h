#pragma once

// Container format appended to the setup executable, and every bounds check the
// installer performs on it. Pure C++ (no Windows headers) so the unit tests and
// the Python packer tools/installer_payload.py describe exactly the same bytes.
//
//   [ setup stub PE ][ file data ... ][ index ][ 64-byte footer ]
//
// The footer is read from the end of the file. The index lists every payload
// file; each file is a sequence of blocks (kBlockSize bytes of content each).
// Method::Store keeps the content raw. Method::Lzms prefixes every block with
// its compressed length (u32 little endian); a block whose compressed length
// equals its content length is stored raw, so incompressible data never grows.
//
// Integrity, not authenticity: CRC-32 detects a damaged download. Nothing here
// is a publisher signature.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace ce::setup {

inline constexpr char kFooterMagic[8] = {'C', 'E', 'S', 'E', 'T', 'U', 'P', '1'};
inline constexpr uint32_t kFormatVersion = 1;
inline constexpr size_t kFooterSize = 64;
inline constexpr uint32_t kBlockSize = 1u << 20;
inline constexpr uint32_t kMaxFiles = 4096;
inline constexpr uint64_t kMaxTotalBytes = 8ull << 30;
inline constexpr size_t kMaxPathChars = 240;
inline constexpr size_t kMaxIndexBytes = 4u << 20;

enum class Method : uint32_t { Store = 0, Lzms = 1 };

enum class PayloadStatus {
    Ok,
    TooSmall,
    BadMagic,
    BadVersion,
    BadFooterChecksum,
    BadBounds,
    BadIndexChecksum,
    BadIndex,
    BadPath,
    DuplicatePath,
    TooManyFiles,
    TooLarge,
    BadFileRange,
    BadMethod,
    BadBlock,
    BadFileChecksum,
    IoError,
};

inline const char* PayloadStatusText(PayloadStatus status) {
    switch (status) {
    case PayloadStatus::Ok:
        return "ok";
    case PayloadStatus::TooSmall:
        return "the setup file is too small to carry a payload";
    case PayloadStatus::BadMagic:
        return "the setup file carries no payload footer";
    case PayloadStatus::BadVersion:
        return "the payload was written by an incompatible setup version";
    case PayloadStatus::BadFooterChecksum:
        return "the payload footer is damaged";
    case PayloadStatus::BadBounds:
        return "the payload footer points outside the setup file";
    case PayloadStatus::BadIndexChecksum:
        return "the payload index is damaged";
    case PayloadStatus::BadIndex:
        return "the payload index is malformed";
    case PayloadStatus::BadPath:
        return "the payload names a file outside the installation folder";
    case PayloadStatus::DuplicatePath:
        return "the payload lists a file twice";
    case PayloadStatus::TooManyFiles:
        return "the payload lists too many files";
    case PayloadStatus::TooLarge:
        return "the payload is larger than any valid release";
    case PayloadStatus::BadFileRange:
        return "a payload file points outside the payload area";
    case PayloadStatus::BadMethod:
        return "a payload file uses an unknown storage method";
    case PayloadStatus::BadBlock:
        return "a payload block is malformed";
    case PayloadStatus::BadFileChecksum:
        return "a payload file failed its checksum; this setup file is damaged";
    case PayloadStatus::IoError:
        return "the payload could not be read or its files could not be written";
    }
    return "unknown payload error";
}

// ---------------------------------------------------------------------------
// CRC-32 (IEEE 802.3, the zlib polynomial)
// ---------------------------------------------------------------------------

namespace detail {
constexpr std::array<uint32_t, 256> MakeCrcTable() {
    std::array<uint32_t, 256> table{};
    for (uint32_t index = 0; index < 256; ++index) {
        uint32_t value = index;
        for (int bit = 0; bit < 8; ++bit)
            value = (value & 1u) ? (value >> 1) ^ 0xEDB88320u : value >> 1;
        table[index] = value;
    }
    return table;
}
inline constexpr std::array<uint32_t, 256> kCrcTable = MakeCrcTable();
}  // namespace detail

// Streaming form: start with 0 and feed the previous result back in.
inline uint32_t Crc32Update(uint32_t crc, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint32_t value = ~crc;
    for (size_t index = 0; index < size; ++index)
        value = detail::kCrcTable[(value ^ bytes[index]) & 0xFFu] ^ (value >> 8);
    return ~value;
}

// ---------------------------------------------------------------------------
// Little-endian helpers
// ---------------------------------------------------------------------------

inline uint16_t ReadLe16(const uint8_t* bytes) {
    return static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
}
inline uint32_t ReadLe32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}
inline uint64_t ReadLe64(const uint8_t* bytes) {
    return static_cast<uint64_t>(ReadLe32(bytes)) | (static_cast<uint64_t>(ReadLe32(bytes + 4)) << 32);
}
inline void WriteLe16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}
inline void WriteLe32(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<uint8_t>(value >> shift));
}
inline void WriteLe64(std::vector<uint8_t>& out, uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8)
        out.push_back(static_cast<uint8_t>(value >> shift));
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

inline bool IsValidUtf8(std::string_view text) {
    size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<uint8_t>(text[index]);
        size_t extra = 0;
        uint32_t minimum = 0;
        if (lead < 0x80)
            extra = 0;
        else if ((lead & 0xE0) == 0xC0) {
            extra = 1;
            minimum = 0x80;
        } else if ((lead & 0xF0) == 0xE0) {
            extra = 2;
            minimum = 0x800;
        } else if ((lead & 0xF8) == 0xF0) {
            extra = 3;
            minimum = 0x10000;
        } else
            return false;
        uint32_t code = extra == 0 ? lead : lead & (0x3Fu >> extra);
        for (size_t offset = 1; offset <= extra; ++offset) {
            if (index + offset >= text.size())
                return false;
            const auto next = static_cast<uint8_t>(text[index + offset]);
            if ((next & 0xC0) != 0x80)
                return false;
            code = (code << 6) | (next & 0x3Fu);
        }
        if (extra != 0 && (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)))
            return false;
        index += extra + 1;
    }
    return true;
}

inline char AsciiLower(char value) {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

inline std::string AsciiLowerCopy(std::string_view text) {
    std::string result(text);
    for (char& character : result)
        character = AsciiLower(character);
    return result;
}

inline bool IsReservedDeviceName(std::string_view component) {
    // The device name applies with or without an extension ("nul.txt" is NUL).
    const size_t dot = component.find('.');
    const std::string stem = AsciiLowerCopy(component.substr(0, dot));
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" || stem == "conin$" || stem == "conout$")
        return true;
    return stem.size() == 4 && (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0) &&
           stem[3] >= '1' && stem[3] <= '9';
}

// A payload path is a relative, forward-slash separated Windows-safe name. It
// can never leave the installation folder, name a drive or stream, or address a
// device, so extraction needs no further sanitising.
inline bool IsSafeRelativePath(std::string_view path) {
    if (path.empty() || path.size() > kMaxPathChars || !IsValidUtf8(path))
        return false;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string_view::npos)
            end = path.size();
        const std::string_view component = path.substr(start, end - start);
        if (component.empty() || component == "." || component == "..")
            return false;
        if (component.back() == '.' || component.back() == ' ')
            return false;
        for (char character : component) {
            const auto code = static_cast<unsigned char>(character);
            if (code < 0x20 || character == '\\' || character == ':' || character == '*' || character == '?' ||
                character == '"' || character == '<' || character == '>' || character == '|')
                return false;
        }
        if (IsReservedDeviceName(component))
            return false;
        if (end == path.size())
            break;
        start = end + 1;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Footer and index
// ---------------------------------------------------------------------------

struct Footer {
    uint64_t payloadOffset = 0;
    uint64_t indexOffset = 0;
    uint64_t indexSize = 0;
    uint64_t totalSize = 0;
    uint32_t fileCount = 0;
    uint32_t indexCrc = 0;
};

struct FileEntry {
    std::string path;
    Method method = Method::Store;
    uint32_t flags = 0;
    uint64_t offset = 0;
    uint64_t storedSize = 0;
    uint64_t size = 0;
    uint32_t crc = 0;
};

inline uint64_t BlockCount(uint64_t size) {
    return (size + kBlockSize - 1) / kBlockSize;
}

// Footer layout (little endian):
//   0 magic[8]  8 version  12 flags  16 payloadOffset  24 indexOffset
//  32 indexSize 40 totalSize 48 fileCount 52 indexCrc 56 reserved 60 footerCrc
inline PayloadStatus ParseFooter(const uint8_t* bytes, uint64_t fileSize, Footer* footer) {
    if (fileSize < kFooterSize)
        return PayloadStatus::TooSmall;
    if (std::memcmp(bytes, kFooterMagic, sizeof(kFooterMagic)) != 0)
        return PayloadStatus::BadMagic;
    if (ReadLe32(bytes + 8) != kFormatVersion)
        return PayloadStatus::BadVersion;
    if (Crc32Update(0, bytes, kFooterSize - 4) != ReadLe32(bytes + 60))
        return PayloadStatus::BadFooterChecksum;
    Footer parsed;
    parsed.payloadOffset = ReadLe64(bytes + 16);
    parsed.indexOffset = ReadLe64(bytes + 24);
    parsed.indexSize = ReadLe64(bytes + 32);
    parsed.totalSize = ReadLe64(bytes + 40);
    parsed.fileCount = ReadLe32(bytes + 48);
    parsed.indexCrc = ReadLe32(bytes + 52);
    const uint64_t footerStart = fileSize - kFooterSize;
    if (parsed.payloadOffset > parsed.indexOffset || parsed.indexOffset > footerStart ||
        parsed.indexSize != footerStart - parsed.indexOffset || parsed.indexSize == 0 ||
        parsed.indexSize > kMaxIndexBytes)
        return PayloadStatus::BadBounds;
    if (parsed.fileCount == 0 || parsed.fileCount > kMaxFiles)
        return PayloadStatus::TooManyFiles;
    if (parsed.totalSize > kMaxTotalBytes)
        return PayloadStatus::TooLarge;
    *footer = parsed;
    return PayloadStatus::Ok;
}

// Entry layout: u16 pathLength, path, u32 method, u32 flags, u64 offset,
// u64 storedSize, u64 size, u32 crc, u32 reserved.
inline PayloadStatus ParseIndex(const uint8_t* bytes, size_t size, const Footer& footer,
                                std::vector<FileEntry>* entries) {
    if (Crc32Update(0, bytes, size) != footer.indexCrc)
        return PayloadStatus::BadIndexChecksum;
    std::vector<FileEntry> parsed;
    parsed.reserve(footer.fileCount);
    size_t cursor = 0;
    uint64_t totalContent = 0;
    for (uint32_t index = 0; index < footer.fileCount; ++index) {
        if (size - cursor < 2)
            return PayloadStatus::BadIndex;
        const size_t pathLength = ReadLe16(bytes + cursor);
        cursor += 2;
        constexpr size_t kFixed = 4 + 4 + 8 + 8 + 8 + 4 + 4;
        if (pathLength == 0 || pathLength > kMaxPathChars || size - cursor < pathLength + kFixed)
            return PayloadStatus::BadIndex;
        FileEntry entry;
        entry.path.assign(reinterpret_cast<const char*>(bytes + cursor), pathLength);
        cursor += pathLength;
        const uint32_t method = ReadLe32(bytes + cursor);
        entry.flags = ReadLe32(bytes + cursor + 4);
        entry.offset = ReadLe64(bytes + cursor + 8);
        entry.storedSize = ReadLe64(bytes + cursor + 16);
        entry.size = ReadLe64(bytes + cursor + 24);
        entry.crc = ReadLe32(bytes + cursor + 32);
        cursor += kFixed;
        if (!IsSafeRelativePath(entry.path))
            return PayloadStatus::BadPath;
        if (method > static_cast<uint32_t>(Method::Lzms))
            return PayloadStatus::BadMethod;
        entry.method = static_cast<Method>(method);
        if (entry.size > kMaxTotalBytes || totalContent > kMaxTotalBytes - entry.size)
            return PayloadStatus::TooLarge;
        totalContent += entry.size;
        // Overflow-safe: offset and storedSize are bounded by the data area.
        if (entry.offset < footer.payloadOffset || entry.offset > footer.indexOffset ||
            entry.storedSize > footer.indexOffset - entry.offset)
            return PayloadStatus::BadFileRange;
        const uint64_t blocks = BlockCount(entry.size);
        if (entry.method == Method::Store ? entry.storedSize != entry.size
                                          : (entry.storedSize < blocks * 4 || entry.storedSize > entry.size + blocks * 4))
            return PayloadStatus::BadFileRange;
        parsed.push_back(std::move(entry));
    }
    if (cursor != size)
        return PayloadStatus::BadIndex;
    if (totalContent != footer.totalSize)
        return PayloadStatus::BadIndex;

    // Case-insensitive duplicates would overwrite each other on NTFS.
    std::vector<std::string> names;
    names.reserve(parsed.size());
    for (const FileEntry& entry : parsed)
        names.push_back(AsciiLowerCopy(entry.path));
    std::sort(names.begin(), names.end());
    if (std::adjacent_find(names.begin(), names.end()) != names.end())
        return PayloadStatus::DuplicatePath;

    // Stored ranges must not overlap, or one file could read another's bytes.
    std::vector<const FileEntry*> ordered;
    ordered.reserve(parsed.size());
    for (const FileEntry& entry : parsed)
        ordered.push_back(&entry);
    std::sort(ordered.begin(), ordered.end(),
              [](const FileEntry* left, const FileEntry* right) { return left->offset < right->offset; });
    for (size_t index = 1; index < ordered.size(); ++index) {
        const FileEntry& previous = *ordered[index - 1];
        if (previous.offset + previous.storedSize > ordered[index]->offset)
            return PayloadStatus::BadFileRange;
    }
    *entries = std::move(parsed);
    return PayloadStatus::Ok;
}

// Validates one block's framing. `rawSize` is the content length of the block;
// the stored length is bounded by it because the packer never expands a block.
inline bool ValidBlockLength(uint32_t storedLength, uint32_t rawSize) {
    return storedLength != 0 && storedLength <= rawSize;
}

}  // namespace ce::setup
