// The build's version string; see setup.h.

#include "common/build_version.h"
#include "setup.h"

namespace ce::setup {

const wchar_t kVersion[] = L"" CAPTURE_VERSION;
const char kVersionUtf8[] = CAPTURE_VERSION;

}  // namespace ce::setup
