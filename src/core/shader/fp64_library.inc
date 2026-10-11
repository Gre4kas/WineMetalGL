// MSL vector, matrix and built-in forms of the soft-float library
// (shader/fp64.cpp). Included as a raw string; scalar operations come from
// fp64_soft.h: two strings, vector forms then matrix forms (which need
// SPIRV-Cross's spvUnsafeArray). Doubles are ulong bit patterns, dvecN are vec<ulong, N> and a
// dmatCxR is spvUnsafeArray<vec<ulong, R>, C> (an array of its columns).
R"WMGL(
#define WMGL_D_UNARY(name) \
template<int N> static inline vec<ulong, N> name(vec<ulong, N> a) { vec<ulong, N> r; for (int i = 0; i < N; ++i) r[i] = name(a[i]); return r; }
#define WMGL_D_BINARY(name) \
template<int N> static inline vec<ulong, N> name(vec<ulong, N> a, vec<ulong, N> b) { vec<ulong, N> r; for (int i = 0; i < N; ++i) r[i] = name(a[i], b[i]); return r; }
#define WMGL_D_TERNARY(name) \
template<int N> static inline vec<ulong, N> name(vec<ulong, N> a, vec<ulong, N> b, vec<ulong, N> c) { vec<ulong, N> r; for (int i = 0; i < N; ++i) r[i] = name(a[i], b[i], c[i]); return r; }
#define WMGL_D_COMPARE(name) \
template<int N> static inline vec<bool, N> name(vec<ulong, N> a, vec<ulong, N> b) { vec<bool, N> r; for (int i = 0; i < N; ++i) r[i] = name(a[i], b[i]); return r; }
#define WMGL_D_TEST(name) \
template<int N> static inline vec<bool, N> name(vec<ulong, N> a) { vec<bool, N> r; for (int i = 0; i < N; ++i) r[i] = name(a[i]); return r; }
#define WMGL_D_TO(name, T) \
template<int N> static inline vec<T, N> name(vec<ulong, N> a) { vec<T, N> r; for (int i = 0; i < N; ++i) r[i] = name(a[i]); return r; }
#define WMGL_D_FROM(name, T) \
template<int N> static inline vec<ulong, N> name(vec<T, N> a) { vec<ulong, N> r; for (int i = 0; i < N; ++i) r[i] = name(a[i]); return r; }

static inline bool wmgl_dcmp_oeq(ulong a, ulong b) { return wmgl_deq(a, b); }
static inline bool wmgl_dcmp_ueq(ulong a, ulong b) { return wmgl_deq(a, b) || wmgl_dunord(a, b); }
static inline bool wmgl_dcmp_one(ulong a, ulong b) { return !wmgl_deq(a, b) && !wmgl_dunord(a, b); }
static inline bool wmgl_dcmp_une(ulong a, ulong b) { return !wmgl_deq(a, b); }
static inline bool wmgl_dcmp_olt(ulong a, ulong b) { return wmgl_dlt(a, b); }
static inline bool wmgl_dcmp_ult(ulong a, ulong b) { return wmgl_dlt(a, b) || wmgl_dunord(a, b); }
static inline bool wmgl_dcmp_ogt(ulong a, ulong b) { return wmgl_dlt(b, a); }
static inline bool wmgl_dcmp_ugt(ulong a, ulong b) { return wmgl_dlt(b, a) || wmgl_dunord(a, b); }
static inline bool wmgl_dcmp_ole(ulong a, ulong b) { return wmgl_dle(a, b); }
static inline bool wmgl_dcmp_ule(ulong a, ulong b) { return wmgl_dle(a, b) || wmgl_dunord(a, b); }
static inline bool wmgl_dcmp_oge(ulong a, ulong b) { return wmgl_dle(b, a); }
static inline bool wmgl_dcmp_uge(ulong a, ulong b) { return wmgl_dle(b, a) || wmgl_dunord(a, b); }
static inline bool wmgl_disnan(ulong a) { return wmgl_d_isnan(a); }
static inline bool wmgl_disinf(ulong a) { return wmgl_d_isinf(a); }
static inline float wmgl_dto_f32(ulong a) { return wmgl_d_to_f32(a); }
static inline ulong wmgl_dfrom_f32(float a) { return wmgl_d_from_f32(a); }
static inline int wmgl_dto_i32(ulong a) { return wmgl_d_to_i32(a); }
static inline uint wmgl_dto_u32(ulong a) { return wmgl_d_to_u32(a); }
static inline ulong wmgl_dfrom_i32(int a) { return wmgl_d_from_i32(a); }
static inline ulong wmgl_dfrom_u32(uint a) { return wmgl_d_from_u32(a); }

