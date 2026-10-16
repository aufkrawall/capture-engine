#include <gtest/gtest.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <mmsystem.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "common/platform/raii_helpers.h"
#include "mediaengine/video/encoder_device_bridge.h"
#include "source_fragment_reader.h"

// NVENC on its own D3D11 device (encoder_device_bridge.h): CE's work device
// writes pool textures through shared aliases and gates each submit on its own
// fence, so no command on the work device ever follows an NVENC submit.

namespace {

using ce::media::EncoderDeviceBridge;
using ce::media::NextSubmitGateAction;
using ce::media::SubmitGateAction;

TEST(EncoderDeviceBridgeTest, OnlyNvencGetsItsOwnDevice) {
    EXPECT_TRUE(ce::media::EncoderUsesSeparateDevice("av1_nvenc"));
    EXPECT_TRUE(ce::media::EncoderUsesSeparateDevice("hevc_nvenc"));
    EXPECT_TRUE(ce::media::EncoderUsesSeparateDevice("h264_nvenc"));
    EXPECT_FALSE(ce::media::EncoderUsesSeparateDevice("hevc_qsv"));
    EXPECT_FALSE(ce::media::EncoderUsesSeparateDevice("av1_amf"));
    EXPECT_FALSE(ce::media::EncoderUsesSeparateDevice("h264_mf"));
    EXPECT_FALSE(ce::media::EncoderUsesSeparateDevice("libx264"));
    EXPECT_FALSE(ce::media::EncoderUsesSeparateDevice(nullptr));
}

TEST(EncoderDeviceBridgeTest, GateDepthCoversTheBudgetAtEveryRate) {
    // 33 ms of frames, rounded up, at least one and capped.
    EXPECT_EQ(ce::media::DeferredSubmitDepth(30), 1u);
    EXPECT_EQ(ce::media::DeferredSubmitDepth(60), 2u);
    EXPECT_EQ(ce::media::DeferredSubmitDepth(120), 4u);
    EXPECT_EQ(ce::media::DeferredSubmitDepth(144), 5u);
    EXPECT_EQ(ce::media::DeferredSubmitDepth(240), 8u);
    EXPECT_EQ(ce::media::DeferredSubmitDepth(1000), ce::media::kDeferredSubmitMaxDepth);
    EXPECT_EQ(ce::media::DeferredSubmitDepth(0), 1u);
    EXPECT_EQ(ce::media::DeferredSubmitDepth(-5), 1u);
}

TEST(EncoderDeviceBridgeTest, GateHoldsUnfinishedConversionsUntilItIsFull) {
    // Nothing pending: nothing to do.
    EXPECT_EQ(NextSubmitGateAction(0, 4, false, false), SubmitGateAction::Hold);
    EXPECT_EQ(NextSubmitGateAction(0, 4, false, true), SubmitGateAction::Hold);
    // A finished conversion is sent at once, whatever the depth.
    EXPECT_EQ(NextSubmitGateAction(1, 4, true, false), SubmitGateAction::Send);
    EXPECT_EQ(NextSubmitGateAction(9, 4, true, false), SubmitGateAction::Send);
    // A running conversion is held while the gate has room ...
    EXPECT_EQ(NextSubmitGateAction(1, 4, false, false), SubmitGateAction::Hold);
    EXPECT_EQ(NextSubmitGateAction(4, 4, false, false), SubmitGateAction::Hold);
    // ... and waited for once it is over depth, or when the recording drains.
    EXPECT_EQ(NextSubmitGateAction(5, 4, false, false), SubmitGateAction::WaitThenSend);
    EXPECT_EQ(NextSubmitGateAction(1, 4, false, true), SubmitGateAction::WaitThenSend);
}

bool CreateWorkDevice(ce::ComGuard<ID3D11Device>& device, ce::ComGuard<ID3D11DeviceContext>& context) {
    return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       nullptr, 0, D3D11_SDK_VERSION, device.addressof(), nullptr,
                                       context.addressof()));
}

