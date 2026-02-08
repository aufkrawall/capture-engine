# Hooking Standardization Guide

## Overview

This document defines the standardized hooking approaches for CaptureEngine. The codebase currently uses multiple hooking mechanisms, and this guide ensures consistency in future development.

## Hooking Mechanisms

### 1. IAT (Import Address Table) Patching - PREFERRED

**Use for:** Hooking imports of a module

**Location:** `hook/wrappers/iat_hook.h`, `hook/wrappers/iat_hook.cpp`

**When to use:**
- Hooking system APIs (LoadLibrary, CreateProcess, etc.)
- Hooking graphics API entry points (D3D11CreateDevice, etc.)
- Early initialization in DllMain

**Pattern:**
```cpp
#include "wrappers/iat_hook.h"

// Hook a function
void* dummy = nullptr;
IATHook::PatchIATAllModules("kernel32.dll", "LoadLibraryA", 
                            (void*)HookedLoadLibraryA, &dummy);

// Register for dynamic hooking (catches GetProcAddress)
IATHook::RegisterDynamicHook("LoadLibraryA", (void*)HookedLoadLibraryA, 
                             (void**)&g_OriginalLoadLibraryA);
```

**Advantages:**
- Works in DllMain safely
- No code modification needed
- Anti-cheat friendly (common pattern)

**Limitations:**
- Only works for imported functions
- Cannot hook internal/private functions

---

### 2. VTable Hooking - FOR COM INTERFACES

**Use for:** Hooking COM interface methods

**Location:** `hook/wrappers/vtable_hook.h`, `hook/wrappers/vtable_hook.cpp`

**When to use:**
- Hooking Present/ResizeBuffers on swapchains
- Hooking ExecuteCommandLists on command queues
- Hooking DXGI factory methods

**Pattern:**
```cpp
#include "wrappers/vtable_hook.h"

// Get vtable from instance
void** vtable = *(void***)pSwapChain;

// Hook Present (vtable[8] for IDXGISwapChain)
if (VTableHook::Create(&vtable[8], (LPVOID)HookedPresent, (LPVOID*)&oPresent)) {
    LogInfo("Present hooked");
}

// Cleanup on shutdown
VTableHook::Destroy(vtable[8]);
```

**Advantages:**
- All instances share same vtable (hook once, catch all)
- Standard COM pattern
- Works well with FSR/DLSS FG

**Limitations:**
- Only works for virtual/COM methods
- Must ensure all instances use same vtable

---

### 3. Wrapper Approach - FOR SWAPCHAINS/DEVICES

**Use for:** Wrapping DXGI/D3D objects for complete control

**Location:** `hook/wrappers/dxgi_swapchain_wrap.cpp`

**When to use:**
- Full control over swapchain behavior
- Intercepting all swapchain methods
- Handling FSR/DLSS FG swapchain recreation

**Pattern:**
```cpp
// Create wrapper around real swapchain
CWrapDXGISwapChain* pWrapper = new CWrapDXGISwapChain(pRealSwapChain, pDevice);

// Return wrapper to game (game calls methods on wrapper)
*pSwapChain = pWrapper;
```

**Advantages:**
- Complete control over object lifetime
- Can store additional state
- Blocks FG runtime unwrapping attempts

**Limitations:**
- More complex
- Must implement all interface methods
- Reference counting complexity

---

### 4. GetProcAddress Interception - FOR DYNAMIC LOADING

**Use for:** Hooking functions loaded via GetProcAddress

**Location:** `hook/wrappers/iat_hook.h` (RegisterDynamicHook)

**When to use:**
- Functions loaded dynamically at runtime
- Delay-loaded DLLs
- Games that manually resolve imports

**Pattern:**
```cpp
// Register dynamic hook
IATHook::RegisterDynamicHook("GetProcAddress", 
                             (void*)HookedGetProcAddress,
                             (void**)&g_OriginalGetProcAddress);
```

**Advantages:**
- Catches runtime lookups
- Works with custom loaders

**Limitations:**
- Overhead of intercepting every GetProcAddress call
- Must be installed early

---

## Standard Hook Installation Pattern

For consistency, use this pattern when installing hooks:

