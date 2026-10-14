#include "loader_init_apc_load.h"

#include <cstddef>
#include <vector>
#include <cstring>

namespace ce::loader_init_apc {

namespace {

struct LoadPacket {
    uint64_t loadLibrary;
    uint64_t getLastError;
    uint64_t ntSetEvent;
    uint64_t path;
    uint64_t completion;
    uint64_t module;
    DWORD error;
    DWORD reserved;
};
static_assert(offsetof(LoadPacket, path) == 24 && offsetof(LoadPacket, module) == 40 &&
              offsetof(LoadPacket, error) == 48 && sizeof(LoadPacket) == 56);

// Native x64 PAPCFUNC(LoadPacket*): preserve RBX, provide ABI shadow space,
// call LoadLibraryW and capture its full-width result / GetLastError. Restore
// the APC stack BEFORE tail-jumping to NtSetEvent(event, nullptr). No return
// address or pending instruction then points into this allocation when the
// host observes completion. ENDBR64 preserves CET indirect-entry compatibility.
#if defined(_WIN64)
constexpr BYTE kLoadApc[] = {
    0xf3, 0x0f, 0x1e, 0xfa,              // endbr64
    0x53,                                // push rbx
    0x48, 0x83, 0xec, 0x20,              // sub rsp, 32
    0x48, 0x89, 0xcb,                    // mov rbx, rcx
    0x48, 0x8b, 0x4b, 0x18,              // mov rcx, [rbx+24]
    0xff, 0x13,                          // call [rbx]
    0x48, 0x89, 0x43, 0x28,              // mov [rbx+40], rax
    0x48, 0x85, 0xc0,                    // test rax, rax
    0x75, 0x06,                          // jnz success
    0xff, 0x53, 0x08,                    // call [rbx+8]
    0x89, 0x43, 0x30,                    // mov [rbx+48], eax
    0x48, 0x8b, 0x4b, 0x20,              // mov rcx, [rbx+32]
    0x31, 0xd2,                          // xor edx, edx
    0x48, 0x8b, 0x43, 0x10,              // mov rax, [rbx+16]
    0x48, 0x83, 0xc4, 0x20,              // add rsp, 32
    0x5b,                                // pop rbx
    0xff, 0xe0,                          // jmp rax (NtSetEvent)
};
#endif

}  // namespace

LoadRequest::~LoadRequest() {
    Reset();
}

void LoadRequest::Reset() {
    // A queued, unacknowledged APC cannot be cancelled safely. Retain its
    // target allocations/handle until process exit rather than free a future
    // instruction or argument. Our own handles always close on shutdown.
    const bool consumed = !queued_ || WaitForSingleObject(completion_, 0) == WAIT_OBJECT_0 ||
                          WaitForSingleObject(process_, 0) == WAIT_OBJECT_0;
    if (consumed) {
        if (remoteCode_)
            VirtualFreeEx(process_, remoteCode_, 0, MEM_RELEASE);
        if (remoteData_)
            VirtualFreeEx(process_, remoteData_, 0, MEM_RELEASE);
        if (remoteEvent_)
            DuplicateHandle(process_, remoteEvent_, nullptr, nullptr, 0, FALSE, DUPLICATE_CLOSE_SOURCE);
    }
    if (completion_)
        CloseHandle(completion_);
    if (process_)
        CloseHandle(process_);
    process_ = completion_ = remoteEvent_ = nullptr;
    remoteData_ = remoteCode_ = nullptr;
    queued_ = false;
}

Result LoadRequest::Queue(HANDLE process, DWORD pid, const std::wstring& path, void* loadLibrary) {
    Result result;
#if defined(_WIN64)
    BOOL wow64 = FALSE;
    if (process_ || !process || !loadLibrary || path.empty() || pid == GetCurrentProcessId() ||
        !IsWow64Process(process, &wow64) || wow64) {
        result.phase = Phase::Unsupported;
        return result;
    }
    // Avoid remote allocations on late injections. The decisive phase check
    // still happens with the initial thread suspended in QueueBeforeEntryPoint.
    result.phase = ReadPhase(process, &result.error);
    if (!RunsBeforeEntryPoint(result.phase))
        return result;

    const auto prepare = [&] {
        // Extra duplication rights belong only to this optional route. A target
        // refusing them must retain the ordinary remote-thread injection path.
        process_ = OpenProcess(PROCESS_DUP_HANDLE | PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE |
                               PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, pid);
        if (!process_)
            return false;
        completion_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!completion_ || !DuplicateHandle(GetCurrentProcess(), completion_, process_, &remoteEvent_,
                                             EVENT_MODIFY_STATE, FALSE, 0))
            return false;
        const size_t bytes = sizeof(LoadPacket) + (path.size() + 1) * sizeof(wchar_t);
        remoteData_ = VirtualAllocEx(process_, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        remoteCode_ = VirtualAllocEx(process_, nullptr, sizeof(kLoadApc), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remoteData_ || !remoteCode_)
            return false;
        const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        LoadPacket packet{};
        packet.loadLibrary = reinterpret_cast<uint64_t>(loadLibrary);
        packet.getLastError = reinterpret_cast<uint64_t>(GetProcAddress(kernel, "GetLastError"));
        packet.ntSetEvent = reinterpret_cast<uint64_t>(GetProcAddress(ntdll, "NtSetEvent"));
        packet.path = reinterpret_cast<uint64_t>(static_cast<BYTE*>(remoteData_) + sizeof(packet));
        packet.completion = reinterpret_cast<uint64_t>(remoteEvent_);
        if (!packet.getLastError || !packet.ntSetEvent) {
            SetLastError(ERROR_PROC_NOT_FOUND);
            return false;
        }
        std::vector<BYTE> data(bytes);
        std::memcpy(data.data(), &packet, sizeof(packet));
        std::memcpy(data.data() + sizeof(packet), path.c_str(), bytes - sizeof(packet));
        DWORD oldProtection = 0;
        return WriteProcessMemory(process_, remoteData_, data.data(), data.size(), nullptr) &&
               WriteProcessMemory(process_, remoteCode_, kLoadApc, sizeof(kLoadApc), nullptr) &&
               VirtualProtectEx(process_, remoteCode_, sizeof(kLoadApc), PAGE_EXECUTE_READ, &oldProtection) &&
               FlushInstructionCache(process_, remoteCode_, sizeof(kLoadApc));
    };
    if (!prepare()) {
        result.error = GetLastError();
        Reset();
        return result;
    }
    result = QueueBeforeEntryPoint(process_, pid, reinterpret_cast<PAPCFUNC>(remoteCode_),
                                  reinterpret_cast<ULONG_PTR>(remoteData_));
    queued_ = result.queued;
    if (!queued_)
        Reset();
#else
    (void)process;
    (void)pid;
    (void)path;
    (void)loadLibrary;
    result.phase = Phase::Unsupported;
#endif
    return result;
}

LoadResult LoadRequest::ReadResult() const {
    if (!queued_)
        return {LoadState::Failed, 0, ERROR_INVALID_STATE};
    const DWORD wait = WaitForSingleObject(completion_, 0);
    if (wait == WAIT_TIMEOUT) {
        if (WaitForSingleObject(process_, 0) == WAIT_OBJECT_0)
            return {LoadState::Failed, 0, ERROR_PROCESS_ABORTED};
        return {};
    }
    if (wait != WAIT_OBJECT_0)
        return {LoadState::Failed, 0, GetLastError()};
    LoadPacket packet{};
    if (!ReadProcessMemory(process_, remoteData_, &packet, sizeof(packet), nullptr))
        return {LoadState::Failed, 0, GetLastError()};
    return {packet.module ? LoadState::Loaded : LoadState::Failed, packet.module, packet.error};
}

}  // namespace ce::loader_init_apc
