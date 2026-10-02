#include "startup_control.h"
#include "common/ipc/elevation_windows.h"
#include <bcrypt.h>
#include <filesystem>
#include <array>

namespace ce::startup {
namespace {
bool HasDigest(const std::filesystem::path& path, const char* expected) {
    ce::elevation::Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file)
        return false;
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(file.Get(), &information) ||
        information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return false;
    bool valid = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    std::array<unsigned char, 65536> buffer{};
    DWORD read = 0;
    while (valid) {
        if (!ReadFile(file.Get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            valid = false;
            break;
        }
        if (!read)
            break;
        valid = BCryptHashData(hash, buffer.data(), read, 0) == 0;
    }
    std::array<unsigned char, 32> digest{};
    if (valid)
        valid = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0;
    if (hash)
        BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    constexpr char hex[] = "0123456789abcdef";
    for (size_t index = 0; valid && index < digest.size(); ++index)
        valid = expected[index * 2] == hex[digest[index] >> 4] && expected[index * 2 + 1] == hex[digest[index] & 15];
    return valid;
}
}  // namespace

DWORD ValidateSensorRuntime(const std::wstring& directory) {
    // Keep synchronized with the build's pinned official LibreHardwareMonitor v0.9.6 closure.
    const std::pair<const wchar_t*, const char*> files[] = {
        {L"LibreHardwareMonitorLib.dll", "6ebc194316536ba61af5be24508ad9fcbb2ecc685e716c12e787c79530f66bf0"},
        {L"System.Memory.dll", "d5e8e4866f9cfa66f7765660f84b210198893e55335487afe5ebda342c0e913d"},
        {L"System.Numerics.Vectors.dll", "20c2fa81b8c70d651099d762954f285fd4f942e63b2d7217c145dab8d4b2f4c9"},
        {L"System.Runtime.CompilerServices.Unsafe.dll",
         "08cbd7278b66f1e68425a82d4b97181a4130d93e3dd91831407aba7212ccdacf"},
    };
    const auto plugin = std::filesystem::path(directory) / L"plugins" / L"LibreHardwareMonitor";
    for (const auto& file : files)
        if (!HasDigest(plugin / file.first, file.second))
            return ERROR_INVALID_IMAGE_HASH;
    return ERROR_SUCCESS;
}
}  // namespace ce::startup
