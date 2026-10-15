// Compatibility profile state (W22): matrix stacks, lighting, materials,
// texture environment and coordinate generation, fog, alpha test, user clip
// planes, point parameters, raster position and display-list bookkeeping.
//
// The state feeds both generated fixed-function shaders and the lowered
// legacy built-ins of application shaders (gl_ModelViewMatrix, ...) through
// the wmgl_FixedFunction uniform block (core/compat/fixed_function.h).
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "core/gl/types.h"

namespace wmgl {

struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // column major
    static Mat4 identity() { return Mat4(); }
    Mat4 operator*(const Mat4& b) const;
    void transform(const float in[4], float out[4]) const;
    Mat4 inverse() const;  // identity if singular
    Mat4 transpose() const;
};

// Conventional vertex arrays live after the 16 generic attributes in a
// vertex array object; the lowered built-ins (wmgl_Vertex, ...) are bound to
// these attribute locations (shader/glsl_upgrade.h).
enum ConventionalAttrib : int {
    kConvVertex = 16, kConvNormal, kConvColor, kConvSecondaryColor, kConvFogCoord, kConvTexCoord0,
    kConvEnd = kConvTexCoord0 + 8
};
// Attribute slots with current values (generic + conventional).
constexpr int kAttribSlotCount = kConvEnd;
// Two internal slots follow: wide lines drawn by the fixed-function program
// read the other endpoint of the segment and the side of the line each
// expanded vertex is on (DrawState::drawWideLines).
enum InternalAttrib : int { kLinePartner = kConvEnd, kLineSide, kInternalAttribEnd };

struct FixedState {
    static constexpr int kTextureUnits = 8, kLights = 8, kClipPlanes = 8;
    static constexpr int kModelViewDepth = 32, kProjectionDepth = 4, kTextureDepth = 4, kColorDepth = 2;
    // ARB program matrices (MATRIX0_ARB..): MAX_PROGRAM_MATRICES_ARB and
    // MAX_PROGRAM_MATRIX_STACK_DEPTH_ARB.
    static constexpr int kProgramMatrices = 8, kProgramMatrixDepth = 4;

    GLenum matrixMode = GL_MODELVIEW;
    std::vector<Mat4> modelview{Mat4()}, projection{Mat4()};
    // The imaging subset's colour matrix (GL_COLOR): kept so applications
    // that save and restore it (the Steam overlay) round-trip, but it does
    // not transform pixels, as ARB_imaging is not advertised.
    std::vector<Mat4> color{Mat4()};
    std::array<std::vector<Mat4>, kTextureUnits> texture;
    std::array<std::vector<Mat4>, kProgramMatrices> programMatrix;

    struct Light {
        bool enabled = false;
        float ambient[4] = {0, 0, 0, 1}, diffuse[4] = {0, 0, 0, 1}, specular[4] = {0, 0, 0, 1};
        float position[4] = {0, 0, 1, 0};   // eye space
        float spotDirection[3] = {0, 0, -1};  // eye space
        float spotExponent = 0, spotCutoff = 180;
        float attenuation[3] = {1, 0, 0};   // constant, linear, quadratic
    };
    struct Material {
        float ambient[4] = {0.2f, 0.2f, 0.2f, 1}, diffuse[4] = {0.8f, 0.8f, 0.8f, 1};
        float specular[4] = {0, 0, 0, 1}, emission[4] = {0, 0, 0, 1};
        float shininess = 0;
    };
    bool lighting = false;
    std::array<Light, kLights> lights;
    float lightModelAmbient[4] = {0.2f, 0.2f, 0.2f, 1};
    bool lightModelLocalViewer = false, lightModelTwoSide = false;
    GLenum lightModelColorControl = GL_SINGLE_COLOR;
    Material material[2];  // front, back
    bool colorMaterial = false;
    GLenum colorMaterialFace = GL_FRONT_AND_BACK, colorMaterialMode = GL_AMBIENT_AND_DIFFUSE;
    GLenum shadeModel = GL_SMOOTH;
    bool normalize = false, rescaleNormal = false;
    bool colorSum = false;  // GL_COLOR_SUM: add the secondary colour

