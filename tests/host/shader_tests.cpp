#include <algorithm>
#include <cstdio>
#include <set>

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

TEST(shader_legacy_macros) {
    // Built-ins and legacy functions inside #define bodies are rewritten too.
    LinkInput in;
    in.shaders.push_back({Stage::Vertex,
                          "#version 120\n#define MVP gl_ModelViewProjectionMatrix\nvarying vec2 v;\n"
                          "void main() { v = gl_MultiTexCoord0.xy; gl_Position = MVP * gl_Vertex; }\n"});
    in.shaders.push_back({Stage::Fragment,
                          "#version 120\n#define TEX(s, c) texture2D(s, c)\n#define OUT(x) \\\n    gl_FragColor = x\n"
                          "varying vec2 v; uniform sampler2D t;\nvoid main() { OUT(TEX(t, v)); }\n"});
    LinkResult r = link(in);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    CHECK(r.ok);
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
    CHECK(msl.find("[[buffer(16)]]") != std::string::npos);

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
    CHECK(r.stages[static_cast<int>(Stage::Vertex)].mslNoRaster.find("[[buffer(16)]]") != std::string::npos);
}

TEST(shader_fragment_point_coord_and_flat) {
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, "#version 330 core\nin vec4 pos; flat out int id; void main() { id = 1; gl_Position = pos; }\n"});
    in.shaders.push_back({Stage::Fragment, "#version 330 core\nflat in int id; out vec4 color;\n"
                                           "void main() { color = vec4(gl_PointCoord, float(id), 1.0); }\n"});
    LinkResult r = link(in);
    CHECK(r.ok);
    const StageOutput& fs = r.stages[static_cast<int>(Stage::Fragment)];
    CHECK(fs.usesPointCoord && fs.flatInputs);
    CHECK(fs.msl.find("wmgl_fs_driver[0]") != std::string::npos);
    in.shaders[1].source = "#version 330 core\nout vec4 color; void main() { color = vec4(1.0); }\n";
    r = link(in);
    CHECK(r.ok && !r.stages[static_cast<int>(Stage::Fragment)].usesPointCoord && !r.stages[static_cast<int>(Stage::Fragment)].flatInputs);
}

TEST(shader_instance_id_excludes_base_instance) {
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, "#version 330 core\nflat out int id; void main() { id = gl_InstanceID; gl_Position = vec4(0.0); }\n"});
    in.shaders.push_back({Stage::Fragment, "#version 330 core\nflat in int id; out vec4 color; void main() { color = vec4(float(id)); }\n"});
    LinkResult r = link(in);
    CHECK(r.ok);
    // GL's gl_InstanceID does not include the base instance (GL_ARB_base_instance).
    const std::string& msl = r.stages[static_cast<int>(Stage::Vertex)].msl;
    CHECK(msl.find("[[base_instance]]") != std::string::npos);
}

TEST(shader_explicit_attrib_location_beats_binding) {
    // layout(location) wins over glBindAttribLocation, and stays reserved.
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, "#version 330 core\nlayout(location = 0) in vec4 pos; in vec4 color; out vec4 c;\n"
                                         "void main() { c = color; gl_Position = pos; }\n"});
    in.shaders.push_back({Stage::Fragment, "#version 330 core\nin vec4 c; out vec4 o; void main() { o = c; }\n"});
    in.attribBindings["pos"] = 5;
    LinkResult r = link(in);
    CHECK(r.ok);
    CHECK(findAttrib(r, "pos") && findAttrib(r, "pos")->location == 0);
    CHECK(findAttrib(r, "color") && findAttrib(r, "color")->location == 1);
}

TEST(shader_fragment_output_explicit_index) {
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, "#version 330 core\nin vec4 pos; void main() { gl_Position = pos; }\n"});
    in.shaders.push_back({Stage::Fragment, "#version 330 core\nlayout(location = 0, index = 0) out vec4 o0;\n"
                                           "layout(location = 0, index = 1) out vec4 o1;\n"
                                           "void main() { o0 = vec4(1.0); o1 = vec4(0.5); }\n"});
    LinkResult r = link(in);
    CHECK(r.ok);
    bool sawIndex1 = false;
    for (const FragOutputInfo& f : r.fragOutputs) {
        if (f.name == "o0") CHECK(f.location == 0 && f.index == 0);
        if (f.name == "o1") sawIndex1 = f.location == 0 && f.index == 1;
    }
    CHECK(sawIndex1);
}

TEST(shader_struct_sampler_members) {
    // glslang moves samplers out of struct uniforms; its place holder is not a uniform.
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, "#version 330 core\nin vec4 pos; out vec2 uv; void main() { uv = pos.xy; gl_Position = pos; }\n"});
    in.shaders.push_back({Stage::Fragment, "#version 330 core\nstruct S { sampler2D t; float k; };\nuniform S s;\nin vec2 uv; out vec4 o;\n"
                                           "void main() { o = texture(s.t, uv) * s.k; }\n"});
    LinkResult r = link(in);
    CHECK(r.ok);
    CHECK_EQ(r.uniforms.size(), size_t(2));
    for (const UniformInfo& u : r.uniforms) CHECK(u.name.find("/*") == std::string::npos);
    const UniformInfo* k = findUniform(r, "s.k");
    const UniformInfo* t = findUniform(r, "s.t");
    CHECK(k && k->samplerSlot < 0);
    CHECK(t && t->samplerSlot >= 0);
}

