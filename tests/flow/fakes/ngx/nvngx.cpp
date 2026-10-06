// Minimal NGX core runtime: real exports and the parameter virtual ABI, with controlled failures.
// It records resource ownership and forwards no hook policy or published status decisions.

#include "tests/flow/fakes/ngx/ngx_fake_api.h"
#include "tests/flow/fakes/fake_runtime_log.h"

#include <array>
#include <map>
#include <mutex>
#include <new>
#include <string>

namespace {
using namespace ce::flow::ngx;

struct ParameterObject : Parameters {
    std::map<std::string, int> integers;
};

std::mutex g_mutex;
Counters g_counters;
Failure g_failure = Failure::kNone;
struct FeatureSlot {
    bool live = false;
    int feature = 0;
};
std::array<FeatureSlot, 64> g_features;

void STDMETHODCALLTYPE SetI(Parameters* parameters, const char* name, int value) {
    if (parameters && name)
        static_cast<ParameterObject*>(parameters)->integers[name] = value;
}

void STDMETHODCALLTYPE SetUI(Parameters* parameters, const char* name, unsigned int value) {
    SetI(parameters, name, static_cast<int>(value));
}

void STDMETHODCALLTYPE SetF(Parameters*, const char*, float) {}

Result STDMETHODCALLTYPE GetI(Parameters* parameters, const char* name, int* value) {
    if (!parameters || !name || !value)
        return kFailure;
    const auto& integers = static_cast<ParameterObject*>(parameters)->integers;
    const auto found = integers.find(name);
    if (found == integers.end())
        return kFailure;
    *value = found->second;
    return kSuccess;
}

Result STDMETHODCALLTYPE GetUI(Parameters* parameters, const char* name, unsigned int* value) {
    if (!value)
        return kFailure;
    int integer = 0;
    const Result result = GetI(parameters, name, &integer);
    if (result == kSuccess)
        *value = static_cast<unsigned int>(integer);
    return result;
}

void* g_parameterVtable[13] = {
    nullptr,
    nullptr,
    nullptr,
    reinterpret_cast<void*>(&SetI),
    reinterpret_cast<void*>(&SetUI),
    nullptr,
    reinterpret_cast<void*>(&SetF),
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    reinterpret_cast<void*>(&GetI),
    reinterpret_cast<void*>(&GetUI),
};

FeatureSlot* FindFeature(const void* handle) {
    for (auto& slot : g_features) {
        if (&slot == handle && slot.live)
            return &slot;
    }
    return nullptr;
}
}  // namespace

extern "C" __declspec(dllexport) Result STDMETHODCALLTYPE NVSDK_NGX_D3D12_GetParameters(Parameters** out) {
    if (!out)
        return kFailure;
    auto* parameters = new (std::nothrow) ParameterObject;
    if (!parameters) {
        *out = nullptr;
        return kFailure;
    }
    parameters->vtable = g_parameterVtable;
    *out = parameters;
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_counters.liveParameters;
    return kSuccess;
}

extern "C" __declspec(dllexport) Result STDMETHODCALLTYPE NVSDK_NGX_D3D12_DestroyParameters(Parameters* parameters) {
    if (!parameters)
        return kFailure;
    delete static_cast<ParameterObject*>(parameters);
    std::lock_guard<std::mutex> lock(g_mutex);
    --g_counters.liveParameters;
    return kSuccess;
}

extern "C" __declspec(dllexport) Result __cdecl NVSDK_NGX_D3D12_CreateFeature(void*, int feature, Parameters*,
                                                                              void** handle) {
    if (!handle)
        return kFailure;
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_counters.creates;
    *handle = nullptr;
    if (g_failure == Failure::kCreate)
        return kFailure;
    for (auto& slot : g_features) {
        if (!slot.live) {
            slot = {true, feature};
            *handle = &slot;
            ++g_counters.liveFeatures;
            ce::flow::fake::Log("nvngx", "created feature=%d live=%u", feature, g_counters.liveFeatures);
            return kSuccess;
        }
    }
    return kFailure;
}

extern "C" __declspec(dllexport) Result __cdecl NVSDK_NGX_D3D12_EvaluateFeature(void*, const void* handle,
                                                                                const Parameters*, void*) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_counters.evaluations;
    return g_failure != Failure::kEvaluate && FindFeature(handle) ? kSuccess : kFailure;
}

extern "C" __declspec(dllexport) Result __cdecl NVSDK_NGX_D3D12_ReleaseFeature(void* handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_counters.releases;
    auto* slot = FindFeature(handle);
    if (g_failure == Failure::kRelease || !slot)
        return kFailure;
    slot->live = false;
    --g_counters.liveFeatures;
    ce::flow::fake::Log("nvngx", "released feature=%d live=%u", slot->feature, g_counters.liveFeatures);
    return kSuccess;
}

extern "C" __declspec(dllexport) void CEFlowNGX_SetFailure(Failure failure) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_failure = failure;
}

extern "C" __declspec(dllexport) void CEFlowNGX_GetCounters(Counters* out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (out)
        *out = g_counters;
}
