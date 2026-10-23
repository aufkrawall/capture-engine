// Present provenance and frame-generation runtime module identity (hook/present/present_originator_policy.h,
// hook/overlay/overlay_compat_detail/module_table.h).
//
// Talos + OptiScaler with FGOutput=XeFG (session 20261023_183119): OptiScaler keeps libxess_fg.dll in its own
// "OptiScaler" folder, and that folder name matched CE's "optiscaler" overlay token. Both originator walks then
// skipped XeFG's frames as an overlay's: the swapchain XeFG created looked like Streamline's, the XeFG present
// on the game thread looked Streamline-originated (CE ran PostSL on it) and the one from XeFG's presenter thread
// looked like the application's own (ProcessFrame skipped it): the overlay was on 72 of 144 displayed frames.

#include <gtest/gtest.h>

#include <string>

#include "hook/overlay/overlay_compat.h"
#include "hook/present/present_originator_policy.h"

namespace {

using namespace ce::present_originator;
using namespace ce::overlay_compat;

constexpr const char* kSystemDirectory = "C:\\WINDOWS\\system32";
constexpr const char* kOptiScalerFolder = "H:\\Games\\Talos1\\Binaries\\Win64\\OptiScaler\\";

class PresentOriginatorPolicyTest : public ::testing::Test {
protected:
    void SetUp() override { ResetThirdPartyOverlayModuleCacheForTesting(); }
    void TearDown() override { ResetThirdPartyOverlayModuleCacheForTesting(); }
};

TEST_F(PresentOriginatorPolicyTest, XeFGModulesAreIdentifiedByBaseNameWhereverTheyLive) {
    EXPECT_TRUE(IsXeFGFrameGenerationModulePath("H:\\Games\\Talos1\\Binaries\\Win64\\OptiScaler\\libxess_fg.dll"));
    EXPECT_TRUE(IsXeFGFrameGenerationModulePath("D:\\Witcher 3\\bin\\x64_dx12\\LIBXESS_FG.DLL"));
    EXPECT_TRUE(IsXeFGFrameGenerationModulePath(L"D:\\Witcher 3\\bin\\x64_dx12\\libxess_fg.dll"));
    EXPECT_TRUE(IsXeFGFrameGenerationModulePath("libxess_fg.dll"));
    EXPECT_FALSE(IsXeFGFrameGenerationModulePath("D:\\game\\libxess.dll"));
    EXPECT_FALSE(IsXeFGFrameGenerationModulePath("D:\\game\\libxess_fg_loader.dll"));
    EXPECT_FALSE(IsXeFGFrameGenerationModulePath("D:\\libxess_fg.dll\\game.dll"));
    EXPECT_FALSE(IsXeFGFrameGenerationModulePath(static_cast<const char*>(nullptr)));

    EXPECT_TRUE(IsXeLLModulePath("D:\\game\\libxell.dll"));
    EXPECT_TRUE(IsXeLLModulePath(L"D:\\game\\LibXeLL.dll"));
    EXPECT_FALSE(IsXeLLModulePath("D:\\game\\libxess_fg.dll"));
    EXPECT_TRUE(IsXeSSUpscalerModulePath("D:\\game\\libxess.dll"));
    EXPECT_TRUE(IsXeSSUpscalerModulePath("D:\\game\\libxess_dx11.dll"));
    EXPECT_FALSE(IsXeSSUpscalerModulePath("D:\\game\\libxess_fg.dll"));
}

// The fix: the runtime's module identity wins over the folder token; OptiScaler's own module stays an overlay.
TEST_F(PresentOriginatorPolicyTest, ARuntimeModuleInOptiScalersFolderIsNotAnOverlay) {
    const std::string folder = kOptiScalerFolder;
    for (const char* runtime : {"libxess_fg.dll", "libxell.dll", "libxess.dll", "amd_fidelityfx_framegeneration_dx12.dll",
                                "amd_fidelityfx_dx12.dll"}) {
        const std::string path = folder + runtime;
        EXPECT_TRUE(IsFrameGenerationRuntimeModulePath(path.c_str())) << path;
        EXPECT_FALSE(IsThirdPartyOverlayModulePath(path.c_str())) << path;
        const std::wstring wide(path.begin(), path.end());
        EXPECT_FALSE(IsThirdPartyOverlayModulePath(wide.c_str())) << path;
    }
    EXPECT_TRUE(IsThirdPartyOverlayModulePath((folder + "OptiScaler.dll").c_str()));
    EXPECT_TRUE(IsThirdPartyOverlayModulePath((folder + "fakenvapi.dll").c_str()));
    EXPECT_TRUE(IsThirdPartyOverlayModulePath("C:\\Program Files (x86)\\Steam\\gameoverlayrenderer64.dll"));
    EXPECT_FALSE(IsFrameGenerationRuntimeModulePath("C:\\Program Files (x86)\\Steam\\gameoverlayrenderer64.dll"));
}

TEST_F(PresentOriginatorPolicyTest, OnlyTheSystemsOwnCopiesAreDispatchModules) {
    EXPECT_TRUE(IsGraphicsDispatchModulePath("C:\\WINDOWS\\SYSTEM32\\dxgi.dll", kSystemDirectory));
    EXPECT_TRUE(IsGraphicsDispatchModulePath("c:\\windows\\system32\\D3D12.dll", kSystemDirectory));
    EXPECT_TRUE(IsGraphicsDispatchModulePath("C:/Windows/System32/d3d11.dll", "C:\\Windows\\System32\\"));
    // A game-folder proxy under a system name is code of its own (OptiScaler, a translation layer).
    EXPECT_FALSE(IsGraphicsDispatchModulePath("H:\\Games\\Talos1\\Binaries\\Win64\\dxgi.dll", kSystemDirectory));
    EXPECT_FALSE(IsGraphicsDispatchModulePath("C:\\WINDOWS\\system32\\sub\\dxgi.dll", kSystemDirectory));
    EXPECT_FALSE(IsGraphicsDispatchModulePath("dxgi.dll", kSystemDirectory));
    EXPECT_FALSE(IsGraphicsDispatchModulePath("C:\\WINDOWS\\SYSTEM32\\dxgi.dll", ""));
    EXPECT_FALSE(IsGraphicsDispatchModulePath("C:\\WINDOWS\\SYSTEM32\\kernel32.dll", kSystemDirectory));
    // The Agility SDK's runtime is Microsoft's own wherever it loads from.
    EXPECT_TRUE(IsGraphicsDispatchModulePath("H:\\Games\\Talos1\\Binaries\\Win64\\D3D12\\D3D12Core.dll",
                                             kSystemDirectory));
    EXPECT_TRUE(IsGraphicsDispatchModulePath("H:\\Games\\D3D12\\d3d12SDKLayers.dll", kSystemDirectory));
}

TEST_F(PresentOriginatorPolicyTest, OriginatorKindsAreResolvedFromTheModule) {
    EXPECT_EQ(ClassifyOriginatorModule("H:\\x\\OptiScaler\\libxess_fg.dll", false), Originator::kXeFG);
    // Exact identity first: even a module CE tracks as Streamline's cannot turn XeFG into Streamline.
    EXPECT_EQ(ClassifyOriginatorModule("H:\\x\\libxess_fg.dll", true), Originator::kXeFG);
    EXPECT_EQ(ClassifyOriginatorModule("C:\\npi\\sl\\sl.interposer.dll", false), Originator::kStreamline);
    EXPECT_EQ(ClassifyOriginatorModule("C:\\game\\renamed_runtime.dll", true), Originator::kStreamline);
    EXPECT_EQ(ClassifyOriginatorModule("C:\\game\\amd_fidelityfx_framegeneration_dx12.dll", false),
              Originator::kFFXFrameGeneration);
    EXPECT_EQ(ClassifyOriginatorModule("C:\\WINDOWS\\System32\\NvPresent64.dll", false),
              Originator::kPresentInterposer);
    EXPECT_EQ(ClassifyOriginatorModule("C:\\game\\Talos1-Win64-Shipping.exe", false), Originator::kApplication);
    EXPECT_EQ(ClassifyOriginatorModule("", false), Originator::kUnresolved);
    EXPECT_STREQ(OriginatorName(Originator::kXeFG), "xefg");
    EXPECT_STREQ(OriginatorName(Originator::kUnresolved), "unresolved");
}

// The game-thread present of session 20261023_183119, frame by frame: CE, the system dxgi.dll, Steam's
// overlay, XeFG's proxy Present, OptiScaler's detour of it, Streamline's proxy, the game. The originator is
// XeFG, not Streamline.
TEST_F(PresentOriginatorPolicyTest, TheOptiScalerXeFGGameThreadPresentOriginatesInXeFG) {
    const std::string folder = kOptiScalerFolder;
    const std::string xefg = folder + "libxess_fg.dll";
    const std::string optiscaler = folder + "OptiScaler.dll";
    struct Frame {
        const char* path;
        bool captureEngine;
    } frames[] = {
        {"C:\\Program Files\\Capture Engine\\capture_hook_x64.dll", true},
        {"", false},
        {"C:\\WINDOWS\\SYSTEM32\\dxgi.dll", false},
        {"C:\\Program Files (x86)\\Steam\\gameoverlayrenderer64.dll", false},
        {xefg.c_str(), false},
        {optiscaler.c_str(), false},
        {"C:\\npi\\sl\\sl.interposer.dll", false},
        {"H:\\Games\\Talos1\\Binaries\\Win64\\Talos1-Win64-Shipping.exe", false},
    };
    FrameKind kinds[8] = {};
    for (int i = 0; i < 8; ++i) {
        kinds[i] = ClassifyFrame(frames[i].path, frames[i].captureEngine, kSystemDirectory);
    }
    EXPECT_EQ(kinds[0], FrameKind::kCaptureEngine);
    EXPECT_EQ(kinds[1], FrameKind::kNoImage);
    EXPECT_EQ(kinds[2], FrameKind::kGraphicsDispatch);
    EXPECT_EQ(kinds[3], FrameKind::kThirdPartyOverlay);
    EXPECT_EQ(kinds[5], FrameKind::kThirdPartyOverlay);
    const int originator = SelectOriginatorFrame(kinds, 8);
    ASSERT_EQ(originator, 4);
    EXPECT_EQ(ClassifyOriginatorModule(frames[originator].path, false), Originator::kXeFG);

    // The same stack before the fix skipped XeFG as an overlay and landed on Streamline.
    kinds[4] = FrameKind::kThirdPartyOverlay;
    EXPECT_EQ(SelectOriginatorFrame(kinds, 8), 6);
}

// A game-folder dxgi.dll proxy that no identity scan recognized presents as itself.
TEST_F(PresentOriginatorPolicyTest, AnUnidentifiedSameNamedProxyIsItsOwnOriginator) {
    const FrameKind kinds[] = {
        ClassifyFrame("C:\\WINDOWS\\SYSTEM32\\dxgi.dll", false, kSystemDirectory),
        ClassifyFrame("H:\\Games\\Game\\dxgi.dll", false, kSystemDirectory),
        ClassifyFrame("C:\\npi\\sl\\sl.interposer.dll", false, kSystemDirectory),
    };
    EXPECT_EQ(SelectOriginatorFrame(kinds, 3), 1);
    EXPECT_EQ(ClassifyOriginatorModule("H:\\Games\\Game\\dxgi.dll", false), Originator::kApplication);
    EXPECT_EQ(SelectOriginatorFrame(nullptr, 3), -1);
    const FrameKind none[] = {FrameKind::kCaptureEngine, FrameKind::kGraphicsDispatch};
    EXPECT_EQ(SelectOriginatorFrame(none, 2), -1);
}

TEST_F(PresentOriginatorPolicyTest, XeFGIsTrackedForLoaderFreePresenceButIsNoOverlay) {
    const int index = MatchKnownThirdPartyOverlayModuleIndex("LIBXESS_FG.dll");
    ASSERT_GE(index, 0);
    EXPECT_FALSE(kTrackedOverlayModules[index].overlay);
    EXPECT_FALSE(kTrackedOverlayModules[index].startupBlocking);
    EXPECT_FALSE(kTrackedOverlayModules[index].startupBlockingRender);
}

}  // namespace
