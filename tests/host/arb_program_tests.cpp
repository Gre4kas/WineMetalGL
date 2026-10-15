// ARB assembly programs (issue #25): parsing, error positions and the GLSL
// they translate to, linked by the shader compiler.
#include <cstring>
#include <string>

#include "core/compat/arb_program.h"
#include "core/shader/compiler.h"
#include "test.h"

using namespace wmgl::compat::arb;
using namespace wmgl::shader;

namespace {

std::shared_ptr<const Parsed> parseString(Kind kind, const std::string& s) { return parse(kind, s.data(), s.size()); }

// Links a vertex program's and a fragment program's GLSL; logs the source on failure.
bool links(const Parsed& vp, const Parsed& fp, const EmitOptions& options = {}) {
    LinkInput in;
    const std::string vs = emit(vp, options), fs = emit(fp, options);
    in.shaders.push_back({Stage::Vertex, vs});
    in.shaders.push_back({Stage::Fragment, fs});
    for (int n = 1; n < 16; ++n) in.attribBindings["arb_attrib" + std::to_string(n)] = static_cast<GLuint>(n);
    LinkResult r = link(in);
    if (!r.ok) std::fprintf(stderr, "  link failed:\n%s\n--- vertex\n%s--- fragment\n%s", r.log.c_str(), vs.c_str(), fs.c_str());
    return r.ok;
}

const char kPassVp[] =
    "!!ARBvp1.0\n"
    "ATTRIB pos = vertex.position;\n"
    "PARAM mvp[4] = { state.matrix.mvp };\n"
    "DP4 result.position.x, mvp[0], pos;\n"
    "DP4 result.position.y, mvp[1], pos;\n"
    "DP4 result.position.z, mvp[2], pos;\n"
    "DP4 result.position.w, mvp[3], pos;\n"
    "MOV result.color, vertex.color;\n"
    "MOV result.texcoord[0], vertex.texcoord[0];\n"
    "END\n";

const char kPassFp[] =
    "!!ARBfp1.0\n"
    "TEMP c;\n"
    "TEX c, fragment.texcoord[0], texture[0], 2D;\n"
    "MUL result.color, c, fragment.color;\n"
    "END\n";

// Doom 3's interaction programs, in their shape: generic attributes for the
// tangent frame, program.env light parameters, dependent texture reads,
// normalization cube maps and DP3 lighting.
const char kInteractionVp[] =
    "!!ARBvp1.0 OPTION ARB_position_invariant ;\n"
    "TEMP R0, R1, R2;\n"
    "PARAM defaultTexCoord = { 0, 0.5, 0, 1 };\n"
    "SUB R0, program.env[4], vertex.position;\n"
    "DP3 result.texcoord[0].x, vertex.attrib[9], R0;\n"
    "DP3 result.texcoord[0].y, vertex.attrib[10], R0;\n"
    "DP3 result.texcoord[0].z, vertex.attrib[11], R0;\n"
    "MOV result.texcoord[1], defaultTexCoord;\n"
    "DP4 result.texcoord[1].x, vertex.attrib[8], program.env[10];\n"
    "DP4 result.texcoord[1].y, vertex.attrib[8], program.env[11];\n"
    "DP4 result.texcoord[3].x, vertex.position, program.env[9];\n"
    "DP4 result.texcoord[2].x, vertex.position, program.env[6];\n"
    "DP4 result.texcoord[2].y, vertex.position, program.env[7];\n"
    "DP4 result.texcoord[2].w, vertex.position, program.env[8];\n"
    "SUB R0, program.env[5], vertex.position;\n"
    "DP3 R1.x, R0, R0;\n"
    "RSQ R1.x, R1.x;\n"
    "MUL R0, R0, R1.x;\n"
    "SUB R1, program.env[4], vertex.position;\n"
    "DP3 R2.x, R1, R1;\n"
    "RSQ R2.x, R2.x;\n"
    "MAD R1, R1, R2.x, R0;\n"
    "DP3 result.texcoord[6].x, vertex.attrib[9], R1;\n"
    "DP3 result.texcoord[6].y, vertex.attrib[10], R1;\n"
    "DP3 result.texcoord[6].z, vertex.attrib[11], R1;\n"
    "MAD result.color, vertex.color, program.env[16], program.env[17];\n"
    "END\n";

const char kInteractionFp[] =
    "!!ARBfp1.0 \n"
    "OPTION ARB_precision_hint_fastest;\n"
    "PARAM subOne = { -1, -1, -1, -1 };\n"
    "PARAM scaleTwo = { 2, 2, 2, 2 };\n"
    "TEMP light, color, R1, R2, localNormal, specular;\n"
    "TEX light, fragment.texcoord[0], texture[0], CUBE;\n"
    "MAD light, light, scaleTwo, subOne;\n"
    "TEX localNormal, fragment.texcoord[1], texture[1], 2D;\n"
    "MOV localNormal.x, localNormal.a;\n"
    "MAD localNormal, localNormal, scaleTwo, subOne;\n"
    "DP3 light, light, localNormal;\n"
    "TXP R1, fragment.texcoord[2], texture[2], 2D;\n"
    "MUL light, light, R1;\n"
    "TEX R1, fragment.texcoord[3], texture[3], 2D;\n"
    "MUL light, light, R1;\n"
    "TEX R1, fragment.texcoord[6], texture[4], CUBE;\n"
    "MAD R1, R1, scaleTwo, subOne;\n"
    "DP3_SAT specular, localNormal, R1;\n"
    "POW specular.x, specular.x, program.env[0].x;\n"
    "MUL color, light, program.local[0];\n"
    "MUL R2, specular.x, program.env[1];\n"
    "MAD color, R2, light, color;\n"
    "MUL result.color, color, fragment.color;\n"
    "END\n";

}  // namespace

