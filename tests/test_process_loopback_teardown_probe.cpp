// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
// Process-loopback teardown probe (libcengine 11, part 5). Not a regression test: every test here is DISABLED_ and
// runs only on request, because it plays a silent stream through the default render device and tears process-loopback
// clients down in the ways CaptureEngine did over time, to find which order crashes AudioSes' CLoopbackMixer:
//
//   unit_tests.exe --gtest_also_run_disabled_tests --gtest_filter=DISABLED_ProcessLoopbackProbe.Orchestrate
//
// The orchestrator starts a silent renderer child and, per teardown variant, a capture child that runs the cycles; a
// crash ends only that child and shows as its exit code. Children are this same executable with another filter, in
// a kill-on-close job. Output: one "[ProcessLoopbackProbe]" line per variant plus the Windows build.
#include <gtest/gtest.h>

#include "mediaengine/audio/app_audio_capture.h"
#include "mediaengine/audio/process_loopback_protocol.h"
#include "mediaengine/engine/mediaengine.h"
#include "common/platform/raii_helpers.h"

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace probe {
// The SDK's process-loopback activation types (audioclientactivationparams.h), declared here because the MinGW headers
// lack them; layout as documented.
enum ActivationType { kDefault = 0, kProcessLoopback = 1 };
enum LoopbackMode { kIncludeTargetProcessTree = 0 };
struct LoopbackParams {
    DWORD targetProcessId;
    LoopbackMode mode;
};
struct ActivationParams {
    ActivationType type;
    LoopbackParams loopback;
};
constexpr wchar_t kVirtualProcessLoopback[] = L"VAD\\Process_Loopback";
// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
constexpr GUID kIeeeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
}  // namespace probe

constexpr DWORD kChildTimeoutMs = 180000;

std::wstring Env(const wchar_t* name) {
    wchar_t buffer[256]{};
    const DWORD length = GetEnvironmentVariableW(name, buffer, 256);
    return length > 0 && length < 256 ? std::wstring(buffer, length) : std::wstring();
}

class Handler final : public IActivateAudioInterfaceCompletionHandler {
public:
    Handler() : done_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~Handler() {
        CloseHandle(done_);
    }
    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler) ||
            riid == IID_IAgileObject) {
            *object = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG count = InterlockedDecrement(&refs_);
        if (count == 0)
            delete this;
        return count;
    }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        HRESULT activate = E_FAIL;
        IUnknown* unknown = nullptr;
        if (SUCCEEDED(operation->GetActivateResult(&activate, &unknown)) && SUCCEEDED(activate) && unknown)
            unknown->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(&client_));
        if (unknown)
            unknown->Release();
        SetEvent(done_);
        return S_OK;
    }
    HANDLE Done() const { return done_; }
    IAudioClient* Take() {
        IAudioClient* client = client_;
        client_ = nullptr;
        return client;
    }

private:
    LONG refs_ = 1;
    HANDLE done_;
    IAudioClient* client_ = nullptr;
};

struct LoopbackClient {
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
};

// Activates, initializes (polling, AUTOCONVERTPCM, float stereo 48 kHz: CaptureEngine's first mode) and starts.
bool OpenClient(DWORD pid, LoopbackClient* out) {
    probe::ActivationParams params{probe::kProcessLoopback, {pid, probe::kIncludeTargetProcessTree}};
    PROPVARIANT variant{};
    variant.vt = VT_BLOB;
    variant.blob.cbSize = sizeof(params);
    variant.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
    Handler* handler = new Handler();
    IActivateAudioInterfaceAsyncOperation* operation = nullptr;
    const HRESULT hr = ActivateAudioInterfaceAsync(probe::kVirtualProcessLoopback, __uuidof(IAudioClient), &variant,
                                                   handler, &operation);
    bool ok = SUCCEEDED(hr) && WaitForSingleObject(handler->Done(), 5000) == WAIT_OBJECT_0;
    out->client = ok ? handler->Take() : nullptr;
    handler->Release();
    if (operation)
        operation->Release();
    if (!out->client)
        return false;
    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = 2;
    format.Format.nSamplesPerSec = 48000;
    format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign = 8;
    format.Format.nAvgBytesPerSec = 48000 * 8;
    format.Format.cbSize = sizeof(format) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    format.SubFormat = probe::kIeeeFloat;
    ok = SUCCEEDED(out->client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                           AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, 2000000,
                                           0, &format.Format, nullptr)) &&
         SUCCEEDED(out->client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&out->capture))) &&
         SUCCEEDED(out->client->Start());
    return ok;
}

