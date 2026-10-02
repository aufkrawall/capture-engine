#pragma once

// Helpers shared between the two IPC endpoints: process_ipc.cpp (message
// validation, command-line parsing and ProcessIPCServer) and
// process_ipc_client.cpp (ProcessIPCClient and child-process spawning).
// Include process_ipc.h for the public API.
//
// Validation helpers reached from here sit on an untrusted-input boundary; see
// llm-wiki/fuzzing.md and tests/fuzz/fuzz_ipc_deserialize.cpp.

#include "process_ipc.h"

#include <atomic>
#include <string>

// Serial number appended to generated pipe names so two endpoints in one
// process cannot collide.
extern std::atomic<uint64_t> g_pipeNameSequence;

bool IsValidCommand(uint16_t opcode);
bool ValidateOpcodePayload(const ProcessMessage& message);
bool IsResponseAllowed(ProcessCommand command, ProcessResponse response);
bool IsIpcMode(ProcessMode mode);

const wchar_t* ModeNameWide(ProcessMode mode);
const char* ModeName(ProcessMode mode);

std::wstring Utf8ToWide(const char* text);
std::wstring QuoteCommandLineArgument(const std::wstring& argument);

std::wstring NonceToHex(const ProcessChannelNonce& nonce);
bool FillNonce(ProcessChannelNonce& nonce);

// Owns the ACL allocated for an anonymous pipe endpoint; the SECURITY_ATTRIBUTES
// it hands out stay valid only while it is alive.
struct PipeSecurity {
    SECURITY_ATTRIBUTES attributes{};
    PSECURITY_DESCRIPTOR descriptor = nullptr;

    ~PipeSecurity() {
        if (descriptor)
            LocalFree(descriptor);
    }
};
bool BuildPipeSecurity(PipeSecurity& security);

void CancelOverlapped(HANDLE pipe, OVERLAPPED& overlapped);

ProcessMessage BuildMessage(ProcessMessageKind kind, uint16_t opcode, ProcessMode senderMode, uint64_t sequence,
                            uint32_t senderPid, const ProcessChannelNonce& nonce, const char* payload);
