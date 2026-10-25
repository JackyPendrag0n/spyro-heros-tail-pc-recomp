/* NV2A vertex program microcode -> GLSL 3.30.
 *
 * Each instruction is 4 DWORDs (the first is unused padding in the D3D token
 * stream).  Field layout and unit semantics follow the NV2A hardware:
 *   - a MAC unit (vector ops) and an ILU unit (scalar ops on input C) run as a pair
 *   - when both run, ILU temp writes go to R1 and MAC writes to R1 are dropped
 *   - one output (oN or a constant) can be written per instruction, chosen by OUT_MUX
 *   - R12 is the same register as oPos
 * The program writes oPos in screen space; the wrapper converts back to clip space. */
#include "gfx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned fld(const u32 *t, int word, int start, int len)
{
    return (t[word] >> start) & ((1u << len) - 1);
}
#define F_ILU(t)       fld(t, 1, 25, 3)
#define F_MAC(t)       fld(t, 1, 21, 4)
#define F_CONST(t)     fld(t, 1, 13, 8)
#define F_V(t)         fld(t, 1, 9, 4)
#define F_A_NEG(t)     fld(t, 1, 8, 1)
#define F_A_SWZ(t, c)  fld(t, 1, 6 - 2 * (c), 2)
#define F_A_R(t)       fld(t, 2, 28, 4)
#define F_A_MUX(t)     fld(t, 2, 26, 2)
#define F_B_NEG(t)     fld(t, 2, 25, 1)
#define F_B_SWZ(t, c)  fld(t, 2, 23 - 2 * (c), 2)
#define F_B_R(t)       fld(t, 2, 13, 4)
#define F_B_MUX(t)     fld(t, 2, 11, 2)
#define F_C_NEG(t)     fld(t, 2, 10, 1)
#define F_C_SWZ(t, c)  fld(t, 2, 8 - 2 * (c), 2)
#define F_C_R(t)       ((fld(t, 2, 0, 2) << 2) | fld(t, 3, 30, 2))
#define F_C_MUX(t)     fld(t, 3, 28, 2)
#define F_OUT_MACMASK(t) fld(t, 3, 24, 4)
#define F_OUT_R(t)     fld(t, 3, 20, 4)
#define F_OUT_ILUMASK(t) fld(t, 3, 16, 4)
#define F_OUT_OMASK(t) fld(t, 3, 12, 4)
#define F_OUT_ORB(t)   fld(t, 3, 11, 1)
#define F_OUT_ADDR(t)  fld(t, 3, 3, 8)
#define F_OUT_MUX(t)   fld(t, 3, 2, 1)
#define F_A0X(t)       fld(t, 3, 1, 1)
#define F_FINAL(t)     fld(t, 3, 0, 1)

enum { MUX_R = 1, MUX_V = 2, MUX_C = 3 };
enum { ILU_NOP, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ, ILU_EXP, ILU_LOG, ILU_LIT };
enum { MAC_NOP, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4,
       MAC_DST, MAC_MIN, MAC_MAX, MAC_SLT, MAC_SGE, MAC_ARL };

static const char *out_names[16] = {
    "oPos", "oUnused1", "oUnused2", "oD0", "oD1", "oFog", "oPts", "oB0",
    "oB1", "oT0", "oT1", "oT2", "oT3", "oUnused13", "oUnused14", "oUnused15",
};

struct sb { char *p; size_t n, cap; };
static void sbf(struct sb *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sbf(struct sb *s, const char *fmt, ...)
{
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int k = vsnprintf(s->p ? s->p + s->n : NULL, s->p ? s->cap - s->n : 0, fmt, ap);
        va_end(ap);
        if (s->p && s->n + k < s->cap) { s->n += k; return; }
        s->cap = (s->cap + k + 1) * 2;
        s->p = realloc(s->p, s->cap);
    }
}

static const char swz[] = "xyzw";