// Reads what arrives for about `ms` milliseconds, as the capture loop does.
void Drain(LoopbackClient& client, DWORD ms) {
    HANDLE never = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    for (DWORD waited = 0; waited < ms; waited += 10) {
        UINT32 next = 0;
        while (SUCCEEDED(client.capture->GetNextPacketSize(&next)) && next > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(client.capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
                break;
            client.capture->ReleaseBuffer(frames);
        }
        WaitForSingleObject(never, 10);  // the probe paces itself; there is nothing to wait for
    }
    CloseHandle(never);
}

void StopAndRelease(LoopbackClient& client) {
    client.client->Stop();
    client.capture->Release();
    client.client->Release();
}

void ReleaseRunning(LoopbackClient& client) {
    client.capture->Release();
    client.client->Release();
}

int g_notStarted = 0;  // product cycles that did not deliver audio before the bounded wait

// CaptureEngine's own capture class (the worker's): start, capture briefly, stop (Stop -> Release since M24).
bool RunCaptureEngineCycle(AppAudioCapture& capture, DWORD pid) {
    capture.SetRequestedFormat(48000, 2, SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    if (!capture.StartByPID(pid))
        return false;
    bool captured = false;
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (!captured && GetTickCount64() < deadline) {
        AudioPacket packet;
        while (capture.GetNextPacket(packet)) {
            captured = captured || (packet.recordType == AudioPacketRecordType::Data && !packet.data.empty());
        }
        if (capture.IsWorkerRecycleRequested())
            break;
        const ULONGLONG now = GetTickCount64();
        if (!captured && now < deadline &&
            WaitForSingleObject(capture.GetPacketReadyEvent(), static_cast<DWORD>(deadline - now)) != WAIT_OBJECT_0)
            break;
    }
    capture.Stop(false);
    return captured;
}

// Exercise the exported worker entry on this child's uninitialized main thread, including IPC and clean-exit state.
bool RunWorkerCycle(DWORD pid) {
    const uint64_t bytes = ce::process_loopback::MappingBytes(48000, 2, 32);
    ce::HandleGuard mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                               static_cast<DWORD>(bytes), nullptr));
    ce::HandleGuard packets(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    ce::HandleGuard stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!mapping || !packets || !stop)
        return false;
    void* view = MapViewOfFile(mapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, static_cast<SIZE_T>(bytes));
    if (!view)
        return false;
    CE_SCOPE_EXIT(UnmapViewOfFile(view));
    auto* header = ce::process_loopback::Initialize(view, 1);
    if (!header)
        return false;
    bool captured = false;
    std::thread consumer([&] {
        ce::process_loopback::ConsumerState state;
        const ULONGLONG deadline = GetTickCount64() + 5000;
        while (!captured && GetTickCount64() < deadline) {
            AudioPacket packet;
            while (ce::process_loopback::ReadPacket(view, state, packet))
                captured = captured || (packet.recordType == AudioPacketRecordType::Data && !packet.data.empty());
            const auto status = static_cast<ce::process_loopback::WorkerState>(header->workerState.load());
            if (status == ce::process_loopback::WorkerState::CleanExit ||
                status == ce::process_loopback::WorkerState::Failed)
                break;
            const ULONGLONG now = GetTickCount64();
            if (!captured && now < deadline)
                WaitForSingleObject(packets.get(), static_cast<DWORD>(deadline - now));
        }
        SetEvent(stop.get());
    });
    const int result = MediaEngine_RunProcessLoopbackWorker(
        reinterpret_cast<uintptr_t>(mapping.get()), reinterpret_cast<uintptr_t>(packets.get()),
        reinterpret_cast<uintptr_t>(stop.get()), 1, pid, nullptr, 48000, 2, SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    consumer.join();
    const bool clean = result == ERROR_SUCCESS && header->workerCleanExitCount.load() == 1 &&
                       header->workerState.load() == static_cast<uint32_t>(ce::process_loopback::WorkerState::CleanExit);
    const bool sessionUnavailable = !captured && result == ERROR_OPEN_FAILED && Env(L"CE_PROBE_CHURN") == L"1";
    EXPECT_TRUE(clean || sessionUnavailable) << "worker result=" << result << " state=" << header->workerState.load();
    return captured && clean;
}

// One teardown variant, `cycles` times. Returns the number of completed cycles.
int RunVariant(const std::wstring& variant, DWORD pid, int cycles) {
    int completed = 0;
    AppAudioCapture reusedCapture;
    for (int cycle = 0; cycle < cycles; ++cycle) {
        if (variant == L"captureengine_worker") {
            if (!RunWorkerCycle(pid)) {
                ++g_notStarted;
                if (Env(L"CE_PROBE_CHURN") != L"1")
                    return completed;
            }
        } else if (variant == L"captureengine_capture" || variant == L"captureengine_reuse" ||
                   variant == L"captureengine_cancel") {
            AppAudioCapture capture;
            if (variant == L"captureengine_cancel") {
                if (!capture.StartByPID(pid))
                    return completed;
                capture.Stop();  // cancellation is a lifecycle boundary, independent of activation's timing
            }
            if (!RunCaptureEngineCycle(variant == L"captureengine_reuse" ? reusedCapture : capture, pid)) {
                ++g_notStarted;
                if (Env(L"CE_PROBE_CHURN") != L"1")
                    return completed;
            }
        } else if (variant == L"cross_thread_release") {
            // Activated, started and drained on a thread whose apartment ends with it, released on another (CaptureEngine
            // before July 2026).
            LoopbackClient client;
            bool opened = false;
            std::thread worker([&] {
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                opened = OpenClient(pid, &client);
                if (opened)
                    Drain(client, 40);
                CoUninitialize();
            });
            worker.join();
            if (!opened)
                return completed;
            ReleaseRunning(client);
        } else if (variant == L"duplicate_release" || variant == L"duplicate_stop_release") {
            LoopbackClient first;
            LoopbackClient second;
            if (!OpenClient(pid, &first) || !OpenClient(pid, &second))
                return completed;
            Drain(first, 20);
            Drain(second, 20);
            if (variant == L"duplicate_release") {
                ReleaseRunning(second);
                ReleaseRunning(first);
            } else {
                StopAndRelease(second);
                StopAndRelease(first);
            }
        } else {
            LoopbackClient client;
            if (!OpenClient(pid, &client))
                return completed;
            Drain(client, 40);
            if (variant == L"stop_release")
                StopAndRelease(client);
            else if (variant == L"release_only")
                ReleaseRunning(client);
            // "abandon": historical behavior, bounded by the probe child process
        }
        ++completed;
    }
    return completed;
}

// One shared-mode render stream on the default device. CE_PROBE_SIGNAL=1 writes a -100 dBFS tone instead of buffers
// flagged silent (inaudible, but processed by the endpoint's effects such as the limiter APO); CE_PROBE_CHURN=1
// closes and reopens the stream every 100 ms, so the target's audio session keeps ending and starting.
struct RenderStream {
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    WAVEFORMATEX* mix = nullptr;
    UINT32 bufferFrames = 0;
    bool Open(IMMDevice* device, HANDLE period) {
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) ||
            FAILED(client->GetMixFormat(&mix)) ||
            FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200000, 0, mix,
                                      nullptr)) ||
            FAILED(client->SetEventHandle(period)) ||
            FAILED(client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render))))
            return false;
        client->GetBufferSize(&bufferFrames);
        return SUCCEEDED(client->Start());
    }
    void Close() {
        if (client)
            client->Stop();
        if (render)
            render->Release();
        if (client)
            client->Release();
        if (mix)
            CoTaskMemFree(mix);
        *this = RenderStream{};
    }
    // Fills the free part of the buffer with silence or with a -100 dBFS tone (float mix formats).
    void Feed(bool signal, uint64_t* phase) {
        UINT32 padding = 0;
        client->GetCurrentPadding(&padding);
        BYTE* data = nullptr;
        const UINT32 frames = bufferFrames > padding ? bufferFrames - padding : 0;
        if (frames == 0 || FAILED(render->GetBuffer(frames, &data)))
            return;
        const bool isFloat = mix->wBitsPerSample == 32;
        if (!signal || !isFloat) {
            render->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
            return;
        }
        float* samples = reinterpret_cast<float*>(data);
        for (UINT32 frame = 0; frame < frames; ++frame, ++*phase) {
            const float value = ((*phase / 24) % 2 == 0) ? 1.0e-5f : -1.0e-5f;  // ~1 kHz square, -100 dBFS
            for (WORD channel = 0; channel < mix->nChannels; ++channel)
                samples[frame * mix->nChannels + channel] = value;
        }
        render->ReleaseBuffer(frames, 0);
    }
};

