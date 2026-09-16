#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "source_fragment_reader.h"

// CE's DirectDraw bootstrap creates a throwaway IDirectDraw7, a primary
// surface and a D3D7 device purely to reach the vtables it patches. Those
// patches belong to ddraw.dll and outlive the objects, so nothing about them
// requires CE to keep a DirectDraw object alive in the application's process.
//
// The primary surface used not to be released at all. A second live
// DDSCAPS_PRIMARYSURFACE - on a window the bootstrap then destroys - is exactly
// the kind of footprint that can keep an application from re-establishing its
// own display mode, and Gothic II session 20260916_014133 ends with four
// consecutive DDERR_UNSUPPORTEDMODE primary creations after an alt-tab.
//
// The prototype sentinels are the other half: they are compared by pointer
// against the application's surfaces, and DirectDraw is free to hand a released
// surface's address back out. A sentinel that names a freed object can exclude
// the application's own surface from presentation tracking.
namespace {

std::string ReadInstaller() {
    const std::filesystem::path source =
        std::filesystem::current_path() / "hook/apis" / "ddraw_hook_install.cpp";
    return ce::test_source::ReadLogicalSource(source);
}

}  // namespace

TEST(DDrawBootstrapFootprintTest, TheBootstrapReleasesItsOwnPrimarySurface) {
    const std::string contents = ReadInstaller();
    ASSERT_FALSE(contents.empty());

    const size_t create = contents.find("ddraw7->CreateSurface(&ddsd, &dummySurface, NULL)");
    ASSERT_NE(create, std::string::npos) << "the bootstrap no longer creates its own primary surface";
    const size_t release = contents.find("dummySurface->Release()", create);
    const size_t ddrawRelease = contents.find("ddraw7->Release()", create);
    EXPECT_NE(release, std::string::npos) << "the bootstrap primary surface is leaked again";
    ASSERT_NE(ddrawRelease, std::string::npos);
    EXPECT_LT(release, ddrawRelease) << "the primary surface must go before the object that owns it";
}

TEST(DDrawBootstrapFootprintTest, TheBootstrapClearsBothPrototypeSentinels) {
    const std::string contents = ReadInstaller();
    ASSERT_FALSE(contents.empty());

    const size_t create = contents.find("ddraw7->CreateSurface(&ddsd, &dummySurface, NULL)");
    ASSERT_NE(create, std::string::npos);
    EXPECT_NE(contents.find("ddraw_hook_g_HookSurfacePrototype = nullptr", create), std::string::npos);
    EXPECT_NE(contents.find("ddraw_hook_g_HookSurfacePrototype4 = nullptr", create), std::string::npos);
}