// Writes `fill` into an encoder-device texture through its work-device alias,
// gates on the fence and reads the first bytes back on the encoder device.
void ExpectWriteVisibleOnEncoderDevice(DXGI_FORMAT format, const uint8_t* fill, size_t fillBytes) {
    ce::ComGuard<ID3D11Device> workDevice;
    ce::ComGuard<ID3D11DeviceContext> workContext;
    if (!CreateWorkDevice(workDevice, workContext))
        GTEST_SKIP() << "no hardware D3D11 device";
    EncoderDeviceBridge bridge;
    std::string error;
    ASSERT_TRUE(bridge.Create(workDevice.get(), &error)) << error;
    ASSERT_NE(bridge.EncoderDevice(), nullptr);
    ASSERT_NE(bridge.EncoderDevice(), workDevice.get());

    constexpr UINT kSize = 16;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = desc.Height = kSize;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = format;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = ce::media::kEncoderBridgePoolMiscFlags;
    ce::ComGuard<ID3D11Texture2D> poolTexture;
    if (FAILED(bridge.EncoderDevice()->CreateTexture2D(&desc, nullptr, poolTexture.addressof())))
        GTEST_SKIP() << "shared pool texture unsupported for format " << format;

    ID3D11Texture2D* alias = bridge.WorkTexture(poolTexture.get());
    ASSERT_NE(alias, nullptr);
    EXPECT_EQ(bridge.WorkTexture(poolTexture.get()), alias);  // cached
    EXPECT_EQ(bridge.CachedTextureCount(), 1u);

    // Source with the fill pattern in every row of the first plane.
    D3D11_TEXTURE2D_DESC sourceDesc = desc;
    sourceDesc.BindFlags = 0;
    sourceDesc.MiscFlags = 0;
    const UINT rowPitch = 64;
    std::vector<uint8_t> pixels(static_cast<size_t>(rowPitch) * kSize * 2, 0);
    for (size_t row = 0; row < kSize * 2; ++row) {
        for (size_t offset = 0; offset + fillBytes <= rowPitch; offset += fillBytes)
            std::memcpy(pixels.data() + row * rowPitch + offset, fill, fillBytes);
    }
    D3D11_SUBRESOURCE_DATA data = {pixels.data(), rowPitch, 0};
    ce::ComGuard<ID3D11Texture2D> source;
    ASSERT_TRUE(SUCCEEDED(workDevice->CreateTexture2D(&sourceDesc, &data, source.addressof())));
    workContext->CopyResource(alias, source.get());
    const uint64_t value = bridge.SignalWorkDone(workContext.get());
    ASSERT_NE(value, 0u);
    EXPECT_EQ(bridge.Wait(value, 5000), EncoderDeviceBridge::WaitResult::Complete);
    EXPECT_TRUE(bridge.IsComplete(value));
    // A value nobody signalled times out without blocking.
    EXPECT_EQ(bridge.Wait(value + 1, 0), EncoderDeviceBridge::WaitResult::Timeout);
    EXPECT_FALSE(bridge.IsComplete(value + 1));

    ce::ComGuard<ID3D11DeviceContext> encoderContext;
    bridge.EncoderDevice()->GetImmediateContext(encoderContext.addressof());
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.MiscFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ce::ComGuard<ID3D11Texture2D> staging;
    ASSERT_TRUE(SUCCEEDED(bridge.EncoderDevice()->CreateTexture2D(&stagingDesc, nullptr, staging.addressof())));
    encoderContext->CopyResource(staging.get(), poolTexture.get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    ASSERT_TRUE(SUCCEEDED(encoderContext->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)));
    EXPECT_EQ(std::memcmp(mapped.pData, fill, fillBytes), 0);
    encoderContext->Unmap(staging.get(), 0);
}

TEST(EncoderDeviceBridgeTest, Nv12WrittenOnTheWorkDeviceIsVisibleToTheEncoderDevice) {
    const uint8_t luma[] = {0x5A};
    ExpectWriteVisibleOnEncoderDevice(DXGI_FORMAT_NV12, luma, sizeof(luma));
}

TEST(EncoderDeviceBridgeTest, P010WrittenOnTheWorkDeviceIsVisibleToTheEncoderDevice) {
    const uint8_t luma[] = {0xC0, 0x8A};  // 10-bit code 554 in the high bits
    ExpectWriteVisibleOnEncoderDevice(DXGI_FORMAT_P010, luma, sizeof(luma));
}

