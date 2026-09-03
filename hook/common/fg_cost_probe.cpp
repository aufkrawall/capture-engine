#include "fg_cost_probe.h"

#include <windows.h>

#include "hook_common.h"

namespace ce::fg_cost_probe {

namespace {

uint32_t ReadMaskFromEnvironment() {
    char buf[16] = {};
    const DWORD n = GetEnvironmentVariableA("CE_FG_COST_PROBE", buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) {
        return 0;
    }
    return ParseMask(buf);
}

}  // namespace

uint32_t Mask() {
    static const uint32_t s_mask = ReadMaskFromEnvironment();
    static const bool s_logged = [] {
        if (s_mask != 0) {
            HookLogImportant(
                "FG COST PROBE ACTIVE mask=0x%X (bridgeTailOff=%d breadcrumbsOff=%d bridgeOverlayOff=%d "
                "bridgeMirrorOff=%d eclPassthrough=%d presentPassthrough=%d ffxBridgeOff=%d "
                "eclCallerModuleLookupOff=%d eclStreamlineUiHooksOff=%d eclStartupBlockOff=%d "
                "eclClassificationOff=%d eclDiagnosticsOff=%d eclQueueRegistrationOff=%d queueVTableHookOff=%d queueDevicePublishOff=%d queueAdoptionOff=%d samplerDeviceHooksOff=%d) — this run "
                "deliberately removes CE behaviour and is a measurement, not a supported configuration",
                s_mask, (s_mask & kBridgeTailOff) != 0, (s_mask & kBreadcrumbsOff) != 0,
                (s_mask & kBridgeOverlayOff) != 0, (s_mask & kBridgeMirrorOff) != 0,
                (s_mask & kEclPassthrough) != 0, (s_mask & kPresentPassthrough) != 0,
                (s_mask & kFfxBridgeOff) != 0, (s_mask & kEclCallerModuleLookupOff) != 0,
                (s_mask & kEclStreamlineUiHooksOff) != 0, (s_mask & kEclStartupBlockOff) != 0,
                (s_mask & kEclClassificationOff) != 0, (s_mask & kEclDiagnosticsOff) != 0,
                (s_mask & kEclQueueRegistrationOff) != 0, (s_mask & kQueueVTableHookOff) != 0,
                (s_mask & kQueueDevicePublishOff) != 0, (s_mask & kQueueAdoptionOff) != 0,
                (s_mask & kSamplerDeviceHooksOff) != 0);
        }
        return true;
    }();
    (void)s_logged;
    return s_mask;
}

}  // namespace ce::fg_cost_probe
