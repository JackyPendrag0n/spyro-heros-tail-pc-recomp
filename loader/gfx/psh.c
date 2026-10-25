/* Pixel pipeline -> GLSL fragment shader.
 *
 * Two sources:
 *  - no pixel shader set: D3D fixed-function texture stages (deferred TSS table)
 *  - pixel shader set: NV2A register combiners from the PS render-state block
 * Fog and alpha test are appended in both cases.
 * Uniforms: tex0..3, texScale[4] (1/size for linear textures), tfactor,
 * fogColor, fogParams(start, end, density, mode), alphaRef/func,
 * pc0[8], pc1[8] (combiner constants), fc0, fc1 (final combiner constants). */
#include "gfx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static u64 mix(u64 h, u64 v) { h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2); return h; }

static void sample_stage(struct sb *s, int i, int kind, int projected)
{
    if (i == 9) {   /* diagnostic: write stage 0's texture straight to the output */
        if (kind == 1) sbf(s, "  fragColor = vec4(texture(tex0, vT0.xy).rgb, 1.0);\n");
        else if (kind == 2) sbf(s, "  fragColor = vec4(texture(tex0, vT0.xy * texScale[0]).rgb, 1.0);\n");
        else sbf(s, "  fragColor = vec4(1.0, 0.0, 1.0, 1.0);\n");
        return;
    }
    /* kind: 0 none, 1 2D, 2 2D linear (texel coords), 3 cube, 4 3D */
    switch (kind) {
    case 1: if (projected) sbf(s, "  vec4 t%d = texture(tex%d, (vT%d.xy / vT%d.w) * texScale[%d]);\n", i, i, i, i, i);
            else sbf(s, "  vec4 t%d = texture(tex%d, vT%d.xy * texScale[%d]);\n", i, i, i, i); break;
    case 2: if (projected) sbf(s, "  vec4 t%d = texture(tex%d, (vT%d.xy / vT%d.w) * texScale[%d]);\n", i, i, i, i, i);
            else sbf(s, "  vec4 t%d = texture(tex%d, vT%d.xy * texScale[%d]);\n", i, i, i, i); break;
    case 3: sbf(s, "  vec4 t%d = texture(tex%dc, vT%d.xyz);\n", i, i, i); break;
    case 4: sbf(s, "  vec4 t%d = texture(tex%d3, vT%d.xyz);\n", i, i, i); break;
    default: sbf(s, "  vec4 t%d = vec4(0.0, 0.0, 0.0, 1.0);\n", i); break;
    }
}

/* ---------- fixed function ---------- */
static void ff_arg(struct sb *s, u32 a, int stage)
{
    const char *base;
    char tb[8];
    switch (a & 0xF) {
    case 0: base = "vD0"; break;
    case 1: base = stage == 0 ? "vD0" : "cur"; break;
    case 2: snprintf(tb, sizeof tb, "t%d", stage); base = tb; break;
    case 3: base = "tfactor"; break;
    case 4: base = "vD1"; break;
    case 5: base = "tmp"; break;
    default: base = "vec4(1.0)"; break;
    }
    if (a & 0x20) sbf(s, (a & 0x10) ? "(1.0 - vec4(%s.a))" : "vec4(%s.a)", base);
    else sbf(s, (a & 0x10) ? "(1.0 - %s)" : "%s", base);
}

