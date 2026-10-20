// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

// The command channel of a helper role that runs on a runtime thread (DR-37, DR-45). The protocol is the
// authenticated pipe's: ProcessMessage is the unit, both ends run ValidateProcessMessage, a command is awaited
// before the next one is sent, and a missed reply is classified by ClassifyReplyTimeout. Both ends live in one
// process, so there is no authentication; the nonce only pins the two ends of one pair together.

#include "common/ipc/process_ipc.h"

#include <cstdint>
#include <memory>

namespace ce::role {
struct InMemoryChannelPair {
    std::unique_ptr<HelperClient> host;  // the supervisor's end (the pipe's ProcessIPCClient)
    std::unique_ptr<RoleChannel> role;   // the role's end (the pipe's ProcessIPCServer)
};

// `controllerPid` is the runtime's process: the sender of every command and of every reply. Both members are
// null when the channel's events cannot be created.
InMemoryChannelPair MakeInMemoryChannel(ProcessMode role, uint32_t controllerPid);
}  // namespace ce::role
