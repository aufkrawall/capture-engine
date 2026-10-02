#pragma once

#include <windows.h>
#include <sddl.h>
#include <aclapi.h>
#include <evntcons.h>
#include <string>
#include <vector>

namespace ce::elevation {

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE source) : value_(source) {}
    ~Handle() {
        Reset();
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.Release()) {}
    Handle& operator=(Handle&& other) noexcept {
        Reset(other.Release());
        return *this;
    }
    HANDLE Get() const {
        return value_;
    }
    explicit operator bool() const {
        return value_ && value_ != INVALID_HANDLE_VALUE;
    }
    HANDLE Release() {
        HANDLE result = value_;
        value_ = nullptr;
        return result;
    }
    void Reset(HANDLE source = nullptr) {
        if (*this)
            CloseHandle(value_);
        value_ = source;
    }

private:
    HANDLE value_ = nullptr;
};

inline std::wstring ExecutablePath() {
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size >= path.size())
        return {};
    path.resize(size);
    return path;
}

inline std::wstring QuoteArgument(std::wstring_view argument) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t character : argument) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        result.append(character == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result.push_back(character);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

inline std::wstring ProcessUserSid(HANDLE process, bool* administrator = nullptr) {
    Handle token;
    HANDLE raw = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &raw))
        return {};
    token.Reset(raw);
    DWORD size = 0;
    GetTokenInformation(token.Get(), TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> bytes(size);
    if (!size || !GetTokenInformation(token.Get(), TokenUser, bytes.data(), size, &size))
        return {};
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &text))
        return {};
    std::wstring result(text);
    LocalFree(text);
    if (administrator) {
        *administrator = false;
        size = 0;
        GetTokenInformation(token.Get(), TokenGroups, nullptr, 0, &size);
        bytes.resize(size);
        if (size && GetTokenInformation(token.Get(), TokenGroups, bytes.data(), size, &size)) {
            const auto* groups = reinterpret_cast<TOKEN_GROUPS*>(bytes.data());
            for (DWORD index = 0; index < groups->GroupCount; ++index) {
                if (IsWellKnownSid(groups->Groups[index].Sid, WinBuiltinAdministratorsSid))
                    *administrator = true;
            }
        }
    }
    return result;
}

inline bool IsElevated() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw))
        return false;
    Handle token(raw);
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    return GetTokenInformation(token.Get(), TokenElevation, &elevation, sizeof(elevation), &size) &&
           elevation.TokenIsElevated != 0;
}

class Security {
public:
    explicit Security(const std::wstring& sddl) {
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor_, nullptr))
            attributes_ = {sizeof(SECURITY_ATTRIBUTES), descriptor_, FALSE};
    }
    ~Security() {
        if (descriptor_)
            LocalFree(descriptor_);
    }
    Security(const Security&) = delete;
    Security& operator=(const Security&) = delete;
    SECURITY_ATTRIBUTES* Get() {
        return descriptor_ ? &attributes_ : nullptr;
    }
    PSECURITY_DESCRIPTOR Descriptor() const {
        return descriptor_;
    }

private:
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
    SECURITY_ATTRIBUTES attributes_{};
};

// Cancel and drain the exact outstanding operation before its stack storage dies.
inline bool Transfer(HANDLE pipe, void* bytes, DWORD size, bool writing, HANDLE stop = nullptr, HANDLE owner = nullptr,
                     DWORD timeoutMs = 5000) {
    DWORD done = 0;
    const uint64_t deadline = GetTickCount64() + timeoutMs;
    while (done < size) {
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event)
            return false;
        OVERLAPPED operation{};
        operation.hEvent = event.Get();
        DWORD transferred = 0;
        const BOOL started =
            writing ? WriteFile(pipe, static_cast<char*>(bytes) + done, size - done, &transferred, &operation)
                    : ReadFile(pipe, static_cast<char*>(bytes) + done, size - done, &transferred, &operation);
        if (!started && GetLastError() != ERROR_IO_PENDING)
            return false;
        if (!started) {
            HANDLE waits[3] = {event.Get(), stop, owner};
            DWORD count = 1;
            if (stop)
                waits[count++] = stop;
            if (owner)
                waits[count++] = owner;
            const uint64_t now = GetTickCount64();
            const DWORD remaining = timeoutMs == INFINITE ? INFINITE
                                    : now < deadline      ? static_cast<DWORD>(deadline - now)
                                                          : 0;
            if (WaitForMultipleObjects(count, waits, FALSE, remaining) != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &operation);
                GetOverlappedResult(pipe, &operation, &transferred, TRUE);
                SetLastError(ERROR_OPERATION_ABORTED);
                return false;
            }
            if (!GetOverlappedResult(pipe, &operation, &transferred, FALSE))
                return false;
        }
        if (!transferred)
            return false;
        done += transferred;
    }
    return true;
}

inline constexpr GUID kTraceGuid = {0x2c3554c7, 0x9b8e, 0x4418, {0x91, 0x51, 0x94, 0x2a, 0x19, 0x43, 0x26, 0x01}};

}  // namespace ce::elevation
