#include <gtest/gtest.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <wrl/client.h>
#include "../common/screen_grab_privacy.h"

namespace privacy = ce::screen_grab_privacy;
using Microsoft::WRL::ComPtr;

TEST(ScreenGrabPrivacyTest, FullscreenGeometryAcceptsWindowOrClientWithinTolerance) {
    const RECT monitor = {100, 50, 2020, 1130};
    RECT exactWindow = monitor;
    RECT tolerantClient = {92, 58, 2028, 1122};
    RECT tooSmall = {109, 50, 2020, 1130};

    EXPECT_TRUE(privacy::IsFullscreenGeometry(&exactWindow, nullptr, monitor));
    EXPECT_TRUE(privacy::IsFullscreenGeometry(nullptr, &tolerantClient, monitor));
    EXPECT_FALSE(privacy::IsFullscreenGeometry(&tooSmall, nullptr, monitor));
    EXPECT_FALSE(privacy::IsFullscreenGeometry(nullptr, nullptr, monitor));
}

TEST(ScreenGrabPrivacyTest, SnapshotMatchesOnlyTheSelectedWindowOrMonitor) {
    const HWND foreground = reinterpret_cast<HWND>(static_cast<uintptr_t>(1));
    const HMONITOR monitor = reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(2));
    privacy::FullscreenFocusSnapshot snapshot{foreground, monitor, true, true, true};

    EXPECT_TRUE(privacy::SnapshotMatchesCaptureTarget(snapshot, foreground, nullptr));
    EXPECT_TRUE(privacy::SnapshotMatchesCaptureTarget(snapshot, nullptr, monitor));
    EXPECT_FALSE(privacy::SnapshotMatchesCaptureTarget(
        snapshot, reinterpret_cast<HWND>(static_cast<uintptr_t>(3)), nullptr));
    EXPECT_FALSE(privacy::SnapshotMatchesCaptureTarget(
        snapshot, nullptr, reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(4))));

    snapshot.stable = false;
    EXPECT_FALSE(privacy::SnapshotMatchesCaptureTarget(snapshot, foreground, nullptr));
}

TEST(ScreenGrabPrivacyTest, RootWindowNormalizationMatchesChildToCaptureTarget) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const HWND root = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED, 0, 0, 1, 1, nullptr, nullptr, instance,
                                      nullptr);
    ASSERT_NE(root, nullptr);
    const HWND child =
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD, 0, 0, 1, 1, root, nullptr, instance, nullptr);
    ASSERT_NE(child, nullptr);

    EXPECT_EQ(privacy::NormalizeRootWindow(child), root);
    EXPECT_EQ(privacy::NormalizeRootWindow(root), root);
    const HMONITOR monitor = MonitorFromWindow(root, MONITOR_DEFAULTTONEAREST);
    ASSERT_NE(monitor, nullptr);
    EXPECT_TRUE(privacy::IsCaptureTargetValid(root, nullptr));
    EXPECT_TRUE(privacy::IsCaptureTargetValid(nullptr, monitor));
    EXPECT_FALSE(privacy::IsCaptureTargetValid(root, monitor));

    DestroyWindow(child);
    EXPECT_FALSE(privacy::IsCaptureTargetValid(child, nullptr));
    DestroyWindow(root);
}

TEST(ScreenGrabPrivacyTest, GateFailsClosedAndWaitsForPostFocusFrame) {
    privacy::FocusPrivacyGate gate;
    gate.Reset(true);

    auto decision = gate.Evaluate(true, true, false, 100, true, 100);
    EXPECT_TRUE(decision.useBlackFrame);
    EXPECT_TRUE(decision.waitingForSafeFrame);

    decision = gate.Evaluate(true, true, true, 200, true, 199);
    EXPECT_TRUE(decision.focusReacquired);
    EXPECT_TRUE(decision.useBlackFrame);
    EXPECT_EQ(gate.SafeFrameThresholdQpc(), 200);

    decision = gate.Evaluate(true, true, true, 210, false, 0);
    EXPECT_TRUE(decision.useBlackFrame);

    decision = gate.Evaluate(true, true, true, 220, true, 200);
    EXPECT_TRUE(decision.exitedBlackout);
    EXPECT_FALSE(decision.useBlackFrame);
    EXPECT_EQ(gate.ResumeWaitMaxQpc(), 20u);

    gate.CommitOutput(false);
    decision = gate.Evaluate(true, true, true, 225, false, 0);
    EXPECT_FALSE(decision.useBlackFrame);
    decision = gate.Evaluate(true, true, false, 230, false, 0);
    EXPECT_TRUE(decision.enteredBlackout);
    EXPECT_TRUE(decision.useBlackFrame);
    gate.CommitOutput(true);
    EXPECT_TRUE(gate.LastOutputWasBlack());
    EXPECT_EQ(gate.BlackFrames(), 1u);
}

