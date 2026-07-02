// DXGI Desktop Duplication frame source (see dxgi_dup_capture.h for the
// architecture contract with the shared screen-grab engine).

#include "dxgi_dup_capture.h"

#include <avrt.h>
#include <algorithm>

#include "../common/capture_pipeline_policy.h"
#include "../common/logging.h"
#include "../common/thread_power_throttling_compat.h"

namespace {

// AcquireNextFrame returns immediately when a desktop update is available; the
// timeout only bounds idle waits (static desktop) and shutdown latency.
constexpr UINT kAcquireTimeoutMs = 50;

// Consecutive unexpected AcquireNextFrame failures (not timeouts, not
// ACCESS_LOST which resets immediately) tolerated before requesting a reset.
constexpr uint32_t kMaxConsecutiveAcquireFailures = 10;

const char* DupFormatName(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return "R10G10B10A2_UNORM";
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return "R16G16B16A16_FLOAT";
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            return "R8G8B8A8_UNORM";
        default:
            return "OTHER";
    }
}

const char* DupRotationName(DXGI_MODE_ROTATION rotation) {
    switch (rotation) {
        case DXGI_MODE_ROTATION_IDENTITY:
            return "identity";
        case DXGI_MODE_ROTATION_ROTATE90:
            return "rotate90";
        case DXGI_MODE_ROTATION_ROTATE180:
            return "rotate180";
        case DXGI_MODE_ROTATION_ROTATE270:
            return "rotate270";
        default:
            return "unspecified";
    }
}

void EnsureDuplicationThreadQoS() {
    THREAD_POWER_THROTTLING_STATE throttlingState = {};
    throttlingState.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    throttlingState.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    throttlingState.StateMask = 0;
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &throttlingState, sizeof(throttlingState));

    DWORD taskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex);
    if (mmcssHandle) {
        AvSetMmThreadPriority(mmcssHandle, AVRT_PRIORITY_HIGH);
        LogInfo("[DXGIDup] Capture thread QoS enabled (tid=%lu, task=Capture)", GetCurrentThreadId());
    } else {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        LogWarn("[DXGIDup] MMCSS unavailable, using THREAD_PRIORITY_HIGHEST (tid=%lu, err=%lu)", GetCurrentThreadId(),
                GetLastError());
    }
}

template <typename T>
void DupSafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

}  // namespace

DxgiDuplicationSource::~DxgiDuplicationSource() {
    Stop();
}

