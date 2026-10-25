#include "wgc_border_access.h"

#include <windows.h>
#include <roapi.h>

#include <atomic>
#include <mutex>

#include "common/logging/logging.h"
#include "common/platform/thread_label.h"

#if __has_include(<winrt/Windows.Graphics.Capture.h>)
#define CE_WGC_BORDER_ACCESS 1
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#else
#define CE_WGC_BORDER_ACCESS 0
#endif

namespace ce::wgc {

struct BorderAccessRequest::Shared {
    Ask ask;
    std::once_flag started;
    std::atomic<int> state{static_cast<int>(BorderAccess::Pending)};
    HANDLE answered = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    ~Shared() {
        if (answered)
            CloseHandle(answered);
    }
    void Answer(BorderAccess access) {
        state.store(static_cast<int>(access), std::memory_order_release);
        if (answered)
            SetEvent(answered);
    }
};

namespace {

struct RequestStart {
    std::shared_ptr<BorderAccessRequest::Shared> shared;
    HMODULE module = nullptr;
};

// Like the engine thread, the request thread holds a reference on its own module until its last instruction, so a
// client unloading cengine.dll before Windows answered cannot unmap the code under it.
DWORD WINAPI RequestThreadMain(void* parameter) {
    ce::platform::NameCurrentThread("CE WGC border access");
    HMODULE module = nullptr;
    {
        std::unique_ptr<RequestStart> start(static_cast<RequestStart*>(parameter));
        module = start->module;
        BorderAccess access = BorderAccess::Unavailable;
        const HRESULT apartment = RoInitialize(RO_INIT_MULTITHREADED);
        if (SUCCEEDED(apartment)) {
            try {
                access = start->shared->ask();
            } catch (...) {
                access = BorderAccess::Unavailable;
            }
            RoUninitialize();
        } else {
            LogWarn("[WGC] Border access request has no apartment (hr=0x%08lX)", static_cast<unsigned long>(apartment));
        }
        start->shared->Answer(access);
    }
    FreeLibraryAndExitThread(module, 0);
}

BorderAccess AskWindows() {
#if CE_WGC_BORDER_ACCESS
    namespace capture = winrt::Windows::Graphics::Capture;
    const ULONGLONG started = GetTickCount64();
    try {
        const auto status = capture::GraphicsCaptureAccess::RequestAccessAsync(
                                capture::GraphicsCaptureAccessKind::Borderless)
                                .get();
        const BorderAccess access =
            status == winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus::Allowed
                ? BorderAccess::Allowed
                : BorderAccess::Denied;
        LogInfo("[WGC] Borderless access answered: %s (status=%d, %llu ms)", BorderAccessName(access),
                static_cast<int>(status), static_cast<unsigned long long>(GetTickCount64() - started));
        return access;
    } catch (winrt::hresult_error const& e) {
        LogInfo("[WGC] Borderless access request unavailable: 0x%08lX (%llu ms)",
                static_cast<unsigned long>(e.code().value), static_cast<unsigned long long>(GetTickCount64() - started));
    } catch (...) {
        LogInfo("[WGC] Borderless access request unavailable (%llu ms)",
                static_cast<unsigned long long>(GetTickCount64() - started));
    }
#endif
    return BorderAccess::Unavailable;
}

}  // namespace

const char* BorderAccessName(BorderAccess access) {
    switch (access) {
        case BorderAccess::Pending:
            return "pending";
        case BorderAccess::Allowed:
            return "allowed";
        case BorderAccess::Denied:
            return "denied";
        case BorderAccess::Unavailable:
            return "unavailable";
    }
    return "unknown";
}

BorderAccessRequest::BorderAccessRequest(Ask ask) : shared_(std::make_shared<Shared>()) {
    shared_->ask = std::move(ask);
}

BorderAccessRequest::~BorderAccessRequest() = default;

void BorderAccessRequest::Start() {
    std::call_once(shared_->started, [this] {
        auto start = std::make_unique<RequestStart>();
        start->shared = shared_;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&RequestThreadMain),
                                &start->module)) {
            LogWarn("[WGC] Border access request cannot pin its module (error=%lu)", GetLastError());
            shared_->Answer(BorderAccess::Unavailable);
            return;
        }
        RequestStart* payload = start.release();
        HANDLE thread = CreateThread(nullptr, 0, &RequestThreadMain, payload, 0, nullptr);
        if (!thread) {
            const DWORD error = GetLastError();
            FreeLibrary(payload->module);
            delete payload;
            LogWarn("[WGC] Border access request thread failed to start (error=%lu)", error);
            shared_->Answer(BorderAccess::Unavailable);
            return;
        }
        CloseHandle(thread);
    });
}

BorderAccess BorderAccessRequest::State() const {
    return static_cast<BorderAccess>(shared_->state.load(std::memory_order_acquire));
}

bool BorderAccessRequest::WaitAnswered(unsigned long timeoutMs) const {
    return shared_->answered && WaitForSingleObject(shared_->answered, timeoutMs) == WAIT_OBJECT_0;
}

BorderAccessRequest& SystemBorderAccess() {
    static BorderAccessRequest request(&AskWindows);
    return request;
}

}  // namespace ce::wgc
