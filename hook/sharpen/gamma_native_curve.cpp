#include "gamma_native_curve.h"

#include <windows.h>
#include <atomic>

#include "common/graphics/gamma_policy.h"

namespace {
std::atomic<float> g_verifiedCurve{-1.0f};
using CurveReader = float (*)();
std::atomic<CurveReader> g_hookCurveReader{nullptr};
}

// The Vulkan layer and inject DLL have separate globals. This small read-only
// bridge lets the layer see the injector's confirmed native output curve too.
extern "C" __declspec(dllexport) float CE_GetVerifiedDisplayGamma() {
    return g_verifiedCurve.load(std::memory_order_acquire);
}

namespace ce::gamma {

void PublishVerifiedNativeCurve(float exponent) {
    g_verifiedCurve.store(ValidExponent(exponent) ? exponent : -1.0f, std::memory_order_release);
}

float VerifiedNativeCurve(bool nativeOverrideRequested) {
    CurveReader reader = g_hookCurveReader.load(std::memory_order_acquire);
    if (!reader && nativeOverrideRequested) {
        HMODULE module = nullptr;
#ifdef _WIN64
        constexpr auto moduleName = L"capture_hook_x64.dll";
#else
        constexpr auto moduleName = L"capture_hook_x86.dll";
#endif
        // This is CE's own process-lifetime injector, never a vendor FG module.
        // Pinning makes the cached export callable across layer reactivation.
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, moduleName, &module)) {
            reader = reinterpret_cast<CurveReader>(GetProcAddress(module, "CE_GetVerifiedDisplayGamma"));
            if (reader)
                g_hookCurveReader.store(reader, std::memory_order_release);
        }
    }
    return reader ? reader() : CE_GetVerifiedDisplayGamma();
}

}  // namespace ce::gamma
