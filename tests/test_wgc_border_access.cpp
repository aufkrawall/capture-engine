#include <gtest/gtest.h>

#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <functional>
#include <memory>

#include "captureengine/media/wgc_border_access.h"

#if __has_include(<winrt/Windows.Foundation.h>)
#define CE_TEST_WINRT_COROUTINES 1
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#else
#define CE_TEST_WINRT_COROUTINES 0
#endif

// The WGC border-access request (user report 2026-10-24): the engine thread is a single-threaded apartment, and a
// screenshot that waited there for Windows' answer deadlocked once the answer arrived asynchronously, freezing every
// later hotkey and the shutdown. Starting the request must return at once on any apartment; the question runs on a
// thread of its own in the multithreaded apartment.

namespace {

using ce::wgc::BorderAccess;
using ce::wgc::BorderAccessRequest;

constexpr DWORD kBoundMs = 60000;  // an upper bound for a hang, never a timing expectation

struct Event {
    HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Event() = default;
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    ~Event() { CloseHandle(handle); }
    void Set() const { SetEvent(handle); }
    bool Wait() const { return WaitForSingleObject(handle, kBoundMs) == WAIT_OBJECT_0; }
};

// Runs `body` on a fresh single-threaded apartment that never pumps messages, like the engine thread mid-hotkey.
// The thread owns `body`, so one that hangs never touches a finished test's stack through it. Returns the thread;
// the caller waits on it with a bound and closes it.
HANDLE RunOnSingleThreadedApartment(std::function<void()> body) {
    auto* owned = new std::function<void()>(std::move(body));
    HANDLE thread = CreateThread(
        nullptr, 0,
        [](void* parameter) -> DWORD {
            const std::unique_ptr<std::function<void()>> run(static_cast<std::function<void()>*>(parameter));
            const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            (*run)();
            if (SUCCEEDED(apartment))
                CoUninitialize();
            return 0;
        },
        owned, 0, nullptr);
    if (!thread)
        delete owned;
    return thread;
}

bool Finishes(HANDLE thread) {
    return WaitForSingleObject(thread, kBoundMs) == WAIT_OBJECT_0;
}

// CaptureEngine always has threads in the multithreaded apartment, and the user's dump shows the completion calling
// into the engine thread from one. A test process may have none; then nothing can call into a single-threaded
// apartment at all and a wait there fails fast instead of deadlocking as it did for the user.
class ProcessMta {
public:
    ProcessMta() : ok_(SUCCEEDED(CoIncrementMTAUsage(&cookie_))) {}
    ProcessMta(const ProcessMta&) = delete;
    ProcessMta& operator=(const ProcessMta&) = delete;
    ~ProcessMta() {
        if (ok_)
            CoDecrementMTAUsage(cookie_);
    }
    bool Ok() const { return ok_; }

private:
    CO_MTA_USAGE_COOKIE cookie_ = nullptr;
    bool ok_ = false;
};

#if CE_TEST_WINRT_COROUTINES
// GraphicsCaptureAccess::RequestAccessAsync as the user dump showed it: a C++/WinRT coroutine whose inner await
// completes on a thread-pool thread of the multithreaded apartment, so C++/WinRT resumes it on the apartment that
// started it (resume_apartment -> ContextCallback). Whoever starts it on a single-threaded apartment and then waits
// there without pumping never sees it finish. Any blocking wait for a WinRT operation has this shape, not only the
// border question.
class ApartmentResumingRequest {
public:
    Event inner;    // set by the test: completes the inner await on a thread-pool thread
    Event started;  // set by the asker once the request is suspended on its inner await
    Event resumed;
    std::atomic<DWORD> resumedOn{0};
    std::atomic<int32_t> resumeFailure{0};  // C++/WinRT could not reach the asking apartment

    winrt::Windows::Foundation::IAsyncOperation<int32_t> Begin() {
        try {
            co_await CompleteOnThePool();
        } catch (winrt::hresult_error const& error) {
            resumeFailure = error.code().value;
        }
        resumedOn = GetCurrentThreadId();
        resumed.Set();
        co_return static_cast<int32_t>(BorderAccess::Allowed);
    }

private:
    winrt::Windows::Foundation::IAsyncAction CompleteOnThePool() { co_await winrt::resume_on_signal(inner.handle); }
};
#endif

}  // namespace