TEST(DISABLED_ProcessLoopbackProbe, Renderer) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ASSERT_TRUE(SUCCEEDED(initialized));
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    HANDLE ready = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wtoi64(Env(L"CE_PROBE_READY").c_str())));
    HANDLE stop = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wtoi64(Env(L"CE_PROBE_STOP").c_str())));
    const bool signal = Env(L"CE_PROBE_SIGNAL") == L"1";
    const bool churn = Env(L"CE_PROBE_CHURN") == L"1";
    HANDLE period = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ASSERT_TRUE(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                           __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator))));
    ASSERT_TRUE(SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)));
    RenderStream stream;
    ASSERT_TRUE(stream.Open(device, period));
    if (ready)
        SetEvent(ready);
    HANDLE waits[] = {stop, period};
    uint64_t phase = 0;
    ULONGLONG reopenAt = GetTickCount64() + 100;
    for (;;) {
        const DWORD result = WaitForMultipleObjects(stop ? 2 : 1, stop ? waits : waits + 1, FALSE, 2000);
        if (stop && result == WAIT_OBJECT_0)
            break;
        if (churn && GetTickCount64() >= reopenAt) {
            stream.Close();
            if (!stream.Open(device, period))
                break;
            reopenAt = GetTickCount64() + 100;
        }
        stream.Feed(signal, &phase);
    }
    stream.Close();
    device->Release();
    enumerator->Release();
    CloseHandle(period);
    CoUninitialize();
}

