// CaptureEngine Multi-Process Architecture
// Main entry point - acts as Controller when run without --mode flag
// Dispatches to Inject, Media, or Limiter process based on --mode=<mode>

#include "../common/config.h"
#include "../common/crash_handler.h"
#include "../common/logging.h"
#include "../common/process_ipc.h"
#include "tray.h"
#include <Windows.h>
#include <shellapi.h>
#include <atomic>
#include <string>
#include <string>
#include <timeapi.h>
#include <winreg.h>
#include "injection.h"

#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif

// Static initializer to set FFmpeg DLL directory BEFORE any DLLs are loaded
// This runs at program startup before WinMain
namespace {
struct FFmpegDllPathInitializer {
  FFmpegDllPathInitializer() {
    // Get exe directory
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    std::string exePath = buffer;
    std::string baseDir = exePath.substr(0, exePath.find_last_of("\\/"));
    std::string ffmpegDir = baseDir + "\\ffmpeg";
    
    // Convert to wide string for AddDllDirectory
    std::wstring ffmpegDirW(ffmpegDir.begin(), ffmpegDir.end());
    
    // Enable extended DLL search and add our ffmpeg folder
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    AddDllDirectory(ffmpegDirW.c_str());
  }
};
static FFmpegDllPathInitializer g_ffmpegDllInitializer;
} // namespace

// Forward declarations for process entry points
extern int InjectProcessMain(const AppConfig &config);
extern int MediaProcessMain(const AppConfig &config);
extern int LimiterProcessMain(const AppConfig &config);
extern int LoggerProcessMain(const AppConfig &config);
extern int SensorProcessMain(const AppConfig &config);

// Hotkey IDs
#define HOTKEY_ID_RECORD 1

// Controller state
static bool g_Running = true;
static bool g_Recording = false;
static AppConfig g_Config;
static std::string g_ConfigPath;

// Auto-record feature for autonomous testing
static bool g_AutoRecordEnabled = false;
static DWORD g_AutoRecordDelayMs = 3000;     // Delay before starting
static DWORD g_AutoRecordDurationMs = 10000; // Recording duration
static DWORD g_AutoRecordStartTime = 0;

// Deferred game launch (for --launch mode)
static std::string g_DeferredLaunchPath;

// Child process handles
static HANDLE g_hInjectProcess = NULL;
static HANDLE g_hMediaProcess = NULL;
static HANDLE g_hLimiterProcess = NULL;
static HANDLE g_hLoggerProcess = NULL;
static HANDLE g_hSensorProcess = NULL;

// IPC clients for child processes
static std::unique_ptr<ProcessIPCClient> g_InjectClient;
static std::unique_ptr<ProcessIPCClient> g_MediaClient;
static std::unique_ptr<ProcessIPCClient> g_LimiterClient;

static TrayIcon *g_Tray = nullptr;

static void PurgeAllLogsInDir(const std::string& logsDir) {
  CreateDirectoryA(logsDir.c_str(), NULL);
  const char* patterns[] = {"\\*.log", "\\*.csv"};
  for (int p = 0; p < 2; p++) {
    std::string pattern = logsDir + patterns[p];
    WIN32_FIND_DATAA ffd;
    HANDLE hFind = FindFirstFileA(pattern.c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE) continue;
    do {
      if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      std::string filePath = logsDir + "\\" + ffd.cFileName;
      DeleteFileA(filePath.c_str());
    } while (FindNextFileA(hFind, &ffd));
    FindClose(hFind);
  }
}