TEST(EncoderDeviceBridgeTest, RgbWrittenOnTheWorkDeviceIsVisibleToTheEncoderDevice) {
    const uint8_t bgra[] = {0x10, 0x20, 0x30, 0xFF};
    ExpectWriteVisibleOnEncoderDevice(DXGI_FORMAT_B8G8R8A8_UNORM, bgra, sizeof(bgra));
}

std::string ReadVideoSource(const char* name) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / "mediaengine" / "video" / name);
}

TEST(EncoderDeviceBridgeTest, EncoderSubmitsGoThroughTheGateUnderTheBridge) {
    const std::string send = ReadVideoSource("video_encoder_send.cpp");
    const std::string gate = ReadVideoSource("video_encoder_submit_gate.cpp");
    ASSERT_FALSE(send.empty());
    ASSERT_FALSE(gate.empty());
    const size_t entry = send.find("bool VideoEncoder::SendFrameToEncoder(");
    ASSERT_NE(entry, std::string::npos);
    EXPECT_NE(send.find("encoderBridgeInUse", entry), std::string::npos);
    EXPECT_NE(send.find("HandOffFrameToEncoder(", entry), std::string::npos);
    // The hand-over signals CE's fence and keeps a frame reference; the send
    // happens only after the fence was observed complete (or waited for).
    const size_t handOff = gate.find("bool VideoEncoder::HandOffFrameToEncoder(");
    ASSERT_NE(handOff, std::string::npos);
    const size_t clone = gate.find("av_frame_clone(frame)", handOff);
    const size_t signal = gate.find("SignalWorkDone(d3d11Context)", handOff);
    ASSERT_NE(clone, std::string::npos);
    ASSERT_NE(signal, std::string::npos);
    const size_t service = gate.find("bool VideoEncoder::ServicePendingEncoderSubmits(");
    ASSERT_NE(service, std::string::npos);
    const size_t isComplete = gate.find("IsComplete(entry.fenceValue)", service);
    const size_t wait = gate.find("encoderBridge.Wait(entry.fenceValue", service);
    const size_t sendNow = gate.find("SendFrameNow(entry.frame", service);
    ASSERT_NE(isComplete, std::string::npos);
    ASSERT_NE(wait, std::string::npos);
    ASSERT_NE(sendNow, std::string::npos);
    EXPECT_LT(isComplete, sendNow);
    EXPECT_LT(wait, sendNow);
    // Pool frames come from the encoder device with shared NT handles.
    EXPECT_NE(gate.find("kEncoderBridgePoolMiscFlags"), std::string::npos);
    EXPECT_NE(gate.find("AV_PIX_FMT_QSV"), std::string::npos);
}

TEST(EncoderDeviceBridgeTest, StopSendsGatedFramesBeforeTheWriterFlushesTheEncoder) {
    const std::string lifecycle = ReadVideoSource("video_encoder_lifecycle.cpp");
    ASSERT_FALSE(lifecycle.empty());
    const size_t stop = lifecycle.find("void VideoEncoder::Stop()");
    ASSERT_NE(stop, std::string::npos);
    const size_t flushGate = lifecycle.find("FlushPendingEncoderSubmits(\"stop\")", stop);
    const size_t stopping = lifecycle.find("isStopping = true", stop);
    ASSERT_NE(flushGate, std::string::npos);
    ASSERT_NE(stopping, std::string::npos);
    EXPECT_LT(flushGate, stopping);
    EXPECT_NE(lifecycle.find("ReleaseEncoderFramesContexts()"), std::string::npos);
}

TEST(EncoderDeviceBridgeTest, CeTouchesPoolTexturesOnlyThroughTheirWorkDeviceAlias) {
    // A raw data[0] on CE's context would be an encoder-device texture.
    for (const char* name : {"video_encoder_encode.cpp", "video_encoder_framegrab.cpp", "video_encoder_textures.cpp",
                             "video_encoder_convert_bgra.cpp"}) {
        const std::string source = ReadVideoSource(name);
        ASSERT_FALSE(source.empty()) << name;
        EXPECT_EQ(source.find("ID3D11Texture2D*>(d3d11Frame->data[0])"), std::string::npos) << name;
        EXPECT_EQ(source.find("(ID3D11Texture2D*)d3d11Frame->data[0]"), std::string::npos) << name;
        EXPECT_EQ(source.find("ID3D11Texture2D*>(outputFrame->data[0])"), std::string::npos) << name;
    }
}

}  // namespace