TEST(arb_parse_minimal_programs) {
    auto vp = parseString(Kind::Vertex, kPassVp);
    auto fp = parseString(Kind::Fragment, kPassFp);
    CHECK(vp->ok);
    CHECK(fp->ok);
    CHECK_EQ(vp->errorPosition, -1);
    CHECK_EQ(vp->instructions, 6);
    CHECK_EQ(fp->instructions, 2);
    CHECK_EQ(fp->texInstructions, 1);
    CHECK(fp->textures[0] == TexTarget::T2D);
    CHECK_EQ(fp->texCoordRead, 1u);
    CHECK(fp->colorRead[0]);
    CHECK(vp->positionWritten);
    CHECK(links(*vp, *fp));
}

TEST(arb_parse_reports_error_positions) {
    const std::string bad = "!!ARBvp1.0\nMOV result.position, nothing;\nEND\n";
    auto p = parseString(Kind::Vertex, bad);
    CHECK(!p->ok);
    CHECK_EQ(p->errorPosition, static_cast<int>(bad.find("nothing")));
    CHECK(p->error.find("nothing") != std::string::npos);

    auto header = parseString(Kind::Fragment, "!!ARBvp1.0\nEND\n");
    CHECK(!header->ok);
    CHECK_EQ(header->errorPosition, 0);

    const std::string noEnd = "!!ARBfp1.0\nMOV result.color, fragment.color;\n";
    auto e = parseString(Kind::Fragment, noEnd);
    CHECK(!e->ok);
    CHECK_EQ(e->errorPosition, static_cast<int>(noEnd.size()));

    // Vertex-only and fragment-only instructions and bindings.
    CHECK(!parseString(Kind::Fragment, "!!ARBfp1.0\nTEMP a;\nEXP a, fragment.color.x;\nEND\n")->ok);
    CHECK(!parseString(Kind::Vertex, "!!ARBvp1.0\nTEMP a;\nTEX a, vertex.position, texture[0], 2D;\nEND\n")->ok);
    CHECK(!parseString(Kind::Vertex, "!!ARBvp1.0\nMOV_SAT result.color, vertex.color;\nEND\n")->ok);
    // Scalar instructions need a scalar suffix; names are declared once; one target per unit.
    CHECK(!parseString(Kind::Fragment, "!!ARBfp1.0\nTEMP a;\nRCP a, fragment.color;\nEND\n")->ok);
    CHECK(!parseString(Kind::Fragment, "!!ARBfp1.0\nTEMP a;\nTEMP a;\nEND\n")->ok);
    CHECK(!parseString(Kind::Fragment,
                       "!!ARBfp1.0\nTEMP a;\nTEX a, fragment.texcoord, texture[1], 2D;\n"
                       "TEX a, fragment.texcoord, texture[1], CUBE;\nEND\n")->ok);
    // Write masks keep xyzw order; position-invariant programs do not write the position.
    CHECK(!parseString(Kind::Vertex, "!!ARBvp1.0\nTEMP a;\nMOV a.yx, vertex.position;\nEND\n")->ok);
    CHECK(!parseString(Kind::Vertex,
                       "!!ARBvp1.0\nOPTION ARB_position_invariant;\nMOV result.position, vertex.position;\nEND\n")->ok);
    // Comments, and anything after END, are ignored.
    CHECK(parseString(Kind::Fragment, "!!ARBfp1.0 # hi\nMOV result.color, fragment.color; # there\nEND\ngarbage(")->ok);
}

