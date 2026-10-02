#include "opengl_hook_internal.h"

#include <iterator>

// Write-only sink for the IAT/dynamic-hook "original" output of a swap entry
// point that already routes through an inline trampoline.
static LPVOID opengl_hook_g_DiscardedSwapOriginal = nullptr;

struct OpenGLTrampolinePublication {
    void* volatile* destination = nullptr;
    void* fallback = nullptr;
};

static void PublishOpenGLSwapTrampoline(void* trampoline, void* context) {
    auto* publication = static_cast<OpenGLTrampolinePublication*>(context);
    InterlockedExchangePointer(publication->destination, trampoline ? trampoline : publication->fallback);
}

static void LogOpenGLSwapInlineHookResult(const char* moduleName, const char* functionName, void* target,
                                          void* trampoline, bool installed) {
    if (!installed) {
        HookLogImportant("OpenGL: Inline hook failed for %s!%s at %p; only IAT-routed callers are covered", moduleName,
                         functionName, target);
        return;
    }

    HookLogImportant("OpenGL: Inline hook installed for %s!%s at %p (trampoline=%p)", moduleName, functionName, target,
                     trampoline);
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
    void* swapTargets[] = {
        reinterpret_cast<void*>(GetProcAddress(gdi32Module, "SwapBuffers")),
        reinterpret_cast<void*>(GetProcAddress(glModule, "wglSwapBuffers")),
        reinterpret_cast<void*>(GetProcAddress(glModule, "wglSwapLayerBuffers")),
    };
    void* swapTrampolines[3] = {};
    OpenGLTrampolinePublication publications[] = {
        {reinterpret_cast<void* volatile*>(&opengl_hook_oSwapBuffers), (void*)opengl_hook_oSwapBuffers},
        {reinterpret_cast<void* volatile*>(&opengl_hook_oWglSwapBuffers), (void*)opengl_hook_oWglSwapBuffers},
        {reinterpret_cast<void* volatile*>(&opengl_hook_oWglSwapLayerBuffers),
         (void*)opengl_hook_oWglSwapLayerBuffers},
    };
    InlineHook::PublishedHookSpec inlineHooks[] = {
        {swapTargets[0], (void*)&DetourSwapBuffers, &swapTrampolines[0], &PublishOpenGLSwapTrampoline,
         &publications[0]},
        {swapTargets[1], (void*)&DetourWglSwapBuffers, &swapTrampolines[1], &PublishOpenGLSwapTrampoline,
         &publications[1]},
        {swapTargets[2], (void*)&DetourWglSwapLayerBuffers, &swapTrampolines[2], &PublishOpenGLSwapTrampoline,
         &publications[2]},
    };
    InlineHook::InstallPublishedBatch(inlineHooks, std::size(inlineHooks));
    const bool swapBuffersInline = inlineHooks[0].installed;
    const bool wglSwapBuffersInline = inlineHooks[1].installed;
    const bool wglSwapLayerBuffersInline = inlineHooks[2].installed;
    LogOpenGLSwapInlineHookResult("gdi32.dll", "SwapBuffers", swapTargets[0], swapTrampolines[0],
                                  swapBuffersInline);
    LogOpenGLSwapInlineHookResult("opengl32.dll", "wglSwapBuffers", swapTargets[1], swapTrampolines[1],
                                  wglSwapBuffersInline);
    LogOpenGLSwapInlineHookResult("opengl32.dll", "wglSwapLayerBuffers", swapTargets[2], swapTrampolines[2],
                                  wglSwapLayerBuffersInline);

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
    // GL objects must be destroyed with their owner context current. Defer the
    // generation reset to the first Swap on the next host rather than trying to
    // move a game-owned context onto HookThread.
    opengl_hook_g_HostGenerationResetPending.store(true, std::memory_order_release);
}
