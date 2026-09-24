#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <set>
#include <string>

#include "../hook/common/gl_overlay_state_policy.h"
#include "source_fragment_reader.h"

namespace gls = ce::gl_overlay_state;

namespace {

// A small model of the GL state the overlay touches: per-unit texture and
// sampler bindings, per-VAO element-array bindings, and a flat integer table.
struct FakeGl {
    std::set<uint32_t> enabled;
    std::map<uint32_t, int32_t> ints;
    std::map<uint32_t, int32_t> textureByUnit;
    std::map<uint32_t, int32_t> samplerByUnit;
    std::map<int32_t, int32_t> elementByVao;
    int32_t viewport[4] = {};
    int32_t colorMask[4] = {1, 1, 1, 1};
    int32_t polygon[2] = {static_cast<int32_t>(gls::kFill), static_cast<int32_t>(gls::kFill)};
    uint32_t activeUnit = gls::kTexture0;
    int32_t vao = 0;
    int32_t arrayBuffer = 0;
    int32_t unpackBuffer = 0;

    void GetInteger(uint32_t pname, int32_t* values) {
        switch (pname) {
            case gls::kViewport:
                std::memcpy(values, viewport, sizeof(viewport));
                return;
            case gls::kColorWritemask:
                std::memcpy(values, colorMask, sizeof(colorMask));
                return;
            case gls::kPolygonMode:
                values[0] = polygon[0];
                values[1] = polygon[1];
                return;
            case gls::kActiveTexture:
                *values = static_cast<int32_t>(activeUnit);
                return;
            case gls::kTextureBinding2D:
                *values = textureByUnit[activeUnit];
                return;
            case gls::kSamplerBinding:
                *values = samplerByUnit[activeUnit];
                return;
            case gls::kVertexArrayBinding:
                *values = vao;
                return;
            case gls::kArrayBufferBinding:
                *values = arrayBuffer;
                return;
            case gls::kElementArrayBufferBinding:
                *values = elementByVao[vao];
                return;
            case gls::kPixelUnpackBufferBinding:
                *values = unpackBuffer;
                return;
            default:
                if (int32_t* field = ArrayField(pname)) {
                    *values = *field;
                    return;
                }
                *values = ints.count(pname) ? ints[pname] : 0;
                return;
        }
    }
    bool IsEnabled(uint32_t cap) { return enabled.count(cap) != 0; }
    void SetEnabled(uint32_t cap, bool on) {
        if (on)
            enabled.insert(cap);
        else
            enabled.erase(cap);
    }
    void BlendFunc(uint32_t src, uint32_t dst) { BlendFuncSeparate(src, dst, src, dst); }
    void BlendFuncSeparate(uint32_t sr, uint32_t dr, uint32_t sa, uint32_t da) {
        ints[gls::kBlendSrcRgb] = static_cast<int32_t>(sr);
        ints[gls::kBlendSrc] = static_cast<int32_t>(sr);
        ints[gls::kBlendDstRgb] = static_cast<int32_t>(dr);
        ints[gls::kBlendDst] = static_cast<int32_t>(dr);
        ints[gls::kBlendSrcAlpha] = static_cast<int32_t>(sa);
        ints[gls::kBlendDstAlpha] = static_cast<int32_t>(da);
    }
    void BlendEquationSeparate(uint32_t rgb, uint32_t alpha) {
        ints[gls::kBlendEquationRgb] = static_cast<int32_t>(rgb);
        ints[gls::kBlendEquationAlpha] = static_cast<int32_t>(alpha);
    }
    void ColorMask(bool r, bool g, bool b, bool a) {
        colorMask[0] = r;
        colorMask[1] = g;
        colorMask[2] = b;
        colorMask[3] = a;
    }
    void PolygonMode(uint32_t, uint32_t mode) { polygon[0] = polygon[1] = static_cast<int32_t>(mode); }
    void ActiveTexture(uint32_t unit) { activeUnit = unit; }
    void BindTexture2D(uint32_t texture) { textureByUnit[activeUnit] = static_cast<int32_t>(texture); }
    void BindSampler(uint32_t unit, uint32_t sampler) {
        samplerByUnit[gls::kTexture0 + unit] = static_cast<int32_t>(sampler);
    }
    void UseProgram(uint32_t program) { ints[gls::kCurrentProgram] = static_cast<int32_t>(program); }
    void BindVertexArray(uint32_t id) { vao = static_cast<int32_t>(id); }
    void BindBuffer(uint32_t target, uint32_t buffer) {
        if (target == gls::kArrayBuffer)
            arrayBuffer = static_cast<int32_t>(buffer);
        else if (target == gls::kElementArrayBuffer)
            elementByVao[vao] = static_cast<int32_t>(buffer);
        else if (target == gls::kPixelUnpackBuffer)
            unpackBuffer = static_cast<int32_t>(buffer);
    }
    void PixelStore(uint32_t pname, int32_t value) { ints[pname] = value; }