TEST(WgcBorderAccessTest, StartingOnASingleThreadedApartmentReturnsBeforeTheAnswer) {
    Event release;
    std::atomic<int> askApartment{-1};
    BorderAccessRequest request([&] {
        APTTYPE type{};
        APTTYPEQUALIFIER qualifier{};
        if (SUCCEEDED(CoGetApartmentType(&type, &qualifier)))
            askApartment = static_cast<int>(type);
        release.Wait();
        return BorderAccess::Allowed;
    });

    BorderAccess seen = BorderAccess::Unavailable;
    HANDLE thread = RunOnSingleThreadedApartment([&] {
        request.Start();
        seen = request.State();
    });
    ASSERT_NE(thread, nullptr);
    const bool returned = Finishes(thread);
    release.Set();  // a blocking Start would only return now
    if (!returned)
        WaitForSingleObject(thread, kBoundMs);
    CloseHandle(thread);

    EXPECT_TRUE(returned) << "Start waited for the answer on a single-threaded apartment";
    EXPECT_EQ(seen, BorderAccess::Pending);
    ASSERT_TRUE(request.WaitAnswered(kBoundMs));
    EXPECT_EQ(request.State(), BorderAccess::Allowed);
    EXPECT_EQ(askApartment.load(), static_cast<int>(APTTYPE_MTA));
}

TEST(WgcBorderAccessTest, TheQuestionIsAskedOnce) {
    std::atomic<int> asked{0};
    BorderAccessRequest request([&] {
        ++asked;
        return BorderAccess::Denied;
    });
    request.Start();
    request.Start();
    HANDLE thread = RunOnSingleThreadedApartment([&] { request.Start(); });
    ASSERT_NE(thread, nullptr);
    EXPECT_TRUE(Finishes(thread));
    CloseHandle(thread);

    ASSERT_TRUE(request.WaitAnswered(kBoundMs));
    EXPECT_EQ(request.State(), BorderAccess::Denied);
    EXPECT_EQ(asked.load(), 1);
}

TEST(WgcBorderAccessTest, NothingIsAskedBeforeStart) {
    std::atomic<int> asked{0};
    BorderAccessRequest request([&] {
        ++asked;
        return BorderAccess::Allowed;
    });
    EXPECT_EQ(request.State(), BorderAccess::Pending);
    EXPECT_FALSE(request.WaitAnswered(0));
    EXPECT_EQ(asked.load(), 0);
}

TEST(WgcBorderAccessTest, AQuestionThatThrowsAnswersUnavailable) {
    BorderAccessRequest request([]() -> BorderAccess { throw 1; });
    request.Start();
    ASSERT_TRUE(request.WaitAnswered(kBoundMs));
    EXPECT_EQ(request.State(), BorderAccess::Unavailable);
}

TEST(WgcBorderAccessTest, AnswerNamesAreStable) {
    EXPECT_STREQ(ce::wgc::BorderAccessName(BorderAccess::Pending), "pending");
    EXPECT_STREQ(ce::wgc::BorderAccessName(BorderAccess::Allowed), "allowed");
    EXPECT_STREQ(ce::wgc::BorderAccessName(BorderAccess::Denied), "denied");
    EXPECT_STREQ(ce::wgc::BorderAccessName(BorderAccess::Unavailable), "unavailable");
}

