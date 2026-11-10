#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

#include "swapchain_lifetime_witness.h"

struct IDXGISwapChain;

// Who drops the references of a native DXGI swapchain, observed where DXGI's own Release returns.
//
// One body hook on DXGI's swapchain Release serves every subscriber (the back-buffer reference trace, the Streamline
// unload-order policy): the hook is neither a vtable slot nor an entry patch, because the Steam overlay and EOS patch
// the entries of the functions a swapchain's vtable points to and skip one that already jumps into another module
// (resize_reconcile_hook_policy.h). Every call of every other swapchain pays one pointer compare per subscriber.
//
// A subscriber watches at most one chain. Its handler runs on the releasing thread after the native Release returned,
// for a caller outside CE and DXGI itself, and treats the chain as an identity: the call may have destroyed it.

namespace ce::native_swapchain_release_observer {

enum class Subscriber : uint32_t {
    kReferenceTrace = 0,  // BackBufferReferenceTrace's [SL LIFETIME] native Release lines
    kUnloadOrder = 1,     // the Streamline unload-order policy
};
constexpr size_t kSubscriberCount = 2;

struct ReleaseReport {
    IDXGISwapChain* chain = nullptr;  // identity only, never dereferenced
    const void* caller = nullptr;     // the Release call site
    HMODULE callerModule = nullptr;
    const char* callerImage = "";  // the caller module's file name; valid during the handler call only
    uintptr_t callerOffset = 0;    // caller - callerModule
    ULONG remaining = 0;           // the count DXGI's Release returned
    // What the lifetime witness said before the call: the Release that destroys the chain ends the witness with it.
    ce::swapchain_lifetime_witness::ChainReport witnessBefore;
    int64_t enterQpcUs = 0;
    int64_t returnQpcUs = 0;
    DWORD threadId = 0;
};

using ReleaseHandler = void (*)(const ReleaseReport& report);

// The handler of `subscriber`; set before the first Watch.
void SetHandler(Subscriber subscriber, ReleaseHandler handler);

// The chain `subscriber` watches; nullptr stops watching. Never installs anything.
void Watch(Subscriber subscriber, IDXGISwapChain* chain);

// Installs the body hook once per process from the Release DXGI's vtable names for `liveChain` (never removed: the
// hook DLL is pinned, and the detour forwards while nothing is watched). A chain whose Release is not DXGI's (a
// Streamline proxy) does not use up the attempt. True while the hook is installed.
bool Install(IDXGISwapChain* liveChain);
bool IsInstalled();

}  // namespace ce::native_swapchain_release_observer