// Launch game suspended and inject immediately (The only way to guarantee API overrides)
// If the target looks like a launcher (not the actual game exe), we just start it normally
// and let WMI + CreateProcess hooks in already-injected processes catch the real game
void LaunchGameSuspended(const std::string& path) {
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    
    // Strip surrounding quotes if present
    std::string cleanPath = path;
    if (cleanPath.length() >= 2 && cleanPath.front() == '"' && cleanPath.back() == '"') {
        cleanPath = cleanPath.substr(1, cleanPath.length() - 2);
    }
    
    // Extract filename
    size_t lastSlash = cleanPath.find_last_of("\\/");
    std::string filename = (lastSlash != std::string::npos) ? cleanPath.substr(lastSlash + 1) : cleanPath;
    
    // Convert to lowercase
    std::string lowerName;
    for (char c : filename) lowerName += (char)tolower(c);
    
    // Check if this is likely a launcher (not the game itself)
    // Heuristic: if filename doesn't contain _dx11, _dx12, _vulkan, etc., it might be a launcher
    bool looksLikeLauncher = (lowerName.find("_dx") == std::string::npos && 
                              lowerName.find("_vulkan") == std::string::npos &&
                              lowerName.find("_vk") == std::string::npos &&
                              lowerName.find("game") == std::string::npos);
    
    // Extract directory
    std::string dir = cleanPath.substr(0, cleanPath.find_last_of("\\/"));
    
    if (looksLikeLauncher) {
        // This looks like a launcher - start it NORMALLY, no injection
        // We'll rely on WMI to catch the actual game
        LogInfo("[Launcher] Detected launcher (not game): %s - Starting normally", cleanPath.c_str());
        
        if (CreateProcessA(cleanPath.c_str(), NULL, NULL, NULL, FALSE, 0, NULL, dir.c_str(), &si, &pi)) {
            LogInfo("[Launcher] Launcher started (PID: %d). WMI will catch the game.", pi.dwProcessId);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else {
            LogError("[Launcher] Failed to start launcher: %d", GetLastError());
        }
    } else {
        // This looks like the actual game - use suspended injection
        LogInfo("[Launcher] Detected game: %s - Launching Suspended", cleanPath.c_str());
        
        if (CreateProcessA(cleanPath.c_str(), NULL, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, dir.c_str(), &si, &pi)) {
            LogInfo("[Launcher] Process Created (PID: %d). Injecting...", pi.dwProcessId);
            
            // Use InjectionManager to inject
            InjectionManager injector(g_Config);
            if (injector.Inject(pi.dwProcessId, cleanPath)) {
                 LogInfo("[Launcher] Injection Successful. Resuming Thread.");
                 ResumeThread(pi.hThread);
            } else {
                 LogInfo("[Launcher] Early Injection failed (likely WoW64 loader). Resuming and retrying...");
                 ResumeThread(pi.hThread);
                 
                 // Fallback: Aggressive polling for 2 seconds to catch it as soon as kernel32 loads
                 bool injected = false;
                 DWORD start = GetTickCount();
                 while (GetTickCount() - start < 2000) {
                     if (injector.Inject(pi.dwProcessId, cleanPath)) {
                         injected = true;
                         break;
                     }
                     Sleep(10);
                 }
                 
                 if (injected) {
                     LogInfo("[Launcher] Late Injection Successful.");
                 } else {
                     LogError("[Launcher] Late Injection FAILED. Game running without hooks.");
                 }
            }
            
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else {
            LogError("[Launcher] Failed to CreateProcess: %d", GetLastError());
        }
    }
}

// Connect to child processes with retry
bool ConnectToChildProcesses(DWORD timeoutMs) {
  DWORD startTime = GetTickCount();

  while (GetTickCount() - startTime < timeoutMs) {
    bool allConnected = true;

    if (g_hInjectProcess && !g_InjectClient->IsConnected()) {
      if (!g_InjectClient->Connect(100))
        allConnected = false;
    }
    if (g_hMediaProcess && !g_MediaClient->IsConnected()) {
      if (!g_MediaClient->Connect(100))
        allConnected = false;
    }
    if (g_hLimiterProcess && !g_LimiterClient->IsConnected()) {
      if (!g_LimiterClient->Connect(100))
        allConnected = false;
    }
    // Note: Logger and Sensors don't use pipe IPC yet, they use shared memory/files
    
    if (allConnected)
      return true;
    Sleep(100);
  }

  return false;
}

// Send command to all child processes
void SendCommandToAll(ProcessCommand cmd) {
  if (g_InjectClient && g_InjectClient->IsConnected()) {
    g_InjectClient->SendCommand(cmd);
  }
  if (g_MediaClient && g_MediaClient->IsConnected()) {
    g_MediaClient->SendCommand(cmd);
  }
  if (g_LimiterClient && g_LimiterClient->IsConnected()) {
    g_LimiterClient->SendCommand(cmd);
  }
}

// Toggle recording - controller notifies inject which sets shared memory
// Media process polls shared memory flags - more reliable than pipe IPC
void ToggleRecording() {
  g_Recording = !g_Recording;

  if (g_Recording) {
    LogInfo("[Controller] Starting recording...");

    // Notify inject process - it sets shared memory flags and media polls them
    if (g_InjectClient && g_InjectClient->IsConnected()) {
      ProcessResponse resp;
      if (g_InjectClient->SendCommand(ProcessCommand::StartRecording, nullptr,
                                      &resp, 5000)) {
        LogInfo("[Controller] Recording started");
      } else {
        LogError(
            "[Controller] Failed to notify inject process - retrying once");
        // Retry once on failure
        if (g_InjectClient->SendCommand(ProcessCommand::StartRecording, nullptr,
                                        &resp, 5000)) {
          LogInfo("[Controller] Recording started (retry)");
        } else {
          LogError("[Controller] Recording start failed");
          g_Recording = false;
        }
      }
    }
  } else {
    LogInfo("[Controller] Stopping recording...");

    // Notify inject process - it clears shared memory recording flag
    if (g_InjectClient && g_InjectClient->IsConnected()) {
      ProcessResponse resp;
      if (!g_InjectClient->SendCommand(ProcessCommand::StopRecording, nullptr,
                                       &resp, 5000)) {
        LogError("[Controller] Stop failed - retrying once");
        g_InjectClient->SendCommand(ProcessCommand::StopRecording, nullptr,
                                    &resp, 5000);
      }
    }

    LogInfo("[Controller] Recording stopped");
  }

  if (g_Tray)
    g_Tray->SetRecordingState(g_Recording);
}

// Shutdown all child processes gracefully
void ShutdownChildProcesses() {
  LogInfo("[Controller] Shutting down child processes...");

  // Send shutdown commands
  SendCommandToAll(ProcessCommand::Shutdown);

  // Wait for processes to exit
  HANDLE handles[5]; // Increased size for Logger and Sensor
  int handleCount = 0;

  if (g_hLimiterProcess)
    handles[handleCount++] = g_hLimiterProcess;
  if (g_hMediaProcess)
    handles[handleCount++] = g_hMediaProcess;
  if (g_hInjectProcess)
    handles[handleCount++] = g_hInjectProcess;
  if (g_hLoggerProcess)
    handles[handleCount++] = g_hLoggerProcess;
  if (g_hSensorProcess)
    handles[handleCount++] = g_hSensorProcess;

  if (handleCount > 0) {
    // Use MsgWaitForMultipleObjects to keep processing messages (for tray animation)
    DWORD startTime = GetTickCount();
    DWORD timeout = 5000;
    bool allExited = false;
    
    while (!allExited && (GetTickCount() - startTime) < timeout) {
      DWORD remaining = timeout - (GetTickCount() - startTime);
      DWORD waitTime = (remaining < 100) ? remaining : 100;
      
      // bWaitAll MUST be FALSE to process messages while waiting
      DWORD result = MsgWaitForMultipleObjects(handleCount, handles, FALSE, 
                                                waitTime, QS_ALLINPUT);
      
      if (result == WAIT_OBJECT_0 + handleCount) {
        // Messages available - process them to keep tray animation running
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
          TranslateMessage(&msg);
          DispatchMessage(&msg);
        }
      } else if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + handleCount) {
        // At least one process exited, re-evaluate all processes
        bool foundActive = false;
        for (int i = 0; i < handleCount; i++) {
          DWORD exitCode;
          if (GetExitCodeProcess(handles[i], &exitCode) && exitCode == STILL_ACTIVE) {
            foundActive = true;
            break;
          }
        }
        if (!foundActive) allExited = true;
      } else {
        // Timeout or other error
      }
    }
    
    if (!allExited) {
      LogInfo(
          "[Controller] Some processes didn't exit cleanly, terminating...");
      if (g_hLimiterProcess)
        TerminateProcess(g_hLimiterProcess, 1);
      if (g_hMediaProcess)
        TerminateProcess(g_hMediaProcess, 1);
      if (g_hInjectProcess)
        TerminateProcess(g_hInjectProcess, 1);
      if (g_hLoggerProcess)
        TerminateProcess(g_hLoggerProcess, 1);
      if (g_hSensorProcess)
        TerminateProcess(g_hSensorProcess, 1);
    }
  }

  // Cleanup handles
  if (g_hLimiterProcess)
    CloseHandle(g_hLimiterProcess);
  if (g_hMediaProcess)
    CloseHandle(g_hMediaProcess);
  if (g_hInjectProcess)
    CloseHandle(g_hInjectProcess);
  if (g_hLoggerProcess)
    CloseHandle(g_hLoggerProcess);
  if (g_hSensorProcess)
    CloseHandle(g_hSensorProcess);

  g_hLimiterProcess = NULL;
  g_hMediaProcess = NULL;
  g_hInjectProcess = NULL;
  g_hLoggerProcess = NULL;
  g_hSensorProcess = NULL;
}

