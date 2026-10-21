// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

// Which transport each helper role runs on (DR-45), decided once when the runtime is created. A role runs as a thread
// unless the runtime is isolated, and only if the role has a thread entry. Isolation (DR-49): [Runtime]
// isolated_helpers=on|off decides when set; auto follows the client's CE_FEATURE_ISOLATED_HELPERS (the app sets it,
// SDK clients default to threads). Sensors are always a process (DR-35). Pure: the create path passes the feature
// bits, the setting and the production entries; the tests drive the combinations.

#include "runtime/children/host_children.h"

#include <cstdint>
#include <string>

enum class HelperIsolation : uint8_t;  // common/config/config.h

namespace ce::engine {

// Where the isolation decision came from, for the create-time log line.
enum class IsolationSource : uint8_t { Default, Feature, Setting };

struct HelperTransportChoice {
    ce::runtime::HelperTransports transports;
    bool isolated = false;
    IsolationSource source = IsolationSource::Default;
};

// The setting wins when it is on or off; auto follows the feature bit.
HelperTransportChoice ChooseHelperTransports(uint32_t features, HelperIsolation setting,
                                             const ce::runtime::RoleEntries& entries);

// The create-time line, e.g. "[Runtime] helper transports: inject=thread media=process logger=process sensors=process
// isolated=0 (default)".
std::string DescribeHelperTransports(const HelperTransportChoice& choice);

}  // namespace ce::engine
