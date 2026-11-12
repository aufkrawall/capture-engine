#pragma once

// Test-only GPU readback probe for inspecting final pre-Present swapchain outputs
// (panel presence/bounds, background blend alpha, text ROI hash, and graph landmark motion).

#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <vector>

namespace ce::flow::fake {

constexpr size_t kGraphProfileColumns = 640;
constexpr uint16_t kNoGraph = 0xFFFF;

struct OutputPanelObservation {
    OutputPanelObservation() { std::fill(std::begin(graphProfile), std::end(graphProfile), kNoGraph); }
    uint64_t frame = 0;
    int64_t clockUs = 0;  // the flow hook's virtual clock when the output was read back
    uint32_t refPixel = 0;
    uint32_t sceneCorner = 0;
    uint32_t bgPixel = 0;
    uint32_t regionChanged = 0;
    uint64_t textHash = 0;
    uint64_t graphHash = 0;
    // Per panel column: the graph line's mean height in quarter pixels below the panel's top edge, or kNoGraph.
    uint16_t graphProfile[kGraphProfileColumns];
    uint16_t graphColumns = 0;
    uint16_t panelWidth = 0;
    uint16_t panelHeight = 0;
    uint8_t hasPanel = 0;
    uint8_t bgDoubleBlended = 0;
};

class OutputPixelProbe {
public:
    void Enable(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_.store(enabled, std::memory_order_release);
        if (enabled) {
            pixels_.clear();
            serial_ = 0;
            historyCount_ = 0;
        }
    }

    void Sample(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* target, UINT index, uint64_t frame) {
        if (!enabled_.load(std::memory_order_acquire))
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_.load(std::memory_order_relaxed) || !device || !queue || !target)
            return;
        const auto desc = target->GetDesc();
        if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM || desc.Width > 4096 || desc.Height > 4096)
            return;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
        UINT64 bytes = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, nullptr, nullptr, &bytes);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&readback))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&list))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
            return;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = target;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = target;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = readback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = layout;
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        list->ResourceBarrier(1, &barrier);
        if (FAILED(list->Close()))
            return;
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event)
            return;
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        const HRESULT signal = queue->Signal(fence.Get(), 1);
        const HRESULT notification = SUCCEEDED(signal) ? fence->SetEventOnCompletion(1, event) : signal;
        const DWORD wait = SUCCEEDED(notification) ? WaitForSingleObject(event, 10000) : WAIT_FAILED;
        CloseHandle(event);
        if (wait != WAIT_OBJECT_0)
            std::abort();
        void* mapped = nullptr;
        if (FAILED(readback->Map(0, nullptr, &mapped)) || !mapped)
            return;
        std::vector<uint32_t> pixels(static_cast<size_t>(desc.Width) * desc.Height);
        for (UINT y = 0; y < desc.Height; ++y) {
            const auto* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapped) +
                                                                 layout.Offset + y * layout.Footprint.RowPitch);
            std::copy(row, row + desc.Width, pixels.begin() + static_cast<size_t>(y) * desc.Width);
        }
        D3D12_RANGE noWrites{};
        readback->Unmap(0, &noWrites);
        pixels_.swap(pixels);
        width_ = static_cast<UINT>(desc.Width);
        height_ = desc.Height;
        index_ = index;
        ++serial_;
        if (frame && historyX_ < width_ && historyY_ < height_) {
            HistoryItem item{};
            item.obs = ObserveFrame(frame);
            item.obs.clockUs = FlowClockMicroseconds();
            history_[historyCount_ % history_.size()] = item;
            ++historyCount_;
        }
    }

    void SetHistoryPoint(UINT x, UINT y) {
        std::lock_guard<std::mutex> lock(mutex_);
        historyX_ = x;
        historyY_ = y;
        historyCount_ = 0;
    }

    void SetHistoryRegion(UINT left, UINT top, UINT right, UINT bottom) {
        std::lock_guard<std::mutex> lock(mutex_);
        regionLeft_ = left;
        regionTop_ = top;
        regionRight_ = right;
        regionBottom_ = bottom;
    }

    uint64_t ReadHistory(uint64_t ordinal, uint64_t* frame, uint32_t* pixel) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!frame || !pixel || ordinal >= historyCount_ || historyCount_ - ordinal > history_.size())
            return historyCount_;
        const auto& item = history_[ordinal % history_.size()];
        *frame = item.obs.frame;
        *pixel = item.obs.refPixel;
        return historyCount_;
    }

    uint64_t ReadObservation(uint64_t ordinal, OutputPanelObservation* out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!out || ordinal >= historyCount_ || historyCount_ - ordinal > history_.size())
            return historyCount_;
        *out = history_[ordinal % history_.size()].obs;
        return historyCount_;
    }

    uint64_t Read(UINT x, UINT y, uint32_t* pixel, UINT* index) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pixel || !index || x >= width_ || y >= height_ || pixels_.empty())
            return 0;
        *pixel = pixels_[static_cast<size_t>(y) * width_ + x];
        *index = index_;
        return serial_;
    }

