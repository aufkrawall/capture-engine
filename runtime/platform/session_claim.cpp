// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "runtime/platform/session_claim.h"

#include <utility>

namespace ce::runtime {

std::optional<SessionClaim> SessionClaim::TryAcquire(const wchar_t* name, Refusal* refusal, DWORD* error) {
    if (refusal)
        *refusal = Refusal::None;
    if (error)
        *error = ERROR_SUCCESS;
    // The mutex is never waited on: its existence is the claim. Opening an existing one (from another process, or a
    // second claim in this one) reports ERROR_ALREADY_EXISTS and is released again at once.
    const HANDLE handle = CreateMutexW(nullptr, FALSE, name);
    const DWORD created = GetLastError();
    if (!handle) {
        if (refusal)
            *refusal = created == ERROR_ACCESS_DENIED ? Refusal::Held : Refusal::Unavailable;
        if (error)
            *error = created;
        return std::nullopt;
    }
    if (created == ERROR_ALREADY_EXISTS) {
        CloseHandle(handle);
        if (refusal)
            *refusal = Refusal::Held;
        return std::nullopt;
    }
    return SessionClaim(handle);
}

SessionClaim::SessionClaim(SessionClaim&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

SessionClaim& SessionClaim::operator=(SessionClaim&& other) noexcept {
    if (this != &other) {
        if (handle_)
            CloseHandle(handle_);
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

SessionClaim::~SessionClaim() {
    if (handle_)
        CloseHandle(handle_);
}

}  // namespace ce::runtime
