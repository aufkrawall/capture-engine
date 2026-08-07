#include "opengl_hook_internal.h"

// Write-only sink for the IAT/dynamic-hook "original" output of a swap entry
// point that already routes through an inline trampoline.
static LPVOID opengl_hook_g_DiscardedSwapOriginal = nullptr;

static void PublishOpenGLSwapTrampoline(void* trampoline, void* context) {
    *static_cast<void**>(context) = trampoline;
}

// Patch the exported swap function itself so callers that cached the import
// address reach the detour as well. The published trampoline becomes the
// "original", so calling it never re-enters the detour.
static bool InstallOpenGLSwapInlineHook(const char* moduleName, const char* functionName, void* detour,
                                        void** original) {
    HMODULE module = GetModuleHandleA(moduleName);
    if (!module)
        return false;

    void* target = reinterpret_cast<void*>(GetProcAddress(module, functionName));
    if (!target || target == detour)
        return false;

    void* trampoline = nullptr;
    if (!InlineHook::InstallPublished(target, detour, &trampoline, &PublishOpenGLSwapTrampoline,
                                      static_cast<void*>(original))) {
        HookLogImportant("OpenGL: Inline hook failed for %s!%s at %p; only IAT-routed callers are covered", moduleName,
                         functionName, target);
        return false;
    }

    HookLogImportant("OpenGL: Inline hook installed for %s!%s at %p (trampoline=%p)", moduleName, functionName, target,
                     trampoline);
    return true;
}

