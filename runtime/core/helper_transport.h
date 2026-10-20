// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

// Which transport each helper role runs on (DR-45), decided once when the runtime is created. A role runs as a thread
// unless the runtime is isolated (CE_FEATURE_ISOLATED_HELPERS, or [Runtime] isolated_helpers read at create), and only
// if the role has a thread entry. Sensors are always a process (DR-35). Pure: the create path passes the feature bits,
// the setting and the production entries; the tests drive the combinations.

#include "runtime/children/host_children.h"

#include <cstdint>
#include <string>

namespace ce::engine {

// Where the isolation decision came from, for the create-time log line.
enum class IsolationSource : uint8_t { Default, Feature, Setting };

struct HelperTransportChoice {
    ce::runtime::HelperTransports transports;
    bool isolated = false;
    IsolationSource source = IsolationSource::Default;
};

// The feature wins over the setting when both isolate.
HelperTransportChoice ChooseHelperTransports(uint32_t features, bool isolatedSetting,
                                             const ce::runtime::RoleEntries& entries);

// The create-time line, e.g. "[Runtime] helper transports: inject=thread media=process logger=process sensors=process
// isolated=0 (default)".
std::string DescribeHelperTransports(const HelperTransportChoice& choice);

}  // namespace ce::engine
