// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include <gtest/gtest.h>

#include "mediaengine/audio/app_audio_capture.h"

#include <combaseapi.h>

#include <cstdlib>
#include <thread>

namespace {

// An untouched thread joins the implicit MTA only while the process has an MTA owner.
bool HasProcessMta() {
    bool alive = false;
    std::thread observer([&] {
        APTTYPE type = APTTYPE_CURRENT;
        APTTYPEQUALIFIER qualifier = APTTYPEQUALIFIER_NONE;
        alive = SUCCEEDED(CoGetApartmentType(&type, &qualifier)) && type == APTTYPE_MTA;
    });
    observer.join();
    return alive;
}

constexpr char kAbsentProcess[] = "ce-apartment-test-absent.exe";

// Fresh subprocesses keep unrelated tests' COM apartments from masking the missing owner.
TEST(AppAudioApartmentTest, MonitoringRetainsMtaBeforeAnyClientExists) {
    ASSERT_EXIT(
        {
            AppAudioCapture capture;
            const bool ok = capture.StartByName(kAbsentProcess) && HasProcessMta();
            capture.Stop();
            std::_Exit(ok ? 0 : 1);
        },
        ::testing::ExitedWithCode(0), "");
}

TEST(AppAudioApartmentTest, CaptureRetainsMtaAfterStartingStaThreadLeaves) {
    ASSERT_EXIT(
        {
            AppAudioCapture capture;
            bool started = false;
            std::thread starter([&] {
                const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                if (FAILED(initialized))
                    return;
                started = capture.StartByName(kAbsentProcess);
                APTTYPE type = APTTYPE_CURRENT;
                APTTYPEQUALIFIER qualifier = APTTYPEQUALIFIER_NONE;
                started = started && SUCCEEDED(CoGetApartmentType(&type, &qualifier)) &&
                          (type == APTTYPE_STA || type == APTTYPE_MAINSTA);
                CoUninitialize();
            });
            starter.join();
            const bool ok = started && HasProcessMta();
            std::thread stopper([&] { capture.Stop(); });
            stopper.join();
            std::_Exit(ok ? 0 : 1);
        },
        ::testing::ExitedWithCode(0), "");
}

TEST(AppAudioApartmentTest, StopAndRestartRetainMtaForEachCaptureLifetime) {
    ASSERT_EXIT(
        {
            AppAudioCapture capture;
            bool ok = true;
            for (int cycle = 0; cycle < 3; ++cycle) {
                ok = capture.StartByName(kAbsentProcess) && HasProcessMta() && ok;
                capture.Stop();
                capture.Stop();
            }
            std::_Exit(ok ? 0 : 1);
        },
        ::testing::ExitedWithCode(0), "");
}

}  // namespace