void OpenGLHook::Init() {
    HookLog("OpenGLHook::Init()");

    // Check if opengl32.dll is loaded
    HMODULE glModule = GetModuleHandleA("opengl32.dll");
    if (!glModule) {
        return;
    }

    HMODULE gdi32Module = GetModuleHandleA("gdi32.dll");
    if (!gdi32Module) {
        return;
    }

    // Seed the originals from the untouched exports before anything is patched,
    // so a detour that goes live between the patch and the "original" write-back
    // still has a callable target.
    if (!opengl_hook_oSwapBuffers)
        opengl_hook_oSwapBuffers = (SwapBuffers_t)GetProcAddress(gdi32Module, "SwapBuffers");
    if (!opengl_hook_oWglSwapBuffers)
        opengl_hook_oWglSwapBuffers = (wglSwapBuffers_t)GetProcAddress(glModule, "wglSwapBuffers");
    if (!opengl_hook_oWglSwapLayerBuffers)
        opengl_hook_oWglSwapLayerBuffers = (wglSwapLayerBuffers_t)GetProcAddress(glModule, "wglSwapLayerBuffers");

    // Swap entry points get an inline hook first. IAT patching alone is not
    // enough: an optimizing compiler may load __imp_SwapBuffers once and keep it
    // in a register for the whole render loop, so a patch installed after
    // injection is never observed and overlay, capture, FPS limiter and perf
    // logging all stay silently dead (opengl_test.exe caches it in r13, which is
    // exactly why its overlay never appeared while opengl_legacy_test.exe worked).
    // Patching the export itself catches those cached pointers too.
    const bool swapBuffersInline = InstallOpenGLSwapInlineHook("gdi32.dll", "SwapBuffers", (void*)&DetourSwapBuffers,
                                                               (void**)&opengl_hook_oSwapBuffers);
    const bool wglSwapBuffersInline =
        InstallOpenGLSwapInlineHook("opengl32.dll", "wglSwapBuffers", (void*)&DetourWglSwapBuffers,
                                    (void**)&opengl_hook_oWglSwapBuffers);
    const bool wglSwapLayerBuffersInline =
        InstallOpenGLSwapInlineHook("opengl32.dll", "wglSwapLayerBuffers", (void*)&DetourWglSwapLayerBuffers,
                                    (void**)&opengl_hook_oWglSwapLayerBuffers);

    // Where an inline trampoline is live, the IAT/dynamic routes must not write
    // the raw export back over it - the detour would then call itself forever.
    LPVOID* const swapBuffersOriginal =
        swapBuffersInline ? &opengl_hook_g_DiscardedSwapOriginal : (LPVOID*)&opengl_hook_oSwapBuffers;
    LPVOID* const wglSwapBuffersOriginal =
        wglSwapBuffersInline ? &opengl_hook_g_DiscardedSwapOriginal : (LPVOID*)&opengl_hook_oWglSwapBuffers;
    LPVOID* const wglSwapLayerBuffersOriginal =
        wglSwapLayerBuffersInline ? &opengl_hook_g_DiscardedSwapOriginal
                                  : (LPVOID*)&opengl_hook_oWglSwapLayerBuffers;

    // Hook SwapBuffers (GDI32)
    // Register for dynamic loading via GetProcAddress
    IATHook::RegisterDynamicHook("SwapBuffers", (LPVOID)&DetourSwapBuffers, swapBuffersOriginal);
    // Patch explicit imports
    IATHook::PatchIATAllModules("gdi32.dll", "SwapBuffers", (LPVOID)&DetourSwapBuffers, swapBuffersOriginal);

    // Hook wglSwapBuffers
    IATHook::RegisterDynamicHook("wglSwapBuffers", (LPVOID)&DetourWglSwapBuffers, wglSwapBuffersOriginal);
    IATHook::PatchIATAllModules("opengl32.dll", "wglSwapBuffers", (LPVOID)&DetourWglSwapBuffers,
                                wglSwapBuffersOriginal);

    // Hook wglSwapLayerBuffers
    IATHook::RegisterDynamicHook("wglSwapLayerBuffers", (LPVOID)&DetourWglSwapLayerBuffers,
                                 wglSwapLayerBuffersOriginal);
    IATHook::PatchIATAllModules("opengl32.dll", "wglSwapLayerBuffers", (LPVOID)&DetourWglSwapLayerBuffers,
                                wglSwapLayerBuffersOriginal);

    // Hook wglDeleteContext
    IATHook::RegisterDynamicHook("wglDeleteContext", (LPVOID)&DetourWglDeleteContext, (LPVOID*)&opengl_hook_oWglDeleteContext);
    IATHook::PatchIATAllModules("opengl32.dll", "wglDeleteContext", (LPVOID)&DetourWglDeleteContext,
                                (LPVOID*)&opengl_hook_oWglDeleteContext);

    // Hook wglGetProcAddress
    // Critical for intercepting extensions
    IATHook::RegisterDynamicHook("wglGetProcAddress", (LPVOID)&DetourWglGetProcAddress, (LPVOID*)&opengl_hook_oWglGetProcAddress);
    IATHook::PatchIATAllModules("opengl32.dll", "wglGetProcAddress", (LPVOID)&DetourWglGetProcAddress,
                                (LPVOID*)&opengl_hook_oWglGetProcAddress);

    ce::opengl_sampler_override::Initialize();

    // Hook wglMakeCurrent
    IATHook::RegisterDynamicHook("wglMakeCurrent", (LPVOID)&DetourWglMakeCurrent, (LPVOID*)&opengl_hook_oWglMakeCurrent);
    IATHook::PatchIATAllModules("opengl32.dll", "wglMakeCurrent", (LPVOID)&DetourWglMakeCurrent,
                                (LPVOID*)&opengl_hook_oWglMakeCurrent);

    opengl_hook_g_HooksInitialized = true;
    HookLog("OpenGLHook: All hooks registered (IAT/Dynamic)");
}

void OpenGLHook::Shutdown() {
    HookLog("OpenGLHook::Shutdown()");
    ce::opengl_sampler_override::Shutdown();
    ResetTrackedOpenGLState(NULL);

    {
        std::lock_guard<std::mutex> lock(opengl_hook_g_PrerenderMutex);
        const HGLRC current = wglGetCurrentContext();
        if (opengl_hook_pglDeleteSync && current) {
            auto it = opengl_hook_g_PrerenderStates.find(current);
            if (it != opengl_hook_g_PrerenderStates.end()) {
                for (GLsync sync : it->second.syncs) {
                    if (sync)
                        opengl_hook_pglDeleteSync(sync);
                }
            }
        }
        opengl_hook_g_PrerenderStates.clear();
    }
    // IAT hooks remain until process exit
}

void OpenGLHook::OnHostDisconnect() {
    HookLog("OpenGLHook::OnHostDisconnect()");
    ResetTrackedOpenGLState(NULL);
}