static void ff_op(struct sb *s, u32 op, const char *a0, const char *a1, const char *a2, int stage)
{
    switch (op) {
    case 2: sbf(s, "%s", a1); break;
    case 3: sbf(s, "%s", a2); break;
    case 4: sbf(s, "%s * %s", a1, a2); break;
    case 5: sbf(s, "%s * %s * 2.0", a1, a2); break;
    case 6: sbf(s, "%s * %s * 4.0", a1, a2); break;
    case 7: sbf(s, "%s + %s", a1, a2); break;
    case 8: sbf(s, "%s + %s - 0.5", a1, a2); break;
    case 9: sbf(s, "(%s + %s - 0.5) * 2.0", a1, a2); break;
    case 10: sbf(s, "%s - %s", a1, a2); break;
    case 11: sbf(s, "%s + %s - %s * %s", a1, a2, a1, a2); break;
    case 12: sbf(s, "mix(%s, %s, vD0.a)", a2, a1); break;
    case 13: sbf(s, "mix(%s, %s, %s.a)", a2, a1, stage == 0 ? "vD0" : "cur"); break;
    case 14: sbf(s, "mix(%s, %s, t%d.a)", a2, a1, stage); break;
    case 15: sbf(s, "mix(%s, %s, tfactor.a)", a2, a1); break;
    case 16: sbf(s, "%s + %s * (1.0 - t%d.a)", a1, a2, stage); break;
    case 17: sbf(s, "%s", a1); break;
    case 18: sbf(s, "vec4(%s.rgb + %s.a * %s.rgb, %s.a)", a1, a1, a2, a1); break;
    case 19: sbf(s, "vec4(%s.rgb * %s.rgb + %s.a, %s.a)", a1, a2, a1, a1); break;
    case 20: sbf(s, "vec4((1.0 - %s.a) * %s.rgb + %s.rgb, %s.a)", a1, a2, a1, a1); break;
    case 21: sbf(s, "vec4((1.0 - %s.rgb) * %s.rgb + %s.a, %s.a)", a1, a2, a1, a1); break;
    case 22: sbf(s, "vec4(clamp(dot((%s.rgb - 0.5) * 2.0, (%s.rgb - 0.5) * 2.0), 0.0, 1.0))", a1, a2); break;
    case 23: sbf(s, "%s + %s * %s", a0, a1, a2); break;
    case 24: sbf(s, "mix(%s, %s, %s)", a2, a1, a0); break;
    default: sbf(s, "%s", a1); break;   /* bump env map: approximated */
    }
}

/* set by the backend: a vertex program is active. NV2A samples 2D textures with PROJECT2D, so
 * the program's oTn.w always divides (D3D only arranges q = 1 for the fixed-function pipeline). */
int psh_vs_program;

static void gen_fixed(struct sb *s, const u32 (*tss)[XTSS_COUNT], const int kind[4], u64 *key)
{
    sbf(s, "  vec4 cur = vD0;\n  vec4 tmp = vec4(0.0);\n");
    for (int i = 0; i < 4; i++) {
        const u32 *t = tss[i];
        u32 cop = t[XTSS_COLOROP], aop = t[XTSS_ALPHAOP];
        *key = mix(*key, cop | aop << 8 | t[XTSS_COLORARG1] << 16 | t[XTSS_COLORARG2] << 24);
        *key = mix(*key, t[XTSS_ALPHAARG1] | t[XTSS_ALPHAARG2] << 8 | t[XTSS_COLORARG0] << 16 | t[XTSS_ALPHAARG0] << 24);
        *key = mix(*key, t[XTSS_RESULTARG] | (u64)kind[i] << 8 | (u64)(t[XTSS_TEXTURETRANSFORMFLAGS] & 0x100) << 16);
        if (cop <= 1) break;                                    /* DISABLE ends the cascade */
        sample_stage(s, i, kind[i], psh_vs_program || (t[XTSS_TEXTURETRANSFORMFLAGS] & 0x100) != 0);
        struct sb a[3] = { { 0 }, { 0 }, { 0 } };
        u32 ca[3] = { t[XTSS_COLORARG0], t[XTSS_COLORARG1], t[XTSS_COLORARG2] };
        u32 aa[3] = { t[XTSS_ALPHAARG0], t[XTSS_ALPHAARG1], t[XTSS_ALPHAARG2] };
        for (int k = 0; k < 3; k++) ff_arg(&a[k], ca[k], i);
        struct sb c = { 0 };
        ff_op(&c, cop, a[0].p, a[1].p, a[2].p, i);
        for (int k = 0; k < 3; k++) { a[k].n = 0; ff_arg(&a[k], aa[k], i); }
        struct sb al = { 0 };
        if (aop > 1) ff_op(&al, aop, a[0].p, a[1].p, a[2].p, i);
        const char *dst = t[XTSS_RESULTARG] == 5 ? "tmp" : "cur";
        sbf(s, "  { vec4 rc = clamp(%s, 0.0, 1.0);\n", c.p);
        if (aop > 1) sbf(s, "    float ra = clamp((%s).a, 0.0, 1.0);\n", al.p);
        else sbf(s, "    float ra = %s.a;\n", i == 0 ? "vD0" : "cur");
        sbf(s, "    %s = vec4(rc.rgb, ra); }\n", dst);
        for (int k = 0; k < 3; k++) free(a[k].p);
        free(c.p); free(al.p);
    }
    sbf(s, "  vec4 col = cur;\n");
}

