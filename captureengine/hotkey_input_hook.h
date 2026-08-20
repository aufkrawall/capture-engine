#pragma once

#include <windows.h>

#include "../common/config.h"

#include "../common/hotkey_matcher.h"

// Delivery of the configured global hotkeys for foreground applications that
// suppress application hotkeys.
//
// RegisterHotKey alone is not sufficient: a process that registers its
// raw-input keyboard with RIDEV_NOHOTKEYS turns off application-defined hotkey
// processing for as long as its window is foreground, so WM_HOTKEY is never
// generated for anyone. DOOM Eternal does exactly that (usage page 1 / usage 6,
// dwFlags=0x200), which is why recording, screenshot and overlay-toggle presses
// were lost there while the same hotkeys worked in other titles.
//
// A low-level keyboard hook runs ahead of that suppression. It consumes the
// keystroke on a match, which is both the RegisterHotKey behaviour and the
// reason the two paths can never both fire for one press: a consumed key
// produces no WM_HOTKEY at all. The hook lives on its own thread that does
// nothing but pump messages, so a busy controller thread can never stall the
// system input queue.

// Registers one configured hotkey with the system and reports whether the
// controller owns it. A combination another application registered first stays
// with that application: the keyboard path below only ever serves hotkeys this
// returned true for, so nothing is taken away from anyone.
bool RegisterConfiguredHotkey(int hotkeyId, const AppConfig::HotkeyConfig& hotkey, const char* name);

// Publishes the hotkeys the hook recognizes. Safe to call before Start and on
// every config reload.
void PublishHotkeyBindings(const AppConfig& config, const HotkeyOwnership& ownership);

// Starts the hook thread and routes matches to targetThreadId as
// main_kMsgHotkeyFromInputHook. Returns false when the hook cannot be
// installed; RegisterHotKey then remains the only delivery path, exactly as
// before this hook existed.
bool StartHotkeyInputHook(DWORD targetThreadId);

// Stops the hook thread and uninstalls the hook. Idempotent.
void StopHotkeyInputHook();

// True while the hook is installed.
bool IsHotkeyInputHookActive();