/* Source operand as a vec4 GLSL expression. which: 0=A 1=B 2=C */
static void operand(struct sb *s, const u32 *t, int which, int scalar_c)
{
    unsigned mux, neg, reg;
    unsigned sw[4];
    if (which == 0) { mux = F_A_MUX(t); neg = F_A_NEG(t); reg = F_A_R(t); for (int c = 0; c < 4; c++) sw[c] = F_A_SWZ(t, c); }
    else if (which == 1) { mux = F_B_MUX(t); neg = F_B_NEG(t); reg = F_B_R(t); for (int c = 0; c < 4; c++) sw[c] = F_B_SWZ(t, c); }
    else { mux = F_C_MUX(t); neg = F_C_NEG(t); reg = F_C_R(t); for (int c = 0; c < 4; c++) sw[c] = F_C_SWZ(t, c); }
    if (scalar_c) sw[1] = sw[2] = sw[3] = sw[0];
    if (neg) sbf(s, "-");
    switch (mux) {
    case MUX_R: sbf(s, "R%u", reg); break;
    case MUX_V: sbf(s, "v%u", F_V(t)); break;
    case MUX_C:
        if (F_A0X(t)) sbf(s, "c[clamp(A0 + %u, 0, 191)]", F_CONST(t));
        else sbf(s, "c[%u]", F_CONST(t));
        break;
    default: sbf(s, "vec4(0.0)"); break;
    }
    if (!(sw[0] == 0 && sw[1] == 1 && sw[2] == 2 && sw[3] == 3))
        sbf(s, ".%c%c%c%c", swz[sw[0]], swz[sw[1]], swz[sw[2]], swz[sw[3]]);
}

static void mask_str(char *out, unsigned m)
{
    int k = 0;
    out[k++] = '.';
    if (m & 8) out[k++] = 'x';
    if (m & 4) out[k++] = 'y';
    if (m & 2) out[k++] = 'z';
    if (m & 1) out[k++] = 'w';
    out[k] = 0;
}

/* assign dst<mask> = expr<mask> */
static void assign(struct sb *s, const char *dst, unsigned mask, const char *val)
{
    char m[6];
    mask_str(m, mask);
    sbf(s, "  %s%s = (%s)%s;\n", dst, m, val, m);
}