/* ---------- register combiners ---------- */
static const char *reg_name(u32 r, int final)
{
    switch (r & 0xF) {
    case 0: return "vec4(0.0)";
    case 1: return final ? "fc0" : "c0";
    case 2: return final ? "fc1" : "c1";
    case 3: return "vec4(vFog.x)";
    case 4: return "vD0";
    case 5: return "vD1";
    case 8: return "t0";
    case 9: return "t1";
    case 10: return "t2";
    case 11: return "t3";
    case 12: return "r0";
    case 13: return "r1";
    case 14: return final ? "v1r0" : "vec4(0.0)";
    case 15: return final ? "efprod" : "vec4(0.0)";
    }
    return "vec4(0.0)";
}

/* one input byte: reg(0-3) | alpha(4) | mapping(5-7); is_alpha: alpha portion (channel 0 = blue) */
static void rc_input(struct sb *s, u32 in, int is_alpha, int final)
{
    const char *r = reg_name(in, final);
    char v[96];
    int alpha = (in >> 4) & 1;
    if (is_alpha) snprintf(v, sizeof v, alpha ? "vec4((%s).a)" : "vec4((%s).b)", r);
    else snprintf(v, sizeof v, alpha ? "vec4((%s).a)" : "(%s)", r);
    switch ((in >> 5) & 7) {
    case 0: sbf(s, "clamp(%s, 0.0, 1.0)", v); break;               /* UNSIGNED_IDENTITY */
    case 1: sbf(s, "(1.0 - clamp(%s, 0.0, 1.0))", v); break;       /* UNSIGNED_INVERT */
    case 2: sbf(s, "(2.0 * max(%s, 0.0) - 1.0)", v); break;        /* EXPAND_NORMAL */
    case 3: sbf(s, "(-2.0 * max(%s, 0.0) + 1.0)", v); break;       /* EXPAND_NEGATE */
    case 4: sbf(s, "(max(%s, 0.0) - 0.5)", v); break;              /* HALFBIAS_NORMAL */
    case 5: sbf(s, "(-max(%s, 0.0) + 0.5)", v); break;             /* HALFBIAS_NEGATE */
    case 6: sbf(s, "(%s)", v); break;                              /* SIGNED_IDENTITY */
    case 7: sbf(s, "(-(%s))", v); break;                           /* SIGNED_NEGATE */
    }
}

static const char *out_reg(u32 r)
{
    switch (r & 0xF) {
    case 4: return "vD0"; case 5: return "vD1";
    case 8: return "t0"; case 9: return "t1"; case 10: return "t2"; case 11: return "t3";
    case 12: return "r0"; case 13: return "r1";
    }
    return NULL;   /* 0 = discard */
}

