#include "main_internal.h"

namespace UE5 {
void EnforceRR() {
  if (!g_pLocalConfig || !g_pLocalConfig->graphics.forceRayReconstruction)
    return;

  static uintptr_t s_ConsoleManagerPtr = 0;
  static bool s_AttemptedScan = false;

  HMODULE hMain = GetModuleHandleA(NULL);
  if (!hMain)
    return;

  if (!s_ConsoleManagerPtr && !s_AttemptedScan) {
    s_AttemptedScan = true;

    // Strategy 1: Scan for "r.DumpingMovie" (Core CVar) string ref
    // This is a very safe anchor.
    uintptr_t refStr = Scanner::ScanForStringRef(hMain, "r.DumpingMovie");
    if (!refStr) {
      // Try another one "r.AmbientOcclusionLevels"
      refStr = Scanner::ScanForStringRef(hMain, "r.AmbientOcclusionLevels");
    }

    if (refStr) {
      // refStr points to the LEA/MOV instruction loading the string.
      // We look backwards for the call to IConsoleManager::Get()
      // usually within 50 bytes.
      // Pattern: CALL Get; ...; LEA RDX, String

      uint8_t *p = (uint8_t *)refStr;
      for (int i = 0; i < 100; i++) {
        // Check for CALL (E8)
        if (*(p - i) == 0xE8) {
          // This MIGHT be IConsoleManager::Get()
          // Let's check where it goes.
          int32_t offset = *(int32_t *)(p - i + 1);
          uintptr_t funcAddr = (uintptr_t)(p - i + 5 + offset);

          // Check if specific function pattern: MOV RAX, [Global]; RET
          // 48 8B 05 ?? ?? ?? ?? C3
          if (*(uint8_t *)funcAddr == 0x48 &&
              *(uint8_t *)(funcAddr + 1) == 0x8B &&
              *(uint8_t *)(funcAddr + 7) == 0xC3) {
            // Found it!
            int32_t gOffset = *(int32_t *)(funcAddr + 3);
            s_ConsoleManagerPtr =
                funcAddr + 7 + gOffset; // The Global Variable Address
            HookLog("UE5: Found ConsoleManager singleton at %p (via "
                    "r.DumpingMovie)",
                    (void *)s_ConsoleManagerPtr);
            break;
          }
        }
      }
    }

    // Strategy 2: If finding Get() failed, try finding GConsoleManager global
    // directly via AOB
    if (!s_ConsoleManagerPtr) {
      // Generic pattern for "MOV RCX, [GConsoleManager]"
      // 48 8B 0D ?? ?? ?? ?? 48 85 C9 75 ?? E8
      uintptr_t aob =
          Scanner::Scan(hMain, "48 8B 0D ?? ?? ?? ?? 48 85 C9 75 ?? E8");
      if (aob) {
        int32_t offset = *(int32_t *)(aob + 3);
        s_ConsoleManagerPtr = aob + 7 + offset;
        HookLog("UE5: Found ConsoleManager singleton at %p (via AOB)",
                (void *)s_ConsoleManagerPtr);
      }
    }
  }

  if (s_ConsoleManagerPtr) {
    void *mgr = *(void **)s_ConsoleManagerPtr;
    HookLog("UE5: GConsoleManager Value at %p is %p",
            (void *)s_ConsoleManagerPtr, mgr);

    bool safeToUseMgr = false;
    if (mgr) {
      // ... verify vtable ...
      MEMORY_BASIC_INFORMATION mbi;
      if (VirtualQuery((void *)mgr, &mbi, sizeof(mbi)) &&
          (mbi.Protect &
           (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) {
        safeToUseMgr = true;
      } else {
        HookLog("UE5: GConsoleManager points to invalid memory!");
      }
    }

    if (safeToUseMgr) {
      // Probe VTable for FindConsoleVariable
      // We test indices 3, 4, 5
      static int s_ValidFindIndex = -1;

      if (s_ValidFindIndex == -1) {
        for (int idx : {4, 3, 5}) {
          FindConsoleVariable_t fn = GetVFunc<FindConsoleVariable_t>(mgr, idx);
          if (!fn)
            continue;

          // Check if points to executable memory
          MEMORY_BASIC_INFORMATION mbi;
          if (VirtualQuery((void *)fn, &mbi, sizeof(mbi))) {
            if (!(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                 PAGE_EXECUTE_READWRITE))) {
              continue;
            }
          }

          // Try "r.vsync"
          // Wrap in try/except if possible (not standard C++) but we don't have
          // it. We rely on memory check.
          void *check = fn(mgr, L"r.vsync");
          // If it returns null, it might just be not found?
          // But r.vsync is standard.
          // Try "r.DumpingMovie" which we scanned for?
          if (check) {
            s_ValidFindIndex = idx;
            HookLog("UE5: Confirmed FindConsoleVariable at VTable Index %d",
                    idx);
            break;
          }
        }
        if (s_ValidFindIndex == -1) {
          HookLog("UE5: Failed to find FindConsoleVariable (Probed 3, 4, 5)");
          // Prevent retry spam
          s_ValidFindIndex = -2;
          // DO NOT RETURN! FALLTHROUGH TO FALLBACK
        }
      }

      if (s_ValidFindIndex >= 0) {
        FindConsoleVariable_t fnFind =
            GetVFunc<FindConsoleVariable_t>(mgr, s_ValidFindIndex);

        // 1. Denoiser Mode
        void *cvarMode = fnFind(mgr, L"r.NGX.DLSS.denoisermode");
        if (cvarMode) {
          static Set_t fnSet = nullptr;
          if (!fnSet)
            fnSet = GetVFunc<Set_t>(cvarMode, 1);

          if (fnSet) {
            fnSet(cvarMode, L"1", 0x02);
            HookLog("UE5: Force Set r.NGX.DLSS.denoisermode=1");
          }
        } else {
          static bool s_LogOnce = false;
          if (!s_LogOnce) {
            HookLog(
                "UE5: CVar 'r.NGX.DLSS.denoisermode' NOT FOUND via Manager.");
            s_LogOnce = true;
          }
        }

        // 2. Ray Reconstruction
        void *cvarRR = fnFind(mgr, L"r.NGX.DLSS.RayReconstruction");
        if (cvarRR) {
          static Set_t fnSet = nullptr;
          if (!fnSet)
            fnSet = GetVFunc<Set_t>(cvarRR, 1);

          if (fnSet) {
            fnSet(cvarRR, L"1", 0x02);
            HookLog("UE5: Force Set r.NGX.DLSS.RayReconstruction=1");
          }
        }
        // If we succeeded here, we can return.
        // But if CVars were not found, Fallback might find them if Manager
        // lookup is broken? Unlikely. If Manager is valid, lookup should work.
      }
    }
  }
}
}
