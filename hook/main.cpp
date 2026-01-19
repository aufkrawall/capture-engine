#include "apis/dx12_hook.h"
#include "../common/utils/scanner.h"
#include <psapi.h>
#include "apis/dx11_hook.h"
#include "apis/dx9_hook.h"
#include "apis/ddraw_hook.h"
#include "apis/dx8_hook.h"
#include "apis/opengl_hook.h"
#ifdef ENABLE_VULKAN_HOOK
#include "apis/vulkan_hook.h"
#endif
#include "apis/nvngx_hook.h"
#include "wrappers/wrapper_hooks.h"
#include "wrappers/iat_hook.h"
#include "wrappers/iat_hook.h"
#include "common/hook_common.h"
#include "common/ipc_client.h"
#include "capture/shared_capture.h"
#include "common/fg_detection.h"
#include "common/system_metrics.h"
// MinHook removed - using IAT patching via iat_hook.h
#include <cstddef>
#include <string>
#include <thread>
#include <fstream>
#include <vector>
#include <windows.h>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <cstring>

HMODULE g_hModule = NULL;

enum class ProcessCategory {
    PotentialGame,
    Launcher,
    InternalTool,
    Blacklisted
};
static ProcessCategory g_ProcessCategory = ProcessCategory::PotentialGame;

static bool g_isDormant = false;
static bool g_isSkippedProcess = false; 

// Global Hook Pointers
DX12Hook *g_DX12Hook = nullptr;
DX11Hook *g_DX11Hook = nullptr;
DX9Hook *g_DX9Hook = nullptr;
DDrawHook *g_DDrawHook = nullptr;
DX8Hook *g_DX8Hook = nullptr;
OpenGLHook *g_OpenGLHook = nullptr;
#ifdef ENABLE_VULKAN_HOOK
VulkanHook *g_VulkanHook = nullptr;
#else
void* g_VulkanHook = nullptr;  // Stub for builds without VulkanHook
#endif

// Global Local Config
// Global Local Config
AppConfig* g_pLocalConfig = nullptr;

// Helper to safely delete hooks
template<typename T>
void SafeShutdownHook(T*& hook) {
    if (hook) {
        hook->Shutdown();
        delete hook;
        hook = nullptr;
    }
}

#include "../common/logging.h"
#include <filesystem>

// LoadLibrary Hook Typedefs
// ... (same as before)
typedef HMODULE(WINAPI *LoadLibraryA_t)(LPCSTR lpLibFileName);
typedef HMODULE(WINAPI *LoadLibraryW_t)(LPCWSTR lpLibFileName);
typedef HMODULE(WINAPI *LoadLibraryExA_t)(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);
typedef HMODULE(WINAPI *LoadLibraryExW_t)(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);

LoadLibraryA_t OriginalLoadLibraryA = nullptr;
LoadLibraryW_t OriginalLoadLibraryW = nullptr;
LoadLibraryExA_t OriginalLoadLibraryExA = nullptr;
LoadLibraryExW_t OriginalLoadLibraryExW = nullptr;

typedef LPSTR(WINAPI *GetCommandLineA_t)();
typedef LPWSTR(WINAPI *GetCommandLineW_t)();
GetCommandLineA_t OriginalGetCommandLineA = nullptr;
GetCommandLineW_t OriginalGetCommandLineW = nullptr;

typedef int(WINAPI *getmainargs_t)(int *argc, char ***argv, char ***env, int doWildCard, void *startInfo);
typedef int(WINAPI *wgetmainargs_t)(int *argc, wchar_t ***argv, wchar_t ***env, int doWildCard, void *startInfo);
static getmainargs_t Original_getmainargs = nullptr;
static wgetmainargs_t Original_wgetmainargs = nullptr;

// CreateProcess Hook Typedefs for child process injection
typedef BOOL(WINAPI *CreateProcessA_t)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
typedef BOOL(WINAPI *CreateProcessW_t)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
CreateProcessA_t OriginalCreateProcessA = nullptr;
CreateProcessW_t OriginalCreateProcessW = nullptr;

// Registry Hook Typedefs (for DLSS Debug Overlay)
typedef LSTATUS(WINAPI *RegQueryValueExW_t)(HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData);
RegQueryValueExW_t OriginalRegQueryValueExW = nullptr;

// Helper: Inject our DLL into a suspended child process
void InjectIntoChild(HANDLE hProcess, HANDLE hThread) {
    char dllPath[MAX_PATH];
    GetModuleFileNameA(g_hModule, dllPath, MAX_PATH);
    
    SIZE_T pathLen = strlen(dllPath) + 1;
    LPVOID pRemote = VirtualAllocEx(hProcess, NULL, pathLen, MEM_COMMIT, PAGE_READWRITE);
    if (!pRemote) {
        HookLog("[ChildInject] VirtualAllocEx failed: %d", GetLastError());
        ResumeThread(hThread);
        return;
    }
    
    if (!WriteProcessMemory(hProcess, pRemote, dllPath, pathLen, NULL)) {
        HookLog("[ChildInject] WriteProcessMemory failed: %d", GetLastError());
        VirtualFreeEx(hProcess, pRemote, 0, MEM_RELEASE);
        ResumeThread(hThread);
        return;
    }
    
    LPVOID pLoadLib = (LPVOID)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    HANDLE hRemote = CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)pLoadLib, pRemote, 0, NULL);
    if (hRemote) {
        WaitForSingleObject(hRemote, 5000);
        CloseHandle(hRemote);
        HookLog("[ChildInject] Injected into child process.");
    } else {
        HookLog("[ChildInject] CreateRemoteThread failed: %d", GetLastError());
    }
    
    VirtualFreeEx(hProcess, pRemote, 0, MEM_RELEASE);
    ResumeThread(hThread);
}

