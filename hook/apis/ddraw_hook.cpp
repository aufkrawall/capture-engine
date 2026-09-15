#include "ddraw_hook_internal.h"

static int64_t g_LastSleepUs = 0;

static HHOOK g_DDrawBootstrapHook = nullptr;

static DWORD g_DDrawBootstrapThreadId = 0;

static std::atomic<bool> g_DDrawBootstrapQueued{false};

static std::atomic<bool> g_DDrawBootstrapRunning{false};

static bool QueueDirectDrawBootstrapOnWindowThread();

static bool FindDirectDrawBootstrapWindow(HWND* outWindow, DWORD* outThreadId) {
    if (!outWindow || !outThreadId)
        return false;

    *outWindow = NULL;
    *outThreadId = 0;

    HWND foregroundWindow = GetForegroundWindow();
    DWORD foregroundPid = 0;
    DWORD foregroundThreadId = 0;
    if (foregroundWindow) {
        foregroundThreadId = GetWindowThreadProcessId(foregroundWindow, &foregroundPid);
        if (foregroundPid == GetCurrentProcessId()) {
            *outWindow = foregroundWindow;
            *outThreadId = foregroundThreadId;
            return true;
        }
    }

    ce::overlay_compat::AuxiliaryProcessWindowInfo info = {};
    if (ce::overlay_compat::FindAuxiliaryProcessWindow(GetCurrentProcessId(), nullptr, &info) && info.hwnd &&
        info.threadId != 0) {
        *outWindow = info.hwnd;
        *outThreadId = info.threadId;
        return true;
    }

    return false;
}

static LRESULT CALLBACK DirectDrawBootstrapHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code >= 0 && g_DDrawBootstrapQueued.load(std::memory_order_acquire) &&
        !g_DDrawBootstrapRunning.exchange(true, std::memory_order_acq_rel)) {
        HHOOK hook = g_DDrawBootstrapHook;
        g_DDrawBootstrapHook = nullptr;
        g_DDrawBootstrapQueued.store(false, std::memory_order_release);
        if (hook) {
            UnhookWindowsHookEx(hook);
        }

        BootstrapDirectDrawHooksOnCurrentThread("window-thread bootstrap");
        g_DDrawBootstrapRunning.store(false, std::memory_order_release);
    }

    return CallNextHookEx(g_DDrawBootstrapHook, code, wParam, lParam);
}

static bool QueueDirectDrawBootstrapOnWindowThread() {
    if (ddraw_hook_g_HooksInitialized)
        return true;

    HWND bootstrapWindow = NULL;
    DWORD bootstrapThreadId = 0;
    if (!FindDirectDrawBootstrapWindow(&bootstrapWindow, &bootstrapThreadId) || !bootstrapWindow ||
        bootstrapThreadId == 0) {
        HookLog("DDraw: Failed to find bootstrap window thread");
        return false;
    }

    if (g_DDrawBootstrapHook) {
        HookLog("DDraw: Bootstrap window hook already queued (hwnd=%p, tid=%lu)", bootstrapWindow,
                (unsigned long)bootstrapThreadId);
        return true;
    }

    ddraw_hook_g_DDrawBootstrapWindow = bootstrapWindow;
    g_DDrawBootstrapThreadId = bootstrapThreadId;
    g_DDrawBootstrapHook = SetWindowsHookExA(WH_CALLWNDPROC, DirectDrawBootstrapHookProc, NULL, bootstrapThreadId);
    if (!g_DDrawBootstrapHook) {
        HookLog("DDraw: Failed to install bootstrap window hook (hwnd=%p, tid=%lu, err=%lu)", bootstrapWindow,
                (unsigned long)bootstrapThreadId, GetLastError());
        return false;
    }

    g_DDrawBootstrapQueued.store(true, std::memory_order_release);
    HookLog("DDraw: Queued bootstrap window hook (hwnd=%p, tid=%lu)", bootstrapWindow,
            (unsigned long)bootstrapThreadId);

    DWORD_PTR sendResult = 0;
    if (!SendMessageTimeoutA(bootstrapWindow, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 1000, &sendResult)) {
        HookLog("DDraw: Failed to send bootstrap wake message (hwnd=%p, tid=%lu, err=%lu)", bootstrapWindow,
                (unsigned long)bootstrapThreadId, GetLastError());
    }

    return true;
}

