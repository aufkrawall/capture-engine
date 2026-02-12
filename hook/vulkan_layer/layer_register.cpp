/**
 * Vulkan Layer Registration Utility
 *
 * Registers/unregisters the capture overlay Vulkan layer in the Windows
 * registry. This allows the layer to be loaded automatically by the Vulkan
 * loader.
 */

#include <filesystem>
#include <iostream>
#include <string>
#include <windows.h>

// Registry paths for implicit layers
const wchar_t *VULKAN_IMPLICIT_LAYERS_32 =
    L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers";
const wchar_t *VULKAN_IMPLICIT_LAYERS_64 =
    L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers";

// For 32-bit on 64-bit Windows
const wchar_t *VULKAN_IMPLICIT_LAYERS_WOW64 =
    L"SOFTWARE\\WOW6432Node\\Khronos\\Vulkan\\ImplicitLayers";

/**
 * Register the Vulkan layer manifest in the registry
 */
bool RegisterVulkanLayer(const std::wstring &manifestPath,
                         bool forAllUsers = false) {
  HKEY hRootKey = forAllUsers ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
  HKEY hKey = nullptr;

  // Create/open the registry key
  LONG result = RegCreateKeyExW(
      hRootKey, VULKAN_IMPLICIT_LAYERS_64, 0, nullptr, REG_OPTION_NON_VOLATILE,
      KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &hKey, nullptr);

  if (result != ERROR_SUCCESS) {
    std::wcerr << L"Failed to create registry key: " << result << std::endl;
    return false;
  }

  // Set the value (0 = enabled, 1 = disabled)
  DWORD enabled = 0;
  result =
      RegSetValueExW(hKey, manifestPath.c_str(), 0, REG_DWORD,
                     reinterpret_cast<const BYTE *>(&enabled), sizeof(enabled));

  RegCloseKey(hKey);

  if (result != ERROR_SUCCESS) {
    std::wcerr << L"Failed to set registry value: " << result << std::endl;
    return false;
  }

  std::wcout << L"Registered layer: " << manifestPath << std::endl;
  return true;
}

/**
 * Unregister the Vulkan layer from the registry
 */
bool UnregisterVulkanLayer(const std::wstring &manifestPath,
                           bool forAllUsers = false) {
  HKEY hRootKey = forAllUsers ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
  HKEY hKey = nullptr;

  LONG result = RegOpenKeyExW(hRootKey, VULKAN_IMPLICIT_LAYERS_64, 0,
                              KEY_SET_VALUE | KEY_WOW64_64KEY, &hKey);

  if (result != ERROR_SUCCESS) {
    // Key doesn't exist, nothing to unregister
    return true;
  }

  result = RegDeleteValueW(hKey, manifestPath.c_str());
  RegCloseKey(hKey);

  if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) {
    std::wcerr << L"Failed to delete registry value: " << result << std::endl;
    return false;
  }

  std::wcout << L"Unregistered layer: " << manifestPath << std::endl;
  return true;
}

/**
 * Check if the layer is registered
 */
bool IsLayerRegistered(const std::wstring &manifestPath,
                       bool forAllUsers = false) {
  HKEY hRootKey = forAllUsers ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
  HKEY hKey = nullptr;

  LONG result = RegOpenKeyExW(hRootKey, VULKAN_IMPLICIT_LAYERS_64, 0,
                              KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hKey);

  if (result != ERROR_SUCCESS) {
    return false;
  }

  DWORD value = 0;
  DWORD size = sizeof(value);
  result = RegQueryValueExW(hKey, manifestPath.c_str(), nullptr, nullptr,
                            reinterpret_cast<BYTE *>(&value), &size);
  RegCloseKey(hKey);

  return result == ERROR_SUCCESS;
}

/**
 * Get the absolute path to the layer manifest
 */
std::wstring GetLayerManifestPath() {
  wchar_t modulePath[MAX_PATH];
  GetModuleFileNameW(nullptr, modulePath, MAX_PATH);

  std::filesystem::path exePath(modulePath);
  std::filesystem::path manifestPath =
      exePath.parent_path() / L"VK_LAYER_CAPTURE_overlay.json";

  return manifestPath.wstring();
}

void PrintUsage(const wchar_t *programName) {
  std::wcout << L"Usage: " << programName << L" [options]" << std::endl;
  std::wcout << L"Options:" << std::endl;
  std::wcout << L"  --register     Register the Vulkan layer" << std::endl;
  std::wcout << L"  --unregister   Unregister the Vulkan layer" << std::endl;
  std::wcout << L"  --status       Check if the layer is registered"
             << std::endl;
  std::wcout << L"  --all-users    Apply to all users (requires admin)"
             << std::endl;
  std::wcout << L"  --help         Show this help" << std::endl;
}

int wmain(int argc, wchar_t *argv[]) {
  bool doRegister = false;
  bool doUnregister = false;
  bool doStatus = false;
  bool forAllUsers = false;

  for (int i = 1; i < argc; i++) {
    std::wstring arg = argv[i];
    if (arg == L"--register")
      doRegister = true;
    else if (arg == L"--unregister")
      doUnregister = true;
    else if (arg == L"--status")
      doStatus = true;
    else if (arg == L"--all-users")
      forAllUsers = true;
    else if (arg == L"--help") {
      PrintUsage(argv[0]);
      return 0;
    }
  }

  if (!doRegister && !doUnregister && !doStatus) {
    PrintUsage(argv[0]);
    return 1;
  }

  std::wstring manifestPath = GetLayerManifestPath();

  if (!std::filesystem::exists(manifestPath)) {
    std::wcerr << L"Layer manifest not found: " << manifestPath << std::endl;
    return 1;
  }

  if (doStatus) {
    bool registered = IsLayerRegistered(manifestPath, forAllUsers);
    std::wcout << L"Layer status: "
               << (registered ? L"REGISTERED" : L"NOT REGISTERED") << std::endl;
    return registered ? 0 : 1;
  }

  if (doRegister) {
    if (!RegisterVulkanLayer(manifestPath, forAllUsers)) {
      return 1;
    }
  }

  if (doUnregister) {
    if (!UnregisterVulkanLayer(manifestPath, forAllUsers)) {
      return 1;
    }
  }

  return 0;
}