static void rc_stage(struct sb *s, int stage, u32 inputs, u32 outputs, int is_alpha, int mux_msb)
{
    const char *m = is_alpha ? "a" : "rgb";
    struct sb in[4] = { { 0 }, { 0 }, { 0 }, { 0 } };
    for (int k = 0; k < 4; k++) rc_input(&in[k], (inputs >> (24 - 8 * k)) & 0xFF, is_alpha, 0);
    u32 cd = outputs & 0xF, ab = (outputs >> 4) & 0xF, sum = (outputs >> 8) & 0xF;
    u32 flags = outputs >> 12;
    int cd_dot = !is_alpha && (flags & 1), ab_dot = !is_alpha && (flags & 2), muxsum = flags & 4;
    u32 mapping = (flags >> 3) & 7;
    const char *map_fmt;
    switch (mapping) {
    case 1: map_fmt = "((%s) - 0.5)"; break;
    case 2: map_fmt = "((%s) * 2.0)"; break;
    case 3: map_fmt = "(((%s) - 0.5) * 2.0)"; break;
    case 4: map_fmt = "((%s) * 4.0)"; break;
    case 6: map_fmt = "((%s) / 2.0)"; break;
    default: map_fmt = "(%s)"; break;
    }
    sbf(s, "  { /* combiner %d %s */\n", stage, is_alpha ? "alpha" : "rgb");
    if (ab_dot) sbf(s, "    vec4 ab = vec4(dot(%s.rgb, %s.rgb));\n", in[0].p, in[1].p);
    else sbf(s, "    vec4 ab = %s * %s;\n", in[0].p, in[1].p);
    if (cd_dot) sbf(s, "    vec4 cd = vec4(dot(%s.rgb, %s.rgb));\n", in[2].p, in[3].p);
    else sbf(s, "    vec4 cd = %s * %s;\n", in[2].p, in[3].p);
    if (muxsum) sbf(s, "    vec4 ms = (%s) ? cd : ab;\n", mux_msb ? "r0.a >= 0.5" : "(uint(r0.a * 255.0) & 1u) == 1u");
    else sbf(s, "    vec4 ms = ab + cd;\n");
    char tmp[64];
    snprintf(tmp, sizeof tmp, map_fmt, "ab"); sbf(s, "    ab = clamp(%s, -1.0, 1.0);\n", tmp);
    snprintf(tmp, sizeof tmp, map_fmt, "cd"); sbf(s, "    cd = clamp(%s, -1.0, 1.0);\n", tmp);
    snprintf(tmp, sizeof tmp, map_fmt, "ms"); sbf(s, "    ms = clamp(%s, -1.0, 1.0);\n", tmp);
    const char *o;
    if ((o = out_reg(ab))) {
        sbf(s, "    %s.%s = ab.%s;\n", o, m, m);
        if (!is_alpha && (flags & 0x40)) sbf(s, "    %s.a = ab.b;\n", o);   /* AB blue-to-alpha */
    }
    if ((o = out_reg(cd))) {
        sbf(s, "    %s.%s = cd.%s;\n", o, m, m);
        if (!is_alpha && (flags & 0x80)) sbf(s, "    %s.a = cd.b;\n", o);   /* CD blue-to-alpha */
    }
    if ((o = out_reg(sum))) sbf(s, "    %s.%s = ms.%s;\n", o, m, m);
    sbf(s, "  }\n");
    for (int k = 0; k < 4; k++) free(in[k].p);
}

static void gen_combiners(struct sb *s, const u32 *rs, const int kind[4], u64 *key)
{
    u32 cc = rs[XD3DRS_PSCOMBINERCOUNT];
    int n = cc & 0xFF;
    if (n < 1) n = 1;
    if (n > 8) n = 8;
    int mux_msb = (cc >> 8) & 1;
    u32 texmodes = rs[XD3DRS_PSTEXTUREMODES];
    *key = mix(*key, cc);
    *key = mix(*key, texmodes);
    for (int i = 0; i < 4; i++) {
        u32 mode = (texmodes >> (5 * i)) & 0x1F;
        *key = mix(*key, kind[i]);
        switch (mode) {
        case 0: sbf(s, "  vec4 t%d = vec4(0.0);\n", i); break;
        case 1: case 2: case 3: sample_stage(s, i, kind[i], mode == 2 && kind[i] < 3); break;
        case 4: sbf(s, "  vec4 t%d = clamp(vT%d, 0.0, 1.0);\n", i, i); break;              /* PASSTHRU */
        case 5: sbf(s, "  vec4 t%d = vec4(0.0);\n  if (any(lessThan(vT%d, vec4(0.0)))) discard;\n", i, i); break; /* CLIPPLANE */
        default: sample_stage(s, i, kind[i], 0); break;   /* dependent / dot modes: approximated */
        }
    }
    sbf(s, "  vec4 r0 = vec4(0.0), r1 = vec4(0.0);\n");
    sbf(s, "  r0.a = %s;\n", (texmodes & 0x1F) ? "t0.a" : "1.0");
    for (int i = 0; i < n; i++) {
        u32 ri = rs[XD3DRS_PSRGBINPUTS0 + i], ro = rs[XD3DRS_PSRGBOUTPUTS0 + i];
        u32 ai = rs[XD3DRS_PSALPHAINPUTS0 + i], ao = rs[XD3DRS_PSALPHAOUTPUTS0 + i];
        *key = mix(*key, ri); *key = mix(*key, ro); *key = mix(*key, ai); *key = mix(*key, ao);
        sbf(s, "  { vec4 c0 = pc0[%d], c1 = pc1[%d];\n", i, i);
        rc_stage(s, i, ri, ro, 0, mux_msb);
        rc_stage(s, i, ai, ao, 1, mux_msb);
        sbf(s, "  }\n");
    }
    /* final combiner */
    u32 abcd = rs[XD3DRS_PSFINALCOMBINERINPUTSABCD], efg = rs[XD3DRS_PSFINALCOMBINERINPUTSEFG];
    *key = mix(*key, abcd); *key = mix(*key, efg);
    if (!abcd && !efg) {
        sbf(s, "  vec4 col = r0;\n");
        return;
    }
    u32 fl = efg & 0xFF;
    sbf(s, "  vec4 v1r0 = %s(%s) + %s(%s);\n", (fl & 0x40) ? "1.0 - " : "", "vD1", (fl & 0x20) ? "1.0 - " : "", "r0");
    if (fl & 0x80) sbf(s, "  v1r0 = clamp(v1r0, 0.0, 1.0);\n");
    struct sb e = { 0 }, f = { 0 };
    rc_input(&e, (efg >> 24) & 0xFF, 0, 1);
    rc_input(&f, (efg >> 16) & 0xFF, 0, 1);
    sbf(s, "  vec4 efprod = %s * %s;\n", e.p, f.p);
    struct sb A = { 0 }, B = { 0 }, C = { 0 }, D = { 0 }, Gg = { 0 };
    rc_input(&A, (abcd >> 24) & 0xFF, 0, 1);
    rc_input(&B, (abcd >> 16) & 0xFF, 0, 1);
    rc_input(&C, (abcd >> 8) & 0xFF, 0, 1);
    rc_input(&D, abcd & 0xFF, 0, 1);
    rc_input(&Gg, (efg >> 8) & 0xFF, 1, 1);
    sbf(s, "  vec4 col = vec4(clamp((%s * %s + (1.0 - %s) * %s + %s).rgb, 0.0, 1.0), clamp((%s).a, 0.0, 1.0));\n",
        A.p, B.p, A.p, C.p, D.p, Gg.p);
    free(e.p); free(f.p); free(A.p); free(B.p); free(C.p); free(D.p); free(Gg.p);
}