    // Fixed-function client arrays of vertex array 0: {size, type, stride, pointer, sourced buffer}.
    struct Array {
        int32_t size = 4, type = 0x1406, stride = 0;
        const void* pointer = nullptr;
        int32_t buffer = 0;
        bool operator==(const Array& o) const {
            return size == o.size && type == o.type && stride == o.stride && pointer == o.pointer &&
                   buffer == o.buffer;
        }
    };
    Array vertexArray, colorArray, texCoordArray;
    const void* GetPointer(uint32_t pname) {
        if (pname == gls::kVertexArrayPointer)
            return vertexArray.pointer;
        if (pname == gls::kColorArrayPointer)
            return colorArray.pointer;
        return texCoordArray.pointer;
    }
    int32_t* ArrayField(uint32_t pname) {
        switch (pname) {
            case gls::kVertexArraySize: return &vertexArray.size;
            case gls::kVertexArrayType: return &vertexArray.type;
            case gls::kVertexArrayStride: return &vertexArray.stride;
            case gls::kVertexArrayBufferBinding: return &vertexArray.buffer;
            case gls::kColorArraySize: return &colorArray.size;
            case gls::kColorArrayType: return &colorArray.type;
            case gls::kColorArrayStride: return &colorArray.stride;
            case gls::kColorArrayBufferBinding: return &colorArray.buffer;
            case gls::kTexCoordArraySize: return &texCoordArray.size;
            case gls::kTexCoordArrayType: return &texCoordArray.type;
            case gls::kTexCoordArrayStride: return &texCoordArray.stride;
            case gls::kTexCoordArrayBufferBinding: return &texCoordArray.buffer;
            default: return nullptr;
        }
    }
    static void Specify(Array& a, int32_t size, uint32_t type, int32_t stride, const void* p, int32_t buffer) {
        a.size = size;
        a.type = static_cast<int32_t>(type);
        a.stride = stride;
        a.pointer = p;
        a.buffer = buffer;  // glXPointer latches the array buffer bound at call time
    }
    void VertexPointer(int32_t s, uint32_t t, int32_t st, const void* p) { Specify(vertexArray, s, t, st, p, arrayBuffer); }
    void ColorPointer(int32_t s, uint32_t t, int32_t st, const void* p) { Specify(colorArray, s, t, st, p, arrayBuffer); }
    void TexCoordPointer(int32_t s, uint32_t t, int32_t st, const void* p) {
        Specify(texCoordArray, s, t, st, p, arrayBuffer);
    }
    void Viewport(int32_t x, int32_t y, int32_t w, int32_t h) {
        viewport[0] = x;
        viewport[1] = y;
        viewport[2] = w;
        viewport[3] = h;
    }

