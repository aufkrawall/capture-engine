#include "screen_grab_privacy.h"

#include <d3d11_3.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <algorithm>
#include <cctype>
#include <string>
#include "common/platform/process_identity.h"
#include "common/platform/runtime_process_identity.h"

namespace ce::screen_grab_privacy {

namespace {

bool IsYuvBlackFormat(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_NV12 || format == DXGI_FORMAT_P010;
}

bool IsSupportedBlackFormat(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_R10G10B10A2_UNORM ||
           format == DXGI_FORMAT_R16G16B16A16_FLOAT || IsYuvBlackFormat(format);
}

// Clears each plane through its own view: Y to the black code, Cb/Cr to neutral.
// PQ black (0 nits) has the same codes as SDR black.
HRESULT ClearYuvBlack(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                      DXGI_FORMAT format, bool fullRange) {
    Microsoft::WRL::ComPtr<ID3D11Device3> device3;
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(device3.GetAddressOf()));
    if (FAILED(hr))
        return hr;
    const bool eightBit = format == DXGI_FORMAT_NV12;
    const float codeScale = eightBit ? 1.0f / 255.0f : 64.0f / 65535.0f;
    const float yCode = fullRange ? 0.0f : (eightBit ? 16.0f : 64.0f);
    const float cCode = eightBit ? 128.0f : 512.0f;
    const FLOAT luma[4] = {yCode * codeScale, 0.0f, 0.0f, 0.0f};
    const FLOAT chroma[4] = {cCode * codeScale, cCode * codeScale, 0.0f, 0.0f};
    for (UINT plane = 0; plane < 2 && SUCCEEDED(hr); ++plane) {
        D3D11_RENDER_TARGET_VIEW_DESC1 desc = {};
        desc.Format = plane == 0 ? (eightBit ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R16_UNORM)
                                 : (eightBit ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R16G16_UNORM);
        desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        desc.Texture2D.PlaneSlice = plane;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView1> view;
        hr = device3->CreateRenderTargetView1(texture, &desc, view.GetAddressOf());
        if (SUCCEEDED(hr))
            context->ClearRenderTargetView(view.Get(), plane == 0 ? luma : chroma);
    }
    return hr;
}

}  // namespace

bool IsWindowCloaked(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        return false;
    }

    HWND root = NormalizeRootWindow(hwnd);
    if (!root) {
        root = hwnd;
    }

    typedef HRESULT(WINAPI* PFN_DwmGetWindowAttribute)(HWND, DWORD, PVOID, DWORD);
    static const auto pfnDwmGetWindowAttribute = []() -> PFN_DwmGetWindowAttribute {
        HMODULE hDwm = GetModuleHandleW(L"dwmapi.dll");
        if (!hDwm) {
            hDwm = LoadLibraryW(L"dwmapi.dll");
        }
        return hDwm ? reinterpret_cast<PFN_DwmGetWindowAttribute>(GetProcAddress(hDwm, "DwmGetWindowAttribute"))
                    : nullptr;
    }();

    if (!pfnDwmGetWindowAttribute) {
        return false;
    }

    DWORD cloaked = 0;
    // DWMWA_CLOAKED = 14
    const HRESULT hr = pfnDwmGetWindowAttribute(root, 14, &cloaked, sizeof(cloaked));
    return SUCCEEDED(hr) && cloaked != 0;
}

