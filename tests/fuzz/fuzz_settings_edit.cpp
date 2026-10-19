// libFuzzer harness for in-place INI edits.
//
// Target: SetIniValue / RemoveIniValue (common/config/config_ini_edit.cpp) on arbitrary document
// bytes - the code a settings commit runs on the text of config.ini. The input is
// [op count - 1] [4 bytes per op] [document bytes]; the ops pick existing sections and keys of the
// document as well as tricky and invalid names and values.
//
// tests/fuzz/settings_edit_checker.h holds the properties (only the edited line changes, the text
// round-trips, a Set is found by a lookup, a Remove is not, unrelated keys keep their values); the
// same checker replays the committed corpus in the unit gate (tests/test_config_edit.cpp).
//
// Built and run by build.py --run-fuzz; see llm-wiki/fuzzing.md.

#include <cstdint>
#include <cstdio>
#include <string>

#include "tests/fuzz/settings_edit_checker.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string failure = ce::settings_edit_check::CheckEditSequence(data, size);
    if (!failure.empty()) {
        fprintf(stderr, "fuzz_settings_edit: %s\n", failure.c_str());
        __builtin_trap();
    }
    return 0;
}
