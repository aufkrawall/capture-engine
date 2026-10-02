#include "wgc_capture_internal.h"


#if HAS_WGC

void WGCCapture::Impl::ReleaseGpuTimingResources() {


        SafeRelease(gpuTimingEnd_);
        SafeRelease(gpuTimingStart_);
        SafeRelease(gpuTimingDisjoint_);
        gpuTimingPending_ = false;
        gpuTimingActive_ = false;
        gpuTimingSubmitQpc_ = 0;

}

bool WGCCapture::Impl::EnsureGpuTimingQueries() {


        if (gpuTimingDisjoint_ && gpuTimingStart_ && gpuTimingEnd_)
            return true;
        SafeRelease(gpuTimingEnd_);
        SafeRelease(gpuTimingStart_);
        SafeRelease(gpuTimingDisjoint_);
        if (!d3dDevice_)
            return false;
        D3D11_QUERY_DESC desc = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        HRESULT hr = d3dDevice_->CreateQuery(&desc, &gpuTimingDisjoint_);
        desc.Query = D3D11_QUERY_TIMESTAMP;
        if (SUCCEEDED(hr))
            hr = d3dDevice_->CreateQuery(&desc, &gpuTimingStart_);
        if (SUCCEEDED(hr))
            hr = d3dDevice_->CreateQuery(&desc, &gpuTimingEnd_);
        if (FAILED(hr)) {
            SafeRelease(gpuTimingEnd_);
            SafeRelease(gpuTimingStart_);
            SafeRelease(gpuTimingDisjoint_);
            LogWarn("[WGC] Nonblocking GPU timing query prewarm failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }
        return true;

}

void WGCCapture::Impl::PollGpuTimingSample() {


        if (!gpuTimingPending_ || !d3dContext_)
            return;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint = {};
        const HRESULT disjointHr =
            d3dContext_->GetData(gpuTimingDisjoint_, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (disjointHr != S_OK)
            return;
        UINT64 start = 0;
        UINT64 end = 0;
        const HRESULT startHr =
            d3dContext_->GetData(gpuTimingStart_, &start, sizeof(start), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        const HRESULT endHr = d3dContext_->GetData(gpuTimingEnd_, &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (startHr != S_OK || endHr != S_OK)
            return;
        LARGE_INTEGER observed = {};
        QueryPerformanceCounter(&observed);
        const double executionUs =
            !disjoint.Disjoint && disjoint.Frequency > 0 && end >= start
                ? static_cast<double>(end - start) * 1000000.0 / static_cast<double>(disjoint.Frequency)
                : -1.0;
        const int64_t observedLatencyUs = qpcFreq_ > 0 && observed.QuadPart >= gpuTimingSubmitQpc_
                                              ? (observed.QuadPart - gpuTimingSubmitQpc_) * 1000000 / qpcFreq_
                                              : -1;
        LogInfo("[WGC GPU Timing] backend=%s execution=%.1fus submitToObserved=%lldus disjoint=%d",
                useDuplicationBackend_ ? "dxgi_dup" : "wgc", executionUs, static_cast<long long>(observedLatencyUs),
                disjoint.Disjoint ? 1 : 0);
        gpuTimingPending_ = false;

}

void WGCCapture::Impl::BeginGpuTimingSample() {


        PollGpuTimingSample();
        const ULONGLONG now = GetTickCount64();
        if (gpuTimingPending_ || gpuTimingActive_ || !EnsureGpuTimingQueries() ||
            (lastGpuTimingSampleTick_ != 0 && now - lastGpuTimingSampleTick_ < 1000)) {
            return;
        }
        d3dContext_->Begin(gpuTimingDisjoint_);
        d3dContext_->End(gpuTimingStart_);
        gpuTimingActive_ = true;
        lastGpuTimingSampleTick_ = now;

}

void WGCCapture::Impl::EndGpuTimingSample() {


        if (!gpuTimingActive_ || !d3dContext_)
            return;
        d3dContext_->End(gpuTimingEnd_);
        d3dContext_->End(gpuTimingDisjoint_);
        LARGE_INTEGER submitted = {};
        QueryPerformanceCounter(&submitted);
        gpuTimingSubmitQpc_ = submitted.QuadPart;
        gpuTimingActive_ = false;
        gpuTimingPending_ = true;

}

#endif
