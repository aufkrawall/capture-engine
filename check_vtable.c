#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <stddef.h>

int main() {
    printf("Checking VTable Indices...\n");
    
    // ID3D12CommandQueue
    size_t offset_ExecuteCommandLists = offsetof(ID3D12CommandQueueVtbl, ExecuteCommandLists);
    size_t index_ExecuteCommandLists = offset_ExecuteCommandLists / sizeof(void*);
    printf("ID3D12CommandQueue::ExecuteCommandLists Index: %zu (Offset: %zu)\n", index_ExecuteCommandLists, offset_ExecuteCommandLists);
    
    printf("Methods before ExecuteCommandLists:\n");
    if (index_ExecuteCommandLists > 0) printf("- UpdateTileMappings: %zu\n", offsetof(ID3D12CommandQueueVtbl, UpdateTileMappings)/sizeof(void*));
    if (index_ExecuteCommandLists > 1) printf("- CopyTileMappings: %zu\n", offsetof(ID3D12CommandQueueVtbl, CopyTileMappings)/sizeof(void*));

    // ID3D12Device
    size_t offset_CreateSampler = offsetof(ID3D12DeviceVtbl, CreateSampler);
    size_t index_CreateSampler = offset_CreateSampler / sizeof(void*);
    printf("ID3D12Device::CreateSampler Index: %zu (Offset: %zu)\n", index_CreateSampler, offset_CreateSampler);

    // IDXGIFactory
    size_t offset_CreateSwapChain = offsetof(IDXGIFactoryVtbl, CreateSwapChain);
    size_t index_CreateSwapChain = offset_CreateSwapChain / sizeof(void*);
    printf("IDXGIFactory::CreateSwapChain Index: %zu (Offset: %zu)\n", index_CreateSwapChain, offset_CreateSwapChain);

    // IDXGIFactory2
    size_t offset_CreateSwapChainForHwnd = offsetof(IDXGIFactory2Vtbl, CreateSwapChainForHwnd);
    size_t index_CreateSwapChainForHwnd = offset_CreateSwapChainForHwnd / sizeof(void*);
    printf("IDXGIFactory2::CreateSwapChainForHwnd Index: %zu (Offset: %zu)\n", index_CreateSwapChainForHwnd, offset_CreateSwapChainForHwnd);
    
    // IDXGISwapChain
    size_t offset_Present = offsetof(IDXGISwapChainVtbl, Present);
    size_t index_Present = offset_Present / sizeof(void*);
    printf("IDXGISwapChain::Present Index: %zu (Offset: %zu)\n", index_Present, offset_Present);

    size_t offset_ResizeBuffers = offsetof(IDXGISwapChainVtbl, ResizeBuffers);
    size_t index_ResizeBuffers = offset_ResizeBuffers / sizeof(void*);
    printf("IDXGISwapChain::ResizeBuffers Index: %zu (Offset: %zu)\n", index_ResizeBuffers, offset_ResizeBuffers);

    // IDXGISwapChain1
    size_t offset_Present1 = offsetof(IDXGISwapChain1Vtbl, Present1);
    size_t index_Present1 = offset_Present1 / sizeof(void*);
    printf("IDXGISwapChain1::Present1 Index: %zu (Offset: %zu)\n", index_Present1, offset_Present1);

    return 0;
}