// Monitor child processes for crashes
void CheckChildProcessHealth() {
  static DWORD lastCheck = 0;
  if (GetTickCount() - lastCheck < 1000)
    return; // Check once per second
  lastCheck = GetTickCount();

  auto checkProcess = [](HANDLE h, const char *name) {
    if (!h)
      return;
    DWORD exitCode;
    if (GetExitCodeProcess(h, &exitCode) && exitCode != STILL_ACTIVE) {
      LogError("[Controller] %s process exited with code %d", name, exitCode);
    }
  };

  checkProcess(g_hInjectProcess, "Inject");
  checkProcess(g_hMediaProcess, "Media");
  checkProcess(g_hLimiterProcess, "Limiter");
}



// Registry Helpers for Ephemeral Vulkan Layer Registration
static void Registry_ManageImplicitLayer(bool install) {
    HKEY hKey;
    // HKCU is safer and sufficient for local user
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Khronos\\Vulkan\\ImplicitLayers", 
                        0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        
        char buffer[MAX_PATH];
        GetModuleFileNameA(NULL, buffer, MAX_PATH);
        std::string exePath = buffer;
        std::string baseDir = exePath.substr(0, exePath.find_last_of("\\/"));
        
        const char* jsons[] = { "VK_LAYER_CE_overlay.json", "VK_LAYER_CE_overlay_x86.json" };
        
        for (const char* json : jsons) {
            std::string fullPath = baseDir + "\\" + json;
            if (install) {
                DWORD data = 0;
                RegSetValueExA(hKey, fullPath.c_str(), 0, REG_DWORD, (const BYTE*)&data, sizeof(data));
                LogInfo("[Controller] Registered Vulkan Layer: %s", json);
            } else {
                RegDeleteValueA(hKey, fullPath.c_str());
                LogInfo("[Controller] Unregistered Vulkan Layer: %s", json);
            }
        }
        RegCloseKey(hKey);
    } else {
        LogError("[Controller] Failed to access Vulkan ImplicitLayers registry key.");
    }
}

