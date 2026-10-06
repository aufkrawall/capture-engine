#pragma once

#include "tests/flow/fakes/ngx/ngx_fake_api.h"

#include <vector>

namespace ce::flow {

class FlowGame;

// Uses the fake core's actual NGX exports. Hook observations come from FlowGame, never this adapter.
class NGXRuntime {
public:
    explicit NGXRuntime(FlowGame& game);
    ~NGXRuntime();
    NGXRuntime(const NGXRuntime&) = delete;
    NGXRuntime& operator=(const NGXRuntime&) = delete;

    bool Ready() const {
        return parameters_ != nullptr;
    }
    void SetGeneratedFrames(int frames);
    void Fail(ngx::Failure failure);
    void* Create(int feature);
    bool Evaluate(void* handle);
    bool Release(void* handle);
    bool Close();
    ngx::Counters Counters() const;

private:
    ngx::Parameters* parameters_ = nullptr;
    ngx::DestroyParameters destroy_ = nullptr;
    ngx::CreateFeature create_ = nullptr;
    ngx::EvaluateFeature evaluate_ = nullptr;
    ngx::ReleaseFeature release_ = nullptr;
    ngx::SetFailure fail_ = nullptr;
    ngx::GetCounters counters_ = nullptr;
    std::vector<void*> handles_;
};

}  // namespace ce::flow
