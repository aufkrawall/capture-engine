#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

// Why the desktop compositor may be drawing a Vulkan swapchain instead of
// letting it flip on its own.
//
// Mirror's Edge on DXVK (sessions 20261018_155920 and 20261018_161139): the
// game presented ~1100 times a second with VK_PRESENT_MODE_IMMEDIATE_KHR, yet
// the sensor service saw its presents expire without a flip completion and took
// screen times from the compositor's flips - at most one per refresh, no
// tearing. Nothing CE logged could say which precondition of an independent
// flip the window missed, so each swapchain now records the window and monitor
// geometry, the window's DPI virtualization and its extended styles, and the
// presenter modules mapped when the driver returned the swapchain.
//
// Pure: the layer collects the facts with user32, this file only judges them,
// so the judgement is unit-tested without a window.
namespace ce::vulkan_presentation_path {

struct Rect {
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;

    int32_t width() const noexcept { return right - left; }
    int32_t height() const noexcept { return bottom - top; }
    bool operator==(const Rect& other) const noexcept {
        return left == other.left && top == other.top && right == other.right && bottom == other.bottom;
    }
};

// DPI_AWARENESS as user32 reports it for the window; kUnknown when the system
// could not answer (pre-1607 user32, or no window).
enum class DpiAwareness : int8_t {
    kUnknown = -1,
    kUnaware = 0,
    kSystemAware = 1,
    kPerMonitorAware = 2,
};

struct WindowFacts {
    bool windowKnown = false;
    bool minimized = false;
    bool layered = false;
    bool topmost = false;
    // All geometry in physical pixels (read from a per-monitor-aware thread
    // context), the space the compositor composes in.
    Rect window = {};
    int32_t clientWidth = 0;
    int32_t clientHeight = 0;
    Rect monitor = {};
    uint32_t swapchainWidth = 0;
    uint32_t swapchainHeight = 0;
    uint32_t windowDpi = 0;
    uint32_t monitorDpi = 0;
    DpiAwareness awareness = DpiAwareness::kUnknown;
};

enum Blocker : uint32_t {
    kBlockerNone = 0,
    kBlockerWindowUnknown = 1u << 0,
    kBlockerMinimized = 1u << 1,
    // The window does not cover its monitor exactly, so the compositor has to
    // place it among other desktop content.
    kBlockerNotCoveringMonitor = 1u << 2,
    // The swapchain is not the size of the window's client area: the
    // compositor scales it.
    kBlockerExtentMismatch = 1u << 3,
    // A DPI-unaware or system-aware window on a monitor with a different DPI
    // is bitmap-stretched by the compositor.
    kBlockerDpiVirtualized = 1u << 4,
    // WS_EX_LAYERED: the window's content is alpha-blended by the compositor.
    kBlockerLayeredWindow = 1u << 5,
};

inline uint32_t IndependentFlipBlockers(const WindowFacts& facts) {
    if (!facts.windowKnown)
        return kBlockerWindowUnknown;
    uint32_t blockers = kBlockerNone;
    if (facts.minimized)
        blockers |= kBlockerMinimized;
    if (!(facts.window == facts.monitor))
        blockers |= kBlockerNotCoveringMonitor;
    if (facts.swapchainWidth != static_cast<uint32_t>(facts.clientWidth > 0 ? facts.clientWidth : 0) ||
        facts.swapchainHeight != static_cast<uint32_t>(facts.clientHeight > 0 ? facts.clientHeight : 0)) {
        blockers |= kBlockerExtentMismatch;
    }
    if ((facts.awareness == DpiAwareness::kUnaware || facts.awareness == DpiAwareness::kSystemAware) &&
        facts.windowDpi != 0 && facts.monitorDpi != 0 && facts.windowDpi != facts.monitorDpi) {
        blockers |= kBlockerDpiVirtualized;
    }
    if (facts.layered)
        blockers |= kBlockerLayeredWindow;
    return blockers;
}

// "none" or the blockers joined by '|'. Always NUL-terminates a non-empty
// buffer; returns the number of characters written.
inline size_t DescribeBlockers(uint32_t blockers, char* out, size_t capacity) {
    if (!out || capacity == 0)
        return 0;
    out[0] = '\0';
    static constexpr struct {
        uint32_t bit;
        const char* name;
    } kNames[] = {
        {kBlockerWindowUnknown, "window-unknown"},
        {kBlockerMinimized, "minimized"},
        {kBlockerNotCoveringMonitor, "not-covering-monitor"},
        {kBlockerExtentMismatch, "extent!=client"},
        {kBlockerDpiVirtualized, "dpi-virtualized"},
        {kBlockerLayeredWindow, "layered-window"},
    };
    size_t written = 0;
    for (const auto& entry : kNames) {
        if ((blockers & entry.bit) == 0)
            continue;
        const int printed = std::snprintf(out + written, capacity - written, "%s%s", written ? "|" : "", entry.name);
        if (printed < 0) {
            out[written] = '\0';
            return written;
        }
        // snprintf truncated and terminated: report what is in the buffer.
        if (static_cast<size_t>(printed) >= capacity - written)
            return capacity - 1;
        written += static_cast<size_t>(printed);
    }
    if (written == 0) {
        const int printed = std::snprintf(out, capacity, "none");
        written = printed > 0 ? static_cast<size_t>(printed) : 0;
        if (written >= capacity)
            written = capacity - 1;
    }
    return written;
}

// Changes when anything the judgement or the log line depends on changes, so
// the layer can log a window that moves into or out of a blocking state once
// per change instead of per present. Never 0 for known facts (0 = not logged).
inline uint32_t FactsSignature(const WindowFacts& facts) {
    uint32_t hash = 2166136261u;
    auto mix = [&hash](uint32_t value) {
        hash ^= value;
        hash *= 16777619u;
    };
    mix(facts.windowKnown ? 1u : 0u);
    mix(facts.minimized ? 1u : 0u);
    mix(facts.layered ? 1u : 0u);
    mix(facts.topmost ? 1u : 0u);
    for (const Rect* rect : {&facts.window, &facts.monitor}) {
        mix(static_cast<uint32_t>(rect->left));
        mix(static_cast<uint32_t>(rect->top));
        mix(static_cast<uint32_t>(rect->right));
        mix(static_cast<uint32_t>(rect->bottom));
    }
    mix(static_cast<uint32_t>(facts.clientWidth));
    mix(static_cast<uint32_t>(facts.clientHeight));
    mix(facts.swapchainWidth);
    mix(facts.swapchainHeight);
    mix(facts.windowDpi);
    mix(facts.monitorDpi);
    mix(static_cast<uint32_t>(static_cast<int32_t>(facts.awareness)));
    return hash == 0 ? 1u : hash;
}

}  // namespace ce::vulkan_presentation_path