WMGL_D_UNARY(wmgl_dneg) WMGL_D_UNARY(wmgl_dabs) WMGL_D_UNARY(wmgl_dsign) WMGL_D_UNARY(wmgl_dsqrt)
WMGL_D_UNARY(wmgl_dinversesqrt) WMGL_D_UNARY(wmgl_dfloor) WMGL_D_UNARY(wmgl_dceil) WMGL_D_UNARY(wmgl_dtrunc)
WMGL_D_UNARY(wmgl_dround) WMGL_D_UNARY(wmgl_dround_even) WMGL_D_UNARY(wmgl_dfract) WMGL_D_UNARY(wmgl_dmodf_int)
WMGL_D_UNARY(wmgl_dmodf_frac) WMGL_D_UNARY(wmgl_dfrexp_mant)
WMGL_D_BINARY(wmgl_dadd) WMGL_D_BINARY(wmgl_dsub) WMGL_D_BINARY(wmgl_dmul) WMGL_D_BINARY(wmgl_ddiv)
WMGL_D_BINARY(wmgl_dmod) WMGL_D_BINARY(wmgl_drem) WMGL_D_BINARY(wmgl_dmin) WMGL_D_BINARY(wmgl_dmax)
WMGL_D_BINARY(wmgl_dstep)
WMGL_D_TERNARY(wmgl_dfma) WMGL_D_TERNARY(wmgl_dclamp) WMGL_D_TERNARY(wmgl_dmix) WMGL_D_TERNARY(wmgl_dsmoothstep)
WMGL_D_COMPARE(wmgl_dcmp_oeq) WMGL_D_COMPARE(wmgl_dcmp_ueq) WMGL_D_COMPARE(wmgl_dcmp_one) WMGL_D_COMPARE(wmgl_dcmp_une)
WMGL_D_COMPARE(wmgl_dcmp_olt) WMGL_D_COMPARE(wmgl_dcmp_ult) WMGL_D_COMPARE(wmgl_dcmp_ogt) WMGL_D_COMPARE(wmgl_dcmp_ugt)
WMGL_D_COMPARE(wmgl_dcmp_ole) WMGL_D_COMPARE(wmgl_dcmp_ule) WMGL_D_COMPARE(wmgl_dcmp_oge) WMGL_D_COMPARE(wmgl_dcmp_uge)
WMGL_D_TEST(wmgl_disnan) WMGL_D_TEST(wmgl_disinf)
WMGL_D_TO(wmgl_dto_f32, float) WMGL_D_TO(wmgl_dto_i32, int) WMGL_D_TO(wmgl_dto_u32, uint) WMGL_D_TO(wmgl_dfrexp_exp, int)
WMGL_D_FROM(wmgl_dfrom_f32, float) WMGL_D_FROM(wmgl_dfrom_i32, int) WMGL_D_FROM(wmgl_dfrom_u32, uint)

template<int N> static inline vec<ulong, N> wmgl_dldexp(vec<ulong, N> a, vec<int, N> e) { vec<ulong, N> r; for (int i = 0; i < N; ++i) r[i] = wmgl_dldexp(a[i], e[i]); return r; }
template<int N> static inline vec<ulong, N> wmgl_dmul_vs(vec<ulong, N> a, ulong s) { vec<ulong, N> r; for (int i = 0; i < N; ++i) r[i] = wmgl_dmul(a[i], s); return r; }
template<int N> static inline vec<ulong, N> wmgl_dmix(vec<ulong, N> a, vec<ulong, N> b, ulong t) { return wmgl_dmix(a, b, vec<ulong, N>(t)); }

