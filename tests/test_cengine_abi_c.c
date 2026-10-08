// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#ifdef __cplusplus
#error This translation unit must compile as C, not C++.
#endif

#include <stddef.h>
#include <stdint.h>

_Static_assert(__STDC_VERSION__ == 201112L, "Compile the ABI probe as C11");

#pragma pack(push, 1)
#include "cengine_abi_layout.h"
struct CengineAmbientPackingProbe {
    char prefix;
    uint64_t value;
};
#pragma pack(pop)
_Static_assert(sizeof(struct CengineAmbientPackingProbe) == 9, "Header must restore ambient packing");
_Static_assert(offsetof(struct CengineAmbientPackingProbe, value) == 1, "Ambient field packing");

uint32_t CengineDraftC11Version(void) {
    return __STDC_VERSION__;
}
