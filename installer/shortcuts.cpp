// Shell links owned by the current or previous installation.
// Both folders count when an update relocates the program and disables a link.

#include "setup.h"

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace ce::setup {
namespace {

class ComScope {
public:
    ComScope() : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
    ~ComScope() {
        if (SUCCEEDED(result_))
            CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    // RPC_E_CHANGED_MODE means COM is already usable on this thread.
    bool Usable() const { return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE; }

private:
    HRESULT result_;
};

template <typename T>
class ComPtr {
public:
    ~ComPtr() {
        if (value_)
            value_->Release();
    }
    T** Put() { return &value_; }
    T* operator->() const { return value_; }
    T* Get() const { return value_; }

private:
    T* value_ = nullptr;
};

DWORD ToWin32(HRESULT result) {
    return HRESULT_FACILITY(result) == FACILITY_WIN32 ? HRESULT_CODE(result) : static_cast<DWORD>(result);
}

std::wstring ShortcutTarget(const std::wstring& linkPath) {
    ComScope com;
    if (!com.Usable())
        return {};
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                reinterpret_cast<void**>(link.Put()))))
        return {};
    ComPtr<IPersistFile> file;
    if (FAILED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(file.Put()))) ||
        FAILED(file->Load(linkPath.c_str(), STGM_READ)))
        return {};
    wchar_t target[MAX_PATH * 2] = {};
    if (FAILED(link->GetPath(target, static_cast<int>(sizeof(target) / sizeof(target[0])), nullptr, SLGP_RAWPATH)))
        return {};
    return target;
}

}  // namespace

std::wstring StartMenuShortcutPath() {
    const std::wstring folder = KnownFolder(FOLDERID_CommonPrograms);
    return folder.empty() ? std::wstring() : JoinPath(folder, kShortcutName);
}

std::wstring DesktopShortcutPath() {
    const std::wstring folder = KnownFolder(FOLDERID_PublicDesktop);
    return folder.empty() ? std::wstring() : JoinPath(folder, kShortcutName);
}

bool CreateAppShortcut(const std::wstring& linkPath, const std::wstring& directory, DWORD* error) {
    if (linkPath.empty()) {
        if (error)
            *error = ERROR_PATH_NOT_FOUND;
        return false;
    }
    ComScope com;
    if (!com.Usable()) {
        if (error)
            *error = ERROR_NOT_READY;
        return false;
    }
    ComPtr<IShellLinkW> link;
    HRESULT result = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                      reinterpret_cast<void**>(link.Put()));
    const std::wstring exe = JoinPath(directory, kAppExe);
    if (SUCCEEDED(result))
        result = link->SetPath(exe.c_str());
    if (SUCCEEDED(result))
        result = link->SetWorkingDirectory(directory.c_str());
    if (SUCCEEDED(result))
        result = link->SetDescription(L"Capture Engine - game recording");
    if (SUCCEEDED(result))
        result = link->SetIconLocation(exe.c_str(), 0);
    ComPtr<IPersistFile> file;
    if (SUCCEEDED(result))
        result = link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(file.Put()));
    if (SUCCEEDED(result))
        result = file->Save(linkPath.c_str(), TRUE);
    if (FAILED(result)) {
        if (error)
            *error = ToWin32(result);
        return false;
    }
    return true;
}

bool OwnedShortcutExists(const std::wstring& linkPath, const std::wstring& directory) {
    if (linkPath.empty() || !PathExists(linkPath))
        return false;
    return ShortcutTargetsInstallation(ShortcutTarget(linkPath), directory, kAppExe);
}

bool RemoveOwnedShortcut(const std::wstring& linkPath, const std::wstring& directory,
                         const std::wstring& previousDirectory) {
    if (linkPath.empty() || !PathExists(linkPath))
        return true;
    const std::wstring target = ShortcutTarget(linkPath);
    if (!ShortcutTargetsInstallation(target, directory, kAppExe) &&
        (previousDirectory.empty() || !ShortcutTargetsInstallation(target, previousDirectory, kAppExe))) {
        Log("shortcut: %s belongs to something else; left in place", Narrow(linkPath).c_str());
        return true;
    }
    Log("shortcut: removing %s owned by the current or previous installation", Narrow(linkPath).c_str());
    return DeleteFileW(linkPath.c_str()) != FALSE;
}

}  // namespace ce::setup