// RAII Wrapper for guaranteed cleanup
class ScopedVulkanRegistration {
public:
    ScopedVulkanRegistration() { Registry_ManageImplicitLayer(true); }
    ~ScopedVulkanRegistration() { Registry_ManageImplicitLayer(false); }
};

// Global pointer for emergency cleanup
static ScopedVulkanRegistration* g_VulkanReg = nullptr;

BOOL WINAPI ControllerConsoleHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
         LogInfo("[Controller] Console Interrupted. Cleaning up...");
         if (g_VulkanReg) {
             Registry_ManageImplicitLayer(false); // Force cleanup
         }
         g_Running = false;
         return TRUE;
    }
    return FALSE;
}

// Controller main function
int ControllerMain(HINSTANCE hInstance) {
  LogInfo("[Controller] Starting...");
  
  SetConsoleCtrlHandler(ControllerConsoleHandler, TRUE);
  
  // Ephemeral Registration (RAII)
  ScopedVulkanRegistration vulkanReg;
  g_VulkanReg = &vulkanReg;

  // Create IPC clients
  g_InjectClient = std::make_unique<ProcessIPCClient>(ProcessMode::Inject);
  g_MediaClient = std::make_unique<ProcessIPCClient>(ProcessMode::Media);
  g_LimiterClient = std::make_unique<ProcessIPCClient>(ProcessMode::Limiter);

  // Spawn child processes
  // Order matters: Inject first (creates shared memory), then Media, then
  // Limiter
  LogInfo("[Controller] Spawning child processes...");

  g_hInjectProcess =
      SpawnChildProcess(ProcessMode::Inject, g_ConfigPath.c_str());
  if (!g_hInjectProcess) {
    LogError("[Controller] Failed to spawn inject process");
    return 1;
  }

  // Wait a bit for inject to create shared memory and event
  Sleep(500);

  g_hMediaProcess = SpawnChildProcess(ProcessMode::Media, g_ConfigPath.c_str());
  if (!g_hMediaProcess) {
    LogError("[Controller] Failed to spawn media process");
    ShutdownChildProcesses();
    return 1;
  }

  g_hLimiterProcess =
      SpawnChildProcess(ProcessMode::Limiter, g_ConfigPath.c_str());
  if (!g_hLimiterProcess) {
    LogError("[Controller] Failed to spawn limiter process");
    ShutdownChildProcesses();
    return 1;
  }
 
  g_hLoggerProcess = SpawnChildProcess(ProcessMode::Logger, g_ConfigPath.c_str());
  g_hSensorProcess = SpawnChildProcess(ProcessMode::Sensors, g_ConfigPath.c_str());

  // Wait for IPC connections
  LogInfo("[Controller] Waiting for child processes to connect...");
  if (!ConnectToChildProcesses(10000)) {
    LogError("[Controller] Failed to connect to all child processes");
    ShutdownChildProcesses();
    return 1;
  }
  LogInfo("[Controller] All child processes connected");
  
  // NOW launch the game if --launch was used (IPC is ready)
  if (!g_DeferredLaunchPath.empty()) {
      LogInfo("[Controller] Launching deferred game: %s", g_DeferredLaunchPath.c_str());
      LaunchGameSuspended(g_DeferredLaunchPath);
  }

  // Create tray icon
  LogInfo("[Controller] Creating tray icon...");
  auto tray = std::make_unique<TrayIcon>(
      hInstance, []() { g_Running = false; },
      []() {
        ShellExecuteA(NULL, "open", g_ConfigPath.c_str(), NULL, NULL, SW_SHOW);
      });
  g_Tray = tray.get();

  // Register hotkeys
  LogInfo("[Controller] Registering hotkeys...");
  RegisterHotKey(NULL, HOTKEY_ID_RECORD, MOD_NOREPEAT,
                 g_Config.hotkeyStartStop);

  LogInfo("[Controller] Ready. Press hotkey to start recording.");

  // Auto-record: start timer after processes are ready
  if (g_AutoRecordEnabled) {
    LogInfo("[Controller] Auto-record enabled: delay=%dms, duration=%dms",
            g_AutoRecordDelayMs, g_AutoRecordDurationMs);
    g_AutoRecordStartTime = GetTickCount();
  }

  // Main message loop
  MSG msg;
  while (g_Running) {
    // Process messages
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        g_Running = false;
      } else if (msg.message == WM_HOTKEY) {
        if (msg.wParam == HOTKEY_ID_RECORD) {
          ToggleRecording();
        }
      }
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }

    // Check child process health
    CheckChildProcessHealth();

    // Config hot-reload (when not recording)
    if (!g_Recording) {
      static DWORD lastConfigCheck = 0;
      if (GetTickCount() - lastConfigCheck > 2000) {
        WIN32_FILE_ATTRIBUTE_DATA fileInfo;
        if (GetFileAttributesExA(g_ConfigPath.c_str(), GetFileExInfoStandard,
                                 &fileInfo)) {
          static FILETIME lastWriteTime = fileInfo.ftLastWriteTime;
          if (CompareFileTime(&fileInfo.ftLastWriteTime, &lastWriteTime) > 0) {
            LogInfo("[Controller] Config change detected, reloading...");
            lastWriteTime = fileInfo.ftLastWriteTime;
            LoadConfig(g_ConfigPath, g_Config);
            SendCommandToAll(ProcessCommand::ReloadConfig);
          }
        }
        lastConfigCheck = GetTickCount();
      }
    }

    // Auto-record logic
    if (g_AutoRecordEnabled && g_AutoRecordStartTime > 0) {
      DWORD elapsed = GetTickCount() - g_AutoRecordStartTime;
      if (!g_Recording && elapsed >= g_AutoRecordDelayMs) {
        LogInfo("[Controller] Auto-record: starting recording...");
        ToggleRecording();
      } else if (g_Recording &&
                 elapsed >= (g_AutoRecordDelayMs + g_AutoRecordDurationMs)) {
        LogInfo("[Controller] Auto-record: stopping recording...");
        ToggleRecording();
        g_Running = false; // Exit after auto-record completes
      }
    }

    Sleep(10);
  }

  // Unregister hotkeys first
  UnregisterHotKey(NULL, HOTKEY_ID_RECORD);
  
  // Keep tray icon alive during shutdown (animation already started by right-click handler)
  // Process messages during shutdown so animation continues
  if (g_Tray) {
      g_Tray->StartShutdownAnimation();
  }

  LogInfo("[Controller] Shutting down child processes...");
  ShutdownChildProcesses();
  
  // Reset global pointer (destructor of ScopedVulkanRegistration will handle cleanup)
  g_VulkanReg = nullptr;
  
  // Now remove tray icon after shutdown is complete
  if (tray) {
    tray->Remove();
  }
  g_Tray = nullptr;
  tray.reset();

  LogInfo("[Controller] Exiting");
  return 0;
}

