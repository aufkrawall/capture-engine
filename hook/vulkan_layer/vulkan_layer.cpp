#include "vulkan_layer_internal.h"

// Reentrancy guard shared with other hooks (defined here for the layer)
thread_local bool g_InPresentHook = false;
