#pragma once

#include <windows.h>

namespace ce::opengl_sampler_override {

using ProcResolver = PROC(WINAPI*)(LPCSTR);

void Initialize();
PROC InterceptProcAddress(const char* name, PROC original, ProcResolver resolver);
void ReconcileBoundTexture(unsigned int target);
void ReconcileTexture(unsigned int texture);
void ReconcileTextureExt(unsigned int texture, unsigned int target);
void ReconcileTextureView(unsigned int texture, unsigned int target);
void NotifyContextChanged();
void Shutdown();

}  // namespace ce::opengl_sampler_override
