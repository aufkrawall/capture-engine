#pragma once
#include <atomic>
#include <string>
#include <windows.h>

class NVNGXHook {
public:
  static NVNGXHook &Get() {
    static NVNGXHook instance;
    return instance;
  }

  void Install();
  void Uninstall();

  bool IsInstalled() const { return m_Installed; }

private:
  NVNGXHook() = default;
  ~NVNGXHook() = default;

  std::atomic<bool> m_Installed{false};
};
