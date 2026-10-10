#include <algorithm>
#include <cstdio>

#include "core/shader/compiler.h"
#include "core/shader/glsl_upgrade.h"
#include "test.h"

using namespace wmgl::shader;

namespace {

const UniformInfo* findUniform(const LinkResult& r, const std::string& name) {
    for (const auto& u : r.uniforms)
        if (u.name == name) return &u;
    return nullptr;
}

const AttribInfo* findAttrib(const LinkResult& r, const std::string& name) {
    for (const auto& a : r.attribs)
        if (a.name == name) return &a;
    return nullptr;
}

const char* kFs = "#version 330 core\nuniform vec4 tint; out vec4 color; void main() { color = tint; }\n";

}  // namespace

TEST(shader_compile_reports_errors) {
    CHECK(compile(Stage::Vertex, "#version 330 core\nvoid main() { gl_Position = vec4(0); }\n").ok);
    CompileResult bad = compile(Stage::Vertex, "#version 330 core\nvoid main() { gl_Position = undefined_thing; }\n");
    CHECK(!bad.ok);
    CHECK(bad.log.find("undefined_thing") != std::string::npos);
}

TEST(shader_link_basic_interface) {
    LinkInput in;
    in.shaders.push_back({Stage::Vertex,
                          "#version 330 core\nlayout(location = 3) in vec3 pos; in vec2 uv; uniform mat4 mvp; uniform float unused;\n"
                          "out vec2 v; void main() { v = uv; gl_Position = mvp * vec4(pos, 1.0); }\n"});
    in.shaders.push_back({Stage::Fragment, "#version 330 core\nin vec2 v; uniform sampler2D tex; uniform vec4 tint; out vec4 color;\n"
                                           "void main() { color = texture(tex, v) * tint; }\n"});
    LinkResult r = link(in);
    CHECK(r.ok);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    CHECK(findUniform(r, "mvp") != nullptr);
    CHECK(findUniform(r, "tint") != nullptr);
    CHECK(findUniform(r, "tex") != nullptr);
    CHECK(findUniform(r, "unused") == nullptr);  // inactive uniforms are not reported
    const AttribInfo* pos = findAttrib(r, "pos");
    const AttribInfo* uv = findAttrib(r, "uv");
    CHECK(pos && pos->location == 3);
    CHECK(uv && uv->location == 0);
    CHECK_EQ(r.samplers.size(), size_t(1));
    CHECK(r.stages[0].msl.find("vertex") != std::string::npos);
    CHECK(r.stages[4].msl.find("fragment") != std::string::npos);
}

TEST(shader_attrib_binding_applies) {
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, "#version 330 core\nin vec4 a; in vec4 b; void main() { gl_Position = a + b; }\n"});
    in.shaders.push_back({Stage::Fragment, kFs});
    in.attribBindings["b"] = 0;
    in.attribBindings["a"] = 5;
    LinkResult r = link(in);
    CHECK(r.ok);
    CHECK(findAttrib(r, "a") && findAttrib(r, "a")->location == 5);
    CHECK(findAttrib(r, "b") && findAttrib(r, "b")->location == 0);
    // The MSL must use the GL locations.
    CHECK(r.stages[0].msl.find("[[attribute(5)]]") != std::string::npos);
}

TEST(shader_struct_array_names) {
    LinkInput in;
    in.shaders.push_back({Stage::Vertex,
                          "#version 330 core\nstruct S { float a; vec2 b[2]; };\nuniform S s[2]; uniform float f[3];\n"
                          "void main() { gl_Position = vec4(s[1].a, s[0].b[1], f[2]); }\n"});
    in.shaders.push_back({Stage::Fragment, kFs});
    LinkResult r = link(in);
    CHECK(r.ok);
    CHECK(findUniform(r, "s[0].a") != nullptr);
    CHECK(findUniform(r, "s[1].a") != nullptr);
    CHECK(findUniform(r, "s[1].b[0]") != nullptr);
    const UniformInfo* fa = findUniform(r, "f[0]");
    CHECK(fa && fa->size == 3);
    // Locations are consecutive per element and unique.
    std::vector<int> locations;
    for (const auto& u : r.uniforms)
        if (u.blockIndex < 0)
            for (int e = 0; e < u.size; ++e) locations.push_back(u.location + e);
    std::sort(locations.begin(), locations.end());
    CHECK(std::adjacent_find(locations.begin(), locations.end()) == locations.end());
}

