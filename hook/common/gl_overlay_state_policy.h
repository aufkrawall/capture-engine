#pragma once

// OpenGL overlay state ownership.
//
// The overlay draws into the application's own GL context right before
// SwapBuffers. Every piece of context state it changes must be put back exactly,
// because GL applications (and engines with a CPU-side state cache, which assume
// nothing changes behind their back) often set state once and rely on it for
// the rest of the run. The modern path used to disable depth testing and face
// culling, replace the blend function and active texture unit, and leave its
// own vertex buffer bound, and never restored any of them.
//
// The capture/prepare/restore logic is written against a small adapter so a
// fake context can prove the round trip in unit tests. The adapter provides:
//   void GetInteger(uint32_t pname, int32_t* values);
//   bool IsEnabled(uint32_t cap);
//   void SetEnabled(uint32_t cap, bool enabled);
//   void BlendFunc(uint32_t src, uint32_t dst);
//   void BlendFuncSeparate(uint32_t srcRgb, uint32_t dstRgb, uint32_t srcAlpha, uint32_t dstAlpha);
//   void BlendEquationSeparate(uint32_t rgb, uint32_t alpha);
//   void ColorMask(bool r, bool g, bool b, bool a);
//   void PolygonMode(uint32_t face, uint32_t mode);
//   void ActiveTexture(uint32_t unit);
//   void BindTexture2D(uint32_t texture);
//   void BindSampler(uint32_t unit, uint32_t sampler);
//   void UseProgram(uint32_t program);
//   void BindVertexArray(uint32_t vao);
//   void BindBuffer(uint32_t target, uint32_t buffer);
//   void PixelStore(uint32_t pname, int32_t value);
//   void Viewport(int32_t x, int32_t y, int32_t width, int32_t height);
// Calls for a capability the context lacks are never made (see Capabilities).

#include <cstdint>

