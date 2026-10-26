#include "face_camera_capture.h"
#include "common/platform/thread_label.h"
#include "common/logging/log_meter.h"

#include <d3d11_4.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <limits>
#include <string>
#include <utility>
#include "common/platform/raii_helpers.h"
#include "common/platform/secure_dll_loading.h"
#include "mediaengine/engine/mediaengine.h"
#include "mediaengine/engine/mediaengine_handles.h"

namespace {

using EnumDeviceSourcesFn = HRESULT(WINAPI*)(IMFAttributes*, IMFActivate***, UINT32*);
using CreateSourceReaderFn = HRESULT(WINAPI*)(IMFMediaSource*, IMFAttributes*, IMFSourceReader**);

void EnableD3D11MultithreadProtection(ID3D11Device* device) {
    if (!device)
        return;
    ce::ComGuard<ID3D11DeviceContext> immediateContext;
    ce::ComGuard<ID3D11Multithread> multithread;
    device->GetImmediateContext(immediateContext.put());
    if (immediateContext && SUCCEEDED(immediateContext->QueryInterface(IID_PPV_ARGS(multithread.put()))) &&
        multithread) {
        multithread->SetMultithreadProtected(TRUE);
    }
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty())
        return {};
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int length = MultiByteToWideChar(codePage, flags, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        codePage = CP_ACP;
        flags = 0;
        length = MultiByteToWideChar(codePage, flags, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    }
    if (length <= 0)
        return {};
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(codePage, flags, value.c_str(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

bool AttributeEquals(IMFActivate* activate, REFGUID key, const std::wstring& requested) {
    if (!activate || requested.empty())
        return false;
    wchar_t* value = nullptr;
    UINT32 length = 0;
    const HRESULT hr = activate->GetAllocatedString(key, &value, &length);
    const bool matches = SUCCEEDED(hr) && value && _wcsicmp(value, requested.c_str()) == 0;
    CoTaskMemFree(value);
    return matches;
}

ce::ComGuard<IMFActivate> SelectCameraDevice(EnumDeviceSourcesFn enumerate,
                                             const ce::face_camera::Config& config) {
    ce::ComGuard<IMFAttributes> attributes;
    if (!enumerate || FAILED(MFCreateAttributes(attributes.put(), 1)) ||
        FAILED(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                   MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))) {
        return {};
    }

    IMFActivate** devices = nullptr;
    UINT32 deviceCount = 0;
    const HRESULT enumHr = enumerate(attributes.get(), &devices, &deviceCount);
    if (FAILED(enumHr) || !devices || deviceCount == 0) {
        DLL_Log("[FaceCamera] No Media Foundation video-capture device is available: HR=%x count=%u", enumHr,
                deviceCount);
        CoTaskMemFree(static_cast<void*>(devices));
        return {};
    }

    const bool useDefault = _stricmp(config.device.c_str(), "default") == 0;
    const std::wstring requested = useDefault ? std::wstring() : Utf8ToWide(config.device);
    IMFActivate* selected = nullptr;
    for (UINT32 index = 0; index < deviceCount; ++index) {
        IMFActivate* candidate = devices[index];
        if (!selected && (useDefault || AttributeEquals(candidate, MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, requested) ||
                          AttributeEquals(candidate, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                                          requested))) {
            candidate->AddRef();
            selected = candidate;
        }
        candidate->Release();
    }
    CoTaskMemFree(static_cast<void*>(devices));
    if (!selected) {
        DLL_Log("[FaceCamera] Configured camera was not found among %u video-capture device(s)", deviceCount);
    }
    return ce::ComGuard<IMFActivate>(selected);
}

struct CameraMode {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fpsNumerator = 30;
    uint32_t fpsDenominator = 1;
    GUID subtype = GUID_NULL;
    uint32_t yuvMatrix = 0;
    uint32_t nominalRange = 0;
    uint64_t score = (std::numeric_limits<uint64_t>::max)();
};

struct NegotiatedCameraOutput {
    uint32_t width = 0;
    uint32_t height = 0;
    LONG stride = 0;
    ce::face_camera::PixelFormat format = ce::face_camera::PixelFormat::kBgra8;
    ce::face_camera::YuvMatrix matrix = ce::face_camera::YuvMatrix::kBt709;
    ce::face_camera::YuvRange range = ce::face_camera::YuvRange::kLimited;
};

uint32_t SubtypePreference(REFGUID subtype) {
    if (subtype == MFVideoFormat_NV12)
        return 0;
    if (subtype == MFVideoFormat_YUY2)
        return 1;
    if (subtype == MFVideoFormat_RGB32 || subtype == MFVideoFormat_ARGB32)
        return 2;
    if (subtype == MFVideoFormat_MJPG)
        return 3;
    if (subtype == MFVideoFormat_H264)
        return 4;
    return 8;
}

CameraMode ChooseCameraMode(IMFSourceReader* reader, const ce::face_camera::Config& config) {
    CameraMode best;
    for (DWORD index = 0;; ++index) {
        ce::ComGuard<IMFMediaType> type;
        const HRESULT typeHr = reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, index, type.put());
        if (typeHr == MF_E_NO_MORE_TYPES)
            break;
        if (FAILED(typeHr) || !type)
            continue;
        GUID major = GUID_NULL;
        GUID subtype = GUID_NULL;
        UINT32 width = 0;
        UINT32 height = 0;
        UINT32 fpsNumerator = 0;
        UINT32 fpsDenominator = 0;
        if (FAILED(type->GetGUID(MF_MT_MAJOR_TYPE, &major)) || major != MFMediaType_Video ||
            FAILED(MFGetAttributeSize(type.get(), MF_MT_FRAME_SIZE, &width, &height)) || width == 0 || height == 0) {
            continue;
        }
        type->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (FAILED(MFGetAttributeRatio(type.get(), MF_MT_FRAME_RATE, &fpsNumerator, &fpsDenominator)) ||
            fpsNumerator == 0 || fpsDenominator == 0) {
            fpsNumerator = 30;
            fpsDenominator = 1;
        }
        UINT32 yuvMatrix = 0;
        type->GetUINT32(MF_MT_YUV_MATRIX, &yuvMatrix);
        UINT32 nominalRange = 0;
        type->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &nominalRange);

        const uint32_t wantedWidth = config.requestedWidth == 0 ? width : config.requestedWidth;
        const uint32_t wantedHeight = config.requestedHeight == 0 ? height : config.requestedHeight;
        const uint64_t dimensionDelta = static_cast<uint64_t>(std::abs(static_cast<int64_t>(width) - wantedWidth)) +
                                        static_cast<uint64_t>(std::abs(static_cast<int64_t>(height) - wantedHeight));
        const uint64_t fpsMilli = static_cast<uint64_t>(fpsNumerator) * 1000u / fpsDenominator;
        const uint64_t wantedFpsMilli = static_cast<uint64_t>(config.requestedFps) * 1000u;
        const uint64_t fpsDelta = fpsMilli > wantedFpsMilli ? fpsMilli - wantedFpsMilli : wantedFpsMilli - fpsMilli;
        const uint64_t score = dimensionDelta * 1000000ull + fpsDelta * 10ull + SubtypePreference(subtype);
        if (score < best.score) {
            best = {width, height, fpsNumerator, fpsDenominator, subtype, yuvMatrix, nominalRange, score};
        }
    }
    return best;
}

bool ConfigureCameraOutput(IMFSourceReader* reader, const ce::face_camera::Config& config,
                           NegotiatedCameraOutput* output) {
    if (!reader || !output)
        return false;
    CameraMode mode = ChooseCameraMode(reader, config);
    if (mode.width == 0 || mode.height == 0) {
        DLL_Log("[FaceCamera] Camera exposes no usable native video mode");
        return false;
    }

    HRESULT lastHr = MF_E_INVALIDMEDIATYPE;
    for (const GUID& candidateSubtype : ce::face_camera::CandidateOutputSubtypes(mode.subtype)) {
        ce::face_camera::PixelFormat candidateFormat = ce::face_camera::PixelFormat::kBgra8;
        if (!ce::face_camera::SubtypeToPixelFormat(candidateSubtype, &candidateFormat) ||
            !ce::face_camera::DimensionsValidForFormat(candidateFormat, mode.width, mode.height)) {
            continue;
        }
        ce::ComGuard<IMFMediaType> outputType;
        if (FAILED(MFCreateMediaType(outputType.put())) ||
            FAILED(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
            FAILED(outputType->SetGUID(MF_MT_SUBTYPE, candidateSubtype)) ||
            FAILED(MFSetAttributeSize(outputType.get(), MF_MT_FRAME_SIZE, mode.width, mode.height)) ||
            FAILED(MFSetAttributeRatio(outputType.get(), MF_MT_FRAME_RATE, mode.fpsNumerator, mode.fpsDenominator)) ||
            FAILED(MFSetAttributeRatio(outputType.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1)) ||
            FAILED(outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive))) {
            continue;
        }

        lastHr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, outputType.get());
        if (FAILED(lastHr))
            continue;

        ce::ComGuard<IMFMediaType> negotiated;
        lastHr = reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, negotiated.put());
        GUID negotiatedSubtype = GUID_NULL;
        ce::face_camera::PixelFormat negotiatedFormat = ce::face_camera::PixelFormat::kBgra8;
        uint32_t negotiatedWidth = 0;
        uint32_t negotiatedHeight = 0;
        if (FAILED(lastHr) || !negotiated || FAILED(negotiated->GetGUID(MF_MT_SUBTYPE, &negotiatedSubtype)) ||
            !ce::face_camera::SubtypeToPixelFormat(negotiatedSubtype, &negotiatedFormat) ||
            FAILED(MFGetAttributeSize(negotiated.get(), MF_MT_FRAME_SIZE, &negotiatedWidth, &negotiatedHeight)) ||
            !ce::face_camera::DimensionsValidForFormat(negotiatedFormat, negotiatedWidth, negotiatedHeight)) {
            continue;
        }