bool IsWindowOnCurrentVirtualDesktop(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        return false;
    }
    if (IsWindowCloaked(hwnd)) {
        return false;
    }

    HWND root = NormalizeRootWindow(hwnd);
    if (!root) {
        root = hwnd;
    }

    static const GUID clsidVirtualDesktopManager = {
        0xaa509086, 0x5ca9, 0x4c25, {0x8f, 0x95, 0x58, 0x9d, 0x3c, 0x07, 0xb4, 0x8a}};
    static const GUID iidVirtualDesktopManager = {
        0xa5cd92ff, 0x29be, 0x454c, {0x8d, 0x04, 0xd8, 0x28, 0x79, 0xfb, 0x3f, 0x1b}};

    // The manager is created and released inside this call and is never cached
    // across calls.
    //
    // A COM interface pointer is only valid while the apartment that created it
    // lives, and this thread's apartment is not CaptureEngine's to rely on:
    // USER32's text-services hook calls CoInitialize/CoUninitialize of its own
    // accord while ordinary window messages are processed, and the balancing
    // CoUninitialize takes the whole process's COM state down with it -
    // `CClassCache::CleanUpDllsForProcess` calls FreeLibrary on every in-process
    // server. A pointer cached across that boundary then has a vtable in
    // unmapped memory, and the next call through it faults reading the vtable
    // slot rather than anywhere inside COM. That was a reproducible 0xC0000005,
    // and it would equally hit the media process's privacy sampling, which
    // queries this once per captured frame.
    //
    // Creating per call keeps the server alive for exactly as long as the
    // reference is held. The cost is a warm class-cache lookup, well under the
    // DWM round trip `IsWindowCloaked` already made above.
    Microsoft::WRL::ComPtr<IVirtualDesktopManager> vdm;
    const HRESULT hr = CoCreateInstance(clsidVirtualDesktopManager, nullptr, CLSCTX_INPROC_SERVER,
                                        iidVirtualDesktopManager, reinterpret_cast<void**>(vdm.GetAddressOf()));
    // No COM on this thread, or no virtual desktop manager: the window cannot be
    // proven to be on another desktop, so it is treated as visible on this one.
    // A later call recovers on its own once the thread initializes COM, which is
    // why there is no "already tried and failed" latch either.
    if (FAILED(hr) || !vdm) {
        return true;
    }

    BOOL onCurrent = TRUE;
    if (SUCCEEDED(vdm->IsWindowOnCurrentVirtualDesktop(root, &onCurrent)) && !onCurrent) {
        return false;
    }

    return true;
}

bool IsIgnoredShellWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        return false;
    }

    HWND root = NormalizeRootWindow(hwnd);
    if (!root) {
        root = hwnd;
    }

    char className[128] = {};
    if (GetClassNameA(root, className, static_cast<int>(sizeof(className))) > 0) {
        std::string lowerClass = className;
        for (char& c : lowerClass) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (lowerClass == "progman" || lowerClass == "workerw" || lowerClass == "shell_traywnd" ||
            lowerClass == "shell_secondarytraywnd" || lowerClass == "xamlexplorerhostislandwindow" ||
            lowerClass == "tasklistthumbnailwnd") {
            return true;
        }
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(root, &pid);
    if (pid != 0) {
        // CE's own windows (any helper role, and the host's when the host is the CE application) belong to the
        // runtime by process identity, whatever the executable is called: a fullscreen-sized CE window must never
        // pass for the user's fullscreen application. An embedding client is the host too, but its own windows
        // are not CE's: a client recording its own window must not be blacked out, so they are not matched here.
        if (ce::runtime_identity::OwnsRuntimeWindows(pid)) {
            return true;
        }
        const ce::process::ProcessIdentityResult identity = ce::process::QueryProcessIdentity(pid);
        if (identity) {
            std::string lowerName = identity.imageName;
            for (char& c : lowerName) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (lowerName == "explorer.exe" || lowerName == "xamlexplorerhost.exe" ||
                lowerName == "shellexperiencehost.exe" || lowerName == "startmenuexperiencehost.exe" ||
                lowerName == "searchhost.exe" || lowerName == "textinputhost.exe" ||
                // The CaptureEngine app by name, for CE windows the runtime identity above cannot claim: a
                // second instance's message box, the setup UI, another session's app.
                lowerName == "captureengine.exe") {
                return true;
            }
        }
    }

    return false;
}