    struct TexEnv {
        GLenum mode = GL_MODULATE;
        float color[4] = {0, 0, 0, 0};
        GLenum combineRGB = GL_MODULATE, combineAlpha = GL_MODULATE;
        GLenum sourceRGB[3] = {GL_TEXTURE, GL_PREVIOUS, GL_CONSTANT};
        GLenum sourceAlpha[3] = {GL_TEXTURE, GL_PREVIOUS, GL_CONSTANT};
        GLenum operandRGB[3] = {GL_SRC_COLOR, GL_SRC_COLOR, GL_SRC_ALPHA};
        GLenum operandAlpha[3] = {GL_SRC_ALPHA, GL_SRC_ALPHA, GL_SRC_ALPHA};
        float scaleRGB = 1, scaleAlpha = 1;
        float lodBias = 0;
        bool coordReplace = false;  // point sprites
    };
    struct TexGen {
        uint8_t enabled = 0;  // bit per coordinate S, T, R, Q
        GLenum mode[4] = {GL_EYE_LINEAR, GL_EYE_LINEAR, GL_EYE_LINEAR, GL_EYE_LINEAR};
        float objectPlane[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
        float eyePlane[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};  // eye space
    };
    // Enabled texture targets per unit (TEXTURE_1D ... TEXTURE_RECTANGLE).
    enum TexTargetBit : uint8_t { kTex1D = 1, kTex2D = 2, kTex3D = 4, kTexCube = 8, kTexRect = 16 };
    std::array<uint8_t, kTextureUnits> textureEnables{};
    std::array<TexEnv, kTextureUnits> texEnv;
    std::array<TexGen, kTextureUnits> texGen;

    bool fog = false;
    GLenum fogMode = GL_EXP, fogCoordSource = GL_FRAGMENT_DEPTH;
    float fogDensity = 1, fogStart = 0, fogEnd = 1, fogColor[4] = {0, 0, 0, 0};

    bool alphaTest = false;
    GLenum alphaFunc = GL_ALWAYS;
    float alphaRef = 0;

    float clipPlanes[kClipPlanes][4] = {};  // eye space; enables are ContextState::clipDistanceEnables

    float pointSizeMin = 0, pointSizeMax = 255, pointDistanceAttenuation[3] = {1, 0, 0};
    bool pointSprite = false, pointSmooth = false;
    bool lineStipple = false, polygonStipple = false;
    // ARB assembly programs (compat/arb_programs.h): VERTEX_PROGRAM_ARB,
    // FRAGMENT_PROGRAM_ARB and VERTEX_PROGRAM_TWO_SIDE (back colours of
    // vertex programs and shaders).
    bool vertexProgram = false, fragmentProgram = false, vertexProgramTwoSide = false;
    GLint lineStippleFactor = 1;
    GLushort lineStipplePattern = 0xffff;
    // Polygon stipple rows from the bottom; bit x is window column x mod 32.
    std::array<uint32_t, 32> stipple;

    GLuint clientActiveTexture = 0;
    bool edgeFlag = true;

    struct RasterPos {
        float window[4] = {0, 0, 0, 1};
        bool valid = true;
        float color[4] = {1, 1, 1, 1};
        float texCoord[4] = {0, 0, 0, 1};
        float distance = 0;
    } rasterPos;
    float pixelZoom[2] = {1, 1};

    GLenum hintPerspective = GL_DONT_CARE, hintPointSmooth = GL_DONT_CARE, hintFog = GL_DONT_CARE;
    float accumClear[4] = {0, 0, 0, 0};
    GLuint listBase = 0;
    // Selection and feedback (glRenderMode): modes are tracked, hit and
    // feedback records are not produced (nothing is drawn outside GL_RENDER).
    GLenum renderMode = GL_RENDER;
    std::vector<GLuint> nameStack;
    uint64_t version = 1;  // bumped by every change (uniform block caching)

    FixedState() {
        stipple.fill(~0u);
        for (auto& stack : texture) stack.assign(1, Mat4());
        for (auto& stack : programMatrix) stack.assign(1, Mat4());
        float one[4] = {1, 1, 1, 1};
        std::copy(one, one + 4, lights[0].diffuse);
        std::copy(one, one + 4, lights[0].specular);
    }
    std::vector<Mat4>& currentStack(GLuint activeTexture);
    Mat4& top(GLuint activeTexture) { return currentStack(activeTexture).back(); }
};

}  // namespace wmgl