// Helper: Check if executable should be injected into (Heuristic)
// We skip common system/launcher processes
bool ShouldInjectChild(const char* exePath) {
    if (!exePath) return false;
    
    // Extract filename from path
    std::string path(exePath);
    size_t lastSlash = path.find_last_of("\\/");
    std::string filename = (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;
    
    // Convert to lowercase
    std::string lowerName;
    for (char c : filename) lowerName += (char)tolower(c);
    
    // Skip common system and launcher processes
    static const char* skipList[] = {
        "cmd.exe", "powershell.exe", "conhost.exe", "explorer.exe",
        "steam.exe", "steamwebhelper.exe", "gameoverlayui.exe",
        "crashpad_handler.exe", "vc_redist", "setup", "install",
        "launcher.exe", "bootstrapper.exe", "updater.exe",
        "epicwebhelper.exe", "eadesktop.exe", "origin.exe",
        "upc.exe", "uplay.exe", "galaxyclient.exe",
        nullptr
    };
    
    for (int i = 0; skipList[i] != nullptr; i++) {
        if (lowerName.find(skipList[i]) != std::string::npos) {
            return false;
        }
    }
    
    // Only inject into .exe files
    if (lowerName.length() < 4 || lowerName.substr(lowerName.length() - 4) != ".exe") {
        return false;
    }
    
    return true;
}

// Hooked CreateProcessA - Inject into whitelisted child processes only
BOOL WINAPI HookedCreateProcessA(LPCSTR lpApp, LPSTR lpCmd, LPSECURITY_ATTRIBUTES lpPA, LPSECURITY_ATTRIBUTES lpTA, BOOL bInherit, DWORD dwFlags, LPVOID lpEnv, LPCSTR lpDir, LPSTARTUPINFOA lpSI, LPPROCESS_INFORMATION lpPI) {
    const char* exePath = lpApp ? lpApp : lpCmd;
    bool shouldInject = ShouldInjectChild(exePath);
    
    DWORD modifiedFlags = shouldInject ? (dwFlags | CREATE_SUSPENDED) : dwFlags;
    BOOL result = OriginalCreateProcessA(lpApp, lpCmd, lpPA, lpTA, bInherit, modifiedFlags, lpEnv, lpDir, lpSI, lpPI);
    
    if (result && lpPI && shouldInject) {
        HookLog("[ChildInject] CreateProcessA: Whitelisted child: %s", exePath);
        InjectIntoChild(lpPI->hProcess, lpPI->hThread);
    }
    return result;
}

// Hooked CreateProcessW - Inject into whitelisted child processes only
BOOL WINAPI HookedCreateProcessW(LPCWSTR lpApp, LPWSTR lpCmd, LPSECURITY_ATTRIBUTES lpPA, LPSECURITY_ATTRIBUTES lpTA, BOOL bInherit, DWORD dwFlags, LPVOID lpEnv, LPCWSTR lpDir, LPSTARTUPINFOW lpSI, LPPROCESS_INFORMATION lpPI) {
    // Convert wide to narrow for whitelist check
    char exePath[MAX_PATH] = {0};
    if (lpApp) WideCharToMultiByte(CP_UTF8, 0, lpApp, -1, exePath, MAX_PATH, NULL, NULL);
    else if (lpCmd) WideCharToMultiByte(CP_UTF8, 0, lpCmd, -1, exePath, MAX_PATH, NULL, NULL);
    
    bool shouldInject = ShouldInjectChild(exePath);
    
    DWORD modifiedFlags = shouldInject ? (dwFlags | CREATE_SUSPENDED) : dwFlags;
    BOOL result = OriginalCreateProcessW(lpApp, lpCmd, lpPA, lpTA, bInherit, modifiedFlags, lpEnv, lpDir, lpSI, lpPI);
    
    if (result && lpPI && shouldInject) {
        HookLog("[ChildInject] CreateProcessW: Whitelisted child: %s", exePath);
        InjectIntoChild(lpPI->hProcess, lpPI->hThread);
    }
    return result;
}

std::mutex g_HookMutex;
HANDLE g_hCheckHooksEvent = NULL;

// Forward declaration
void CheckAndInstallHooks();
// DLL Redirection Helper
// DLL Redirection Helper
std::string GetRedirectedPath(const std::string& requestedPath) {
    if (requestedPath.empty()) return "";

    try {
        std::filesystem::path path(requestedPath);
        std::string filename = path.filename().string();
        std::string filenameLower = filename;
        std::transform(filenameLower.begin(), filenameLower.end(), filenameLower.begin(), ::tolower);

        std::string overridePath;

        // 0. Custom Detours (Removed)
        // auto it = g_LocalConfig.graphics.customFileDetours.find(filenameLower);
        // if (it != g_LocalConfig.graphics.customFileDetours.end()) {
        //      overridePath = it->second;
        // }

        // 1. DLSS/Streamline Logic - Only if no custom detour set
        if (overridePath.empty() && g_pLocalConfig) {
            if (filenameLower == "nvngx_dlss.dll") {
                overridePath = g_pLocalConfig->graphics.dlssSrDllPath;
            } 
            // 2. DLSS Frame Generation
            else if (filenameLower == "nvngx_dlssg.dll") {
                overridePath = g_pLocalConfig->graphics.dlssFgDllPath;
            } 
            // 3. DLSS Ray Reconstruction (Denoiser)
            else if (filenameLower == "nvngx_dlssd.dll") {
                overridePath = g_pLocalConfig->graphics.dlssRrDllPath;
            }
            // 4. Streamline and related components
            else if (filenameLower.find("sl.") == 0 || 
                     filenameLower == "nvngx_deepdvc.dll" || 
                     filenameLower == "nvlowlatencyvk.dll") {
                overridePath = g_pLocalConfig->graphics.streamlineDllPath;
            }
        }

        if (!overridePath.empty()) {
            std::filesystem::path cfgPath(overridePath);
            std::filesystem::path finalPath;

            // Simple Heuristic: 
            // If the configured path has an extension (like .dll), assume it's a full file path.
            // But if it's for Streamline, we really want the folder if matching other files.
            // 
            // Universal Logic:
            // 1. If we are replacing "nvngx_dlss.dll", and config is "folder/nvngx_dlss.dll", use it.
            // 2. If config is "folder", append "nvngx_dlss.dll".
            // 3. For Streamline, if config is "file.dll", take parent folder, then append requested filename.

            if (cfgPath.has_extension()) {
                // It looks like a file.
                // If it ends with the SAME filename as requested, just use it.
                std::string cfgFilename = cfgPath.filename().string();
                std::string cfgFilenameLower = cfgFilename;
                std::transform(cfgFilenameLower.begin(), cfgFilenameLower.end(), cfgFilenameLower.begin(), ::tolower);

                if (cfgFilenameLower == filenameLower) {
                     finalPath = cfgPath;
                } else {
                     // Config points to a file, but we want a potentially different file (common in Streamline case)
                     // Or user pointed to "dlss.dll" but we are loading "dlssg.dll" (shouldn't happen with separate configs but safe to handle)
                     finalPath = cfgPath.parent_path() / filename;
                }
            } else {
                // It looks like a directory. Append the requested filename.
                finalPath = cfgPath / filename;
            }
            
            HookLog("Redirecting %s to: %s", filename.c_str(), finalPath.string().c_str());
            return finalPath.string();
        }

    } catch (...) {
        // Fallback
    }
    return "";
}

// Hooked RegQueryValueExW - For DLSS Debug Overlay
LSTATUS WINAPI HookedRegQueryValueExW(HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData) {
    LSTATUS status = OriginalRegQueryValueExW(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
    
    // Check if probing for DLSS Indicator
    if (lpValueName && _wcsicmp(lpValueName, L"ShowDlssIndicator") == 0) {
        // Only if we have a config override
        if (g_pLocalConfig && !g_pLocalConfig->graphics.dlssDebugOverlay.empty() && g_pLocalConfig->graphics.dlssDebugOverlay != "default") {
             // If caller provided buffer to read data
             if (lpData && lpcbData && *lpcbData >= 4) {
                 DWORD* outData = (DWORD*)lpData;
                 if (g_pLocalConfig->graphics.dlssDebugOverlay == "on") {
                     *outData = 0x400; // Force ON
                     // HookLog("RegQueryValueExW: Force-enabled DLSS Indicator");
                 } else if (g_pLocalConfig->graphics.dlssDebugOverlay == "off") {
                     *outData = 0; // Force OFF
                 }
                 return ERROR_SUCCESS; // Pretend we succeeded even if registry key didn't exist
             }
        }
    }
    return status;
}

// Hooked Functions - Signal Event & Redirect
static std::string g_SpoofedCmdLineA;
static std::wstring g_SpoofedCmdLineW;

LPSTR WINAPI HookedGetCommandLineA() {
    LPSTR original = OriginalGetCommandLineA();
    
    // Only spoof if config is loaded and feature forced
    if (g_pLocalConfig && g_pLocalConfig->graphics.forceRayReconstruction) {
        static bool s_Logged = false;
        if (!s_Logged) {
            HookLog("HookedGetCommandLineA called. Original: %s", original ? original : "<null>");
            s_Logged = true;
        }

        if (g_SpoofedCmdLineA.empty()) {
            if (original) g_SpoofedCmdLineA = original;
            
            // Check if argument already exists to avoid duplication
            if (g_SpoofedCmdLineA.find("r.NGX.DLSS.denoisermode") == std::string::npos) {
                 g_SpoofedCmdLineA += " -r.NGX.DLSS.denoisermode=1";
                 // Also force the RR feature cvar just in case (some plugins use this)
                 g_SpoofedCmdLineA += " -r.NGX.DLSS.RayReconstruction=1";
                 HookLog("HookedGetCommandLineA: Appended CVar flags.");
            }
        }
        return (LPSTR)g_SpoofedCmdLineA.c_str();
    }
    return original;
}

LPWSTR WINAPI HookedGetCommandLineW() {
    LPWSTR original = OriginalGetCommandLineW();

    if (g_pLocalConfig && g_pLocalConfig->graphics.forceRayReconstruction) {
        static bool s_Logged = false;
        if (!s_Logged) {
            // wchar conversion for logging
            char buf[2048];
            WideCharToMultiByte(CP_UTF8, 0, original, -1, buf, 2048, NULL, NULL);
            HookLog("HookedGetCommandLineW called. Original: %s", buf);
            s_Logged = true;
        }

        if (g_SpoofedCmdLineW.empty()) {
            if (original) g_SpoofedCmdLineW = original;
            
            if (g_SpoofedCmdLineW.find(L"r.NGX.DLSS.denoisermode") == std::wstring::npos) {
                 g_SpoofedCmdLineW += L" -r.NGX.DLSS.denoisermode=1";
                 g_SpoofedCmdLineW += L" -r.NGX.DLSS.RayReconstruction=1";
                 HookLog("HookedGetCommandLineW: Appended CVar flags.");
            }
        }
        return (LPWSTR)g_SpoofedCmdLineW.c_str();
    }
    return original;
}

// CRT Hook Wrappers
int WINAPI Hooked_getmainargs(int *argc, char ***argv, char ***env, int doWildCard, void *startInfo) {
    int result = Original_getmainargs(argc, argv, env, doWildCard, startInfo);
    if (g_pLocalConfig && g_pLocalConfig->graphics.forceRayReconstruction && result == 0 && *argc > 0) {
        HookLog("Hooked_getmainargs called. Argc=%d", *argc);
        // TODO: Modify argv here if GetCommandLine fails
    }
    return result;
}

int WINAPI Hooked_wgetmainargs(int *argc, wchar_t ***argv, wchar_t ***env, int doWildCard, void *startInfo) {
    int result = Original_wgetmainargs(argc, argv, env, doWildCard, startInfo);
    if (g_pLocalConfig && g_pLocalConfig->graphics.forceRayReconstruction && result == 0 && *argc > 0) {
        HookLog("Hooked_wgetmainargs called. Argc=%d", *argc);
        // Proactive modification: Allocate new argv and append
        // This is risky but standard for this kind of override
        /*
        static std::vector<wchar_t*> newArgv;
        if (newArgv.empty()) {
            for (int i = 0; i < *argc; i++) newArgv.push_back((*argv)[i]);
            static wchar_t* arg1 = _wcsdup(L"-r.NGX.DLSS.denoisermode=1");
            newArgv.push_back(arg1);
            *argv = newArgv.data();
            *argc += 1;
            HookLog("Hooked_wgetmainargs: Injected argument.");
        }
        */
    }
    return result;
}

// Hooked Functions - Signal Event & Redirect
HMODULE WINAPI HookedLoadLibraryA(LPCSTR lpLibFileName) {
    if (lpLibFileName) {
        std::string redirect = GetRedirectedPath(lpLibFileName);
        if (!redirect.empty()) {
            HMODULE hMod = OriginalLoadLibraryA(redirect.c_str());
            if (hMod && g_hCheckHooksEvent) SetEvent(g_hCheckHooksEvent);
            return hMod;
        }
    }
    HMODULE hMod = OriginalLoadLibraryA(lpLibFileName);
    if (hMod && g_hCheckHooksEvent) SetEvent(g_hCheckHooksEvent);
    return hMod;
}

HMODULE WINAPI HookedLoadLibraryW(LPCWSTR lpLibFileName) {
    if (lpLibFileName) {
        // Convert to UTF-8 for check
        char pathUtf8[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, lpLibFileName, -1, pathUtf8, MAX_PATH, NULL, NULL);
        std::string redirect = GetRedirectedPath(pathUtf8);
        
        if (!redirect.empty()) {
            // Convert back to Wide for LoadLibraryW if needed, or just use A?
            // Safer to use W with W
            std::wstring redirectW;
            int len = MultiByteToWideChar(CP_UTF8, 0, redirect.c_str(), -1, NULL, 0);
            if (len > 0) {
                redirectW.resize(len);
                MultiByteToWideChar(CP_UTF8, 0, redirect.c_str(), -1, &redirectW[0], len);
                // Remove null terminator added by resize if strictly needed, but usually LoadLibraryW handles it
                if (redirectW.back() == L'\0') redirectW.pop_back();

                HMODULE hMod = OriginalLoadLibraryW(redirectW.c_str());
                if (hMod && g_hCheckHooksEvent) SetEvent(g_hCheckHooksEvent);
                return hMod;
            }
        }
    }
    HMODULE hMod = OriginalLoadLibraryW(lpLibFileName);
    if (hMod && g_hCheckHooksEvent) SetEvent(g_hCheckHooksEvent);
    return hMod;
}

HMODULE WINAPI HookedLoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    if (lpLibFileName) {
        std::string redirect = GetRedirectedPath(lpLibFileName);
        if (!redirect.empty()) {
             // Use OriginalLoadLibraryA for the redirect to simplify (Ex flags might conflict with absolute path? usually ok)
             // But let's stick to ExA to respect flags if possible, filtering flags that shouldn't apply to absolute path?
             // Actually, usually users just want to load the DLL.
             // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR might be an issue.
             // Let's try to trust the user path is absolute.
             HMODULE hMod = OriginalLoadLibraryExA(redirect.c_str(), hFile, dwFlags); 
             if (hMod && g_hCheckHooksEvent) SetEvent(g_hCheckHooksEvent);
             return hMod;
        }
    }
    HMODULE hMod = OriginalLoadLibraryExA(lpLibFileName, hFile, dwFlags);
    if (hMod && g_hCheckHooksEvent) SetEvent(g_hCheckHooksEvent);
    return hMod;
}

HMODULE WINAPI HookedLoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    if (lpLibFileName) {
        char pathUtf8[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, lpLibFileName, -1, pathUtf8, MAX_PATH, NULL, NULL);
        std::string redirect = GetRedirectedPath(pathUtf8);
        if (!redirect.empty()) {
             std::wstring redirectW;
             int len = MultiByteToWideChar(CP_UTF8, 0, redirect.c_str(), -1, NULL, 0);
             if (len > 0) {
                 redirectW.resize(len);
                 MultiByteToWideChar(CP_UTF8, 0, redirect.c_str(), -1, &redirectW[0], len);
                 if (redirectW.back() == L'\0') redirectW.pop_back();
                 
                 HMODULE hMod = OriginalLoadLibraryExW(redirectW.c_str(), hFile, dwFlags);
                 if (hMod && g_hCheckHooksEvent) SetEvent(g_hCheckHooksEvent);
                 return hMod;
             }
        }
    }
    HMODULE hMod = OriginalLoadLibraryExW(lpLibFileName, hFile, dwFlags);
    if (hMod && g_hCheckHooksEvent) SetEvent(g_hCheckHooksEvent);
    return hMod;
}