namespace {

bool TryClassifyWindowFullscreenLike(HWND hwnd, HMONITOR* classifiedMonitor, bool* fullscreenLike) {
    if (classifiedMonitor) {
        *classifiedMonitor = nullptr;
    }
    if (fullscreenLike) {
        *fullscreenLike = false;
    }
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd) || IsWindowCloaked(hwnd) ||
        !IsWindowOnCurrentVirtualDesktop(hwnd) || IsIgnoredShellWindow(hwnd)) {
        return false;
    }

    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo = {};
    monitorInfo.cbSize = sizeof(MONITORINFO);
    if (!monitor || !GetMonitorInfo(monitor, &monitorInfo)) {
        return false;
    }

    RECT windowRect = {};
    RECT clientRect = {};
    const bool haveWindowRect = GetWindowRect(hwnd, &windowRect) != FALSE;
    const bool haveClientRect = GetWindowClientRectInScreen(hwnd, clientRect);
    if (!haveWindowRect && !haveClientRect) {
        return false;
    }
    if (classifiedMonitor) {
        *classifiedMonitor = monitor;
    }
    if (fullscreenLike) {
        *fullscreenLike =
            IsFullscreenGeometry(haveWindowRect ? &windowRect : nullptr, haveClientRect ? &clientRect : nullptr,
                                 monitorInfo.rcMonitor);
    }
    return true;
}

}  // namespace

bool RectNearlyMatches(const RECT& lhs, const RECT& rhs, LONG tolerance) {
    const auto absDiff = [](LONG a, LONG b) -> LONG { return (a >= b) ? (a - b) : (b - a); };
    return absDiff(lhs.left, rhs.left) <= tolerance && absDiff(lhs.top, rhs.top) <= tolerance &&
           absDiff(lhs.right, rhs.right) <= tolerance && absDiff(lhs.bottom, rhs.bottom) <= tolerance;
}

bool IsFullscreenGeometry(const RECT* windowRect, const RECT* clientRect, const RECT& monitorRect, LONG tolerance) {
    return (windowRect && RectNearlyMatches(*windowRect, monitorRect, tolerance)) ||
           (clientRect && RectNearlyMatches(*clientRect, monitorRect, tolerance));
}