TEST(shader_legacy_declarations_outside_conditionals) {
    // Declarations must not land inside a compiled-out #ifdef block.
    const char* fs = "#version 120\n#ifdef GL_ES\nprecision mediump float;\n#endif\nvarying vec4 vv;\n"
                     "void main() { gl_FragColor = vv; }\n";
    UpgradeResult u = upgradeLegacyGlsl(Stage::Fragment, fs);
    CHECK(u.ok);
    CHECK(u.source.find("out vec4 wmgl_FragColor;") < u.source.find("#ifdef GL_ES"));
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, "attribute vec4 pos; varying vec4 vv; void main() { gl_Position = pos; vv = pos; }\n"});
    in.shaders.push_back({Stage::Fragment, fs});
    LinkResult r = link(in);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    CHECK(r.ok);
}

TEST(shader_legacy_redeclared_builtin_varyings) {
    // GLSL 1.10 lets shaders size gl_TexCoord; 1.30 adds interpolation qualifiers.
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, "#version 120\nvarying vec4 gl_TexCoord[2];\n"
                                         "void main() { gl_Position = ftransform(); gl_TexCoord[0] = gl_MultiTexCoord0; }\n"});
    in.shaders.push_back({Stage::Fragment, "#version 120\nuniform sampler2D t;\nvarying vec4 gl_TexCoord[2];\n"
                                           "void main() { gl_FragColor = texture2D(t, gl_TexCoord[0].xy); }\n"});
    LinkResult r = link(in);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    CHECK(r.ok);
    LinkInput flat;
    flat.shaders.push_back({Stage::Vertex, "#version 130\nflat out vec4 gl_FrontColor;\n"
                                           "void main() { gl_Position = ftransform(); gl_FrontColor = gl_Color; }\n"});
    flat.shaders.push_back({Stage::Fragment, "#version 130\nflat in vec4 gl_Color;\nvoid main() { gl_FragColor = gl_Color; }\n"});
    r = link(flat);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    CHECK(r.ok);
    CHECK(r.ok && r.stages[static_cast<int>(Stage::Fragment)].flatInputs);
}

TEST(shader_legacy_helpers_in_several_objects) {
    // Generated helpers must not be defined once per shader object of a stage.
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, "#version 120\nvec4 xf() { return ftransform(); }\n"});
    in.shaders.push_back({Stage::Vertex, "#version 120\nvec4 xf();\n"
                                         "void main() { gl_Position = xf() + ftransform(); gl_TexCoord[0] = gl_MultiTexCoord0; }\n"});
    in.shaders.push_back({Stage::Fragment, "#version 120\nuniform sampler2DShadow sh;\n"
                                           "vec4 light() { return gl_LightSource[0].diffuse * shadow2D(sh, vec3(0.5)); }\n"});
    in.shaders.push_back({Stage::Fragment, "#version 120\nuniform sampler2DShadow sh; uniform sampler2D t;\nvec4 light();\n"
                                           "void main() { gl_FragColor = light() + shadow2DProj(sh, vec4(texture2D(t, gl_TexCoord[0].xy).xy, "
                                           "0.5, 1.0)).r * vec4(1.0); }\n"});
    LinkResult r = link(in);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    CHECK(r.ok);
    UpgradeResult u = upgradeLegacyGlsl(Stage::Fragment, "#version 120\nuniform sampler2DShadow s;\n"
                                                         "void main() { gl_FragColor = shadow2D(s, vec3(f(1.0), 0.5, 0.5)); }\n");
    CHECK(u.source.find("vec4(texture(s, vec3(f(1.0), 0.5, 0.5)))") != std::string::npos);
}

