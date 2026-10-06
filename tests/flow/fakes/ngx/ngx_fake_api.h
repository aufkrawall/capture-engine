#pragma once

#include <windows.h>

#include <cstdint>

namespace ce::flow::ngx {

using Result = uint32_t;
constexpr Result kSuccess = 1;
constexpr Result kFailure = 0xBAD00001;

// Independent model of the intercepted NGX parameter ABI. Unsupported virtual slots are never invoked.
// The integer setters/getters occupy slots 3/4 and 11/12; slot 6 is the float setter.
struct Parameters {
    void** vtable;
};

enum class Failure : uint32_t { kNone, kCreate, kEvaluate, kRelease };

struct Counters {
    uint32_t creates = 0;
    uint32_t evaluations = 0;
    uint32_t releases = 0;
    uint32_t liveFeatures = 0;
    uint32_t liveParameters = 0;
};

using GetParameters = Result(STDMETHODCALLTYPE*)(Parameters**);
using DestroyParameters = Result(STDMETHODCALLTYPE*)(Parameters*);
using CreateFeature = Result(__cdecl*)(void*, int, Parameters*, void**);
using EvaluateFeature = Result(__cdecl*)(void*, const void*, const Parameters*, void*);
using ReleaseFeature = Result(__cdecl*)(void*);
using SetFailure = void (*)(Failure);
using GetCounters = void (*)(Counters*);
using SetInteger = void(STDMETHODCALLTYPE*)(Parameters*, const char*, int);

}  // namespace ce::flow::ngx
