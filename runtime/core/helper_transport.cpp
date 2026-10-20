// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "runtime/core/helper_transport.h"

#include "include/cengine/cengine.h"

#include <cstdio>

namespace ce::engine {
namespace {

ce::runtime::HelperTransport TransportFor(bool isolated, ce::runtime::RoleEntry entry) {
    return !isolated && entry ? ce::runtime::HelperTransport::Thread : ce::runtime::HelperTransport::Process;
}

const char* TransportName(ce::runtime::HelperTransport transport) {
    return transport == ce::runtime::HelperTransport::Thread ? "thread" : "process";
}

const char* SourceName(IsolationSource source) {
    switch (source) {
        case IsolationSource::Feature:
            return "feature";
        case IsolationSource::Setting:
            return "setting";
        case IsolationSource::Default:
            break;
    }
    return "default";
}

}  // namespace

HelperTransportChoice ChooseHelperTransports(uint32_t features, bool isolatedSetting,
                                             const ce::runtime::RoleEntries& entries) {
    HelperTransportChoice choice;
    if ((features & CE_FEATURE_ISOLATED_HELPERS) != 0) {
        choice.isolated = true;
        choice.source = IsolationSource::Feature;
    } else if (isolatedSetting) {
        choice.isolated = true;
        choice.source = IsolationSource::Setting;
    }
    choice.transports.inject = TransportFor(choice.isolated, entries.inject);
    choice.transports.media = TransportFor(choice.isolated, entries.media);
    choice.transports.logger = TransportFor(choice.isolated, entries.logger);
    return choice;
}

std::string DescribeHelperTransports(const HelperTransportChoice& choice) {
    char text[256];
    snprintf(text, sizeof(text),
             "[Runtime] helper transports: inject=%s media=%s logger=%s sensors=process isolated=%d (%s)",
             TransportName(choice.transports.inject), TransportName(choice.transports.media),
             TransportName(choice.transports.logger), choice.isolated ? 1 : 0, SourceName(choice.source));
    return text;
}

}  // namespace ce::engine
