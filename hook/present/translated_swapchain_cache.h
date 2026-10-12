#pragma once

#include <dxgi.h>
#include <optional>

namespace ce::translated_swapchain::detail {

enum class CachePublication { kChanged, kKnown, kUnavailable };

// Decisions belong to the frontend's observed COM lifetime, never to forwarded private data.
// No swapchain reference is retained. An active Release makes its entry unavailable until
// the exact predecessor returns; a final Release retires the entry before address reuse.
std::optional<bool> FindDecision(IDXGISwapChain* frontend);
CachePublication PublishDecision(IDXGISwapChain* frontend, const void* native, bool delegated);

}  // namespace ce::translated_swapchain::detail
