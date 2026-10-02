#pragma once

#include <d3d12.h>
#include <dxgi1_5.h>
#include <windows.h>

void EarlyLog(const char* fmt, ...);
void HookLog(const char* fmt, ...);
void DX12_OnSwapchainResizeBegin();
void DX12_OnSwapchainResizeEnd();

static const GUID IID_StreamlineNativeInterfaceBlock = {
    0xADEC44E2, 0x61F0, 0x45C3, {0xAD, 0x9F, 0x1B, 0x37, 0x37, 0x92, 0x84, 0xFF}};