    bool operator==(const FakeGl& o) const {
        return enabled == o.enabled && ints == o.ints && textureByUnit == o.textureByUnit &&
               samplerByUnit == o.samplerByUnit && elementByVao == o.elementByVao &&
               std::memcmp(viewport, o.viewport, sizeof(viewport)) == 0 &&
               std::memcmp(colorMask, o.colorMask, sizeof(colorMask)) == 0 &&
               std::memcmp(polygon, o.polygon, sizeof(polygon)) == 0 && activeUnit == o.activeUnit &&
               vao == o.vao && arrayBuffer == o.arrayBuffer && unpackBuffer == o.unpackBuffer &&
               vertexArray == o.vertexArray && colorArray == o.colorArray && texCoordArray == o.texCoordArray;
    }
};

gls::Capabilities FullCaps() {
    gls::Capabilities caps;
    caps.blendFuncSeparate = true;
    caps.blendEquationSeparate = true;
    caps.activeTexture = true;
    caps.samplerObjects = true;
    caps.programs = true;
    caps.vertexArrays = true;
    caps.buffers = true;
    caps.pixelUnpackBuffer = true;
    caps.polygonMode = true;
    caps.fixedFunction = true;
    caps.clientArrayPointers = true;
    return caps;
}

// An engine that set its state once and relies on it staying that way.
FakeGl GameState() {
    FakeGl gl;
    gl.enabled = {gls::kDepthTest, gls::kCullFace, gls::kScissorTest, gls::kStencilTest, gls::kLighting};
    gl.BlendFuncSeparate(1, 0x0301, 0, 1);
    gl.BlendEquationSeparate(0x8008, gls::kFuncAdd);  // GL_MAX
    gl.ColorMask(true, true, true, false);
    gl.PolygonMode(gls::kFrontAndBack, 0x1B01);  // GL_LINE
    gl.textureByUnit[gls::kTexture0] = 11;
    gl.textureByUnit[gls::kTexture0 + 3] = 33;
    gl.samplerByUnit[gls::kTexture0] = 5;
    gl.activeUnit = gls::kTexture0 + 3;
    gl.ints[gls::kCurrentProgram] = 42;
    gl.vao = 7;
    gl.elementByVao[7] = 70;
    gl.elementByVao[0] = 0;
    gl.arrayBuffer = 0;  // the application had no vertex buffer bound
    gl.Viewport(10, 20, 1280, 720);
    gl.ints[gls::kUnpackAlignment] = 1;
    gl.ints[gls::kUnpackRowLength] = 512;
    gl.ints[gls::kUnpackSkipRows] = 3;
    gl.ints[gls::kUnpackSkipPixels] = 2;
    gl.unpackBuffer = 99;
    return gl;
}

// What the overlay's own draw does to the context between prepare and restore.
void OverlayDraws(FakeGl& gl) {
    gl.UseProgram(900);
    gl.BindVertexArray(901);
    gl.BindBuffer(gls::kArrayBuffer, 902);
    gl.BindBuffer(gls::kElementArrayBuffer, 903);
    gl.BindTexture2D(904);
}

}  // namespace

TEST(GlOverlayStatePolicyTest, ModernDrawRestoresEveryStateItChanges) {
    const gls::Capabilities caps = FullCaps();
    FakeGl gl = GameState();
    const FakeGl before = gl;

    const gls::Snapshot snapshot = gls::Capture(gl, caps);
    gls::PrepareOverlayDraw(gl, caps, 1920, 1080);
    EXPECT_TRUE(gl.IsEnabled(gls::kBlend));
    EXPECT_FALSE(gl.IsEnabled(gls::kDepthTest));
    EXPECT_FALSE(gl.IsEnabled(gls::kScissorTest));
    EXPECT_FALSE(gl.IsEnabled(gls::kStencilTest));
    EXPECT_FALSE(gl.IsEnabled(gls::kLighting));
    EXPECT_EQ(gl.colorMask[3], 1);
    EXPECT_EQ(gl.activeUnit, gls::kTexture0);
    EXPECT_EQ(gl.samplerByUnit[gls::kTexture0], 0);
    OverlayDraws(gl);
    gls::Restore(gl, caps, snapshot);

    // The overlay's own vertex array keeps its element binding; that object is
    // not application state.
    EXPECT_EQ(gl.elementByVao[901], 903);
    gl.elementByVao.erase(901);
    EXPECT_TRUE(gl == before);
    // The regressions this pins, spelled out.
    EXPECT_TRUE(gl.IsEnabled(gls::kDepthTest));
    EXPECT_TRUE(gl.IsEnabled(gls::kCullFace));
    EXPECT_EQ(gl.arrayBuffer, 0) << "overlay VBO must not stay bound when the application had none";
    EXPECT_EQ(gl.activeUnit, gls::kTexture0 + 3);
    EXPECT_EQ(gl.textureByUnit[gls::kTexture0], 11) << "unit 0 must get unit 0's binding, not the active unit's";
    EXPECT_EQ(gl.textureByUnit[gls::kTexture0 + 3], 33);
    EXPECT_EQ(gl.elementByVao[7], 70);
}

TEST(GlOverlayStatePolicyTest, MissingCapabilitiesAreNeitherQueriedNorTouched) {
    gls::Capabilities caps;  // GL 1.1 context: nothing optional
    FakeGl gl = GameState();
    const FakeGl before = gl;

    const gls::Snapshot snapshot = gls::Capture(gl, caps);
    gls::PrepareOverlayDraw(gl, caps, 640, 480);
    gl.BindTexture2D(904);
    gls::Restore(gl, caps, snapshot);

    // Program, VAO, buffers, samplers, polygon mode and equations were not
    // owned, so they were not changed; everything else round-trips.
    EXPECT_EQ(gl.ints[gls::kCurrentProgram], 42);
    EXPECT_EQ(gl.vao, 7);
    EXPECT_EQ(gl.samplerByUnit[gls::kTexture0], 5);
    EXPECT_EQ(gl.polygon[0], 0x1B01);
    EXPECT_EQ(gl.ints[gls::kBlendEquationRgb], 0x8008);
    EXPECT_TRUE(gl.IsEnabled(gls::kDepthTest));
    EXPECT_EQ(gl.viewport[2], 1280);
    // The legacy blend path round-trips the RGB pair it can express.
    EXPECT_EQ(gl.ints[gls::kBlendSrcRgb], before.ints.at(gls::kBlendSrcRgb));
    EXPECT_EQ(gl.ints[gls::kBlendDstRgb], before.ints.at(gls::kBlendDstRgb));
}