TEST(shader_separable_block_varyings_match_by_block_name) {
    auto locations = [](const std::string& msl) {
        std::set<std::string> found;
        for (size_t at = msl.find("[[user(locn"); at != std::string::npos; at = msl.find("[[user(locn", at + 1))
            found.insert(msl.substr(at, msl.find(')', at) - at));
        return found;
    };
    LinkInput vs;
    vs.separable = true;
    vs.shaders.push_back({Stage::Vertex, "#version 330 core\nin vec4 pos; out VS_OUT { vec4 c; } vs_out;\n"
                                         "void main() { gl_Position = pos; vs_out.c = pos; }\n"});
    LinkInput fs;
    fs.separable = true;
    fs.shaders.push_back({Stage::Fragment, "#version 330 core\nin VS_OUT { vec4 c; } fs_in; out vec4 o; void main() { o = fs_in.c; }\n"});
    LinkResult v = link(vs), f = link(fs);
    CHECK(v.ok && f.ok);
    CHECK(!locations(f.stages[static_cast<int>(Stage::Fragment)].msl).empty());
    CHECK(locations(v.stages[static_cast<int>(Stage::Vertex)].msl) == locations(f.stages[static_cast<int>(Stage::Fragment)].msl));
    // With a geometry stage, the pass-through stage declares the same hashed locations.
    LinkInput vg = vs;
    vg.shaders.push_back({Stage::Geometry, "#version 330 core\nlayout(triangles) in; layout(triangle_strip, max_vertices = 3) out;\n"
                                           "in VS_OUT { vec4 c; } gs_in[]; out VS_OUT { vec4 c; } gs_out;\n"
                                           "void main() { for (int i = 0; i < 3; ++i) { gs_out.c = gs_in[i].c; "
                                           "gl_Position = gl_in[i].gl_Position; EmitVertex(); } }\n"});
    LinkResult g = link(vg);
    if (!g.ok) std::fprintf(stderr, "%s\n", g.log.c_str());
    CHECK(g.ok && g.geometry.active);
    CHECK(g.ok && locations(g.geometry.passthrough.msl) == locations(f.stages[static_cast<int>(Stage::Fragment)].msl));
}

TEST(shader_glsl420_pack_rules) {
    // invariant inputs outside the vertex stage, in any qualifier order.
    CHECK(compile(Stage::Fragment, "#version 420\nhighp in layout(location = 1) smooth invariant vec4 v;\n"
                                   "out vec4 o; void main() { o = v; }\n").ok);
    CHECK(!compile(Stage::Vertex, "#version 420\ninvariant in vec4 v; void main() { gl_Position = v; }\n").ok);
    // Image bindings name image units; block bindings stay below GL_MAX_UNIFORM_BUFFER_BINDINGS.
    CHECK(compile(Stage::Fragment, "#version 420\nlayout(binding = 7, rgba8) readonly uniform image2D i;\n"
                                   "out vec4 o; void main() { o = imageLoad(i, ivec2(0)); }\n").ok);
    CHECK(!compile(Stage::Fragment, "#version 420\nlayout(binding = 8, rgba8) readonly uniform image2D i;\n"
                                    "out vec4 o; void main() { o = imageLoad(i, ivec2(0)); }\n").ok);
    CHECK(compile(Stage::Fragment, "#version 420\nlayout(std140, binding = 70) uniform B { vec4 a; } b[2];\n"
                                   "out vec4 o; void main() { o = b[1].a; }\n").ok);
    CHECK(!compile(Stage::Fragment, "#version 420\nlayout(std140, binding = 70) uniform B { vec4 a; } b[3];\n"
                                    "out vec4 o; void main() { o = b[2].a; }\n").ok);
}

TEST(shader_fourteen_uniform_blocks_per_stage) {
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, "#version 420\nlayout(std140, binding = 2) uniform G { vec4 a; } g[14];\n"
                                         "void main() { vec4 s = vec4(0); for (int i = 0; i < 14; ++i) s += g[i].a;\n"
                                         "gl_Position = s; }\n"});
    in.shaders.push_back({Stage::Fragment, "#version 420\nout vec4 o; void main() { o = vec4(1); }\n"});
    LinkResult r = link(in);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    CHECK(r.ok && r.blocks.size() == 14);
    if (r.blocks.size() == 14) CHECK_EQ(r.blocks[13].stageSlot[0], static_cast<int>(kSlotFirstUniformBlock + 13));
}

TEST(shader_rectangle_texture_outside_fragment_stages) {
    // Translated back to GLSL for the tessellation control kernel, which has no textureLod for rectangles.
    LinkInput in;
    in.shaders.push_back({Stage::Vertex, "#version 420\nout vec4 v; void main() { v = vec4(1); gl_Position = vec4(0); }\n"});
    in.shaders.push_back({Stage::TessControl, "#version 420\nlayout(vertices = 1) out; in vec4 v[]; out vec4 c[];\n"
                                              "uniform sampler2DRect r;\n"
                                              "void main() { c[gl_InvocationID] = v[gl_InvocationID] + texture(r, vec2(0)) +\n"
                                              "texelFetch(r, ivec2(0)); gl_TessLevelOuter[0] = gl_TessLevelOuter[1] = 1.0; }\n"});
    in.shaders.push_back({Stage::TessEval, "#version 420\nlayout(isolines, point_mode) in; in vec4 c[]; out vec4 e;\n"
                                           "void main() { e = c[0]; gl_Position = vec4(0); }\n"});
    in.shaders.push_back({Stage::Fragment, "#version 420\nin vec4 e; out vec4 o; void main() { o = e; }\n"});
    LinkResult r = link(in);
    if (!r.ok) std::fprintf(stderr, "%s\n", r.log.c_str());
    CHECK(r.ok);
}
