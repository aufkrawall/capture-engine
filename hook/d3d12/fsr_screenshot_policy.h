#pragma once

#include <windows.h>
#include <cstdint>

#include "hook/sharpen/gamma_external_submission.h"

namespace ce::fsr_screenshot {
// A failed queue Signal cannot prove that submitted commands are no longer using their objects.
class GpuCompletion {
public:
    static constexpr uint64_t kFenceValue = 1;
    void RecordSignal(HRESULT result) { signaled_ = SUCCEEDED(result); }
    bool Complete(uint64_t completed) const {
        return completed == UINT64_MAX || (signaled_ && completed >= kFenceValue);
    }
private:
    bool signaled_ = false;
};

enum class PostProcessResult { NotRequested, Recorded, Failed };

// Screenshot inclusion controls only the later overlay draw. Correct the completed scene/UI composite in all cases.
inline PostProcessResult RecordPostProcess(ID3D12Device* device, ID3D12GraphicsCommandList* list,
                                           ID3D12Resource* target, const ce::sharpen::Request& request,
                                           ce::sharpen::TargetEncoding encoding) {
    if (!ce::sharpen::Requested(request) || !ce::sharpen::RequestHasWork(request, encoding))
        return PostProcessResult::NotRequested;
    return ce::sharpen::RecordRuntimePostProcess(device, list, target, D3D12_RESOURCE_STATE_PRESENT, request, encoding)
        ? PostProcessResult::Recorded : PostProcessResult::Failed;
}
}  // namespace ce::fsr_screenshot
