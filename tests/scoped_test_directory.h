#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace ce::test_support {

// Own only a directory this instance created exclusively. Windows can still
// retire a deleted DLL/file after its last handle closes, so neither a clock
// tick nor a fixed test name is a safe lifetime identifier. A directory removed
// while another process (antivirus, indexer, a just-exited child) still holds a
// handle stays delete-pending, and creating that name again fails with
// ERROR_ACCESS_DENIED until the handle closes: fixtures that deleted and
// recreated one fixed name per test failed exactly that way. Never reuse a
// name; a delete-pending leftover of an earlier process with the same id is
// skipped like an existing directory.
class ScopedTestDirectory {
public:
    explicit ScopedTestDirectory(const char* prefix) {
        static std::atomic<uint64_t> sequence{0};
        constexpr int kMaxUnusableNames = 64;
        const auto parent = std::filesystem::temp_directory_path();
        for (int unusable = 0;;) {
            path_ = parent / (std::string(prefix) + "_" + std::to_string(GetCurrentProcessId()) + "_" +
                              std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error))
                break;
            const bool deletePending = error == std::errc::permission_denied ||
                                       (error.category() == std::system_category() &&
                                        error.value() == ERROR_ACCESS_DENIED);
            if (error && (!deletePending || ++unusable >= kMaxUnusableNames))
                throw std::filesystem::filesystem_error("create test directory", path_, error);
        }
    }

    ~ScopedTestDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    ScopedTestDirectory(const ScopedTestDirectory&) = delete;
    ScopedTestDirectory& operator=(const ScopedTestDirectory&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace ce::test_support
