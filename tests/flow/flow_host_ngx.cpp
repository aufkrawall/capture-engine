#include "tests/flow/flow_host_ngx.h"
#include "tests/flow/flow_host.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace ce::flow {

NGXRuntime::NGXRuntime(FlowGame& game) {
    char path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return;
    const std::string exe(path);
    const std::string modulePath = exe.substr(0, exe.find_last_of('\\') + 1) + "nvngx.dll";
    // Like the injected hook and the other flow runtimes, keep the patched image loaded until process exit.
    const HMODULE module = LoadLibraryA(modulePath.c_str());
    if (!module)
        return;
    game.ServiceHookThread();
    const auto get = reinterpret_cast<ngx::GetParameters>(GetProcAddress(module, "NVSDK_NGX_D3D12_GetParameters"));
    destroy_ = reinterpret_cast<ngx::DestroyParameters>(GetProcAddress(module, "NVSDK_NGX_D3D12_DestroyParameters"));
    create_ = reinterpret_cast<ngx::CreateFeature>(GetProcAddress(module, "NVSDK_NGX_D3D12_CreateFeature"));
    evaluate_ = reinterpret_cast<ngx::EvaluateFeature>(GetProcAddress(module, "NVSDK_NGX_D3D12_EvaluateFeature"));
    release_ = reinterpret_cast<ngx::ReleaseFeature>(GetProcAddress(module, "NVSDK_NGX_D3D12_ReleaseFeature"));
    fail_ = reinterpret_cast<ngx::SetFailure>(GetProcAddress(module, "CEFlowNGX_SetFailure"));
    counters_ = reinterpret_cast<ngx::GetCounters>(GetProcAddress(module, "CEFlowNGX_GetCounters"));
    if (get && destroy_ && create_ && evaluate_ && release_ && fail_ && counters_)
        get(&parameters_);
}

NGXRuntime::~NGXRuntime() {
    if (!Close())
        std::fprintf(stderr, "NGX flow cleanup failed: live runtime resources remain\n");
}

void NGXRuntime::SetGeneratedFrames(int frames) {
    if (parameters_) {
        const auto set = reinterpret_cast<ngx::SetInteger>(parameters_->vtable[3]);
        set(parameters_, "DLSSG.MultiFrameCount", frames);
    }
}

void NGXRuntime::Fail(ngx::Failure failure) {
    if (fail_)
        fail_(failure);
}

void* NGXRuntime::Create(int feature) {
    void* handle = nullptr;
    if (parameters_ && create_(nullptr, feature, parameters_, &handle) == ngx::kSuccess && handle) {
        handles_.push_back(handle);
        return handle;
    }
    return nullptr;
}

bool NGXRuntime::Evaluate(void* handle) {
    return parameters_ && evaluate_(nullptr, handle, parameters_, nullptr) == ngx::kSuccess;
}

bool NGXRuntime::Release(void* handle) {
    if (!release_ || release_(handle) != ngx::kSuccess)
        return false;
    handles_.erase(std::remove(handles_.begin(), handles_.end(), handle), handles_.end());
    return true;
}

bool NGXRuntime::Close() {
    Fail(ngx::Failure::kNone);
    while (!handles_.empty()) {
        if (!Release(handles_.back()))
            return false;
    }
    if (parameters_) {
        if (destroy_(parameters_) != ngx::kSuccess)
            return false;
        parameters_ = nullptr;
    }
    const auto counts = Counters();
    return counts.liveFeatures == 0 && counts.liveParameters == 0;
}

ngx::Counters NGXRuntime::Counters() const {
    ngx::Counters out;
    if (counters_)
        counters_(&out);
    return out;
}

}  // namespace ce::flow
