// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "common/ipc/role_channel_memory.h"

#include "common/ipc/process_ipc_internal.h"
#include "common/logging/logging.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>

namespace ce::role {
namespace {

// The most commands that may wait unpolled: a tolerated command whose reply is missing and the command that
// follows it. A third one means the role is not serving its channel, which ClassifyReplyTimeout ends anyway.
constexpr size_t kMaxQueuedCommands = 2;

class ScopedEvent {
public:
    ScopedEvent(BOOL manualReset) : handle_(CreateEventW(nullptr, manualReset, FALSE, nullptr)) {}
    ~ScopedEvent() {
        if (handle_)
            CloseHandle(handle_);
    }
    ScopedEvent(const ScopedEvent&) = delete;
    ScopedEvent& operator=(const ScopedEvent&) = delete;
    HANDLE Get() const {
        return handle_;
    }

private:
    HANDLE handle_;
};

struct Frame {
    ProcessMessage message{};
    size_t bytes = 0;  // the total size the validator is given, as the pipe's read count
};

// The state both ends share. Its mutex guards the queues and is never held across a wait, so a waiting host
// never blocks a role that is replying.
struct ChannelState {
    ChannelState(ProcessMode mode, uint32_t pid)
        : role(mode), controllerPid(pid), commandReady(FALSE), replyReady(FALSE), closed(TRUE) {
        (void)FillNonce(nonce);  // all zero on failure: the pair still matches itself
    }
    bool Valid() const {
        return commandReady.Get() && replyReady.Get() && closed.Get();
    }
    // Ends the channel for both ends. Requires `mutex`. `roleMustStop` is the pipe's fatal disconnect: the role
    // must stop because the host disconnected or broke the channel.
    void Down(bool roleMustStop) {
        if (roleMustStop)
            fatal.store(true, std::memory_order_release);
        down.store(true, std::memory_order_release);
        commands.clear();
        replies.clear();
        SetEvent(closed.Get());
    }

    const ProcessMode role;
    const uint32_t controllerPid;
    ProcessChannelNonce nonce{};
    // The role end is gone. Replies it queued before leaving stay readable, as a pipe keeps what its writer wrote.
    std::atomic<bool> roleEnded{false};
    ScopedEvent commandReady;  // auto-reset: a command was queued
    ScopedEvent replyReady;    // auto-reset: a reply was queued
    ScopedEvent closed;        // manual-reset: the channel is down
    std::mutex mutex;
    std::deque<Frame> commands;  // guarded by mutex
    std::deque<Frame> replies;   // guarded by mutex
    std::atomic<bool> down{false};
    std::atomic<bool> fatal{false};
};

class InMemoryHelperClient final : public HelperClient {
public:
    explicit InMemoryHelperClient(std::shared_ptr<ChannelState> state) : state_(std::move(state)) {}
    ~InMemoryHelperClient() override {
        Disconnect();
    }

    bool SendCommand(ProcessCommand command, const char* payload, ProcessResponse* response,
                     DWORD timeoutMs) override;
    bool IsConnected() const override {
        return !state_->down.load(std::memory_order_acquire) && !state_->roleEnded.load(std::memory_order_acquire);
    }
    void Disconnect() override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->Down(true);
    }

private:
    bool AwaitReply(ProcessCommand command, uint64_t sequence, ProcessResponse* response, DWORD timeoutMs);
    // Requires state_->mutex. Host-side state (sequences) is guarded by sendMutex_, held by the caller.
    void BreakLocked(const char* reason);

    std::shared_ptr<ChannelState> state_;
    std::mutex sendMutex_;  // one command in flight; the role end never takes it
    uint64_t sequence_ = 0;
    // Sequence of a tolerated command whose reply never arrived (0 = none). Its reply, if it still comes, is
    // discarded by the next command, exactly as the pipe does.
    uint64_t unansweredSequence_ = 0;
};

class InMemoryRoleChannel final : public RoleChannel {
public:
    explicit InMemoryRoleChannel(std::shared_ptr<ChannelState> state) : state_(std::move(state)) {}
    ~InMemoryRoleChannel() override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->roleEnded.store(true, std::memory_order_release);
        SetEvent(state_->closed.Get());  // a host waiting for a reply that will not come returns at once
    }