// Main entry point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow) {
  // Parse process mode from command line
  ProcessMode mode = ParseProcessMode(lpCmdLine);

  // Get paths
  char buffer[MAX_PATH];
  GetModuleFileNameA(NULL, buffer, MAX_PATH);
  std::string exePath = buffer;
  std::string baseDir = exePath.substr(0, exePath.find_last_of("\\/"));
  g_ConfigPath = baseDir + "\\config.ini";

  PurgeAllLogsInDir(baseDir + "\\logs");
  
  // NOTE: FFmpeg DLL path is set by static initializer (FFmpegDllPathInitializer)
  // before WinMain runs, so we don't need to set it here

  // Load config
  LoadConfig(g_ConfigPath, g_Config);

  // Parse --auto-record flag: --auto-record=DELAY_MS,DURATION_MS
  // Parse --license flag
  std::string cmdLine(lpCmdLine);
  if (cmdLine.find("--license") != std::string::npos) {
    std::string licensePath = baseDir + "\\LICENSE.txt";
    ShellExecuteA(NULL, "open", licensePath.c_str(), NULL, NULL, SW_SHOWNORMAL);
    return 0;
  }
  size_t autoRecordPos = cmdLine.find("--auto-record=");
  if (autoRecordPos != std::string::npos) {
    std::string params = cmdLine.substr(autoRecordPos + 14);
    size_t commaPos = params.find(',');
    if (commaPos != std::string::npos) {
      g_AutoRecordDelayMs = (DWORD)atoi(params.substr(0, commaPos).c_str());
      g_AutoRecordDurationMs = (DWORD)atoi(params.substr(commaPos + 1).c_str());
      g_AutoRecordEnabled = true;
    }
  }
  
  // Parse --launch flag: --launch <command> or --launch=<command>
  // Supports Steam Launch Options: "CaptureEngine.exe" --launch %command%
  std::string searchFlag = "--launch";
  size_t launchPos = cmdLine.find(searchFlag);
  if (launchPos != std::string::npos) {
      size_t valueStart = launchPos + searchFlag.length();
      
      // Skip delimiter (+, =, or space)
      while (valueStart < cmdLine.length() && (cmdLine[valueStart] == '=' || cmdLine[valueStart] == ' ')) {
          valueStart++;
      }
      
      if (valueStart < cmdLine.length()) {
          g_DeferredLaunchPath = cmdLine.substr(valueStart);
          
          // Game launch will happen in ControllerMain AFTER child processes are ready
          Log_Init(baseDir + "\\logs\\launcher.log");
          LogInfo("[Launcher] Deferred launch path: %s", g_DeferredLaunchPath.c_str());
          
          // Continue as Controller
          mode = ProcessMode::Controller;
      }
  }

  // Setup logging with process-specific log file in logs/ subfolder
  std::string logsDir = baseDir + "\\logs";
  CreateDirectoryA(logsDir.c_str(), NULL); // Create logs folder if it doesn't exist
  std::string logPath = logsDir + "\\" + GetLogFileName(mode);
  g_Config.logFilePath = logPath;

  if (g_Config.debugLogging) {
    Log_Init(logPath);
    LogInfo("CaptureEngine Starting... Version: %s (Built: %s)",
            CAPTURE_VERSION, BUILD_TIMESTAMP);
    LogInfo("Process Mode: %s", mode == ProcessMode::Controller ? "Controller"
                                : mode == ProcessMode::Inject   ? "Inject"
                                : mode == ProcessMode::Media    ? "Media"
                                : mode == ProcessMode::Limiter  ? "Limiter"
                                : mode == ProcessMode::Logger   ? "Logger"
                                : mode == ProcessMode::Sensors  ? "Sensors"
                                                                : "Unknown");
  }

  // Enable 1ms timer resolution
  timeBeginPeriod(1);

  // Controller: Single instance check
  if (mode == ProcessMode::Controller) {
    CreateMutexA(0, FALSE, "Local\\CaptureEngine_Instance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      MessageBoxA(NULL, "CaptureEngine is already running.", "Error",
                  MB_ICONERROR);
      return 1;
    }
  }

  // Install crash handler
  std::string crashDir = g_Config.crashDumpDir;
  if (crashDir.find(":") == std::string::npos) {
    crashDir = baseDir + "\\" + crashDir;
  }
  SetCrashDumpDirectory(crashDir);
  InstallCrashHandler();

  // Dispatch to appropriate process
  int result = 0;
  switch (mode) {
  case ProcessMode::Controller:
    result = ControllerMain(hInstance);
    break;
  case ProcessMode::Inject:
    result = InjectProcessMain(g_Config);
    break;
  case ProcessMode::Media:
    result = MediaProcessMain(g_Config);
    break;
  case ProcessMode::Limiter:
    result = LimiterProcessMain(g_Config);
    break;
  case ProcessMode::Logger:
    result = LoggerProcessMain(g_Config);
    break;
  case ProcessMode::Sensors:
    result = SensorProcessMain(g_Config);
    break;
  }

  // Cleanup
  timeEndPeriod(1);
  Log_Shutdown();

  return result;
}
