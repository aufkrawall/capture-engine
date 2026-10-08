#include "sharpen_d3d12.h"

#include "hook/runtime/hook_common.h"
#include "sharpen_d3d11.h"

namespace ce::sharpen {

bool D3D12Pass::RecordExternal(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* target,
                               D3D12_RESOURCE_STATES stateBefore, const Request& request, TargetEncoding encoding) {
    if (!device || !list || !target || !Requested(request) || HookIsShuttingDown())
        return false;
    const auto desc = target->GetDesc();
    Target targetInfo;
    targetInfo.route = Route::NativeFgOutput;
    targetInfo.encoding = encoding;
    targetInfo.width = static_cast<uint32_t>(desc.Width);
    targetInfo.height = desc.Height;
    targetInfo.colorBits = IntegerColorBits(desc.Format);
    targetInfo.viewAppliesSrgbConversion = FormatAppliesSrgbConversion(desc.Format);
    targetInfo.sourceRaw = targetInfo.viewAppliesSrgbConversion && ce::gamma::Requested(request.gamma);
    targetInfo.readable = desc.SampleDesc.Count == 1;
    targetInfo.writable = (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
    const auto decision = Decide(request, targetInfo);
    if (logGate_.ShouldLog(decision)) {
        HookLogImportant("PostProcess: DX12 runtime-output %s reason=%s gamma=%.2f->%.2f %ux%u",
                         decision.run ? "running" : "idle", decision.reason, static_cast<double>(decision.gamma.source),
                         static_cast<double>(decision.gamma.destination), targetInfo.width, targetInfo.height);
    }
    if (!decision.run || !EnsureDeviceObjects(device, false) ||
        !EnsurePipelineState(device, decision.mode, desc.Format) || !EnsureSourceCopy(device, target, targetInfo.sourceRaw ? RawUnormViewFormat(desc.Format) : desc.Format) ||
        !EnsureTargetView(device, target, desc.Format))
        return false;
    list->SetPipelineState(pipelineState_.Get());
    RecordCommands(list, target, stateBefore, decision, targetInfo.width, targetInfo.height);
    return true;
}

}  // namespace ce::sharpen
