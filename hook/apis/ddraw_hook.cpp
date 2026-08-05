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

void DDrawHook::Init() {
    HookLog("DDrawHook::Init()");

    if (ShouldSuppressDirectDrawHooking()) {
        HookLog("DDraw: Init suppressed because DXVK d3d9 Vulkan path is active");
        return;
    }

    // Skip DDraw hooks when a higher-level D3D API (d3d9, d3d8) is already loaded.
    // ddraw.dll is often loaded as a transitive system dependency even in DX9+ games,
    // and bootstrapping DDraw hooks (which internally creates a D3D9 device via
    // DirectDrawCreateEx -> Windows DDraw-on-D3D9 mapping) can crash when third-party
    // overlays (Steam, Discord, etc.) have already hooked Direct3DCreate9 and their
    // internal state is not prepared for a synthetic device creation on a worker thread.
    //
    // BioShockInfinite crash family (2026-04-30):
    //   gameoverlayrenderer!OverlayHookD3D3+0x8ba7: FF 50 50 (call [eax+0x50])
    //   Access violation reading vtable slot at 0x6284d010 from EAX=0x6284CFC0
    //   Triggered by DDraw bootstrap calling DirectDrawCreateEx on the hook thread
    //   while Steam overlay controls the D3D9 vtable.
    if (GetModuleHandleA("d3d9.dll") || GetModuleHandleA("d3d8.dll")) {
        HookLog("DDraw: Skipping DDraw hooks (higher-level D3D API present; d3d9=%d d3d8=%d)",
                GetModuleHandleA("d3d9.dll") ? 1 : 0, GetModuleHandleA("d3d8.dll") ? 1 : 0);
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
