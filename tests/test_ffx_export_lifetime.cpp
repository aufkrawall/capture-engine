#include <gtest/gtest.h>

#include <windows.h>

#include <cstdint>
#include <string>

#include "../hook/apis/ffx_export_lifetime.h"

namespace {

// A private copy gives this test the DLL's only application reference. No
// renderer, driver or timing assumptions are needed to exercise a last unload.
class PrivateExportImage {
public:
    PrivateExportImage() {
        wchar_t systemDirectory[MAX_PATH] = {};
        wchar_t temporaryDirectory[MAX_PATH] = {};
        wchar_t temporaryFile[MAX_PATH] = {};
        if (!GetSystemDirectoryW(systemDirectory, MAX_PATH) || !GetTempPathW(MAX_PATH, temporaryDirectory) ||
            !GetTempFileNameW(temporaryDirectory, L"cex", 0, temporaryFile)) {
            return;
        }
        path_ = temporaryFile;
        const std::wstring source = std::wstring(systemDirectory) + L"\\version.dll";
        if (CopyFileW(source.c_str(), path_.c_str(), FALSE))
            module_ = LoadLibraryExW(path_.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    }

    ~PrivateExportImage() {
        ReleaseOwner();
        if (!path_.empty())
            DeleteFileW(path_.c_str());
    }

    PrivateExportImage(const PrivateExportImage&) = delete;
    PrivateExportImage& operator=(const PrivateExportImage&) = delete;

    HMODULE Get() const { return module_; }
    const wchar_t* Path() const { return path_.c_str(); }
    void* Export() const {
        return module_ ? reinterpret_cast<void*>(GetProcAddress(module_, "GetFileVersionInfoSizeW")) : nullptr;
    }
    bool ReleaseOwner() {
        if (!module_)
            return true;
        HMODULE module = module_;
        module_ = nullptr;
        return FreeLibrary(module) != FALSE;
    }

private:
    std::wstring path_;
    HMODULE module_ = nullptr;
};

TEST(FFXExportLifetimeTest, LastApplicationUnloadCannotUnmapAnExportWhilePinned) {
    PrivateExportImage image;
    HMODULE module = image.Get();
    void* target = image.Export();
    ASSERT_NE(module, nullptr);
    ASSERT_NE(target, nullptr);
    {
        ce::ffx_export_lifetime::ModulePin pin(module, target, "GetFileVersionInfoSizeW");
        ASSERT_EQ(pin.Get(), module);
        // Deterministically unload after the export check and before its entry
        // is inspected, the exact ordering that raced breakpoint restoration.
        ASSERT_TRUE(image.ReleaseOwner());
        EXPECT_EQ(GetModuleHandleW(image.Path()), module);
        MEMORY_BASIC_INFORMATION memory = {};
        ASSERT_NE(VirtualQuery(target, &memory, sizeof(memory)), 0u);
        EXPECT_EQ(memory.State, static_cast<DWORD>(MEM_COMMIT));
        EXPECT_EQ(memory.AllocationBase, static_cast<void*>(module));
        const volatile uint8_t entryByte = *static_cast<const volatile uint8_t*>(target);
        (void)entryByte;
    }
    EXPECT_EQ(GetModuleHandleW(image.Path()), nullptr) << "the pin must also let the runtime unload afterwards";
}

TEST(FFXExportLifetimeTest, AlreadyUnloadedExportIsRejectedWithoutReadingItsAddress) {
    PrivateExportImage image;
    HMODULE module = image.Get();
    void* target = image.Export();
    ASSERT_NE(module, nullptr);
    ASSERT_NE(target, nullptr);
    ASSERT_TRUE(image.ReleaseOwner());
    ASSERT_EQ(GetModuleHandleW(image.Path()), nullptr);
    ce::ffx_export_lifetime::ModulePin pin(module, target, "GetFileVersionInfoSizeW");
    EXPECT_EQ(pin.Get(), nullptr);
}

TEST(FFXExportLifetimeTest, WrongImageOrExportIsRejectedAndTheAcquiredReferenceIsReleased) {
    PrivateExportImage image;
    ASSERT_NE(image.Get(), nullptr);
    ASSERT_NE(image.Export(), nullptr);
    {
        ce::ffx_export_lifetime::ModulePin wrongImage(GetModuleHandleW(L"kernel32.dll"), image.Export(),
                                                    "GetFileVersionInfoSizeW");
        EXPECT_EQ(wrongImage.Get(), nullptr);
        ce::ffx_export_lifetime::ModulePin wrongExport(image.Get(), image.Export(), "GetFileVersionInfoW");
        EXPECT_EQ(wrongExport.Get(), nullptr);
        ce::ffx_export_lifetime::ModulePin noTarget(image.Get(), nullptr, "GetFileVersionInfoSizeW");
        EXPECT_EQ(noTarget.Get(), nullptr);
    }
    ASSERT_TRUE(image.ReleaseOwner());
    EXPECT_EQ(GetModuleHandleW(image.Path()), nullptr) << "a refused pin must not leak a reference";
}

}  // namespace
