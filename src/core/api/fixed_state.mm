// Compatibility profile state commands (W22): matrices, lighting, materials,
// texture environment and coordinate generation, fog, alpha test, user clip
// planes and point parameters (GL 3.3 compatibility profile, chapters 2-3).

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/compat/compat_api.h"
#include "core/objects.h"
#include "core/compat/fixed_state.h"
#include "core/context.h"
#include "gl_api.gen.h"

namespace wmgl {

Mat4 Mat4::operator*(const Mat4& b) const {
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float sum = 0;
            for (int k = 0; k < 4; ++k) sum += m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = sum;
        }
    return r;
}

void Mat4::transform(const float in[4], float out[4]) const {
    for (int row = 0; row < 4; ++row)
        out[row] = m[row] * in[0] + m[4 + row] * in[1] + m[8 + row] * in[2] + m[12 + row] * in[3];
}

Mat4 Mat4::transpose() const {
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) r.m[c * 4 + row] = m[row * 4 + c];
    return r;
}

Mat4 Mat4::inverse() const {
    // Cofactor expansion (MESA gluInvertMatrix).
    const float* a = m;
    float inv[16];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    const float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    Mat4 r;
    if (det == 0) return r;
    for (int i = 0; i < 16; ++i) r.m[i] = inv[i] / det;
    return r;
}

std::vector<Mat4>& FixedState::currentStack(GLuint activeTexture) {
    switch (matrixMode) {
    case GL_PROJECTION: return projection;
    case GL_TEXTURE: return texture[std::min<GLuint>(activeTexture, kTextureUnits - 1)];
    case GL_COLOR: return color;
    default:
        if (matrixMode >= GL_MATRIX0_ARB && matrixMode < GL_MATRIX0_ARB + kProgramMatrices)
            return programMatrix[matrixMode - GL_MATRIX0_ARB];
        return modelview;
    }
}