static inline ulong wmgl_ddot(ulong a, ulong b) { return wmgl_dmul(a, b); }
template<int N> static inline ulong wmgl_ddot(vec<ulong, N> a, vec<ulong, N> b) {
    ulong r = wmgl_dmul(a[0], b[0]);
    for (int i = 1; i < N; ++i) r = wmgl_dadd(r, wmgl_dmul(a[i], b[i]));
    return r;
}
static inline ulong wmgl_dlength(ulong a) { return wmgl_dabs(a); }
template<int N> static inline ulong wmgl_dlength(vec<ulong, N> a) { return wmgl_dsqrt(wmgl_ddot(a, a)); }
static inline ulong wmgl_ddistance(ulong a, ulong b) { return wmgl_dabs(wmgl_dsub(a, b)); }
template<int N> static inline ulong wmgl_ddistance(vec<ulong, N> a, vec<ulong, N> b) { return wmgl_dlength(wmgl_dsub(a, b)); }
static inline ulong wmgl_dnormalize(ulong a) { return wmgl_dsign(a); }
template<int N> static inline vec<ulong, N> wmgl_dnormalize(vec<ulong, N> a) {
    const ulong len = wmgl_dlength(a);
    vec<ulong, N> r;
    for (int i = 0; i < N; ++i) r[i] = wmgl_ddiv(a[i], len);
    return r;
}
static inline ulong3 wmgl_dcross(ulong3 a, ulong3 b) {
    return ulong3(wmgl_dsub(wmgl_dmul(a.y, b.z), wmgl_dmul(b.y, a.z)), wmgl_dsub(wmgl_dmul(a.z, b.x), wmgl_dmul(b.z, a.x)),
                  wmgl_dsub(wmgl_dmul(a.x, b.y), wmgl_dmul(b.x, a.y)));
}
static inline ulong wmgl_dfaceforward(ulong n, ulong i, ulong nref) { return wmgl_dlt(wmgl_ddot(nref, i), 0ul) ? n : wmgl_dneg(n); }
template<int N> static inline vec<ulong, N> wmgl_dfaceforward(vec<ulong, N> n, vec<ulong, N> i, vec<ulong, N> nref) {
    return wmgl_dlt(wmgl_ddot(nref, i), 0ul) ? n : wmgl_dneg(n);
}
static inline ulong wmgl_dreflect(ulong i, ulong n) {
    return wmgl_dsub(i, wmgl_dmul(wmgl_dmul(0x4000000000000000ul, wmgl_ddot(n, i)), n));
}
template<int N> static inline vec<ulong, N> wmgl_dreflect(vec<ulong, N> i, vec<ulong, N> n) {
    return wmgl_dsub(i, wmgl_dmul_vs(n, wmgl_dmul(0x4000000000000000ul, wmgl_ddot(n, i))));
}
static inline ulong wmgl_drefract(ulong i, ulong n, ulong eta) {
    const ulong d = wmgl_ddot(n, i);
    const ulong k = wmgl_dsub(wmgl_d_one(), wmgl_dmul(wmgl_dmul(eta, eta), wmgl_dsub(wmgl_d_one(), wmgl_dmul(d, d))));
    if (wmgl_dlt(k, 0ul)) return 0ul;
    return wmgl_dsub(wmgl_dmul(eta, i), wmgl_dmul(wmgl_dadd(wmgl_dmul(eta, d), wmgl_dsqrt(k)), n));
}
template<int N> static inline vec<ulong, N> wmgl_drefract(vec<ulong, N> i, vec<ulong, N> n, ulong eta) {
    const ulong d = wmgl_ddot(n, i);
    const ulong k = wmgl_dsub(wmgl_d_one(), wmgl_dmul(wmgl_dmul(eta, eta), wmgl_dsub(wmgl_d_one(), wmgl_dmul(d, d))));
    if (wmgl_dlt(k, 0ul)) return vec<ulong, N>(0ul);
    return wmgl_dsub(wmgl_dmul_vs(i, eta), wmgl_dmul_vs(n, wmgl_dadd(wmgl_dmul(eta, d), wmgl_dsqrt(k))));
}
)WMGL",
R"WMGL(
// Matrices: spvUnsafeArray<vec<ulong, R>, C>, C columns of R rows.
template<int R, size_t C> static inline spvUnsafeArray<vec<ulong, C>, R> wmgl_dtranspose(spvUnsafeArray<vec<ulong, R>, C> m) {
    spvUnsafeArray<vec<ulong, C>, R> t;
    for (int r = 0; r < R; ++r)
        for (size_t c = 0; c < C; ++c) t[r][c] = m[c][r];
    return t;
}
// Column `c` of a row-major matrix stored as R rows of C columns.
template<int C, size_t R> static inline vec<ulong, R> wmgl_dcolumn(spvUnsafeArray<vec<ulong, C>, R> rows, int c) {
    vec<ulong, R> v;
    for (size_t r = 0; r < R; ++r) v[r] = rows[r][c];
    return v;
}
template<int R, size_t C> static inline spvUnsafeArray<vec<ulong, R>, C> wmgl_dmul_ms(spvUnsafeArray<vec<ulong, R>, C> m, ulong s) {
    spvUnsafeArray<vec<ulong, R>, C> o;
    for (size_t c = 0; c < C; ++c) o[c] = wmgl_dmul_vs(m[c], s);
    return o;
}
template<int R, size_t C> static inline vec<ulong, R> wmgl_dmul_mv(spvUnsafeArray<vec<ulong, R>, C> m, vec<ulong, int(C)> v) {
    vec<ulong, R> o = wmgl_dmul_vs(m[0], v[0]);
    for (size_t c = 1; c < C; ++c) o = wmgl_dadd(o, wmgl_dmul_vs(m[c], v[c]));
    return o;
}
template<int R, size_t C> static inline vec<ulong, int(C)> wmgl_dmul_vm(vec<ulong, R> v, spvUnsafeArray<vec<ulong, R>, C> m) {
    vec<ulong, int(C)> o;
    for (size_t c = 0; c < C; ++c) o[c] = wmgl_ddot(v, m[c]);
    return o;
}
template<int R, size_t K, size_t C> static inline spvUnsafeArray<vec<ulong, R>, C> wmgl_dmul_mm(spvUnsafeArray<vec<ulong, R>, K> a, spvUnsafeArray<vec<ulong, int(K)>, C> b) {
    spvUnsafeArray<vec<ulong, R>, C> o;
    for (size_t c = 0; c < C; ++c) o[c] = wmgl_dmul_mv(a, b[c]);
    return o;
}
template<int R, int C> static inline spvUnsafeArray<vec<ulong, R>, size_t(C)> wmgl_douter(vec<ulong, R> c, vec<ulong, C> r) {
    spvUnsafeArray<vec<ulong, R>, size_t(C)> o;
    for (int j = 0; j < C; ++j) o[j] = wmgl_dmul_vs(c, r[j]);
    return o;
}
#define WMGL_D_MAT_BINARY(name) \
template<int R, size_t C> static inline spvUnsafeArray<vec<ulong, R>, C> name(spvUnsafeArray<vec<ulong, R>, C> a, spvUnsafeArray<vec<ulong, R>, C> b) { \
    spvUnsafeArray<vec<ulong, R>, C> o; for (size_t c = 0; c < C; ++c) o[c] = name(a[c], b[c]); return o; }