/* Translate a microcode program. tokens: 4 DWORDs per instruction. */
char *vsh_translate(const u32 *tokens, int ninstr, int *writes_consts)
{
    struct sb s = { 0 };
    struct sb e = { 0 };
    *writes_consts = 0;
    for (int i = 0; i < ninstr; i++) {
        const u32 *t = tokens + 4 * i;
        unsigned ilu = F_ILU(t), mac = F_MAC(t);
        int paired = ilu != ILU_NOP && mac != MAC_NOP;
        sbf(&s, "  /* %d: %08x %08x %08x %08x mac=%u ilu=%u */\n", i, t[0], t[1], t[2], t[3], mac, ilu);

        /* compute unit results into temporaries first (reads see old values) */
        if (mac != MAC_NOP) {
            e.n = 0;
            struct sb *x = &e;
            char A[160], B[160], C[160];
            struct sb ta = { 0 }, tb = { 0 }, tc = { 0 };
            operand(&ta, t, 0, 0); operand(&tb, t, 1, 0); operand(&tc, t, 2, 0);
            snprintf(A, sizeof A, "%s", ta.p); snprintf(B, sizeof B, "%s", tb.p); snprintf(C, sizeof C, "%s", tc.p);
            free(ta.p); free(tb.p); free(tc.p);
            switch (mac) {
            case MAC_MOV: sbf(x, "%s", A); break;
            case MAC_MUL: sbf(x, "(%s) * (%s)", A, B); break;
            case MAC_ADD: sbf(x, "(%s) + (%s)", A, C); break;
            case MAC_MAD: sbf(x, "(%s) * (%s) + (%s)", A, B, C); break;
            case MAC_DP3: sbf(x, "vec4(dot((%s).xyz, (%s).xyz))", A, B); break;
            case MAC_DPH: sbf(x, "vec4(dot((%s).xyz, (%s).xyz) + (%s).w)", A, B, B); break;
            case MAC_DP4: sbf(x, "vec4(dot(%s, %s))", A, B); break;
            case MAC_DST: sbf(x, "vec4(1.0, (%s).y * (%s).y, (%s).z, (%s).w)", A, B, A, B); break;
            case MAC_MIN: sbf(x, "min(%s, %s)", A, B); break;
            case MAC_MAX: sbf(x, "max(%s, %s)", A, B); break;
            case MAC_SLT: sbf(x, "vec4(lessThan(%s, %s))", A, B); break;
            case MAC_SGE: sbf(x, "vec4(greaterThanEqual(%s, %s))", A, B); break;
            case MAC_ARL: sbf(x, "%s", A); break;
            default: sbf(x, "vec4(0.0)"); break;
            }
            sbf(&s, "  macT = %s;\n", e.p);
        }
        if (ilu != ILU_NOP) {
            e.n = 0;
            struct sb tc = { 0 };
            operand(&tc, t, 2, 1);
            char C[200];
            snprintf(C, sizeof C, "(%s).x", tc.p);
            free(tc.p);
            switch (ilu) {
            case ILU_MOV: {     /* vector move of the swizzled C operand */
                struct sb full = { 0 };
                operand(&full, t, 2, 0);
                sbf(&e, "(%s)", full.p);
                free(full.p);
                break; }
            case ILU_RCP: sbf(&e, "vec4(xrcp(%s))", C); break;
            case ILU_RCC: sbf(&e, "vec4(xrcc(%s))", C); break;
            case ILU_RSQ: sbf(&e, "vec4(xrsq(%s))", C); break;
            case ILU_EXP: sbf(&e, "xexp(%s)", C); break;
            case ILU_LOG: sbf(&e, "xlog(%s)", C); break;
            case ILU_LIT: {
                struct sb full = { 0 };
                operand(&full, t, 2, 0);
                sbf(&e, "xlit(%s)", full.p);
                free(full.p);
                break; }
            }
            sbf(&s, "  iluT = %s;\n", e.p);
        }

        /* temp-register writes */
        if (mac != MAC_NOP) {
            unsigned m = F_OUT_MACMASK(t), r = F_OUT_R(t);
            if (mac == MAC_ARL) {
                sbf(&s, "  A0 = int(floor(macT.x + 0.001));\n");
            } else if (m && !(paired && r == 1)) {
                char dst[8]; snprintf(dst, sizeof dst, "R%u", r);
                assign(&s, dst, m, "macT");
            }
        }
        if (ilu != ILU_NOP) {
            unsigned m = F_OUT_ILUMASK(t), r = paired ? 1 : F_OUT_R(t);
            if (m) { char dst[8]; snprintf(dst, sizeof dst, "R%u", r); assign(&s, dst, m, "iluT"); }
        }
        /* output / constant write, from whichever unit OUT_MUX selects */
        unsigned om = F_OUT_OMASK(t);
        if (om) {
            const char *src = F_OUT_MUX(t) ? "iluT" : "macT";
            int src_active = F_OUT_MUX(t) ? ilu != ILU_NOP : (mac != MAC_NOP && mac != MAC_ARL);
            if (src_active) {
                if (F_OUT_ORB(t) == 0) {
                    char dst[32];
                    snprintf(dst, sizeof dst, "c[%u]", F_OUT_ADDR(t));
                    assign(&s, dst, om, src);
                    *writes_consts = 1;
                } else {
                    unsigned o = F_OUT_ADDR(t) & 0xF;
                    if (o == 5) {   /* oFog: a scalar; the first masked component is written */
                        char m[6]; mask_str(m, om);
                        sbf(&s, "  oFog = vec4((%s).%c);\n", src, m[1]);
                    } else {
                        assign(&s, out_names[o], om, src);
                    }
                }
            }
        }
        if (F_FINAL(t)) break;
    }
    free(e.p);
    return s.p ? s.p : strdup("");
}

