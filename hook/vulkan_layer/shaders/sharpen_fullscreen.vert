#version 450

// Fullscreen pass vertex shader: three generated vertices, no vertex buffer and
// no input bindings, matching hook/shaders/sharpen_fullscreen.hlsl.

void main() {
    vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