bool DxgiDuplicationSource::Init(ID3D11Device* device, HMONITOR monitor, bool requireHighPrecision, bool outputIsHdr,
                                 std::string* failureReason) {
    auto fail = [&](const std::string& reason) {
        if (failureReason) {
            *failureReason = reason;
        }
        LogWarn("[DXGIDup] Init failed: %s", reason.c_str());
        return false;
    };

    if (!device) {
        return fail("no D3D11 device");
    }
    Stop();
    ReleaseDuplication();

    if (!monitor) {
        const POINT origin = {0, 0};
        monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
        if (!monitor) {
            return fail("no primary monitor");
        }
    }

    IDXGIDevice* dxgiDevice = nullptr;
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(hr) || !dxgiDevice) {
        return fail("QueryInterface(IDXGIDevice) failed: 0x" + std::to_string(static_cast<unsigned long>(hr)));
    }
    IDXGIAdapter* adapter = nullptr;
    hr = dxgiDevice->GetAdapter(&adapter);
    DupSafeRelease(dxgiDevice);
    if (FAILED(hr) || !adapter) {
        return fail("IDXGIDevice::GetAdapter failed");
    }

    // Find the output on the device's adapter that owns the target monitor.
    // Duplication can only be created on the adapter that owns the output; a
    // cross-adapter monitor (hybrid laptop) must fall back to WGC.
    IDXGIOutput* matchedOutput = nullptr;
    for (UINT i = 0;; ++i) {
        IDXGIOutput* output = nullptr;
        if (adapter->EnumOutputs(i, &output) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (!output) {
            continue;
        }
        DXGI_OUTPUT_DESC desc = {};
        if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) {
            matchedOutput = output;
            break;
        }
        DupSafeRelease(output);
    }
    DupSafeRelease(adapter);
    if (!matchedOutput) {
        return fail("target monitor is not on the capture device's adapter (cross-adapter output)");
    }

    DXGI_OUTPUT_DESC outputDesc = {};
    matchedOutput->GetDesc(&outputDesc);

    // Prefer DuplicateOutput1 (format negotiation, HDR/10-bit support).
    IDXGIOutput5* output5 = nullptr;
    HRESULT duplicateHr = E_NOINTERFACE;
    if (SUCCEEDED(matchedOutput->QueryInterface(IID_PPV_ARGS(&output5))) && output5) {
        // The OS delivers the native composed desktop format if it is in this
        // list. High-precision requirements exclude BGRA8 so an 8-bit desktop
        // fails loudly here instead of silently degrading explicit 10-bit.
        DXGI_FORMAT supportedFormats[3] = {};
        UINT formatCount = 0;
        supportedFormats[formatCount++] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (!outputIsHdr) {
            supportedFormats[formatCount++] = DXGI_FORMAT_R10G10B10A2_UNORM;
            if (!requireHighPrecision) {
                supportedFormats[formatCount++] = DXGI_FORMAT_B8G8R8A8_UNORM;
            }
        }
        duplicateHr = output5->DuplicateOutput1(device, 0, formatCount, supportedFormats, &duplication_);
        DupSafeRelease(output5);
        if (FAILED(duplicateHr)) {
            LogWarn("[DXGIDup] DuplicateOutput1 failed: 0x%08lX (formats=%u requireHighPrecision=%d hdr=%d)",
                    static_cast<unsigned long>(duplicateHr), formatCount, requireHighPrecision ? 1 : 0,
                    outputIsHdr ? 1 : 0);
        }
    }

    if (!duplication_ && !requireHighPrecision && !outputIsHdr) {
        // Legacy fallback (pre-1703 or missing IDXGIOutput5): BGRA8 only.
        IDXGIOutput1* output1 = nullptr;
        if (SUCCEEDED(matchedOutput->QueryInterface(IID_PPV_ARGS(&output1))) && output1) {
            duplicateHr = output1->DuplicateOutput(device, &duplication_);
            DupSafeRelease(output1);
            if (FAILED(duplicateHr)) {
                LogWarn("[DXGIDup] Legacy DuplicateOutput failed: 0x%08lX", static_cast<unsigned long>(duplicateHr));
            }
        }
    }
    DupSafeRelease(matchedOutput);

    if (!duplication_) {
        if (duplicateHr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
            return fail("duplication not currently available (too many active duplications)");
        }
        if (duplicateHr == E_ACCESSDENIED) {
            return fail("duplication access denied (secure desktop / session state)");
        }
        if (duplicateHr == DXGI_ERROR_UNSUPPORTED) {
            return fail("duplication unsupported on this output/driver or desktop format excluded by bit-depth policy");
        }
        char reason[96];
        snprintf(reason, sizeof(reason), "duplication creation failed: 0x%08lX",
                 static_cast<unsigned long>(duplicateHr));
        return fail(reason);
    }

    DXGI_OUTDUPL_DESC duplDesc = {};
    duplication_->GetDesc(&duplDesc);

    if (duplDesc.Rotation != DXGI_MODE_ROTATION_IDENTITY && duplDesc.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
        char reason[96];
        snprintf(reason, sizeof(reason), "rotated output (%s) not supported by duplication backend",
                 DupRotationName(duplDesc.Rotation));
        ReleaseDuplication();
        return fail(reason);
    }

    if (!ce::capture_policy::IsAcceptableDxgiDuplicationFormat(static_cast<uint32_t>(duplDesc.ModeDesc.Format),
                                                               requireHighPrecision, outputIsHdr)) {
        char reason[128];
        snprintf(reason, sizeof(reason),
                 "desktop surface format %s violates bit-depth policy (requireHighPrecision=%d hdr=%d)",
                 DupFormatName(duplDesc.ModeDesc.Format), requireHighPrecision ? 1 : 0, outputIsHdr ? 1 : 0);
        ReleaseDuplication();
        return fail(reason);
    }

    device_ = device;
    monitor_ = monitor;
    width_ = duplDesc.ModeDesc.Width;
    height_ = duplDesc.ModeDesc.Height;
    format_ = duplDesc.ModeDesc.Format;
    desktopImageInSystemMemory_ = duplDesc.DesktopImageInSystemMemory != FALSE;

    LogInfo(
        "[DXGIDup] Duplication created: output=%ls monitor=0x%p size=%ux%u format=%s rotation=%s "
        "refresh=%u/%u systemMemory=%d requireHighPrecision=%d hdr=%d",
        outputDesc.DeviceName, monitor, width_, height_, DupFormatName(format_), DupRotationName(duplDesc.Rotation),
        duplDesc.ModeDesc.RefreshRate.Numerator, duplDesc.ModeDesc.RefreshRate.Denominator,
        desktopImageInSystemMemory_ ? 1 : 0, requireHighPrecision ? 1 : 0, outputIsHdr ? 1 : 0);
    return true;
}

