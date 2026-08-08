#include <gtest/gtest.h>

#include "../hook/common/ngx_module_policy.h"

namespace {

using ce::ngx::IsNgxCoreModulePath;
using ce::ngx::IsStreamlineNgxClientPath;
using ce::ngx::ModuleFileName;
using ce::ngx::ShouldInterceptNgxExportLookup;

TEST(NgxModulePolicy, ModuleFileNameStripsBothSeparators) {
    EXPECT_STREQ(ModuleFileName("C:\\Windows\\System32\\DriverStore\\nv_dispi\\nvngx.dll"), "nvngx.dll");
    EXPECT_STREQ(ModuleFileName("C:/games/gta/sl.dlss.dll"), "sl.dlss.dll");
    EXPECT_STREQ(ModuleFileName("nvngx.dll"), "nvngx.dll");
    EXPECT_STREQ(ModuleFileName(nullptr), "");
    EXPECT_STREQ(ModuleFileName(""), "");
}

TEST(NgxModulePolicy, RecognizesTheRealNgxProvider) {
    EXPECT_TRUE(IsNgxCoreModulePath("nvngx.dll"));
    EXPECT_TRUE(IsNgxCoreModulePath("NVNGX.DLL"));
    EXPECT_TRUE(IsNgxCoreModulePath("_nvngx.dll"));
    // The driver-store path is where sl.common.dll loads it from, via the
    // NGXCore FullPath registry value.
    EXPECT_TRUE(IsNgxCoreModulePath(
        "C:\\WINDOWS\\System32\\DriverStore\\FileRepository\\nv_dispi.inf_amd64_0373d825005116d0\\nvngx.dll"));
}

// The regression: sl.dlss.dll was accepted as "the NGX DLL", which latched the
// hook as installed and stopped every retry, even though it exports no
// NVSDK_NGX_* symbol.
TEST(NgxModulePolicy, StreamlinePluginsAreNotTheNgxProvider) {
    for (const char* path : {"sl.dlss.dll", "sl.dlss_g.dll", "sl.dlss_d.dll", "sl.common.dll", "sl.interposer.dll",
                             "C:\\games\\gta\\sl.dlss.dll"}) {
        EXPECT_FALSE(IsNgxCoreModulePath(path)) << path;
        EXPECT_TRUE(IsStreamlineNgxClientPath(path)) << path;
    }
}

// The feature DLLs carry the DLSS implementation but not the NGX API surface.
TEST(NgxModulePolicy, FeatureAndUnrelatedModulesAreRejected) {
    for (const char* path : {"nvngx_dlss.dll", "nvngx_dlssg.dll", "nvngx_dlssd.dll", "nvngx_update.exe", "d3d12.dll",
                             "nvngx", "nvngxfoo.dll", "mynvngx.dll", ""}) {
        EXPECT_FALSE(IsNgxCoreModulePath(path)) << path;
    }
    EXPECT_FALSE(IsNgxCoreModulePath(nullptr));
    EXPECT_FALSE(IsStreamlineNgxClientPath(nullptr));
    EXPECT_FALSE(IsStreamlineNgxClientPath("sl.reflex.dll"));
}

// Regression: the NGX export GetProcAddress hooks were registered unfiltered, so
// they also answered the core's *internal* dispatch lookups into the feature
// snippets. The detour then forwarded through the single per-symbol original -
// the core's own inline-hook trampoline - re-entering the core body until the
// stack overflowed (0xC00000FD in nvapi64_impl.dll during
// NVSDK_NGX_D3D12_GetFeatureRequirements). Only the core may be intercepted.
TEST(NgxModulePolicy, ExportLookupInterceptionIsLimitedToTheCoreProvider) {
    EXPECT_TRUE(ShouldInterceptNgxExportLookup("nvngx.dll"));
    EXPECT_TRUE(ShouldInterceptNgxExportLookup("_nvngx.dll"));
    EXPECT_TRUE(ShouldInterceptNgxExportLookup("C:\\games\\app\\_nvngx.dll"));

    // These export the same NVSDK_NGX_* names and are resolved by the core.
    EXPECT_FALSE(ShouldInterceptNgxExportLookup("nvngx_dlss.dll"));
    EXPECT_FALSE(ShouldInterceptNgxExportLookup("nvngx_dlssg.dll"));
    EXPECT_FALSE(ShouldInterceptNgxExportLookup("nvngx_dlssd.dll"));
    EXPECT_FALSE(ShouldInterceptNgxExportLookup("C:\\games\\app\\nvngx_dlssg.dll"));
    EXPECT_FALSE(ShouldInterceptNgxExportLookup("nvngx_deepdvc.dll"));

    EXPECT_FALSE(ShouldInterceptNgxExportLookup(nullptr));
    EXPECT_FALSE(ShouldInterceptNgxExportLookup(""));
}

TEST(NgxModulePolicy, UnderscoreStubIsNotConfusedWithTheBareName) {
    // Both are providers, but each must match on its own exact base name so a
    // near-miss such as "x_nvngx.dll" never qualifies.
    EXPECT_TRUE(IsNgxCoreModulePath("_nvngx.dll"));
    EXPECT_FALSE(IsNgxCoreModulePath("x_nvngx.dll"));
    EXPECT_FALSE(IsNgxCoreModulePath("nvngx.dll.bak"));
}

}  // namespace
