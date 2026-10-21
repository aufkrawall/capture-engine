// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
// ce_role_host_main serves only launches the runtime makes (libcengine 11, part 6): a client's own start must never
// be mistaken for a helper, whatever its arguments are.
#include "captureengine/app/role_host_command_line.h"

#include <gtest/gtest.h>

#include <string>

using ce::role_host::CommandLineTail;
using ce::role_host::IsClientHelperLaunch;

TEST(RoleHostCommandLineTest, TheTailSkipsAQuotedOrBareProgramNameLikeWinMain) {
    EXPECT_STREQ(CommandLineTail("\"C:\\Program Files\\Game Tool\\tool.exe\" --mode=media --config=x"),
                 "--mode=media --config=x");
    EXPECT_STREQ(CommandLineTail("tool.exe   --sensor-bridge"), "--sensor-bridge");
    EXPECT_STREQ(CommandLineTail("tool.exe\t--ce-setup --ce-context=x"), "--ce-setup --ce-context=x");
    EXPECT_STREQ(CommandLineTail("\"tool.exe\""), "");
    EXPECT_STREQ(CommandLineTail("tool.exe"), "");
    EXPECT_STREQ(CommandLineTail(nullptr), "");
}

TEST(RoleHostCommandLineTest, EveryHelperCommandTheRuntimeWritesIsRecognized) {
    for (const char* tail : {"--mode=inject --config=\"C:\\p\\config.ini\"", "--mode=media --session-dir=s",
                             "--mode=logger --parent-pid=4", "--mode=sensors", "--ce-setup --ce-context=pipe",
                             "--process-loopback-worker 1 2", "--sensor-bridge", "--install-pawnio",
                             "--uninstall-pawnio"}) {
        EXPECT_TRUE(IsClientHelperLaunch(tail)) << tail;
    }
}

TEST(RoleHostCommandLineTest, AClientsOwnStartIsNeverAHelperLaunch) {
    for (const char* tail : {"", "--record-ms 3000 --video", "--level 3 --mode=media", "--mode=mediaplayer",
                             "--mode=", "--dump-helper --dump-helper-pid=1", "--launch game.exe --mode=inject",
                             "--ce-setupx", "-mode=inject", "--MODE=inject"}) {
        EXPECT_FALSE(IsClientHelperLaunch(tail)) << tail;
    }
    EXPECT_FALSE(IsClientHelperLaunch(nullptr));
}
