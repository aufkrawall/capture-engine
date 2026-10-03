#include <gtest/gtest.h>

#include "common/setup/elevation_runtime_policy.h"
#include "common/ipc/elevation_windows.h"

namespace {

using namespace ce::startup;
namespace fs = std::filesystem;
constexpr wchar_t kRuntime[] = L"runtime-0123456789abcdef0123456789abcdef";

TEST(ElevationRuntimePolicyTest, UsesActualApplicationFolderIncludingSpacesAndCustomLocations) {
    for (const fs::path& folder :
         {fs::path(LR"(C:\Program Files\Capture Engine)"), fs::path(LR"(D:\Apps\Custom Capture Folder)"),
          fs::path(LR"(C:\portable\captureengine)"), fs::path(LR"(\\server\share\Capture Engine)"), fs::path(LR"(D:\)")}) {
        const fs::path directory = ServiceDirectoryForExecutable(folder / L"captureengine.exe");
        EXPECT_EQ(directory, folder / L"ElevationService");
        const fs::path runtime = directory / kRuntime;
        const std::wstring binary =
            ce::elevation::QuoteArgument((runtime / L"captureengine_elevation_service.exe").wstring());
        EXPECT_EQ(RegisteredServiceRuntime(binary), runtime);
    }
}

TEST(ElevationRuntimePolicyTest, MissingOrRelativeExecutableCannotSelectUnrelatedFolder) {
    for (const fs::path& executable : {fs::path(), fs::path(L"captureengine.exe"),
                                       fs::path(LR"(relative\captureengine.exe)"), fs::path(LR"(C:\Apps\)")})
        EXPECT_TRUE(ServiceDirectoryForExecutable(executable).empty());
}

TEST(ElevationRuntimePolicyTest, RecognizesLegacyAndRelocatedRegistrationsForReplacementAndRemoval) {
    const fs::path current = LR"(C:\Program Files\Capture Engine\captureengine.exe)";
    for (const fs::path& folder :
         {fs::path(LR"(C:\Program Files\CaptureEngine)"), fs::path(LR"(D:\Previous Install\Capture Engine)")}) {
        const fs::path oldRuntime = folder / L"ElevationService" / kRuntime;
        const auto oldBinary =
            ce::elevation::QuoteArgument((oldRuntime / L"captureengine_elevation_service.exe").wstring());
        EXPECT_EQ(RegisteredServiceRuntime(oldBinary), oldRuntime);
        EXPECT_NE(oldRuntime.parent_path(), ServiceDirectoryForExecutable(current));
    }
}

TEST(ElevationRuntimePolicyTest, RefusesForeignBinariesArgumentsTraversalAndInvalidNonceLayouts) {
    const fs::path directory = LR"(C:\Program Files\Capture Engine\ElevationService)";
    const fs::path runtime = directory / kRuntime;
    const std::wstring executable = (runtime / L"captureengine_elevation_service.exe").wstring();
    const std::wstring binary = ce::elevation::QuoteArgument(executable);
    for (const std::wstring& invalid : {std::wstring(), executable, binary + L" --argument",
                                        L"\"" + executable + L"\"\"", L"\"relative\\" + executable + L"\""})
        EXPECT_TRUE(RegisteredServiceRuntime(invalid).empty());
    for (const fs::path& invalid :
         {runtime / L"other.exe", directory / L"captureengine_elevation_service.exe",
          directory / L"runtime-0123456789abcdef0123456789abcde" / L"captureengine_elevation_service.exe",
          directory / L"runtime-0123456789abcdef0123456789abcdeg" / L"captureengine_elevation_service.exe",
          directory / L"runtime-0123456789ABCDEF0123456789ABCDEF" / L"captureengine_elevation_service.exe",
          runtime / L".." / kRuntime / L"captureengine_elevation_service.exe",
          directory.parent_path() / L"OtherService" / kRuntime / L"captureengine_elevation_service.exe"})
        EXPECT_TRUE(RegisteredServiceRuntime(ce::elevation::QuoteArgument(invalid.wstring())).empty());
}

}  // namespace