TEST(DISABLED_ProcessLoopbackProbe, Capture) {
    const DWORD pid = static_cast<DWORD>(_wtoi(Env(L"CE_PROBE_TARGET_PID").c_str()));
    const std::wstring variant = Env(L"CE_PROBE_VARIANT");
    const int cycles = _wtoi(Env(L"CE_PROBE_CYCLES").c_str());
    ASSERT_NE(pid, 0u);
    ASSERT_GT(cycles, 0);
    // Product variants must run without an ambient MTA; otherwise this test masks apartment teardown bugs.
    const bool product = variant.rfind(L"captureengine_", 0) == 0;
    const HRESULT initialized = product ? S_FALSE : CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ASSERT_TRUE(SUCCEEDED(initialized));
    const int completed = RunVariant(variant, pid, cycles);
    printf("[ProcessLoopbackProbe] child variant=%ls completed=%d of %d not_started=%d\n", variant.c_str(), completed,
           cycles, g_notStarted);
    if (!product)
        CoUninitialize();
    EXPECT_EQ(completed, cycles);
    if (product)
        EXPECT_GT(completed - g_notStarted, 0) << "No active stream teardown was exercised";
}

struct Child {
    PROCESS_INFORMATION info{};
    DWORD exitCode = STILL_ACTIVE;
    ~Child() {
        if (info.hProcess) {
            if (WaitForSingleObject(info.hProcess, 0) != WAIT_OBJECT_0) {
                TerminateProcess(info.hProcess, 0xDEAD);
                WaitForSingleObject(info.hProcess, 5000);
            }
            CloseHandle(info.hProcess);
        }
        if (info.hThread)
            CloseHandle(info.hThread);
    }
};

bool Spawn(const std::wstring& filter, const std::vector<std::pair<std::wstring, std::wstring>>& env, HANDLE job,
           const std::vector<HANDLE>& inherit, Child* child) {
    std::vector<std::pair<std::wstring, std::wstring>> previous;
    for (const auto& [name, value] : env) {
        previous.emplace_back(name, Env(name.c_str()));
        SetEnvironmentVariableW(name.c_str(), value.c_str());
    }
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(self) + L"\" --gtest_also_run_disabled_tests --gtest_filter=" + filter;
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<BYTE> attributes(size);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size);
    std::vector<HANDLE> handles = inherit;
    if (!handles.empty())
        UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                  static_cast<void*>(handles.data()), handles.size() * sizeof(HANDLE), nullptr,
                                  nullptr);
    const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, handles.empty() ? FALSE : TRUE,
                                        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED, nullptr, nullptr,
                                        &startup.StartupInfo, &child->info);
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    for (const auto& [name, value] : previous)
        SetEnvironmentVariableW(name.c_str(), value.empty() ? nullptr : value.c_str());
    if (!created)
        return false;
    return AssignProcessToJobObject(job, child->info.hProcess) && ResumeThread(child->info.hThread) != DWORD(-1);
}

