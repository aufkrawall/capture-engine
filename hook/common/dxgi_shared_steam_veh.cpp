#include "dxgi_shared_internal.h"

namespace DXGIShared {
void** ResolveSteamNullCallbackSlotFromFault(uintptr_t returnAddress, uintptr_t steamStart, uintptr_t steamEnd) {

#ifdef _WIN64
    if (returnAddress < steamStart + 2 || returnAddress >= steamEnd) {
        return nullptr;
    }

    const uintptr_t callAddress = returnAddress - 2;
    const auto* callBytes = reinterpret_cast<const uint8_t*>(callAddress);
    if (!IsReadableMemory(callBytes, 2) || callBytes[0] != 0xFF || callBytes[1] != 0xD0) {
        return nullptr;
    }

    const uintptr_t scanStart = (callAddress > steamStart + 64) ? callAddress - 64 : steamStart;
    for (uintptr_t instr = callAddress; instr >= scanStart; --instr) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(instr);
        if (!IsReadableMemory(bytes, 7)) {
            if (instr == scanStart) {
                break;
            }
            continue;
        }

        if (bytes[0] == 0x48 && bytes[1] == 0x8B && bytes[2] == 0x05) {
            int32_t disp = 0;
            memcpy(&disp, bytes + 3, sizeof(disp));
            auto** slot = reinterpret_cast<void**>(instr + 7 + disp);
            const uintptr_t slotAddress = reinterpret_cast<uintptr_t>(slot);
            if (slotAddress >= steamStart && slotAddress + sizeof(void*) <= steamEnd &&
                IsReadableMemory(reinterpret_cast<const void*>(slot), sizeof(void*)) && *slot == nullptr) {
                return slot;
            }
        }

        if (instr == scanStart) {
            break;
        }
    }
#else
    if (returnAddress < steamStart + 2 || returnAddress >= steamEnd) {
        return nullptr;
    }

    const uintptr_t callAddress = returnAddress - 2;
    const auto* callBytes = reinterpret_cast<const uint8_t*>(callAddress);
    if (!IsReadableMemory(callBytes, 2) || callBytes[0] != 0xFF || callBytes[1] != 0xD0) {
        return nullptr;
    }

    const uintptr_t scanStart = (callAddress > steamStart + 32) ? callAddress - 32 : steamStart;
    for (uintptr_t instr = callAddress; instr >= scanStart; --instr) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(instr);
        uintptr_t slotAddress = 0;
        if (IsReadableMemory(bytes, 5) && bytes[0] == 0xA1) {
            uint32_t absolute = 0;
            memcpy(&absolute, bytes + 1, sizeof(absolute));
            slotAddress = absolute;
        } else if (IsReadableMemory(bytes, 6) && bytes[0] == 0x8B && bytes[1] == 0x05) {
            uint32_t absolute = 0;
            memcpy(&absolute, bytes + 2, sizeof(absolute));
            slotAddress = absolute;
        }

        auto** slot = reinterpret_cast<void**>(slotAddress);
        if (slotAddress >= steamStart && slotAddress + sizeof(void*) <= steamEnd &&
            IsReadableMemory(slot, sizeof(void*)) && *slot == nullptr) {
            return slot;
        }

        if (instr == scanStart) {
            break;
        }
    }
#endif

    return nullptr;
}
}