        const uint32_t defaultRowBytes = ce::face_camera::DefaultRowBytes(negotiatedFormat, negotiatedWidth);
        if (defaultRowBytes > static_cast<uint32_t>((std::numeric_limits<LONG>::max)())) {
            DLL_Log("[FaceCamera] Negotiated %s width cannot be represented by a signed row stride: %u",
                    ce::face_camera::DescribePixelFormat(negotiatedFormat), negotiatedWidth);
            return false;
        }
        UINT32 rawStride = 0;
        const LONG stride = SUCCEEDED(negotiated->GetUINT32(MF_MT_DEFAULT_STRIDE, &rawStride))
                                ? static_cast<LONG>(rawStride)
                                : static_cast<LONG>(defaultRowBytes);
        UINT32 yuvMatrix = 0;
        if (FAILED(negotiated->GetUINT32(MF_MT_YUV_MATRIX, &yuvMatrix)))
            yuvMatrix = mode.yuvMatrix;
        UINT32 nominalRange = 0;
        if (FAILED(negotiated->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &nominalRange)))
            nominalRange = mode.nominalRange;

        output->width = negotiatedWidth;
        output->height = negotiatedHeight;
        output->stride = stride;
        output->format = negotiatedFormat;
        output->matrix = ce::face_camera::ResolveYuvMatrix(yuvMatrix, negotiatedWidth, negotiatedHeight);
        output->range = ce::face_camera::ResolveYuvRange(nominalRange);

        const double fps = static_cast<double>(mode.fpsNumerator) / static_cast<double>(mode.fpsDenominator);
        DLL_Log(
            "[FaceCamera] Negotiated %ux%u %.3f-fps %s (matrix=%s range=%s) from native subtype=%08x; "
            "hardware transforms requested, VideoProcessor MFT disabled, CPU fallback allowed",
            output->width, output->height, fps, ce::face_camera::DescribePixelFormat(output->format),
            ce::face_camera::DescribeYuvMatrix(output->matrix, output->format),
            ce::face_camera::DescribeYuvRange(output->range, output->format), mode.subtype.Data1);
        return true;
    }

    DLL_Log("[FaceCamera] Media-type negotiation failed for %ux%u native subtype=%08x: HR=%x", mode.width, mode.height,
            mode.subtype.Data1, lastHr);
    return false;
}

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
            const uint32_t totalRows =
                format == ce::face_camera::PixelFormat::kNv12 ? (height + height / 2u) : height;
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
    const bool copied = ce::face_camera::ExtractCpuPlanes(format, data, currentLength, width, height, defaultStride,
                                                          primary, chroma);
    buffer->Unlock();
    return copied;
}

}  // namespace