TEST(DISABLED_ProcessLoopbackProbe, Orchestrate) {
    OSVERSIONINFOEXW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
    if (auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))
        rtlGetVersion(&version);
    printf("[ProcessLoopbackProbe] windows=%lu.%lu.%lu\n", version.dwMajorVersion, version.dwMinorVersion,
           version.dwBuildNumber);

    ce::HandleGuard job(CreateJobObjectW(nullptr, nullptr));
    ASSERT_TRUE(job);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    ASSERT_TRUE(SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)));
    SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
    const std::wstring cycles = Env(L"CE_PROBE_CYCLES").empty() ? L"150" : Env(L"CE_PROBE_CYCLES");
    const std::wstring variantFilter = Env(L"CE_PROBE_VARIANT");
    struct RendererMode {
        const wchar_t* name;
        const wchar_t* signal;
        const wchar_t* churn;
    };
    const RendererMode modes[] = {{L"silent", L"0", L"0"}, {L"signal", L"1", L"0"}, {L"signal+churn", L"1", L"1"}};
    int scenarios = 0;
    for (const RendererMode& mode : modes)
    for (const wchar_t* variant : {L"stop_release", L"release_only", L"cross_thread_release", L"duplicate_stop_release",
                                   L"duplicate_release", L"abandon", L"captureengine_capture", L"captureengine_reuse",
                                   L"captureengine_worker", L"captureengine_cancel"}) {
        if (!variantFilter.empty() && variantFilter != variant)
            continue;
        ++scenarios;
        ce::HandleGuard ready(CreateEventW(&inheritable, TRUE, FALSE, nullptr));
        ce::HandleGuard stop(CreateEventW(&inheritable, TRUE, FALSE, nullptr));
        ASSERT_TRUE(ready && stop);
        Child renderer;
        ASSERT_TRUE(Spawn(L"DISABLED_ProcessLoopbackProbe.Renderer",
                          {{L"CE_PROBE_READY", std::to_wstring(reinterpret_cast<uintptr_t>(ready.get()))},
                           {L"CE_PROBE_STOP", std::to_wstring(reinterpret_cast<uintptr_t>(stop.get()))},
                           {L"CE_PROBE_SIGNAL", mode.signal},
                           {L"CE_PROBE_CHURN", mode.churn}},
                          job.get(), {ready.get(), stop.get()}, &renderer));
        if (WaitForSingleObject(ready.get(), 30000) != WAIT_OBJECT_0) {
            printf("[ProcessLoopbackProbe] renderer=%ls variant=%ls renderer_not_ready\n", mode.name, variant);
            TerminateProcess(renderer.info.hProcess, 1);
            ADD_FAILURE() << "Renderer did not become ready";
        } else {
            Child capture;
            ASSERT_TRUE(Spawn(L"DISABLED_ProcessLoopbackProbe.Capture",
                              {{L"CE_PROBE_TARGET_PID", std::to_wstring(renderer.info.dwProcessId)},
                               {L"CE_PROBE_VARIANT", variant},
                               {L"CE_PROBE_CYCLES", cycles},
                               {L"CE_PROBE_CHURN", mode.churn}},
                              job.get(), {}, &capture));
            const DWORD waited = WaitForSingleObject(capture.info.hProcess, kChildTimeoutMs);
            if (waited != WAIT_OBJECT_0) {
                TerminateProcess(capture.info.hProcess, 0xDEAD);
                WaitForSingleObject(capture.info.hProcess, 5000);
            }
            EXPECT_EQ(waited, WAIT_OBJECT_0);
            EXPECT_TRUE(GetExitCodeProcess(capture.info.hProcess, &capture.exitCode));
            const char* outcome = capture.exitCode == 0 ? "ok" : (capture.exitCode >= 0xC0000000u ? "CRASH" : "failed");
            printf("[ProcessLoopbackProbe] renderer=%ls variant=%ls cycles=%ls exit=0x%08lx outcome=%s\n", mode.name,
                   variant, cycles.c_str(), capture.exitCode, outcome);
            EXPECT_EQ(capture.exitCode, 0u);
            SetEvent(stop.get());
            EXPECT_EQ(WaitForSingleObject(renderer.info.hProcess, 10000), WAIT_OBJECT_0);
        }
    }
    EXPECT_GT(scenarios, 0) << "Unknown CE_PROBE_VARIANT";
}

}  // namespace