namespace api {
namespace {

FixedState& fixedState(Context& ctx) {
    ++ctx.state.fixed.version;
    return ctx.state.fixed;
}

bool compatOnly(Context& ctx) {
    if (ctx.isCompat()) return true;
    ctx.setError(GL_INVALID_OPERATION);  // removed from the core profile
    return false;
}

// The stack the matrix commands operate on. Texture units past
// MAX_TEXTURE_COORDS have no texture matrix: GL_INVALID_OPERATION.
std::vector<Mat4>* matrixStack(Context& ctx) {
    if (!compatOnly(ctx)) return nullptr;
    if (ctx.state.fixed.matrixMode == GL_TEXTURE && ctx.state.activeTexture >= FixedState::kTextureUnits) {
        ctx.setError(GL_INVALID_OPERATION);
        return nullptr;
    }
    return &fixedState(ctx).currentStack(ctx.state.activeTexture);
}

void load(Context& ctx, const Mat4& m) {
    if (std::vector<Mat4>* stack = matrixStack(ctx)) stack->back() = m;
}

void multiply(Context& ctx, const Mat4& m) {
    if (std::vector<Mat4>* stack = matrixStack(ctx)) stack->back() = stack->back() * m;
}

int depthLimit(GLenum mode) {
    switch (mode) {
    case GL_MODELVIEW: return FixedState::kModelViewDepth;
    case GL_PROJECTION: return FixedState::kProjectionDepth;
    case GL_COLOR: return FixedState::kColorDepth;
    case GL_TEXTURE: return FixedState::kTextureDepth;
    default: return FixedState::kProgramMatrixDepth;  // MATRIXi_ARB
    }
}

template <class T>
Mat4 toMat4(const T* m, bool transpose) {
    Mat4 r;
    for (int i = 0; i < 16; ++i) r.m[i] = static_cast<float>(m[i]);
    return transpose ? r.transpose() : r;
}

}  // namespace

// ------------------------------------------------------------- matrices

void MatrixMode(Context& ctx, GLenum mode) {
    if (!compatOnly(ctx)) return;
    // GL_COLOR selects the imaging subset's colour matrix. ARB_imaging is not
    // advertised, but drivers that implement it accept GL_COLOR, and the
    // Steam overlay selects it unchecked to save and restore it: rejected,
    // its matrix commands would land on the game's projection and texture
    // matrices instead.
    // MATRIXi_ARB: the program matrices of ARB assembly programs (state.matrix.program[i]).
    const bool programMatrix = mode >= GL_MATRIX0_ARB && mode < GL_MATRIX0_ARB + FixedState::kProgramMatrices;
    if (mode != GL_MODELVIEW && mode != GL_PROJECTION && mode != GL_TEXTURE && mode != GL_COLOR && !programMatrix)
        return ctx.setError(GL_INVALID_ENUM);
    if (mode == GL_TEXTURE && ctx.state.activeTexture >= FixedState::kTextureUnits) return ctx.setError(GL_INVALID_OPERATION);
    fixedState(ctx).matrixMode = mode;
}

void LoadIdentity(Context& ctx) { load(ctx, Mat4()); }
void LoadMatrixf(Context& ctx, const GLfloat* m) { if (compatOnly(ctx)) load(ctx, toMat4(m, false)); }
void LoadMatrixd(Context& ctx, const GLdouble* m) { if (compatOnly(ctx)) load(ctx, toMat4(m, false)); }
void LoadTransposeMatrixf(Context& ctx, const GLfloat* m) { if (compatOnly(ctx)) load(ctx, toMat4(m, true)); }
void LoadTransposeMatrixd(Context& ctx, const GLdouble* m) { if (compatOnly(ctx)) load(ctx, toMat4(m, true)); }
void MultMatrixf(Context& ctx, const GLfloat* m) { if (compatOnly(ctx)) multiply(ctx, toMat4(m, false)); }
void MultMatrixd(Context& ctx, const GLdouble* m) { if (compatOnly(ctx)) multiply(ctx, toMat4(m, false)); }
void MultTransposeMatrixf(Context& ctx, const GLfloat* m) { if (compatOnly(ctx)) multiply(ctx, toMat4(m, true)); }
void MultTransposeMatrixd(Context& ctx, const GLdouble* m) { if (compatOnly(ctx)) multiply(ctx, toMat4(m, true)); }

void Translatef(Context& ctx, GLfloat x, GLfloat y, GLfloat z) {
    if (!compatOnly(ctx)) return;
    Mat4 t;
    t.m[12] = x;
    t.m[13] = y;
    t.m[14] = z;
    multiply(ctx, t);
}
void Translated(Context& ctx, GLdouble x, GLdouble y, GLdouble z) {
    Translatef(ctx, static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
}

void Scalef(Context& ctx, GLfloat x, GLfloat y, GLfloat z) {
    if (!compatOnly(ctx)) return;
    Mat4 s;
    s.m[0] = x;
    s.m[5] = y;
    s.m[10] = z;
    multiply(ctx, s);
}
void Scaled(Context& ctx, GLdouble x, GLdouble y, GLdouble z) {
    Scalef(ctx, static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
}

void Rotatef(Context& ctx, GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {
    if (!compatOnly(ctx)) return;
    const float len = std::sqrt(x * x + y * y + z * z);
    Mat4 r;
    if (len > 0) {
        x /= len;
        y /= len;
        z /= len;
        const float rad = angle * 3.14159265358979323846f / 180.0f;
        const float c = std::cos(rad), s = std::sin(rad), ic = 1 - c;
        r.m[0] = x * x * ic + c;     r.m[4] = x * y * ic - z * s; r.m[8] = x * z * ic + y * s;
        r.m[1] = y * x * ic + z * s; r.m[5] = y * y * ic + c;     r.m[9] = y * z * ic - x * s;
        r.m[2] = x * z * ic - y * s; r.m[6] = y * z * ic + x * s; r.m[10] = z * z * ic + c;
    }
    multiply(ctx, r);
}
void Rotated(Context& ctx, GLdouble angle, GLdouble x, GLdouble y, GLdouble z) {
    Rotatef(ctx, static_cast<float>(angle), static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
}

void Frustum(Context& ctx, GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
    if (!compatOnly(ctx)) return;
    if (n <= 0 || f <= 0 || l == r || b == t || n == f) return ctx.setError(GL_INVALID_VALUE);
    Mat4 m;
    m.m[0] = static_cast<float>(2 * n / (r - l));
    m.m[5] = static_cast<float>(2 * n / (t - b));
    m.m[8] = static_cast<float>((r + l) / (r - l));
    m.m[9] = static_cast<float>((t + b) / (t - b));
    m.m[10] = static_cast<float>(-(f + n) / (f - n));
    m.m[11] = -1;
    m.m[14] = static_cast<float>(-2 * f * n / (f - n));
    m.m[15] = 0;
    multiply(ctx, m);
}

void Ortho(Context& ctx, GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
    if (!compatOnly(ctx)) return;
    if (l == r || b == t || n == f) return ctx.setError(GL_INVALID_VALUE);
    Mat4 m;
    m.m[0] = static_cast<float>(2 / (r - l));
    m.m[5] = static_cast<float>(2 / (t - b));
    m.m[10] = static_cast<float>(-2 / (f - n));
    m.m[12] = static_cast<float>(-(r + l) / (r - l));
    m.m[13] = static_cast<float>(-(t + b) / (t - b));
    m.m[14] = static_cast<float>(-(f + n) / (f - n));
    multiply(ctx, m);
}

void PushMatrix(Context& ctx) {
    std::vector<Mat4>* stack = matrixStack(ctx);
    if (!stack) return;
    if (static_cast<int>(stack->size()) >= depthLimit(ctx.state.fixed.matrixMode)) return ctx.setError(GL_STACK_OVERFLOW);
    stack->push_back(stack->back());
}

void PopMatrix(Context& ctx) {
    std::vector<Mat4>* stack = matrixStack(ctx);
    if (!stack) return;
    if (stack->size() <= 1) return ctx.setError(GL_STACK_UNDERFLOW);
    stack->pop_back();
}

// ------------------------------------------------------------- lighting

namespace {

FixedState::Light* lightOrError(Context& ctx, GLenum light) {
    if (!compatOnly(ctx)) return nullptr;
    if (light < GL_LIGHT0 || light >= GL_LIGHT0 + FixedState::kLights) {
        ctx.setError(GL_INVALID_ENUM);
        return nullptr;
    }
    return &fixedState(ctx).lights[light - GL_LIGHT0];
}

void setLight(Context& ctx, GLenum light, GLenum pname, const float* v) {
    FixedState::Light* l = lightOrError(ctx, light);
    if (!l) return;
    const Mat4& mv = ctx.state.fixed.modelview.back();
    switch (pname) {
    case GL_AMBIENT: std::copy(v, v + 4, l->ambient); return;
    case GL_DIFFUSE: std::copy(v, v + 4, l->diffuse); return;
    case GL_SPECULAR: std::copy(v, v + 4, l->specular); return;
    case GL_POSITION: mv.transform(v, l->position); return;  // stored in eye space
    case GL_SPOT_DIRECTION: {
        const float d[4] = {v[0], v[1], v[2], 0};
        float e[4];
        mv.transform(d, e);
        std::copy(e, e + 3, l->spotDirection);
        return;
    }
    case GL_SPOT_EXPONENT:
        if (v[0] < 0 || v[0] > 128) return ctx.setError(GL_INVALID_VALUE);
        l->spotExponent = v[0];
        return;
    case GL_SPOT_CUTOFF:
        if ((v[0] < 0 || v[0] > 90) && v[0] != 180) return ctx.setError(GL_INVALID_VALUE);
        l->spotCutoff = v[0];
        return;
    case GL_CONSTANT_ATTENUATION: case GL_LINEAR_ATTENUATION: case GL_QUADRATIC_ATTENUATION:
        if (v[0] < 0) return ctx.setError(GL_INVALID_VALUE);
        l->attenuation[pname - GL_CONSTANT_ATTENUATION] = v[0];
        return;
    default: ctx.setError(GL_INVALID_ENUM);
    }
}

int lightParamCount(GLenum pname) {
    switch (pname) {
    case GL_AMBIENT: case GL_DIFFUSE: case GL_SPECULAR: case GL_POSITION: return 4;
    case GL_SPOT_DIRECTION: return 3;
    default: return 1;
    }
}

bool getLight(Context& ctx, GLenum light, GLenum pname, float* out) {
    FixedState::Light* l = lightOrError(ctx, light);
    if (!l) return false;
    switch (pname) {
    case GL_AMBIENT: std::copy(l->ambient, l->ambient + 4, out); return true;
    case GL_DIFFUSE: std::copy(l->diffuse, l->diffuse + 4, out); return true;
    case GL_SPECULAR: std::copy(l->specular, l->specular + 4, out); return true;
    case GL_POSITION: std::copy(l->position, l->position + 4, out); return true;
    case GL_SPOT_DIRECTION: std::copy(l->spotDirection, l->spotDirection + 3, out); return true;
    case GL_SPOT_EXPONENT: out[0] = l->spotExponent; return true;
    case GL_SPOT_CUTOFF: out[0] = l->spotCutoff; return true;
    case GL_CONSTANT_ATTENUATION: case GL_LINEAR_ATTENUATION: case GL_QUADRATIC_ATTENUATION:
        out[0] = l->attenuation[pname - GL_CONSTANT_ATTENUATION];
        return true;
    default: ctx.setError(GL_INVALID_ENUM); return false;
    }
}

// Integer colour parameters map [-2^31, 2^31-1] to [-1, 1] (GL 3.3 compatibility section 2.19.2).
float intColor(GLint v) { return static_cast<float>((2.0 * v + 1.0) / 4294967295.0); }

bool isColorParam(GLenum pname) {
    return pname == GL_AMBIENT || pname == GL_DIFFUSE || pname == GL_SPECULAR || pname == GL_EMISSION ||
           pname == GL_AMBIENT_AND_DIFFUSE || pname == GL_LIGHT_MODEL_AMBIENT;
}

// Integer queries of colours map [-1, 1] linearly to the integer range;
// other values are rounded (GL 3.3 compatibility section 6.1.2).
GLint queryInt(GLenum pname, float v) {
    if (isColorParam(pname)) return static_cast<GLint>(std::llround(std::clamp(static_cast<double>(v), -1.0, 1.0) * 2147483647.0));
    return static_cast<GLint>(std::lround(v));
}

}  // namespace

void Lightfv(Context& ctx, GLenum light, GLenum pname, const GLfloat* params) { setLight(ctx, light, pname, params); }
void Lightf(Context& ctx, GLenum light, GLenum pname, GLfloat param) {
    if (lightParamCount(pname) != 1) return ctx.setError(GL_INVALID_ENUM);
    setLight(ctx, light, pname, &param);
}
void Lightiv(Context& ctx, GLenum light, GLenum pname, const GLint* params) {
    float v[4] = {};
    for (int i = 0; i < lightParamCount(pname); ++i)
        v[i] = isColorParam(pname) ? intColor(params[i]) : static_cast<float>(params[i]);
    setLight(ctx, light, pname, v);
}
void Lighti(Context& ctx, GLenum light, GLenum pname, GLint param) {
    if (lightParamCount(pname) != 1) return ctx.setError(GL_INVALID_ENUM);
    Lightiv(ctx, light, pname, &param);
}
void GetLightfv(Context& ctx, GLenum light, GLenum pname, GLfloat* params) { (void)getLight(ctx, light, pname, params); }
void GetLightiv(Context& ctx, GLenum light, GLenum pname, GLint* params) {
    float v[4] = {};
    if (!getLight(ctx, light, pname, v)) return;
    for (int i = 0; i < lightParamCount(pname); ++i) params[i] = queryInt(pname, v[i]);
}

namespace {

void setLightModel(Context& ctx, GLenum pname, const float* v) {
    if (!compatOnly(ctx)) return;
    FixedState& f = fixedState(ctx);
    switch (pname) {
    case GL_LIGHT_MODEL_AMBIENT: std::copy(v, v + 4, f.lightModelAmbient); return;
    case GL_LIGHT_MODEL_LOCAL_VIEWER: f.lightModelLocalViewer = v[0] != 0; return;
    case GL_LIGHT_MODEL_TWO_SIDE: f.lightModelTwoSide = v[0] != 0; return;
    case GL_LIGHT_MODEL_COLOR_CONTROL:
        if (v[0] != GL_SINGLE_COLOR && v[0] != GL_SEPARATE_SPECULAR_COLOR) return ctx.setError(GL_INVALID_ENUM);
        f.lightModelColorControl = static_cast<GLenum>(v[0]);
        return;
    default: ctx.setError(GL_INVALID_ENUM);
    }
}

}  // namespace

void LightModelfv(Context& ctx, GLenum pname, const GLfloat* params) { setLightModel(ctx, pname, params); }
void LightModelf(Context& ctx, GLenum pname, GLfloat param) {
    if (pname == GL_LIGHT_MODEL_AMBIENT) return ctx.setError(GL_INVALID_ENUM);
    setLightModel(ctx, pname, &param);
}
void LightModeliv(Context& ctx, GLenum pname, const GLint* params) {
    float v[4] = {};
    for (int i = 0; i < (pname == GL_LIGHT_MODEL_AMBIENT ? 4 : 1); ++i)
        v[i] = pname == GL_LIGHT_MODEL_AMBIENT ? intColor(params[i]) : static_cast<float>(params[i]);
    setLightModel(ctx, pname, v);
}
void LightModeli(Context& ctx, GLenum pname, GLint param) {
    if (pname == GL_LIGHT_MODEL_AMBIENT) return ctx.setError(GL_INVALID_ENUM);
    LightModeliv(ctx, pname, &param);
}

namespace {

void setMaterial(Context& ctx, GLenum face, GLenum pname, const float* v) {
    if (!compatOnly(ctx)) return;
    if (face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) return ctx.setError(GL_INVALID_ENUM);
    FixedState& f = fixedState(ctx);
    for (int side = 0; side < 2; ++side) {
        if ((side == 0 && face == GL_BACK) || (side == 1 && face == GL_FRONT)) continue;
        FixedState::Material& m = f.material[side];
        switch (pname) {
        case GL_AMBIENT: std::copy(v, v + 4, m.ambient); break;
        case GL_DIFFUSE: std::copy(v, v + 4, m.diffuse); break;
        case GL_SPECULAR: std::copy(v, v + 4, m.specular); break;
        case GL_EMISSION: std::copy(v, v + 4, m.emission); break;
        case GL_AMBIENT_AND_DIFFUSE:
            std::copy(v, v + 4, m.ambient);
            std::copy(v, v + 4, m.diffuse);
            break;
        case GL_SHININESS:
            if (v[0] < 0 || v[0] > 128) return ctx.setError(GL_INVALID_VALUE);
            m.shininess = v[0];
            break;
        case GL_COLOR_INDEXES: break;
        default: return ctx.setError(GL_INVALID_ENUM);
        }
    }
    trackColorMaterial(ctx);  // the parameters COLOR_MATERIAL tracks keep the current colour
}

bool getMaterial(Context& ctx, GLenum face, GLenum pname, float* out) {
    if (!compatOnly(ctx)) return false;
    if (face != GL_FRONT && face != GL_BACK) return ctx.setError(GL_INVALID_ENUM), false;
    const FixedState::Material& m = ctx.state.fixed.material[face == GL_BACK];
    switch (pname) {
    case GL_AMBIENT: std::copy(m.ambient, m.ambient + 4, out); return true;
    case GL_DIFFUSE: std::copy(m.diffuse, m.diffuse + 4, out); return true;
    case GL_SPECULAR: std::copy(m.specular, m.specular + 4, out); return true;
    case GL_EMISSION: std::copy(m.emission, m.emission + 4, out); return true;
    case GL_SHININESS: out[0] = m.shininess; return true;
    case GL_COLOR_INDEXES: out[0] = 0; out[1] = 1; out[2] = 1; return true;
    default: ctx.setError(GL_INVALID_ENUM); return false;
    }
}

}  // namespace

void Materialfv(Context& ctx, GLenum face, GLenum pname, const GLfloat* params) { setMaterial(ctx, face, pname, params); }
void Materialf(Context& ctx, GLenum face, GLenum pname, GLfloat param) {
    if (pname != GL_SHININESS) return ctx.setError(GL_INVALID_ENUM);
    setMaterial(ctx, face, pname, &param);
}
void Materialiv(Context& ctx, GLenum face, GLenum pname, const GLint* params) {
    float v[4] = {};
    const int n = pname == GL_SHININESS ? 1 : pname == GL_COLOR_INDEXES ? 3 : 4;
    for (int i = 0; i < n; ++i) v[i] = isColorParam(pname) ? intColor(params[i]) : static_cast<float>(params[i]);
    setMaterial(ctx, face, pname, v);
}
void Materiali(Context& ctx, GLenum face, GLenum pname, GLint param) {
    if (pname != GL_SHININESS) return ctx.setError(GL_INVALID_ENUM);
    Materialiv(ctx, face, pname, &param);
}
void GetMaterialfv(Context& ctx, GLenum face, GLenum pname, GLfloat* params) { (void)getMaterial(ctx, face, pname, params); }
void GetMaterialiv(Context& ctx, GLenum face, GLenum pname, GLint* params) {
    float v[4] = {};
    if (!getMaterial(ctx, face, pname, v)) return;
    const int n = pname == GL_SHININESS ? 1 : pname == GL_COLOR_INDEXES ? 3 : 4;
    for (int i = 0; i < n; ++i) params[i] = queryInt(pname, v[i]);
}

void ColorMaterial(Context& ctx, GLenum face, GLenum mode) {
    if (!compatOnly(ctx)) return;
    if ((face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) ||
        (mode != GL_EMISSION && mode != GL_AMBIENT && mode != GL_DIFFUSE && mode != GL_SPECULAR &&
         mode != GL_AMBIENT_AND_DIFFUSE))
        return ctx.setError(GL_INVALID_ENUM);
    FixedState& f = fixedState(ctx);
    f.colorMaterialFace = face;
    f.colorMaterialMode = mode;
    trackColorMaterial(ctx);
}

void trackColorMaterial(Context& ctx) {
    FixedState& f = ctx.state.fixed;
    if (!f.colorMaterial) return;
    const float* color = ctx.state.genericAttribs[kConvColor].value.f;
    bool changed = false;
    auto track = [&](float* parameter) {
        if (std::equal(color, color + 4, parameter)) return;
        std::copy(color, color + 4, parameter);
        changed = true;
    };
    for (int side = 0; side < 2; ++side) {
        if ((side == 0 && f.colorMaterialFace == GL_BACK) || (side == 1 && f.colorMaterialFace == GL_FRONT)) continue;
        FixedState::Material& m = f.material[side];
        switch (f.colorMaterialMode) {
        case GL_AMBIENT: track(m.ambient); break;
        case GL_DIFFUSE: track(m.diffuse); break;
        case GL_SPECULAR: track(m.specular); break;
        case GL_EMISSION: track(m.emission); break;
        default: track(m.ambient); track(m.diffuse); break;  // GL_AMBIENT_AND_DIFFUSE
        }
    }
    if (changed) ++f.version;
}

void ShadeModel(Context& ctx, GLenum mode) {
    if (!compatOnly(ctx)) return;
    if (mode != GL_FLAT && mode != GL_SMOOTH) return ctx.setError(GL_INVALID_ENUM);
    fixedState(ctx).shadeModel = mode;
}

// ------------------------------------------------- texture environment

namespace {

FixedState::TexEnv* texEnvUnit(Context& ctx) {
    if (!compatOnly(ctx)) return nullptr;
    if (ctx.state.activeTexture >= FixedState::kTextureUnits) {
        ctx.setError(GL_INVALID_OPERATION);
        return nullptr;
    }
    return &fixedState(ctx).texEnv[ctx.state.activeTexture];
}

bool validCombine(GLenum mode, bool rgb) {
    switch (mode) {
    case GL_REPLACE: case GL_MODULATE: case GL_ADD: case GL_ADD_SIGNED: case GL_INTERPOLATE: case GL_SUBTRACT: return true;
    case GL_DOT3_RGB: case GL_DOT3_RGBA: return rgb;
    default: return false;
    }
}

void setTexEnv(Context& ctx, GLenum target, GLenum pname, const float* v, bool fromInt) {
    FixedState::TexEnv* e = texEnvUnit(ctx);
    if (!e) return;
    const GLenum value = static_cast<GLenum>(v[0]);
    if (target == GL_TEXTURE_FILTER_CONTROL) {
        if (pname != GL_TEXTURE_LOD_BIAS) return ctx.setError(GL_INVALID_ENUM);
        e->lodBias = v[0];
        ctx.markDirty(kDirtyTextures);  // part of the unit's sampler state (compat::textureUnitLodBias)
        return;
    }
    if (target == GL_POINT_SPRITE) {
        if (pname != GL_COORD_REPLACE) return ctx.setError(GL_INVALID_ENUM);
        e->coordReplace = v[0] != 0;
        return;
    }
    if (target != GL_TEXTURE_ENV) return ctx.setError(GL_INVALID_ENUM);
    switch (pname) {
    case GL_TEXTURE_ENV_MODE:
        if (value != GL_MODULATE && value != GL_DECAL && value != GL_BLEND && value != GL_REPLACE && value != GL_ADD &&
            value != GL_COMBINE)
            return ctx.setError(GL_INVALID_ENUM);
        e->mode = value;
        return;
    case GL_TEXTURE_ENV_COLOR:
        for (int i = 0; i < 4; ++i) e->color[i] = fromInt ? v[i] : std::clamp(v[i], 0.0f, 1.0f);
        return;
    case GL_COMBINE_RGB:
        if (!validCombine(value, true)) return ctx.setError(GL_INVALID_ENUM);
        e->combineRGB = value;
        return;
    case GL_COMBINE_ALPHA:
        if (!validCombine(value, false)) return ctx.setError(GL_INVALID_ENUM);
        e->combineAlpha = value;
        return;
    case GL_SRC0_RGB: case GL_SRC1_RGB: case GL_SRC2_RGB: case GL_SRC0_ALPHA: case GL_SRC1_ALPHA: case GL_SRC2_ALPHA: {
        const bool texUnit = value >= GL_TEXTURE0 && value < GL_TEXTURE0 + FixedState::kTextureUnits;
        if (value != GL_TEXTURE && value != GL_CONSTANT && value != GL_PRIMARY_COLOR && value != GL_PREVIOUS && !texUnit)
            return ctx.setError(GL_INVALID_ENUM);
        if (pname <= GL_SRC2_RGB) e->sourceRGB[pname - GL_SRC0_RGB] = value;
        else e->sourceAlpha[pname - GL_SRC0_ALPHA] = value;
        return;
    }
    case GL_OPERAND0_RGB: case GL_OPERAND1_RGB: case GL_OPERAND2_RGB:
        if (value != GL_SRC_COLOR && value != GL_ONE_MINUS_SRC_COLOR && value != GL_SRC_ALPHA &&
            value != GL_ONE_MINUS_SRC_ALPHA)
            return ctx.setError(GL_INVALID_ENUM);
        e->operandRGB[pname - GL_OPERAND0_RGB] = value;
        return;
    case GL_OPERAND0_ALPHA: case GL_OPERAND1_ALPHA: case GL_OPERAND2_ALPHA:
        if (value != GL_SRC_ALPHA && value != GL_ONE_MINUS_SRC_ALPHA) return ctx.setError(GL_INVALID_ENUM);
        e->operandAlpha[pname - GL_OPERAND0_ALPHA] = value;
        return;
    case GL_RGB_SCALE: case GL_ALPHA_SCALE:
        if (v[0] != 1 && v[0] != 2 && v[0] != 4) return ctx.setError(GL_INVALID_VALUE);
        (pname == GL_RGB_SCALE ? e->scaleRGB : e->scaleAlpha) = v[0];
        return;
    default: ctx.setError(GL_INVALID_ENUM);
    }
}

int texEnvParamCount(GLenum pname) { return pname == GL_TEXTURE_ENV_COLOR ? 4 : 1; }

void getTexEnv(Context& ctx, GLenum target, GLenum pname, float* out) {
    if (!compatOnly(ctx)) return;
    if (ctx.state.activeTexture >= FixedState::kTextureUnits) return ctx.setError(GL_INVALID_OPERATION);
    const FixedState::TexEnv& e = ctx.state.fixed.texEnv[ctx.state.activeTexture];
    if (target == GL_TEXTURE_FILTER_CONTROL && pname == GL_TEXTURE_LOD_BIAS) { out[0] = e.lodBias; return; }
    if (target == GL_POINT_SPRITE && pname == GL_COORD_REPLACE) { out[0] = e.coordReplace; return; }
    if (target != GL_TEXTURE_ENV) return ctx.setError(GL_INVALID_ENUM);
    switch (pname) {
    case GL_TEXTURE_ENV_MODE: out[0] = static_cast<float>(e.mode); return;
    case GL_TEXTURE_ENV_COLOR: std::copy(e.color, e.color + 4, out); return;
    case GL_COMBINE_RGB: out[0] = static_cast<float>(e.combineRGB); return;
    case GL_COMBINE_ALPHA: out[0] = static_cast<float>(e.combineAlpha); return;
    case GL_SRC0_RGB: case GL_SRC1_RGB: case GL_SRC2_RGB: out[0] = static_cast<float>(e.sourceRGB[pname - GL_SRC0_RGB]); return;
    case GL_SRC0_ALPHA: case GL_SRC1_ALPHA: case GL_SRC2_ALPHA:
        out[0] = static_cast<float>(e.sourceAlpha[pname - GL_SRC0_ALPHA]);
        return;
    case GL_OPERAND0_RGB: case GL_OPERAND1_RGB: case GL_OPERAND2_RGB:
        out[0] = static_cast<float>(e.operandRGB[pname - GL_OPERAND0_RGB]);
        return;
    case GL_OPERAND0_ALPHA: case GL_OPERAND1_ALPHA: case GL_OPERAND2_ALPHA:
        out[0] = static_cast<float>(e.operandAlpha[pname - GL_OPERAND0_ALPHA]);
        return;
    case GL_RGB_SCALE: out[0] = e.scaleRGB; return;
    case GL_ALPHA_SCALE: out[0] = e.scaleAlpha; return;
    default: ctx.setError(GL_INVALID_ENUM);
    }
}

}  // namespace

void TexEnvfv(Context& ctx, GLenum target, GLenum pname, const GLfloat* params) { setTexEnv(ctx, target, pname, params, false); }
void TexEnvf(Context& ctx, GLenum target, GLenum pname, GLfloat param) {
    if (texEnvParamCount(pname) != 1) return ctx.setError(GL_INVALID_ENUM);
    setTexEnv(ctx, target, pname, &param, false);
}
void TexEnviv(Context& ctx, GLenum target, GLenum pname, const GLint* params) {
    float v[4] = {};
    for (int i = 0; i < texEnvParamCount(pname); ++i)
        v[i] = pname == GL_TEXTURE_ENV_COLOR ? std::clamp(intColor(params[i]), 0.0f, 1.0f) : static_cast<float>(params[i]);
    setTexEnv(ctx, target, pname, v, true);
}
void TexEnvi(Context& ctx, GLenum target, GLenum pname, GLint param) {
    if (texEnvParamCount(pname) != 1) return ctx.setError(GL_INVALID_ENUM);
    TexEnviv(ctx, target, pname, &param);
}
void GetTexEnvfv(Context& ctx, GLenum target, GLenum pname, GLfloat* params) { getTexEnv(ctx, target, pname, params); }
void GetTexEnviv(Context& ctx, GLenum target, GLenum pname, GLint* params) {
    float v[4] = {};
    getTexEnv(ctx, target, pname, v);
    for (int i = 0; i < texEnvParamCount(pname); ++i)
        params[i] = pname == GL_TEXTURE_ENV_COLOR ? static_cast<GLint>(std::lround(v[i] * 2147483647.0)) : static_cast<GLint>(v[i]);
}

// ------------------------------------------- texture coordinate generation

namespace {

void setTexGen(Context& ctx, GLenum coord, GLenum pname, const double* v) {
    if (!compatOnly(ctx)) return;
    if (coord < GL_S || coord > GL_Q) return ctx.setError(GL_INVALID_ENUM);
    if (ctx.state.activeTexture >= FixedState::kTextureUnits) return ctx.setError(GL_INVALID_OPERATION);
    FixedState& f = fixedState(ctx);
    FixedState::TexGen& g = f.texGen[ctx.state.activeTexture];
    const int c = static_cast<int>(coord - GL_S);
    switch (pname) {
    case GL_TEXTURE_GEN_MODE: {
        const GLenum mode = static_cast<GLenum>(v[0]);
        const bool ok = mode == GL_OBJECT_LINEAR || mode == GL_EYE_LINEAR ||
                        (mode == GL_SPHERE_MAP && c < 2) || (mode == GL_REFLECTION_MAP && c < 3) ||
                        (mode == GL_NORMAL_MAP && c < 3);
        if (!ok) return ctx.setError(GL_INVALID_ENUM);
        g.mode[c] = mode;
        return;
    }
    case GL_OBJECT_PLANE:
        for (int i = 0; i < 4; ++i) g.objectPlane[c][i] = static_cast<float>(v[i]);
        return;
    case GL_EYE_PLANE: {
        // p' = p * inverse(modelview) (GL 3.3 compatibility section 2.12.4).
        const Mat4 inv = f.modelview.back().inverse();
        for (int col = 0; col < 4; ++col) {
            double sum = 0;
            for (int k = 0; k < 4; ++k) sum += v[k] * inv.m[col * 4 + k];
            g.eyePlane[c][col] = static_cast<float>(sum);
        }
        return;
    }
    default: ctx.setError(GL_INVALID_ENUM);
    }
}

bool getTexGen(Context& ctx, GLenum coord, GLenum pname, double* out) {
    if (!compatOnly(ctx)) return false;
    if (coord < GL_S || coord > GL_Q) return ctx.setError(GL_INVALID_ENUM), false;
    if (ctx.state.activeTexture >= FixedState::kTextureUnits) return ctx.setError(GL_INVALID_OPERATION), false;
    const FixedState::TexGen& g = ctx.state.fixed.texGen[ctx.state.activeTexture];
    const int c = static_cast<int>(coord - GL_S);
    switch (pname) {
    case GL_TEXTURE_GEN_MODE: out[0] = g.mode[c]; return true;
    case GL_OBJECT_PLANE: for (int i = 0; i < 4; ++i) out[i] = g.objectPlane[c][i]; return true;
    case GL_EYE_PLANE: for (int i = 0; i < 4; ++i) out[i] = g.eyePlane[c][i]; return true;
    default: ctx.setError(GL_INVALID_ENUM); return false;
    }
}

}  // namespace

void TexGendv(Context& ctx, GLenum coord, GLenum pname, const GLdouble* params) { setTexGen(ctx, coord, pname, params); }
void TexGenfv(Context& ctx, GLenum coord, GLenum pname, const GLfloat* params) {
    double v[4] = {};
    for (int i = 0; i < (pname == GL_TEXTURE_GEN_MODE ? 1 : 4); ++i) v[i] = params[i];
    setTexGen(ctx, coord, pname, v);
}
void TexGeniv(Context& ctx, GLenum coord, GLenum pname, const GLint* params) {
    double v[4] = {};
    for (int i = 0; i < (pname == GL_TEXTURE_GEN_MODE ? 1 : 4); ++i) v[i] = params[i];
    setTexGen(ctx, coord, pname, v);
}
void TexGend(Context& ctx, GLenum coord, GLenum pname, GLdouble param) {
    if (pname != GL_TEXTURE_GEN_MODE) return ctx.setError(GL_INVALID_ENUM);
    setTexGen(ctx, coord, pname, &param);
}
void TexGenf(Context& ctx, GLenum coord, GLenum pname, GLfloat param) { TexGend(ctx, coord, pname, param); }
void TexGeni(Context& ctx, GLenum coord, GLenum pname, GLint param) { TexGend(ctx, coord, pname, param); }
void GetTexGendv(Context& ctx, GLenum coord, GLenum pname, GLdouble* params) { (void)getTexGen(ctx, coord, pname, params); }
void GetTexGenfv(Context& ctx, GLenum coord, GLenum pname, GLfloat* params) {
    double v[4] = {};
    if (!getTexGen(ctx, coord, pname, v)) return;
    for (int i = 0; i < (pname == GL_TEXTURE_GEN_MODE ? 1 : 4); ++i) params[i] = static_cast<float>(v[i]);
}
void GetTexGeniv(Context& ctx, GLenum coord, GLenum pname, GLint* params) {
    double v[4] = {};
    if (!getTexGen(ctx, coord, pname, v)) return;
    for (int i = 0; i < (pname == GL_TEXTURE_GEN_MODE ? 1 : 4); ++i) params[i] = static_cast<GLint>(std::lround(v[i]));
}

// ------------------------------------------------------------- fog

namespace {

void setFog(Context& ctx, GLenum pname, const float* v) {
    if (!compatOnly(ctx)) return;
    FixedState& f = fixedState(ctx);
    switch (pname) {
    case GL_FOG_MODE: {
        const GLenum mode = static_cast<GLenum>(v[0]);
        if (mode != GL_LINEAR && mode != GL_EXP && mode != GL_EXP2) return ctx.setError(GL_INVALID_ENUM);
        f.fogMode = mode;
        return;
    }
    case GL_FOG_DENSITY:
        if (v[0] < 0) return ctx.setError(GL_INVALID_VALUE);
        f.fogDensity = v[0];
        return;
    case GL_FOG_START: f.fogStart = v[0]; return;
    case GL_FOG_END: f.fogEnd = v[0]; return;
    case GL_FOG_COLOR: for (int i = 0; i < 4; ++i) f.fogColor[i] = std::clamp(v[i], 0.0f, 1.0f); return;
    case GL_FOG_COORD_SRC: {
        const GLenum src = static_cast<GLenum>(v[0]);
        if (src != GL_FOG_COORD && src != GL_FRAGMENT_DEPTH) return ctx.setError(GL_INVALID_ENUM);
        f.fogCoordSource = src;
        return;
    }
    case GL_FOG_INDEX: return;
    default: ctx.setError(GL_INVALID_ENUM);
    }
}

}  // namespace

void Fogfv(Context& ctx, GLenum pname, const GLfloat* params) { setFog(ctx, pname, params); }
void Fogf(Context& ctx, GLenum pname, GLfloat param) {
    if (pname == GL_FOG_COLOR) return ctx.setError(GL_INVALID_ENUM);
    setFog(ctx, pname, &param);
}
void Fogiv(Context& ctx, GLenum pname, const GLint* params) {
    float v[4] = {};
    for (int i = 0; i < (pname == GL_FOG_COLOR ? 4 : 1); ++i)
        v[i] = pname == GL_FOG_COLOR ? intColor(params[i]) : static_cast<float>(params[i]);
    setFog(ctx, pname, v);
}
void Fogi(Context& ctx, GLenum pname, GLint param) {
    if (pname == GL_FOG_COLOR) return ctx.setError(GL_INVALID_ENUM);
    Fogiv(ctx, pname, &param);
}

// ----------------------------------------------- alpha test, clip planes, points

void AlphaFunc(Context& ctx, GLenum func, GLfloat ref) {
    if (!compatOnly(ctx)) return;
    if (func < GL_NEVER || func > GL_ALWAYS) return ctx.setError(GL_INVALID_ENUM);
    FixedState& f = fixedState(ctx);
    f.alphaFunc = func;
    f.alphaRef = std::clamp(ref, 0.0f, 1.0f);
}

void ClipPlane(Context& ctx, GLenum plane, const GLdouble* equation) {
    if (!compatOnly(ctx)) return;
    if (plane < GL_CLIP_PLANE0 || plane >= GL_CLIP_PLANE0 + FixedState::kClipPlanes) return ctx.setError(GL_INVALID_ENUM);
    FixedState& f = fixedState(ctx);
    const Mat4 inv = f.modelview.back().inverse();
    for (int col = 0; col < 4; ++col) {
        double sum = 0;
        for (int k = 0; k < 4; ++k) sum += equation[k] * inv.m[col * 4 + k];
        f.clipPlanes[plane - GL_CLIP_PLANE0][col] = static_cast<float>(sum);
    }
}

void GetClipPlane(Context& ctx, GLenum plane, GLdouble* equation) {
    if (!compatOnly(ctx)) return;
    if (plane < GL_CLIP_PLANE0 || plane >= GL_CLIP_PLANE0 + FixedState::kClipPlanes) return ctx.setError(GL_INVALID_ENUM);
    for (int i = 0; i < 4; ++i) equation[i] = ctx.state.fixed.clipPlanes[plane - GL_CLIP_PLANE0][i];
}

void LineStipple(Context& ctx, GLint factor, GLushort pattern) {
    if (!compatOnly(ctx)) return;
    FixedState& f = fixedState(ctx);
    f.lineStippleFactor = std::clamp(factor, 1, 256);
    f.lineStipplePattern = pattern;
}

// The fixed-function program discards fragments whose stipple bit (window
// x mod 32, y mod 32) is clear (fixed_function.mm).
void PolygonStipple(Context& ctx, const GLubyte* mask) {
    if (!compatOnly(ctx)) return;
    const uint8_t* bits = nullptr;
    if (!bitmapSource(ctx, mask, 32, 32, bits) || !bits) return;
    const std::vector<uint8_t> pixels = unpackBitmap(ctx.state.unpack, bits, 32, 32);
    FixedState& f = fixedState(ctx);
    for (int y = 0; y < 32; ++y) {
        uint32_t row = 0;
        for (int x = 0; x < 32; ++x)
            if (pixels[static_cast<size_t>(y * 32 + x)]) row |= 1u << x;
        f.stipple[static_cast<size_t>(y)] = row;
    }
}

void GetPolygonStipple(Context& ctx, GLubyte* mask) {
    if (!compatOnly(ctx)) return;
    // A 32x32 GL_BITMAP image packed with the pack state, into client memory
    // or the bound pixel pack buffer (GL 3.3 compatibility section 6.1.6).
    const PixelStore& pack = ctx.state.pack;
    uint8_t* out = mask;
    if (Buffer* pbo = ctx.state.pixelPackBuffer.get()) {
        const uintptr_t offset = reinterpret_cast<uintptr_t>(mask);
        if (pbo->mappedNonPersistent() || !pbo->mtl || offset + bitmapBytes(pack, 32, 32) > static_cast<size_t>(pbo->size))
            return ctx.setError(GL_INVALID_OPERATION);
        ctx.stream().waitFor(pbo->lastUseSerial);
        out = pbo->contents() + offset;
        ++pbo->version;
    }
    if (!out) return;
    const size_t rowPixels = static_cast<size_t>(pack.rowLength > 0 ? pack.rowLength : 32);
    const size_t align = static_cast<size_t>(std::max(1, pack.alignment));
    const size_t rowBytes = ((rowPixels + 7) / 8 + align - 1) / align * align;
    for (int y = 0; y < 32; ++y) {
        uint8_t* row = out + (static_cast<size_t>(std::max(0, pack.skipRows)) + static_cast<size_t>(y)) * rowBytes;
        for (int x = 0; x < 32; ++x) {
            const size_t bit = static_cast<size_t>(std::max(0, pack.skipPixels)) + static_cast<size_t>(x);
            const uint8_t m = pack.lsbFirst ? static_cast<uint8_t>(1u << (bit % 8)) : static_cast<uint8_t>(0x80u >> (bit % 8));
            if (ctx.state.fixed.stipple[static_cast<size_t>(y)] & (1u << x)) row[bit / 8] |= m;
            else row[bit / 8] &= static_cast<uint8_t>(~m);
        }
    }
}

void ClientActiveTexture(Context& ctx, GLenum texture) {
    if (!compatOnly(ctx)) return;
    if (texture < GL_TEXTURE0 || texture >= GL_TEXTURE0 + FixedState::kTextureUnits) return ctx.setError(GL_INVALID_ENUM);
    ctx.state.fixed.clientActiveTexture = texture - GL_TEXTURE0;
}

// Compatibility capabilities (called by Enable/Disable/IsEnabled).
bool setFixedCapability(Context& ctx, GLenum cap, bool on) {
    if (!ctx.isCompat()) return false;
    FixedState& f = ctx.state.fixed;
    const GLuint unit = ctx.state.activeTexture;
    auto texBit = [&](uint8_t bit) {
        if (unit >= FixedState::kTextureUnits) return ctx.setError(GL_INVALID_OPERATION);
        f.textureEnables[unit] = on ? (f.textureEnables[unit] | bit) : (f.textureEnables[unit] & ~bit);
    };
    ++f.version;
    switch (cap) {
    case GL_LIGHTING: f.lighting = on; return true;
    case GL_COLOR_MATERIAL:
        f.colorMaterial = on;
        trackColorMaterial(ctx);
        return true;
    case GL_NORMALIZE: f.normalize = on; return true;
    case GL_RESCALE_NORMAL: f.rescaleNormal = on; return true;
    case GL_FOG: f.fog = on; return true;
    case GL_ALPHA_TEST: f.alphaTest = on; return true;
    case GL_COLOR_SUM: f.colorSum = on; return true;
    case GL_POINT_SPRITE: f.pointSprite = on; return true;
    case GL_POINT_SMOOTH: f.pointSmooth = on; return true;
    case GL_LINE_STIPPLE: f.lineStipple = on; return true;
    case GL_POLYGON_STIPPLE: f.polygonStipple = on; return true;
    case GL_TEXTURE_1D: texBit(FixedState::kTex1D); return true;
    case GL_TEXTURE_2D: texBit(FixedState::kTex2D); return true;
    case GL_TEXTURE_3D: texBit(FixedState::kTex3D); return true;
    case GL_TEXTURE_CUBE_MAP: texBit(FixedState::kTexCube); return true;
    case GL_TEXTURE_RECTANGLE: texBit(FixedState::kTexRect); return true;
    case GL_TEXTURE_GEN_S: case GL_TEXTURE_GEN_T: case GL_TEXTURE_GEN_R: case GL_TEXTURE_GEN_Q: {
        if (unit >= FixedState::kTextureUnits) {
            ctx.setError(GL_INVALID_OPERATION);
            return true;
        }
        const uint8_t bit = static_cast<uint8_t>(1u << (cap - GL_TEXTURE_GEN_S));
        f.texGen[unit].enabled = on ? (f.texGen[unit].enabled | bit) : (f.texGen[unit].enabled & ~bit);
        return true;
    }
    case GL_AUTO_NORMAL: case GL_MAP1_VERTEX_3: case GL_MAP1_VERTEX_4: case GL_MAP2_VERTEX_3: case GL_MAP2_VERTEX_4:
    case GL_INDEX_LOGIC_OP:
        return true;  // accepted; evaluators and colour-index mode are not supported
    case GL_VERTEX_PROGRAM_TWO_SIDE: f.vertexProgramTwoSide = on; return true;
    case GL_VERTEX_PROGRAM_ARB: f.vertexProgram = on; return true;
    case GL_FRAGMENT_PROGRAM_ARB: f.fragmentProgram = on; return true;
    default: break;
    }
    if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + FixedState::kLights) {
        f.lights[cap - GL_LIGHT0].enabled = on;
        return true;
    }
    return false;
}

bool getFixedCapability(Context& ctx, GLenum cap, bool& value) {
    if (!ctx.isCompat()) return false;
    const FixedState& f = ctx.state.fixed;
    const GLuint unit = std::min<GLuint>(ctx.state.activeTexture, FixedState::kTextureUnits - 1);
    switch (cap) {
    case GL_TEXTURE_1D: case GL_TEXTURE_2D: case GL_TEXTURE_3D: case GL_TEXTURE_CUBE_MAP: case GL_TEXTURE_RECTANGLE:
    case GL_TEXTURE_GEN_S: case GL_TEXTURE_GEN_T: case GL_TEXTURE_GEN_R: case GL_TEXTURE_GEN_Q:
        if (ctx.state.activeTexture >= FixedState::kTextureUnits) {
            ctx.setError(GL_INVALID_OPERATION);  // no fixed-function state past MAX_TEXTURE_UNITS
            value = false;
            return true;
        }
        break;
    default: break;
    }
    switch (cap) {
    case GL_LIGHTING: value = f.lighting; return true;
    case GL_COLOR_MATERIAL: value = f.colorMaterial; return true;
    case GL_NORMALIZE: value = f.normalize; return true;
    case GL_RESCALE_NORMAL: value = f.rescaleNormal; return true;
    case GL_FOG: value = f.fog; return true;
    case GL_ALPHA_TEST: value = f.alphaTest; return true;
    case GL_COLOR_SUM: value = f.colorSum; return true;
    case GL_POINT_SPRITE: value = f.pointSprite; return true;
    case GL_POINT_SMOOTH: value = f.pointSmooth; return true;
    case GL_LINE_STIPPLE: value = f.lineStipple; return true;
    case GL_POLYGON_STIPPLE: value = f.polygonStipple; return true;
    case GL_TEXTURE_1D: value = f.textureEnables[unit] & FixedState::kTex1D; return true;
    case GL_TEXTURE_2D: value = f.textureEnables[unit] & FixedState::kTex2D; return true;
    case GL_TEXTURE_3D: value = f.textureEnables[unit] & FixedState::kTex3D; return true;
    case GL_TEXTURE_CUBE_MAP: value = f.textureEnables[unit] & FixedState::kTexCube; return true;
    case GL_TEXTURE_RECTANGLE: value = f.textureEnables[unit] & FixedState::kTexRect; return true;
    case GL_TEXTURE_GEN_S: case GL_TEXTURE_GEN_T: case GL_TEXTURE_GEN_R: case GL_TEXTURE_GEN_Q:
        value = f.texGen[unit].enabled & (1u << (cap - GL_TEXTURE_GEN_S));
        return true;
    case GL_AUTO_NORMAL: case GL_MAP1_VERTEX_3: case GL_MAP1_VERTEX_4: case GL_MAP2_VERTEX_3: case GL_MAP2_VERTEX_4:
    case GL_INDEX_LOGIC_OP:
        value = false;
        return true;
    case GL_VERTEX_PROGRAM_TWO_SIDE: value = f.vertexProgramTwoSide; return true;
    case GL_VERTEX_PROGRAM_ARB: value = f.vertexProgram; return true;
    case GL_FRAGMENT_PROGRAM_ARB: value = f.fragmentProgram; return true;
    default: break;
    }
    if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + FixedState::kLights) {
        value = f.lights[cap - GL_LIGHT0].enabled;
        return true;
    }
    return false;
}

}  // namespace api
}  // namespace wmgl
