#include "tests/flow/flow_test_support.h"

#include <cstring>
#include <filesystem>
#include <string>

namespace {

// A foreign tool's hook on GetModuleHandleA that answers a query for one module name with its own image, as OptiScaler's
// WINMM.dll answered nvngx_dlssg.dll in W3 (session 20261106_195839). Patched in place, since every caller must see it;
// other queries go on through GetModuleHandleW, which stays untouched.
const char* g_answeredName = nullptr;
HMODULE g_answer = nullptr;

HMODULE WINAPI AnswerModuleQuery(LPCSTR name) {
    if (name && g_answeredName && _stricmp(name, g_answeredName) == 0)
        return g_answer;
    if (!name)
        return GetModuleHandleW(nullptr);
    wchar_t wide[MAX_PATH] = {};
    if (MultiByteToWideChar(CP_ACP, 0, name, -1, wide, MAX_PATH) == 0)
        return nullptr;
    return GetModuleHandleW(wide);
}

class ModuleQueryAnswer {
public:
    ModuleQueryAnswer(const char* name, HMODULE answer) {
        g_answeredName = name;
        g_answer = answer;
        target_ = reinterpret_cast<uint8_t*>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "GetModuleHandleA"));
        if (!target_)
            return;
        std::memcpy(saved_, target_, sizeof(saved_));
        uint8_t jump[sizeof(saved_)] = {0xFF, 0x25, 0, 0, 0, 0};  // jmp [rip+0], then the address
        const auto destination = reinterpret_cast<uintptr_t>(&AnswerModuleQuery);
        std::memcpy(jump + 6, &destination, sizeof(destination));
        Write(jump);
    }
    ~ModuleQueryAnswer() {
        if (target_)
            Write(saved_);
        g_answeredName = nullptr;
        g_answer = nullptr;
    }
    ModuleQueryAnswer(const ModuleQueryAnswer&) = delete;
    ModuleQueryAnswer& operator=(const ModuleQueryAnswer&) = delete;
    bool Installed() const { return target_ != nullptr; }

private:
    void Write(const uint8_t* bytes) {
        DWORD protection = 0;
        VirtualProtect(target_, sizeof(saved_), PAGE_EXECUTE_READWRITE, &protection);
        std::memcpy(target_, bytes, sizeof(saved_));
        VirtualProtect(target_, sizeof(saved_), protection, &protection);
        FlushInstructionCache(GetCurrentProcess(), target_, sizeof(saved_));
    }
    uint8_t* target_ = nullptr;
    uint8_t saved_[14] = {};
};

}  // namespace

TEST(FlowLoaderBootstrap, NativeStreamlineCoreLoadBeforeFullHooksUsesOverride) {
    char executable[MAX_PATH] = {};
    ASSERT_NE(GetModuleFileNameA(nullptr, executable, MAX_PATH), 0u);
    const auto directory = std::filesystem::path(executable).parent_path();
    const auto vendorDirectory = directory / "logs" / ce::flow::CurrentTestName() / "vendor";
    std::filesystem::create_directories(vendorDirectory);
    const auto requested = vendorDirectory / "sl.common.dll";
    std::filesystem::copy_file(directory / "sl.common.dll", requested,
                               std::filesystem::copy_options::overwrite_existing);
    const auto requestedPath = requested.string();
    const auto overridePath = directory.string();
    CEFlowEarlyLoaderProbe probe{requestedPath.c_str(), overridePath.c_str(), nullptr};
    {
        ce::flow::FlowGame game(ce::flow::CurrentTestName(), &probe);
        ASSERT_TRUE(game.Error().empty()) << game.Error();
        ASSERT_NE(probe.loadedModule, nullptr);
        char loadedPath[MAX_PATH] = {};
        ASSERT_NE(GetModuleFileNameA(probe.loadedModule, loadedPath, MAX_PATH), 0u);
        EXPECT_TRUE(std::filesystem::equivalent(loadedPath, directory / "sl.common.dll"));
        EXPECT_FALSE(std::filesystem::equivalent(loadedPath, requested));
        EXPECT_TRUE(FreeLibrary(probe.loadedModule));
    }
    std::filesystem::remove(requested);
}

// No copy of the module is loaded, so the override must apply; refusing it routed the load to the tool's own image.
TEST(FlowLoaderBootstrap, AModuleQueryAnsweredByAToolsImageDoesNotBlockTheOverride) {
    char executable[MAX_PATH] = {};
    ASSERT_NE(GetModuleFileNameA(nullptr, executable, MAX_PATH), 0u);
    const auto directory = std::filesystem::path(executable).parent_path();
    const auto vendorDirectory = directory / "logs" / ce::flow::CurrentTestName() / "vendor";
    std::filesystem::create_directories(vendorDirectory);
    const auto requested = vendorDirectory / "sl.dlss_g.dll";
    std::filesystem::copy_file(directory / "sl.dlss_g.dll", requested,
                               std::filesystem::copy_options::overwrite_existing);
    const auto requestedPath = requested.string();
    const auto overridePath = directory.string();
    CEFlowEarlyLoaderProbe probe{requestedPath.c_str(), overridePath.c_str(), nullptr};
    {
        ModuleQueryAnswer answer("sl.dlss_g.dll", GetModuleHandleW(nullptr));
        ASSERT_TRUE(answer.Installed());
        ce::flow::FlowGame game(ce::flow::CurrentTestName(), &probe);
        ASSERT_TRUE(game.Error().empty()) << game.Error();
        ASSERT_NE(probe.loadedModule, nullptr);
        char loadedPath[MAX_PATH] = {};
        ASSERT_NE(GetModuleFileNameA(probe.loadedModule, loadedPath, MAX_PATH), 0u);
        EXPECT_TRUE(std::filesystem::equivalent(loadedPath, directory / "sl.dlss_g.dll"))
            << "loaded " << loadedPath << "; logs: " << game.LogDirectory();
        EXPECT_TRUE(FreeLibrary(probe.loadedModule));
    }
    std::filesystem::remove(requested);
}
