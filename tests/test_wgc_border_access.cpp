#include <gtest/gtest.h>

#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <functional>

#include "captureengine/media/wgc_border_access.h"

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
    ~Event() { CloseHandle(handle); }
    void Set() const { SetEvent(handle); }
    bool Wait() const { return WaitForSingleObject(handle, kBoundMs) == WAIT_OBJECT_0; }
};

// Runs `body` on a fresh single-threaded apartment that never pumps messages, like the engine thread mid-hotkey.
// Returns the thread; the caller waits on it with a bound and closes it.
HANDLE RunOnSingleThreadedApartment(std::function<void()>* body) {
    return CreateThread(
        nullptr, 0,
        [](void* parameter) -> DWORD {
            const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            (*static_cast<std::function<void()>*>(parameter))();
            if (SUCCEEDED(apartment))
                CoUninitialize();
            return 0;
        },
        body, 0, nullptr);
}

bool Finishes(HANDLE thread) {
    return WaitForSingleObject(thread, kBoundMs) == WAIT_OBJECT_0;
}

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
    std::function<void()> body = [&] {
        request.Start();
        seen = request.State();
    };
    HANDLE thread = RunOnSingleThreadedApartment(&body);
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
    std::function<void()> body = [&] { request.Start(); };
    HANDLE thread = RunOnSingleThreadedApartment(&body);
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

// The real request, started where the screenshot path starts it: Windows answers (or reports the API unavailable)
// without the asking apartment ever pumping a message.
TEST(WgcBorderAccessTest, WindowsAnswersWithoutTheAskingApartmentPumping) {
    BorderAccessRequest& request = ce::wgc::SystemBorderAccess();
    std::function<void()> body = [&] { request.Start(); };
    HANDLE thread = RunOnSingleThreadedApartment(&body);
    ASSERT_NE(thread, nullptr);
    EXPECT_TRUE(Finishes(thread));
    CloseHandle(thread);
    ASSERT_TRUE(request.WaitAnswered(kBoundMs));
    EXPECT_NE(request.State(), BorderAccess::Pending);
}