char *psh_generate(const u32 *rs, const u32 (*tss)[XTSS_COUNT], const struct ps_obj *ps,
                   const int kind[4], u64 *key_out)
{
    struct sb s = { 0 };
    u64 key = ps ? 0x5053 : 0x4646;
    sbf(&s, "#version 330 core\n"
            "in vec4 vD0; in vec4 vD1; in vec4 vFog; in vec4 vT0; in vec4 vT1; in vec4 vT2; in vec4 vT3; in float vW;\n"
            "uniform float zScale; uniform float wBias;\n"
            "out vec4 fragColor;\n"
            "uniform sampler2D tex0, tex1, tex2, tex3;\n"
            "uniform samplerCube tex0c, tex1c, tex2c, tex3c;\n"
            "uniform sampler3D tex03, tex13, tex23, tex33;\n"
            "uniform vec2 texScale[4];\n"
            "uniform vec4 tfactor, fogColor, fogParams, pc0[8], pc1[8], fc0, fc1;\n"
            "uniform float alphaRef; uniform int alphaFunc;\n"
            "void main() {\n");
    if (ps) gen_combiners(&s, rs, kind, &key);
    else gen_fixed(&s, tss, kind, &key);
    /* specular add (fixed function only), fog, alpha test */
    if (!ps && rs[XD3DRS_SPECULARENABLE]) sbf(&s, "  col.rgb = clamp(col.rgb + vD1.rgb, 0.0, 1.0);\n");
    int fog = rs[XD3DRS_FOGENABLE] != 0;
    key = mix(key, fog | rs[XD3DRS_SPECULARENABLE] << 1);
    if (fog) {
        sbf(&s, "  float fd = abs(vFog.x), ff;\n"
                "  if (fogParams.w == 3.0) ff = clamp((fogParams.y - fd) / max(fogParams.y - fogParams.x, 1e-6), 0.0, 1.0);\n"
                "  else if (fogParams.w == 1.0) ff = exp(-fogParams.z * fd);\n"
                "  else if (fogParams.w == 2.0) ff = exp(-(fogParams.z * fd) * (fogParams.z * fd));\n"
                "  else ff = clamp(vFog.x, 0.0, 1.0);\n"
                "  col.rgb = mix(fogColor.rgb, col.rgb, ff);\n");
    }
    sbf(&s, "  if (alphaFunc != 7) { float a = floor(col.a * 255.0 + 0.5); bool pass = true;\n"
            "    if (alphaFunc == 0) pass = false; else if (alphaFunc == 1) pass = a < alphaRef;\n"
            "    else if (alphaFunc == 2) pass = a == alphaRef; else if (alphaFunc == 3) pass = a <= alphaRef;\n"
            "    else if (alphaFunc == 4) pass = a > alphaRef; else if (alphaFunc == 5) pass = a != alphaRef;\n"
            "    else if (alphaFunc == 6) pass = a >= alphaRef;\n"
            "    if (!pass) discard; }\n"
            "  fragColor = col;\n");
    /* D3DZB_USEW: the NV2A stores the interpolated w (linear eye depth) instead of z/w */
    if (rs[XD3DRS_ZENABLE] == 2)
        sbf(&s, "  { float zv = vW; zv = (zv > 0.0 && !isnan(zv)) ? zv + wBias : 3.4e38;\n"
                "    gl_FragDepth = clamp(zv / zScale, 0.0, 1.0); }\n");
    /* XBOX_DIAG_FRAG=diffuse|tex0|texcoord: diagnostic override of the fragment colour */
    const char *dg = getenv("XBOX_DIAG_FRAG");
    if (dg && !strcmp(dg, "diffuse")) sbf(&s, "  fragColor = vec4(vD0.rgb, 1.0);\n");
    else if (dg && !strcmp(dg, "tex0")) { sample_stage(&s, 9, kind[0], 0); }
    else if (dg && !strcmp(dg, "texcoord")) sbf(&s, "  fragColor = vec4(fract(vT0.xy), 0.5, 1.0);\n");
    else if (dg && dg[0] == 't' && dg[1] >= '0' && dg[1] <= '3' && !dg[2]) {     /* t0..t3: that stage's sample */
        int st = dg[1] - '0';
        char nm[4] = { 't', (char)dg[1], 0, 0 };
        if (strstr(s.p, nm) && kind[st]) sbf(&s, "  fragColor = vec4(t%d.rgb, 1.0);\n", st);
        else sbf(&s, "  fragColor = vec4(1.0, 0.0, 1.0, 1.0);\n");
    }
    sbf(&s, "}\n");
    *key_out = key;
    return s.p;
}