// ----------------------------------------------------------------------------
// UE5 CVar Enforcer using Pattern Scanning
// ----------------------------------------------------------------------------
namespace UE5 {
    // IConsoleManager Interface (Virtual Table Reconstruction)
    // We only need FindConsoleVariable.
    
    // Virtual Table Layout (Estimated for UE4/5):
    // 0: Destructor
    // ...
    // FindConsoleVariable is often index 3 (UE4.27 to UE5.3 often consistent)
    // but can vary.
    
    // A safer, albeit more complex, approach is to find the CVar directly by name string scan.
    
    class IConsoleVariable {
    public:
        virtual ~IConsoleVariable() {}
        virtual void Set(const wchar_t* Value, uint32_t SetBy = 0) = 0; 
        virtual void Set(const char* Value, uint32_t SetBy = 0) = 0; 
        virtual void Set(int32_t Value, uint32_t SetBy = 0) = 0;
        virtual void Set(float Value, uint32_t SetBy = 0) = 0;
        // The above is a GUESS. The actual interface has overloads.
        // Usually Set(const TCHAR* InValue, EConsoleVariableSetBy InSetBy) is the main one.
        // EConsoleVariableSetBy: SetByCommandline = 0x00000002.
    };
    
    // We will use a "manual vtable call" helper to avoid interface mismatches.
    template<typename T>
    T GetVFunc(void* instance, int index) {
        uintptr_t* vtable = *((uintptr_t**)instance);
        return (T)vtable[index];
    }
    