TEST(shader_uniform_block_interface) {
    LinkInput in;
    in.shaders.push_back({Stage::Vertex,
                          "#version 330 core\nlayout(std140) uniform Camera { mat4 view; vec4 light; } cam;\n"
                          "void main() { gl_Position = cam.view * cam.light; }\n"});
    in.shaders.push_back({Stage::Fragment, kFs});
    LinkResult r = link(in);
    CHECK(r.ok);
    CHECK_EQ(r.blocks.size(), size_t(1));
    if (!r.blocks.empty()) {
        CHECK_EQ(r.blocks[0].name, std::string("Camera"));
        CHECK_EQ(r.blocks[0].dataSize, 80);
        CHECK_EQ(r.blocks[0].stageSlot[0], 2);
    }
    const UniformInfo* light = findUniform(r, "Camera.light");
    CHECK(light && light->offset == 64);
}

TEST(shader_legacy_glsl_110) {
    LinkInput in;
    in.shaders.push_back({Stage::Vertex,
                          "#version 110\nattribute vec4 pos; uniform mat4 m; varying vec2 v;\n"
                          "void main() { v = pos.xy; gl_Position = m * pos; }\n"});
    in.shaders.push_back({Stage::Fragment,
                          "#version 110\nvarying vec2 v; uniform sampler2D t;\nvoid main() { gl_FragColor = texture2D(t, v); }\n"});
    LinkResult r = link(in);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    CHECK(r.ok);
    CHECK(findUniform(r, "m") != nullptr);
    CHECK(findAttrib(r, "pos") != nullptr);
}

TEST(shader_legacy_fragdata) {
    UpgradeResult u = upgradeLegacyGlsl(Stage::Fragment, "#version 120\nvoid main() { gl_FragData[1] = vec4(1.0); }\n");
    CHECK(u.ok);
    CHECK(u.source.find("wmgl_FragData[1]") != std::string::npos);
    CHECK(u.source.find("#version 140") == 0);
    UpgradeResult both = upgradeLegacyGlsl(Stage::Fragment,
                                           "#version 120\nvoid main() { gl_FragColor = vec4(0.0); gl_FragData[0] = vec4(1.0); }\n");
    CHECK(!both.ok);
}

TEST(shader_version_parsing) {
    CHECK_EQ(parseGlslVersion("// comment\n/* block */\n#version 330 core\n").number, 330);
    CHECK(!parseGlslVersion("#version 330 core\n").compatibility);
    CHECK(parseGlslVersion("#version 150 compatibility\n").compatibility);
    CHECK(parseGlslVersion("void main() {}").compatibility);
    CHECK_EQ(parseGlslVersion("void main() {}").number, 110);
}

TEST(shader_transform_feedback_vertex_capture) {
    LinkInput in;
    in.shaders.push_back({Stage::Vertex,
                          "#version 330 core\nin vec4 pos; out vec3 a; flat out ivec2 b; out float c[3];\n"
                          "void main() { a = pos.xyz; b = ivec2(gl_VertexID); c[0] = 1.0; c[1] = 2.0; c[2] = 3.0; gl_Position = pos; }\n"});
    in.shaders.push_back({Stage::Fragment, kFs});
    in.transformFeedbackVaryings = {"gl_Position", "a", "b", "c[1]"};
    in.transformFeedbackMode = GL_INTERLEAVED_ATTRIBS;
    LinkResult r = link(in);
    CHECK(r.ok);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    CHECK_EQ(r.transformFeedback.size(), static_cast<size_t>(4));
    CHECK_EQ(r.xfbStride[0], 4u + 3u + 2u + 1u);
    CHECK(r.xfbVertexCapture);
    if (r.transformFeedback.size() == 4) {
        CHECK_EQ(r.transformFeedback[3].offset, 9u);
        CHECK(r.transformFeedback[3].type == GL_FLOAT && r.transformFeedback[3].size == 1);
    }
    const std::string& msl = r.stages[static_cast<int>(Stage::Vertex)].msl;
    CHECK(msl.find("[[buffer(30)]]") != std::string::npos);
    CHECK(msl.find("[[buffer(14)]]") != std::string::npos);

    in.transformFeedbackMode = GL_SEPARATE_ATTRIBS;
    in.transformFeedbackVaryings = {"a", "c"};  // c is 3 components: allowed separately
    r = link(in);
    CHECK(r.ok && r.xfbBufferCount == 2 && r.xfbStride[0] == 3 && r.xfbStride[1] == 3);
}

