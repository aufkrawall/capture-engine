// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "face_camera_sample_extractor.h"

#include <mfapi.h>
#include <mfobjects.h>
#include <cstdlib>
#include <utility>
#include "common/logging/log_meter.h"
#include "common/platform/raii_helpers.h"
#include "face_camera_history.h"
#include "mediaengine/engine/mediaengine.h"

namespace {

bool CopyCpuCameraFrame(IMFSample* sample, uint32_t width, uint32_t height, LONG defaultStride,
                        ce::face_camera::PixelFormat format, std::vector<uint8_t>* primary,
                        std::vector<uint8_t>* chroma) {
    if (!sample || !primary || !chroma || !ce::face_camera::DimensionsValidForFormat(format, width, height))
        return false;
    ce::ComGuard<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(buffer.put())) || !buffer)
        return false;

    ce::ComGuard<IMF2DBuffer> buffer2d;
    if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(buffer2d.put()))) && buffer2d) {
        BYTE* scanline = nullptr;
        LONG pitch = 0;
        if (SUCCEEDED(buffer2d->Lock2D(&scanline, &pitch)) && scanline) {
            const uint32_t totalRows = format == ce::face_camera::PixelFormat::kNv12 ? (height + height / 2u) : height;
            const size_t lockedLength =
                static_cast<size_t>(std::llabs(static_cast<int64_t>(pitch))) * static_cast<size_t>(totalRows);
            const bool extracted =
                ce::face_camera::ExtractCpuPlanes(format, scanline, lockedLength, width, height, pitch, primary, chroma,
                                                  ce::face_camera::CpuBufferOrigin::kTopScanline);
            buffer2d->Unlock2D();
            if (extracted) {
                if (pitch < 0) {
                    static thread_local ce::log_meter::ChangeGate layoutLog{30000};
                    const auto log = layoutLog.Observe(ce::log_meter::FieldKey(pitch, width, height), GetTickCount64());
                    if (log) {
                        DLL_Log("[FaceCamera] CPU bottom-up RGB32: Lock2D top scanline preserved, pitch=%ld %ux%u%s",
                                pitch, width, height, ce::log_meter::SuppressedNote(log.suppressed).c_str());
                    }
                }
                return true;
            }
        } else if (scanline) {
            buffer2d->Unlock2D();
        }
    }

    BYTE* data = nullptr;
    DWORD maximumLength = 0;
    DWORD currentLength = 0;
    if (FAILED(buffer->Lock(&data, &maximumLength, &currentLength)) || !data)
        return false;
    const bool copied =
        ce::face_camera::ExtractCpuPlanes(format, data, currentLength, width, height, defaultStride, primary, chroma);
    buffer->Unlock();
    return copied;
}

}  // namespace

FaceCameraFrame::~FaceCameraFrame() {
    if (gpuSample)
        gpuSample->Release();
}

ce::face_camera::SampleExtraction ce::face_camera::SampleExtractor::Extract(
    IMFSample* sample, uint32_t width, uint32_t height, LONG stride, ce::face_camera::PixelFormat format,
    ce::face_camera::YuvMatrix matrix, ce::face_camera::YuvRange range, LONGLONG sampleTime100ns, bool cancelled) {
    if (cancelled)
        return {SampleStatus::kCancelled, {}};
    if (!sample)
        return {SampleStatus::kFailed, {}};
    auto frame = std::make_shared<FaceCameraFrame>();
    frame->sequence = nextSequence_++;
    frame->receivedTickMs = GetTickCount64();
    LARGE_INTEGER arrival = {};
    LARGE_INTEGER frequency = {};
    QueryPerformanceCounter(&arrival);
    QueryPerformanceFrequency(&frequency);
    const bool deviceTime =
        ce::face_camera::ResolveCaptureQpc(sampleTime100ns, arrival.QuadPart, frequency.QuadPart, &frame->captureQpc);
    if (!timeBasisLogged_) {
        timeBasisLogged_ = true;
        DLL_Log(
            "[FaceCamera] Frame time basis: %s (sample=%lld x100ns arrivalQpc=%lld); the encoder shows the frame "
            "taken at each output slot's content time",
            deviceTime ? "device sample time (QPC)" : "arrival", static_cast<long long>(sampleTime100ns),
            static_cast<long long>(arrival.QuadPart));
    }
    frame->width = width;
    frame->height = height;
    frame->format = format;
    frame->matrix = matrix;
    frame->range = range;

    ce::ComGuard<IMFMediaBuffer> firstBuffer;
    ce::ComGuard<IMFDXGIBuffer> dxgiBuffer;
    if (SUCCEEDED(sample->GetBufferByIndex(0, firstBuffer.put())) && firstBuffer &&
        SUCCEEDED(firstBuffer->QueryInterface(IID_PPV_ARGS(dxgiBuffer.put()))) && dxgiBuffer) {
        ce::ComGuard<ID3D11Texture2D> texture;
        UINT subresource = 0;
        if (SUCCEEDED(dxgiBuffer->GetResource(IID_PPV_ARGS(texture.put()))) && texture &&
            SUCCEEDED(dxgiBuffer->GetSubresourceIndex(&subresource))) {
            D3D11_TEXTURE2D_DESC desc = {};
            texture->GetDesc(&desc);
            ce::face_camera::PixelFormat gpuFormat = ce::face_camera::PixelFormat::kBgra8;
            if (ce::face_camera::DxgiToPixelFormat(desc.Format, &gpuFormat) &&
                ce::face_camera::DimensionsValidForFormat(gpuFormat, width, height) && desc.Width >= width &&
                desc.Height >= height) {
                sample->AddRef();
                frame->gpuSample = sample;
                frame->gpuSubresource = subresource;
                frame->format = gpuFormat;
            }
        }
    }
    if (!frame->gpuSample && !CopyCpuCameraFrame(sample, width, height, stride, format, &frame->bgra, &frame->chroma))
        return {SampleStatus::kFailed, {}};

    return {SampleStatus::kPublished, std::move(frame)};
}
