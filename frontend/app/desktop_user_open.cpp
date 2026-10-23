// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "frontend/app/desktop_user_open.h"

// shellapi.h first: its ShellExecute macro then renames IShellDispatch2::ShellExecute consistently.
#include <shellapi.h>
#include <exdisp.h>
#include <oleauto.h>
#include <shldisp.h>
#include <shlguid.h>
#include <shlobj.h>

#include <cstdio>

namespace ce::frontend {
namespace {

// Owns one COM interface pointer for the duration of a call; nothing here is cached across calls.
template <typename T>
class ComRef {
public:
    ComRef() = default;
    ~ComRef() {
        if (pointer_)
            pointer_->Release();
    }
    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;
    T* operator->() const {
        return pointer_;
    }
    void** Put() {
        return reinterpret_cast<void**>(&pointer_);
    }
    T** PutTyped() {
        return &pointer_;
    }

private:
    T* pointer_ = nullptr;
};

std::string HexResult(HRESULT result) {
    char text[16];
    snprintf(text, sizeof(text), "0x%08lx", static_cast<unsigned long>(result));
    return text;
}

// The desktop shell's Shell.Application, reached through the desktop window's folder view: a ShellExecute through it
// runs inside Explorer, with Explorer's token. This is the documented automation route for starting an unelevated
// program from an elevated one (Raymond Chen, "How can I launch an unelevated process from my elevated process,
// redux").
HRESULT DesktopShellDispatch(IShellDispatch2** out) {
    ComRef<IShellWindows> windows;
    HRESULT result = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_IShellWindows, windows.Put());
    if (FAILED(result))
        return result;
    VARIANT location;
    VARIANT root;
    VariantInit(&location);
    VariantInit(&root);
    long desktopWindow = 0;
    ComRef<IDispatch> desktop;
    result = windows->FindWindowSW(&location, &root, SWC_DESKTOP, &desktopWindow, SWFO_NEEDDISPATCH,
                                   desktop.PutTyped());
    if (result != S_OK)  // S_FALSE: there is no desktop window
        return FAILED(result) ? result : E_FAIL;
    ComRef<IServiceProvider> provider;
    result = desktop->QueryInterface(IID_IServiceProvider, provider.Put());
    if (FAILED(result))
        return result;
    ComRef<IShellBrowser> browser;
    result = provider->QueryService(SID_STopLevelBrowser, IID_IShellBrowser, browser.Put());
    if (FAILED(result))
        return result;
    ComRef<IShellView> view;
    result = browser->QueryActiveShellView(view.PutTyped());
    if (FAILED(result))
        return result;
    ComRef<IDispatch> background;
    result = view->GetItemObject(SVGIO_BACKGROUND, IID_IDispatch, background.Put());
    if (FAILED(result))
        return result;
    ComRef<IShellFolderViewDual> folderView;
    result = background->QueryInterface(IID_IShellFolderViewDual, folderView.Put());
    if (FAILED(result))
        return result;
    ComRef<IDispatch> application;
    result = folderView->get_Application(application.PutTyped());
    if (FAILED(result))
        return result;
    return application->QueryInterface(IID_IShellDispatch2, reinterpret_cast<void**>(out));
}

HRESULT OpenThroughDesktopShell(const std::wstring& path) {
    ComRef<IShellDispatch2> shell;
    HRESULT result = DesktopShellDispatch(shell.PutTyped());
    if (FAILED(result))
        return result;
    // Explorer starts the editor: hand it the foreground right this tray click gave the app.
    DWORD shellProcess = 0;
    if (HWND shellWindow = GetShellWindow())
        GetWindowThreadProcessId(shellWindow, &shellProcess);
    if (shellProcess)
        AllowSetForegroundWindow(shellProcess);
    BSTR file = SysAllocStringLen(path.data(), static_cast<UINT>(path.size()));
    VARIANT arguments;
    VARIANT directory;
    VARIANT operation;
    VARIANT show;
    VariantInit(&arguments);
    VariantInit(&directory);
    VariantInit(&operation);
    VariantInit(&show);
    operation.vt = VT_BSTR;
    operation.bstrVal = SysAllocString(L"open");
    show.vt = VT_I4;
    show.lVal = SW_SHOWNORMAL;
    result = file && operation.bstrVal ? shell->ShellExecute(file, arguments, directory, operation, show) : E_OUTOFMEMORY;
    VariantClear(&operation);
    SysFreeString(file);
    return result;
}

OpenResult OpenDirectly(const std::wstring& path, const char* route) {
    const INT_PTR code =
        reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (code > 32)
        return {true, route};
    return {false, std::string(route) + " failed (ShellExecute " + std::to_string(static_cast<long long>(code)) + ")"};
}

}  // namespace

bool IsProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation = {};
    DWORD returned = 0;
    const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned) != FALSE &&
                          elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

OpenResult OpenForDesktopUser(const std::wstring& path) {
    if (!IsProcessElevated())
        return OpenDirectly(path, "opened directly (unelevated app)");
    // RPC_E_CHANGED_MODE: this thread is already in the multithreaded apartment, which serves the call as well.
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const HRESULT shell = SUCCEEDED(init) || init == RPC_E_CHANGED_MODE ? OpenThroughDesktopShell(path) : init;
    if (SUCCEEDED(init))
        CoUninitialize();
    if (SUCCEEDED(shell))
        return {true, "handed to the desktop shell (the editor runs unelevated)"};
    // No desktop shell to hand it to (Explorer not running, or replaced): the click still opens the file, elevated.
    OpenResult direct = OpenDirectly(path, "opened directly, elevated");
    direct.description += "; the desktop shell refused it (" + HexResult(shell) + ")";
    return direct;
}

}  // namespace ce::frontend
