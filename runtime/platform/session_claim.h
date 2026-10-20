// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

// One CaptureEngine runtime per Windows session (llm-wiki/library/04 runtime/platform). ce_runtime_create claims the
// session once; while another process holds the claim it answers CE_E_BUSY. The kernel releases a claim when its
// holder exits, so a restart that waited for the prior process claims at once: there is no retry loop.

#include <windows.h>

#include <optional>

namespace ce::runtime {

// The name every CaptureEngine version uses, so older and newer installations exclude each other.
inline constexpr wchar_t kSessionClaimName[] = L"Local\\CaptureEngine_Instance_Mutex";

class SessionClaim {
public:
    enum class Refusal { None, Held, Unavailable };

    // The claim, or nothing: `refusal` says whether another holder exists (Held) or the object cannot be created
    // (Unavailable, `error` then carries the Win32 error).
    static std::optional<SessionClaim> TryAcquire(const wchar_t* name, Refusal* refusal = nullptr,
                                                  DWORD* error = nullptr);

    SessionClaim(SessionClaim&& other) noexcept;
    SessionClaim& operator=(SessionClaim&& other) noexcept;
    SessionClaim(const SessionClaim&) = delete;
    SessionClaim& operator=(const SessionClaim&) = delete;
    ~SessionClaim();

private:
    explicit SessionClaim(HANDLE handle) : handle_(handle) {}
    HANDLE handle_ = nullptr;
};

}  // namespace ce::runtime