    bool PollCommand(ProcessCommand& command, char* payload, size_t payloadSize) override;
    bool SendResponse(ProcessResponse response, const char* payload) override;
    bool WaitForCommand(DWORD timeoutMs) override;
    bool IsConnected() const override {
        return !state_->down.load(std::memory_order_acquire);
    }
    bool HasFatalDisconnect() const override {
        return state_->fatal.load(std::memory_order_acquire);
    }
    uint32_t ControllerPid() const override {
        return state_->controllerPid;
    }

private:
    std::shared_ptr<ChannelState> state_;
    uint64_t lastSequence_ = 0;  // the sequence of the last command polled; replies carry it
};

bool InMemoryHelperClient::SendCommand(ProcessCommand command, const char* payload, ProcessResponse* response,
                                       DWORD timeoutMs) {
    std::lock_guard<std::mutex> serial(sendMutex_);
    if (!IsConnected() || !IsValidCommand(static_cast<uint16_t>(command)) ||
        (payload && strnlen(payload, PROCESS_MAX_PAYLOAD) == PROCESS_MAX_PAYLOAD)) {
        return false;
    }
    if (sequence_ == std::numeric_limits<uint64_t>::max()) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        BreakLocked("sequence exhausted");
        return false;
    }
    const uint64_t sequence = ++sequence_;
    const ProcessMessage request =
        BuildMessage(ProcessMessageKind::Command, static_cast<uint16_t>(command), ProcessMode::Controller, sequence,
                     state_->controllerPid, state_->nonce, payload);
    if (!ValidateOpcodePayload(request))
        return false;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->down.load(std::memory_order_acquire) || state_->roleEnded.load(std::memory_order_acquire))
            return false;
        if (state_->commands.size() >= kMaxQueuedCommands) {
            BreakLocked("commands queued without a reply");
            return false;
        }
        Frame frame;
        frame.message = request;
        frame.bytes = request.totalSize;
        state_->commands.push_back(frame);
    }
    SetEvent(state_->commandReady.Get());
    return AwaitReply(command, sequence, response, timeoutMs);
}

bool InMemoryHelperClient::AwaitReply(ProcessCommand command, uint64_t sequence, ProcessResponse* response,
                                      DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        Frame reply;
        bool received = false;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (!state_->replies.empty()) {
                reply = std::move(state_->replies.front());
                state_->replies.pop_front();
                received = true;
            } else if (state_->down.load(std::memory_order_acquire) || state_->roleEnded.load(std::memory_order_acquire)) {
                return false;
            }
        }
        if (received) {
            if (unansweredSequence_ != 0 && reply.message.sequence < sequence &&
                ValidateProcessMessage(reply.message, reply.bytes, ProcessMessageKind::Response, state_->role,
                                       state_->controllerPid, state_->nonce, unansweredSequence_ - 1, false)) {
                LogInfo("[IPC] %s late reply for seq=%llu discarded", ModeName(state_->role),
                        static_cast<unsigned long long>(reply.message.sequence));
                unansweredSequence_ = 0;
                continue;
            }
            const auto replied = static_cast<ProcessResponse>(reply.message.opcode);
            if (!ValidateProcessMessage(reply.message, reply.bytes, ProcessMessageKind::Response, state_->role,
                                        state_->controllerPid, state_->nonce, sequence, true) ||
                !IsResponseAllowed(command, replied)) {
                std::lock_guard<std::mutex> lock(state_->mutex);
                BreakLocked("invalid reply");
                return false;
            }
            // An in-order reply proves an earlier tolerated command was consumed or lost, so nothing is outstanding.
            unansweredSequence_ = 0;
            if (response)
                *response = replied;
            return true;
        }

        const ULONGLONG now = GetTickCount64();
        const DWORD remaining = now < deadline ? static_cast<DWORD>(deadline - now) : 0;
        if (remaining == 0) {
            if (ClassifyReplyTimeout(command, unansweredSequence_ != 0) == ReplyTimeoutOutcome::kKeepChannel) {
                unansweredSequence_ = sequence;
                LogWarn(
                    "[IPC] %s reply to command %u (seq=%llu) missed its %lu ms window; keeping the channel and "
                    "discarding the reply if it arrives late",
                    ModeName(state_->role), static_cast<unsigned>(command), static_cast<unsigned long long>(sequence),
                    static_cast<unsigned long>(timeoutMs));
                return false;
            }
            std::lock_guard<std::mutex> lock(state_->mutex);
            BreakLocked(unansweredSequence_ != 0 ? "second missed reply" : "reply timeout");
            return false;
        }
        const HANDLE events[] = {state_->replyReady.Get(), state_->closed.Get()};
        if (WaitForMultipleObjects(2, events, FALSE, remaining) == WAIT_FAILED) {
            std::lock_guard<std::mutex> lock(state_->mutex);
            BreakLocked("wait failed");
            return false;
        }
    }
}