private:
    static int64_t FlowClockMicroseconds() {
        using Clock = int64_t (*)();
        static const auto clock = reinterpret_cast<Clock>(
            GetProcAddress(GetModuleHandleW(L"capture_hook_x64.dll"), "CEFlow_ClockMicroseconds"));
        return clock ? clock() : 0;
    }

    static bool IsGraphPixel(uint32_t pixel) {
        const int r = static_cast<int>(pixel & 255);
        const int g = static_cast<int>((pixel >> 8) & 255);
        const int b = static_cast<int>((pixel >> 16) & 255);
        return r >= 90 && b >= 90 && g * 2 < (std::min)(r, b);
    }

    static uint64_t FnvMix(uint64_t h, uint64_t value) {
        h ^= value;
        h *= 1099511628211ull;
        return h;
    }

    OutputPanelObservation ObserveFrame(uint64_t frame) const {
        OutputPanelObservation obs{};
        obs.frame = frame;
        obs.refPixel = pixels_[static_cast<size_t>(historyY_) * width_ + historyX_];
        uint32_t changed = 0;
        for (UINT y = regionTop_; y < (std::min)(regionBottom_, height_); ++y) {
            for (UINT x = regionLeft_; x < (std::min)(regionRight_, width_); ++x) {
                const uint32_t pixel = pixels_[static_cast<size_t>(y) * width_ + x];
                bool different = false;
                for (unsigned channel = 0; channel < 3; ++channel) {
                    const int a = static_cast<int>((pixel >> (channel * 8)) & 255);
                    const int b = static_cast<int>((obs.refPixel >> (channel * 8)) & 255);
                    different = different || std::abs(a - b) > 4;
                }
                changed += different ? 1u : 0u;
            }
        }
        obs.regionChanged = changed;
        if (width_ <= 32 || height_ <= 64)
            return obs;

        // Panel origin starts at padding * dpiScale (typically 10..20, e.g. 15 at 150% DPI).
        // (2, 2) is pure scene clear; scan along diagonal to locate panel top-left corner.
        obs.sceneCorner = pixels_[22u * width_ + 2u];
        obs.bgPixel = pixels_[22u * width_ + 22u];
        const int sceneB = static_cast<int>((obs.sceneCorner >> 16) & 255);
        if (sceneB < 50)
            return obs;
        UINT panelOrigin = 0;
        const UINT scanRow = (std::min)(22u, height_ > 1u ? height_ - 1u : 0u);
        const UINT maxScan = (std::min)({width_, height_, 48u});
        for (UINT p = 5; p < maxScan; ++p) {
            const uint32_t candidate = pixels_[static_cast<size_t>(scanRow) * width_ + p];
            const int r = static_cast<int>(candidate & 255);
            const int b = static_cast<int>((candidate >> 16) & 255);
            if (b <= (sceneB * 48) / 100 && r <= 15) {
                panelOrigin = p;
                break;
            }
        }
        if (panelOrigin == 0 || panelOrigin + 4 >= maxScan)
            return obs;

        const UINT sampleInset = panelOrigin + 4u;
        obs.bgPixel = pixels_[static_cast<size_t>(scanRow) * width_ + sampleInset];
        const int bgB = static_cast<int>((obs.bgPixel >> 16) & 255);
        // Single blend of bgAlpha=0.72 over B~77 produces B~22; double blend produces B~6; unblended ~77.
        obs.hasPanel = 1;
        obs.bgDoubleBlended = (bgB * 100 < sceneB * 17) ? 1 : 0;

        UINT panelRight = sampleInset;
        while (panelRight < (std::min)(width_, 700u)) {
            const uint32_t p = pixels_[static_cast<size_t>(scanRow) * width_ + panelRight];
            const int b = static_cast<int>((p >> 16) & 255);
            if (std::abs(b - bgB) > 8)
                break;
            ++panelRight;
        }
        UINT panelBottom = scanRow;
        while (panelBottom < (std::min)(height_, 600u)) {
            const uint32_t p = pixels_[static_cast<size_t>(panelBottom) * width_ + sampleInset];
            const int b = static_cast<int>((p >> 16) & 255);
            if (std::abs(b - bgB) > 8)
                break;
            ++panelBottom;
        }
        if (panelRight <= panelOrigin + 40 || panelBottom <= panelOrigin + 50)
            return obs;

        obs.panelWidth = static_cast<uint16_t>(panelRight - panelOrigin);
        obs.panelHeight = static_cast<uint16_t>(panelBottom - panelOrigin);

        // The frame-time graph is the only magenta element (the scenario's frametime_color, flow_host.cpp): one
        // polyline whose height per column is the measured value under it. Its profile moves left with the graph.
        const UINT left = panelOrigin;
        const UINT right = (std::min)(panelRight, left + static_cast<UINT>(kGraphProfileColumns));
        UINT graphTop = panelBottom;
        UINT graphBottom = panelOrigin;
        uint64_t gh = 14695981039346656037ull;
        for (UINT x = left; x < right; ++x) {
            uint32_t sum = 0;
            uint32_t pixelsInColumn = 0;
            for (UINT y = panelOrigin; y < panelBottom; ++y) {
                if (!IsGraphPixel(pixels_[static_cast<size_t>(y) * width_ + x]))
                    continue;
                sum += y - panelOrigin;
                ++pixelsInColumn;
                graphTop = (std::min)(graphTop, y);
                graphBottom = (std::max)(graphBottom, y + 1u);
                gh = FnvMix(gh, (static_cast<uint64_t>(x - left) << 16) | (y - panelOrigin));
            }
            if (pixelsInColumn != 0) {
                obs.graphProfile[x - left] = static_cast<uint16_t>((sum * 4u + pixelsInColumn / 2u) / pixelsInColumn);
                ++obs.graphColumns;
            }
        }
        obs.graphHash = obs.graphColumns != 0 ? gh : 0;

        // Text: bright glyph pixels outside the graph's rows.
        const UINT textTop = panelOrigin + 4u;
        const UINT textBottom = panelBottom > 6u ? panelBottom - 6u : textTop;
        uint64_t th = 14695981039346656037ull;
        for (UINT y = textTop; y < textBottom; ++y) {
            if (graphBottom > graphTop && y >= graphTop && y < graphBottom)
                continue;
            for (UINT x = left + 8u; x + 8u < panelRight; ++x) {
                const uint32_t p = pixels_[static_cast<size_t>(y) * width_ + x];
                const int r = static_cast<int>(p & 255);
                const int g = static_cast<int>((p >> 8) & 255);
                const int b = static_cast<int>((p >> 16) & 255);
                // Panel background with bgAlpha=0.72 stays <= 75; glyphs are brighter.
                if ((std::max)({r, g, b}) > 105)
                    th = FnvMix(th, (static_cast<uint64_t>(x - left) << 16) | (y - textTop));
            }
        }
        obs.textHash = th;
        return obs;
    }

    std::mutex mutex_;
    std::atomic<bool> enabled_{false};
    UINT width_ = 0, height_ = 0, index_ = 0;
    uint64_t serial_ = 0;
    std::vector<uint32_t> pixels_;
    struct HistoryItem {
        OutputPanelObservation obs;
    };
    std::array<HistoryItem, 2048> history_{};
    uint64_t historyCount_ = 0;
    UINT historyX_ = 0, historyY_ = 0;
    UINT regionLeft_ = 0, regionTop_ = 0, regionRight_ = 0, regionBottom_ = 0;
};

}  // namespace ce::flow::fake
