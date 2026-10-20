#include "pawnio_workers.h"
#include "common/logging/logging.h"
#include "pawnio_setup_core.h"
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
// Silent requests. Admission is shared with the interactive thread: both are decided under `mutex`.
ce::startup::SetupWorker silentRequest;

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
    const Admission admission = AdmitRequest(closing.load(), running.load(), silentRequest.Busy());
    if (admission == Admission::Busy)
        LogInfo("[PawnIO] Setup request ignored: another PawnIO request is in flight");
    if (admission != Admission::Start)
        return;
    running.store(true);
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

unsigned long StartSilentSetupRequest(std::function<unsigned long()> work) {
    std::lock_guard<std::mutex> guard(mutex);
    return StartSilentRequest(silentRequest, closing.load(), running.load(), std::move(work));
}

// No lock here: collecting joins the request thread, and holding `mutex` across that join would stall the tray. The
// SetupWorker stays busy until the result is collected, so no start can overlap the join.
std::optional<unsigned long> TakeSilentSetupResult() {
    return silentRequest.TakeResult();
}

std::optional<unsigned long> WaitSilentSetupResult() {
    return silentRequest.WaitForResult();
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
    silentRequest.Shutdown();
}
}  // namespace ce::pawnio