TEST(arb_doom3_interaction_links) {
    auto vp = parseString(Kind::Vertex, kInteractionVp);
    auto fp = parseString(Kind::Fragment, kInteractionFp);
    CHECK(vp->ok);
    if (!vp->ok) std::fprintf(stderr, "  vp: %d %s\n", vp->errorPosition, vp->error.c_str());
    CHECK(fp->ok);
    if (!fp->ok) std::fprintf(stderr, "  fp: %d %s\n", fp->errorPosition, fp->error.c_str());
    if (!vp->ok || !fp->ok) return;
    CHECK(vp->positionInvariant);
    CHECK_EQ(vp->envCount, 18);
    CHECK_EQ(vp->genericRead, (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11));
    CHECK(fp->textures[0] == TexTarget::Cube);
    CHECK(fp->textures[4] == TexTarget::Cube);
    CHECK_EQ(fp->localCount, 1);
    CHECK(links(*vp, *fp));
    EmitOptions o;
    o.flat = true;
    o.twoSide = true;
    o.clipPlanes = 3;
    CHECK(links(*vp, *fp, o));
}

TEST(arb_every_instruction_links) {
    const std::string vp =
        "!!ARBvp1.0\n"
        "ADDRESS A0;\n"
        "TEMP a, b;\n"
        "PARAM c[] = { program.env[0..3], { 1, 2, 3, 4 }, state.matrix.modelview.inverse.row[1..2] };\n"
        "PARAM half = 0.5;\n"
        "PARAM lights[2] = { state.light[0].position, state.light[1].half };\n"
        "OUTPUT oc = result.color.back.secondary;\n"
        "ALIAS t = a;\n"
        "ARL A0.x, vertex.attrib[1].x;\n"
        "MOV a, c[A0.x + 2];\n"
        "MOV b, c[A0.x - 1];\n"
        "ABS t, -a.wzyx;\n"
        "ADD a, a, b;\n"
        "DP3 a.x, a, b;\n"
        "DPH a.y, a, b;\n"
        "DST b, a, b;\n"
        "EX2 a.z, b.y;\n"
        "EXP b, a.x;\n"
        "FLR a, b;\n"
        "FRC a, a;\n"
        "LG2 a.w, b.z;\n"
        "LIT b, a;\n"
        "LOG a, b.w;\n"
        "MAD a, a, half, c[4];\n"
        "MAX a, a, lights[1];\n"
        "MIN a, a, state.material.front.shininess;\n"
        "MUL a, a, state.lightprod[0].back.diffuse;\n"
        "POW a.x, a.x, b.y;\n"
        "RCP a.y, a.z;\n"
        "RSQ a.z, a.w;\n"
        "SGE b, a, state.lightmodel.back.scenecolor;\n"
        "SLT b, a, state.texgen[2].eye.q;\n"
        "SUB b, b, state.matrix.texture[1].invtrans.row[3];\n"
        "SWZ a, b, -x, 0, 1, w;\n"
        "XPD a, a, state.fog.params;\n"
        "MUL a, a, state.matrix.program[2].inverse.row[0];\n"
        "MUL a, a, state.point.attenuation;\n"
        "MUL a, a, state.clip[3].plane;\n"
        "MUL a, a, state.depth.range;\n"
        "MUL a, a, state.light[2].spot.direction;\n"
        "MOV result.position, vertex.position;\n"
        "MOV oc, a;\n"
        "MOV result.fogcoord.x, a;\n"
        "MOV result.pointsize, b;\n"
        "MOV result.color.front.primary, vertex.color.secondary;\n"
        "MOV result.texcoord[7], vertex.normal;\n"
        "MOV result.texcoord[1], vertex.fogcoord;\n"
        "END\n";
    const std::string fp =
        "!!ARBfp1.0\n"
        "OPTION ARB_fog_exp2;\n"
        "TEMP a, b;\n"
        "PARAM p[2] = { program.local[3..4] };\n"
        "CMP a, fragment.color.secondary, p[0], p[1];\n"
        "COS a.x, a.y;\n"
        "SIN a.y, a.z;\n"
        "SCS b.xy, a.x;\n"
        "LRP_SAT a, a, b, fragment.position;\n"
        "KIL -a;\n"
        "TXB b, fragment.texcoord[1], texture[1], 3D;\n"
        "TXP a, fragment.texcoord[2], texture[2], RECT;\n"
        "TEX b.rg, fragment.fogcoord, texture[3], 1D;\n"
        "MAD a, a, b, state.texenv[2].color;\n"
        "MUL a, a, state.fog.color;\n"
        "MOV result.color, a.bgra;\n"
        "MOV result.depth.z, b;\n"
        "END\n";
    auto v = parseString(Kind::Vertex, vp);
    auto f = parseString(Kind::Fragment, fp);
    CHECK(v->ok);
    if (!v->ok) std::fprintf(stderr, "  vp: %d %s: %s\n", v->errorPosition, v->error.c_str(), vp.c_str() + v->errorPosition);
    CHECK(f->ok);
    if (!f->ok) std::fprintf(stderr, "  fp: %d %s: %s\n", f->errorPosition, f->error.c_str(), fp.c_str() + f->errorPosition);
    if (!v->ok || !f->ok) return;
    CHECK_EQ(v->programMatrices, 4u);
    CHECK_EQ(v->programMatrixInverses, 4u);
    CHECK_EQ(v->envCount, 4);
    CHECK_EQ(f->localCount, 5);
    CHECK_EQ(f->fog, static_cast<int>(GL_EXP2));
    CHECK(f->kills);
    CHECK(f->depthWritten);
    CHECK(links(*v, *f));
}

