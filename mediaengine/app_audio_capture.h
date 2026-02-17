#pragma once

#include <windows.h>
#include <atomic>
#include <audioclient.h>
#include <chrono>
#include <deque>
#include <mmdeviceapi.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Include for AudioPacket definition
#include "audio_capture.h"

/**
 * Per-application audio capture using Windows 10/11 process loopback API.
 *
 * Captures audio from a specific process using ActivateAudioInterfaceAsync
 * with AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK.
 *
 * Features:
 * - Capture by process ID or process name
 * - Auto-detect when target process starts (for name-based capture)
 * - Works with audio that started before capture began
 * - Graceful handling when target process exits
 *
 * Requirements: Windows 10 build 20348+ or Windows 11
 */
class AppAudioCapture {
public:
  AppAudioCapture();
  ~AppAudioCapture();

  // Non-copyable
  AppAudioCapture(const AppAudioCapture &) = delete;
  AppAudioCapture &operator=(const AppAudioCapture &) = delete;

  /**
   * Start capturing audio from a specific process by PID.
   * @param processId Target process ID
   * @return true if capture started successfully
   */
  bool StartByPID(DWORD processId);

  /**
   * Start capturing audio from a process by name.
   * Will monitor for the process and start capture when found.
   * @param processName Process name (e.g., "chrome.exe")
   * @return true if monitoring started (capture may start later)
   */
  bool StartByName(const std::string &processName);

  /**
   * Stop capturing and monitoring.
   */
  void Stop();

  /**
   * Get next available audio packet.
   * @param packet Output packet (data copied)
   * @return true if packet was available
   */
  bool GetNextPacket(AudioPacket &packet);

  /**
   * Check if currently capturing audio.
   */
  bool IsCapturing() const { return isCapturing.load(); }

  /**
   * Check if monitoring for a process (name-based mode).
   */
  bool IsMonitoring() const { return isMonitoring.load(); }

  /**
   * Get the current target process ID (0 if not capturing).
   */
  DWORD GetTargetPID() const { return targetPID.load(); }

  /**
   * Get the target process name (empty if PID-based).
   */
  const std::string &GetTargetProcessName() const { return targetProcessName; }

  /**
   * Check if per-process audio loopback is supported on this Windows version.
   * @return true if Windows 10 build 20348+ or Windows 11
   */
  static bool IsSupported();

private:
  // Internal activation handler (implements
  // IActivateAudioInterfaceCompletionHandler)
  class ActivationHandler;

  // Initialize capture for the given PID (called internally)
  bool InitializeCaptureForPID(DWORD pid);

  // Cleanup capture resources
  void CleanupCapture();

  // Audio capture loop (runs in captureThread)
  void CaptureLoop();

  // Process monitoring loop (runs in monitorThread for name-based capture)
  void ProcessMonitorLoop();

  // Find PID by process name
  static DWORD FindProcessByName(const std::string &name);

  // Check if a process is still running
  static bool IsProcessRunning(DWORD pid);

  // Audio client and capture interfaces
  IAudioClient *pAudioClient = nullptr;
  IAudioCaptureClient *pCaptureClient = nullptr;
  WAVEFORMATEX *pwfx = nullptr;

  // Target process info
  std::atomic<DWORD> targetPID{0};
  std::string targetProcessName;

  // State flags
  std::atomic<bool> isCapturing{false};
  std::atomic<bool> isMonitoring{false};
  std::atomic<bool> shouldStop{false};

  // Threads
  std::thread captureThread;
  std::thread monitorThread;

  // Packet queue
  std::mutex queueMutex;
  std::deque<AudioPacket> packetQueue;

  // Event for async activation completion
  HANDLE activationCompleteEvent = nullptr;
  HRESULT activationResult = 0x80004005; // E_FAIL

  // Per-instance silence synthesis timing (NOT static - each instance needs
  // independent timing)
  std::chrono::steady_clock::time_point m_lastRealTime{};
  int64_t m_synthesizedMs = 0;
  bool m_heartbeatInit = false;
};