FaceCameraFrame::~FaceCameraFrame() {
    if (gpuSample)
        gpuSample->Release();
}

FaceCameraCapture::FaceCameraCapture(ce::face_camera::Config config) : config_(std::move(config)) {}

FaceCameraCapture::~FaceCameraCapture() {
    Stop();
}

bool FaceCameraCapture::Start(ID3D11Device* device) {
    if (!config_.enabled || !device || started_.exchange(true, std::memory_order_acq_rel))
        return config_.enabled && device;
    EnableD3D11MultithreadProtection(device);
    device_ = device;
    device_->AddRef();
    stopRequested_.store(false, std::memory_order_release);
    state_.store(State::kStarting, std::memory_order_release);
    try {
        captureThread_ = std::thread(WithInheritedLogRoute([this] { CaptureThreadMain(); }));
    } catch (...) {
        device_->Release();
        device_ = nullptr;
        started_.store(false, std::memory_order_release);
        state_.store(State::kFailed, std::memory_order_release);
        DLL_Log("[FaceCamera] Failed to start the independent camera worker");
        return false;
    }
    return true;
}

void FaceCameraCapture::Stop() {
    if (!started_.exchange(false, std::memory_order_acq_rel))
        return;
    stopRequested_.store(true, std::memory_order_release);
    IMFSourceReader* reader = nullptr;
    {
        // Keep publication locked through the synchronous flush. Once a cancelled
        // ReadSample returns, the worker must not pass ClearActiveReader and shut
        // down its source until the reader has finished cancelling that request.
        std::lock_guard<std::mutex> lock(activeSourceMutex_);
        reader = activeReader_;
        if (reader) {
            reader->AddRef();
            DLL_Log("[FaceCamera] Stop requested; cancelling the pending source-reader sample before worker join");
            const uint64_t flushStartMs = GetTickCount64();
            const HRESULT flushHr = reader->Flush(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
            DLL_Log("[FaceCamera] Source-reader flush finished: HR=%x elapsed=%llums", flushHr,
                    static_cast<unsigned long long>(GetTickCount64() - flushStartMs));
        }
    }
    if (reader)
        reader->Release();
    if (captureThread_.joinable()) {
        const uint64_t joinStartMs = GetTickCount64();
        captureThread_.join();
        DLL_Log("[FaceCamera] Camera worker joined: elapsed=%llums",
                static_cast<unsigned long long>(GetTickCount64() - joinStartMs));
    }
    state_.store(State::kStopped, std::memory_order_release);
    std::atomic_store_explicit(&latestFrame_, std::shared_ptr<const FaceCameraFrame>(), std::memory_order_release);
    if (device_) {
        device_->Release();
        device_ = nullptr;
    }
}

std::shared_ptr<const FaceCameraFrame> FaceCameraCapture::LatestFrame() const {
    return std::atomic_load_explicit(&latestFrame_, std::memory_order_acquire);
}

bool FaceCameraCapture::MayProduceFrames() const {
    const State state = state_.load(std::memory_order_acquire);
    return state == State::kNotStarted || state == State::kStarting || state == State::kRunning;
}

void FaceCameraCapture::PublishActiveReader(IMFSourceReader* reader) {
    std::lock_guard<std::mutex> lock(activeSourceMutex_);
    activeReader_ = reader;
}

void FaceCameraCapture::ClearActiveReader(IMFSourceReader* reader) {
    std::lock_guard<std::mutex> lock(activeSourceMutex_);
    if (activeReader_ == reader)
        activeReader_ = nullptr;
}

bool FaceCameraCapture::PublishSample(IMFSample* sample, uint32_t width, uint32_t height, LONG stride,
                                      ce::face_camera::PixelFormat format, ce::face_camera::YuvMatrix matrix,
                                      ce::face_camera::YuvRange range, LONGLONG sampleTime100ns) {
    if (!sample || stopRequested_.load(std::memory_order_acquire))
        return false;
    auto frame = std::make_shared<FaceCameraFrame>();
    frame->sequence = nextSequence_.fetch_add(1, std::memory_order_relaxed);
    frame->receivedTickMs = GetTickCount64();
    LARGE_INTEGER arrival = {};
    LARGE_INTEGER frequency = {};
    QueryPerformanceCounter(&arrival);
    QueryPerformanceFrequency(&frequency);
    const bool deviceTime =
        ce::face_camera::ResolveCaptureQpc(sampleTime100ns, arrival.QuadPart, frequency.QuadPart, &frame->captureQpc);
    if (!timeBasisLogged_) {
        timeBasisLogged_ = true;
        DLL_Log("[FaceCamera] Frame time basis: %s (sample=%lld x100ns arrivalQpc=%lld); the encoder shows the frame "
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
                ce::face_camera::DimensionsValidForFormat(gpuFormat, width, height) &&
                desc.Width >= width && desc.Height >= height) {
                sample->AddRef();
                frame->gpuSample = sample;
                frame->gpuSubresource = subresource;
                frame->format = gpuFormat;
            }
        }
    }
    if (!frame->gpuSample &&
        !CopyCpuCameraFrame(sample, width, height, stride, format, &frame->bgra, &frame->chroma))
        return false;

    std::atomic_store_explicit(&latestFrame_, std::shared_ptr<const FaceCameraFrame>(std::move(frame)),
                               std::memory_order_release);
    return true;
}

