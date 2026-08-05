#include "ddraw_hook_internal.h"

static ce::graphics_api_identity::DirectDrawVersion DirectDrawVersionFromIID(REFIID iid) {
    if (IsEqualIID(iid, IID_IDirectDraw7))
        return ce::graphics_api_identity::DirectDrawVersion::DirectDraw7;
    if (IsEqualIID(iid, IID_IDirectDraw4))
        return ce::graphics_api_identity::DirectDrawVersion::DirectDraw4;
    if (IsEqualIID(iid, ddraw_hook_kIID_IDirectDraw3))
        return ce::graphics_api_identity::DirectDrawVersion::DirectDraw3;
    if (IsEqualIID(iid, IID_IDirectDraw2))
        return ce::graphics_api_identity::DirectDrawVersion::DirectDraw2;
    if (IsEqualIID(iid, IID_IDirectDraw))
        return ce::graphics_api_identity::DirectDrawVersion::DirectDraw;
    return ce::graphics_api_identity::DirectDrawVersion::Unknown;
}

bool HookDirectDrawObject(void* directDrawObject, REFIID iid) {
    HookLog("DDraw: HookDirectDrawObject called (object=%p, iidIsDDraw7=%d, iidIsDDraw4=%d)", directDrawObject,
            IsEqualIID(iid, IID_IDirectDraw7) ? 1 : 0, IsEqualIID(iid, IID_IDirectDraw4) ? 1 : 0);

    if (!directDrawObject)
        return false;

    if (ShouldSuppressDirectDrawHooking()) {
        return false;
    }

    const auto requestedVersion = DirectDrawVersionFromIID(iid);
    if (requestedVersion != ce::graphics_api_identity::DirectDrawVersion::Unknown && ddraw_hook_g_DDrawBootstrapDepth != 0) {
        static std::atomic<int> s_ignoredBootstrapIdentityLogs{0};
        if (s_ignoredBootstrapIdentityLogs.fetch_add(1, std::memory_order_relaxed) < 4) {
            HookLog("[GraphicsAPI] ignored synthetic DirectDraw bootstrap interface api=%s",
                    ce::graphics_api_identity::DirectDrawLabel(requestedVersion));
        }
    }
    if (requestedVersion != ce::graphics_api_identity::DirectDrawVersion::Unknown && ddraw_hook_g_DDrawBootstrapDepth == 0) {
        ddraw_hook_g_LegacyD3DCallbackVersion.store(0, std::memory_order_release);
        ddraw_hook_g_ActiveLegacyD3DVersion.store(0, std::memory_order_release);
        const int previous =
            ddraw_hook_g_ActiveDirectDrawVersion.exchange(static_cast<int>(requestedVersion), std::memory_order_acq_rel);
        if (previous != static_cast<int>(requestedVersion)) {
            HookLogImportant("[GraphicsAPI] DirectDraw interface accepted api=%s evidence=application creation",
                             ce::graphics_api_identity::DirectDrawLabel(requestedVersion));
        }
    }

    if (IsEqualIID(iid, IID_IDirectDraw7)) {
        InstallDirectDrawHooksForInstance(reinterpret_cast<IDirectDraw7*>(directDrawObject), "wrapper CreateEx");
        return true;
    }

    if (IsEqualIID(iid, IID_IDirectDraw4)) {
        auto* ddraw4 = reinterpret_cast<IDirectDraw4*>(directDrawObject);
        InstallDirectDraw4HooksForInstance(ddraw4, "wrapper CreateEx");

        IDirectDraw7* ddraw7 = nullptr;
        if (SUCCEEDED(ddraw4->QueryInterface(IID_IDirectDraw7, reinterpret_cast<void**>(&ddraw7))) && ddraw7) {
            InstallDirectDrawHooksForInstance(ddraw7, "wrapper CreateEx upgrade");
            ddraw7->Release();
        }
        return true;
    }

    if (requestedVersion == ce::graphics_api_identity::DirectDrawVersion::DirectDraw ||
        requestedVersion == ce::graphics_api_identity::DirectDrawVersion::DirectDraw2 ||
        requestedVersion == ce::graphics_api_identity::DirectDrawVersion::DirectDraw3) {
        auto* legacy = reinterpret_cast<IDirectDraw*>(directDrawObject);
        InstallLegacyDirectDrawHooksForInstance(legacy, requestedVersion, "wrapper creation");

        IDirectDraw7* ddraw7 = nullptr;
        if (SUCCEEDED(legacy->QueryInterface(IID_IDirectDraw7, reinterpret_cast<void**>(&ddraw7))) && ddraw7) {
            InstallDirectDrawHooksForInstance(ddraw7, "wrapper creation upgrade");
            ddraw7->Release();
        }
        return true;
    }

    return false;
}