TEST(ScreenGrabPrivacyTest, AmbiguousObservationAndTargetResetRemainFailClosed) {
    privacy::FocusPrivacyGate gate;
    gate.Reset(true);

    auto decision = gate.Evaluate(true, false, false, 100, true, 100);
    EXPECT_TRUE(decision.useBlackFrame);
    EXPECT_EQ(gate.AmbiguousObservations(), 1u);

    decision = gate.Evaluate(true, true, true, 200, true, 200);
    ASSERT_FALSE(decision.useBlackFrame);
    gate.CommitOutput(false);

    gate.ResetTarget();
    decision = gate.Evaluate(true, true, true, 300, true, 299);
    EXPECT_TRUE(decision.useBlackFrame);
    EXPECT_TRUE(decision.waitingForSafeFrame);
    decision = gate.Evaluate(true, true, true, 301, true, 300);
    EXPECT_FALSE(decision.useBlackFrame);
}

// Regression: warmup builds the WGC look-ahead reservoir that the live handoff keeps.
// Before warmup observation existed, the first live evaluation opened the verified-focus
// interval at "now", which is newer than every reservoir frame, so a recording that was
// started with the capture target already focused began with a reservoir-long blackout.
TEST(ScreenGrabPrivacyTest, WarmupObservationRevealsTheReservoirItVerified) {
    privacy::FocusPrivacyGate gate;
    gate.Reset(true);

    auto observed = gate.Observe(true, true, true, 100);
    EXPECT_TRUE(observed.focusReacquired);
    EXPECT_TRUE(observed.enteredBlackout);
    EXPECT_FALSE(observed.useBlackFrame);
    EXPECT_TRUE(observed.waitingForSafeFrame);
    EXPECT_EQ(gate.SafeFrameThresholdQpc(), 100);

    // Repeated warmup ticks with the same verified focus must not re-arm the interval.
    observed = gate.Observe(true, true, true, 300);
    EXPECT_FALSE(observed.focusReacquired);
    EXPECT_FALSE(observed.enteredBlackout);
    EXPECT_EQ(gate.SafeFrameThresholdQpc(), 100);

    // The first live frame is reservoir content captured long before the handoff but
    // after the verified interval opened, so it is revealed without a black frame.
    const auto live = gate.Evaluate(true, true, true, 1000, true, 670);
    EXPECT_TRUE(live.exitedBlackout);
    EXPECT_FALSE(live.useBlackFrame);
    EXPECT_EQ(gate.BlackFrames(), 0u);
}

TEST(ScreenGrabPrivacyTest, WarmupFocusLossStillBlanksTheReservoirCapturedWhileUnfocused) {
    privacy::FocusPrivacyGate gate;
    gate.Reset(true);

    EXPECT_TRUE(gate.Observe(true, true, true, 100).focusReacquired);
    // Focus left the capture target mid-warmup: everything buffered so far is unsafe.
    auto observed = gate.Observe(true, true, false, 200);
    EXPECT_TRUE(observed.waitingForSafeFrame);
    EXPECT_EQ(gate.SafeFrameThresholdQpc(), 0);
    // Reacquired later in warmup: the interval restarts at the reacquisition.
    observed = gate.Observe(true, true, true, 400);
    EXPECT_TRUE(observed.focusReacquired);
    EXPECT_EQ(gate.SafeFrameThresholdQpc(), 400);

    auto live = gate.Evaluate(true, true, true, 500, true, 150);
    EXPECT_TRUE(live.useBlackFrame);
    gate.CommitOutput(true);
    live = gate.Evaluate(true, true, true, 520, true, 410);
    EXPECT_TRUE(live.exitedBlackout);
    EXPECT_FALSE(live.useBlackFrame);
    EXPECT_EQ(gate.BlackFrames(), 1u);
}