void InMemoryHelperClient::BreakLocked(const char* reason) {
    if (!state_->down.load(std::memory_order_acquire)) {
        LogWarn("[IPC] %s in-process command channel broke (%s); a fresh role thread is required",
                ModeName(state_->role), reason);
    }
    state_->Down(true);
    unansweredSequence_ = 0;
}

bool InMemoryRoleChannel::PollCommand(ProcessCommand& command, char* payload, size_t payloadSize) {
    Frame frame;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->down.load(std::memory_order_acquire) || state_->commands.empty())
            return false;
        frame = std::move(state_->commands.front());
        state_->commands.pop_front();
    }
    if (!ValidateProcessMessage(frame.message, frame.bytes, ProcessMessageKind::Command, ProcessMode::Controller,
                                state_->controllerPid, state_->nonce, lastSequence_, false)) {
        LogWarn("[IPC] %s role rejected a command: identity/header/sequence mismatch", ModeName(state_->role));
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->Down(true);
        return false;
    }
    lastSequence_ = frame.message.sequence;
    command = static_cast<ProcessCommand>(frame.message.opcode);
    if (payload && payloadSize != 0) {
        payload[0] = '\0';
        if (frame.message.payloadSize != 0) {
            const size_t copyLength = std::min(payloadSize - 1, static_cast<size_t>(frame.message.payloadSize - 1));
            memcpy(payload, frame.message.payload, copyLength);
            payload[copyLength] = '\0';
        }
    }
    return true;
}

bool InMemoryRoleChannel::SendResponse(ProcessResponse response, const char* payload) {
    if (payload && strnlen(payload, PROCESS_MAX_PAYLOAD) == PROCESS_MAX_PAYLOAD)
        return false;
    const ProcessMessage message =
        BuildMessage(ProcessMessageKind::Response, static_cast<uint16_t>(response), state_->role, lastSequence_,
                     state_->controllerPid, state_->nonce, payload);
    if (!ValidateProcessMessage(message, message.totalSize, ProcessMessageKind::Response, state_->role,
                                state_->controllerPid, state_->nonce, lastSequence_, true) ||
        !ValidateOpcodePayload(message)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->down.load(std::memory_order_acquire))
            return false;
        Frame frame;
        frame.message = message;
        frame.bytes = message.totalSize;
        state_->replies.push_back(frame);
    }
    SetEvent(state_->replyReady.Get());
    return true;
}

bool InMemoryRoleChannel::WaitForCommand(DWORD timeoutMs) {
    {
        // Checked under the lock the host queues under, so a command queued before this wait is never missed.
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->down.load(std::memory_order_acquire) || !state_->commands.empty())
            return true;
    }
    const HANDLE events[] = {state_->commandReady.Get(), state_->closed.Get()};
    const DWORD result = WaitForMultipleObjects(2, events, FALSE, timeoutMs);
    return result == WAIT_OBJECT_0 || result == WAIT_OBJECT_0 + 1;
}

}  // namespace

InMemoryChannelPair MakeInMemoryChannel(ProcessMode role, uint32_t controllerPid) {
    auto state = std::make_shared<ChannelState>(role, controllerPid);
    if (!state->Valid()) {
        LogError("[IPC] %s in-process command channel cannot be created", ModeName(role));
        return {};
    }
    InMemoryChannelPair pair;
    pair.host = std::make_unique<InMemoryHelperClient>(state);
    pair.role = std::make_unique<InMemoryRoleChannel>(state);
    return pair;
}

}  // namespace ce::role
