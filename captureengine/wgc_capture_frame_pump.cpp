#include "wgc_capture_internal.h"


#if HAS_WGC

HMONITOR WGCCapture::Impl::ResolveTargetMonitor() const {

        if (targetWindow_) {
            return MonitorFromWindow(targetWindow_, MONITOR_DEFAULTTONEAREST);
        }
        if (targetMonitor_) {
            return targetMonitor_;
        }
        return MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);

}

#endif


#if HAS_WGC

bool WGCCapture::Impl::GetCaptureOrigin(int32_t& left,  int32_t& top) const {

        left = 0;
        top = 0;

        if (targetWindow_) {
            const char* originMode = ResolveWindowCaptureOrigin(left, top);
            return originMode != nullptr;
        }

        MONITORINFO monitorInfo = {};
        monitorInfo.cbSize = sizeof(monitorInfo);
        HMONITOR monitor = ResolveTargetMonitor();
        if (monitor && GetMonitorInfo(monitor, &monitorInfo)) {
            left = monitorInfo.rcMonitor.left;
            top = monitorInfo.rcMonitor.top;
            return true;
        }

        return false;

}

#endif


#if HAS_WGC

const char* WGCCapture::Impl::ResolveWindowCaptureOrigin(int32_t& left,  int32_t& top) const {

        left = 0;
        top = 0;
        if (!targetWindow_ || !IsWindow(targetWindow_)) {
            return nullptr;
        }

        RECT windowRect = {};
        RECT clientRect = {};
        const bool haveWindowRect = GetWindowRect(targetWindow_, &windowRect) != FALSE;
        const bool haveClientRect = GetWindowClientRectInScreen(targetWindow_, clientRect);
        constexpr LONG kOriginSizeTolerancePx = 8;

        if (frameWidth_ > 0 && frameHeight_ > 0) {
            if (haveClientRect &&
                SizeNearlyMatchesRect(frameWidth_, frameHeight_, clientRect, kOriginSizeTolerancePx)) {
                left = clientRect.left;
                top = clientRect.top;
                return "client-size-match";
            }

            if (haveWindowRect &&
                SizeNearlyMatchesRect(frameWidth_, frameHeight_, windowRect, kOriginSizeTolerancePx)) {
                left = windowRect.left;
                top = windowRect.top;
                return "window-size-match";
            }
        }

        if (haveWindowRect) {
            left = windowRect.left;
            top = windowRect.top;
            return "window-fallback";
        }

        if (haveClientRect) {
            left = clientRect.left;
            top = clientRect.top;
            return "client-fallback";
        }

        return nullptr;

}

#endif

#if HAS_WGC

#endif

#if HAS_WGC

#endif


#if HAS_WGC

size_t WGCCapture::Impl::DrainPendingFrames(std::vector<WGCCapturedFrame>& frames,  size_t maxFrames) {


        if (!framePool_ && !dupSource_) {
            return 0;
        }

        MaybePerformDeferredHDRRecheck();
        frames.clear();
        std::lock_guard<std::mutex> lock(frameMutex_);
        while (!pendingFrames_.empty()) {
            frames.push_back(std::move(pendingFrames_.front()));
            pendingFrames_.pop_front();
            if (maxFrames > 0 && frames.size() > maxFrames) {
                WGCCapturedFrame stale = std::move(frames.front());
                frames.erase(frames.begin());
                ReleaseCapturedFrame(stale);
            }
        }

        return frames.size();

}

#endif


#if HAS_WGC

bool WGCCapture::Impl::GetNextFrame(WGCCapturedFrame& frame) {


        std::vector<WGCCapturedFrame> frames;
        frames.reserve(1);
        if (DrainPendingFrames(frames, 1) == 0) {
            return false;
        }
        frame = std::move(frames.back());
        return frame.texture != nullptr;

}

#endif


#if !HAS_WGC

bool WGCCapture::Impl::GetNextFrame(WGCCapturedFrame&) {


        return false;

}

#endif


#if !HAS_WGC

bool WGCCapture::Impl::GetCaptureOrigin(int32_t&,  int32_t&) const {


        return false;

}

#endif
