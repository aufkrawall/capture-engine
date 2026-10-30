#pragma once

#include <windows.h>

namespace ce::present_stall::detail {

struct Sampler;

// Startup owns the timer until a worker has started; on success the sampler
// remains alive for the process. The callbacks make failure cleanup testable.
struct SamplerStartupOps {
    void* context;
    HANDLE (*createTimer)(void* context);
    HANDLE (*createThread)(void* context, LPTHREAD_START_ROUTINE entry, void* argument);
    void (*closeHandle)(void* context, HANDLE handle);
};

Sampler* StartSampler(const SamplerStartupOps& ops);

}  // namespace ce::present_stall::detail
