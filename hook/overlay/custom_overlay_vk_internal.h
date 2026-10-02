#pragma once

#include "custom_overlay_vk.h"

#include <cstring>

#include <vector>

#include "hook/runtime/hook_common.h"

#include "overlay_shader_spirv.h"

// Include dispatch table structures from vulkan_layer.h
// This is safe because the Vulkan layer build includes this file
#include "hook/vulkan_layer/vulkan_layer.h"
