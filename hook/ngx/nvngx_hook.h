#pragma once
#include <windows.h>
#include <atomic>
#include <string>

class NVNGXHook {
public:
    static NVNGXHook& Get() {
        static NVNGXHook instance;
        return instance;
    }

    void Install();
    void Uninstall();

    // Called synchronously from the loader hook while a module is being loaded.
    // When that module is the NGX provider its exports are hooked before the
    // caller can resolve and invoke them.
    void OnModuleLoaded(HMODULE module, const char* moduleNameOrPath);

    bool IsInstalled() const {
        return m_Installed;
    }

private:
    NVNGXHook() = default;
    ~NVNGXHook() = default;

    std::atomic<bool> m_Installed{false};
};