TEST(GlOverlayStatePolicyTest, LegacyDrawPutsTheApplicationsClientArrayPointersBack) {
    const gls::Capabilities caps = FullCaps();
    FakeGl gl = GameState();
    static const float gameVertices[16] = {};
    static const unsigned char gameColors[16] = {};
    // The application sourced vertices from its own buffer and colours from CPU memory.
    gl.BindVertexArray(0);
    gl.BindBuffer(gls::kArrayBuffer, 55);
    gl.VertexPointer(3, 0x1406, 24, reinterpret_cast<const void*>(12));
    gl.BindBuffer(gls::kArrayBuffer, 0);
    gl.ColorPointer(4, 0x1401, 4, gameColors);
    gl.TexCoordPointer(2, 0x1406, 8, gameVertices);
    gl.BindBuffer(gls::kElementArrayBuffer, 66);  // vertex array 0's element binding
    gl.BindVertexArray(7);
    const FakeGl before = gl;

    // RenderLegacy's order: capture, move to vertex array 0, capture its arrays,
    // point them at overlay storage, draw, restore arrays, then the rest.
    const gls::Snapshot state = gls::Capture(gl, caps);
    gl.BindVertexArray(0);
    const gls::ClientArraySnapshot arrays = gls::CaptureClientArrays(gl, caps);
    gl.BindBuffer(gls::kArrayBuffer, 0);
    gl.BindBuffer(gls::kElementArrayBuffer, 0);
    gls::PrepareOverlayDraw(gl, caps, 800, 600);
    static const float overlayVertices[8] = {};
    gl.VertexPointer(2, 0x1406, 20, overlayVertices);
    gl.ColorPointer(4, 0x1401, 20, overlayVertices);
    gl.TexCoordPointer(2, 0x1406, 20, overlayVertices);
    gl.BindTexture2D(904);
    gls::RestoreClientArrays(gl, caps, arrays);
    gls::Restore(gl, caps, state);

    EXPECT_TRUE(gl == before);
    EXPECT_EQ(gl.vertexArray.pointer, reinterpret_cast<const void*>(12));
    EXPECT_EQ(gl.vertexArray.buffer, 55);
    EXPECT_EQ(gl.colorArray.pointer, gameColors);
    EXPECT_EQ(gl.elementByVao[0], 66);
}

TEST(GlOverlayStatePolicyTest, FontUploadNeverReadsFromTheApplicationsUnpackState) {
    const gls::Capabilities caps = FullCaps();
    FakeGl gl = GameState();
    const FakeGl before = gl;

    const gls::UnpackSnapshot unpack = gls::CaptureAndResetUnpack(gl, caps);
    EXPECT_EQ(gl.unpackBuffer, 0);
    EXPECT_EQ(gl.ints[gls::kUnpackAlignment], 4);
    EXPECT_EQ(gl.ints[gls::kUnpackRowLength], 0);
    gls::RestoreUnpack(gl, caps, unpack);
    EXPECT_TRUE(gl == before);
}

TEST(GlOverlayStatePolicyTest, BothRenderPathsAndInitializationUseTheSnapshot) {
    const std::string source = ce::test_source::ReadLogicalSource("hook/common/custom_overlay_gl.cpp");
    ASSERT_FALSE(source.empty());
    for (const char* entry : {"void OpenGLBackend::RenderModern(", "void OpenGLBackend::RenderLegacy(",
                              "bool OpenGLBackend::Initialize("}) {
        SCOPED_TRACE(entry);
        const size_t start = source.find(entry);
        ASSERT_NE(start, std::string::npos);
        const size_t end = source.find("\nnamespace CustomOverlay {", start);
        const std::string body = source.substr(start, end == std::string::npos ? std::string::npos : end - start);
        EXPECT_NE(body.find("gl_overlay_state::Capture"), std::string::npos);
        EXPECT_NE(body.find("gl_overlay_state::Restore"), std::string::npos);
    }
}
