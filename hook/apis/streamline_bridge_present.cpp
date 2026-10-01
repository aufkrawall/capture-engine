// Streamline is a 64-bit runtime; see streamline_bridge_translate.cpp.
#if defined(_M_X64) || defined(__x86_64__)

#include "streamline_bridge_present.h"

#include <dxgi1_2.h>

#include <atomic>
#include <mutex>

#include "../common/hook_common.h"
#include "../wrappers/inline_hook.h"
#include "streamline_bridge_dlssg.h"
#include "streamline_bridge_dlssg_gate.h"

namespace ce::streamline_bridge {
namespace {

// sl.common's present hooks as 2.x's interposer calls them before every proxy present
// (`commonInterface.cpp`: both run presentCommon, which counts the present).
using SlGetPluginFunctionFn = void*(const char* functionName);
using SlHookPresentFn = HRESULT(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags, bool& skip);
using SlHookPresent1Fn = HRESULT(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags,
                                 DXGI_PRESENT_PARAMETERS* params, bool& skip);

void* volatile g_originalHookPresent = nullptr;
void* volatile g_originalHookPresent1 = nullptr;

std::mutex g_ledgerMutex;
PresentMarkerLedger g_ledger;  // guarded by g_ledgerMutex

void PublishTrampoline(void* trampoline, void* context) {
    InterlockedExchangePointer(static_cast<void* volatile*>(context), trampoline);
}

// Runs on the title's present thread, ahead of the present 2.x is about to count.
void BeforeCountedPresent(UINT flags) {
    const bool dlssgEnabled = DlssgEnabledOnAnyViewport();
    uint32_t frameIndex = 0;
    bool needsMarker = false;
    {
        std::lock_guard<std::mutex> lock(g_ledgerMutex);
        needsMarker = g_ledger.PresentNeedsMarker(flags, dlssgEnabled, &frameIndex);
    }
    if (!needsMarker) {
        return;
    }
    const bool remarked = SynthesizePresentMarkersFor(frameIndex);
    static std::atomic<uint32_t> count{0};
    const uint32_t n = count.fetch_add(1, std::memory_order_relaxed);
    if (n < 32 || (n % 256) == 0) {
        HookLogImportant(
            "Streamline bridge: title presented without a Reflex PRESENT_START marker - %s frame %u so 2.x DLSS-G "
            "does not fail its Reflex check and skip this present (1.x had no such check) #%u",
            remarked ? "re-marked" : "could NOT re-mark (no frame token)", frameIndex, n + 1);
    }
}

HRESULT HookedSlHookPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags, bool& skip) {
    BeforeCountedPresent(flags);
    auto* original = reinterpret_cast<SlHookPresentFn*>(
        InterlockedCompareExchangePointer(&g_originalHookPresent, nullptr, nullptr));
    return original ? original(swapChain, syncInterval, flags, skip) : S_OK;
}

HRESULT HookedSlHookPresent1(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags,
                             DXGI_PRESENT_PARAMETERS* params, bool& skip) {
    BeforeCountedPresent(flags);
    auto* original = reinterpret_cast<SlHookPresent1Fn*>(
        InterlockedCompareExchangePointer(&g_originalHookPresent1, nullptr, nullptr));
    return original ? original(swapChain, syncInterval, flags, params, skip) : S_OK;
}

bool IsInModule(void* address, HMODULE module) {
    HMODULE owner = nullptr;
    return address &&
           GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              static_cast<LPCWSTR>(address), &owner) &&
           owner == module;
}

// The plugin publishes its hooks only through slGetPluginFunction, never as exports.
void* InstallOne(SlGetPluginFunctionFn* getPluginFunction, HMODULE v2Common, const char* name, void* detour,
                 void* volatile* original) {
    void* target = getPluginFunction(name);
    if (!IsInModule(target, v2Common)) {
        HookLogImportant("Streamline bridge: 2.x sl.common answered %s with %p, outside its image - not hooking it",
                         name, target);
        return nullptr;
    }
    void* trampoline = nullptr;
    if (!InlineHook::InstallPublished(target, detour, &trampoline, PublishTrampoline,
                                      const_cast<void**>(original))) {
        HookLogImportant("Streamline bridge: could not hook 2.x sl.common %s at %p", name, target);
        return nullptr;
    }
    return target;
}

}  // namespace

void NoteTitlePresentStart(uint32_t frameIndex) {
    std::lock_guard<std::mutex> lock(g_ledgerMutex);
    g_ledger.NoteTitlePresentStart(frameIndex);
}

bool InstallPresentMarkerGuard(HMODULE v2Common) {
    static std::atomic<bool> attempted{false};
    if (attempted.exchange(true, std::memory_order_acq_rel)) {
        return true;  // one attempt per process: a second would stack a hook on the first
    }
    auto* getPluginFunction =
        v2Common ? reinterpret_cast<SlGetPluginFunctionFn*>(GetProcAddress(v2Common, "slGetPluginFunction")) : nullptr;
    if (!IsInModule(reinterpret_cast<void*>(getPluginFunction), v2Common)) {
        HookLogImportant(
            "Streamline bridge: no 2.x sl.common slGetPluginFunction (module=%p) - presents the title leaves without a "
            "Reflex PRESENT_START marker will fail DLSS-G's Reflex check",
            v2Common);
        return false;
    }
    void* present = InstallOne(getPluginFunction, v2Common, "slHookPresent",
                               reinterpret_cast<void*>(&HookedSlHookPresent), &g_originalHookPresent);
    void* present1 = InstallOne(getPluginFunction, v2Common, "slHookPresent1",
                                reinterpret_cast<void*>(&HookedSlHookPresent1), &g_originalHookPresent1);
    HookLogImportant(
        "Streamline bridge: present-marker guard on 2.x sl.common - slHookPresent=%p slHookPresent1=%p (a title "
        "present without its own Reflex PRESENT_START re-marks the last presented frame while DLSS-G generates)",
        present, present1);
    return present != nullptr;
}

}  // namespace ce::streamline_bridge

#endif  // x64
