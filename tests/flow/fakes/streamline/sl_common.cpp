// Fake sl.common.dll: the always-loaded common plugin. CE hooks its slGetPluginFunction export; the fake
// resolves nothing through it.

#include "tests/flow/fakes/fake_runtime_log.h"

extern "C" __declspec(dllexport) void* slGetPluginFunction(const char* name) {
    ce::flow::fake::Log("sl.common", "slGetPluginFunction(%s) -> none", name ? name : "(null)");
    return nullptr;
}
