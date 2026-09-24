#pragma once

#include <windows.h>

#include <cstdint>
#include <mutex>

#include "../common/ddraw_present_policy.h"

struct IDirectDrawSurface7;

// Owns one application presentation from the pre-composite queue gate through
// the completed DirectDraw call. Keeping the lock for that whole interval makes
// queue-depth accounting deterministic even when an old runtime presents from
// more than one thread. PrepareForCall puts any explicit vertical-blank wait as
// close to the actual publication as possible, after overlay composition.
class DirectDrawPresentationOverrideScope {
public:
    DirectDrawPresentationOverrideScope(IDirectDrawSurface7* surface,
                                        ce::ddraw_present_policy::PresentOperation operation,
                                        bool isPresentation, DWORD& flags);
    DirectDrawPresentationOverrideScope(IUnknown* surface,
                                        ce::ddraw_present_policy::PresentOperation operation,
                                        bool isPresentation, DWORD& flags);
    ~DirectDrawPresentationOverrideScope();

    DirectDrawPresentationOverrideScope(const DirectDrawPresentationOverrideScope&) = delete;
    DirectDrawPresentationOverrideScope& operator=(const DirectDrawPresentationOverrideScope&) = delete;

    void PrepareForCall();
    void Complete(HRESULT result);

private:
    static std::recursive_mutex& PresentationMutex();
    void Initialize(IUnknown* surfaceToUpgrade, DWORD& flags);
    void ReleaseExecutionOwnership();

    std::unique_lock<std::recursive_mutex> lock_;
    IDirectDrawSurface7* surface_ = nullptr;
    ce::ddraw_present_policy::PresentOperation operation_ =
        ce::ddraw_present_policy::PresentOperation::None;
    uint64_t generation_ = 0;
    int queueDepth_ = -1;
    bool ownsSurface_ = false;
    bool presentation_ = false;
    bool depthEntered_ = false;
    bool active_ = false;
    bool fifoBltPacing_ = false;
    bool applicationVblankWait_ = false;
    bool callPrepared_ = false;
    bool completed_ = false;
};

// Every DirectDraw interface generation keeps WaitForVerticalBlank at ABI slot
// 22. Intercepting application waits lets FIFO Blt pacing reuse a real wait
// instead of accidentally waiting for the following refresh as well.
void InstallDirectDrawWaitForVerticalBlankHook(IUnknown* directDraw, const char* reason);

// Returns how many application objects (queued presentation surfaces and the
// DirectDraw owner) CE stopped referencing, for the chain-boundary diagnostic.
uint32_t ResetDirectDrawPresentationOverrides();