bool GetWindowClientRectInScreen(HWND hwnd, RECT& rect) {
    RECT clientRect = {};
    if (!GetClientRect(hwnd, &clientRect)) {
        return false;
    }

    POINT topLeft = {clientRect.left, clientRect.top};
    POINT bottomRight = {clientRect.right, clientRect.bottom};
    if (!ClientToScreen(hwnd, &topLeft) || !ClientToScreen(hwnd, &bottomRight)) {
        return false;
    }

    rect = {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
    return true;
}

HWND NormalizeRootWindow(HWND hwnd) {
    if (!hwnd) {
        return nullptr;
    }
    const HWND root = GetAncestor(hwnd, GA_ROOT);
    return root ? root : hwnd;
}

bool IsWindowFullscreenLike(HWND hwnd) {
    bool fullscreenLike = false;
    return TryClassifyWindowFullscreenLike(hwnd, nullptr, &fullscreenLike) && fullscreenLike;
}

FullscreenFocusSnapshot CaptureStableFullscreenFocus() {
    FullscreenFocusSnapshot snapshot;
    const HWND firstForeground = NormalizeRootWindow(GetForegroundWindow());
    if (!firstForeground) {
        return snapshot;
    }

    HMONITOR monitor = nullptr;
    bool fullscreenLike = false;
    const bool classificationReliable = TryClassifyWindowFullscreenLike(firstForeground, &monitor, &fullscreenLike);
    const HWND secondForeground = NormalizeRootWindow(GetForegroundWindow());
    snapshot.foregroundRoot = firstForeground;
    snapshot.monitor = monitor;
    snapshot.stable = firstForeground == secondForeground && monitor != nullptr;
    snapshot.fullscreenLike = snapshot.stable && fullscreenLike;
    snapshot.classificationReliable = snapshot.stable && classificationReliable;
    return snapshot;
}

bool IsCaptureTargetValid(HWND targetWindow, HMONITOR targetMonitor) {
    if ((targetWindow != nullptr) == (targetMonitor != nullptr)) {
        return false;
    }
    if (targetWindow) {
        return IsWindow(NormalizeRootWindow(targetWindow)) != FALSE;
    }
    MONITORINFO monitorInfo = {};
    monitorInfo.cbSize = sizeof(MONITORINFO);
    return GetMonitorInfo(targetMonitor, &monitorInfo) != FALSE;
}

bool SnapshotMatchesCaptureTarget(const FullscreenFocusSnapshot& snapshot, HWND targetWindow, HMONITOR targetMonitor) {
    if (!snapshot.stable || !snapshot.classificationReliable || !snapshot.fullscreenLike ||
        !snapshot.foregroundRoot || !snapshot.monitor || (targetWindow != nullptr) == (targetMonitor != nullptr)) {
        return false;
    }
    if (targetWindow) {
        return snapshot.foregroundRoot == NormalizeRootWindow(targetWindow);
    }
    return targetMonitor && snapshot.monitor == targetMonitor;
}

void FocusPrivacyGate::Reset(bool enabled) {
    enabled_ = enabled;
    focusAccepted_ = false;
    blackoutActive_ = false;
    waitingForSafeFrame_ = enabled;
    lastOutputWasBlack_ = false;
    safeFrameThresholdQpc_ = 0;
    blackoutEntries_ = 0;
    blackFrames_ = 0;
    ambiguousObservations_ = 0;
    resumeWaitMaxQpc_ = 0;
}

void FocusPrivacyGate::ResetTarget() {
    focusAccepted_ = false;
    blackoutActive_ = false;
    waitingForSafeFrame_ = enabled_;
    lastOutputWasBlack_ = false;
    safeFrameThresholdQpc_ = 0;
}

void FocusPrivacyGate::UpdateFocusTracking(bool reliableFocusObservation, bool matchingFullscreenFocus,
                                           int64_t observationQpc, GateDecision& decision) {
    if (!reliableFocusObservation || !matchingFullscreenFocus) {
        if (!reliableFocusObservation) {
            ++ambiguousObservations_;
        }
        if (!blackoutActive_) {
            ++blackoutEntries_;
            decision.enteredBlackout = true;
        }
        focusAccepted_ = false;
        blackoutActive_ = true;
        waitingForSafeFrame_ = true;
        safeFrameThresholdQpc_ = 0;
        return;
    }

    if (!focusAccepted_) {
        focusAccepted_ = true;
        if (!blackoutActive_) {
            ++blackoutEntries_;
            decision.enteredBlackout = true;
        }
        blackoutActive_ = true;
        waitingForSafeFrame_ = true;
        safeFrameThresholdQpc_ = std::max<int64_t>(1, observationQpc);
        decision.focusReacquired = true;
    }
}

GateDecision FocusPrivacyGate::Observe(bool activeScreenGrab, bool reliableFocusObservation,
                                       bool matchingFullscreenFocus, int64_t observationQpc) {
    GateDecision decision;
    if (!enabled_ || !activeScreenGrab) {
        return decision;
    }

    UpdateFocusTracking(reliableFocusObservation, matchingFullscreenFocus, observationQpc, decision);
    decision.waitingForSafeFrame = waitingForSafeFrame_;
    return decision;
}

GateDecision FocusPrivacyGate::Evaluate(bool activeScreenGrab, bool reliableFocusObservation,
                                        bool matchingFullscreenFocus, int64_t observationQpc, bool hasFreshFrame,
                                        int64_t freshFrameQpc) {
    GateDecision decision;
    if (!enabled_ || !activeScreenGrab) {
        decision.useBlackFrame = false;
        return decision;
    }

    UpdateFocusTracking(reliableFocusObservation, matchingFullscreenFocus, observationQpc, decision);

    if (!focusAccepted_) {
        decision.useBlackFrame = true;
        decision.waitingForSafeFrame = true;
        return decision;
    }

    if (waitingForSafeFrame_) {
        if (!hasFreshFrame || freshFrameQpc < safeFrameThresholdQpc_) {
            decision.useBlackFrame = true;
            decision.waitingForSafeFrame = true;
            return decision;
        }
        const int64_t resumeWaitQpc = observationQpc - safeFrameThresholdQpc_;
        resumeWaitMaxQpc_ =
            std::max<uint64_t>(resumeWaitMaxQpc_, static_cast<uint64_t>(std::max<int64_t>(0, resumeWaitQpc)));
        waitingForSafeFrame_ = false;
        blackoutActive_ = false;
        decision.exitedBlackout = true;
    }

    decision.useBlackFrame = false;
    return decision;
}

void FocusPrivacyGate::CommitOutput(bool blackFrame) {
    lastOutputWasBlack_ = blackFrame;
    if (blackFrame) {
        ++blackFrames_;
    }
}

BlackFrameTextureCache::~BlackFrameTextureCache() {
    Reset();
}

bool BlackFrameTextureCache::Prepare(ID3D11Texture2D* referenceTexture) {
    if (!referenceTexture) {
        return false;
    }

    D3D11_TEXTURE2D_DESC referenceDesc = {};
    referenceTexture->GetDesc(&referenceDesc);
    ID3D11Device* referenceDevice = nullptr;
    referenceTexture->GetDevice(&referenceDevice);
    if (!referenceDevice) {
        return false;
    }

    const bool cacheMatches = device_ == referenceDevice && texture_ && width_ == referenceDesc.Width &&
                              height_ == referenceDesc.Height && format_ == referenceDesc.Format;
    if (cacheMatches) {
        referenceDevice->Release();
        return true;
    }
    if (!IsSupportedBlackFormat(referenceDesc.Format) || referenceDesc.Width == 0 || referenceDesc.Height == 0) {
        referenceDevice->Release();
        return false;
    }

    Reset();
    D3D11_TEXTURE2D_DESC blackDesc = {};
    blackDesc.Width = referenceDesc.Width;
    blackDesc.Height = referenceDesc.Height;
    blackDesc.MipLevels = 1;
    blackDesc.ArraySize = 1;
    blackDesc.Format = referenceDesc.Format;
    blackDesc.SampleDesc.Count = 1;
    blackDesc.Usage = D3D11_USAGE_DEFAULT;
    blackDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    ID3D11Texture2D* blackTexture = nullptr;
    ID3D11RenderTargetView* blackRtv = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HRESULT hr = referenceDevice->CreateTexture2D(&blackDesc, nullptr, &blackTexture);
    const bool yuvBlack = IsYuvBlackFormat(referenceDesc.Format);
    if (SUCCEEDED(hr) && !yuvBlack) {
        hr = referenceDevice->CreateRenderTargetView(blackTexture, nullptr, &blackRtv);
    }
    if (SUCCEEDED(hr)) {
        referenceDevice->GetImmediateContext(&context);
        if (!context) {
            hr = E_FAIL;
        }
    }
    if (SUCCEEDED(hr) && yuvBlack) {
        hr = ClearYuvBlack(referenceDevice, context, blackTexture, referenceDesc.Format, yuvFullRange_);
    }
    if (SUCCEEDED(hr)) {
        if (!yuvBlack) {
            const FLOAT opaqueBlack[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            context->ClearRenderTargetView(blackRtv, opaqueBlack);
        }
        device_ = referenceDevice;
        texture_ = blackTexture;
        width_ = blackDesc.Width;
        height_ = blackDesc.Height;
        format_ = blackDesc.Format;
        ++generation_;
    } else {
        referenceDevice->Release();
        if (blackTexture) {
            blackTexture->Release();
        }
    }
    if (context) {
        context->Release();
    }
    if (blackRtv) {
        blackRtv->Release();
    }
    return SUCCEEDED(hr);
}

void BlackFrameTextureCache::Reset() {
    if (texture_) {
        texture_->Release();
        texture_ = nullptr;
    }
    if (device_) {
        device_->Release();
        device_ = nullptr;
    }
    width_ = 0;
    height_ = 0;
    format_ = DXGI_FORMAT_UNKNOWN;
}

}  // namespace ce::screen_grab_privacy