/* Key over exactly the state psh_generate reads, without building any text. */
u64 psh_key(const u32 *rs, const u32 (*tss)[XTSS_COUNT], const struct ps_obj *ps, const int kind[4])
{
    u64 k = ps ? 0x5053 : 0x4646;
    for (int i = 0; i < 4; i++) k = mix(k, (u64)kind[i]);
    if (ps) {
        for (int i = 0; i <= XD3DRS_PSINPUTTEXTURE; i++)
            if (i < XD3DRS_PSCONSTANT0_0 || i > XD3DRS_PSCONSTANT1_7) k = mix(k, rs[i]);   /* constants are uniforms */
        k = mix(k, rs[XD3DRS_PSTEXTUREMODES]);
    } else {
        for (int i = 0; i < 4; i++) {
            const u32 *t = tss[i];
            k = mix(k, t[XTSS_COLOROP] | t[XTSS_ALPHAOP] << 8 | (u64)t[XTSS_COLORARG0] << 16 | (u64)t[XTSS_COLORARG1] << 24
                       | (u64)t[XTSS_COLORARG2] << 32 | (u64)t[XTSS_ALPHAARG0] << 40 | (u64)t[XTSS_ALPHAARG1] << 48 | (u64)t[XTSS_ALPHAARG2] << 56);
            k = mix(k, t[XTSS_RESULTARG] | (t[XTSS_TEXTURETRANSFORMFLAGS] & 0x100) << 8);
            if (t[XTSS_COLOROP] <= 1) break;
        }
    }
    k = mix(k, (rs[XD3DRS_FOGENABLE] != 0) | (rs[XD3DRS_SPECULARENABLE] != 0) << 1 | (u64)(psh_vs_program != 0) << 2
               | (u64)(rs[XD3DRS_ZENABLE] == 2) << 3);
    return k;
}
