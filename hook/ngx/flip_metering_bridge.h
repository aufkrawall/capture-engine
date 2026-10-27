#pragma once

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall

#include <cstdint>

// D3D12 side of the flip-metering bridge: NvAPI_D3D12_SetFlipConfig and
// NvAPI_NGX_GetDriverFeatureSupport answered in-process for a D3D12 device that runs on Vulkan, the
// batch sizes they carry bound to D3D12 frame ids and handed to the resident Vulkan layer. Rules and
// ABI in flip_metering_bridge_policy.h; the Vulkan side in hook/vulkan_layer/vulkan_flip_metering_bridge.h.
namespace ce::flip_metering_bridge {

// nvapi_QueryInterface participation. Returns CE's entry point when `functionId` is one the bridge
// implements, the resident NVAPI returned none (`resolved` is null) and the Vulkan layer has a device
// that can meter presents; nullptr hands the caller the resident NVAPI's answer.
void* MaybeAnswerQueryInterface(uint32_t functionId, void* resolved, const void* callerAddress);

// True when the resident nvapi64.dll (nvapi.dll for x86) is not the NVIDIA driver's - a translation
// NVAPI such as dxvk-nvapi loaded from a custom path. The DLSS consumers' nvapi_QueryInterface must
// then be routed through CE even with no driver-settings override configured, or the bridge never
// sees the frame generator's flip-metering request. Cached per module; safe under the loader lock.
bool IsTranslatedNvApiResident();

}  // namespace ce::flip_metering_bridge