/* GLSL helpers shared by every translated program. */
const char *vsh_helpers =
    "float xrcp(float x) { return x == 0.0 ? 1.0e30 : 1.0 / x; }\n"
    "float xrcc(float x) { float r = xrcp(x);\n"
    "  if (r > 0.0) r = clamp(r, 5.42101e-20, 1.884467e19); else r = clamp(r, -1.884467e19, -5.42101e-20); return r; }\n"
    "float xrsq(float x) { x = abs(x); return x == 0.0 ? 1.0e30 : inversesqrt(x); }\n"
    "vec4 xexp(float x) { float f = floor(x); return vec4(exp2(f), x - f, exp2(x), 1.0); }\n"
    "vec4 xlog(float x) { x = abs(x); if (x == 0.0) return vec4(-1.0e30, 1.0, -1.0e30, 1.0);\n"
    "  float e = floor(log2(x)); return vec4(e, x / exp2(e), log2(x), 1.0); }\n"
    "vec4 xlit(vec4 s) { float d = max(s.x, 0.0), sp = max(s.y, 0.0); float p = clamp(s.w, -127.9961, 127.9961);\n"
    "  return vec4(1.0, d, (s.x > 0.0) ? pow(sp, p) : 0.0, 1.0); }\n";

/* ---- CPU reference interpreter (diagnostics): runs a program on one vertex ---- */
#include <math.h>
static void cpu_operand(const u32 *t, int which, int scalar_c, const float (*R)[4], const float (*v)[4],
                        const float (*c)[4], int a0, float *o)
{
    unsigned mux, neg, reg, sw[4];
    if (which == 0) { mux = F_A_MUX(t); neg = F_A_NEG(t); reg = F_A_R(t); for (int k = 0; k < 4; k++) sw[k] = F_A_SWZ(t, k); }
    else if (which == 1) { mux = F_B_MUX(t); neg = F_B_NEG(t); reg = F_B_R(t); for (int k = 0; k < 4; k++) sw[k] = F_B_SWZ(t, k); }
    else { mux = F_C_MUX(t); neg = F_C_NEG(t); reg = F_C_R(t); for (int k = 0; k < 4; k++) sw[k] = F_C_SWZ(t, k); }
    if (scalar_c) sw[1] = sw[2] = sw[3] = sw[0];
    const float *src = NULL; static const float zero[4];
    if (mux == MUX_R) src = R[reg];
    else if (mux == MUX_V) src = v[F_V(t)];
    else if (mux == MUX_C) { int i = F_CONST(t) + (F_A0X(t) ? a0 : 0); if (i < 0) i = 0; if (i > 191) i = 191; src = c[i]; }
    else src = zero;
    for (int k = 0; k < 4; k++) o[k] = neg ? -src[sw[k]] : src[sw[k]];
}