TEST(ScreenGrabPrivacyTest, WarmupObservationNeverRevealsWithoutAVerifiedFocus) {
    privacy::FocusPrivacyGate gate;
    gate.Reset(true);

    const auto ambiguous = gate.Observe(true, false, false, 100);
    EXPECT_FALSE(ambiguous.useBlackFrame);
    EXPECT_TRUE(ambiguous.waitingForSafeFrame);
    EXPECT_EQ(gate.AmbiguousObservations(), 1u);
    EXPECT_EQ(gate.SafeFrameThresholdQpc(), 0);

    // A frame arriving while focus is still unverified stays black.
    EXPECT_TRUE(gate.Evaluate(true, false, false, 200, true, 150).useBlackFrame);

    // Disabled gate and inject paths never observe.
    privacy::FocusPrivacyGate disabled;
    disabled.Reset(false);
    EXPECT_FALSE(disabled.Observe(true, true, true, 100).waitingForSafeFrame);
    privacy::FocusPrivacyGate injectPath;
    injectPath.Reset(true);
    EXPECT_FALSE(injectPath.Observe(false, true, true, 100).waitingForSafeFrame);
    EXPECT_EQ(injectPath.SafeFrameThresholdQpc(), 0);
}

TEST(ScreenGrabPrivacyTest, DisabledAndInjectPathsRemainVisible) {
    privacy::FocusPrivacyGate gate;
    gate.Reset(false);
    EXPECT_FALSE(gate.Evaluate(true, false, false, 100, false, 0).useBlackFrame);

    gate.Reset(true);
    EXPECT_FALSE(gate.Evaluate(false, false, false, 100, false, 0).useBlackFrame);
}

namespace {

bool CreateTestDevice(ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& context) {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selected = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 1, D3D11_SDK_VERSION,
                                   &device, &selected, &context);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 1, D3D11_SDK_VERSION, &device,
                               &selected, &context);
    }
    return SUCCEEDED(hr);
}

std::array<uint8_t, 8> ReadFirstPixel(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    EXPECT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)));
    context->CopyResource(staging.Get(), texture);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    EXPECT_TRUE(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
    std::array<uint8_t, 8> pixel = {};
    std::memcpy(pixel.data(), mapped.pData, pixel.size());
    context->Unmap(staging.Get(), 0);
    return pixel;
}

}  // namespace

TEST(ScreenGrabPrivacyTest, BlackTextureCacheProducesOpaqueBlackForSupportedFormats) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ASSERT_TRUE(CreateTestDevice(device, context));

    privacy::BlackFrameTextureCache cache;
    uint64_t previousGeneration = 0;
    for (const DXGI_FORMAT format :
         {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT}) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = 4;
        desc.Height = 4;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> reference;
        ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &reference))) << format;

        ASSERT_TRUE(cache.Prepare(reference.Get())) << format;
        EXPECT_GT(cache.Generation(), previousGeneration);
        previousGeneration = cache.Generation();
        ASSERT_TRUE(cache.Prepare(reference.Get())) << format;
        EXPECT_EQ(cache.Generation(), previousGeneration);
        const auto pixel = ReadFirstPixel(device.Get(), context.Get(), cache.Get());
        if (format == DXGI_FORMAT_B8G8R8A8_UNORM) {
            EXPECT_EQ(pixel[0], 0u);
            EXPECT_EQ(pixel[1], 0u);
            EXPECT_EQ(pixel[2], 0u);
            EXPECT_EQ(pixel[3], 0xffu);
        } else if (format == DXGI_FORMAT_R10G10B10A2_UNORM) {
            uint32_t packed = 0;
            std::memcpy(&packed, pixel.data(), sizeof(packed));
            EXPECT_EQ(packed, 0xc0000000u);
        } else {
            uint16_t channels[4] = {};
            std::memcpy(channels, pixel.data(), sizeof(channels));
            EXPECT_EQ(channels[0], 0u);
            EXPECT_EQ(channels[1], 0u);
            EXPECT_EQ(channels[2], 0u);
            EXPECT_EQ(channels[3], 0x3c00u);
        }
    }
    cache.Reset();
    EXPECT_EQ(cache.Get(), nullptr);
}