void FaceCameraCapture::CaptureThreadMain() {
    ce::platform::NameCurrentThread("CE face camera");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitializeCom = SUCCEEDED(comHr);
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE) {
        DLL_Log("[FaceCamera] COM initialization failed: HR=%x", comHr);
        state_.store(State::kFailed, std::memory_order_release);
        return;
    }
    const HRESULT mfStartupHr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(mfStartupHr)) {
        DLL_Log("[FaceCamera] Media Foundation startup failed: HR=%x", mfStartupHr);
        state_.store(State::kFailed, std::memory_order_release);
        if (uninitializeCom)
            CoUninitialize();
        return;
    }

    ce::ModuleGuard mfModule(ce::security::LoadSystemLibrary(L"mf.dll"));
    ce::ModuleGuard readWriteModule(ce::security::LoadSystemLibrary(L"mfreadwrite.dll"));
    auto enumerate = mfModule ? reinterpret_cast<EnumDeviceSourcesFn>(GetProcAddress(mfModule.get(),
                                                                                    "MFEnumDeviceSources"))
                              : nullptr;
    auto createReader = readWriteModule
                            ? reinterpret_cast<CreateSourceReaderFn>(
                                  GetProcAddress(readWriteModule.get(), "MFCreateSourceReaderFromMediaSource"))
                            : nullptr;
    ce::ComGuard<IMFActivate> activation = SelectCameraDevice(enumerate, config_);
    ce::ComGuard<IMFMediaSource> source;
    ce::ComGuard<IMFSourceReader> reader;
    ce::ComGuard<IMFAttributes> readerAttributes;
    ce::ComGuard<IMFDXGIDeviceManager> deviceManager;
    UINT resetToken = 0;

    HRESULT setupHr = activation ? activation->ActivateObject(IID_PPV_ARGS(source.put())) : E_FAIL;
    if (SUCCEEDED(setupHr))
        setupHr = MFCreateAttributes(readerAttributes.put(), 10);
    if (SUCCEEDED(setupHr)) {
        readerAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        readerAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, FALSE);
        readerAttributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);
        readerAttributes->SetUINT32(MF_READWRITE_D3D_OPTIONAL, TRUE);
        readerAttributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE);
        readerAttributes->SetUINT32(MF_LOW_LATENCY, TRUE);

        if (SUCCEEDED(MFCreateDXGIDeviceManager(&resetToken, deviceManager.put())) && deviceManager &&
            SUCCEEDED(deviceManager->ResetDevice(device_, resetToken))) {
            const HRESULT managerHr =
                readerAttributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, deviceManager.get());
            if (SUCCEEDED(managerHr)) {
                DLL_Log("[FaceCamera] D3D11 device-manager path enabled for GPU-resident camera transforms");
            } else {
                DLL_Log("[FaceCamera] D3D11 manager attribute failed; using isolated CPU frame transport: HR=%x",
                        managerHr);
                deviceManager.reset();
            }
        } else {
            DLL_Log("[FaceCamera] D3D11 device-manager setup unavailable; using isolated CPU frame transport");
        }
    }
    if (SUCCEEDED(setupHr) && createReader)
        setupHr = createReader(source.get(), readerAttributes.get(), reader.put());
    else if (SUCCEEDED(setupHr))
        setupHr = E_NOINTERFACE;

    NegotiatedCameraOutput negotiated = {};
    if (SUCCEEDED(setupHr)) {
        reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
        if (!ConfigureCameraOutput(reader.get(), config_, &negotiated))
            setupHr = MF_E_INVALIDMEDIATYPE;
    }

    uint64_t received = 0;
    uint64_t published = 0;
    uint64_t extractFailures = 0;
    bool transportLogged = false;
    if (SUCCEEDED(setupHr) && !stopRequested_.load(std::memory_order_acquire)) {
        PublishActiveReader(reader.get());
        state_.store(State::kRunning, std::memory_order_release);
        DLL_Log("[FaceCamera] Camera worker ready; encoder sampling is nonblocking and latest-frame-only");
        while (!stopRequested_.load(std::memory_order_acquire)) {
            DWORD streamIndex = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            IMFSample* rawSample = nullptr;
            const HRESULT readHr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex, &flags,
                                                      &timestamp, &rawSample);
            ce::ComGuard<IMFSample> sample(rawSample);
            if (FAILED(readHr)) {
                if (!stopRequested_.load(std::memory_order_acquire))
                    DLL_Log("[FaceCamera] Camera ReadSample failed; main video continues without camera: HR=%x",
                            readHr);
                break;
            }
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                DLL_Log("[FaceCamera] Camera stream ended; main video continues without camera");
                break;
            }
            if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
                if (!ConfigureCameraOutput(reader.get(), config_, &negotiated)) {
                    DLL_Log("[FaceCamera] Camera media type changed and renegotiation failed; overlay hidden");
                    break;
                }
                transportLogged = false;
                DLL_Log("[FaceCamera] Camera media type changed; %s output was renegotiated",
                        ce::face_camera::DescribePixelFormat(negotiated.format));
                continue;
            }
            if (!sample)
                continue;
            ++received;
            if (PublishSample(sample.get(), negotiated.width, negotiated.height, negotiated.stride, negotiated.format,
                              negotiated.matrix, negotiated.range, timestamp)) {
                ++published;
                const auto latest = LatestFrame();
                if (latest && !transportLogged) {
                    transportLogged = true;
                    const bool gpu = latest->gpuSample != nullptr;
                    DLL_Log("[FaceCamera] Native YUV path: subtype=%s matrix=%s range=%s gpu=%d",
                            ce::face_camera::DescribePixelFormat(latest->format),
                            ce::face_camera::DescribeYuvMatrix(latest->matrix, latest->format),
                            ce::face_camera::DescribeYuvRange(latest->range, latest->format), gpu ? 1 : 0);
                    if (gpu) {
                        DLL_Log("[FaceCamera] Camera frames are GPU-resident; no per-frame CPU upload is used");
                    } else {
                        DLL_Log(
                            "[FaceCamera] Camera frames use one %s upload per camera frame; conversion and blending "
                            "stay on GPU",
                            ce::face_camera::DescribePixelFormat(latest->format));
                    }
                }
            } else {
                ++extractFailures;
                if (extractFailures <= 5)
                    DLL_Log("[FaceCamera] Camera frame extraction failed (%llu)",
                            static_cast<unsigned long long>(extractFailures));
            }
        }
        ClearActiveReader(reader.get());
    } else if (!stopRequested_.load(std::memory_order_acquire)) {
        DLL_Log("[FaceCamera] Camera startup failed; main video continues without camera: HR=%x", setupHr);
    }

    state_.store(stopRequested_.load(std::memory_order_acquire) ? State::kStopped : State::kFailed,
                 std::memory_order_release);

    if (source)
        source->Shutdown();
    std::atomic_store_explicit(&latestFrame_, std::shared_ptr<const FaceCameraFrame>(), std::memory_order_release);
    DLL_Log("[FaceCamera] Camera worker stopped: received=%llu published=%llu extractFailures=%llu",
            static_cast<unsigned long long>(received), static_cast<unsigned long long>(published),
            static_cast<unsigned long long>(extractFailures));
    reader.reset();
    source.reset();
    if (activation)
        activation->ShutdownObject();
    activation.reset();
    deviceManager.reset();
    readerAttributes.reset();
    MFShutdown();
    if (uninitializeCom)
        CoUninitialize();
}
