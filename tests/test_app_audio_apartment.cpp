// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include <gtest/gtest.h>

#include "mediaengine/audio/app_audio_capture.h"
#include "mediaengine/audio/process_loopback_protocol.h"
#include "common/ipc/process_loopback_selector_transport.h"
#include "mediaengine/engine/mediaengine.h"

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

TEST(AppAudioApartmentTest, WorkerAcceptsProfileExclusionsAndStopsCleanly) {
    if (!AppAudioCapture::IsSupported())
        GTEST_SKIP() << "process loopback is unavailable";
    ASSERT_EXIT(
        {
            constexpr uint64_t generation = 41;
            const uint64_t bytes = ce::process_loopback::MappingBytes(48000, 2, 32);
            HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                                static_cast<DWORD>(bytes >> 32), static_cast<DWORD>(bytes), nullptr);
            void* view = mapping ? MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, static_cast<SIZE_T>(bytes)) : nullptr;
            auto* header = view ? ce::process_loopback::Initialize(view, generation, 48000, 2, 32) : nullptr;
            HANDLE packet = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            HANDLE stop = CreateEventW(nullptr, TRUE, TRUE, nullptr);
            const wchar_t* exclusions[] = {LR"(EAlternative\ce-apartment-test-absent.exe)"};
            int result = ERROR_INVALID_HANDLE;
            if (header && packet && stop) {
                result = MediaEngine_RunProcessLoopbackWorkerV2(
                    reinterpret_cast<uintptr_t>(mapping), reinterpret_cast<uintptr_t>(packet),
                    reinterpret_cast<uintptr_t>(stop), generation, 0, L"ce-apartment-test-absent.exe", 48000, 2,
                    SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT, 1, exclusions);
            }
            bool clean = header && result == ERROR_SUCCESS &&
                header->workerState.load() == static_cast<uint32_t>(ce::process_loopback::WorkerState::CleanExit);
            WhitelistEntry exclusion;
            exclusion.pattern = R"(Alternative\ce-apartment-test-absent.exe)";
            std::vector<uint8_t> policy;
            clean = clean && ce::process_loopback::EncodeSelectorPolicy({exclusion}, policy);
            if (clean) {
                ce::process_loopback::Initialize(view, generation, 48000, 2, 32);
                ResetEvent(packet);
                result = MediaEngine_RunProcessLoopbackWorkerV3(
                    reinterpret_cast<uintptr_t>(mapping), reinterpret_cast<uintptr_t>(packet),
                    reinterpret_cast<uintptr_t>(stop), generation, 0, L"ce-apartment-test-absent.exe", 48000, 2,
                    SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT, policy.data(), policy.size());
                clean = result == ERROR_SUCCESS && header->workerState.load() ==
                    static_cast<uint32_t>(ce::process_loopback::WorkerState::CleanExit);
            }
            if (view) UnmapViewOfFile(view);
            if (mapping) CloseHandle(mapping);
            if (packet) CloseHandle(packet);
            if (stop) CloseHandle(stop);
            std::_Exit(clean ? 0 : 1);
        },
        ::testing::ExitedWithCode(0), "");
}

}  // namespace
