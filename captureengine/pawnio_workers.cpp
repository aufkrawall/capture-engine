#include "pawnio_workers.h"
#include "../common/logging.h"
#include <windows.h>
#include <atomic>
#include <mutex>
#include <thread>

namespace ce::pawnio {
namespace {
std::mutex mutex;
std::thread worker;
std::atomic<bool> running{false};
std::atomic<bool> closing{false};
std::atomic<DWORD> threadId{0};

LRESULT CALLBACK DialogCreation(int code, WPARAM window, LPARAM parameters) {
    if (code == HCBT_CREATEWND && closing.load())
        PostMessageW(reinterpret_cast<HWND>(window), WM_CLOSE, 0, 0);
    return CallNextHookEx(nullptr, code, window, parameters);
}
}  // namespace
bool SetupShuttingDown() {
    return closing.load();
}

void LaunchSetupWorker(std::function<void()> task) {
    std::lock_guard<std::mutex> guard(mutex);
    if (closing.load() || running.exchange(true))
        return;
    if (worker.joinable())
        worker.join();
    try {
        worker = std::thread([task = std::move(task)] {
            threadId.store(GetCurrentThreadId());
            const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            HHOOK hook = SetWindowsHookExW(WH_CBT, DialogCreation, nullptr, GetCurrentThreadId());
            try {
                if (!hook)
                    LogError("[PawnIO] Cannot protect setup dialog shutdown (error=%lu)", GetLastError());
                else if (!closing.load())
                    task();
            } catch (...) {
                LogError("[PawnIO] Setup worker failed; owned resources released");
            }
            if (hook)
                UnhookWindowsHookEx(hook);
            if (SUCCEEDED(apartment))
                CoUninitialize();
            threadId.store(0);
            running.store(false);
        });
    } catch (...) {
        running.store(false);
        LogError("[PawnIO] Cannot create the setup worker");
    }
}

void ShutdownSetupWorkers() {
    std::thread owned;
    {
        std::lock_guard<std::mutex> guard(mutex);
        closing.store(true);
        const DWORD id = threadId.load();
        if (id)
            EnumThreadWindows(
                id,
                [](HWND window, LPARAM) -> BOOL {
                    PostMessageW(window, WM_CLOSE, 0, 0);
                    return TRUE;
                },
                0);
        owned = std::move(worker);
    }
    if (owned.joinable())
        owned.join();
}
}  // namespace ce::pawnio
