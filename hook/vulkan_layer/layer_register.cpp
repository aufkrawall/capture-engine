/**
 * Vulkan Layer Registration Utility
 *
 * Registers/unregisters the capture overlay Vulkan layer manifests in the
 * Windows registry. Uses the same hardened implementation as captureengine.exe.
 */

#include <windows.h>

#include <filesystem>
#include <iostream>
#include <string>

#include "../../common/vulkan_layer_registration.h"

namespace {

void PrintUsage(const wchar_t* programName) {
    std::wcout << L"Usage: " << programName << L" [options]" << std::endl;
    std::wcout << L"Options:" << std::endl;
    std::wcout << L"  --register     Register the Vulkan layer manifests" << std::endl;
    std::wcout << L"  --unregister   Unregister the Vulkan layer manifests" << std::endl;
    std::wcout << L"  --status       Check if the Vulkan layer is registered" << std::endl;
    std::wcout << L"  --all-users    Use HKLM (requires elevation)" << std::endl;
    std::wcout << L"  --current-user Force HKCU registration" << std::endl;
    std::wcout << L"  --help         Show this help" << std::endl;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    bool doRegister = false;
    bool doUnregister = false;
    bool doStatus = false;
    ce::vulkan_layer::RegistrationMode requestedMode = ce::vulkan_layer::RegistrationMode::Auto;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--register") {
            doRegister = true;
        } else if (arg == L"--unregister") {
            doUnregister = true;
        } else if (arg == L"--status") {
            doStatus = true;
        } else if (arg == L"--all-users") {
            requestedMode = ce::vulkan_layer::RegistrationMode::AllUsers;
        } else if (arg == L"--current-user") {
            requestedMode = ce::vulkan_layer::RegistrationMode::CurrentUser;
        } else if (arg == L"--help") {
            PrintUsage(argv[0]);
            return 0;
        }
    }

    if (!doRegister && !doUnregister && !doStatus) {
        PrintUsage(argv[0]);
        return 1;
    }

    std::filesystem::path baseDir;
    if (!ce::vulkan_layer::GetCurrentExecutableDirectory(&baseDir)) {
        std::wcerr << L"Failed to resolve executable directory" << std::endl;
        return 1;
    }

    const bool elevated = ce::vulkan_layer::IsCurrentProcessElevated();
    const ce::vulkan_layer::RegistrationPlan plan =
        ce::vulkan_layer::BuildRegistrationPlan(baseDir, requestedMode, elevated);

    if (doStatus) {
        const bool registered = ce::vulkan_layer::IsRegistrationActive(plan);
        std::wcout << L"Layer status: " << (registered ? L"REGISTERED" : L"NOT REGISTERED") << std::endl;
        return registered ? 0 : 1;
    }

    if (plan.effectiveMode == ce::vulkan_layer::RegistrationMode::AllUsers && !elevated) {
        std::wcerr << L"All-users registration requires elevation." << std::endl;
        return 1;
    }

    const bool success = ce::vulkan_layer::ApplyRegistrationPlan(plan, doRegister && !doUnregister);
    return success ? 0 : 1;
}
