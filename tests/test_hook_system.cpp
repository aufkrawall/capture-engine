#include <gtest/gtest.h>
#include <array>

#include "../hook/wrappers/hook_system.h"

namespace {

void* g_LastFunctionTarget = nullptr;
void* g_LastFunctionOriginal = nullptr;
void* g_LastExportOriginal = nullptr;
void* g_LastRemovedFunctionTarget = nullptr;
const char* g_LastRemovedExportModule = nullptr;
const char* g_LastRemovedExportName = nullptr;
void* g_LastRemovedVtableEntry = nullptr;
int g_InitializeCalls = 0;
int g_ShutdownCalls = 0;

bool StubInitialize() {
    ++g_InitializeCalls;
    return true;
}

void StubShutdown() {
    ++g_ShutdownCalls;
}

const char* StubStatusString(CustomHook::Status status) {
    switch (status) {
        case CustomHook::Status::Success:
            return "Success";
        case CustomHook::Status::ErrorInvalidParameter:
            return "Invalid parameter";
        default:
            return "Other";
    }
}

CustomHook::Status StubHookFunction(void* target, void* detour, void** original) {
    g_LastFunctionTarget = target;
    if (original) {
        *original = reinterpret_cast<void*>(0x1234);
        g_LastFunctionOriginal = *original;
    }
    return target && detour ? CustomHook::Status::Success : CustomHook::Status::ErrorInvalidParameter;
}

CustomHook::Status StubHookExport(const char* moduleName, const char* functionName, void* detour, void** original) {
    if (original) {
        *original = reinterpret_cast<void*>(0x5678);
        g_LastExportOriginal = *original;
    }
    return moduleName && functionName && detour ? CustomHook::Status::Success
                                                : CustomHook::Status::ErrorInvalidParameter;
}

CustomHook::Status StubHookExportW(const wchar_t* moduleName, const char* functionName, void* detour, void** original) {
    return StubHookExport(moduleName ? "wide" : nullptr, functionName, detour, original);
}

CustomHook::Status StubHookVtableEntry(void** vtableEntry, void* detour, void** original) {
    if (!vtableEntry || !detour) {
        return CustomHook::Status::ErrorInvalidParameter;
    }
    if (original) {
        *original = *vtableEntry;
    }
    *vtableEntry = detour;
    return CustomHook::Status::Success;
}

CustomHook::Status StubUnhookFunction(void* target, void* original) {
    g_LastRemovedFunctionTarget = target;
    g_LastFunctionOriginal = original;
    return CustomHook::Status::Success;
}

CustomHook::Status StubUnhookExport(const char* moduleName, const char* functionName, void* original) {
    g_LastRemovedExportModule = moduleName;
    g_LastRemovedExportName = functionName;
    g_LastExportOriginal = original;
    return CustomHook::Status::Success;
}

CustomHook::Status StubUnhookVtableEntry(void** vtableEntry, void* original) {
    g_LastRemovedVtableEntry = vtableEntry;
    if (vtableEntry && original) {
        *vtableEntry = original;
    }
    return CustomHook::Status::Success;
}

HookSystem::HookBackendOps MakeStubOps() {
    return HookSystem::HookBackendOps{StubInitialize,   StubShutdown,         StubStatusString,    StubHookFunction,
                                      StubHookExport,   StubHookExportW,      StubHookVtableEntry, StubUnhookFunction,
                                      StubUnhookExport, StubUnhookVtableEntry};
}

class HookSystemTest : public ::testing::Test {
protected:
    void SetUp() override {
        g_LastFunctionTarget = nullptr;
        g_LastFunctionOriginal = nullptr;
        g_LastExportOriginal = nullptr;
        g_LastRemovedFunctionTarget = nullptr;
        g_LastRemovedExportModule = nullptr;
        g_LastRemovedExportName = nullptr;
        g_LastRemovedVtableEntry = nullptr;
        g_InitializeCalls = 0;
        g_ShutdownCalls = 0;
        HookSystem::ResetHookBackendOpsForTesting();
        HookSystem::SetHookBackendOpsForTesting(MakeStubOps());
        HookSystem::Shutdown();
    }