    // IConsoleManager::FindConsoleVariable is usually index 3 or 4.
    // IConsoleVariable::Set is usually index 0, 1, or 2 (Set has overloads).
    // Let's assume standard UE4/5 layout:
    // IConsoleManager:
    // 0: ~
    // 1: RegisterConsoleObject
    // 2: UnregisterConsoleObject
    // 3: FindConsoleObject(name)
    // 4: FindConsoleVariable(name) <--- Target
    
    // IConsoleVariable:
    // 0: ~
    // 1: Set(const TCHAR* InValue, uint32 SetBy)
    // 2: ...
    
    typedef void* (*FindConsoleVariable_t)(void* mgr, const wchar_t* name);
    typedef void (*Set_t)(void* cvar, const wchar_t* value, uint32_t setBy);

    void EnforceRR() {
        if (!g_pLocalConfig || !g_pLocalConfig->graphics.forceRayReconstruction) return;

        static uintptr_t s_ConsoleManagerPtr = 0;
        static bool s_AttemptedScan = false;
        
        HMODULE hMain = GetModuleHandleA(NULL);
        if (!hMain) return;

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
                 
                 uint8_t* p = (uint8_t*)refStr;
                 for (int i = 0; i < 100; i++) {
                     // Check for CALL (E8)
                     if (*(p - i) == 0xE8) {
                         // This MIGHT be IConsoleManager::Get()
                         // Let's check where it goes.
                         int32_t offset = *(int32_t*)(p - i + 1);
                         uintptr_t funcAddr = (uintptr_t)(p - i + 5 + offset);
                         
                         // Check if specific function pattern: MOV RAX, [Global]; RET
                         // 48 8B 05 ?? ?? ?? ?? C3
                         if (*(uint8_t*)funcAddr == 0x48 && *(uint8_t*)(funcAddr+1) == 0x8B && *(uint8_t*)(funcAddr+7) == 0xC3) {
                             // Found it!
                             int32_t gOffset = *(int32_t*)(funcAddr + 3);
                             s_ConsoleManagerPtr = funcAddr + 7 + gOffset; // The Global Variable Address
                             HookLog("UE5: Found ConsoleManager singleton at %p (via r.DumpingMovie)", (void*)s_ConsoleManagerPtr);
                             break;
                         }
                     }
                 }
             }
             
