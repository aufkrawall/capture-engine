#include <gtest/gtest.h>

// clang-format off
#include <windows.h>
#include <ddraw.h>
// clang-format on

#include <filesystem>
#include <string>

#include "../hook/common/ddraw_chain_lifetime_policy.h"
#include "source_fragment_reader.h"

// CE must not keep an application's DirectDraw presentation chain alive past
// the point where the application builds a new one. Gothic II session
// 20260924_233030: after loading a save the game released its chain and asked
// for a new primary four times, DirectDraw answered
// DDERR_PRIMARYSURFACEALREADYEXISTS every time - CE's CPU-prerender queue still
// held the flipped primary, and its device tracker and native D3D7 sidecar held
// the device whose render target is the chain's back buffer - and the game
// closed with its own error box.
namespace lifetime = ce::ddraw_chain_lifetime;

namespace {

std::string ReadSource(const char* relativePath) {
    return ce::test_source::ReadFile(std::filesystem::current_path() / relativePath);
}

// The body of the first definition starting with `signature`, up to the
// closing brace at column zero.
std::string FunctionBody(const std::string& contents, const std::string& signature) {
    const size_t begin = contents.find(signature);
    if (begin == std::string::npos)
        return {};
    const size_t end = contents.find("\n}\n", begin);
    return contents.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

// The release has to happen before the creation is forwarded: after a failed
// creation there is no "after", which is exactly what the old post-success
// reset got wrong.
void ExpectReleaseBeforeForwarding(const std::string& body, const char* forwardingCall, const char* detour) {
    ASSERT_FALSE(body.empty()) << detour << " not found";
    const size_t release = body.find("ReleaseDirectDrawChainBeforePrimaryCreation(");
    const size_t forward = body.find(forwardingCall);
    ASSERT_NE(release, std::string::npos) << detour << " no longer releases CE's chain references";
    ASSERT_NE(forward, std::string::npos) << detour << " no longer forwards the creation";
    EXPECT_LT(release, forward) << detour << " forwards the primary creation while CE still holds the old chain";
    EXPECT_EQ(body.find("ResetDirectDrawPresentationStateForPrimaryChange"), std::string::npos)
        << detour << " resets only after the creation again";
    EXPECT_NE(body.find("LogApplicationPrimaryCreationFailure("), std::string::npos)
        << detour << " drops the diagnostic for a rejected primary creation";
}

// Same GUID as IID_IDirectDraw7; spelled out so the test needs no dxguid.
constexpr GUID kIidDirectDraw7 = {0x15e65ec0, 0x3b9c, 0x11d2, {0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b}};

HRESULT CreatePrimary(IDirectDraw7* directDraw, IDirectDrawSurface7** surface) {
    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS;
    desc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    return directDraw->CreateSurface(&desc, surface, nullptr);
}

}  // namespace

TEST(DDrawChainLifetimeTest, OnlyTheApplicationsOwnPrimaryCreationReleasesTheChain) {
    EXPECT_TRUE(lifetime::ShouldReleaseChainBeforeCreation(true, 0, false));
    EXPECT_FALSE(lifetime::ShouldReleaseChainBeforeCreation(false, 0, false)) << "textures and offscreen surfaces";
    EXPECT_FALSE(lifetime::ShouldReleaseChainBeforeCreation(true, 1, false)) << "CE's own vtable bootstrap";
    EXPECT_FALSE(lifetime::ShouldReleaseChainBeforeCreation(true, 0, true)) << "process teardown";
}

TEST(DDrawChainLifetimeTest, FailureCodesMatchTheSdkAndAreNamed) {
    EXPECT_EQ(lifetime::kPrimarySurfaceAlreadyExists, static_cast<uint32_t>(DDERR_PRIMARYSURFACEALREADYEXISTS));
    EXPECT_EQ(lifetime::kUnsupportedMode, static_cast<uint32_t>(DDERR_UNSUPPORTEDMODE));
    EXPECT_EQ(lifetime::kNoExclusiveMode, static_cast<uint32_t>(DDERR_NOEXCLUSIVEMODE));
    EXPECT_EQ(lifetime::kExclusiveModeAlreadySet, static_cast<uint32_t>(DDERR_EXCLUSIVEMODEALREADYSET));
    EXPECT_EQ(lifetime::kOutOfVideoMemory, static_cast<uint32_t>(DDERR_OUTOFVIDEOMEMORY));
    EXPECT_EQ(lifetime::kInvalidCaps, static_cast<uint32_t>(DDERR_INVALIDCAPS));
    EXPECT_EQ(lifetime::kInvalidParams, static_cast<uint32_t>(DDERR_INVALIDPARAMS));

    EXPECT_STREQ(lifetime::DescribePrimaryCreationFailure(0x88760234u), "DDERR_PRIMARYSURFACEALREADYEXISTS");
    EXPECT_STREQ(lifetime::DescribePrimaryCreationFailure(0x8876024Eu), "DDERR_UNSUPPORTEDMODE");
    EXPECT_STREQ(lifetime::DescribePrimaryCreationFailure(0x80004005u), "unrecognized");
}

// The runtime contract the invariant rests on, observed on the real
// DirectDraw: one extra reference to a released primary is enough to refuse
// the application's next one with the exact code the Gothic II session shows.
TEST(DDrawChainLifetimeTest, DirectDrawRefusesANewPrimaryWhileAnyReferenceToTheOldOneRemains) {
    HMODULE ddraw = LoadLibraryW(L"ddraw.dll");
    if (!ddraw)
        GTEST_SKIP() << "ddraw.dll unavailable";
    using DirectDrawCreateEx_t = HRESULT(WINAPI*)(GUID*, void**, REFIID, IUnknown*);
    auto create = reinterpret_cast<DirectDrawCreateEx_t>(
        reinterpret_cast<void*>(GetProcAddress(ddraw, "DirectDrawCreateEx")));
    ASSERT_NE(create, nullptr);

    IDirectDraw7* directDraw = nullptr;
    if (FAILED(create(nullptr, reinterpret_cast<void**>(&directDraw), kIidDirectDraw7, nullptr)) || !directDraw) {
        FreeLibrary(ddraw);
        GTEST_SKIP() << "no DirectDraw device in this session";
    }
    ASSERT_HRESULT_SUCCEEDED(directDraw->SetCooperativeLevel(nullptr, DDSCL_NORMAL));

    IDirectDrawSurface7* primary = nullptr;
    ASSERT_HRESULT_SUCCEEDED(CreatePrimary(directDraw, &primary));
    primary->AddRef();  // what an injected helper's queue does
    EXPECT_EQ(primary->Release(), 1u) << "the application's own release";

    IDirectDrawSurface7* replacement = nullptr;
    EXPECT_EQ(CreatePrimary(directDraw, &replacement), DDERR_PRIMARYSURFACEALREADYEXISTS);
    EXPECT_EQ(replacement, nullptr);

    EXPECT_EQ(primary->Release(), 0u) << "the helper lets go";
    EXPECT_HRESULT_SUCCEEDED(CreatePrimary(directDraw, &replacement));
    if (replacement)
        replacement->Release();
    directDraw->Release();
    FreeLibrary(ddraw);
}

TEST(DDrawChainLifetimeTest, EveryCreateSurfaceGenerationReleasesTheChainBeforeForwarding) {
    const std::string detours = ReadSource("hook/apis/ddraw_hook_detours.cpp");
    const std::string detours4 = ReadSource("hook/apis/ddraw_hook_detours_surface4.cpp");
    ExpectReleaseBeforeForwarding(
        FunctionBody(detours, "HRESULT STDMETHODCALLTYPE DetourDirectDrawLegacyCreateSurface("),
        "record.createSurface(pThis", "DetourDirectDrawLegacyCreateSurface");
    ExpectReleaseBeforeForwarding(FunctionBody(detours, "HRESULT STDMETHODCALLTYPE DetourDirectDraw7CreateSurface("),
                                  "original(pThis", "DetourDirectDraw7CreateSurface");
    ExpectReleaseBeforeForwarding(
        FunctionBody(detours4, "HRESULT STDMETHODCALLTYPE DetourDirectDraw4CreateSurface("), "original(pThis",
        "DetourDirectDraw4CreateSurface");
}

TEST(DDrawChainLifetimeTest, TheChainReleaseCoversEveryReferenceCeTakes) {
    const std::string route = ReadSource("hook/apis/ddraw_hook_overlay_route.cpp");
    const std::string reset =
        FunctionBody(route, "DirectDrawChainReferenceRelease ResetDirectDrawPresentationStateForPrimaryChange()");
    ASSERT_FALSE(reset.empty());
    EXPECT_NE(reset.find("ReleaseNativeLegacyD3DOverlay()"), std::string::npos) << "the sidecar's device reference";
    EXPECT_NE(reset.find("ResetDirectDrawPresentationOverrides()"), std::string::npos)
        << "the prerender queue's surface references";

    const size_t sidecar = reset.find("ReleaseNativeLegacyD3DOverlay()");
    const size_t queue = reset.find("ResetDirectDrawPresentationOverrides()");
    EXPECT_LT(sidecar, queue) << "a device reference must go before the surfaces its render target lives in";

    const std::string release = FunctionBody(route, "void ReleaseDirectDrawChainBeforePrimaryCreation(");
    ASSERT_FALSE(release.empty());
    EXPECT_NE(release.find("ResetDirectDrawPresentationStateForPrimaryChange()"), std::string::npos);
    // Gothic II 20260924_235830: releasing the tracked device here made CE's
    // reference the device's last one after the chain was gone, and Direct3D
    // faulted destroying it. Only the device Release interception drops it.
    EXPECT_EQ(release.find("ReleaseTrackedLegacyD3D7Device"), std::string::npos)
        << "the pre-creation release must not destroy the application's device";
    EXPECT_NE(release.find("ddraw_hook_g_PrimarySurface = nullptr"), std::string::npos);
    EXPECT_NE(release.find("ddraw_hook_g_PrimarySurface4 = nullptr"), std::string::npos);
    EXPECT_NE(release.find("HookLogImportant("), std::string::npos) << "what CE released must be provable";

    const std::string overrides = ReadSource("hook/apis/ddraw_hook_present_overrides.cpp");
    const std::string overrideReset = FunctionBody(overrides, "uint32_t ResetDirectDrawPresentationOverrides()");
    ASSERT_FALSE(overrideReset.empty());
    EXPECT_NE(overrideReset.find("ClearPendingLocked(state)"), std::string::npos);
    EXPECT_NE(overrideReset.find("directDrawOwner->Release()"), std::string::npos);
}

// Dropping the tracked device at a chain boundary must not strand the native
// sidecar in a title that stops calling SetTextureStageState: every rendered
// frame ends in EndScene, which tracks the device again.
TEST(DDrawChainLifetimeTest, EndSceneTracksTheDeviceAgain) {
    const std::string legacy = ReadSource("hook/apis/ddraw_hook_detours_legacy_d3d.cpp");
    const std::string endScene = FunctionBody(legacy, "HRESULT STDMETHODCALLTYPE DetourD3D7EndScene(");
    ASSERT_FALSE(endScene.empty());
    const size_t internalGuard = endScene.find("LegacyD3DInternalCallActive()");
    const size_t track = endScene.find("TrackLegacyD3D7Device(");
    const size_t draw = endScene.find("DrawNativeLegacyD3DOverlayAtEndScene(");
    ASSERT_NE(track, std::string::npos);
    ASSERT_NE(internalGuard, std::string::npos);
    ASSERT_NE(draw, std::string::npos);
    EXPECT_LT(internalGuard, track) << "CE's own EndScene calls must not track anything";
    EXPECT_LT(track, draw);

    const std::string helpers = ReadSource("hook/apis/ddraw_hook_helpers.cpp");
    const std::string untrack = FunctionBody(helpers, "bool ReleaseTrackedLegacyD3D7DeviceIf(");
    ASSERT_FALSE(untrack.empty());
    const size_t exchange = untrack.find("exchange(nullptr");
    const size_t unlock = untrack.find("}", exchange);
    const size_t releaseCall = untrack.find("tracked->Release()");
    ASSERT_NE(exchange, std::string::npos);
    ASSERT_NE(releaseCall, std::string::npos);
    EXPECT_LT(unlock, releaseCall) << "the last device reference must be dropped outside the identity lock";
}

TEST(DDrawChainLifetimeTest, OnlyAReleaseThatLeavesCeAloneEndsCesReferences) {
    EXPECT_TRUE(lifetime::ApplicationReleasedLastDeviceReference(2, 2)) << "tracker + sidecar remain, nothing else";
    EXPECT_TRUE(lifetime::ApplicationReleasedLastDeviceReference(1, 1));
    EXPECT_FALSE(lifetime::ApplicationReleasedLastDeviceReference(2, 3)) << "the application still owns one";
    EXPECT_FALSE(lifetime::ApplicationReleasedLastDeviceReference(0, 0))
        << "CE held nothing - the device is already gone and must not be touched";
    EXPECT_FALSE(lifetime::ApplicationReleasedLastDeviceReference(2, 1)) << "never below CE's own count";
}

// CE may hold the application's device only while the Release interception
// can hand the reference back inside the application's own last Release.
TEST(DDrawChainLifetimeTest, CesDeviceReferencesEndInsideTheApplicationsLastRelease) {
    const std::string lifetimeSource = ReadSource("hook/apis/ddraw_hook_device_lifetime.cpp");
    const std::string detour = FunctionBody(lifetimeSource, "ULONG STDMETHODCALLTYPE DetourD3D7DeviceRelease(");
    ASSERT_FALSE(detour.empty());
    const size_t count = detour.find("CountCeDeviceReferences(ddraw_hook_device)");
    const size_t forward = detour.find("const ULONG remaining = release(ddraw_hook_device);");
    const size_t sidecar = detour.find("ReleaseNativeLegacyD3DOverlayForDevice(");
    const size_t tracked = detour.find("ReleaseTrackedLegacyD3D7DeviceIf(");
    const size_t textures = detour.find("ReleaseLegacyD3D7TextureBindingsForDevice(");
    ASSERT_NE(count, std::string::npos);
    ASSERT_NE(forward, std::string::npos);
    ASSERT_NE(sidecar, std::string::npos);
    ASSERT_NE(tracked, std::string::npos);
    ASSERT_NE(textures, std::string::npos);
    EXPECT_LT(count, forward) << "CE's references must be counted before the device can be gone";
    EXPECT_LT(forward, sidecar);
    EXPECT_LT(sidecar, tracked) << "the sidecar deletes its state block on the still-live device";
    EXPECT_LT(tracked, textures);
    EXPECT_NE(detour.find("LegacyD3DInternalCallActive()"), std::string::npos) << "CE's own releases pass through";

    const std::string helpers = ReadSource("hook/apis/ddraw_hook_helpers.cpp");
    const std::string track = FunctionBody(helpers, "void TrackLegacyD3D7Device(");
    const size_t gate = track.find("LegacyD3D7DeviceReleaseIsIntercepted(device)");
    const size_t addRef = track.find("device->AddRef()");
    ASSERT_NE(gate, std::string::npos) << "a device CE cannot release in time must not be referenced";
    ASSERT_NE(addRef, std::string::npos);
    EXPECT_LT(gate, addRef);

    const std::string route = ReadSource("hook/apis/ddraw_hook_overlay_route.cpp");
    const std::string prime = FunctionBody(route, "bool PrimeNativeLegacyD3DOverlay(");
    EXPECT_NE(prime.find("LegacyD3D7DeviceReleaseIsIntercepted(device)"), std::string::npos);

    const std::string install = ReadSource("hook/apis/ddraw_hook_install.cpp");
    EXPECT_NE(install.find("InstallD3D7DeviceReleaseHook(record, vtable)"), std::string::npos);
}