```cpp
class GraphicsHook {
public:
    bool Install() {
        // 1. Install IAT hooks first (safer in DllMain)
        if (!InstallIATHooks()) {
            LogError("Failed to install IAT hooks");
            return false;
        }
        
        // 2. Install vtable hooks after objects are created
        // (usually done on first use)
        return true;
    }
    
    void Shutdown() {
        // Cleanup in reverse order
        CleanupVTableHooks();
        CleanupIATHooks();
    }
    
private:
    bool InstallIATHooks() {
        void* dummy = nullptr;
        return IATHook::PatchIATAllModules("d3d11.dll", "D3D11CreateDevice",
                                           (void*)Hooked_D3D11CreateDevice, &dummy);
    }
    
    void CleanupVTableHooks() {
        // Remove hooks, restore original pointers
    }
    
    void CleanupIATHooks() {
        // IAT hooks are automatically cleaned up when DLL unloads
        // (or use IATHook::RestoreIAT if needed)
    }
};
```

---

## Hook Safety Guidelines

### Thread Safety

1. **Always use atomics for shared state**
   ```cpp
   std::atomic<bool> g_HookInstalled{false};
   ```

2. **Use mutex for complex state**
   ```cpp
   std::mutex g_HookMutex;
   std::lock_guard<std::mutex> lock(g_HookMutex);
   ```

3. **Avoid blocking in hot paths**
   - Present hooks should be fast
   - Use lock-free data structures where possible

### Recursion Prevention

1. **Check if already in hook**
   ```cpp
   thread_local bool g_InHook = false;
   if (g_InHook) return CallOriginal(...);
   g_InHook = true;
   // ... hook logic ...
   g_InHook = false;
   ```

2. **Use atomic thread IDs**
   ```cpp
   static std::atomic<DWORD> s_hookThreadId{0};
   DWORD currentId = GetCurrentThreadId();
   if (s_hookThreadId.load() == currentId) {
       return CallOriginal(...);
   }
   s_hookThreadId.store(currentId);
   // ... hook logic ...
   s_hookThreadId.store(0);
   ```

### Anti-Cheat Compatibility

1. **Prefer IAT over inline hooks**
   - IAT patching is more common and less suspicious
   - Inline hooks modify code, which anti-cheats flag

2. **Avoid global hooks**
   - Hook only specific modules
   - Don't use SetWindowsHookEx globally

3. **Clean up on unload**
   - Always remove hooks before DLL unload
   - Restore original pointers

---

## Error Handling

Standard error handling for hooks:

```cpp
enum class HookResult {
    Success,
    ModuleNotFound,
    FunctionNotFound,
    AlreadyHooked,
    ProtectionFailed,
    UnknownError
};

HookResult InstallHook() {
    HMODULE hMod = GetModuleHandleA("d3d11.dll");
    if (!hMod) {
        LogError("d3d11.dll not loaded");
        return HookResult::ModuleNotFound;
    }
    
    void* target = GetProcAddress(hMod, "D3D11CreateDevice");
    if (!target) {
        LogError("D3D11CreateDevice not found");
        return HookResult::FunctionNotFound;
    }
    
    // ... install hook ...
    
    return HookResult::Success;
}
```

---

## Future Improvements

### Unified Hook System

A unified hook system is partially implemented in `hook_system.h`. Future development should:

1. Migrate to the builder pattern:
   ```cpp
   auto hook = Hook::IAT("kernel32.dll", "LoadLibraryA")
       .SetHook(&HookedLoadLibraryA)
       .Install();
   ```

2. Use RAII guards for automatic cleanup:
   ```cpp
   {
       auto guard = Hook::VTable(pSwapChain, 8)
           .SetHook(&HookedPresent)
           .InstallGuard();
       
       // Hook active in this scope
   }  // Automatically removed
   ```

3. Central hook registry:
   ```cpp
   Hook::Manager::Register("DX11_Present", hook);
   Hook::Manager::RemoveAll();
   ```

---

## Migration Checklist

When modifying existing hook code:

- [ ] Use IAT for API entry points
- [ ] Use VTable for COM methods
- [ ] Use wrappers for full control
- [ ] Add thread-safety checks
- [ ] Add recursion guards
- [ ] Log installation failures
- [ ] Cleanup hooks on shutdown
- [ ] Test with FSR/DLSS FG if applicable
- [ ] Test with anti-cheat if applicable

---

## References

- `hook/wrappers/iat_hook.h` - IAT patching
- `hook/wrappers/vtable_hook.h` - VTable hooking
- `hook/wrappers/hook_system.h` - Unified system (future)
- `hook/apis/dx11_hook.cpp` - Example implementation
- `hook/apis/dx12_hook.cpp` - Example with multiple APIs