bool DxgiDuplicationSource::Start(DxgiDuplicationFrameSink sink) {
    if (!duplication_ || !sink.onFrame) {
        return false;
    }
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }

    sink_ = std::move(sink);
    shutdown_.store(false, std::memory_order_release);
    acquireTimeoutCount_.store(0, std::memory_order_relaxed);
    cursorOnlyUpdateCount_.store(0, std::memory_order_relaxed);
    accumulatedMissedFrameCount_.store(0, std::memory_order_relaxed);
    protectedContentMaskedFrameCount_.store(0, std::memory_order_relaxed);
    consecutiveAcquireFailures_.store(0, std::memory_order_relaxed);
    deliveredFrameCount_.store(0, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);
    captureThread_ = std::thread(&DxgiDuplicationSource::CaptureThreadFunc, this);
    LogInfo("[DXGIDup] Capture thread started (%ux%u %s)", width_, height_, DupFormatName(format_));
    return true;
}

void DxgiDuplicationSource::Stop() {
    shutdown_.store(true, std::memory_order_release);
    if (captureThread_.joinable()) {
        captureThread_.join();
    }
    running_.store(false, std::memory_order_release);
    ReleaseDuplication();
    sink_ = {};
}

void DxgiDuplicationSource::ReleaseDuplication() {
    DupSafeRelease(duplication_);
}

