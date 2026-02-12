#include "input_manager.h"
#include "hook_common.h" // For Logging

InputManager &InputManager::Get() {
  static InputManager instance;
  return instance;
}

InputManager::~InputManager() { Shutdown(); }

void InputManager::HookWindow(HWND hwnd) {
  if (!hwnd || !IsWindow(hwnd))
    return;

  std::lock_guard<std::mutex> lock(m_Mutex);

  if (m_Hooks.find(hwnd) != m_Hooks.end()) {
    // Already hooked or tracked
    return;
  }

  // Attempt to hook
  // Note: SetWindowLongPtrA/W depends on window encoding.
  // GWLP_WNDPROC handles both but we should be careful.
  // Since we are injected, we are in-process.

  WNDPROC original = (WNDPROC)GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
  if (!original) {
    // Try ANSI
    original = (WNDPROC)GetWindowLongPtrA(hwnd, GWLP_WNDPROC);
  }

  if (!original) {
    HookLog("InputManager: Failed to get original WndProc for hwnd %p", hwnd);
    return;
  }

  if (original == HookWndProc) {
    HookLog("InputManager: Already hooked by us?");
    return;
  }

  // Install hook
  SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)HookWndProc);

  // Store
  m_Hooks[hwnd] = {original, true};
  HookLog("InputManager: Hooked WndProc for hwnd %p (Original: %p)", hwnd,
          original);
}

void InputManager::UnhookWindow(HWND hwnd) {
  // Write operation - need exclusive lock
  std::lock_guard<std::mutex> lock(m_Mutex);

  auto it = m_Hooks.find(hwnd);
  if (it != m_Hooks.end()) {
    if (it->second.hooked && IsWindow(hwnd)) {
      // Only restore if current proc is still us
      WNDPROC current = (WNDPROC)GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
      if (current == HookWndProc) {
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                          (LONG_PTR)it->second.originalProc);
        HookLog("InputManager: Unhooked WndProc for hwnd %p", hwnd);
      } else {
        HookLog("InputManager: Skip unhook, WndProc changed externally "
                "(Current: %p, Us: %p)",
                current, HookWndProc);
      }
    }
    m_Hooks.erase(it);
  }
}

void InputManager::Shutdown() {
  // Write operation - need exclusive lock
  std::lock_guard<std::mutex> lock(m_Mutex);
  for (auto &pair : m_Hooks) {
    HWND hwnd = pair.first;
    if (IsWindow(hwnd) && pair.second.hooked) {
      WNDPROC current = (WNDPROC)GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
      if (current == HookWndProc) {
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                          (LONG_PTR)pair.second.originalProc);
      }
    }
  }
  m_Hooks.clear();
}

LRESULT CALLBACK InputManager::HookWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                           LPARAM lParam) {
  // 1. Pass to ImGui (only if context exists)
  // Filter out mouse messages so overlay never reacts to hardware cursor
  bool isMouseMsg = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
                    (msg >= WM_NCMOUSEMOVE && msg <= WM_NCMBUTTONDBLCLK) ||
                    (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL);

  (void)isMouseMsg; // Suppress unused warning

  // 2. Call Original
  WNDPROC original = nullptr;
  {
    // Note: Using unique_lock for read access
    // In C++17+ with shared_mutex, this would be shared_lock for concurrent
    // reads
    auto &mgr = InputManager::Get();
    std::unique_lock<std::mutex> lock(mgr.m_Mutex);
    auto it = mgr.m_Hooks.find(hwnd);
    if (it != mgr.m_Hooks.end()) {
      original = it->second.originalProc;
    }
  }

  if (original) {
    return CallWindowProcW(original, hwnd, msg, wParam, lParam);
  }

  return DefWindowProcW(hwnd, msg, wParam, lParam);
}