    void TearDown() override {
        HookSystem::Shutdown();
        HookSystem::ResetHookBackendOpsForTesting();
    }
};

void DummyDetour() {}

void DummyOriginal() {}

}  // namespace

TEST_F(HookSystemTest, CreateFunctionHookStoresAndRemovesHook) {
    ASSERT_TRUE(HookSystem::Initialize());

    void* original = nullptr;
    ASSERT_TRUE(HookSystem::CreateFunctionHook(reinterpret_cast<void*>(0x1000), reinterpret_cast<void*>(&DummyDetour),
                                               &original));
    EXPECT_EQ(original, reinterpret_cast<void*>(0x1234));
    EXPECT_EQ(g_LastFunctionTarget, reinterpret_cast<void*>(0x1000));

    EXPECT_TRUE(HookSystem::DisableHook(reinterpret_cast<void*>(0x1000)));
    EXPECT_TRUE(HookSystem::EnableHook(reinterpret_cast<void*>(0x1000)));

    HookSystem::RemoveHook(reinterpret_cast<void*>(0x1000));
    EXPECT_EQ(g_LastRemovedFunctionTarget, reinterpret_cast<void*>(0x1000));
    EXPECT_EQ(g_LastFunctionOriginal, reinterpret_cast<void*>(0x1234));
}

TEST_F(HookSystemTest, CreateFunctionHookRejectsDuplicateTarget) {
    ASSERT_TRUE(HookSystem::Initialize());

    ASSERT_TRUE(HookSystem::CreateFunctionHook(reinterpret_cast<void*>(0x2000), reinterpret_cast<void*>(&DummyDetour),
                                               nullptr));
    EXPECT_FALSE(HookSystem::CreateFunctionHook(reinterpret_cast<void*>(0x2000),
                                                reinterpret_cast<void*>(&DummyOriginal), nullptr));
}

TEST_F(HookSystemTest, CreateExportHookTracksRemovalByModuleAndName) {
    ASSERT_TRUE(HookSystem::Initialize());

    void* original = nullptr;
    ASSERT_TRUE(HookSystem::CreateExportHook("missing-module.dll", "CreateThing", reinterpret_cast<void*>(&DummyDetour),
                                             &original));
    EXPECT_EQ(original, reinterpret_cast<void*>(0x5678));

    HookSystem::RemoveHook(reinterpret_cast<void*>(&DummyDetour));
    EXPECT_STREQ(g_LastRemovedExportModule, "missing-module.dll");
    EXPECT_STREQ(g_LastRemovedExportName, "CreateThing");
    EXPECT_EQ(g_LastExportOriginal, reinterpret_cast<void*>(0x5678));
}

TEST_F(HookSystemTest, CreateComHookRestoresOriginalEntryOnRemove) {
    ASSERT_TRUE(HookSystem::Initialize());

    std::array<void*, 1> vtable{reinterpret_cast<void*>(&DummyOriginal)};
    void* original = nullptr;
    ASSERT_TRUE(HookSystem::CreateCOMHook(&vtable[0], reinterpret_cast<void*>(&DummyDetour), &original));
    EXPECT_EQ(original, reinterpret_cast<void*>(&DummyOriginal));
    EXPECT_EQ(vtable[0], reinterpret_cast<void*>(&DummyDetour));

    HookSystem::RemoveHook(&vtable[0]);
    EXPECT_EQ(g_LastRemovedVtableEntry, &vtable[0]);
    EXPECT_EQ(vtable[0], reinterpret_cast<void*>(&DummyOriginal));
}

