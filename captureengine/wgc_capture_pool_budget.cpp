#include "wgc_capture_internal.h"


#if HAS_WGC

ce::capture_policy::WgcSmoothnessSurfaceBudget WGCCapture::Impl::ComputeTexturePoolBudget(uint32_t width,  uint32_t height, 
                                                                            DXGI_FORMAT format) const {

        const DXGI_FORMAT retainedFormat = GetRetainedPoolFormat(format);
        // The duplication backend has no consumer-owned source frame pool (the
        // OS holds the desktop image), so its entire VRAM budget funds retained
        // copy slots for the smoothness reservoir.
        const bool requiresSourceFramePool = !useDuplicationBackend_;
        return ce::capture_policy::ComputeWgcSmoothnessSurfaceBudget(
            smoothnessBufferEnabled_ ? smoothnessOutputFps_ : 0u, smoothnessBufferEnabled_ ? smoothnessMaxMs_ : 0u,
            width, height, BytesPerPixelForFormat(format), BytesPerPixelForFormat(retainedFormat),

#endif
#if HAS_WGC
            smoothnessVramBudgetMb_, smoothnessSyncDelayFrames_, requiresSourceFramePool);

}

#endif


#if HAS_WGC

void WGCCapture::Impl::UpdateSmoothnessBudget(uint32_t width,  uint32_t height,  DXGI_FORMAT format,  bool logBudget) {

        const DXGI_FORMAT retainedFormat = GetRetainedPoolFormat(format);
        const auto budget = ComputeTexturePoolBudget(width, height, format);
        smoothnessRetainedFrames_ = smoothnessBufferEnabled_ ? budget.retainedExtraFrames : 0u;
        smoothnessRetainedFrameCap_ = budget.retainedFrameCap;
        sourceFramePoolBufferCount_ = allocationLimitedSourceBuffers_ != 0
                                          ? std::min(budget.sourceFramePoolBuffers, allocationLimitedSourceBuffers_)
                                          : budget.sourceFramePoolBuffers;
        smoothnessBudgetSurfaceCount_ = budget.budgetSurfaceCount;
        smoothnessSafetySlots_ = budget.safetySlots;
        smoothnessReservedFreeSlots_ = budget.reservedFreeCopySlots;
        smoothnessSourceFormat_ = format;
        smoothnessCopyFormat_ = retainedFormat;
        smoothnessSourceBytesPerSurface_ = budget.sourceBytesPerSurface;
        smoothnessCopyBytesPerSurface_ = budget.copyBytesPerSurface;
        smoothnessSourceEstimatedVramBytes_ = budget.sourceEstimatedBytes;
        smoothnessCopyEstimatedVramBytes_ = budget.copyEstimatedBytes;
        smoothnessEstimatedVramBytes_ = budget.estimatedBytes;
        smoothnessBudgetExhausted_ = budget.budgetExhausted;
        texturePoolSlotCount_ = budget.copyPoolSlots;
        compactRetainedCopyActive_ = IsCompactRetainedCopy(format, retainedFormat);
        ingressRetainedFrameCap_.store(smoothnessRetainedFrameCap_, std::memory_order_relaxed);
        if (logBudget) {
            const uint32_t budgetFps =
                smoothnessBufferEnabled_ ? ce::capture_policy::GetWgcSmoothnessBudgetFps(smoothnessOutputFps_) : 0;
            const uint32_t desiredFrames = smoothnessBufferEnabled_ ? ce::capture_policy::GetWgcSmoothnessDesiredFrames(
                                                                          smoothnessOutputFps_, smoothnessMaxMs_)
                                                                    : 0;
            const uint32_t capShortfall =
                desiredFrames > smoothnessRetainedFrames_ ? desiredFrames - smoothnessRetainedFrames_ : 0;
            const uint64_t capShortfallBytes = capShortfall * smoothnessCopyBytesPerSurface_;
            LogInfo(
                "[WGC] Smoothness buffer budget: enabled=%d targetMs=%u outputFps=%u budgetFps=%u desiredFrames=%u "
                "retainedFrames=%u sourceFramePoolBuffers=%u copyPoolSlots=%u budgetSurfaces=%u syncFrames=%u "
                "extraFrames=%u retainedCap=%u reservedFreeSlots=%u safetySlots=%u fmt=%d %ux%u bpp=%u budget=%uMB "
                "sourceFmt=%s retainedFmt=%s compactRetained=%d sourceSurfaceMB=%.1f copySurfaceMB=%.1f "
                "sourceBudgetMB=%.1f copyBudgetMB=%.1f estimated=%lluMB capLimited=%d capShortfall=%u "
                "capShortfallMB=%.0f budgetExhausted=%d",
                smoothnessBufferEnabled_ ? 1 : 0, smoothnessMaxMs_, smoothnessOutputFps_, budgetFps, desiredFrames,
                smoothnessRetainedFrames_, sourceFramePoolBufferCount_, texturePoolSlotCount_,
                smoothnessBudgetSurfaceCount_, smoothnessSyncDelayFrames_, smoothnessRetainedFrames_,
                smoothnessRetainedFrameCap_, smoothnessReservedFreeSlots_, smoothnessSafetySlots_, format, width,
                height, BytesPerPixelForFormat(format), smoothnessVramBudgetMb_, DxgiFormatName(format),
                DxgiFormatName(retainedFormat), compactRetainedCopyActive_ ? 1 : 0,
                static_cast<double>(smoothnessSourceBytesPerSurface_) / (1024.0 * 1024.0),
                static_cast<double>(smoothnessCopyBytesPerSurface_) / (1024.0 * 1024.0),
                static_cast<double>(smoothnessSourceEstimatedVramBytes_) / (1024.0 * 1024.0),
                static_cast<double>(smoothnessCopyEstimatedVramBytes_) / (1024.0 * 1024.0),
                static_cast<unsigned long long>((smoothnessEstimatedVramBytes_ + 1024ull * 1024ull - 1ull) /
                                                (1024ull * 1024ull)),
                budget.capLimited ? 1 : 0, capShortfall, static_cast<double>(capShortfallBytes) / (1024.0 * 1024.0),
                budget.budgetExhausted ? 1 : 0);
        }

}

