#pragma once

// 2.x's interposer runs every plugin's swapchain before-hooks with no lock of its own
// (sl.interposer dxgiSwapchain.cpp): a title is expected to make its swapchain calls one at a time.
// A 1.x title need not. Witcher 3 calls SetFullscreenState(FALSE) from its window thread on alt-tab
// while its render thread presents. sl.dlss_g's SetFullscreenStatePre flushes and force-destroys the
// back-buffer wrappers that the in-flight Present hook is still using (session 20261001_153717:
// NativeBackBuffer[1] destroyed on the window thread, read 80 ms later by the render thread's present
// -> access violation in sl.dlss_g, and the game hung inside Streamline's exception handler).
//
// The serializer gives 2.x the one-call-at-a-time caller it was written for. It is re-entrant per
// thread (sl.dlss_g's slHookPresent runs its own slHookPresent1). A blocked waiter keeps dispatching
// cross-thread sent messages: DXGI may SendMessage to the window thread from inside the holder's
// Present or SetFullscreenState, and that window thread can be the one waiting here.

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace ce::streamline_bridge {

class SwapchainCallSerializer {
 public:
  SwapchainCallSerializer() : released_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
  ~SwapchainCallSerializer() {
    if (released_) {
      CloseHandle(released_);
    }
  }
  SwapchainCallSerializer(const SwapchainCallSerializer&) = delete;
  SwapchainCallSerializer& operator=(const SwapchainCallSerializer&) = delete;

  // Returns true when this call had to wait for another thread.
  bool Enter() {
    const DWORD self = GetCurrentThreadId();
    bool waited = false;
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!released_) {
          // No event (creation failed): a correct but non-pumping wait.
          waited = waited || (owner_ != 0 && owner_ != self);
          freed_.wait(lock, [&] { return owner_ == 0 || owner_ == self; });
        }
        if (owner_ == 0 || owner_ == self) {
          owner_ = self;
          ++depth_;
          return waited;
        }
      }
      waited = true;
      const DWORD result = MsgWaitForMultipleObjectsEx(1, &released_, INFINITE, QS_SENDMESSAGE, 0);
      if (result == WAIT_OBJECT_0 + 1) {
        // Delivers pending sent messages only; posted messages stay queued for the title's pump.
        MSG message{};
        PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
      }
    }
  }

  void Leave() {
    bool released = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (owner_ != GetCurrentThreadId() || depth_ == 0) {
        return;
      }
      if (--depth_ == 0) {
        owner_ = 0;
        released = true;
      }
    }
    if (!released) {
      return;
    }
    if (released_) {
      SetEvent(released_);  // auto-reset: one waiter re-checks; its own Leave wakes the next
    } else {
      freed_.notify_one();
    }
  }

  uint32_t DepthForTest() {
    std::lock_guard<std::mutex> lock(mutex_);
    return depth_;
  }

 private:
  std::mutex mutex_;
  std::condition_variable freed_;
  HANDLE released_ = nullptr;
  DWORD owner_ = 0;    // guarded by mutex_
  uint32_t depth_ = 0;  // guarded by mutex_
};

class SwapchainCallScope {
 public:
  explicit SwapchainCallScope(SwapchainCallSerializer& serializer)
      : serializer_(serializer), waited_(serializer.Enter()) {}
  ~SwapchainCallScope() { serializer_.Leave(); }
  SwapchainCallScope(const SwapchainCallScope&) = delete;
  SwapchainCallScope& operator=(const SwapchainCallScope&) = delete;
  bool Waited() const { return waited_; }

 private:
  SwapchainCallSerializer& serializer_;
  bool waited_;
};

}  // namespace ce::streamline_bridge
