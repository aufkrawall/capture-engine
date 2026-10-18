#include "screen_grab_adapter.h"

#include <dxgi1_6.h>

#include <cstdio>
#include <cstring>
#include <cwchar>

#include "common/platform/monitor_selection.h"

namespace ce::media {

namespace {

bool EndsWith(const std::string& value, const char* suffix) {
    const size_t length = std::strlen(suffix);
    return value.size() >= length && value.compare(value.size() - length, length, suffix) == 0;
}

HMONITOR ResolveTargetMonitor(const std::string& captureMonitor) {
    namespace selection = ce::monitor_selection;
    selection::Selector selector;
    if (selection::TryParseSelector(captureMonitor, selector) && selector.kind == selection::SelectorKind::kStableId) {
        const auto monitors = selection::EnumerateActiveMonitors();
        if (const auto* monitor = selection::FindByStableId(monitors, selector.stableId))
            return monitor->handle;
    }
    const POINT origin = {0, 0};
    return MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
}

bool AdapterDrivesMonitor(IDXGIAdapter1* adapter, HMONITOR monitor) {
    for (UINT index = 0;; ++index) {
        IDXGIOutput* output = nullptr;
        if (adapter->EnumOutputs(index, &output) == DXGI_ERROR_NOT_FOUND)
            return false;
        if (!output)
            continue;
        DXGI_OUTPUT_DESC desc = {};
        const bool match = SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor;
        output->Release();
        if (match)
            return true;
    }
}

std::string Narrow(const wchar_t* text) {
    std::string result;
    for (; text && *text && result.size() < 64; ++text)
        result.push_back(*text < 128 ? static_cast<char>(*text) : '?');
    return result;
}

}  // namespace

uint32_t EncoderVendorId(const std::string& encoderName) {
    if (EndsWith(encoderName, "_nvenc"))
        return kVendorNvidia;
    if (EndsWith(encoderName, "_amf"))
        return kVendorAmd;
    if (EndsWith(encoderName, "_qsv"))
        return kVendorIntel;
    return 0;
}

AdapterChoice ChooseScreenGrabAdapter(const std::vector<AdapterCandidate>& candidates, uint32_t encoderVendor,
                                      const std::function<bool(size_t index)>& hasAutoEncoder) {
    AdapterChoice choice;
    if (hasAutoEncoder) {
        for (const bool monitorPass : {true, false}) {
            for (size_t index = 0; index < candidates.size(); ++index) {
                const AdapterCandidate& candidate = candidates[index];
                if (candidate.software || candidate.drivesTargetMonitor != monitorPass || !hasAutoEncoder(index))
                    continue;
                choice.index = index;
                choice.reason = monitorPass ? "auto-encoder-drives-monitor" : "auto-encoder-high-performance";
                choice.crossAdapterCapture = !monitorPass;
                return choice;
            }
        }
        choice = ChooseScreenGrabAdapter(candidates, 0);
        choice.autoEncoderMissing = true;
        return choice;
    }
    bool vendorPresent = false;
    for (const AdapterCandidate& candidate : candidates)
        vendorPresent = vendorPresent || (!candidate.software && candidate.vendorId == encoderVendor);
    const bool filterByVendor = encoderVendor != 0 && vendorPresent;
    choice.encoderVendorMissing = encoderVendor != 0 && !vendorPresent;

    size_t firstEligible = SIZE_MAX;
    for (size_t index = 0; index < candidates.size(); ++index) {
        const AdapterCandidate& candidate = candidates[index];
        if (candidate.software || (filterByVendor && candidate.vendorId != encoderVendor))
            continue;
        if (candidate.drivesTargetMonitor) {
            choice.index = index;
            choice.reason = filterByVendor ? "encoder-vendor-drives-monitor" : "drives-monitor";
            return choice;
        }
        if (firstEligible == SIZE_MAX)
            firstEligible = index;
    }
    if (firstEligible != SIZE_MAX) {
        choice.index = firstEligible;
        choice.reason = filterByVendor ? "encoder-vendor-high-performance" : "high-performance";
        choice.crossAdapterCapture = true;
    }
    return choice;
}

IDXGIAdapter1* SelectScreenGrabAdapter(const std::string& encoderName, const std::string& captureMonitor,
                                       std::string* log,
                                       const std::function<bool(const LUID& adapter)>& hasAutoEncoder) {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) {
        if (log)
            *log = "[MediaEngine] Screen-grab GPU: DXGI factory unavailable; using the default adapter";
        return nullptr;
    }
    IDXGIFactory6* factory6 = nullptr;
    factory->QueryInterface(IID_PPV_ARGS(&factory6));

    const HMONITOR monitor = ResolveTargetMonitor(captureMonitor);
    std::vector<IDXGIAdapter1*> adapters;
    std::vector<AdapterCandidate> candidates;
    std::vector<std::string> names;
    for (UINT index = 0;; ++index) {
        IDXGIAdapter1* adapter = nullptr;
        const HRESULT hr = factory6 ? factory6->EnumAdapterByGpuPreference(
                                          index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter))
                                    : factory->EnumAdapters1(index, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND)
            break;
        if (FAILED(hr) || !adapter)
            continue;
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        AdapterCandidate candidate;
        candidate.luid = desc.AdapterLuid;
        candidate.vendorId = desc.VendorId;
        candidate.software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        candidate.drivesTargetMonitor = monitor && AdapterDrivesMonitor(adapter, monitor);
        adapters.push_back(adapter);
        candidates.push_back(candidate);
        names.push_back(Narrow(desc.Description));
    }
    if (factory6)
        factory6->Release();
    factory->Release();

    const uint32_t vendor = EncoderVendorId(encoderName);
    std::function<bool(size_t)> hasEncoderAt;
    if (hasAutoEncoder)
        hasEncoderAt = [&](size_t index) { return hasAutoEncoder(candidates[index].luid); };
    const AdapterChoice choice = ChooseScreenGrabAdapter(candidates, vendor, hasEncoderAt);
    IDXGIAdapter1* chosen = nullptr;
    if (choice.index != SIZE_MAX) {
        chosen = adapters[choice.index];
        chosen->AddRef();
    }
    if (log) {
        char line[384];
        if (chosen) {
            std::snprintf(line, sizeof(line),
                          "[MediaEngine] Screen-grab GPU: %s (vendor=0x%04X luid=%08lX:%08lX) reason=%s adapters=%zu "
                          "encoder=%s crossAdapterCapture=%d",
                          names[choice.index].c_str(), candidates[choice.index].vendorId,
                          static_cast<unsigned long>(candidates[choice.index].luid.HighPart),
                          static_cast<unsigned long>(candidates[choice.index].luid.LowPart), choice.reason,
                          candidates.size(), encoderName.c_str(), choice.crossAdapterCapture ? 1 : 0);
        } else {
            std::snprintf(line, sizeof(line), "[MediaEngine] Screen-grab GPU: no hardware adapter among %zu; using the "
                                              "default adapter",
                          candidates.size());
        }
        *log = line;
        if (choice.encoderVendorMissing)
            *log += " (no GPU of the encoder's vendor; the encoder cannot open)";
        if (choice.autoEncoderMissing)
            *log += " (encoder=auto found no usable encoder on any GPU)";
    }
    for (IDXGIAdapter1* adapter : adapters)
        adapter->Release();
    return chosen;
}

}  // namespace ce::media