TEST(arb_shadow_and_draw_buffers_options) {
    const std::string fp =
        "!!ARBfp1.0\n"
        "OPTION ARB_fragment_program_shadow;\n"
        "OPTION ARB_draw_buffers;\n"
        "TEMP s;\n"
        "TEX s, fragment.texcoord[0], texture[0], SHADOW2D;\n"
        "MOV result.color[0], s;\n"
        "MOV result.color[1], fragment.color;\n"
        "END\n";
    auto f = parseString(Kind::Fragment, fp);
    CHECK(f->ok);
    if (!f->ok) return;
    CHECK(f->textures[0] == TexTarget::Shadow2D);
    CHECK_EQ(f->colorOutputs, 3u);
    auto v = parseString(Kind::Vertex, kPassVp);
    CHECK(links(*v, *f));
    // Without the option SHADOW2D is not a target.
    CHECK(!parseString(Kind::Fragment, "!!ARBfp1.0\nTEMP s;\nTEX s, fragment.texcoord, texture, SHADOW2D;\nEND\n")->ok);
}

TEST(arb_vertex_inputs) {
    // vertex.attrib[0] is the position; other generic attributes are inputs
    // bound to their index (DrawState::attribFor aliases the arrays).
    const std::string vp =
        "!!ARBvp1.0\nOPTION ARB_position_invariant;\n"
        "ADD result.color, vertex.normal, vertex.attrib[0];\nMOV result.texcoord[0], vertex.attrib[8];\nEND\n";
    auto v = parseString(Kind::Vertex, vp);
    CHECK(v->ok);
    if (!v->ok) return;
    const std::string glsl = emit(*v, EmitOptions{});
    CHECK(glsl.find("vec4(gl_Normal, 1.0)") != std::string::npos);
    CHECK(glsl.find("gl_Vertex") != std::string::npos);
    CHECK(glsl.find("in vec4 arb_attrib8;") != std::string::npos);
    CHECK(glsl.find("arb_attrib0") == std::string::npos);
    CHECK_EQ(v->conventionalRead, static_cast<uint32_t>(kInPosition | kInNormal));
    CHECK_EQ(v->genericRead, 1u << 8);
    auto f = parseString(Kind::Fragment, kPassFp);
    CHECK(links(*v, *f));
}
