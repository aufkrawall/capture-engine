#include <gtest/gtest.h>

#include "runtime/desktop/hotkey_input_hook.h"

// SetWindowsHookEx's hMod is the module containing the hook procedure. While the runtime is linked
// into the executable that is the executable itself, which is what the hook always passed; a DLL
// build answers with the DLL (checked by hand with an exe + DLL probe, see the commit message).
TEST(HotkeyInputHookModuleTest, ExecutableHostedHookNamesTheExecutable) {
    const HMODULE module = HotkeyInputHookModule();
    ASSERT_NE(module, nullptr);
    EXPECT_EQ(module, GetModuleHandleW(nullptr));
}

TEST(HotkeyInputHookModuleTest, HookIsInactiveUntilStarted) {
    EXPECT_FALSE(IsHotkeyInputHookActive());
    EXPECT_EQ(GetHotkeyInputHookDeliveredCount(), 0u);
    StopHotkeyInputHook();  // stopping a hook that never started is a no-op
    EXPECT_FALSE(IsHotkeyInputHookActive());
}