TEST(shader_transform_feedback_link_errors) {
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, "#version 330 core\nout vec4 a; out mat2 m; void main() { a = vec4(1); m = mat2(1); gl_Position = a; }\n"});
    in.shaders.push_back({Stage::Fragment, kFs});
    in.transformFeedbackVaryings = {"missing"};
    CHECK(!link(in).ok);
    in.transformFeedbackVaryings = {"a", "a"};
    CHECK(!link(in).ok);
    in.transformFeedbackVaryings = {"a[0]"};
    CHECK(!link(in).ok);
    in.transformFeedbackMode = GL_SEPARATE_ATTRIBS;
    in.transformFeedbackVaryings = {"m", "a"};  // m has 4 components: fine; 5 would not be
    CHECK(link(in).ok);
    LinkInput fragmentOnly;
    fragmentOnly.shaders.push_back({Stage::Fragment, kFs});
    fragmentOnly.transformFeedbackVaryings = {"color"};
    CHECK(!link(fragmentOnly).ok);
}

TEST(shader_geometry_emulation) {
    LinkInput in;
    in.shaders.push_back({Stage::Vertex,
                          "#version 330 core\nin vec4 pos; out VS_OUT { vec4 color; } vs_out; out float w;\n"
                          "void main() { vs_out.color = pos; w = 2.0; gl_Position = pos; }\n"});
    in.shaders.push_back({Stage::Geometry,
                          "#version 330 core\nlayout(triangles) in; layout(triangle_strip, max_vertices = 4) out;\n"
                          "in VS_OUT { vec4 color; } gs_in[]; in float w[];\nuniform float scale; uniform sampler2D tex;\n"
                          "out vec4 fcol; flat out int prim;\n"
                          "void emit(int i) { fcol = gs_in[i].color * texture(tex, vec2(0.5)) * w[i]; prim = gl_PrimitiveIDIn;\n"
                          "    gl_Position = gl_in[i].gl_Position * scale; EmitVertex(); }\n"
                          "void main() { for (int i = 0; i < 3; ++i) emit(i); EndPrimitive(); }\n"});
    in.shaders.push_back({Stage::Fragment,
                          "#version 330 core\nin vec4 fcol; flat in int prim; uniform vec4 tint; out vec4 color;\n"
                          "void main() { color = fcol * tint + float(prim); }\n"});
    in.transformFeedbackVaryings = {"fcol", "gl_Position"};
    LinkResult r = link(in);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    CHECK(r.ok);
    CHECK(r.geometry.active);
    CHECK_EQ(r.geometryInputType, static_cast<GLenum>(GL_TRIANGLES));
    CHECK_EQ(r.geometryOutputType, static_cast<GLenum>(GL_TRIANGLE_STRIP));
    CHECK_EQ(r.geometryVerticesOut, 4);
    CHECK_EQ(r.geometry.inputStride, 4u + 4u + 1u);   // gl_Position, VS_OUT.color, w
    CHECK_EQ(r.geometry.outputStride, 4u + 4u + 1u);  // gl_Position, fcol, prim
    CHECK(r.transformFeedback.size() == 2 && r.xfbStride[0] == 8u);
    CHECK(!r.geometry.kernel.msl.empty() && !r.geometry.passthrough.msl.empty());
    CHECK(r.geometry.kernel.msl.find("kernel void") != std::string::npos);
    const UniformInfo* tex = findUniform(r, "tex");
    CHECK(tex && tex->samplerSlot >= 0 && r.samplers[tex->samplerSlot].stageTexture[static_cast<int>(Stage::Compute)] == 0);
    CHECK(findUniform(r, "scale") != nullptr);
    CHECK(r.stages[static_cast<int>(Stage::Vertex)].mslNoRaster.find("[[buffer(14)]]") != std::string::npos);
}
