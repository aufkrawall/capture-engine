#include "wgc_capture_internal.h"


#if HAS_WGC

int64_t WGCCapture::Impl::NormalizeSourceFrameQpc(int64_t sourceFrameQpc,  bool* duplicateSourceTimestamp) {


        if (duplicateSourceTimestamp) {
            *duplicateSourceTimestamp = false;
        }
        if (sourceFrameQpc <= 0) {
            return 0;
        }

        const int64_t rawSourceFrameQpc = sourceFrameQpc;
        const int64_t lastObservedRawSourceQpc = lastObservedRawSourceQpc_.load(std::memory_order_relaxed);
        const int64_t lastAssignedSourceQpc = lastAssignedSourceQpc_.load(std::memory_order_relaxed);
        if (lastAssignedSourceQpc > 0 && rawSourceFrameQpc < lastAssignedSourceQpc) {
            if (duplicateSourceTimestamp) {
                *duplicateSourceTimestamp = true;
            }
            normalizedDuplicateTimestampCount_.fetch_add(1, std::memory_order_relaxed);
            sourceFrameQpc = lastAssignedSourceQpc;
        } else if (lastObservedRawSourceQpc > 0 && rawSourceFrameQpc == lastObservedRawSourceQpc) {
            if (duplicateSourceTimestamp) {
                *duplicateSourceTimestamp = true;
            }
            normalizedDuplicateTimestampCount_.fetch_add(1, std::memory_order_relaxed);
            sourceFrameQpc = lastObservedRawSourceQpc;
        }

        if (rawSourceFrameQpc > lastObservedRawSourceQpc) {
            lastObservedRawSourceQpc_.store(rawSourceFrameQpc, std::memory_order_relaxed);
        }
        if (sourceFrameQpc > lastAssignedSourceQpc) {
            lastAssignedSourceQpc_.store(sourceFrameQpc, std::memory_order_relaxed);
        }
        return sourceFrameQpc;

}

bool WGCCapture::Impl::QueryOutputDesc1ForMonitor(HMONITOR monitor,  DXGI_OUTPUT_DESC1& desc1) {


        if (!monitor) {
            return false;
        }

        IDXGIFactory1* factory = nullptr;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr) || !factory) {
            LogWarn("[WGC] CreateDXGIFactory1 failed while probing output format: 0x%lX", (unsigned long)hr);
            return false;
        }

        bool found = false;
        for (UINT adapterIndex = 0; !found; ++adapterIndex) {
            IDXGIAdapter1* adapter = nullptr;
            hr = factory->EnumAdapters1(adapterIndex, &adapter);
            if (hr == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(hr) || !adapter) {
                continue;
            }

            for (UINT outputIndex = 0; !found; ++outputIndex) {
                IDXGIOutput* output = nullptr;
                hr = adapter->EnumOutputs(outputIndex, &output);
                if (hr == DXGI_ERROR_NOT_FOUND) {
                    break;
                }
                if (FAILED(hr) || !output) {
                    continue;
                }

                DXGI_OUTPUT_DESC outputDesc = {};
                if (SUCCEEDED(output->GetDesc(&outputDesc)) && outputDesc.Monitor == monitor) {
                    IDXGIOutput6* output6 = nullptr;
                    hr = output->QueryInterface(IID_PPV_ARGS(&output6));
                    if (SUCCEEDED(hr) && output6) {
                        found = SUCCEEDED(output6->GetDesc1(&desc1));
                        output6->Release();
                    }
                }

                output->Release();
            }

            adapter->Release();
        }

        factory->Release();
        return found;

}

void WGCCapture::Impl::UpdateCaptureFormatSelection() {


        useHighPrecisionCapture_ = false;
        captureIsHDR_ = false;
        outputBitsPerColor_ = 8;
        outputColorSpace_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        capturePixelFormat_ = winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized;
        captureDxgiFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;

        DXGI_OUTPUT_DESC1 desc1 = {};
        const HMONITOR monitor = ResolveTargetMonitor();
        if (!QueryOutputDesc1ForMonitor(monitor, desc1)) {
            if (requireHighPrecisionCapture_) {
                useHighPrecisionCapture_ = true;
                capturePixelFormat_ = winrt::DirectXPixelFormat::R10G10B10A2UIntNormalized;
                captureDxgiFormat_ = DXGI_FORMAT_R10G10B10A2_UNORM;
                LogWarn("[WGC] Output probe unavailable; explicit 10-bit request requires high-precision capture");
            } else {
                LogInfo("[WGC] Output probe unavailable, using BGRA8 capture");
            }
            return;
        }

        outputBitsPerColor_ = desc1.BitsPerColor;
        outputColorSpace_ = desc1.ColorSpace;
        captureIsHDR_ = IsHdrOutputColorSpace(desc1.ColorSpace);
        if (captureIsHDR_) {
            useHighPrecisionCapture_ = true;
            capturePixelFormat_ = winrt::DirectXPixelFormat::R16G16B16A16Float;
            captureDxgiFormat_ = DXGI_FORMAT_R16G16B16A16_FLOAT;
        } else if (desc1.BitsPerColor > 8 || requireHighPrecisionCapture_) {
            useHighPrecisionCapture_ = true;
            capturePixelFormat_ = winrt::DirectXPixelFormat::R10G10B10A2UIntNormalized;
            captureDxgiFormat_ = DXGI_FORMAT_R10G10B10A2_UNORM;
        }

        LogInfo(
            "[WGC] Output probe: bpc=%u colorSpace=%d hdr=%s highPrecision=%s requireHighPrecision=%s "
            "captureFormat=%s",
            outputBitsPerColor_, (int)outputColorSpace_, captureIsHDR_ ? "YES" : "NO",
            useHighPrecisionCapture_ ? "YES" : "NO", requireHighPrecisionCapture_ ? "YES" : "NO",
            DescribeCaptureFormat());

}

#endif
