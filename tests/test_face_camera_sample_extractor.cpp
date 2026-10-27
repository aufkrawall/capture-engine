// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include <gtest/gtest.h>

#include <mfapi.h>
#include <wrl/client.h>
#include <array>
#include <functional>

#include "mediaengine/video/face_camera_sample_extractor.h"

namespace {
using ce::face_camera::SampleStatus;
using Microsoft::WRL::ComPtr;

// A real Media Foundation sample holds this buffer. Lock is the deterministic injection point for a stop
// arriving during extraction; no camera hardware, private capture state, sleeps or timing assumptions are needed.
class ScriptedBuffer final : public IMFMediaBuffer {
public:
    STDMETHODIMP QueryInterface(REFIID id, void** value) override {
        if (!value)
            return E_POINTER;
        *value = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(IMFMediaBuffer))
            return E_NOINTERFACE;
        *value = static_cast<IMFMediaBuffer*>(this);
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override {
        return InterlockedIncrement(&refs_);
    }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG refs = InterlockedDecrement(&refs_);
        if (!refs)
            delete this;
        return refs;
    }
    STDMETHODIMP Lock(BYTE** data, DWORD* maximum, DWORD* current) override {
        ++locks;
        if (onLock)
            onLock();
        if (failLock)
            return E_FAIL;
        *data = bytes.data();
        if (maximum)
            *maximum = static_cast<DWORD>(bytes.size());
        if (current)
            *current = length_;
        return S_OK;
    }
    STDMETHODIMP Unlock() override {
        return S_OK;
    }
    STDMETHODIMP GetCurrentLength(DWORD* length) override {
        *length = length_;
        return S_OK;
    }
    STDMETHODIMP SetCurrentLength(DWORD length) override {
        length_ = length;
        return S_OK;
    }
    STDMETHODIMP GetMaxLength(DWORD* length) override {
        *length = static_cast<DWORD>(bytes.size());
        return S_OK;
    }

    std::array<BYTE, 16> bytes{1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255};
    std::function<void()> onLock;
    bool failLock = false;
    int locks = 0;

private:
    LONG refs_ = 1;
    DWORD length_ = 16;
};

class FaceCameraSampleExtractorTest : public testing::Test {
protected:
    void SetUp() override {
        initialized_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(initialized_));
        startup_ = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        ASSERT_TRUE(SUCCEEDED(startup_));
        buffer.Attach(new ScriptedBuffer());
        ASSERT_TRUE(SUCCEEDED(MFCreateSample(&sample)));
        ASSERT_TRUE(SUCCEEDED(sample->AddBuffer(buffer.Get())));
    }
    void TearDown() override {
        sample.Reset();
        buffer.Reset();
        if (SUCCEEDED(startup_))
            MFShutdown();
        if (SUCCEEDED(initialized_))
            CoUninitialize();
    }
    ce::face_camera::SampleExtraction Extract(bool cancelled = false) {
        return extractor.Extract(sample.Get(), 2, 2, 8, ce::face_camera::PixelFormat::kBgra8,
                                 ce::face_camera::YuvMatrix::kBt709, ce::face_camera::YuvRange::kFull, 0, cancelled);
    }

    ce::face_camera::SampleExtractor extractor;
    ComPtr<ScriptedBuffer> buffer;
    ComPtr<IMFSample> sample;
    HRESULT initialized_ = E_FAIL;
    HRESULT startup_ = E_FAIL;
};

TEST_F(FaceCameraSampleExtractorTest, StopBetweenReadSampleAndExtractionIsCancellation) {
    const auto result = Extract(true);
    EXPECT_EQ(result.status, SampleStatus::kCancelled);
    EXPECT_EQ(result.frame, nullptr);
    EXPECT_EQ(buffer->locks, 0);
}

TEST_F(FaceCameraSampleExtractorTest, SuccessfulCpuExtractionPublishesAnImmutableFrame) {
    const auto result = Extract();
    ASSERT_EQ(result.status, SampleStatus::kPublished);
    ASSERT_NE(result.frame, nullptr);
    EXPECT_EQ(result.frame->sequence, 1u);
    EXPECT_EQ(result.frame->bgra, std::vector<uint8_t>(buffer->bytes.begin(), buffer->bytes.end()));
    EXPECT_EQ(result.frame->gpuSample, nullptr);
    EXPECT_EQ(result.frame->width, 2u);
    EXPECT_EQ(result.frame->height, 2u);
}

TEST_F(FaceCameraSampleExtractorTest, InFlightExtractionFailureIsNotHiddenByALaterStop) {
    bool stopping = false;
    buffer->onLock = [&] { stopping = true; };
    buffer->failLock = true;
    const auto result = Extract(stopping);
    EXPECT_TRUE(stopping);
    EXPECT_EQ(result.status, SampleStatus::kFailed);
    EXPECT_EQ(result.frame, nullptr);
}

TEST_F(FaceCameraSampleExtractorTest, InFlightSuccessfulExtractionKeepsItsResultAfterALaterStop) {
    bool stopping = false;
    buffer->onLock = [&] { stopping = true; };
    const auto result = Extract(stopping);
    EXPECT_TRUE(stopping);
    EXPECT_EQ(result.status, SampleStatus::kPublished);
    EXPECT_NE(result.frame, nullptr);
}

TEST_F(FaceCameraSampleExtractorTest, InvalidSampleIsAnExtractionFailure) {
    sample.Reset();
    const auto result = Extract();
    EXPECT_EQ(result.status, SampleStatus::kFailed);
    EXPECT_EQ(result.frame, nullptr);
}

TEST_F(FaceCameraSampleExtractorTest, GpuExtractionRetainsTheSampleWithoutACpuCopy) {
    ComPtr<ID3D11Device> device;
    ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                            &device, nullptr, nullptr)));
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 2;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)));
    ComPtr<IMFMediaBuffer> gpuBuffer;
    ASSERT_TRUE(SUCCEEDED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), texture.Get(), 0, FALSE, &gpuBuffer)));
    ASSERT_TRUE(SUCCEEDED(sample->RemoveAllBuffers()));
    ASSERT_TRUE(SUCCEEDED(sample->AddBuffer(gpuBuffer.Get())));
    const auto result = Extract();
    ASSERT_EQ(result.status, SampleStatus::kPublished);
    ASSERT_NE(result.frame, nullptr);
    EXPECT_EQ(result.frame->gpuSample, sample.Get());
    EXPECT_TRUE(result.frame->bgra.empty());
    sample.Reset();
    DWORD buffers = 0;
    EXPECT_TRUE(SUCCEEDED(result.frame->gpuSample->GetBufferCount(&buffers)));
    EXPECT_EQ(buffers, 1u);
}

}  // namespace
