#include "injection_internal.h"

#include "common/logging/log_meter.h"
#include "common/platform/runtime_process_identity.h"

// The injector never injects into CaptureEngine's own processes. Until the runtime can be hosted by any
// client process this rested on the whitelist alone (strict, but a partial entry matches captureengine.exe
// by substring) and nothing stopped an entry from naming the host or a helper. Every path that reaches a
// remote load asks here first: discovery (LaunchDelayedInjectionThread) and the two injectors themselves
// (InjectImpl, InjectEarly), which also serve the controller's target launcher and the startup-launch hosts.
bool InjectionManager::RefuseRuntimeProcess(DWORD pid, const std::string& name, const char* stage) {
    namespace ri = ce::runtime_identity;
    const char* reason = ce::injection_policy::RuntimeTargetRefusalReason(pid == GetCurrentProcessId(),
                                                                          ri::ClassifyAgainstRegisteredHost(pid));
    if (!reason)
        return false;
    static ce::log_meter::ChangeGate refusalLog;
    if (const auto verdict = refusalLog.Observe(pid)) {
        LogWarn("[Inject] Refusing to inject into %s (PID: %lu) at %s: it is %s%s", name.c_str(),
                static_cast<unsigned long>(pid), stage ? stage : "?", reason,
                verdict.suppressed != 0 ? " (repeat refusals suppressed)" : "");
    }
    return true;
}