void DxgiDuplicationSource::CaptureThreadFunc() {
    EnsureDuplicationThreadQoS();

    bool resetRequested = false;
    auto requestReset = [&](const char* reason) {
        if (!resetRequested) {
            resetRequested = true;
            LogWarn("[DXGIDup] Requesting duplication reset: %s (delivered=%llu timeouts=%llu cursorOnly=%llu)",
                    reason, static_cast<unsigned long long>(deliveredFrameCount_.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(acquireTimeoutCount_.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(cursorOnlyUpdateCount_.load(std::memory_order_relaxed)));
            if (sink_.onResetNeeded) {
                sink_.onResetNeeded(reason);
            }
        }
    };

    while (!shutdown_.load(std::memory_order_acquire)) {
        if (resetRequested) {
            // Duplication is unusable; park until the owner stops/reinits so
            // the reset request is not spammed and no busy loop burns CPU.
            Sleep(10);
            continue;
        }

        DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
        IDXGIResource* resource = nullptr;
        const HRESULT hr = duplication_->AcquireNextFrame(kAcquireTimeoutMs, &frameInfo, &resource);
        lastAcquireHr_.store(static_cast<int32_t>(hr), std::memory_order_relaxed);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            // Static desktop: no update is normal, CFR repeats cover it.
            acquireTimeoutCount_.fetch_add(1, std::memory_order_relaxed);
            consecutiveAcquireFailures_.store(0, std::memory_order_relaxed);
            continue;
        }

        if (hr == DXGI_ERROR_ACCESS_LOST) {
            requestReset("access lost (mode change / HDR toggle / desktop switch)");
            continue;
        }

        if (FAILED(hr)) {
            DupSafeRelease(resource);
            const uint32_t failures = consecutiveAcquireFailures_.fetch_add(1, std::memory_order_relaxed) + 1;
            static std::atomic<uint32_t> failureLogCount{0};
            if (failureLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
                LogWarn("[DXGIDup] AcquireNextFrame failed: 0x%08lX (consecutive=%u)", static_cast<unsigned long>(hr),
                        failures);
            }
            if (failures >= kMaxConsecutiveAcquireFailures) {
                requestReset("persistent AcquireNextFrame failures");
            }
            continue;
        }

        consecutiveAcquireFailures_.store(0, std::memory_order_relaxed);

        if (frameInfo.ProtectedContentMaskedOut) {
            const uint64_t maskedCount = protectedContentMaskedFrameCount_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (maskedCount <= 3 || (maskedCount % 1000ull) == 0ull) {
                LogWarn("[DXGIDup] Protected content masked out of duplicated frame (count=%llu)",
                        static_cast<unsigned long long>(maskedCount));
            }
        }

        // LastPresentTime == 0: pointer shape/position metadata only, the
        // desktop image itself did not change. The encoder-side cursor
        // composition samples the live cursor directly, so nothing to encode.
        if (frameInfo.LastPresentTime.QuadPart == 0) {
            cursorOnlyUpdateCount_.fetch_add(1, std::memory_order_relaxed);
            if (sink_.onCursorOnlyUpdate) {
                sink_.onCursorOnlyUpdate();
            }
            DupSafeRelease(resource);
            duplication_->ReleaseFrame();
            continue;
        }

        if (frameInfo.AccumulatedFrames > 1) {
            // Desktop updated more than once since the last acquire; we only
            // get the newest image. Under a maxed GPU this measures how much
            // the duplication consumer fell behind DWM.
            accumulatedMissedFrameCount_.fetch_add(frameInfo.AccumulatedFrames - 1, std::memory_order_relaxed);
        }

        ID3D11Texture2D* texture = nullptr;
        if (resource && SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&texture))) && texture) {
            D3D11_TEXTURE2D_DESC desc = {};
            texture->GetDesc(&desc);
            if (desc.Width != width_ || desc.Height != height_) {
                LogWarn("[DXGIDup] Desktop size changed from %ux%u to %ux%u", width_, height_, desc.Width,
                        desc.Height);
                texture->Release();
                DupSafeRelease(resource);
                duplication_->ReleaseFrame();
                requestReset("desktop size changed");
                continue;
            }
            deliveredFrameCount_.fetch_add(1, std::memory_order_relaxed);
            sink_.onFrame(texture, desc, frameInfo.LastPresentTime.QuadPart, frameInfo.AccumulatedFrames);
            texture->Release();
        } else {
            static std::atomic<uint32_t> textureFailLogCount{0};
            if (textureFailLogCount.fetch_add(1, std::memory_order_relaxed) < 4) {
                LogWarn("[DXGIDup] Failed to obtain ID3D11Texture2D from duplicated resource");
            }
        }
        DupSafeRelease(resource);

        const HRESULT releaseHr = duplication_->ReleaseFrame();
        if (releaseHr == DXGI_ERROR_ACCESS_LOST) {
            requestReset("access lost at ReleaseFrame");
        } else if (FAILED(releaseHr) && releaseHr != DXGI_ERROR_INVALID_CALL) {
            static std::atomic<uint32_t> releaseFailLogCount{0};
            if (releaseFailLogCount.fetch_add(1, std::memory_order_relaxed) < 4) {
                LogWarn("[DXGIDup] ReleaseFrame failed: 0x%08lX", static_cast<unsigned long>(releaseHr));
            }
        }
    }

    LogInfo("[DXGIDup] Capture thread exiting (delivered=%llu timeouts=%llu cursorOnly=%llu accumulatedMissed=%llu "
            "protectedMasked=%llu)",
            static_cast<unsigned long long>(deliveredFrameCount_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(acquireTimeoutCount_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(cursorOnlyUpdateCount_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(accumulatedMissedFrameCount_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(protectedContentMaskedFrameCount_.load(std::memory_order_relaxed)));
}