namespace ce::gl_overlay_state {

// OpenGL enumerants (values fixed by the specification).
inline constexpr uint32_t kBlend = 0x0BE2;
inline constexpr uint32_t kDepthTest = 0x0B71;
inline constexpr uint32_t kCullFace = 0x0B44;
inline constexpr uint32_t kScissorTest = 0x0C11;
inline constexpr uint32_t kStencilTest = 0x0B90;
inline constexpr uint32_t kAlphaTest = 0x0BC0;
inline constexpr uint32_t kLighting = 0x0B50;
inline constexpr uint32_t kFog = 0x0B60;
inline constexpr uint32_t kTexture2D = 0x0DE1;
inline constexpr uint32_t kViewport = 0x0BA2;
inline constexpr uint32_t kBlendSrc = 0x0BE1;
inline constexpr uint32_t kBlendDst = 0x0BE0;
inline constexpr uint32_t kBlendSrcRgb = 0x80C9;
inline constexpr uint32_t kBlendDstRgb = 0x80C8;
inline constexpr uint32_t kBlendSrcAlpha = 0x80CB;
inline constexpr uint32_t kBlendDstAlpha = 0x80CA;
inline constexpr uint32_t kBlendEquationRgb = 0x8009;
inline constexpr uint32_t kBlendEquationAlpha = 0x883D;
inline constexpr uint32_t kFuncAdd = 0x8006;
inline constexpr uint32_t kSrcAlpha = 0x0302;
inline constexpr uint32_t kOneMinusSrcAlpha = 0x0303;
inline constexpr uint32_t kColorWritemask = 0x0C23;
inline constexpr uint32_t kPolygonMode = 0x0B40;
inline constexpr uint32_t kFrontAndBack = 0x0408;
inline constexpr uint32_t kFill = 0x1B02;
inline constexpr uint32_t kActiveTexture = 0x84E0;
inline constexpr uint32_t kTexture0 = 0x84C0;
inline constexpr uint32_t kTextureBinding2D = 0x8069;
inline constexpr uint32_t kSamplerBinding = 0x8919;
inline constexpr uint32_t kCurrentProgram = 0x8B8D;
inline constexpr uint32_t kVertexArrayBinding = 0x85B5;
inline constexpr uint32_t kArrayBuffer = 0x8892;
inline constexpr uint32_t kElementArrayBuffer = 0x8893;
inline constexpr uint32_t kArrayBufferBinding = 0x8894;
inline constexpr uint32_t kElementArrayBufferBinding = 0x8895;
inline constexpr uint32_t kPixelUnpackBuffer = 0x88EC;
inline constexpr uint32_t kPixelUnpackBufferBinding = 0x88EF;
inline constexpr uint32_t kUnpackAlignment = 0x0CF5;
inline constexpr uint32_t kUnpackRowLength = 0x0CF2;
inline constexpr uint32_t kUnpackSkipRows = 0x0CF3;
inline constexpr uint32_t kUnpackSkipPixels = 0x0CF4;

// Which optional entry points the current context resolved. A missing one is
// neither captured nor touched, so state it guards is left exactly as it was.
struct Capabilities {
    bool blendFuncSeparate = false;
    bool blendEquationSeparate = false;
    bool activeTexture = false;
    bool samplerObjects = false;
    bool programs = false;
    bool vertexArrays = false;
    bool buffers = false;
    bool pixelUnpackBuffer = false;
    bool polygonMode = false;
    // glGetPointerv resolved: the legacy path can read back and restore the
    // fixed-function client-array pointers it overwrites.
    bool clientArrayPointers = false;
    // Fixed-function enables (alpha test, lighting, fog, GL_TEXTURE_2D) exist
    // only outside a core profile; querying them there is an error.
    bool fixedFunction = false;
};

struct Snapshot {
    int32_t viewport[4] = {};
    bool blend = false;
    bool depthTest = false;
    bool cullFace = false;
    bool scissorTest = false;
    bool stencilTest = false;
    bool alphaTest = false;
    bool lighting = false;
    bool fog = false;
    bool texture2D = false;
    int32_t blendSrcRgb = 1;
    int32_t blendDstRgb = 0;
    int32_t blendSrcAlpha = 1;
    int32_t blendDstAlpha = 0;
    int32_t blendEquationRgb = static_cast<int32_t>(kFuncAdd);
    int32_t blendEquationAlpha = static_cast<int32_t>(kFuncAdd);
    int32_t colorMask[4] = {1, 1, 1, 1};
    int32_t polygonMode[2] = {static_cast<int32_t>(kFill), static_cast<int32_t>(kFill)};
    int32_t activeTexture = static_cast<int32_t>(kTexture0);
    // Unit 0 is the one the overlay draws with; its binding is read with unit 0
    // active, never with whatever unit the application left active.
    int32_t texture0Binding = 0;
    int32_t sampler0Binding = 0;
    int32_t program = 0;
    int32_t vertexArray = 0;
    int32_t arrayBuffer = 0;
    // Element-array binding belongs to the bound vertex array; it is read and
    // restored while the application's vertex array is bound.
    int32_t elementArrayBuffer = 0;
};

template <class Gl>
Snapshot Capture(Gl& gl, const Capabilities& caps) {
    Snapshot s{};
    gl.GetInteger(kViewport, s.viewport);
    s.blend = gl.IsEnabled(kBlend);
    s.depthTest = gl.IsEnabled(kDepthTest);
    s.cullFace = gl.IsEnabled(kCullFace);
    s.scissorTest = gl.IsEnabled(kScissorTest);
    s.stencilTest = gl.IsEnabled(kStencilTest);
    if (caps.fixedFunction) {
        s.alphaTest = gl.IsEnabled(kAlphaTest);
        s.lighting = gl.IsEnabled(kLighting);
        s.fog = gl.IsEnabled(kFog);
    }
    if (caps.blendFuncSeparate) {
        gl.GetInteger(kBlendSrcRgb, &s.blendSrcRgb);
        gl.GetInteger(kBlendDstRgb, &s.blendDstRgb);
        gl.GetInteger(kBlendSrcAlpha, &s.blendSrcAlpha);
        gl.GetInteger(kBlendDstAlpha, &s.blendDstAlpha);
    } else {
        gl.GetInteger(kBlendSrc, &s.blendSrcRgb);
        gl.GetInteger(kBlendDst, &s.blendDstRgb);
        s.blendSrcAlpha = s.blendSrcRgb;
        s.blendDstAlpha = s.blendDstRgb;
    }
    if (caps.blendEquationSeparate) {
        gl.GetInteger(kBlendEquationRgb, &s.blendEquationRgb);
        gl.GetInteger(kBlendEquationAlpha, &s.blendEquationAlpha);
    }
    gl.GetInteger(kColorWritemask, s.colorMask);
    if (caps.polygonMode) {
        gl.GetInteger(kPolygonMode, s.polygonMode);
    }
    if (caps.activeTexture) {
        gl.GetInteger(kActiveTexture, &s.activeTexture);
        gl.ActiveTexture(kTexture0);
    }
    gl.GetInteger(kTextureBinding2D, &s.texture0Binding);
    if (caps.fixedFunction) {
        s.texture2D = gl.IsEnabled(kTexture2D);
    }
    if (caps.samplerObjects) {
        gl.GetInteger(kSamplerBinding, &s.sampler0Binding);
    }
    if (caps.programs) {
        gl.GetInteger(kCurrentProgram, &s.program);
    }
    if (caps.vertexArrays) {
        gl.GetInteger(kVertexArrayBinding, &s.vertexArray);
    }
    if (caps.buffers) {
        gl.GetInteger(kArrayBufferBinding, &s.arrayBuffer);
        gl.GetInteger(kElementArrayBufferBinding, &s.elementArrayBuffer);
    }
    return s;
}

// Overlay draw state. Expects Capture() to have run (it leaves unit 0 active).
template <class Gl>
void PrepareOverlayDraw(Gl& gl, const Capabilities& caps, int32_t viewportWidth, int32_t viewportHeight) {
    gl.SetEnabled(kBlend, true);
    if (caps.blendFuncSeparate) {
        gl.BlendFuncSeparate(kSrcAlpha, kOneMinusSrcAlpha, kSrcAlpha, kOneMinusSrcAlpha);
    } else {
        gl.BlendFunc(kSrcAlpha, kOneMinusSrcAlpha);
    }
    if (caps.blendEquationSeparate) {
        gl.BlendEquationSeparate(kFuncAdd, kFuncAdd);
    }
    gl.SetEnabled(kDepthTest, false);
    gl.SetEnabled(kCullFace, false);
    gl.SetEnabled(kScissorTest, false);
    gl.SetEnabled(kStencilTest, false);
    if (caps.fixedFunction) {
        gl.SetEnabled(kAlphaTest, false);
        gl.SetEnabled(kLighting, false);
        gl.SetEnabled(kFog, false);
    }
    gl.ColorMask(true, true, true, true);
    if (caps.polygonMode) {
        gl.PolygonMode(kFrontAndBack, kFill);
    }
    if (caps.samplerObjects) {
        gl.BindSampler(0, 0);
    }
    gl.Viewport(0, 0, viewportWidth, viewportHeight);
}

template <class Gl>
void Restore(Gl& gl, const Capabilities& caps, const Snapshot& s) {
    if (caps.programs) {
        gl.UseProgram(static_cast<uint32_t>(s.program));
    }
    if (caps.vertexArrays) {
        gl.BindVertexArray(static_cast<uint32_t>(s.vertexArray));
    }
    if (caps.buffers) {
        gl.BindBuffer(kElementArrayBuffer, static_cast<uint32_t>(s.elementArrayBuffer));
        gl.BindBuffer(kArrayBuffer, static_cast<uint32_t>(s.arrayBuffer));
    }
    if (caps.activeTexture) {
        gl.ActiveTexture(kTexture0);
    }
    gl.BindTexture2D(static_cast<uint32_t>(s.texture0Binding));
    if (caps.fixedFunction) {
        gl.SetEnabled(kTexture2D, s.texture2D);
    }
    if (caps.samplerObjects) {
        gl.BindSampler(0, static_cast<uint32_t>(s.sampler0Binding));
    }
    if (caps.activeTexture) {
        gl.ActiveTexture(static_cast<uint32_t>(s.activeTexture));
    }
    if (caps.polygonMode) {
        // Core profiles only accept GL_FRONT_AND_BACK, so both faces share the
        // front value there; compatibility contexts report them separately.
        gl.PolygonMode(kFrontAndBack, static_cast<uint32_t>(s.polygonMode[0]));
    }
    gl.ColorMask(s.colorMask[0] != 0, s.colorMask[1] != 0, s.colorMask[2] != 0, s.colorMask[3] != 0);
    if (caps.blendEquationSeparate) {
        gl.BlendEquationSeparate(static_cast<uint32_t>(s.blendEquationRgb),
                                 static_cast<uint32_t>(s.blendEquationAlpha));
    }
    if (caps.blendFuncSeparate) {
        gl.BlendFuncSeparate(static_cast<uint32_t>(s.blendSrcRgb), static_cast<uint32_t>(s.blendDstRgb),
                             static_cast<uint32_t>(s.blendSrcAlpha), static_cast<uint32_t>(s.blendDstAlpha));
    } else {
        gl.BlendFunc(static_cast<uint32_t>(s.blendSrcRgb), static_cast<uint32_t>(s.blendDstRgb));
    }
    gl.SetEnabled(kBlend, s.blend);
    gl.SetEnabled(kDepthTest, s.depthTest);
    gl.SetEnabled(kCullFace, s.cullFace);
    gl.SetEnabled(kScissorTest, s.scissorTest);
    gl.SetEnabled(kStencilTest, s.stencilTest);
    if (caps.fixedFunction) {
        gl.SetEnabled(kAlphaTest, s.alphaTest);
        gl.SetEnabled(kLighting, s.lighting);
        gl.SetEnabled(kFog, s.fog);
    }
    gl.Viewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
}

// Fixed-function client arrays (legacy path). The overlay points the vertex,
// colour and texcoord arrays of vertex array 0 at its own CPU vertex storage.
// Leaving them there hands the application's next client-array draw a pointer
// into overlay memory that is reused or freed, so the pointer specification
// (including the buffer it was sourced from) is put back. Additional adapter
// entry points: const void* GetPointer(uint32_t pname);
//   void VertexPointer/ColorPointer/TexCoordPointer(int32_t size, uint32_t type,
//                                                   int32_t stride, const void* pointer);
inline constexpr uint32_t kVertexArraySize = 0x807A;
inline constexpr uint32_t kVertexArrayType = 0x807B;
inline constexpr uint32_t kVertexArrayStride = 0x807C;
inline constexpr uint32_t kVertexArrayPointer = 0x808E;
inline constexpr uint32_t kVertexArrayBufferBinding = 0x8896;
inline constexpr uint32_t kColorArraySize = 0x8081;
inline constexpr uint32_t kColorArrayType = 0x8082;
inline constexpr uint32_t kColorArrayStride = 0x8083;
inline constexpr uint32_t kColorArrayPointer = 0x8090;
inline constexpr uint32_t kColorArrayBufferBinding = 0x8898;
inline constexpr uint32_t kTexCoordArraySize = 0x8088;
inline constexpr uint32_t kTexCoordArrayType = 0x8089;
inline constexpr uint32_t kTexCoordArrayStride = 0x808A;
inline constexpr uint32_t kTexCoordArrayPointer = 0x8092;
inline constexpr uint32_t kTexCoordArrayBufferBinding = 0x889A;

struct ClientArray {
    int32_t size = 4;
    int32_t type = 0x1406;  // GL_FLOAT
    int32_t stride = 0;
    const void* pointer = nullptr;
    int32_t buffer = 0;
};

struct ClientArraySnapshot {
    ClientArray vertex;
    ClientArray color;
    ClientArray texCoord;  // of client texture unit 0
    // Element-array binding of vertex array 0, which the legacy draw clears
    // even when the application had another vertex array bound.
    int32_t elementArrayBuffer = 0;
};

namespace detail {
template <class Gl>
ClientArray CaptureClientArray(Gl& gl, const Capabilities& caps, uint32_t size, uint32_t type, uint32_t stride,
                               uint32_t pointer, uint32_t buffer) {
    ClientArray array;
    gl.GetInteger(size, &array.size);
    gl.GetInteger(type, &array.type);
    gl.GetInteger(stride, &array.stride);
    array.pointer = gl.GetPointer(pointer);
    if (caps.buffers) {
        gl.GetInteger(buffer, &array.buffer);
    }
    return array;
}
}  // namespace detail

// Call with vertex array 0 and client texture unit 0 current.
template <class Gl>
ClientArraySnapshot CaptureClientArrays(Gl& gl, const Capabilities& caps) {
    ClientArraySnapshot s;
    if (caps.buffers) {
        gl.GetInteger(kElementArrayBufferBinding, &s.elementArrayBuffer);
    }
    if (!caps.clientArrayPointers) {
        return s;
    }
    s.vertex = detail::CaptureClientArray(gl, caps, kVertexArraySize, kVertexArrayType, kVertexArrayStride,
                                          kVertexArrayPointer, kVertexArrayBufferBinding);
    s.color = detail::CaptureClientArray(gl, caps, kColorArraySize, kColorArrayType, kColorArrayStride,
                                         kColorArrayPointer, kColorArrayBufferBinding);
    s.texCoord = detail::CaptureClientArray(gl, caps, kTexCoordArraySize, kTexCoordArrayType, kTexCoordArrayStride,
                                            kTexCoordArrayPointer, kTexCoordArrayBufferBinding);
    return s;
}

// Call with vertex array 0 and client texture unit 0 current, before Restore()
// puts the application's array-buffer binding back.
template <class Gl>
void RestoreClientArrays(Gl& gl, const Capabilities& caps, const ClientArraySnapshot& s) {
    if (!caps.clientArrayPointers) {
        if (caps.buffers) {
            gl.BindBuffer(kElementArrayBuffer, static_cast<uint32_t>(s.elementArrayBuffer));
        }
        return;
    }
    if (caps.buffers) {
        gl.BindBuffer(kArrayBuffer, static_cast<uint32_t>(s.vertex.buffer));
    }
    gl.VertexPointer(s.vertex.size, static_cast<uint32_t>(s.vertex.type), s.vertex.stride, s.vertex.pointer);
    if (caps.buffers) {
        gl.BindBuffer(kArrayBuffer, static_cast<uint32_t>(s.color.buffer));
    }
    gl.ColorPointer(s.color.size, static_cast<uint32_t>(s.color.type), s.color.stride, s.color.pointer);
    if (caps.buffers) {
        gl.BindBuffer(kArrayBuffer, static_cast<uint32_t>(s.texCoord.buffer));
    }
    gl.TexCoordPointer(s.texCoord.size, static_cast<uint32_t>(s.texCoord.type), s.texCoord.stride,
                       s.texCoord.pointer);
    if (caps.buffers) {
        gl.BindBuffer(kElementArrayBuffer, static_cast<uint32_t>(s.elementArrayBuffer));
    }
}

// Font-atlas upload at initialization: the application may have a pixel-unpack
// buffer bound (the upload would then read from that buffer) or non-default
// unpack parameters (the upload would read the wrong rows).
struct UnpackSnapshot {
    int32_t pixelUnpackBuffer = 0;
    int32_t alignment = 4;
    int32_t rowLength = 0;
    int32_t skipRows = 0;
    int32_t skipPixels = 0;
};

template <class Gl>
UnpackSnapshot CaptureAndResetUnpack(Gl& gl, const Capabilities& caps) {
    UnpackSnapshot s{};
    if (caps.pixelUnpackBuffer) {
        gl.GetInteger(kPixelUnpackBufferBinding, &s.pixelUnpackBuffer);
        gl.BindBuffer(kPixelUnpackBuffer, 0);
    }
    gl.GetInteger(kUnpackAlignment, &s.alignment);
    gl.GetInteger(kUnpackRowLength, &s.rowLength);
    gl.GetInteger(kUnpackSkipRows, &s.skipRows);
    gl.GetInteger(kUnpackSkipPixels, &s.skipPixels);
    gl.PixelStore(kUnpackAlignment, 4);
    gl.PixelStore(kUnpackRowLength, 0);
    gl.PixelStore(kUnpackSkipRows, 0);
    gl.PixelStore(kUnpackSkipPixels, 0);
    return s;
}

template <class Gl>
void RestoreUnpack(Gl& gl, const Capabilities& caps, const UnpackSnapshot& s) {
    gl.PixelStore(kUnpackAlignment, s.alignment);
    gl.PixelStore(kUnpackRowLength, s.rowLength);
    gl.PixelStore(kUnpackSkipRows, s.skipRows);
    gl.PixelStore(kUnpackSkipPixels, s.skipPixels);
    if (caps.pixelUnpackBuffer) {
        gl.BindBuffer(kPixelUnpackBuffer, static_cast<uint32_t>(s.pixelUnpackBuffer));
    }
}

}  // namespace ce::gl_overlay_state
