#include "sharpen_constants.h"

// AMD's headers are a shader dialect that also compiles for the host once
// FFX_CPU selects the CPU backend. Its math helpers call `sqrt`, `floor` and
// `exp2` unqualified, so the C names have to be in the global namespace first;
// <math.h> rather than <cmath> guarantees that. FFX_HALF is stated explicitly
// because the headers test it with `#if` and the build warns on undefined
// macros in a conditional.
#include <math.h>

#define FFX_CPU 1
#define FFX_HALF 0
#include "../../external/fidelityfx/gpu/ffx_core.h"
#include "../../external/fidelityfx/gpu/cas/ffx_cas.h"
#include "../../external/fidelityfx/gpu/fsr1/ffx_fsr1.h"

namespace ce::sharpen {

ShaderConstants BuildShaderConstants(Mode mode, const Decision& decision, uint32_t width, uint32_t height) {
    ShaderConstants constants;
    constants.filterSpace = static_cast<uint32_t>(decision.filterSpace);
    constants.maxCoord[0] = width > 0 ? static_cast<int32_t>(width - 1) : 0;
    constants.maxCoord[1] = height > 0 ? static_cast<int32_t>(height - 1) : 0;

    if (mode == Mode::Cas) {
        // Sharpen only: input and output extents are identical, which is what
        // makes `ffxCasFilter`'s noScaling path valid.
        const float extentX = static_cast<float>(width);
        const float extentY = static_cast<float>(height);
        ffxCasSetup(constants.const0, constants.const1, decision.effectParameter, extentX, extentY, extentX, extentY);
    } else if (mode == Mode::Rcas) {
        FsrRcasCon(constants.const0, decision.effectParameter);
    }

    return constants;
}

}  // namespace ce::sharpen
