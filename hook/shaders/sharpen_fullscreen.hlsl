// Fullscreen pass vertex shader for the sharpen pixel shaders.
//
// Three vertices covering the viewport, generated from SV_VertexID: no vertex
// or index buffer, no input layout, and nothing to keep alive per swapchain.
// The oversized triangle avoids the diagonal seam a two-triangle quad puts
// through the middle of the frame.

struct VS_OUTPUT {
    float4 pos : SV_POSITION;
};

VS_OUTPUT main(uint vertexId : SV_VertexID) {
    VS_OUTPUT output;
    float2 uv = float2(float((vertexId << 1) & 2u), float(vertexId & 2u));
    output.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}