bool WasDirectDrawInterfaceObserved() {
    // Synthetic bootstrap deliberately does not publish an active API. Keeping
    // this proof atomic also avoids racing a presentation thread's first real
    // surface activation against the hook-management thread.
    return ddraw_hook_g_ActiveDirectDrawVersion.load(std::memory_order_acquire) != 0;
}

void DDrawHook::Init() {
    HookLog("DDrawHook::Init()");

    if (ShouldSuppressDirectDrawHooking()) {
        HookLog("DDraw: Init suppressed because DXVK d3d9 Vulkan path is active");
        return;
    }

    // Check if ddraw.dll is loaded
    HMODULE ddrawModule = GetModuleHandleA("ddraw.dll");
    if (!ddrawModule) {
        return;
    }

    DirectDrawCreateEx_t pDirectDrawCreateEx = (DirectDrawCreateEx_t)GetProcAddress(ddrawModule, "DirectDrawCreateEx");
    if (!pDirectDrawCreateEx) {
        HookLog("DDraw: DirectDrawCreateEx not found");
        return;
    }

    DirectDrawCreate_t pDirectDrawCreate = (DirectDrawCreate_t)GetProcAddress(ddrawModule, "DirectDrawCreate");
    InstallDirectDrawCreateInlineHook(pDirectDrawCreate);
    InstallDirectDrawCreateExInlineHook(pDirectDrawCreateEx);

    // Export interception is safe and must stay installed even when synthetic
    // bootstrap is not: a real later DirectDrawCreate call supplies its object
    // directly to HookDirectDrawObject. Only the bootstrap creates a synthetic
    // DirectDraw/D3D device and can therefore enter a co-resident overlay at an
    // unexpected point. This is the BioShock Infinite crash family from
    // 2026-04-30. Conversely, Gothic II session 20260914_195422 proves that a
    // loaded d3d9.dll alone cannot suppress the export hooks.
    const bool higherLevelDeviceCreated = WasGameD3D9DeviceCreated();
    const bool higherLevelModuleLoaded =
        GetModuleHandleA("d3d9.dll") != nullptr || GetModuleHandleA("d3d8.dll") != nullptr;
    const bool directDrawEvidence = WasDirectDrawInterfaceObserved();
    if (ce::ddraw_present_policy::ShouldSkipDirectDrawBootstrap(higherLevelDeviceCreated, higherLevelModuleLoaded,
                                                                directDrawEvidence)) {
        HookLog("DDraw: Export hooks active; deferring synthetic bootstrap "
                "(higherLevelDevice=%d d3d9=%d d3d8=%d directDrawEvidence=%d)",
                higherLevelDeviceCreated ? 1 : 0, GetModuleHandleA("d3d9.dll") ? 1 : 0,
                GetModuleHandleA("d3d8.dll") ? 1 : 0, directDrawEvidence ? 1 : 0);
        return;
    }

    if (!QueueDirectDrawBootstrapOnWindowThread()) {
        HookLog("DDraw: Falling back to hook-thread bootstrap");
        if (!ddraw_hook_g_HooksInitialized) {
            BootstrapDirectDrawHooksOnCurrentThread("hook-thread bootstrap");
        }
    } else if (!ddraw_hook_g_HooksInitialized) {
        HookLog("DDraw: Awaiting queued window-thread bootstrap callback");
    }
}

void DDrawHook::Shutdown() {
    HookLog("DDrawHook::Shutdown()");
    ce::legacy_d3d_sampler_state::LogSummary(ce::legacy_d3d_sampler_state::Api::D3D6);
    ce::legacy_d3d_sampler_state::LogSummary(ce::legacy_d3d_sampler_state::Api::D3D7);

    if (g_OverlayAdapter.IsInitialized()) {
        g_OverlayAdapter.Shutdown();
    }

    ddraw_hook_g_DDrawCapture.CleanupDDraw(true);
}

void DDrawHook::OnHostDisconnect() {
    HookLog("DDrawHook::OnHostDisconnect()");
    ddraw_hook_g_DDrawCapture.CleanupDDraw(true);
}