namespace DXGIShared {
// VEH handler: catches Steam's NULL rendering callback crash during the
// one-time init and guarded Present paths.  It resolves the exact Steam global
// slot that supplied NULL to `call (e)ax`, patches that slot to CE's DXGI
// bypass Present when possible, and retries the call so Steam can keep its own
// overlay chain alive.
//
// Architecture notes:
//   x64: returnAddr from [RSP], RIP, RAX, RSP, call rax = FF D0 (2 bytes)
//   x86: returnAddr from [ESP], EIP, EAX, ESP, call eax = FF D0 (2 bytes)
//   Steam module: x64=gameoverlayrenderer64.dll, x86=gameoverlayrenderer.dll
//   Legacy fallback RVA: x64=0x1621d8. Newer Steam builds can use nearby slots;
//   the handler first resolves the slot dynamically from the faulting mov/call.
LONG CALLBACK SteamOverlayInitVehHandler(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode != STATUS_ACCESS_VIOLATION) {
        static std::atomic<int> s_nonAvDeclineLogCount{0};
        const int n = s_nonAvDeclineLogCount.fetch_add(1, std::memory_order_relaxed);
        if (n < 10 || (n % 200) == 0) {
            HookLog("SteamOverlayInitVehHandler: declined non-AV exception 0x%08lX (n=%d)",
                    (unsigned long)ep->ExceptionRecord->ExceptionCode, n);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

#ifdef _WIN64
    const wchar_t* steamModuleName = L"gameoverlayrenderer64.dll";
    const uintptr_t kSteamCallbackRva = 0x1621d8;
    const int kCallOpcodeSize = 2;  // FF D0 = call rax (2 bytes)
    // RIP=0, RAX=0: calling through NULL (`call rax` where RAX loaded from NULL ptr)
    if (ep->ContextRecord->Rip != 0 || ep->ContextRecord->Rax != 0) {
        static std::atomic<int> s_contextDeclineLogCount{0};
        const int n = s_contextDeclineLogCount.fetch_add(1, std::memory_order_relaxed);
        if (n < 10 || (n % 200) == 0) {
            HookLog("SteamOverlayInitVehHandler: declined context rip=%p rax=%p (n=%d)",
                    (void*)ep->ContextRecord->Rip, (void*)ep->ContextRecord->Rax, n);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    uintptr_t returnAddress = 0;
    if (ep->ContextRecord->Rsp) {
        returnAddress = *(uintptr_t*)ep->ContextRecord->Rsp;
    }
#else
    const wchar_t* steamModuleName = L"gameoverlayrenderer.dll";
    const uintptr_t kSteamCallbackRva = 0x1621d8;
    const int kCallOpcodeSize = 2;  // FF D0 = call eax (2 bytes)
    // EIP=0, EAX=0: calling through NULL (`call eax` where EAX loaded from NULL ptr)
    if (ep->ContextRecord->Eip != 0 || ep->ContextRecord->Eax != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    uintptr_t returnAddress = 0;
    if (ep->ContextRecord->Esp) {
        returnAddress = *(uintptr_t*)ep->ContextRecord->Esp;
    }
#endif

    if (!returnAddress) {
        static std::atomic<int> s_noReturnAddressDeclineLogCount{0};
        const int n = s_noReturnAddressDeclineLogCount.fetch_add(1, std::memory_order_relaxed);
        if (n < 10 || (n % 200) == 0) {
            HookLog("SteamOverlayInitVehHandler: declined missing return address (n=%d)", n);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    HMODULE steamMod = GetModuleHandleW(steamModuleName);
    if (!steamMod) {
        static std::atomic<int> s_noSteamModuleDeclineLogCount{0};
        const int n = s_noSteamModuleDeclineLogCount.fetch_add(1, std::memory_order_relaxed);
        if (n < 10 || (n % 200) == 0) {
            HookLog("SteamOverlayInitVehHandler: declined Steam module not loaded (n=%d)", n);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    MODULEINFO modInfo = {};
    if (!GetModuleInformation(GetCurrentProcess(), steamMod, &modInfo, sizeof(modInfo))) {
        static std::atomic<int> s_moduleInfoDeclineLogCount{0};
        const int n = s_moduleInfoDeclineLogCount.fetch_add(1, std::memory_order_relaxed);
        if (n < 10 || (n % 200) == 0) {
            HookLog("SteamOverlayInitVehHandler: declined GetModuleInformation failure (n=%d)", n);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    uintptr_t steamStart = (uintptr_t)steamMod;
    uintptr_t steamEnd = steamStart + modInfo.SizeOfImage;
    if (returnAddress < steamStart || returnAddress >= steamEnd) {
        static std::atomic<int> s_outsideModuleDeclineLogCount{0};
        const int n = s_outsideModuleDeclineLogCount.fetch_add(1, std::memory_order_relaxed);
        if (n < 10 || (n % 200) == 0) {
            HookLog("SteamOverlayInitVehHandler: declined return address outside Steam module "
                    "(ret=%p steam=%p-%p n=%d)",
                    (void*)returnAddress, (void*)steamStart, (void*)steamEnd, n);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const SteamNullCallbackRecoveryContext recoveryContext = dxgi_shared_s_steamNullCallbackRecoveryContext;
    const void* patchTarget = SelectSteamNullCallbackRecoveryTarget(recoveryContext);
    void** nullFnPtr = ResolveSteamNullCallbackSlotFromFault(returnAddress, steamStart, steamEnd);
    const bool dynamicallyResolvedSlot = nullFnPtr != nullptr;
    if (!nullFnPtr) {
        nullFnPtr = reinterpret_cast<void**>(steamStart + kSteamCallbackRva);
    }
    const uintptr_t resolvedRva = reinterpret_cast<uintptr_t>(nullFnPtr) - steamStart;
    void* callbackBefore = nullptr;
    const bool callbackSlotReadable = IsReadableMemory(reinterpret_cast<const void*>(nullFnPtr), sizeof(void*));
    if (callbackSlotReadable) {
        callbackBefore = *nullFnPtr;
    }
    bool patched = false;
    if (callbackSlotReadable && callbackBefore == nullptr) {
        DWORD oldProtect;
        if (VirtualProtect(reinterpret_cast<void*>(nullFnPtr), sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            *nullFnPtr = const_cast<void*>(patchTarget);
            VirtualProtect(reinterpret_cast<void*>(nullFnPtr), sizeof(void*), oldProtect, &oldProtect);
            patched = true;
            HookLogImportant(
                "SteamOverlayInitVehHandler: Patched NULL callback at %p (steam+0x%zX) "
                "-> %s=%p context=%s reason=%s hook=%p bypass=%p dynamicSlot=%d streamlineStack=%d pluginGuard=%d",
                nullFnPtr, resolvedRva,
                patchTarget == recoveryContext.bypass ? "DXGIBypassPresent" : "SteamDummyRenderingCallback",
                patchTarget, recoveryContext.context ? recoveryContext.context : "unknown",
                recoveryContext.reason ? recoveryContext.reason : "Present", recoveryContext.hook,
                recoveryContext.bypass, dynamicallyResolvedSlot ? 1 : 0, recoveryContext.streamlineStackActive ? 1 : 0,
                recoveryContext.pluginLookupGuardReady ? 1 : 0);
        } else {
            HookLogImportant(
                "SteamOverlayInitVehHandler: VirtualProtect failed for Steam callback at %p context=%s reason=%s",
                nullFnPtr, recoveryContext.context ? recoveryContext.context : "unknown",
                recoveryContext.reason ? recoveryContext.reason : "Present");
        }
    } else {
        HookLogImportant(
            "SteamOverlayInitVehHandler: RVA 0x%zX not patchable (slot=%p readable=%d value=%p) - RVA may have "
            "changed, skipping patch and falling back to crash skip context=%s reason=%s dynamicSlot=%d",
            resolvedRva, nullFnPtr, callbackSlotReadable ? 1 : 0, callbackBefore,
            recoveryContext.context ? recoveryContext.context : "unknown",
            recoveryContext.reason ? recoveryContext.reason : "Present", dynamicallyResolvedSlot ? 1 : 0);
    }

    if (patched) {
        // Retry `call (e)ax` with our patched callback.
#ifdef _WIN64
        ep->ContextRecord->Rsp += 8;  // undo the call's stack push (8 bytes on x64)
        ep->ContextRecord->Rax = (DWORD64)patchTarget;
        ep->ContextRecord->Rip = returnAddress - kCallOpcodeSize;
        HookLog("SteamOverlayInitVehHandler: Retrying call rax with RAX=%p", (void*)ep->ContextRecord->Rax);
#else
        ep->ContextRecord->Esp += 4;  // undo the call's stack push (4 bytes on x86)
        ep->ContextRecord->Eax = (DWORD)(uintptr_t)patchTarget;
        ep->ContextRecord->Eip = returnAddress - kCallOpcodeSize;
        HookLog("SteamOverlayInitVehHandler: Retrying call eax with EAX=%p", (void*)(DWORD_PTR)ep->ContextRecord->Eax);
#endif
    } else {
        // Could not patch - skip past the crash entirely.
        // Set (R/E)IP past the call and (R/E)AX = S_OK (0) so Steam continues.
        // This may cause Steam to crash elsewhere if the callback was mandatory,
        // but at least we tried.
#ifdef _WIN64
        ep->ContextRecord->Rax = 0;  // S_OK
        ep->ContextRecord->Rip = returnAddress;
        // NOTE: RSP already points to the return address (pushed by `call rax`).
        // By setting RIP=returnAddress, we consume that pushed return address
        // as the "function returned normally". RSP is preserved correctly.
        HookLog("SteamOverlayInitVehHandler: Skipped past crash (fallback) - RIP=%p RAX=0",
                (void*)ep->ContextRecord->Rip);
#else
        ep->ContextRecord->Eax = 0;  // S_OK
        ep->ContextRecord->Eip = returnAddress;
        HookLog("SteamOverlayInitVehHandler: Skipped past crash (fallback) - EIP=%p EAX=0",
                (void*)(DWORD_PTR)ep->ContextRecord->Eip);
#endif
    }

    return EXCEPTION_CONTINUE_EXECUTION;
}
}