/* out[16][4] receives o-registers (index 0 = oPos). Returns instructions executed. */
int vsh_cpu_run(const u32 *tokens, int ninstr, const float (*v)[4], const float (*cin)[4], float (*out)[4])
{
    float R[13][4] = { { 0 } };
    static float c[192][4];
    memcpy(c, cin, sizeof c);
    int a0 = 0;
    for (int i = 0; i < 16; i++) { out[i][0] = out[i][1] = out[i][2] = 0; out[i][3] = 1; }
    int i;
    for (i = 0; i < ninstr; i++) {
        const u32 *t = tokens + 4 * i;
        unsigned ilu = F_ILU(t), mac = F_MAC(t);
        int paired = ilu != ILU_NOP && mac != MAC_NOP;
        /* R12 is oPos */
        memcpy(R[12], out[0], 16);
        float A[4], B[4], C[4], m[4] = { 0 }, l[4] = { 0 };
        cpu_operand(t, 0, 0, (const float (*)[4])R, v, (const float (*)[4])c, a0, A);
        cpu_operand(t, 1, 0, (const float (*)[4])R, v, (const float (*)[4])c, a0, B);
        cpu_operand(t, 2, 0, (const float (*)[4])R, v, (const float (*)[4])c, a0, C);
        switch (mac) {
        case MAC_MOV: case MAC_ARL: memcpy(m, A, 16); break;
        case MAC_MUL: for (int k = 0; k < 4; k++) m[k] = A[k] * B[k]; break;
        case MAC_ADD: for (int k = 0; k < 4; k++) m[k] = A[k] + C[k]; break;
        case MAC_MAD: for (int k = 0; k < 4; k++) m[k] = A[k] * B[k] + C[k]; break;
        case MAC_DP3: m[0] = m[1] = m[2] = m[3] = A[0]*B[0] + A[1]*B[1] + A[2]*B[2]; break;
        case MAC_DPH: m[0] = m[1] = m[2] = m[3] = A[0]*B[0] + A[1]*B[1] + A[2]*B[2] + B[3]; break;
        case MAC_DP4: m[0] = m[1] = m[2] = m[3] = A[0]*B[0] + A[1]*B[1] + A[2]*B[2] + A[3]*B[3]; break;
        case MAC_DST: m[0] = 1; m[1] = A[1] * B[1]; m[2] = A[2]; m[3] = B[3]; break;
        case MAC_MIN: for (int k = 0; k < 4; k++) m[k] = A[k] < B[k] ? A[k] : B[k]; break;
        case MAC_MAX: for (int k = 0; k < 4; k++) m[k] = A[k] > B[k] ? A[k] : B[k]; break;
        case MAC_SLT: for (int k = 0; k < 4; k++) m[k] = A[k] < B[k]; break;
        case MAC_SGE: for (int k = 0; k < 4; k++) m[k] = A[k] >= B[k]; break;
        }
        if (ilu != ILU_NOP) {
            float x; float Cs[4];
            cpu_operand(t, 2, 1, (const float (*)[4])R, v, (const float (*)[4])c, a0, Cs);
            x = Cs[0];
            switch (ilu) {
            case ILU_MOV: memcpy(l, C, 16); break;
            case ILU_RCP: l[0] = l[1] = l[2] = l[3] = x == 0 ? 1e30f : 1 / x; break;
            case ILU_RCC: { float r = x == 0 ? 1e30f : 1 / x;
                if (r > 0) r = fminf(fmaxf(r, 5.42101e-20f), 1.884467e19f); else r = fminf(fmaxf(r, -1.884467e19f), -5.42101e-20f);
                l[0] = l[1] = l[2] = l[3] = r; break; }
            case ILU_RSQ: l[0] = l[1] = l[2] = l[3] = x == 0 ? 1e30f : 1 / sqrtf(fabsf(x)); break;
            case ILU_EXP: l[0] = exp2f(floorf(x)); l[1] = x - floorf(x); l[2] = exp2f(x); l[3] = 1; break;
            case ILU_LOG: { float ax = fabsf(x), e = floorf(log2f(ax)); l[0] = e; l[1] = ax / exp2f(e); l[2] = log2f(ax); l[3] = 1; break; }
            case ILU_LIT: l[0] = 1; l[1] = fmaxf(C[0], 0); l[2] = C[0] > 0 ? powf(fmaxf(C[1], 0), C[3]) : 0; l[3] = 1; break;
            }
        }
        if (mac != MAC_NOP) {
            unsigned mm = F_OUT_MACMASK(t), r = F_OUT_R(t);
            if (mac == MAC_ARL) a0 = (int)floorf(m[0] + 0.001f);
            else if (mm && !(paired && r == 1)) for (int k = 0; k < 4; k++) if (mm & (8 >> k)) R[r][k] = m[k];
        }
        if (ilu != ILU_NOP) {
            unsigned mm = F_OUT_ILUMASK(t), r = paired ? 1 : F_OUT_R(t);
            for (int k = 0; k < 4; k++) if (mm & (8 >> k)) R[r][k] = l[k];
        }
        /* R12 written -> oPos */
        memcpy(out[0], R[12], 16);
        unsigned om = F_OUT_OMASK(t);
        if (om) {
            const float *src = F_OUT_MUX(t) ? l : m;
            int act = F_OUT_MUX(t) ? ilu != ILU_NOP : (mac != MAC_NOP && mac != MAC_ARL);
            if (act) {
                if (!F_OUT_ORB(t)) { for (int k = 0; k < 4; k++) if (om & (8 >> k)) c[F_OUT_ADDR(t)][k] = src[k]; }
                else { unsigned o = F_OUT_ADDR(t) & 0xF; for (int k = 0; k < 4; k++) if (om & (8 >> k)) out[o][k] = src[k];
                       if (o == 0) memcpy(R[12], out[0], 16); }
            }
        }
        if (F_FINAL(t)) { i++; break; }
    }
    return i;
}