#if CE_TEST_WINRT_COROUTINES
// The model is only worth its name if it re-enters the asking apartment: started on a single-threaded apartment that
// pumps while it waits, its continuation runs on that very thread. Without this the test below could pass vacuously.
TEST(WgcBorderAccessTest, TheModelResumesOnTheApartmentThatStartedIt) {
    struct Seen {
        std::atomic<DWORD> asker{0};
        std::atomic<bool> pumped{false};
        std::atomic<int> result{-1};
    };
    const ProcessMta mta;
    ASSERT_TRUE(mta.Ok());
    auto fake = std::make_shared<ApartmentResumingRequest>();
    auto seen = std::make_shared<Seen>();
    HANDLE thread = RunOnSingleThreadedApartment([fake, seen] {
        seen->asker = GetCurrentThreadId();
        const auto operation = fake->Begin();
        fake->inner.Set();
        HANDLE resumed = fake->resumed.handle;
        DWORD index = 0;
        // Dispatches the cross-apartment call that carries the continuation.
        seen->pumped = SUCCEEDED(CoWaitForMultipleHandles(0, kBoundMs, 1, &resumed, &index));
        if (operation.Status() == winrt::Windows::Foundation::AsyncStatus::Completed)
            seen->result = operation.GetResults();
    });
    ASSERT_NE(thread, nullptr);
    const bool finished = Finishes(thread);
    CloseHandle(thread);
    ASSERT_TRUE(finished);

    EXPECT_TRUE(seen->pumped.load());
    EXPECT_EQ(fake->resumeFailure.load(), 0) << "C++/WinRT could not call into the asking apartment";
    EXPECT_EQ(fake->resumedOn.load(), seen->asker.load()) << "the model resumed away from the asking apartment";
    EXPECT_EQ(seen->result.load(), static_cast<int>(BorderAccess::Allowed));
}

// The engine thread's situation, modelled: a single-threaded apartment starts the request and then blocks without
// pumping until the answer is there. The answer must arrive although the request resumes on whichever apartment
// asked: asking or waiting on the caller's apartment deadlocks here, as it did in the user's dump.
TEST(WgcBorderAccessTest, AnAnswerResumingOnTheAskingApartmentReachesAStaWaitingWithoutPumping) {
    const ProcessMta mta;
    ASSERT_TRUE(mta.Ok());
    auto fake = std::make_shared<ApartmentResumingRequest>();
    auto request = std::make_shared<BorderAccessRequest>([fake] {
        const auto operation = fake->Begin();
        fake->started.Set();
        return static_cast<BorderAccess>(operation.get());
    });
    auto answered = std::make_shared<std::atomic<bool>>(false);
    HANDLE thread = RunOnSingleThreadedApartment([request, answered] {
        request->Start();
        *answered = request->WaitAnswered(kBoundMs);  // blocks this apartment without pumping
    });
    ASSERT_NE(thread, nullptr);
    ASSERT_TRUE(fake->started.Wait());
    // From a third thread, so only the apartment the request resumes on decides whether it finishes.
    fake->inner.Set();
    const bool finished = Finishes(thread);
    CloseHandle(thread);

    ASSERT_TRUE(finished) << "the answer was routed to the single-threaded apartment that waits for it";
    EXPECT_TRUE(answered->load());
    EXPECT_EQ(request->State(), BorderAccess::Allowed);
    EXPECT_EQ(fake->resumeFailure.load(), 0);
    EXPECT_NE(fake->resumedOn.load(), 0u);
}
#endif

// The real request, started where the screenshot path starts it, on a single-threaded apartment that then blocks
// without pumping as the engine thread did: Windows' answer (or its report that the API is unavailable) arrives all
// the same. Only a smoke test: where Windows answers without calling back into the asker it passes with the old
// blocking wait too (the maintainer's machine, 2026-10-25); the model above is the regression test.
TEST(WgcBorderAccessTest, WindowsAnswersWhileTheAskingApartmentWaitsWithoutPumping) {
    const ProcessMta mta;
    ASSERT_TRUE(mta.Ok());
    BorderAccessRequest& request = ce::wgc::SystemBorderAccess();
    auto answered = std::make_shared<std::atomic<bool>>(false);
    HANDLE thread = RunOnSingleThreadedApartment([&request, answered] {
        request.Start();
        *answered = request.WaitAnswered(kBoundMs);
    });
    ASSERT_NE(thread, nullptr);
    const bool finished = Finishes(thread);
    CloseHandle(thread);

    ASSERT_TRUE(finished);
    EXPECT_TRUE(answered->load());
    EXPECT_NE(request.State(), BorderAccess::Pending);
}