             // Strategy 2: If finding Get() failed, try finding GConsoleManager global directly via AOB
             if (!s_ConsoleManagerPtr) {
                 // Generic pattern for "MOV RCX, [GConsoleManager]"
                 // 48 8B 0D ?? ?? ?? ?? 48 85 C9 75 ?? E8
                 uintptr_t aob = Scanner::Scan(hMain, "48 8B 0D ?? ?? ?? ?? 48 85 C9 75 ?? E8");
                 if (aob) {
                     int32_t offset = *(int32_t*)(aob + 3);
                     s_ConsoleManagerPtr = aob + 7 + offset;
                     HookLog("UE5: Found ConsoleManager singleton at %p (via AOB)", (void*)s_ConsoleManagerPtr);
                 }
             }
        }

        if (s_ConsoleManagerPtr) {
            void* mgr = *(void**)s_ConsoleManagerPtr;
            HookLog("UE5: GConsoleManager Value at %p is %p", (void*)s_ConsoleManagerPtr, mgr);
            
            bool safeToUseMgr = false;
            if (mgr) {
                // ... verify vtable ...
                 MEMORY_BASIC_INFORMATION mbi;
                 if (VirtualQuery((void*)mgr, &mbi, sizeof(mbi)) && 
                     (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) {
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
                         if (!fn) continue;
                         
                         // Check if points to executable memory
                         MEMORY_BASIC_INFORMATION mbi;
                         if (VirtualQuery((void*)fn, &mbi, sizeof(mbi))) {
                             if (!(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) {
                                 continue;
                             }
                         }
                         
                         // Try "r.vsync"
                         // Wrap in try/except if possible (not standard C++) but we don't have it.
                         // We rely on memory check.
                         void* check = fn(mgr, L"r.vsync");
                          // If it returns null, it might just be not found?
                          // But r.vsync is standard.
                          // Try "r.DumpingMovie" which we scanned for?
                         if (check) {
                             s_ValidFindIndex = idx;
                             HookLog("UE5: Confirmed FindConsoleVariable at VTable Index %d", idx);
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
                     FindConsoleVariable_t fnFind = GetVFunc<FindConsoleVariable_t>(mgr, s_ValidFindIndex);
                     
                     // 1. Denoiser Mode
                     void* cvarMode = fnFind(mgr, L"r.NGX.DLSS.denoisermode");
                     if (cvarMode) {
                         static Set_t fnSet = nullptr; 
                         if (!fnSet) fnSet = GetVFunc<Set_t>(cvarMode, 1); 
                         
                         if (fnSet) {
                             fnSet(cvarMode, L"1", 0x02); 
                             HookLog("UE5: Force Set r.NGX.DLSS.denoisermode=1");
                         }
                     } else {
                         static bool s_LogOnce = false;
                         if (!s_LogOnce) {
                             HookLog("UE5: CVar 'r.NGX.DLSS.denoisermode' NOT FOUND via Manager.");
                             s_LogOnce = true;
                         }
                     }
                     
                     // 2. Ray Reconstruction
                     void* cvarRR = fnFind(mgr, L"r.NGX.DLSS.RayReconstruction");
                     if (cvarRR) {
                         static Set_t fnSet = nullptr; 
                         if (!fnSet) fnSet = GetVFunc<Set_t>(cvarRR, 1); 
                         
                         if (fnSet) {
                             fnSet(cvarRR, L"1", 0x02); 
                             HookLog("UE5: Force Set r.NGX.DLSS.RayReconstruction=1");
                         }
                     }
                     // If we succeeded here, we can return.
                     // But if CVars were not found, Fallback might find them if Manager lookup is broken?
                     // Unlikely. If Manager is valid, lookup should work.
                }
            }
        }
        

    }
}

// Centralized Hook Detection Logic (Executed by HookThread)
void CheckAndInstallHooks() {
    std::lock_guard<std::mutex> lock(g_HookMutex);
    
    // Initialize wrapper-based hooks (IAT patching for factory creation)
    // This enables MinHook-free hooking of CreateDXGIFactory* etc.
    InitializeWrapperHooks();

    if (!g_DX12Hook && GetModuleHandleA("d3d12.dll")) {
        EarlyLog("Detected d3d12.dll. Installing DX12 hooks...");
        g_DX12Hook = &g_dx12HookInstance; // Use global static instance
        g_DX12Hook->Init(); // Idempotent (safe to call multiple times)
        EarlyLog("DX12 hooks installed");
    }

    // IMPORTANT: Don't install DX11 hooks in DX12 processes.
    // Our hook DLL links against D3D11, so d3d11.dll may be loaded even for DX12 apps.
    // Installing DX11 Present hooks can interfere with DX12 swapchains and crash.
    if (!g_DX11Hook && !GetModuleHandleA("d3d12.dll") &&
        (GetModuleHandleA("d3d11.dll") || GetModuleHandleA("d3d10.dll") || GetModuleHandleA("d3d10_1.dll"))) {
        HookLog("Detected D3D10/11. Installing hooks...");
        g_DX11Hook = new DX11Hook();
        g_DX11Hook->Init();
        HookLog("D3D10/11 hooks installed");
    }

    if (!g_DX9Hook && GetModuleHandleA("d3d9.dll")) {
        HookLog("Detected d3d9.dll. Installing DX9 hooks...");
        g_DX9Hook = new DX9Hook();
        g_DX9Hook->Init();
        HookLog("DX9 hooks installed");
    }

    if (!g_DDrawHook && GetModuleHandleA("ddraw.dll")) {
        HookLog("Detected ddraw.dll. Installing DirectDraw hooks...");
        g_DDrawHook = new DDrawHook();
        g_DDrawHook->Init();
        HookLog("DDraw hooks installed");
    }

    if (!g_DX8Hook && GetModuleHandleA("d3d8.dll")) {
        HookLog("Detected d3d8.dll. Installing DX8 hooks...");
        g_DX8Hook = new DX8Hook();
        g_DX8Hook->Init();
        HookLog("DX8 hooks installed");
    }

    if (!g_OpenGLHook && GetModuleHandleA("opengl32.dll")) {
        HookLog("Detected opengl32.dll. Installing OpenGL hooks...");
        g_OpenGLHook = new OpenGLHook();
        g_OpenGLHook->Init();
        HookLog("OpenGL hooks installed");
    }


#ifdef ENABLE_VULKAN_HOOK
    if (!g_VulkanHook && GetModuleHandleA("vulkan-1.dll")) {
        HookLog("Detected vulkan-1.dll. Installing Vulkan hooks...");
        g_VulkanHook = new VulkanHook();
        g_VulkanHook->Init();
        HookLog("Vulkan hooks installed");
    }
#endif

    // Check for Frame Generation Runtimes (Startup Safety)
    // If FG is loaded, we MUST suspend overlay immediately to avoid startup crashes
    static bool fgDetected = false;
    if (!fgDetected) {
        if (g_FGCompat.DetectLoadedFGRuntime() != FGCompatibility::FGType::None) {
            HookLog("Main: FG Runtime detected via LoadLibrary");
            g_FGCompat.SuspendFor(500);  // Allow FG to initialize before overlay 
            fgDetected = true;
        }
    }

    // Install NGX hooks if DLL is present
    NVNGXHook::Get().Install();
}

DWORD WINAPI HookThread(LPVOID lpParam) {
  // Load Local Config (to support per-app overrides) EARLY
  {
      char dllPath[MAX_PATH];
      GetModuleFileNameA(g_hModule, dllPath, MAX_PATH);
      std::string pathString = dllPath;
      std::string dir = pathString.substr(0, pathString.find_last_of("\\/"));
      std::string configPath = dir + "\\config.ini";
      
      if (!g_pLocalConfig) g_pLocalConfig = new AppConfig();
      LoadConfig(configPath, *g_pLocalConfig);
      // Prime the graphics override state immediately
      GetActiveGraphicsConfig();
  }

  // FAST PATH: Install IAT wrappers immediately before anything else
  // This helps catch early startup API calls in fast-loading processes.
  InitializeWrapperHooks();
  CheckAndInstallHooks();
  
  EarlyLog("HookThread: Started (PID=%d)", GetCurrentProcessId());

  // Create Event for Async Hook Checks
  g_hCheckHooksEvent = CreateEvent(NULL, FALSE, FALSE, NULL); // Auto-reset
  if (!g_hCheckHooksEvent) {
      // Logic without event...
  }

  // --- BLACKLISTED PROCESSES ---
  if (g_ProcessCategory == ProcessCategory::Blacklisted) {
      if (g_hCheckHooksEvent) CloseHandle(g_hCheckHooksEvent);
      FreeLibraryAndExitThread(g_hModule, 0);
      return 0;
  }

  // --- LAUNCHERS ---
  if (g_ProcessCategory == ProcessCategory::Launcher) {
      // launchers only need CreateProcess hooks. No IPC, no graphics.
      // Use IAT patching (MinHook-free)
      OriginalCreateProcessA = (CreateProcessA_t)GetProcAddress(
          GetModuleHandleA("kernel32.dll"), "CreateProcessA");
      OriginalCreateProcessW = (CreateProcessW_t)GetProcAddress(
          GetModuleHandleA("kernel32.dll"), "CreateProcessW");
      
      void* dummy;
      IATHook::PatchIATAllModules("kernel32.dll", "CreateProcessA", 
          (void*)&HookedCreateProcessA, &dummy);
      IATHook::PatchIATAllModules("kernel32.dll", "CreateProcessW", 
          (void*)&HookedCreateProcessW, &dummy);
      
      // launchers don't have an IPC loop, they just stay alive to hook child processes
      // We still need to unload eventually if we want perfect cleanup, 
      // but for launchers it's safer to just stay loaded until process exit 
      // to avoid missing a CreateProcess call during transition.
      // However, we need to check for shutdown signal to allow DLL unload.
      // Use 100ms instead of 1000ms to respond quickly to shutdown.
      while (!g_ShuttingDown) {
          Sleep(100);
      }
      return 0; 
  }

  // --- POTENTIAL GAMES ---
  EarlyLog("HookThread: Potential game detected. Watchdog started.");

  // Config was already loaded early in HookThread
  
  // Track last write time for Hot Reloading
  FILETIME g_ConfigLastWriteTime = {};
  {
      char dllPath[MAX_PATH];
      GetModuleFileNameA(g_hModule, dllPath, MAX_PATH);
      std::string pathString = dllPath;
      std::string dir = pathString.substr(0, pathString.find_last_of("\\/"));
      std::string configPath = dir + "\\config.ini";
      
      WIN32_FILE_ATTRIBUTE_DATA fileInfo;
      if (GetFileAttributesExA(configPath.c_str(), GetFileExInfoStandard, &fileInfo)) {
          g_ConfigLastWriteTime = fileInfo.ftLastWriteTime;
      }
  }

  // Init IPC loop
  g_IPC = new IPCClient();
  
  if (g_isSkippedProcess) {
      // EarlyLog removed from here to prevent file locks in system processes
      while (true) {
          bool hasGraphicsAPI = (
              GetModuleHandleA("d3d12.dll") != NULL ||
              GetModuleHandleA("d3d11.dll") != NULL ||
              GetModuleHandleA("d3d10.dll") != NULL ||
              GetModuleHandleA("d3d9.dll") != NULL ||
              GetModuleHandleA("d3d8.dll") != NULL ||
              GetModuleHandleA("ddraw.dll") != NULL ||
              GetModuleHandleA("opengl32.dll") != NULL ||
              GetModuleHandleA("vulkan-1.dll") != NULL
          );

          if (hasGraphicsAPI) {
              EarlyLog("HookThread: [%s] Late graphics API detection! Transitioning to game mode.", g_ProcessName);
              g_isSkippedProcess = false;
              break; 
          }

          if (g_IPC->Connect()) {
              Sleep(1000); // 1s is aggressive enough without being a CPU hog/bomb
          } else {
              // Engine not found or closed - time to exit
              if (g_hCheckHooksEvent) CloseHandle(g_hCheckHooksEvent);
              FreeLibraryAndExitThread(g_hModule, 0);
              return 0;
          }
      }
  }

  EarlyLog("HookThread: [%s] IPCClient created, attempting connect...", g_ProcessName);
  if (g_IPC->Connect()) {
    EarlyLog("HookThread: IPC Connected successfully!");
    HookLog("IPC Connected successfully!");
    
    if (g_IPC->GetSharedMem()) {
        g_IPC->GetSharedMem()->sourcePid = GetCurrentProcessId();
    }
  } else {
    EarlyLog("HookThread: IPC Connection FAILED!");
    HookLog("IPC Connection FAILED!");
  }

  // Use IAT patching instead of MinHook for kernel32/advapi32 hooks
  EarlyLog("HookThread: Initializing IAT-based kernel32 hooks...");

  // Install LoadLibrary and CreateProcess hooks via IAT patching
  HookLog("Installing LoadLibrary/CreateProcess hooks via IAT patching (MinHook-free)...");
  
  IATHook::InitializeKernel32Hooks(
      (void*)&HookedLoadLibraryA, (void**)&OriginalLoadLibraryA,
      (void*)&HookedLoadLibraryW, (void**)&OriginalLoadLibraryW,
      (void*)&HookedLoadLibraryExA, (void**)&OriginalLoadLibraryExA,
      (void*)&HookedLoadLibraryExW, (void**)&OriginalLoadLibraryExW,
      (void*)&HookedCreateProcessA, (void**)&OriginalCreateProcessA,
      (void*)&HookedCreateProcessW, (void**)&OriginalCreateProcessW);
  
  // Install RegQueryValueExW for DLSS Debug Overlay
  if (GetModuleHandleA("advapi32.dll")) {
      HookLog("Installing RegQueryValueExW hook via IAT patching...");
      IATHook::InitializeAdvapi32Hooks(
          (void*)&HookedRegQueryValueExW, (void**)&OriginalRegQueryValueExW);
  } else {
      HookLog("advapi32.dll not loaded yet - skipping RegQueryValueExW hook");
  }
  
  EarlyLog("HookThread: IAT hooks initialized OK");
  HookLog("All kernel32/advapi32 hooks enabled (IAT mode)");

  // Initial Check
  // CheckAndInstallHooks moved to start of HookThread for faster response
  // EarlyLog("HookThread: About to call CheckAndInstallHooks (d3d12=%p)", GetModuleHandleA("d3d12.dll"));
  // CheckAndInstallHooks();
  // EarlyLog("HookThread: CheckAndInstallHooks returned");

  EarlyLog("HookThread: All hooks installed, entering exit monitor loop");

  // Monitor Loop - Waits for Event OR Timeout (for Exit Checks)
  while (true) {
    // Wait for event (signaled by LoadLibrary) or timeout (100ms)
    DWORD waitResult = WAIT_TIMEOUT;
    if (g_hCheckHooksEvent) {
        waitResult = WaitForSingleObject(g_hCheckHooksEvent, 100);
    } else {
        Sleep(100);
    }
    
    DWORD now = GetTickCount();

    // Periodically update active graphics config state 
    // This ensures g_GraphicsOverridesActive is updated even if no hooks are calling it yet
    GetActiveGraphicsConfig();
    
    // --- UE5 Enforce RR ---
    static DWORD s_LastRRCheck = 0;
    if (now - s_LastRRCheck > 2000) {
        s_LastRRCheck = now;
        UE5::EnforceRR(); 
    }
    
    // --- Hot Reloading ---
    static int hotReloadTick = 0;
    if (++hotReloadTick > 10) { // Check every 1s (10 * 100ms)
        hotReloadTick = 0;
        char dllPath[MAX_PATH];
        GetModuleFileNameA(g_hModule, dllPath, MAX_PATH);
        std::string pathString = dllPath;
        std::string dir = pathString.substr(0, pathString.find_last_of("\\/"));
        std::string configPath = dir + "\\config.ini";
        
        WIN32_FILE_ATTRIBUTE_DATA fileInfo;
        if (GetFileAttributesExA(configPath.c_str(), GetFileExInfoStandard, &fileInfo)) {
            if (CompareFileTime(&fileInfo.ftLastWriteTime, &g_ConfigLastWriteTime) != 0) {
                 // Update timestamp first to avoid loop if load fails
                 g_ConfigLastWriteTime = fileInfo.ftLastWriteTime;
                 
                 if (g_pLocalConfig) LoadConfig(configPath, *g_pLocalConfig);
                 // Force override update
                 GetActiveGraphicsConfig();
                 
                 HookLog("Hot Reload: Config updated from file!");
            }
        }
    }
    
    // If signaled OR timeout, we check logic
    // (Timeout is needed for Exit/IPC checks)
    
    if (waitResult == WAIT_OBJECT_0) {
        // Event signaled - run detection
        CheckAndInstallHooks();
    }
    
    // Check for recording state changes
    static bool s_WasRecording = false;
    bool isRecording = false;
    if (g_IPC && g_IPC->IsRecording()) {
        isRecording = true;
    }
    
    if (isRecording != s_WasRecording) {
        s_WasRecording = isRecording;
        CaptureManager::Get().SetCaptureEnabled(isRecording);
        HookLog("Capture state changed: %s", isRecording ? "ENABLED" : "DISABLED");
    }
    
    // Always check for exit/IPC maintenance on every loop iteration
    bool shouldExit = false;
    uint32_t hostPID = 0;
    
    if (g_IPC && g_IPC->GetSharedMem()) {
        shouldExit = g_IPC->GetSharedMem()->requestExit;
        hostPID = g_IPC->GetSharedMem()->hostPID;
    }
    
    if (shouldExit) {
      EarlyLog("HookThread: Exit requested by host");
      HookLog("Exit requested by host");
      break;
    }
    
    if (hostPID != 0) {
      HANDLE hHost = OpenProcess(SYNCHRONIZE, FALSE, hostPID);
      if (hHost) {
        DWORD waitResultHost = WaitForSingleObject(hHost, 0); // Immediate check
        CloseHandle(hHost);
        if (waitResultHost == WAIT_OBJECT_0) {
          EarlyLog("HookThread: Host process died");
          HookLog("Host process died. Cleaning up...");
          break;
        }
      } else {
        if (g_IPC->GetSharedMem()) {
            EarlyLog("HookThread: Can't open host process, assuming dead");
            HookLog("Host process inaccessible. Exiting...");
            break;
        }
      }
    } else {
      // Reconnect logic
      if (g_IPC) {
        if (g_IPC->Connect()) {
          EarlyLog("HookThread: Reconnected to new host");
          HookLog("IPC Reconnected to new captureengine instance!");
          CheckAndInstallHooks();
        } else {
           // Host not found, maybe it closed?
           // Target games get a longer grace period (30s) before self-unloading
           static int missedHeartbeats = 0;
           if (++missedHeartbeats > 300) { // 30s at 100ms per loop
               HookLog("HookThread: Host lost for 30s. Self-unloading...");
               break;
           }
        }
      }
    }
  }

  // Cleanup Event
  if (g_hCheckHooksEvent) CloseHandle(g_hCheckHooksEvent);

  // Self-unload to release file lock when host requests exit or dies
  // This is crucial for the CBT global hook to not pin the DLL forever
  FreeLibraryAndExitThread(g_hModule, 0);
  return 0;
}

// Helper for QueueUserWorkItem (requires DWORD return, LPVOID param)
static DWORD WINAPI HookThreadWrapper(LPVOID lpParam) {
  timeBeginPeriod(1); 
  return HookThread(lpParam);
}

// Unload thread for blacklisted processes - releases DLL file lock
static DWORD WINAPI UnloadSelfThread(LPVOID) {
    Sleep(100); // Small delay to let DllMain complete
    FreeLibraryAndExitThread(g_hModule, 0);
    return 0;
}

extern "C" __declspec(dllexport) LRESULT CALLBACK CBTHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// Thread to safely eject the DLL




static bool isProcessWhitelistedFast(const char* name) {
    if (!name) return false;

    // 1. Internal Whitelist
    if (_stricmp(name, "captureengine.exe") == 0 || _stricmp(name, "captureengine_x86.exe") == 0) {
        return true;
    }
    
    // 2. Shared Memory Whitelist Cache (Fastest & Safest)
    // Reliance on Shared Memory avoids Disk I/O in DllMain.
    HANDLE hDisc = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
    if (hDisc) {
        DiscoveryInfo* pDisc = (DiscoveryInfo*)MapViewOfFile(hDisc, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));
        bool found = false;
        if (pDisc) {
            if (pDisc->magic == DISCOVERY_MAGIC) {
                const char* p = pDisc->processWhitelist;
                const char* end = pDisc->processWhitelist + sizeof(pDisc->processWhitelist);
                
                while (p < end && *p != '\0') {
                    if (_stricmp(name, p) == 0) {
                        found = true;
                        break;
                    }
                    p += strlen(p) + 1;
                }
            }
            UnmapViewOfFile(pDisc);
        }
        CloseHandle(hDisc);
        if (found) return true;
    }
    
    // Config.ini fallback removed from DllMain - safer to stay dormant if 
    // CaptureEngine hasn't explicitly whitelisted via Shared Memory yet.
    return false;
}

