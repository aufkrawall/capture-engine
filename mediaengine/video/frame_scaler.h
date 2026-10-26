#pragma once

#include <d3d11.h>
#include <cstdint>

#include "common/logging/log_meter.h"
#include "common/platform/raii_helpers.h"
#include "scale_kernel.h"

// D3D11 backend of the shader scaler; scale_kernel.h owns the policy (kernel,
// anti-ringing, pass order, intermediate precision). It replaces the D3D11
// VideoProcessor's scaler, whose quality and chroma siting were up to each
// driver and whose Blt on NVIDIA queued behind NVENC (llm-wiki/gpu-api-plan.md).
//
// Scale() resamples an RGB image in one pass per scaled axis. The result is
// output-size RGB that is still in the source's encoding (gamma or PQ), ready
// for any output layout: the RGB -> YUV draws (rgb_yuv_draw.h) for 4:2:0,
// 4:2:2 and 4:4:4, or written straight into an RGB target.
//
// Per-geometry state (weight tables, constants, textures) is rebuilt only when
// the request's geometry, kernel or precision changes; each rebuild logs one
// `[Scaler] Plan` line. The caller serializes immediate-context use (MediaEngine
// D3D11 lock) and owns all device state outside the passes.
namespace ce::video_scale {

class FrameScaler {
public:
    using PlanBuilder = decltype(&PlanScale);
    explicit FrameScaler(PlanBuilder buildPlan = &PlanScale) : buildPlan_(buildPlan) {}

    struct Request {
        ID3D11ShaderResourceView* source = nullptr;  // sampleable RGB view of the whole source
        uint32_t sourceWidth = 0;
        uint32_t sourceHeight = 0;
        uint32_t outputWidth = 0;
        uint32_t outputHeight = 0;
        KernelSpec kernel;
        uint32_t sourceBits = 8;  // integer precision of the source (8 or 10)
        uint32_t outputBits = 8;  // of the final output
        // Optional: the last pass renders here (an RGB output layout, outputWidth x outputHeight)
        // instead of the scaler's own output texture.
        ID3D11RenderTargetView* target = nullptr;
    };

    // Shader objects from the build's bytecode. Repeated calls on the same
    // device are free; another device drops everything first.
    bool EnsureShaders(ID3D11Device* device);

    // Runs the passes. Without `target`, *scaled receives the scaler's output
    // view (owned by the scaler, valid until the next Scale or Reset). Source
    // and output of equal size are refused: there is nothing to scale.
    bool Scale(ID3D11Device* device, ID3D11DeviceContext* context, const Request& request,
               ID3D11ShaderResourceView** scaled);

    void Reset();

    // Approximate video memory of the intermediate and output surfaces ([GpuMemory]).
    uint64_t GpuBytes() const;

    // The active plan, for diagnostics and tests.
    const ScalePlan& Plan() const { return plan_; }

private:
    struct PlanKey {
        uint32_t sourceWidth = 0;
        uint32_t sourceHeight = 0;
        uint32_t outputWidth = 0;
        uint32_t outputHeight = 0;
        uint32_t lobes = 0;
        float sideLobeGain = -1.0f;
        IntermediatePrecision precision = IntermediatePrecision::kUnorm16;
        bool ownOutput = false;
        bool operator==(const PlanKey& other) const;
    };
    struct AxisResources {
        ce::ComGuard<ID3D11Buffer> table;
        ce::ComGuard<ID3D11ShaderResourceView> tableView;
        ce::ComGuard<ID3D11Buffer> constants;
    };
    struct Surface {
        ce::ComGuard<ID3D11Texture2D> texture;
        ce::ComGuard<ID3D11RenderTargetView> target;
        ce::ComGuard<ID3D11ShaderResourceView> view;
    };

    bool EnsurePlan(ID3D11Device* device, const Request& request);
    bool CreateAxis(ID3D11Device* device, const AxisTable& table, AxisResources* axis);
    bool CreateSurface(ID3D11Device* device, uint32_t width, uint32_t height, Surface* surface);
    void DrawPass(ID3D11DeviceContext* context, bool horizontal, ID3D11ShaderResourceView* source,
                  ID3D11RenderTargetView* target, uint32_t width, uint32_t height);
    void LogFailure(uint64_t site, const char* what, long hr);

    ID3D11Device* owner_ = nullptr;  // identity only
    ce::ComGuard<ID3D11VertexShader> vertexShader_;
    ce::ComGuard<ID3D11PixelShader> horizontalShader_;
    ce::ComGuard<ID3D11PixelShader> verticalShader_;

    PlanBuilder buildPlan_;
    PlanKey key_;
    bool planReady_ = false;
    ScalePlan plan_;
    AxisResources horizontal_;
    AxisResources vertical_;
    Surface intermediate_;
    Surface output_;
    ce::log_meter::ChangeGate failureLog_{30000};
};

}  // namespace ce::video_scale