#define WMGL_D_MAT_UNARY(name) \
template<int R, size_t C> static inline spvUnsafeArray<vec<ulong, R>, C> name(spvUnsafeArray<vec<ulong, R>, C> a) { \
    spvUnsafeArray<vec<ulong, R>, C> o; for (size_t c = 0; c < C; ++c) o[c] = name(a[c]); return o; }
WMGL_D_MAT_BINARY(wmgl_dadd) WMGL_D_MAT_BINARY(wmgl_dsub) WMGL_D_MAT_BINARY(wmgl_dmul) WMGL_D_MAT_BINARY(wmgl_ddiv)
WMGL_D_MAT_UNARY(wmgl_dneg)

static inline ulong wmgl_ddet2(ulong a, ulong b, ulong c, ulong d) { return wmgl_dsub(wmgl_dmul(a, d), wmgl_dmul(b, c)); }
static inline ulong wmgl_ddeterminant(spvUnsafeArray<ulong2, 2> m) { return wmgl_ddet2(m[0][0], m[1][0], m[0][1], m[1][1]); }
static inline ulong wmgl_ddet3(ulong3 c0, ulong3 c1, ulong3 c2) {
    return wmgl_dadd(wmgl_dsub(wmgl_dmul(c0.x, wmgl_ddet2(c1.y, c2.y, c1.z, c2.z)), wmgl_dmul(c1.x, wmgl_ddet2(c0.y, c2.y, c0.z, c2.z))),
                     wmgl_dmul(c2.x, wmgl_ddet2(c0.y, c1.y, c0.z, c1.z)));
}
static inline ulong wmgl_ddeterminant(spvUnsafeArray<ulong3, 3> m) { return wmgl_ddet3(m[0], m[1], m[2]); }
// The 3x3 minor of a 4x4 matrix without column `col` and row `row`.
static inline ulong wmgl_dminor4(spvUnsafeArray<ulong4, 4> m, int col, int row) {
    ulong3 c[3];
    int k = 0;
    for (int j = 0; j < 4; ++j) {
        if (j == col) continue;
        int n = 0;
        for (int i = 0; i < 4; ++i)
            if (i != row) c[k][n++] = m[j][i];
        ++k;
    }
    return wmgl_ddet3(c[0], c[1], c[2]);
}
static inline ulong wmgl_ddeterminant(spvUnsafeArray<ulong4, 4> m) {
    ulong r = 0ul;
    for (int j = 0; j < 4; ++j) {
        const ulong term = wmgl_dmul(m[j][0], wmgl_dminor4(m, j, 0));
        r = (j & 1) ? wmgl_dsub(r, term) : wmgl_dadd(r, term);
    }
    return r;
}
static inline spvUnsafeArray<ulong2, 2> wmgl_dinverse(spvUnsafeArray<ulong2, 2> m) {
    const ulong det = wmgl_ddeterminant(m);
    spvUnsafeArray<ulong2, 2> o;
    o[0] = ulong2(wmgl_ddiv(m[1][1], det), wmgl_ddiv(wmgl_dneg(m[0][1]), det));
    o[1] = ulong2(wmgl_ddiv(wmgl_dneg(m[1][0]), det), wmgl_ddiv(m[0][0], det));
    return o;
}
static inline spvUnsafeArray<ulong3, 3> wmgl_dinverse(spvUnsafeArray<ulong3, 3> m) {
    const ulong det = wmgl_ddeterminant(m);
    spvUnsafeArray<ulong3, 3> o;
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) {
            // inverse[c][r] = cofactor(r, c) / det: the minor without column r and row c.
            const int c0 = r == 0 ? 1 : 0, c1 = r == 2 ? 1 : 2, r0 = c == 0 ? 1 : 0, r1 = c == 2 ? 1 : 2;
            ulong minor = wmgl_ddet2(m[c0][r0], m[c1][r0], m[c0][r1], m[c1][r1]);
            if (((c + r) & 1) != 0) minor = wmgl_dneg(minor);
            o[c][r] = wmgl_ddiv(minor, det);
        }
    return o;
}
static inline spvUnsafeArray<ulong4, 4> wmgl_dinverse(spvUnsafeArray<ulong4, 4> m) {
    const ulong det = wmgl_ddeterminant(m);
    spvUnsafeArray<ulong4, 4> o;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            ulong minor = wmgl_dminor4(m, r, c);
            if (((c + r) & 1) != 0) minor = wmgl_dneg(minor);
            o[c][r] = wmgl_ddiv(minor, det);
        }
    return o;
}
)WMGL"