// Helper: Identify Service Processes for Safe Unload
static bool IsServiceProcess(const char* name) {
    if (!name) return false;
    // These processes are safe to unload from (services, non-interactive).
    // Returning FALSE in DllMain allows the OS to unload us cleanly.
    return (_stricmp(name, "svchost.exe") == 0 ||
            _stricmp(name, "lsass.exe") == 0 ||
            _stricmp(name, "services.exe") == 0 ||
            _stricmp(name, "smss.exe") == 0 ||
            _stricmp(name, "wininit.exe") == 0 ||
            _stricmp(name, "csrss.exe") == 0 ||
            _stricmp(name, "conhost.exe") == 0 ||
            _stricmp(name, "dllhost.exe") == 0 || // COM Surrogate
            _stricmp(name, "sihost.exe") == 0 ||
            _stricmp(name, "pwahelper.exe") == 0 ||
            _stricmp(name, "PerfWatson.exe") == 0 ||
            _stricmp(name, "DataExchangeHost.exe") == 0 ||
            _stricmp(name, "GamebarFTServer.exe") == 0 ||
            _stricmp(name, "ApplicationFrameHost.exe") == 0); // SpecialK lists this
}

extern "C" BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD ul_reason_for_call,
                       LPVOID lpReserved) {
  if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
    g_hModule = hinstDLL;
    DisableThreadLibraryCalls(hinstDLL);

    char fullPath[MAX_PATH] = {0};
    char* fileName = (char*)"unknown";
    if (GetModuleFileNameA(NULL, fullPath, MAX_PATH)) {
        char* fileLastSlash = strrchr(fullPath, '\\');
        fileName = fileLastSlash ? (fileLastSlash + 1) : fullPath;
        strncpy(g_ProcessName, fileName, sizeof(g_ProcessName) - 1);
        g_ProcessName[sizeof(g_ProcessName) - 1] = '\0';
    }

    // 1. SAFE UNLOAD: Services and non-interactive helpers
    // These processes should unload the DLL immediately and cleanly.
    if (IsServiceProcess(fileName)) {
        return FALSE; // Detach immediately
    }

    // 2. DORMANT MODE: Shell, Critical UI, and Internal processes
    // These MUST stay loaded to avoid the "Unload Loop" (repeated injection),
    // but they must remain completely inert.
    if (_stricmp(fileName, "explorer.exe") == 0 || 
        _stricmp(fileName, "dwm.exe") == 0 ||
        _stricmp(fileName, "winlogon.exe") == 0 ||
        _stricmp(fileName, "captureengine.exe") == 0 ||
        _stricmp(fileName, "captureengine_x86.exe") == 0) {
        
        g_isDormant = true;
        return TRUE; // Stay loaded but totally inert
    }

    // 2. WHITELIST CHECK: Fast & Inert
    if (isProcessWhitelistedFast(fileName)) {
        // Whitelisted Game (Shared Mem OR Config - but we only use ShMem now in WhitelistFast)
        if (!g_pLocalConfig) {
            g_pLocalConfig = new AppConfig();
        }
        _putenv("FERMI_UNOPT_LOD_SPREAD=1");
        _putenv("NIAGARA_UNOPT_LOD_SPREAD=1");
        EarlyLog("DllMain: Process '%s' is a Whitelisted Game", fileName);
    } else {
        // Not whitelisted - assume blacklist
        g_ProcessCategory = ProcessCategory::Blacklisted;
        g_isDormant = true;
        
        // NO Self-Unload thread anymore. It causes crashes during CBT callbacks.
        // NO Adaptive Strategy. Just stay dormant.
        return TRUE;
    }

    if (g_isDormant) {
        // Silent return
        return TRUE;
    }


    if (g_ProcessCategory == ProcessCategory::PotentialGame) {
        if (!(GetModuleHandleA("d3d12.dll") || GetModuleHandleA("d3d11.dll") ||
              GetModuleHandleA("d3d9.dll") || GetModuleHandleA("vulkan-1.dll") ||
              GetModuleHandleA("opengl32.dll") || GetModuleHandleA("d3d8.dll"))) {
            g_isSkippedProcess = true;
            EarlyLog("DllMain: Process '%s' skipped (No Graphics API modules found)", fileName);
        }
    }

    if (g_ProcessCategory != ProcessCategory::InternalTool) {
        // CRITICAL: Only initialize hooks and dependencies if we have a graphics API.
        // For processes without graphics APIs (launchers, tools, etc.), we skip hook
        // installation entirely to prevent crashes in non-whitelisted processes.
        bool hasGraphicsAPI = (
            GetModuleHandleA("d3d12.dll") != NULL ||
            GetModuleHandleA("d3d11.dll") != NULL ||
            GetModuleHandleA("d3d10.dll") != NULL ||
            GetModuleHandleA("d3d9.dll") != NULL ||
            GetModuleHandleA("vulkan-1.dll") != NULL ||
            GetModuleHandleA("opengl32.dll") != NULL ||
            GetModuleHandleA("d3d8.dll") != NULL ||
            GetModuleHandleA("ddraw.dll") != NULL
        );

        if (hasGraphicsAPI) {
            // Pre-load dependencies (d3d12_wrappers) from our own directory
            // This is CRITICAL for 32-bit where we need the absolute path.
            char modulePath[MAX_PATH];
            if (GetModuleFileNameA(hinstDLL, modulePath, MAX_PATH)) {
                 std::string path(modulePath);
                 size_t lastSlash = path.find_last_of("\\/");
                 if (lastSlash != std::string::npos) {
                     std::string dir = path.substr(0, lastSlash);

#ifdef _WIN64
                     std::string wrapperDll = dir + "\\d3d12_wrappers.dll";
#else
                     std::string wrapperDll = dir + "\\d3d12_wrappers_x86.dll";
#endif

                     EarlyLog("DllMain: Pre-loading wrapper DLL from %s", wrapperDll.c_str());

                     UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS);
                     HMODULE hWrapper = LoadLibraryA(wrapperDll.c_str());
                     if (!hWrapper) {
                         EarlyLog("DllMain: Failed to load wrapper DLL! Err=%d", GetLastError());
                     } else {
                         EarlyLog("DllMain: Successfully loaded wrapper DLL at %p", hWrapper);
                     }
                     SetErrorMode(oldMode);
                 }
            }

            // RACE CONDITION FIX: Initialize wrappers (IAT patching) immediately in DllMain.
            // Waiting for HookThread is too late for fast-starting 32-bit apps like dx12_test.
            // InitializeWrapperHooks is safe to call multiple times (has internal guard)
            // and IAT patching is generally safe in DllMain (no GetModuleHandle loop for *other* modules yet, just specific ones).
            // SAFETY: Only do this for processes with graphics APIs to avoid hooking non-target processes.
            EarlyLog("DllMain: Initializing IAT hooks immediately to prevent race conditions...");
            InitializeWrapperHooks();
        } else {
            EarlyLog("DllMain: Skipping hook initialization (no graphics API detected)");
        }

        EarlyLog("DllMain: Spawning HookThread for '%s'", fileName);
        HANDLE hThread = CreateThread(NULL, 0, HookThreadWrapper, NULL, 0, NULL);
        if (hThread) {
            SetThreadPriority(hThread, THREAD_PRIORITY_HIGHEST);
            CloseHandle(hThread);
        }
    }
    
    return TRUE;
  } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
    // CRITICAL FIX: If we never initialized (blacklisted/dormant), 
    // we MUST NOT execute cleanup logic (Sleep, Locks, etc.)
    // Attempting to sleep or wait on locks in a blacklisted process during shutdown
    // can cause deadlocks or crashes (e.g. Brave/Chrome watchdog).
    if (g_isDormant) {
        return TRUE;
    }

    g_ShuttingDown = true;
    
    // Signal HookThread to exit and give it time to finish
    // Now that launcher loop uses 100ms Sleep, 200ms grace is enough
    if (g_hCheckHooksEvent) {
      SetEvent(g_hCheckHooksEvent);
    }
    Sleep(200); // Increased grace period for threads using 100ms sleep loops
    
    // Shutdown API hooks first to stop new frames
    if (g_DX12Hook) { g_DX12Hook->Shutdown(); g_DX12Hook = nullptr; } // No delete for static instance
    // SafeShutdownHook(g_DX12Hook); // DO NOT USE - double free risk
    SafeShutdownHook(g_DX11Hook);
    SafeShutdownHook(g_DX9Hook);
    SafeShutdownHook(g_DDrawHook);
    SafeShutdownHook(g_DX8Hook);
    SafeShutdownHook(g_OpenGLHook);
#ifdef ENABLE_VULKAN_HOOK
    SafeShutdownHook(g_VulkanHook);
#endif

    IATHook::ShutdownIATHooks();
    
    // Shutdown SystemMetricsCollector BEFORE deleting g_IPC
    // The metrics thread accesses g_IPC, so we must stop it first
    SystemMetricsCollector::Get().Shutdown();
    
    // Now safe to delete g_IPC after thread has exited
    if (g_IPC) {
      delete g_IPC;
      g_IPC = nullptr;
    }
    timeEndPeriod(1);
    
    if (g_hCheckHooksEvent) CloseHandle(g_hCheckHooksEvent);
  }
  return TRUE;
}