#endif


#if HAS_WGC

void WGCCapture::Impl::ReleaseTexturePool() {

        ReleasePoolConversionResources();
        for (auto* mutex : captureTextureMutexPool_) {
            SafeRelease(mutex);
        }
        for (auto* texture : captureTexturePool_) {
            SafeRelease(texture);
        }
        for (auto* texture : texturePool_) {
            SafeRelease(texture);
        }
        texturePool_.clear();
        captureTexturePool_.clear();
        captureTextureMutexPool_.clear();
        poolSlotLastWriteQpc_.clear();
        poolLeaseState_.reset();
        poolWidth_ = 0;
        poolHeight_ = 0;
        poolSourceFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
        poolFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
        compactRetainedCopyActive_ = false;
        poolWriteIndex_.store(0, std::memory_order_relaxed);

}

#endif

#if HAS_WGC

#endif

#if HAS_WGC

#endif

#if HAS_WGC

#endif

#if HAS_WGC

#endif


#if HAS_WGC

void WGCCapture::Impl::LogVideoMemoryInfo(const char* stage,  bool force) {

        if (!d3dDevice_)
            return;
        const ULONGLONG now = GetTickCount64();
        ULONGLONG previous = lastVideoMemoryLogTick_.load(std::memory_order_relaxed);
        if (!force && previous != 0 && now - previous < 1000)
            return;
        if (!lastVideoMemoryLogTick_.compare_exchange_strong(previous, now, std::memory_order_relaxed) && !force) {
            return;
        }
        if (force)
            lastVideoMemoryLogTick_.store(now, std::memory_order_relaxed);

        IDXGIDevice* dxgiDevice = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIAdapter3* adapter3 = nullptr;
        HRESULT hr = d3dDevice_->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
        if (SUCCEEDED(hr) && dxgiDevice)
            hr = dxgiDevice->GetAdapter(&adapter);
        if (SUCCEEDED(hr) && adapter)
            hr = adapter->QueryInterface(IID_PPV_ARGS(&adapter3));
        DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
        if (SUCCEEDED(hr) && adapter3)
            hr = adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
        if (SUCCEEDED(hr)) {
            const bool overBudget = info.CurrentUsage > info.Budget;
            const uint32_t episodes = overBudget
                                          ? videoMemoryOverBudgetEpisodes_.fetch_add(1, std::memory_order_relaxed) + 1
                                          : videoMemoryOverBudgetEpisodes_.load(std::memory_order_relaxed);
            LogInfo(
                "[WGC] Video memory: stage=%s budget=%.1fMB processUsage=%.1fMB reservation=%.1fMB "
                "availableReservation=%.1fMB estimatedPool=%.1fMB overBudget=%d episodes=%u",
                stage ? stage : "periodic", static_cast<double>(info.Budget) / (1024.0 * 1024.0),
                static_cast<double>(info.CurrentUsage) / (1024.0 * 1024.0),
                static_cast<double>(info.CurrentReservation) / (1024.0 * 1024.0),
                static_cast<double>(info.AvailableForReservation) / (1024.0 * 1024.0),
                static_cast<double>(smoothnessEstimatedVramBytes_) / (1024.0 * 1024.0), overBudget ? 1 : 0, episodes);
        } else if (force) {
            LogWarn("[WGC] QueryVideoMemoryInfo failed: stage=%s hr=0x%08lX", stage ? stage : "unknown",
                    static_cast<unsigned long>(hr));
        }
        SafeRelease(adapter3);
        SafeRelease(adapter);
        SafeRelease(dxgiDevice);

}

#endif


#if HAS_WGC

bool WGCCapture::Impl::IsAllocationExhaustion(HRESULT hr) {

        return hr == E_OUTOFMEMORY || hr == HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY) ||
               hr == HRESULT_FROM_WIN32(ERROR_COMMITMENT_LIMIT);

}

#endif
