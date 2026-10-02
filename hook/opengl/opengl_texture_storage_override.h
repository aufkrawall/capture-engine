#pragma once

#include "opengl_sampler_override.h"

namespace ce::opengl_texture_storage_override {

void Initialize();
PROC InterceptProcAddress(const char* name, PROC original, ce::opengl_sampler_override::ProcResolver resolver);
void Shutdown();

}  // namespace ce::opengl_texture_storage_override