TEST_F(HookSystemTest, ScopedInitializerUsesSharedInitRefcount) {
    {
        HookSystem::ScopedInitializer first;
        ASSERT_TRUE(first.IsInitialized());
        EXPECT_EQ(g_InitializeCalls, 1);
        EXPECT_EQ(g_ShutdownCalls, 0);

        {
            HookSystem::ScopedInitializer second;
            ASSERT_TRUE(second.IsInitialized());
            EXPECT_EQ(g_InitializeCalls, 1);
            EXPECT_EQ(g_ShutdownCalls, 0);
        }

        EXPECT_EQ(g_ShutdownCalls, 0);
    }

    EXPECT_EQ(g_InitializeCalls, 1);
    EXPECT_EQ(g_ShutdownCalls, 1);
}

TEST_F(HookSystemTest, ShutdownAtZeroDoesNotUnderflowOrCallBackend) {
    HookSystem::Shutdown();
    HookSystem::Shutdown();

    EXPECT_EQ(g_ShutdownCalls, 0);

    ASSERT_TRUE(HookSystem::Initialize());
    EXPECT_EQ(g_InitializeCalls, 1);

    HookSystem::Shutdown();
    EXPECT_EQ(g_ShutdownCalls, 1);
}

TEST_F(HookSystemTest, EnableAndDisableAllHooksAffectTypedHookStateTransitions) {
    ASSERT_TRUE(HookSystem::Initialize());

    HookSystem::TypedHook<void (*)()> hook;
    ASSERT_TRUE(hook.Create(reinterpret_cast<void*>(0x3000), reinterpret_cast<void*>(&DummyDetour)));
    EXPECT_TRUE(hook.IsCreated());
    EXPECT_TRUE(hook.IsEnabled());
    EXPECT_EQ(hook.Original(), reinterpret_cast<void (*)()>(0x1234));

    EXPECT_FALSE(hook.Enable());
    EXPECT_TRUE(hook.Disable());
    EXPECT_FALSE(hook.IsEnabled());
    EXPECT_FALSE(hook.Disable());

    EXPECT_TRUE(HookSystem::EnableAllHooks());
    EXPECT_TRUE(hook.Enable());
    EXPECT_TRUE(hook.IsEnabled());

    EXPECT_TRUE(HookSystem::DisableAllHooks());
    EXPECT_TRUE(hook.Disable());
    EXPECT_FALSE(hook.IsEnabled());
}

TEST_F(HookSystemTest, TypedHookCreateExportTracksOriginalAndRemovesOnDestruction) {
    ASSERT_TRUE(HookSystem::Initialize());

    {
        HookSystem::TypedHook<void (*)()> hook;
        ASSERT_TRUE(hook.CreateExport("missing-module.dll", "ExportedThing", reinterpret_cast<void*>(&DummyDetour)));
        EXPECT_TRUE(hook.IsCreated());
        EXPECT_TRUE(hook.IsEnabled());
        EXPECT_EQ(hook.Original(), reinterpret_cast<void (*)()>(0x5678));
        EXPECT_FALSE(hook.CreateExport("missing-module.dll", "ExportedThing", reinterpret_cast<void*>(&DummyDetour)));
    }

    EXPECT_STREQ(g_LastRemovedExportModule, "missing-module.dll");
    EXPECT_STREQ(g_LastRemovedExportName, "ExportedThing");
    EXPECT_EQ(g_LastExportOriginal, reinterpret_cast<void*>(0x5678));
}

TEST_F(HookSystemTest, TypedHookDestructorRemovesFunctionHook) {
    ASSERT_TRUE(HookSystem::Initialize());

    {
        HookSystem::TypedHook<void (*)()> hook;
        ASSERT_TRUE(hook.Create(reinterpret_cast<void*>(0x4000), reinterpret_cast<void*>(&DummyDetour)));
    }

    EXPECT_EQ(g_LastRemovedFunctionTarget, reinterpret_cast<void*>(0x4000));
    EXPECT_EQ(g_LastFunctionOriginal, reinterpret_cast<void*>(0x1234));
}
