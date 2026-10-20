// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
// The runtime lifecycle of the C ABI: create, request shutdown, shutdown, destroy (llm-wiki/library/03 "Lifecycle").
#include "include/cengine/cengine.h"

#include "common/logging/logging.h"
#include "runtime/api/capi_testing.h"
#include "runtime/api/handle_registry.h"
#include "runtime/api/runtime_desc.h"
#include "runtime/api/runtime_handle.h"
#include "runtime/api/runtime_package.h"
#include "runtime/core/core.h"

#include <memory>
#include <optional>
#include <string>

namespace ce::capi {
namespace {

// The engine a create call starts: the test factory when one is set, else the production assembly.
std::unique_ptr<ce::engine::Runtime> StartEngine(const ce::engine::RuntimeOptions& options, ce_status_t& status) {
#ifdef CE_UNIT_TESTS
    if (testing::RuntimeFactorySlot())
        return testing::RuntimeFactorySlot()(options, &status);
    // This image links no production engine (runtime/core/runtime_create.cpp), so without a factory there is none.
    status = CE_E_INTERNAL;
    return nullptr;
#else
    return ce::engine::Runtime::Create(options, &status);
#endif
}

// The engine options of a create call. With a test factory set, package resolution is skipped (capi_testing.h).
ce_status_t BuildOptions(const RuntimeDescCopy& copy, ce::engine::RuntimeOptions& options) {
    options.features = copy.features;
#ifdef CE_UNIT_TESTS
    if (testing::RuntimeFactorySlot())
        return CE_OK;
#endif
    PackageLayout package;
    const char* reason = nullptr;
    const ce_status_t status = ResolvePackage(copy.packageDir, package, &reason);
    if (status != CE_OK) {
        LogWarn("[CApi] ce_runtime_create refused: %s", reason ? reason : "package unavailable");
        return status;
    }
    options.configPath = package.configPath;
    options.roleHost = package.roleHost;
    options.windowModule = package.windowModule;
    return CE_OK;
}

// Validates the descriptor, claims the session, resolves the package and starts the engine. The claim is held
// until the handle is published, so a second create during this one is refused before it builds anything.
ce_status_t CreateRuntime(const ce_runtime_desc_t* desc, ce_runtime_t** out) {
    RuntimeDescCopy copy;
    const char* reason = nullptr;
    ce_status_t status = ValidateRuntimeDesc(desc, &copy, &reason);
    if (status != CE_OK) {
        LogWarn("[CApi] ce_runtime_create refused: %s", reason ? reason : "invalid descriptor");
        return status;
    }
    auto claim = LiveRuntimes().TryClaim();
    if (!claim) {
        LogWarn("[CApi] ce_runtime_create refused: a runtime is already live in this session");
        return claim.status();
    }
    ce::engine::RuntimeOptions options;
    status = BuildOptions(copy, options);
    if (status != CE_OK)
        return status;
    std::unique_ptr<ce::engine::Runtime> engine = StartEngine(options, status);
    if (!engine) {
        if (status == CE_OK)
            status = CE_E_INTERNAL;
        LogError("[CApi] ce_runtime_create: the engine did not start (%s)", ce_status_string(status));
        return status;
    }
    std::string clientName = copy.clientName.empty() ? std::string("unnamed") : copy.clientName;
    std::unique_ptr<ce_runtime> handle(new ce_runtime{std::move(engine), std::move(clientName)});
    if (!claim.Publish(handle.get())) {
        LogError("[CApi] ce_runtime_create: the handle registry refused the new runtime");
        return CE_E_INTERNAL;
    }
    LogInfo("[CApi] runtime #%llu created for client %s",
            static_cast<unsigned long long>(LiveRuntimes().Generation()), handle->clientName.c_str());
    *out = handle.release();
    return CE_OK;
}

}  // namespace
}  // namespace ce::capi

extern "C" {

ce_status_t ce_runtime_create(const ce_runtime_desc_t* desc, ce_runtime_t** out_runtime) {
    if (!out_runtime)
        return CE_E_INVALID_ARGUMENT;
    *out_runtime = nullptr;
    return ce::capi::RunGuarded("ce_runtime_create", [&]() -> ce_status_t {
        return ce::capi::CreateRuntime(desc, out_runtime);
    });
}

ce_status_t ce_runtime_request_shutdown(ce_runtime_t* runtime) {
    return ce::capi::RunGuarded("ce_runtime_request_shutdown", [&]() -> ce_status_t {
        ce_runtime* live = ce::capi::ResolveRuntime(runtime, "ce_runtime_request_shutdown");
        if (!live)
            return CE_E_INVALID_ARGUMENT;
        live->engine->RequestShutdown();
        return CE_OK;
    });
}

ce_status_t ce_runtime_shutdown(ce_runtime_t* runtime, uint32_t timeout_ms) {
    return ce::capi::RunGuarded("ce_runtime_shutdown", [&]() -> ce_status_t {
        ce_runtime* live = ce::capi::ResolveRuntime(runtime, "ce_runtime_shutdown");
        if (!live)
            return CE_E_INVALID_ARGUMENT;
        live->engine->RequestShutdown();
        if (!live->engine->WaitStopped(timeout_ms)) {
            // The handle stays valid: the shutdown continues, and the client may wait again.
            LogWarn("[CApi] ce_runtime_shutdown: not stopped within %u ms", timeout_ms);
            return CE_E_TIMEOUT;
        }
        return CE_OK;
    });
}

ce_status_t ce_runtime_destroy(ce_runtime_t* runtime) {
    return ce::capi::RunGuarded("ce_runtime_destroy", [&]() -> ce_status_t {
        ce_runtime* live = ce::capi::ResolveRuntime(runtime, "ce_runtime_destroy");
        if (!live)
            return CE_E_INVALID_ARGUMENT;
        const int32_t state = live->engine->Status().runtimeState;
        if (state != CE_RUNTIME_STOPPED && state != CE_RUNTIME_FAILED) {
            LogWarn("[CApi] ce_runtime_destroy refused: runtime state %d (shut down first)", state);
            return CE_E_INVALID_STATE;
        }
        // Retiring first makes the handle stale before its object dies. A second destroy of the same handle is a
        // client error; the registry only makes it fail cleanly.
        const ce_status_t retired = ce::capi::LiveRuntimes().Retire(live);
        if (retired != CE_OK)
            return retired;
        LogInfo("[CApi] runtime destroyed (client %s)", live->clientName.c_str());
        delete live;  // joins the engine thread, which has finished
        return CE_OK;
    });
}

}  // extern "C"
