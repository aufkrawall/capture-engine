#pragma once

#include <windows.h>

namespace ce::ffx_export_lifetime {

// Acquire/release only outside CE's breakpoint mutex: either operation may
// enter the loader. The reference keeps a checked export mapped until every
// read/write of its entry byte has finished.
class ModulePin {
public:
    ModulePin(HMODULE expectedModule, void* target, const char* exportName) {
        HMODULE owner = nullptr;
        if (!expectedModule || !target || !exportName ||
            !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCSTR>(target), &owner)) {
            return;
        }
        if (owner != expectedModule || reinterpret_cast<void*>(GetProcAddress(owner, exportName)) != target) {
            FreeLibrary(owner);
            return;
        }
        module_ = owner;
    }

    ~ModulePin() {
        if (module_)
            FreeLibrary(module_);
    }

    ModulePin(const ModulePin&) = delete;
    ModulePin& operator=(const ModulePin&) = delete;

    HMODULE Get() const { return module_; }

private:
    HMODULE module_ = nullptr;
};

}  // namespace ce::ffx_export_lifetime
