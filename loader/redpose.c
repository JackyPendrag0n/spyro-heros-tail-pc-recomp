/* Red's pose layer: new animations on top of (or instead of) the ones in bossred.edb.
 *
 * The animator samples its clips into a scratch pose ([0x47a690]: per bone 0x20 bytes,
 * translation vec4 then rotation quaternion, parent-relative) inside 0x1fce30, and hands it to
 * 0x2119f0 (call at 0x1fcf65) which copies it into the model's bones. We hook that call: for Red's
 * animator the pose can be changed first - mirrored, layered with procedural motion (wing flaps,
 * tail, head), or replaced by synthesized clips.
 *
 * Skeleton (bossred.edb anim skin, 54 bones; model space: +y up, -z forward, x sideways):
 *   0 root, 1 staff, 2 pelvis, 3-5 spine, 6-10 neck..head, 11/12 13/14 face pairs,
 *   15-20 wing (+x), 21-26 wing (-x), 27-30 arm (+x), 31-32 chest, 33-36 arm (-x),
 *   37-40 leg (+x), 41-49 tail, 50-53 leg (-x). */
#include "xbox.h"
#include <math.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#define NB 54
static const signed char g_parent[NB] = {
    -1, 0, 0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 10, 10, 10, 5, 15, 16, 17, 17, 17, 5, 21, 22, 23, 23, 23,
    5, 27, 28, 29, 5, 31, 5, 33, 34, 35, 2, 37, 38, 39, 2, 41, 42, 43, 44, 45, 46, 47, 48, 2, 50, 51, 52 };
static const signed char g_mirror[NB] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 11, 14, 13, 21, 22, 23, 24, 25, 26, 15, 16, 17, 18, 19, 20,
    33, 34, 35, 36, 31, 32, 27, 28, 29, 30, 50, 51, 52, 53, 41, 42, 43, 44, 45, 46, 47, 48, 49, 37, 38, 39, 40 };

typedef struct { float x, y, z, w; } quat;
static quat qmul(quat a, quat b)
{
    quat r = { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
               a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
               a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
               a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
    return r;
}
static quat qconj(quat a) { quat r = { -a.x, -a.y, -a.z, a.w }; return r; }
static quat qaxis(float ax, float ay, float az, float ang)
{
    float s = sinf(ang * 0.5f), l = sqrtf(ax * ax + ay * ay + az * az);
    if (l < 1e-6f) { quat i = { 0, 0, 0, 1 }; return i; }
    quat r = { ax / l * s, ay / l * s, az / l * s, cosf(ang * 0.5f) };
    return r;
}
static quat qnorm(quat a)
{
    float l = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z + a.w * a.w);
    if (l < 1e-6f) { quat i = { 0, 0, 0, 1 }; return i; }
    a.x /= l; a.y /= l; a.z /= l; a.w /= l; return a;
}
static quat qslerp(quat a, quat b, float t)
{
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) { b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w; d = -d; }
    quat r = { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t };
    (void)d;
    return qnorm(r);
}

typedef struct { float t[4]; quat q; } bone;      /* layout of the scratch pose */

RS u32 g_pose_animator;                              /* set on entry to 0x1fce30 */
RS u32 g_red_animator;                               /* Red's animator (item +0x13c - 4) */
RS int g_pose_mirror;                                /* mirror the sampled clip left <-> right */
RS int g_pose_synth;                                 /* 0 none, else a synthesized clip id */
RS float g_pose_synth_t;                             /* its time (s) */
RS float g_pose_flap;                                /* wing flap amount 0..1 (flying) */
RS float g_pose_look;                                /* head turn (radians, + = toward +x) */
RS float g_pose_time;                                /* clock for procedural motion */
int g_pose_bonetest = -1;                         /* research: bend this bone */
static int g_pose_logged;
int g_pose_debug;

static quat g_world[NB];
static void fk(bone *b)
{
    for (int i = 0; i < NB; i++) g_world[i] = g_parent[i] < 0 ? b[i].q : qmul(g_world[(int)g_parent[i]], b[i].q);
}
/* rotate bone i by a model-space rotation r (children follow) */
static void rot_world(bone *b, int i, quat r)
{
    quat pw = g_parent[i] < 0 ? (quat){ 0, 0, 0, 1 } : g_world[(int)g_parent[i]];
    quat nw = qmul(r, g_world[i]);
    b[i].q = qnorm(qmul(qconj(pw), nw));
}
static void rot_world_axis(bone *b, int i, float ax, float ay, float az, float ang)
{
    fk(b);
    rot_world(b, i, qaxis(ax, ay, az, ang));
}

static void mirror(bone *b)
{
    bone tmp[NB];
    memcpy(tmp, b, sizeof tmp);
    for (int i = 0; i < NB; i++) {
        bone s = tmp[(int)g_mirror[i]];
        s.t[0] = -s.t[0];
        s.q.y = -s.q.y; s.q.z = -s.q.z;           /* reflection across the YZ plane */
        b[i] = s;
    }
}

/* ---- procedural layers ---- */
static void wings(bone *b, float amount, float t)
{
    if (amount <= 0) return;
    float ph = sinf(t * 7.0f);                    /* ~1.1 flaps per second */
    float a = amount * (0.55f * ph + 0.15f);
    rot_world_axis(b, 15, 0, 0, 1, -a);           /* wing roots about the body's long axis: */
    rot_world_axis(b, 21, 0, 0, 1,  a);           /* +x wing spreads with -angle, -x with + */
    float tip = amount * 0.35f * sinf(t * 7.0f - 0.9f);
    rot_world_axis(b, 17, 0, 0, 1, -tip);
    rot_world_axis(b, 23, 0, 0, 1,  tip);
}
static void tail(bone *b, float amount, float t)
{
    for (int i = 41; i <= 49; i++)
        rot_world_axis(b, i, 0, 1, 0, amount * 0.09f * sinf(t * 3.0f - (i - 41) * 0.55f));
}
static void look(bone *b, float yaw)
{
    if (fabsf(yaw) < 1e-3f) return;
    for (int i = 6; i <= 10; i++) rot_world_axis(b, i, 0, 1, 0, yaw * 0.2f);
}

/* ---- locomotion, made here: a stepping cycle laid over whatever clip plays ----
 * phase advances with the distance travelled (redchar), so the feet keep pace with the ground.
 * walk 0..1 = amount (0 standing, 1 full run). Model space: +x his side, -z forward. */
RS float g_pose_gait_phase, g_pose_gait;           /* phase (radians), amount 0..1 */
RS float g_pose_lean, g_pose_bank, g_pose_crouch;   /* forward lean, roll into turns, landing squat */
RS float g_pose_breathe = 1.0f;                     /* idle breathing / tail life */
static void gait(bone *b, float amt, float ph)
{
    if (amt <= 0.001f) return;
    float run = amt > 0.5f ? (amt - 0.5f) * 2 : 0;          /* 0 walk .. 1 run */
    float A = amt * (0.40f + 0.30f * run);                  /* hip swing */
    float s = sinf(ph), c = cosf(ph);
    /* legs: +x leg 37-40, -x leg 50-53; swing about x (forward = +angle) */
    rot_world_axis(b, 37, 1, 0, 0,  A * s);
    rot_world_axis(b, 50, 1, 0, 0, -A * s);
    float liftL = c > 0 ? c : 0, liftR = c < 0 ? -c : 0;    /* the swinging leg folds */
    rot_world_axis(b, 38, 1, 0, 0, -amt * (0.15f + 0.65f * liftL));
    rot_world_axis(b, 51, 1, 0, 0, -amt * (0.15f + 0.65f * liftR));
    rot_world_axis(b, 39, 1, 0, 0,  amt * 0.5f * liftL);
    rot_world_axis(b, 52, 1, 0, 0,  amt * 0.5f * liftR);
    /* body: bob twice per cycle, a little sway and counter-twist */
    b[2].t[1] -= amt * (0.05f + 0.10f * run) * fabsf(c);
    rot_world_axis(b, 2, 0, 1, 0, amt * 0.08f * s);
    rot_world_axis(b, 4, 0, 1, 0, -amt * 0.12f * s);
    rot_world_axis(b, 3, 0, 0, 1, amt * 0.05f * c);
    /* arms swing against the legs, head steadies */
    rot_world_axis(b, 27, 1, 0, 0, -amt * 0.35f * s);
    rot_world_axis(b, 33, 1, 0, 0,  amt * 0.35f * s);
    rot_world_axis(b, 8, 0, 1, 0, amt * 0.08f * s);
    /* wings fold in as he runs, with a beat to the stride */
    float fold = run * 0.35f + amt * 0.05f * s;
    rot_world_axis(b, 15, 0, 0, 1,  fold);
    rot_world_axis(b, 21, 0, 0, 1, -fold);
    /* tail swings opposite the hips, wave down its length */
    for (int i = 41; i <= 49; i++)
        rot_world_axis(b, i, 0, 1, 0, -amt * 0.10f * sinf(ph - (i - 41) * 0.45f));
}
/* ---- the stride, take two (anim1): each foot plants and pushes off instead of swinging on a sine ----
 * A leg's cycle is stance (foot on the ground, hip sweeping back at an even rate so the foot doesn't
 * skate) then swing (knee folds high, foot flicks through and reaches). Walking spends more time on
 * the ground, running less (a moment with both feet up). The pelvis rides each step - highest over
 * the planted foot when walking, sinking into it when running - and shifts its weight over the
 * planted leg; arms counter-swing a beat late (the staff arm less, it carries the staff); the head
 * stays level; wings and tail answer the body's bounce and twist on springs, a little after it.
 * Same phase as before (debugmenu.c drives it by distance), so nothing else changes.
 * XBOX_OLD_GAIT=1 = the old sine gait. */
static RS float g_dt_pose;
static RS int g_staff_hand;
static float smooth01(float x) { return x <= 0 ? 0 : x >= 1 ? 1 : x * x * (3 - 2 * x); }
static float frac01(float x) { x -= floorf(x); return x; }
static void leg_cycle(bone *b, int hip, float p, float amt, float run, float A)
{
    float duty = 0.62f - 0.26f * run;                     /* share of the cycle the foot is down */
    float h, knee, ank, toe;
    if (p < duty) {                                       /* stance: plant -> push off */
        float u = p / duty;
        h = A * (1 - 2 * u);
        knee = (0.10f + 0.28f * run) * sinf(3.14159f * u);   /* gives under the weight */
        float push = smooth01((u - 0.62f) / 0.38f);          /* heel peels up, toes push */
        ank = -0.50f * push + 0.10f * knee;               /* (+ x on the ankle = toes up) */
        toe = 0.45f * push;                               /* toes stay flat as the heel rises */
    } else {                                              /* swing: fold, carry through, reach */
        float u = (p - duty) / (1 - duty);
        h = -A + 2 * A * smooth01(u * 1.08f);
        float f = sinf(3.14159f * powf(u, 0.8f));
        knee = (0.75f + 0.55f * run) * f;
        ank = -0.45f * (1 - smooth01(u / 0.35f)) + 0.30f * f;  /* push-off flick, then toes up to clear the ground */
        toe = -0.15f * f;
        if (u > 0.8f) knee *= 1 - 0.6f * (u - 0.8f) / 0.2f;   /* straightening to reach */
    }
    rot_world_axis(b, hip, 1, 0, 0, amt * h);
    rot_world_axis(b, hip + 1, 1, 0, 0, -amt * knee);
    rot_world_axis(b, hip + 2, 1, 0, 0, amt * ank);
    rot_world_axis(b, hip + 3, 1, 0, 0, amt * toe);
}
static void gait2(bone *b, float amt, float ph)
{
    static RS struct { float x, v; } sp_bob, sp_tw, sp_roll;     /* springs: wings/tail behind the body */
    float dt = g_dt_pose;
    float run = smooth01((amt - 0.45f) / 0.4f);
    float a = amt > 1 ? 1 : amt;
    float bob_y = 0, roll = 0, yaw = 0;
    if (a > 0.001f) {
        float A = 0.34f + 0.20f * run;                    /* hip sweep each way */
        /* +x leg plants at ph = pi/2 (where the old sine gait had it furthest forward); -x half a cycle on */
        float pR = frac01((ph - 1.5708f) / 6.28318f), pL = frac01(pR + 0.5f);
        leg_cycle(b, 37, pR, a, run, A);
        leg_cycle(b, 50, pL, a, run, A);
        /* the step: two per cycle, q = 0 at each plant */
        float q = frac01(pR * 2);
        float walk_y = -0.07f * cosf(6.28318f * q);       /* walk: low at the plant, up over the foot */
        float run_y = -0.09f * cosf(6.28318f * (q - 0.30f)) - 0.04f;   /* run: sinks into the stance, up in the air */
        bob_y = a * (walk_y + (run_y - walk_y) * run);
        b[2].t[1] += bob_y;
        float side = cosf(6.28318f * pR + 1.2f);          /* + = weight over the +x foot (mid-stance) */
        b[2].t[0] += a * (0.07f - 0.04f * run) * side;   /* hips over the planted foot */
        roll = a * 0.06f * side;
        rot_world_axis(b, 2, 0, 0, 1, -roll);             /* the swinging side's hip drops a little */
        yaw = a * (0.07f + 0.02f * run) * sinf(ph);       /* hips turn with the leg going forward */
        rot_world_axis(b, 2, 0, 1, 0, yaw);
        rot_world_axis(b, 2, 1, 0, 0, -0.10f * run * a);  /* a runner's tilt into it (the head stays up, lean_bank) */
        rot_world_axis(b, 3, 0, 1, 0, -yaw * 0.6f);       /* chest turns back the other way */
        rot_world_axis(b, 4, 0, 1, 0, -yaw * 0.9f);
        rot_world_axis(b, 3, 0, 0, 1, roll * 0.8f);       /* spine keeps him upright over it */
        /* arms: counter to the legs, a beat late; elbows bend as he runs; the staff arm swings less */
        int sa = g_staff_hand == 30 ? 27 : 33;
        float as = sinf(ph - 0.35f);
        float ka = a * (0.32f + 0.25f * run);
        rot_world_axis(b, 27, 1, 0, 0, -ka * as * (sa == 27 ? 0.55f : 1.0f));
        rot_world_axis(b, 33, 1, 0, 0,  ka * as * (sa == 33 ? 0.55f : 1.0f));
        rot_world_axis(b, 28, 1, 0, 0, a * (0.15f + 0.45f * run) * (0.8f - 0.2f * as));
        rot_world_axis(b, 34, 1, 0, 0, a * (0.15f + 0.45f * run) * (0.8f + 0.2f * as));
        /* head: steady - takes out the hips' turn and roll and the bounce's nod */
        rot_world_axis(b, 6, 0, 1, 0, -yaw * 0.4f);
        rot_world_axis(b, 8, 0, 0, 1, -roll * 0.6f);
        rot_world_axis(b, 8, 1, 0, 0, -bob_y * 0.8f);
        /* wings tucked in tighter the faster he goes */
        float fold = 0.12f * a + 0.30f * run;
        rot_world_axis(b, 15, 0, 0, 1,  fold);
        rot_world_axis(b, 21, 0, 0, 1, -fold);
    }
    /* follow-through: wings bounce after the body, tail swings after the hips (it settles when he stops) */
    float k = dt > 0.05f ? 0.05f : dt;
    sp_bob.v += (240.0f * (bob_y - sp_bob.x) - 22.0f * sp_bob.v) * k; sp_bob.x += sp_bob.v * k;
    sp_tw.v += (90.0f * (yaw - sp_tw.x) - 14.0f * sp_tw.v) * k; sp_tw.x += sp_tw.v * k;
    sp_roll.v += (120.0f * (roll - sp_roll.x) - 9.0f * sp_roll.v) * k; sp_roll.x += sp_roll.v * k;
    float wb = (bob_y - sp_bob.x) * 1.4f;                 /* wings lag the bounce */
    if (getenv("XBOX_GAIT_LOG") && a > 0.01f) { extern u32 debugmenu_play_frame(void);
        klog("gait f%u ph %.2f a %.2f run %.2f dt %.3f bob %.3f yaw %.3f roll %.3f lean %.3f\n", debugmenu_play_frame(), ph, a, run, dt, bob_y, yaw, roll, g_pose_lean); }
    if (fabsf(wb) > 1e-4f) {
        rot_world_axis(b, 15, 0, 0, 1, -wb); rot_world_axis(b, 21, 0, 0, 1, wb);
        rot_world_axis(b, 17, 0, 0, 1, -wb * 0.5f); rot_world_axis(b, 23, 0, 0, 1, wb * 0.5f);
    }
    if (fabsf(sp_tw.x) + fabsf(sp_roll.x) + fabsf(wb) > 1e-4f || a > 0.001f)
        for (int i = 41; i <= 49; i++) {
            float d = (i - 41) / 8.0f;                    /* further down the tail, further behind */
            rot_world_axis(b, i, 0, 1, 0, -sp_tw.x * (0.07f + 0.06f * d) - a * 0.025f * sinf(ph - 1.2f - d * 2.2f));
            /* held up off the ground as a counterweight while he moves, the bounce rippling down it */
            rot_world_axis(b, i, 1, 0, 0, (bob_y - sp_bob.x) * (0.10f + 0.15f * d) - (i < 44 ? a * (0.06f + 0.04f * run) : 0));   /* (- x = up) */
        }
}
static void lean_bank(bone *b)
{
    if (fabsf(g_pose_lean) > 1e-3f) {                                                /* forward */
        rot_world_axis(b, 2, 1, 0, 0, -g_pose_lean);
        static RS int old = -1; if (old < 0) old = getenv("XBOX_OLD_GAIT") != NULL;
        if (!old) { rot_world_axis(b, 6, 1, 0, 0, 0.45f * g_pose_lean); rot_world_axis(b, 8, 1, 0, 0, 0.2f * g_pose_lean); }   /* eyes stay on where he's going */
    }
    if (fabsf(g_pose_bank) > 1e-3f) rot_world_axis(b, 2, 0, 0, 1, g_pose_bank);
    if (g_pose_crouch > 1e-3f) {                                                     /* knees give */
        float k = g_pose_crouch;
        b[2].t[1] -= 0.35f * k;
        rot_world_axis(b, 37, 1, 0, 0, 0.5f * k);  rot_world_axis(b, 50, 1, 0, 0, 0.5f * k);
        rot_world_axis(b, 38, 1, 0, 0, -0.9f * k); rot_world_axis(b, 51, 1, 0, 0, -0.9f * k);
        rot_world_axis(b, 4, 1, 0, 0, -0.25f * k);
    }
}
static void breathe(bone *b, float t, float amt)
{
    if (amt <= 0) return;
    float br = sinf(t * 1.6f);
    rot_world_axis(b, 4, 1, 0, 0, amt * 0.03f * br);
    rot_world_axis(b, 15, 0, 0, 1, -amt * 0.04f * br);
    rot_world_axis(b, 21, 0, 0, 1,  amt * 0.04f * br);
    for (int i = 41; i <= 49; i++) rot_world_axis(b, i, 0, 1, 0, amt * 0.05f * sinf(t * 1.1f - (i - 41) * 0.5f));
}

/* ---- synthesized clips ---- */
enum { SYN_NONE, SYN_TAUNT, SYN_HOVER, SYN_STAFFSPIN, SYN_COUNT };
static void synth(bone *b, int id, float t)
{
    switch (id) {
    case SYN_TAUNT: {                             /* wings wide, chest out, beckoning arm, head tilt */
        float e = t < 0.4f ? t / 0.4f : t > 2.6f ? (3.0f - t) / 0.4f : 1;   /* ease in/out */
        if (e < 0) e = 0;
        rot_world_axis(b, 15, 0, 0, 1, -0.7f * e);                     /* wings spread wide */
        rot_world_axis(b, 21, 0, 0, 1,  0.7f * e);
        rot_world_axis(b, 4, 1, 0, 0, 0.25f * e);                      /* lean back */
        rot_world_axis(b, 8, 0, 0, 1, 0.3f * e * sinf(t * 2.0f));      /* head tilt */
        rot_world_axis(b, 27, 0, 0, 1, 1.1f * e);                     /* arm up */
        rot_world_axis(b, 29, 1, 0, 0, e * (0.6f + 0.5f * sinf(t * 9.0f)));   /* beckon */
        tail(b, 2.0f * e, t);
        break;
    }
    case SYN_HOVER:
        wings(b, 1.0f, t);
        rot_world_axis(b, 37, 1, 0, 0, 0.5f);                        /* legs tucked */
        rot_world_axis(b, 50, 1, 0, 0, 0.5f);
        rot_world_axis(b, 38, 1, 0, 0, -0.7f);
        rot_world_axis(b, 51, 1, 0, 0, -0.7f);
        tail(b, 1.5f, t);
        break;
    case SYN_STAFFSPIN:                           /* the staff twirls in his hand (after staff_follow) */
        break;
    }
}

static void modify(bone *b);
static void bake_capture(u32 an, const bone *b);
static RS float g_dt_pose = 1.0f / 60;
/* after the animator update (0x1fd2c0, past its call to 0x1fce30): the model's bones
 * ([animator+0x104]: +0x28 array of 0x48-byte records, +0x2c count; record +0x18 translation,
 * +0x28 rotation) hold this frame's pose. Red's clips are decoded straight into them, so his
 * pose is changed here. */
void red_pose_post(u32 a)
{
    if (!g_red_animator || a != g_red_animator) return;
    u32 out = *(u32 *)(uintptr_t)(a + 0x104);
    if (!out) return;
    u8 *arr = *(u8 **)(uintptr_t)(out + 0x28);
    u32 n = *(u32 *)(uintptr_t)(out + 0x2c);
    if (!arr || n != NB) { static int w; if (!w++) klog("redpose: Red's model has %u bones, expected %d\n", n, NB); return; }
    /* the decoder only rewrites bones its clip animates: a bone still holding what we wrote
     * last frame gets its clean (unmodified) value back first, so edits never pile up */
    static RS bone clean[NB], written[NB];
    static RS u32 owner;
    bone b[NB];
    for (int i = 0; i < NB; i++) {
        u8 *r = arr + i * 0x48 + 0x18;
        if (owner == out && !memcmp(r, &written[i], sizeof(bone))) memcpy(r, &clean[i], sizeof(bone));
        memcpy(&clean[i], r, sizeof(bone));
        memcpy(&b[i], r, sizeof(bone));
    }
    owner = out;
    if (getenv("XBOX_POSE_DUMP")) {
        static int d; if (d++ == 30) {
            klog("out %08x hdr:", out); for (int k = 0; k < 0x48; k += 4) klog(" %08x", *(u32 *)(uintptr_t)(out + k)); klog("\n");
            for (int i = 0; i < 6; i++) {
                u8 *r = arr + i * 0x48;
                klog("rec %d:", i); for (int k = 0; k < 0x48; k += 4) klog(" %08x", *(u32 *)(r + k)); klog("\n");
                float *m = *(float **)(r + 0xc);
                if (m) { klog("mtx %d:", i); for (int k = 0; k < 16; k++) klog(" %.3f", m[k]); klog("\n"); }
            }
            klog("item world:"); 
        }
    }
    if (getenv("XBOX_POSE_LOG") && g_pose_logged < 2) {
        g_pose_logged++;
        for (int i = 0; i < NB; i++)
            klog("pose %2d t %.3f %.3f %.3f %.3f q %.3f %.3f %.3f %.3f\n", i, b[i].t[0], b[i].t[1], b[i].t[2], b[i].t[3],
                 b[i].q.x, b[i].q.y, b[i].q.z, b[i].q.w);
    }
    {   static RS double lt; struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        double t = ts.tv_sec + ts.tv_nsec * 1e-9; g_dt_pose = lt ? (float)(t - lt) : 1.0f / 60; lt = t;
        if (g_dt_pose > 0.1f) g_dt_pose = 0.1f;
        if (g_dt_pose < 0.001f) g_dt_pose = 0.001f; }
    { extern float g_dbg_root[6]; memcpy(g_dbg_root, b[0].t, 12); memcpy(g_dbg_root + 3, b[2].t, 12); }
    bake_capture(a, b);
    {   /* the staff in the world, for the effects: the bone matrix the engine built last frame */
        float *m = *(float **)(arr + 1 * 0x48 + 0xc);
        extern float g_staff_w[3], g_staff_ax[3], g_staff_m[9]; extern int g_staff_ok;
        if (m) { for (int k = 0; k < 3; k++) { g_staff_w[k] = m[12 + k]; g_staff_ax[k] = m[4 + k]; g_staff_m[k] = m[k]; g_staff_m[3 + k] = m[4 + k]; g_staff_m[6 + k] = m[8 + k]; } g_staff_ok = 1; }
    }
    modify(b);
    for (int i = 0; i < NB; i++) { memcpy(arr + i * 0x48 + 0x18, &b[i], sizeof(bone)); written[i] = b[i]; }
    *(u8 *)(uintptr_t)(out + 0x10) = 0;           /* matrices out of date (as 0x2119f0 does) */
    *(u8 *)(uintptr_t)(out + 0xe) = 0;
}
RS u32 g_pose_out;
RS float g_staff_head[3], g_staff_grip[3]; RS int g_staff_head_ok; RS float g_staff_cal[4][3];
RS float g_staff_w[3], g_staff_ax[3], g_staff_m[9]; RS int g_staff_ok;
float g_dbg_root[6];
void red_pose_c(void)
{
    if (getenv("XBOX_POSE_DEBUG") && g_red_animator) {
        static u32 seen[40]; static int ns; int k;
        for (k = 0; k < ns && seen[k] != g_pose_animator; k++) ;
        if (k == ns && ns < 40) {
            seen[ns++] = g_pose_animator;
            u32 a = g_pose_animator;
            klog("pose out: animator %08x +104 %08x +20 %08x layers %u (red %08x +104 %08x +20 %08x) out %08x n %u\n", a,
                 *(u32 *)(uintptr_t)(a + 0x104), *(u32 *)(uintptr_t)(a + 0x20), *(u8 *)(uintptr_t)(a + 0x118), g_red_animator,
                 *(u32 *)(uintptr_t)(g_red_animator + 0x104), *(u32 *)(uintptr_t)(g_red_animator + 0x20), g_pose_out, g_pose_out ? *(u32 *)(uintptr_t)(g_pose_out + 0x2c) : 0);
        }
    }
}
static float ease(float x) { return x <= 0 ? 0 : x >= 1 ? 1 : x * x * (3 - 2 * x); }
/* ---- full forward kinematics (rotation and position, model space) ---- */
static void qrot(quat q, const float *v, float *o)
{
    quat p = { v[0], v[1], v[2], 0 }, r = qmul(qmul(q, p), qconj(q));
    o[0] = r.x; o[1] = r.y; o[2] = r.z;
}
static RS quat g_wq[NB]; static RS float g_wp[NB][3];
static void fk_full(bone *b)
{
    for (int i = 0; i < NB; i++) {
        int p = g_parent[i];
        if (p < 0) { g_wq[i] = b[i].q; memcpy(g_wp[i], b[i].t, 12); continue; }
        g_wq[i] = qmul(g_wq[p], b[i].q);
        float r[3]; qrot(g_wq[p], b[i].t, r);
        for (int k = 0; k < 3; k++) g_wp[i][k] = g_wp[p][k] + r[k];
    }
}
static void lerp_bone(bone *d, const bone *a, const bone *c, float t)
{
    for (int k = 0; k < 3; k++) d->t[k] = a->t[k] + (c->t[k] - a->t[k]) * t;
    d->q = qslerp(a->q, c->q, t);
}

/* ---- the staff stays in his hand ----
 * The staff (bone 1) hangs off the root, not the hand: his clips move both together. Whatever
 * the layers do to his arms, the staff keeps the place in the hand it had in the clip. */
static RS int g_staff_hand;
static RS quat g_staff_rq; static RS float g_staff_rp[3];
static void staff_grab(bone *b)
{
    fk_full(b);
    float best = 1e9f; int hand = 36;
    static const int hands[2] = { 30, 36 };
    for (int h = 0; h < 2; h++) {
        float d = 0; for (int k = 0; k < 3; k++) { float e = g_wp[1][k] - g_wp[hands[h]][k]; d += e * e; }
        if (d < best) { best = d; hand = hands[h]; }
    }
    g_staff_hand = hand;
    quat iq = qconj(g_wq[hand]);
    g_staff_rq = qmul(iq, g_wq[1]);
    float dp[3] = { g_wp[1][0] - g_wp[hand][0], g_wp[1][1] - g_wp[hand][1], g_wp[1][2] - g_wp[hand][2] };
    qrot(iq, dp, g_staff_rp);
}
static void staff_follow(bone *b)
{
    fk_full(b);
    int h = g_staff_hand;
    quat sq = qmul(g_wq[h], g_staff_rq);
    float off[3], sp[3]; qrot(g_wq[h], g_staff_rp, off);
    for (int k = 0; k < 3; k++) sp[k] = g_wp[h][k] + off[k];
    quat iq0 = qconj(g_wq[0]);                 /* staff's parent is the root */
    b[1].q = qnorm(qmul(iq0, sq));
    float d[3] = { sp[0] - g_wp[0][0], sp[1] - g_wp[0][1], sp[2] - g_wp[0][2] };
    qrot(iq0, d, b[1].t);
}

/* ---- in the air ---- rise: going up after a jump; glide: wings out, body flat; fall: dropping */
RS float g_pose_rise, g_pose_glide, g_pose_fall, g_pose_kick;   /* weights 0..1, kick = a big wing stroke */
RS float g_pose_flip, g_pose_aim, g_pose_dive, g_pose_spin, g_pose_shield;          /* somersault progress 0..1, staff levelled to aim 0..1 */
RS float g_pose_charge;                    /* the dash (charge) pose 0..1 */
RS float g_pose_hop;                       /* the spring of a plain jump: 1 at take-off, fading (anim1) */
/* ---- the dash: low and forward, head up, wings swept back flat, staff thrust ahead like a lance,
 * the other arm back, tail streaming straight; the run cycle carries on underneath ---- */
static void charge_pose(bone *b, float t)
{
    float c = g_pose_charge;
    if (c < 1e-3f) return;
    rot_world_axis(b, 2, 1, 0, 0, -0.50f * c);                       /* body pitched forward */
    b[2].t[1] -= 0.25f * c;                                          /* and low */
    rot_world_axis(b, 6, 1, 0, 0, 0.40f * c);                        /* neck/head up to look ahead */
    rot_world_axis(b, 8, 1, 0, 0, 0.15f * c);
    int sh = g_staff_hand == 30 ? 27 : 33, oth = sh == 27 ? 33 : 27;
    rot_world_axis(b, sh, 1, 0, 0, 1.15f * c);                       /* staff arm forward */
    rot_world_axis(b, sh + 1, 1, 0, 0, 0.35f * c);
    rot_world_axis(b, oth, 1, 0, 0, -0.6f * c);                      /* other arm back */
    rot_world_axis(b, 15, 1, 0, 0, 0.9f * c); rot_world_axis(b, 21, 1, 0, 0, 0.9f * c);   /* wings back */
    rot_world_axis(b, 15, 0, 0, 1, 0.35f * c); rot_world_axis(b, 21, 0, 0, 1, -0.35f * c); /* and folded */
    float flut = 0.06f * c * sinf(t * 18.0f);                        /* wing edges flutter in the wind */
    rot_world_axis(b, 17, 0, 0, 1, -flut); rot_world_axis(b, 23, 0, 0, 1, flut);
    for (int i = 41; i <= 49; i++) rot_world_axis(b, i, 1, 0, 0, 0.05f * c);   /* tail out straight */
}
static void air_pose(bone *b, float t)
{
    float r = g_pose_rise, g = g_pose_glide, f = g_pose_fall;
    if (g_pose_flip > 0 && g_pose_flip < 1) {                         /* forward somersault, tucked */
        float p = g_pose_flip, tuck = sinf(p * 3.14159f);
        rot_world_axis(b, 37, 1, 0, 0, 1.0f * tuck); rot_world_axis(b, 50, 1, 0, 0, 1.0f * tuck);
        rot_world_axis(b, 38, 1, 0, 0, -1.2f * tuck); rot_world_axis(b, 51, 1, 0, 0, -1.2f * tuck);
        rot_world_axis(b, 4, 1, 0, 0, -0.5f * tuck);
        rot_world_axis(b, 2, 1, 0, 0, -6.2832f * ease(p));
    }
    if (g_pose_hop > 1e-3f) {                                         /* the push-off of a hop */
        float h = g_pose_hop, e = h * h;                              /* strongest right at the take-off */
        rot_world_axis(b, 37, 1, 0, 0, -0.35f * e); rot_world_axis(b, 50, 1, 0, 0, -0.25f * e);   /* legs driving down and back */
        rot_world_axis(b, 39, 1, 0, 0, -0.6f * e); rot_world_axis(b, 52, 1, 0, 0, -0.6f * e);     /* toes pointed */
        rot_world_axis(b, 40, 1, 0, 0, -0.2f * e); rot_world_axis(b, 53, 1, 0, 0, -0.2f * e);
        rot_world_axis(b, 4, 1, 0, 0, 0.12f * e); rot_world_axis(b, 6, 1, 0, 0, 0.15f * e);        /* chest and chin up */
        /* one wing beat to lift him: up and open as he leaves the ground, then a downstroke */
        float s = sinf((1 - h) * 3.14159f * 1.6f), wb = 0.55f * s * h;
        rot_world_axis(b, 15, 0, 0, 1, -0.35f * h - wb); rot_world_axis(b, 21, 0, 0, 1, 0.35f * h + wb);
        rot_world_axis(b, 17, 0, 0, 1, -0.3f * wb); rot_world_axis(b, 23, 0, 0, 1, 0.3f * wb);
        for (int i = 41; i <= 49; i++) rot_world_axis(b, i, 1, 0, 0, 0.04f * e);                   /* tail swings down under him (+ x = down) */
    }
    if (r + g + f < 1e-3f && g_pose_kick < 1e-3f) return;
    /* body */
    rot_world_axis(b, 2, 1, 0, 0, 0.15f * r - 0.40f * g);           /* back on take-off, flat gliding */
    rot_world_axis(b, 6, 1, 0, 0, 0.30f * g);                        /* head up while flat */
    rot_world_axis(b, 8, 1, 0, 0, 0.10f * g + 0.10f * f);
    /* legs: trail back going up / gliding, dangle a little falling */
    float hip = -0.55f * r - 0.95f * g + 0.30f * f, knee = -0.45f * r + 0.25f * g - 0.35f * f;
    rot_world_axis(b, 37, 1, 0, 0, hip);  rot_world_axis(b, 50, 1, 0, 0, hip);
    rot_world_axis(b, 38, 1, 0, 0, knee); rot_world_axis(b, 51, 1, 0, 0, knee);
    rot_world_axis(b, 39, 1, 0, 0, 0.4f * r + 0.3f * g); rot_world_axis(b, 52, 1, 0, 0, 0.4f * r + 0.3f * g);
    /* arms up and forward on the jump, out a little gliding */
    rot_world_axis(b, 27, 1, 0, 0, 0.45f * r); rot_world_axis(b, 33, 1, 0, 0, 0.45f * r);
    rot_world_axis(b, 27, 0, 0, 1, 0.25f * g + 0.3f * f); rot_world_axis(b, 33, 0, 0, 1, -0.25f * g - 0.3f * f);
    /* tail streams out behind */
    for (int i = 41; i <= 49; i++)
        rot_world_axis(b, i, 1, 0, 0, (0.06f * g - 0.05f * r) + 0.03f * (g + f) * sinf(t * 2.5f - (i - 41) * 0.5f));
    /* wings: swept back and up on the way up, spread wide and slowly beating to glide,
     * half open and flapping hard when dropping, one big stroke on a double jump */
    float spread = -1.15f * g - 0.55f * f, back = 0.70f * r + 0.15f * f;
    float beat = g * 0.14f * sinf(t * 3.0f) + f * 0.35f * sinf(t * 9.0f);
    float k = g_pose_kick, stroke = k > 0 ? sinf((1 - k) * 6.2832f) * 0.9f * k : 0;
    rot_world_axis(b, 15, 1, 0, 0, back); rot_world_axis(b, 21, 1, 0, 0, back);
    rot_world_axis(b, 15, 0, 0, 1,  spread - beat - stroke);
    rot_world_axis(b, 21, 0, 0, 1, -spread + beat + stroke);
    rot_world_axis(b, 17, 0, 0, 1, -0.30f * g - 0.6f * beat);
    rot_world_axis(b, 23, 0, 0, 1,  0.30f * g + 0.6f * beat);
}

/* ---- melee: a staff swing (forehand, then backhand if pressed again) ----
 * t from 0: wind-up 0-0.12 s, strike to 0.26 s, follow-through, back by 0.5 s. Only the upper
 * body moves, so he can swing while walking. */
RS float g_pose_melee_t = -1; RS int g_pose_melee_side;
static void melee(bone *b, float t)
{
    if (t < 0 || t > 0.55f) return;
    float wind = ease(t / 0.12f) * (1 - ease((t - 0.12f) / 0.14f));
    float strike = ease((t - 0.10f) / 0.16f) * (1 - ease((t - 0.30f) / 0.25f));
    float w = (t < 0.12f ? wind : 0) * -1 + strike;                 /* -1 wound up .. +1 struck */
    int hand_side = g_staff_hand == 30 ? 1 : -1;                      /* +x arm 27-30 or -x arm 33-36 */
    float dir = (g_pose_melee_side ? -1.0f : 1.0f) * hand_side;
    int sh = hand_side > 0 ? 27 : 33, el = sh + 1;
    rot_world_axis(b, 3, 0, 1, 0, -0.45f * w * dir);                  /* torso twists into it */
    rot_world_axis(b, 5, 0, 1, 0, -0.35f * w * dir);
    rot_world_axis(b, sh, 1, 0, 0, 0.9f * (strike + 0.4f * wind));    /* arm up and forward */
    rot_world_axis(b, sh, 0, 1, 0, -1.1f * w * dir);                  /* sweep across */
    rot_world_axis(b, el, 1, 0, 0, -0.5f * wind);
    rot_world_axis(b, 2, 1, 0, 0, -0.12f * strike);                   /* lean into the blow */
    rot_world_axis(b, 15, 0, 0, 1, -0.25f * strike); rot_world_axis(b, 21, 0, 0, 1, 0.25f * strike);
}

/* ---- transitions ----
 * base_w: how much of his idle stands in for whatever clip the engine is still playing (his clips
 * can't be cut short: when you move during one it fades to the idle base underneath, so he never
 * gets stuck in it). xfade: a short blend from the last shown pose whenever the state changes. */
RS float g_pose_base_w; RS int g_pose_capture_idle;
RS float g_pose_nolift;
static RS float g_base_cur;
static RS bone g_idle_ref[NB], g_last_out[NB];
static RS int g_idle_ok, g_last_ok;
static RS float g_xf, g_xf_rate;
void redpose_xfade(float secs) { if (g_last_ok) { g_xf = 1; g_xf_rate = 1.0f / (secs > 0.02f ? secs : 0.02f); } }

/* ---- Red's clips, baked: his animations recorded pose by pose and played back here ----
 * The game can only play his clips its own way (a one-shot can't be looped, cut short or held, and
 * asking for the idle mid-clip freezes it), so each clip is recorded once: debugmenu.c plays it
 * and moves its play position step by step (track +0x80, only on a settled single track), and
 * every pose the animator samples is captured here with the position it was sampled at. After
 * that the engine just plays his idle, and every clip is played from the recording - looped,
 * sped up, blended, held, on the whole body or the upper/lower half. The recordings are kept in
 * a file in the save folder (redclips.bin) so this happens only once. */
#define BC_MAX 40
#define BS_MAX 48
typedef struct { u32 id; int n; float frames; float p[BS_MAX]; bone pose[BS_MAX][NB]; } bclip;
static bclip *g_bc; int g_nbc;
int g_bake_idx = -1;                 /* clip being recorded (debugmenu.c), -1 none */
u32 g_bake_res;                      /* its track's resource: a pose counts only once the track changed */
int g_bake_got;                      /* samples recorded so far for g_bake_idx */
float g_bake_lastp;                  /* the last one's position */
static bclip *bc_get(int i)
{
    if (!g_bc) g_bc = calloc(BC_MAX, sizeof(bclip));
    return i >= 0 && i < BC_MAX ? &g_bc[i] : NULL;
}
void redclip_begin(int i, u32 id, float frames)
{
    bclip *c = bc_get(i); if (!c) return;
    c->id = id; c->n = 0; c->frames = frames; if (i >= g_nbc) g_nbc = i + 1;
    g_bake_idx = i; g_bake_got = 0;
}
/* from red_pose_post, before any changes: the pose the animator sampled for the clip */
static void bake_capture(u32 an, const bone *b)
{
    { extern int g_bake_owner; int red_ctx(void); if (g_bake_idx < 0 || g_bake_owner != red_ctx()) return; }
    u32 head = *(u32 *)(uintptr_t)(an + 0x11c), tail = *(u32 *)(uintptr_t)(an + 0x120);
    if (!tail || head != tail) return;
    u32 tr = tail - 4;
    if (*(u32 *)(uintptr_t)(tr + 0x18) != g_bake_res) return;
    bclip *c = bc_get(g_bake_idx);
    if (!c || c->n >= BS_MAX) return;
    float p = *(float *)(uintptr_t)(tr + 0x80);
    if (!c->n && p > 0.1f) return;                         /* from the start: wait for the first step */
    if (c->n && p <= c->p[c->n - 1] + 0.3f / (BS_MAX)) return;   /* forward, and not crowded */        /* positions only go forward */
    c->p[c->n] = p; memcpy(c->pose[c->n], b, sizeof(bone) * NB); c->n++; g_bake_got = c->n; g_bake_lastp = p;
}
int redclip_find(u32 id)
{
    for (int i = 0; i < g_nbc; i++) if (g_bc[i].id == id && g_bc[i].n >= 2) return i;
    return -1;
}
u32 redclip_id(int i) { return i >= 0 && i < g_nbc ? g_bc[i].id : 0; }
float redclip_secs(int i) { return i >= 0 && i < g_nbc ? g_bc[i].frames / 30.0f : 1.0f; }
/* the clip's pose at position u (0..1), between the two nearest recorded poses */
static void redclip_sample(int ci, float u, bone *out)
{
    bclip *c = &g_bc[ci];
    if (u <= c->p[0]) { memcpy(out, c->pose[0], sizeof(bone) * NB); return; }
    if (u >= c->p[c->n - 1]) { memcpy(out, c->pose[c->n - 1], sizeof(bone) * NB); return; }
    int k = 0; while (k + 2 < c->n && c->p[k + 1] < u) k++;
    float f = (u - c->p[k]) / (c->p[k + 1] - c->p[k]);
    for (int i = 0; i < NB; i++) lerp_bone(&out[i], &c->pose[k][i], &c->pose[k + 1][i], f);
}
static const char *bake_path(void)
{
    static char path[600]; const char *fs_save_dir(void);
    snprintf(path, sizeof path, "%s/redclips.bin", fs_save_dir());
    return path;
}
#define BC_MAGIC 0x52434c31u     /* "RCL1" */
void redclip_save(void)
{
    FILE *f = fopen(bake_path(), "wb");
    if (!f) return;
    u32 h[3] = { BC_MAGIC, (u32)NB, (u32)g_nbc };
    fwrite(h, sizeof h, 1, f);
    fwrite(g_bc, sizeof(bclip), g_nbc, f);
    fclose(f);
    klog("redclips: %d clips recorded, saved to %s\n", g_nbc, bake_path());
}
int redclip_load(void)
{
    FILE *f = fopen(bake_path(), "rb");
    if (!f) return 0;
    u32 h[3]; int ok = fread(h, sizeof h, 1, f) == 1 && h[0] == BC_MAGIC && h[1] == NB && h[2] > 0 && h[2] <= BC_MAX;
    if (ok) { bc_get(0); ok = fread(g_bc, sizeof(bclip), h[2], f) == h[2]; if (ok) g_nbc = (int)h[2]; }
    fclose(f);
    klog("redclips: %s %s\n", ok ? "loaded" : "could not read", bake_path());
    return ok;
}

/* clip layers, set by debugmenu.c each frame: clip index, position 0..1, weight, body part,
 * whether to take out the clip's own lift (his flight clips raise the pelvis by ~7 units) */
enum { LM_FULL, LM_UPPER, LM_LOWER };
RS struct rclayer { int clip; float u, w; int mask; int nolift; } g_rcl[3] = { { -1 }, { -1 }, { -1 } };
static int in_mask(int bone_i, int mask)
{
    int lower = bone_i == 0 || bone_i == 2 || bone_i >= 37;   /* root, pelvis, legs, tail */
    return mask == LM_FULL || (mask == LM_LOWER) == lower;
}
static void clip_layers(bone *b)
{
    static bone tmp[NB];
    for (int L = 0; L < 3; L++) {
        struct rclayer *l = &g_rcl[L];
        if (l->clip < 0 || l->clip >= g_nbc || l->w < 1e-3f) continue;
        redclip_sample(l->clip, l->u, tmp);
        if (l->nolift) {
            float y = g_idle_ok ? g_idle_ref[2].t[1] : 1.6f;
            float dy = y - tmp[2].t[1], dx = b[2].t[0] - tmp[2].t[0], dz = b[2].t[2] - tmp[2].t[2];
            tmp[2].t[1] = y; tmp[2].t[0] = b[2].t[0]; tmp[2].t[2] = b[2].t[2];
            tmp[1].t[0] += dx; tmp[1].t[1] += dy; tmp[1].t[2] += dz;    /* the staff hangs off the root: same shift */
        }
        float w = l->w > 1 ? 1 : l->w;
        for (int i = 0; i < NB; i++) if (in_mask(i, l->mask)) lerp_bone(&b[i], &b[i], &tmp[i], w);
    }
}

/* ---- casting (the breaths): a calm, upright spell pose made for him ----
 * staff raised high and upright in the staff hand, the other arm reaching forward with the palm
 * out, chest lifted, head up, wings spread wide and slowly breathing, tail curled, a gentle float */
RS float g_pose_cast, g_cast_spread = 0.3f, g_cast_tip = 0.25f;
static void cast_pose(bone *b, float w, float t)
{
    if (w < 1e-3f) return;
    int sh = g_staff_hand == 30 ? 27 : 33, oth = sh == 27 ? 33 : 27;
    float side = sh == 27 ? 1.0f : -1.0f, br = sinf(t * 2.2f);
    b[2].t[1] += w * (0.12f + 0.06f * br);                            /* rises a little onto his toes */
    rot_world_axis(b, 4, 1, 0, 0, w * 0.18f);                         /* chest lifted, leaning back */
    rot_world_axis(b, 6, 1, 0, 0, w * 0.22f);                         /* head up */
    rot_world_axis(b, 8, 1, 0, 0, w * 0.08f);
    rot_world_axis(b, sh, 1, 0, 0, w * 2.35f);                        /* staff arm up overhead */
    rot_world_axis(b, sh, 0, 0, 1, w * 0.35f * side);                 /* a little out to the side */
    rot_world_axis(b, sh + 1, 1, 0, 0, w * 0.25f);                    /* elbow almost straight */
    rot_world_axis(b, oth, 1, 0, 0, w * 1.25f);                       /* other arm reaching forward */
    rot_world_axis(b, oth, 0, 0, 1, -w * 0.25f * side);
    rot_world_axis(b, oth + 1, 1, 0, 0, -w * 0.15f);
    rot_world_axis(b, oth + 3, 1, 0, 0, -w * 0.6f);                   /* palm out */
    float spread = g_cast_spread + 0.06f * br;                        /* wings open, breathing slowly */
    rot_world_axis(b, 15, 0, 0, 1, -w * spread); rot_world_axis(b, 21, 0, 0, 1, w * spread);
    rot_world_axis(b, 17, 0, 0, 1, w * g_cast_tip); rot_world_axis(b, 23, 0, 0, 1, -w * g_cast_tip);
    for (int i = 41; i <= 49; i++) rot_world_axis(b, i, 0, 1, 0, w * (0.07f + 0.02f * sinf(t * 1.5f - (i - 41) * 0.5f)));
    rot_world_axis(b, 37, 1, 0, 0, w * 0.08f); rot_world_axis(b, 50, 1, 0, 0, -w * 0.05f);   /* feet set apart */
}
/* ---- squashed (the game's squash state, mode 67): flattened like Spyro, lying spread out ----
 * the flattening itself is the model scale (debugmenu.c); this lays him out: legs and arms out
 * flat to the sides, wings spread on the ground, head down, a little quiver */
RS float g_pose_squash;
static void squash_pose(bone *b, float w, float t)
{
    /* spread-eagled, belly down, like Spyro's squash: arms and legs straight out to the sides
     * (standing frame: +x limbs rotate +, -x limbs -), wings opened out wide and swept back so
     * they don't overlap, tail straight; then the body tipped forward flat onto the ground */
    if (w < 1e-3f) return;
    float q = w < 0.98f ? 0.035f * sinf(t * 23.0f) * w : 0.0f;   /* (a shiver on the way down only) */
    rot_world_axis(b, 37, 0, 0, 1, w * 1.25f); rot_world_axis(b, 50, 0, 0, 1, -w * 1.25f);     /* legs out sideways */
    rot_world_axis(b, 38, 1, 0, 0, w * 0.3f); rot_world_axis(b, 51, 1, 0, 0, w * 0.3f);         /* knees straight */
    rot_world_axis(b, 27, 0, 0, 1, w * 1.35f); rot_world_axis(b, 33, 0, 0, 1, -w * 1.35f);     /* arms out (a T) */
    rot_world_axis(b, 27, 0, 1, 0, -w * 0.5f); rot_world_axis(b, 33, 0, 1, 0, w * 0.5f);       /* and reaching forward */
    rot_world_axis(b, 28, 1, 0, 0, w * 0.4f); rot_world_axis(b, 34, 1, 0, 0, w * 0.4f);
    static RS float wz = 99, wy, wx; if (wz == 99) { wz = -1.4f; wy = 0.7f; wx = 0; if (getenv("XBOX_SQ_WING")) sscanf(getenv("XBOX_SQ_WING"), "%f,%f,%f", &wz, &wy, &wx); }
    rot_world_axis(b, 15, 1, 0, 0, w * wx); rot_world_axis(b, 21, 1, 0, 0, w * wx);
    rot_world_axis(b, 15, 0, 0, 1, w * (wz - q)); rot_world_axis(b, 21, 0, 0, 1, -w * (wz - q));   /* wings open */
    rot_world_axis(b, 15, 0, 1, 0, w * wy); rot_world_axis(b, 21, 0, 1, 0, -w * wy);
    for (int i = 41; i <= 49; i++) rot_world_axis(b, i, 1, 0, 0, w * 0.06f);                  /* tail straight back */
    rot_world_axis(b, 6, 1, 0, 0, w * 0.35f);                                                /* chin up off the ground */
    rot_world_axis(b, 2, 1, 0, 0, -w * (1.45f + q));                                         /* tip forward, flat */
    b[2].t[1] -= w * 1.15f;
    b[2].t[2] += w * 0.4f;
}


/* ---- the staff combo, made here: four swings keyed by hand ----
 * A forehand cut (high on the staff side down across to low on the other), B a backhand sweep
 * rising back the other way, C a spin with the staff held straight out, D a leaping two-handed
 * slam. Each move is a few keys (anticipation, strike, overshoot, recovery) over the controls
 * below; the values flow through the keys on a Catmull-Rom curve, each span timed by its own
 * ease (a wind-up slows into the coil, a strike speeds into the hit, a follow-through slows out
 * of it). Values are for the staff in his +x hand and mirrored for the other. The staff itself
 * is pointed along the swing (AZ/EL, after staff_follow), so the blade line - and the trail drawn
 * along it - is exactly what's keyed; the tail and wings follow the body on springs. */
enum { C_TW, C_LEAN, C_BEND, C_DROP, C_SP, C_SY, C_SO, C_SE, C_OP, C_OY, C_OO, C_OE,
       C_AZ, C_EL, C_HD, C_WG, C_STEP, C_KNEE, C_IK, C_NUM };
/* TW torso twist (+ toward the staff side), LEAN forward, BEND sideways (+ toward the staff side),
 * DROP pelvis down; SP/SY/SO/SE staff arm: raised forward, swung across the body, out to the side,
 * elbow bent; OP/OY/OO/OE the same for the other arm; AZ/EL where the staff points (0 = straight
 * ahead, + toward the staff side / up); HD head turn; WG wings opened; STEP the other foot forward;
 * KNEE knees bent; IK the other hand on the staff (0..1) */
enum { E_SMOOTH, E_OUT, E_IN, E_LIN };
typedef struct { float t; int ease; float v[C_NUM]; } sckey;
/*                      TW    LEAN  BEND  DROP  SP    SY    SO    SE    OP    OY    OO    OE    AZ    EL    HD    WG    STEP  KNEE  IK */
#define SC_READY      { 0.0f, 0.10f, 0.0f, 0.10f, 0.60f, 0.20f, 0.10f, 0.50f, 0.50f, 0.20f, 0.10f, 0.60f, 0.30f, 0.90f, 0.0f, 0.20f, 0.25f, 0.25f, 0 }
static const sckey sc_a[] = {
    { 0.00f, E_SMOOTH, SC_READY },
    { 0.09f, E_OUT,  {  0.60f,-0.05f, 0.10f, 0.12f, 2.30f,-0.30f, 0.50f, 0.90f, 1.00f, 0.50f, 0.00f, 0.50f, 2.20f, 0.90f,-0.40f, 0.40f, 0.35f, 0.30f, 0 } },
    { 0.155f, E_IN,  { -0.50f, 0.35f,-0.15f, 0.30f, 1.10f, 0.90f, 0.10f, 0.10f,-0.30f,-0.20f, 0.60f, 0.30f,-0.60f,-0.40f, 0.30f, 0.90f, 0.55f, 0.50f, 0 } },
    { 0.23f, E_OUT,  { -0.80f, 0.45f,-0.20f, 0.32f, 0.80f, 1.20f, 0.10f, 0.20f,-0.40f,-0.20f, 0.70f, 0.30f,-1.30f,-0.60f, 0.40f, 0.70f, 0.55f, 0.50f, 0 } },
    { 0.48f, E_SMOOTH, SC_READY },
};
static const sckey sc_b[] = {
    { 0.00f, E_SMOOTH, { -0.80f, 0.45f,-0.20f, 0.32f, 0.80f, 1.20f, 0.10f, 0.20f,-0.40f,-0.20f, 0.70f, 0.30f,-1.30f,-0.50f, 0.40f, 0.70f, 0.55f, 0.50f, 0 } },
    { 0.07f, E_OUT,  { -0.95f, 0.30f,-0.25f, 0.30f, 0.70f, 1.30f, 0.00f, 0.60f, 0.20f, 0.30f, 0.30f, 0.50f,-1.80f,-0.30f, 0.50f, 0.50f, 0.45f, 0.45f, 0 } },
    { 0.14f, E_IN,   {  0.55f, 0.25f, 0.10f, 0.25f, 1.40f,-0.60f, 0.60f, 0.05f, 0.30f,-0.30f, 0.50f, 0.40f, 1.00f, 0.25f,-0.30f, 1.00f, 0.40f, 0.40f, 0 } },
    { 0.21f, E_OUT,  {  0.80f, 0.20f, 0.15f, 0.25f, 1.60f,-0.90f, 0.80f, 0.10f, 0.30f,-0.30f, 0.60f, 0.40f, 1.70f, 0.50f,-0.40f, 0.80f, 0.40f, 0.40f, 0 } },
    { 0.46f, E_SMOOTH, SC_READY },
};
static const sckey sc_c[] = {
    { 0.00f, E_SMOOTH, {  0.80f, 0.20f, 0.15f, 0.25f, 1.60f,-0.90f, 0.80f, 0.10f, 0.30f,-0.30f, 0.60f, 0.40f, 1.70f, 0.40f,-0.40f, 0.80f, 0.40f, 0.40f, 0 } },
    { 0.08f, E_OUT,  {  0.90f, 0.15f, 0.10f, 0.35f, 1.30f,-0.40f, 1.10f, 0.00f, 0.40f, 0.00f, 0.90f, 0.20f, 1.50f, 0.00f,-0.50f, 0.70f, 0.30f, 0.60f, 0 } },
    { 0.20f, E_SMOOTH, { -0.20f, 0.20f,-0.20f, 0.25f, 1.40f,-0.20f, 1.20f, 0.00f, 0.40f, 0.00f, 1.00f, 0.20f, 1.45f,-0.05f, 0.00f, 1.00f, 0.30f, 0.50f, 0 } },
    { 0.34f, E_OUT,  { -0.40f, 0.25f,-0.15f, 0.25f, 1.20f, 0.10f, 0.90f, 0.10f, 0.40f, 0.00f, 0.80f, 0.30f, 0.90f, 0.00f, 0.20f, 0.70f, 0.30f, 0.45f, 0 } },
    { 0.55f, E_SMOOTH, SC_READY },
};
static const sckey sc_d[] = {
    { 0.00f, E_SMOOTH, SC_READY },
    { 0.15f, E_OUT,  {  0.00f,-0.35f, 0.00f,-0.80f, 2.90f, 0.30f, 0.15f, 0.40f, 2.80f, 0.30f, 0.15f, 0.50f, 0.00f, 2.10f, 0.00f, 1.00f, 0.20f, 0.00f, 1 } },
    { 0.23f, E_IN,   {  0.00f, 0.65f, 0.00f, 0.70f, 1.00f, 0.20f, 0.00f, 0.10f, 1.00f, 0.20f, 0.00f, 0.20f, 0.00f,-0.75f, 0.00f, 0.30f, 0.50f, 1.00f, 1 } },
    { 0.42f, E_OUT,  {  0.00f, 0.60f, 0.00f, 0.65f, 1.00f, 0.20f, 0.00f, 0.15f, 1.00f, 0.20f, 0.00f, 0.25f, 0.00f,-0.70f, 0.00f, 0.40f, 0.50f, 0.95f, 1 } },
    { 0.72f, E_SMOOTH, SC_READY },
};
/* timings (s): when it hits, when the next swing may start, how long it lasts; the window he
 * steps forward in and how far (units per second); the window the trail is drawn in */
struct scmove { const sckey *k; int nk; float hit, cancel, len, l0, l1, lv, tr0, tr1; };
static const struct scmove g_scm[4] = {
    { sc_a, 5, 0.150f, 0.22f, 0.48f, 0.07f, 0.17f, 15.0f, 0.085f, 0.24f },
    { sc_b, 5, 0.130f, 0.20f, 0.46f, 0.06f, 0.15f, 13.0f, 0.065f, 0.22f },
    { sc_c, 5, 0.200f, 0.36f, 0.55f, 0.06f, 0.30f, 6.0f, 0.070f, 0.36f },
    { sc_d, 5, 0.230f, 0.46f, 0.72f, 0.04f, 0.22f, 12.0f, 0.150f, 0.27f },
};
RS int g_sc_move = -1;                 /* the move playing (debugmenu.c), -1 none */
RS float g_sc_t, g_sc_legs = 1;        /* its time (s); how much the legs join in (0 moving / in the air) */
RS int g_sc_side = 1;                  /* +1 staff in his +x hand, -1 the other (for the spin's direction) */
RS float g_sc_w;                       /* the layer's weight this frame */
static RS float g_sc_az, g_sc_el;      /* where the staff points (radians), for staff_point */
void redcombo_times(int m, float *hit, float *cancel, float *len, float *l0, float *l1, float *lv, float *tr0, float *tr1)
{
    const struct scmove *s = &g_scm[m < 0 ? 0 : m > 3 ? 3 : m];
    *hit = s->hit; *cancel = s->cancel; *len = s->len; *l0 = s->l0; *l1 = s->l1; *lv = s->lv; *tr0 = s->tr0; *tr1 = s->tr1;
}
static float ease_k(int e, float u)
{
    switch (e) {
    case E_OUT: return 1 - (1 - u) * (1 - u);
    case E_IN: return u * u;
    case E_LIN: return u;
    default: return u * u * (3 - 2 * u);
    }
}
static float catmull(float p0, float p1, float p2, float p3, float u)
{
    float u2 = u * u, u3 = u2 * u;
    return 0.5f * (2 * p1 + (p2 - p0) * u + (2 * p0 - 5 * p1 + 4 * p2 - p3) * u2 + (3 * p1 - p0 - 3 * p2 + p3) * u3);
}
static void sc_sample(const struct scmove *m, float t, float *v)
{
    const sckey *k = m->k; int n = m->nk;
    if (t <= k[0].t) { memcpy(v, k[0].v, sizeof k[0].v); return; }
    if (t >= k[n - 1].t) { memcpy(v, k[n - 1].v, sizeof k[0].v); return; }
    int i = 0; while (i + 2 < n && k[i + 1].t <= t) i++;
    float u = ease_k(k[i + 1].ease, (t - k[i].t) / (k[i + 1].t - k[i].t));
    const sckey *a = &k[i > 0 ? i - 1 : 0], *b = &k[i + 2 < n ? i + 2 : n - 1];
    for (int c = 0; c < C_NUM; c++) v[c] = catmull(a->v[c], k[i].v[c], k[i + 1].v[c], b->v[c], u);
}
/* two-bone reach: the arm from shoulder a (a+1 elbow, a+2 wrist) bent and turned so the wrist
 * comes to target T (model space), by weight w */
static void ik2(bone *b, int a, const float *T, float w)
{
    if (w < 1e-3f) return;
    fk_full(b);
    float *A = g_wp[a], *B = g_wp[a + 1], *C = g_wp[a + 2];
    float ab[3], bc[3], ac[3], at[3];
    for (int k = 0; k < 3; k++) { ab[k] = B[k] - A[k]; bc[k] = C[k] - B[k]; ac[k] = C[k] - A[k]; at[k] = T[k] - A[k]; }
    float l1 = sqrtf(ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2]), l2 = sqrtf(bc[0] * bc[0] + bc[1] * bc[1] + bc[2] * bc[2]);
    float lt = sqrtf(at[0] * at[0] + at[1] * at[1] + at[2] * at[2]), lc = sqrtf(ac[0] * ac[0] + ac[1] * ac[1] + ac[2] * ac[2]);
    if (l1 < 1e-4f || l2 < 1e-4f || lt < 1e-4f || lc < 1e-4f) return;
    if (lt > (l1 + l2) * 0.999f) lt = (l1 + l2) * 0.999f;
    /* elbow: from its angle now to the one that makes shoulder-wrist as long as shoulder-target */
    float cur = acosf(fmaxf(-1, fminf(1, -(ab[0] * bc[0] + ab[1] * bc[1] + ab[2] * bc[2]) / (l1 * l2))));
    float want = acosf(fmaxf(-1, fminf(1, (l1 * l1 + l2 * l2 - lt * lt) / (2 * l1 * l2))));
    float n[3] = { ab[1] * bc[2] - ab[2] * bc[1], ab[2] * bc[0] - ab[0] * bc[2], ab[0] * bc[1] - ab[1] * bc[0] };
    float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (nl < 1e-5f) { n[0] = 1; n[1] = 0; n[2] = 0; nl = 1; }
    rot_world_axis(b, a + 1, n[0] / nl, n[1] / nl, n[2] / nl, -(want - cur) * w);
    /* then the whole arm turned at the shoulder onto the target */
    fk_full(b);
    for (int k = 0; k < 3; k++) ac[k] = g_wp[a + 2][k] - g_wp[a][k];
    float ax[3] = { ac[1] * at[2] - ac[2] * at[1], ac[2] * at[0] - ac[0] * at[2], ac[0] * at[1] - ac[1] * at[0] };
    float al = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
    lc = sqrtf(ac[0] * ac[0] + ac[1] * ac[1] + ac[2] * ac[2]); lt = sqrtf(at[0] * at[0] + at[1] * at[1] + at[2] * at[2]);
    if (al < 1e-6f || lc < 1e-4f) return;
    float d = (ac[0] * at[0] + ac[1] * at[1] + ac[2] * at[2]) / (lc * lt);
    rot_world_axis(b, a, ax[0] / al, ax[1] / al, ax[2] / al, acosf(fmaxf(-1, fminf(1, d))) * w);
}
static RS float g_sc_v[C_NUM];
static RS quat g_sc_qref;              /* the staff's own (clip) orientation this frame: what AZ/EL turn from */
static void combo_pose(bone *b)
{
    static RS struct { float x, v; } sp_tail, sp_wing, sp_lean;           /* the springs */
    float dt = g_dt_pose;
    fk_full(b); g_sc_qref = g_wq[1];
    int on = g_sc_move >= 0 && g_sc_move < 4;
    const struct scmove *m = on ? &g_scm[g_sc_move] : NULL;
    float w = 0, *v = g_sc_v;
    if (on) {
        sc_sample(m, g_sc_t, v);
        float f0 = m->cancel + 0.06f;                                   /* settle back into his stance at the end */
        w = 1 - ease((g_sc_t - f0) / (m->len - f0));
    }
    /* tail and wings swing after the body, overshoot and settle (they keep moving after it stops) */
    float tw = w * (on ? v[C_TW] : 0), wg = w * (on ? v[C_WG] : 0), ln = w * (on ? v[C_LEAN] : 0);
    sp_tail.v += (180.0f * (-tw - sp_tail.x) - 11.0f * sp_tail.v) * dt; sp_tail.x += sp_tail.v * dt;
    sp_wing.v += (260.0f * (wg - sp_wing.x) - 14.0f * sp_wing.v) * dt; sp_wing.x += sp_wing.v * dt;
    sp_lean.v += (200.0f * (ln - sp_lean.x) - 13.0f * sp_lean.v) * dt; sp_lean.x += sp_lean.v * dt;
    g_sc_w = w;
    if (w < 1e-3f && fabsf(sp_tail.x) + fabsf(sp_wing.x) + fabsf(sp_lean.x) < 1e-3f) return;
    int sh = g_staff_hand == 30 ? 27 : 33, oh = sh == 27 ? 33 : 27;
    float S = sh == 27 ? 1.0f : -1.0f, L = g_sc_legs * w;
    g_sc_side = (int)S;
    if (w > 1e-3f) {
        /* arms (in the body's frame: the torso turns them after) */
        rot_world_axis(b, sh, 0, 0, 1, w * v[C_SO] * S);
        rot_world_axis(b, sh, 1, 0, 0, w * v[C_SP]);
        rot_world_axis(b, sh, 0, 1, 0, w * v[C_SY] * S);
        rot_world_axis(b, sh + 1, 1, 0, 0, w * v[C_SE]);
        rot_world_axis(b, oh, 0, 0, 1, -w * v[C_OO] * S);
        rot_world_axis(b, oh, 1, 0, 0, w * v[C_OP]);
        rot_world_axis(b, oh, 0, 1, 0, -w * v[C_OY] * S);
        rot_world_axis(b, oh + 1, 1, 0, 0, w * v[C_OE]);
        /* torso: twist through the hips and chest, lean, bend; the head keeps on the target */
        rot_world_axis(b, 3, 0, 1, 0, -w * 0.45f * v[C_TW] * S);
        rot_world_axis(b, 5, 0, 1, 0, -w * 0.35f * v[C_TW] * S);
        rot_world_axis(b, 2, 0, 1, 0, -L * 0.25f * v[C_TW] * S);
        rot_world_axis(b, 3, 0, 0, 1, -w * v[C_BEND] * S);
        rot_world_axis(b, 6, 0, 1, 0, -w * (v[C_HD] - 0.85f * v[C_TW]) * 0.5f * S);
        rot_world_axis(b, 8, 0, 1, 0, -w * (v[C_HD] - 0.85f * v[C_TW]) * 0.5f * S);
        /* legs: a fighting stance - the other foot forward, knees bent, low */
        int ol = S > 0 ? 50 : 37, sl = ol == 50 ? 37 : 50;
        b[2].t[1] -= L * v[C_DROP] + (1 - g_sc_legs) * w * v[C_DROP] * 0.3f;
        rot_world_axis(b, ol, 1, 0, 0, L * (v[C_STEP] + 0.5f * v[C_KNEE]));
        rot_world_axis(b, sl, 1, 0, 0, L * (-0.6f * v[C_STEP] + 0.5f * v[C_KNEE]));
        rot_world_axis(b, ol + 1, 1, 0, 0, -L * 0.9f * v[C_KNEE]);
        rot_world_axis(b, sl + 1, 1, 0, 0, -L * 1.0f * v[C_KNEE]);
        rot_world_axis(b, ol + 2, 1, 0, 0, L * 0.4f * v[C_KNEE]);
        rot_world_axis(b, sl + 2, 1, 0, 0, L * 0.5f * v[C_KNEE]);
    }
    /* the lean (sprung), the wings (sprung), the tail dragged round after the twist */
    rot_world_axis(b, 2, 1, 0, 0, -sp_lean.x);
    rot_world_axis(b, 15, 0, 0, 1, -0.7f * sp_wing.x); rot_world_axis(b, 21, 0, 0, 1, 0.7f * sp_wing.x);
    rot_world_axis(b, 17, 0, 0, 1, 0.25f * sp_wing.x); rot_world_axis(b, 23, 0, 0, 1, -0.25f * sp_wing.x);
    for (int i = 41; i <= 49; i++) rot_world_axis(b, i, 0, 1, 0, -0.10f * sp_tail.x * S * (0.6f + 0.1f * (i - 41)));
    g_sc_az = v[C_AZ] * S; g_sc_el = v[C_EL];
}

static void modify(bone *b)
{
    if (g_pose_mirror) mirror(b);
    if (g_pose_capture_idle) { memcpy(g_idle_ref, b, sizeof g_idle_ref); g_idle_ok = 1; }
    g_base_cur += (g_pose_base_w - g_base_cur) * (g_dt_pose * 10 > 1 ? 1 : g_dt_pose * 10);
    if (g_idle_ok && g_base_cur > 0.001f)
        for (int i = 0; i < NB; i++) lerp_bone(&b[i], &b[i], &g_idle_ref[i], g_base_cur);
    if (g_pose_nolift > 1e-3f) {                     /* his flight clips lift the pelvis: keep him at standing height */
        float y = g_idle_ok ? g_idle_ref[2].t[1] : 1.6f;
        b[2].t[1] += (y - b[2].t[1]) * g_pose_nolift;
    }
    clip_layers(b);
    staff_grab(b);
    if (g_pose_synth) synth(b, g_pose_synth, g_pose_synth_t);
    {   static RS int old = -1; if (old < 0) old = getenv("XBOX_OLD_GAIT") != NULL;
        if (old) gait(b, g_pose_gait, g_pose_gait_phase); else gait2(b, g_pose_gait, g_pose_gait_phase); }
    breathe(b, g_pose_time, g_pose_breathe * (1 - g_pose_gait));
    lean_bank(b);
    charge_pose(b, g_pose_time);
    air_pose(b, g_pose_time);
    if (g_pose_flap > 0) wings(b, g_pose_flap, g_pose_time);
    melee(b, g_pose_melee_t);
    combo_pose(b);
    if (g_pose_aim > 1e-3f) {                         /* aiming / casting: staff thrust out at the aim, the other hand guiding */
        extern float g_aim_pitch, g_cast_recoil; extern int g_red_aiming;
        int sh = g_staff_hand == 30 ? 27 : 33, oth = sh == 27 ? 33 : 27;
        float w = g_pose_aim, side = sh == 27 ? 1.0f : -1.0f, pt = g_red_aiming ? g_aim_pitch : 0.0f, rc = g_cast_recoil;
        rot_world_axis(b, 3, 0, 1, 0, side * 0.35f * w);                     /* shoulders square to the shot */
        rot_world_axis(b, 4, 1, 0, 0, -w * (0.08f + 0.12f * rc));             /* leaning into it, rocked back by each cast */
        rot_world_axis(b, 6, 1, 0, 0, w * pt * 0.6f);                          /* head follows the aim */
        rot_world_axis(b, sh, 1, 0, 0, w * (1.45f + pt * 0.9f - 0.35f * rc));
        rot_world_axis(b, sh, 0, 0, 1, -w * 0.25f * side);                    /* in toward the centre line */
        rot_world_axis(b, sh + 1, 1, 0, 0, w * (0.15f + 0.35f * rc));         /* elbow gives on the recoil */
        rot_world_axis(b, oth, 1, 0, 0, w * (1.1f + pt * 0.7f));               /* off hand reaching after it */
        rot_world_axis(b, oth, 0, 0, 1, w * 0.45f * side);
        rot_world_axis(b, oth + 3, 1, 0, 0, -w * 0.7f);                        /* palm out */
        rot_world_axis(b, 15, 0, 0, 1, -w * 0.18f); rot_world_axis(b, 21, 0, 0, 1, w * 0.18f);   /* wings half open */
    }
    cast_pose(b, g_pose_cast, g_pose_time);
    if (g_pose_dive > 1e-3f) {                        /* ground pound: tipped nose-down, staff raised to smash, wings swept back */
        int sh = g_staff_hand == 30 ? 27 : 33, oth = sh == 27 ? 33 : 27; float w = g_pose_dive;
        rot_world_axis(b, 2, 1, 0, 0, -w * 0.55f);
        rot_world_axis(b, sh, 1, 0, 0, w * 2.5f); rot_world_axis(b, sh + 1, 1, 0, 0, w * 0.3f);
        rot_world_axis(b, oth, 1, 0, 0, w * 2.2f);
        rot_world_axis(b, 15, 0, 1, 0, w * 0.9f); rot_world_axis(b, 21, 0, 1, 0, -w * 0.9f);    /* wings back */
        rot_world_axis(b, 15, 0, 0, 1, w * 0.5f); rot_world_axis(b, 21, 0, 0, 1, -w * 0.5f);
        rot_world_axis(b, 37, 1, 0, 0, w * 0.6f); rot_world_axis(b, 50, 1, 0, 0, w * 0.6f);     /* legs tucked */
        for (int i = 41; i <= 49; i++) rot_world_axis(b, i, 1, 0, 0, w * 0.08f);                 /* tail up behind */
    }
    if (g_pose_spin > 1e-3f) {                        /* tail-spin: staff swept straight out, tail and wings flung wide */
        int sh = g_staff_hand == 30 ? 27 : 33, oth = sh == 27 ? 33 : 27; float w = g_pose_spin, side = sh == 27 ? 1.0f : -1.0f;
        rot_world_axis(b, sh, 0, 0, 1, w * 1.35f * side); rot_world_axis(b, sh + 1, 1, 0, 0, -w * 0.2f);
        rot_world_axis(b, oth, 0, 0, 1, -w * 1.2f * side);
        rot_world_axis(b, 2, 0, 0, 1, w * 0.15f * side);                                          /* leaning out of the turn */
        rot_world_axis(b, 15, 0, 0, 1, -w * 0.6f); rot_world_axis(b, 21, 0, 0, 1, w * 0.6f);
        for (int i = 41; i <= 49; i++) rot_world_axis(b, i, 1, 0, 0, -w * 0.07f);                /* tail straight out behind */
        for (int i = 41; i <= 49; i++) rot_world_axis(b, i, 0, 1, 0, w * 0.06f * side);          /* and dragged round */
    }
    if (g_pose_shield > 1e-3f) {                      /* wing shield: wings wrapped forward round him, staff across, head down */
        int sh = g_staff_hand == 30 ? 27 : 33, oth = sh == 27 ? 33 : 27; float w = g_pose_shield, side = sh == 27 ? 1.0f : -1.0f;
        static RS float wy = 99, wz, wt; if (wy == 99) { wy = -1.6f; wz = -0.25f; wt = -0.7f; if (getenv("XBOX_SHIELD_WING")) sscanf(getenv("XBOX_SHIELD_WING"), "%f,%f,%f", &wy, &wz, &wt); }
        rot_world_axis(b, 15, 0, 1, 0, -w * wy); rot_world_axis(b, 21, 0, 1, 0, w * wy);          /* wings swung round to the front */
        rot_world_axis(b, 15, 0, 0, 1, -w * wz); rot_world_axis(b, 21, 0, 0, 1, w * wz);
        rot_world_axis(b, 17, 0, 1, 0, -w * wt); rot_world_axis(b, 23, 0, 1, 0, w * wt);          /* tips curled in */
        rot_world_axis(b, sh, 1, 0, 0, w * 1.0f); rot_world_axis(b, sh, 0, 0, 1, -w * 0.6f * side);
        rot_world_axis(b, oth, 1, 0, 0, w * 0.9f);
        rot_world_axis(b, 4, 1, 0, 0, -w * 0.25f); rot_world_axis(b, 6, 1, 0, 0, -w * 0.3f);        /* hunched, head down */
        b[2].t[1] -= w * 0.25f;                                                                    /* braced low */
    }
    squash_pose(b, g_pose_squash, g_pose_time);
    look(b, g_pose_look);
    {   /* squashed flat: held completely still, like Spyro (his idle, breathing and looking about
         * keep running underneath, so the flattened pose is caught once and kept) */
        static RS bone held[NB]; static RS int have;
        if (g_pose_squash > 0.98f) { if (!have) { memcpy(held, b, sizeof held); have = 1; } else memcpy(b, held, sizeof held); }
        else have = 0;
    }
    staff_follow(b);
    {   /* the staff head (its crystal), in model space from our own FK of the final pose. In his idle
         * the staff stands upright in his hand with the crystal ~1.9 above the grip; that point is
         * kept in the staff's own frame, so it follows the staff. */
        extern float g_staff_head[3]; extern int g_staff_head_ok;
        static RS float c_loc[3]; static RS int have;
        int h = g_staff_hand ? g_staff_hand : 36;
        fk_full(b);
        static RS float H = -1; if (H < 0) H = getenv("XBOX_STAFF_HEAD") ? atof(getenv("XBOX_STAFF_HEAD")) : 1.7f;
        if ((g_pose_capture_idle && g_pose_cast < 1e-3f && g_pose_aim < 1e-3f && g_sc_w < 1e-3f) || !have) {
            float v[3] = { 0, H, 0 };                       /* in the staff's own frame (rigid with it) */
            qrot(qconj(g_wq[1]), v, c_loc); have = 1;
        }
        /* casting: the staff is turned in his hand to stand upright, crystal to the sky */
        extern float g_aim_pitch; extern int g_red_aiming;
        float sw = g_pose_cast > g_pose_aim ? g_pose_cast : g_pose_aim;
        if (g_sc_w > 1e-3f && g_sc_w >= sw) {
            /* the combo: the staff pointed along the swing, turned from its own upright hold (not
             * from wherever the arm left it, so a big swing never flips it the short way round) */
            float cr[3]; qrot(g_sc_qref, c_loc, cr);
            float t[3] = { sinf(g_sc_az) * cosf(g_sc_el), sinf(g_sc_el), -cosf(g_sc_az) * cosf(g_sc_el) };
            float n = sqrtf(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]);
            if (n > 1e-4f) {
                for (int k = 0; k < 3; k++) cr[k] /= n;
                float ax[3] = { cr[1] * t[2] - cr[2] * t[1], cr[2] * t[0] - cr[0] * t[2], cr[0] * t[1] - cr[1] * t[0] };
                float d = cr[0] * t[0] + cr[1] * t[1] + cr[2] * t[2], al = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
                quat R = al > 1e-5f ? qaxis(ax[0] / al, ax[1] / al, ax[2] / al, acosf(d > 1 ? 1 : d < -1 ? -1 : d)) : (quat){ 0, 0, 0, 1 };
                quat qn = qslerp(g_wq[1], qmul(R, g_sc_qref), g_sc_w), D = qmul(qn, qconj(g_wq[1]));
                float rel[3] = { g_wp[1][0] - g_wp[h][0], g_wp[1][1] - g_wp[h][1], g_wp[1][2] - g_wp[h][2] }, rr[3], p1[3];
                qrot(D, rel, rr); for (int k = 0; k < 3; k++) p1[k] = g_wp[h][k] + rr[k];
                quat iq0 = qconj(g_wq[0]);
                b[1].q = qnorm(qmul(iq0, qn));
                float d0[3] = { p1[0] - g_wp[0][0], p1[1] - g_wp[0][1], p1[2] - g_wp[0][2] };
                qrot(iq0, d0, b[1].t);
                fk_full(b);
            }
            sw = 0;
        }
        if (sw > 1e-3f) {
            float cd[3]; qrot(g_wq[1], c_loc, cd);
            for (int k = 0; k < 3; k++) cd[k] += g_wp[1][k] - g_wp[h][k];
            float n = sqrtf(cd[0] * cd[0] + cd[1] * cd[1] + cd[2] * cd[2]);
            if (n > 1e-4f) {
                for (int k = 0; k < 3; k++) cd[k] /= n;
                float t[3] = { 0, 0.97f, -0.24f };
                if (g_pose_aim > g_pose_cast) {                 /* aiming: the crystal points down the shot (model -z forward) */
                    float pt = g_red_aiming ? g_aim_pitch : 0.0f; t[0] = 0; t[1] = sinf(pt + 0.12f); t[2] = -cosf(pt + 0.12f);
                }
                float ax[3] = { cd[1] * t[2] - cd[2] * t[1], cd[2] * t[0] - cd[0] * t[2], cd[0] * t[1] - cd[1] * t[0] };
                float dt_ = cd[0] * t[0] + cd[1] * t[1] + cd[2] * t[2];
                float an = acosf(dt_ > 1 ? 1 : dt_ < -1 ? -1 : dt_) * sw, al = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
                if (al > 1e-5f) {
                    quat R = qaxis(ax[0] / al, ax[1] / al, ax[2] / al, an);
                    quat q1 = qmul(R, g_wq[1]);
                    float rel[3] = { g_wp[1][0] - g_wp[h][0], g_wp[1][1] - g_wp[h][1], g_wp[1][2] - g_wp[h][2] }, rr[3], p1[3];
                    qrot(R, rel, rr); for (int k = 0; k < 3; k++) p1[k] = g_wp[h][k] + rr[k];
                    quat iq0 = qconj(g_wq[0]);
                    b[1].q = qnorm(qmul(iq0, q1));
                    float d0[3] = { p1[0] - g_wp[0][0], p1[1] - g_wp[0][1], p1[2] - g_wp[0][2] };
                    qrot(iq0, d0, b[1].t);
                    fk_full(b);
                }
            }
        }
        float c[3]; qrot(g_wq[1], c_loc, c);
        for (int k = 0; k < 3; k++) g_staff_head[k] = g_wp[1][k] + c[k];
        g_staff_head_ok = 1;
        { extern float g_staff_grip[3]; for (int k = 0; k < 3; k++) g_staff_grip[k] = g_wp[h][k]; }
        if (g_sc_w > 1e-3f && g_sc_v[C_IK] > 1e-3f) {      /* two-handed: the other hand takes the shaft above the first */
            int oh = h == 30 ? 33 : 27; float T[3];
            for (int k = 0; k < 3; k++) T[k] = g_wp[h][k] + (g_staff_head[k] - g_wp[h][k]) * 0.38f;
            ik2(b, oh, T, g_sc_w * g_sc_v[C_IK]);
        }
        extern float g_staff_cal[4][3];
        for (int k = 0; k < 3; k++) { g_staff_cal[0][k] = g_staff_head[k]; g_staff_cal[1][k] = g_wp[h][k]; g_staff_cal[2][k] = g_wp[1][k]; g_staff_cal[3][k] = g_wp[10][k]; }
    }
    if (g_pose_synth == SYN_STAFFSPIN) rot_world_axis(b, 1, 1, 0, 0, g_pose_synth_t * 12.0f);
    if (g_pose_bonetest >= 0 && g_pose_bonetest < NB) rot_world_axis(b, g_pose_bonetest, 1, 0, 0, 1.2f);
    if (g_xf > 0 && g_last_ok) {
        for (int i = 0; i < NB; i++) lerp_bone(&b[i], &b[i], &g_last_out[i], ease(g_xf));
        g_xf -= g_dt_pose * g_xf_rate;
    }
    memcpy(g_last_out, b, sizeof g_last_out); g_last_ok = 1;
}

/* hooks */
void pose_enter_log(u32 a)
{
    if (a == g_red_animator) {
        static int n; if (n++ < 3) {
            u32 cnt = *(u8 *)(uintptr_t)(a + 0x118);
            klog("red animator %08x layers %u flags %02x:", a, cnt, *(u8 *)(uintptr_t)(a + 0x11b));
            for (u32 l = 0; l < cnt && l < 4; l++) {
                u32 node = *(u32 *)(uintptr_t)(a + 0x11c + l * 0x20 + 4);
                for (int k = 0; node && k < 4; k++) {
                    u32 tr = node - 4;
                    klog(" L%u track %08x vt %08x anim+0c %08x", l, tr, *(u32 *)(uintptr_t)tr, *(u32 *)(uintptr_t)(tr + 0xc));
                    node = *(u32 *)(uintptr_t)(tr + 4);
                }
            }
            klog("\n");
        }
    }
    static u32 seen[16]; static int ns; int k;
    for (k = 0; k < ns && seen[k] != a; k++) ;
    if (!g_red_animator) return;
    if (k == ns && ns < 16) { seen[ns++] = a; klog("pose enter: animator %08x (red %08x)\n", a, g_red_animator); }
}
__asm__(".globl pose_enter_stub\n"
        "pose_enter_stub:\n"
        "  movl %ecx, g_pose_animator\n"
        "  cmpl $0, g_pose_debug\n"
        "  je 3f\n"
        "  pushal\n"
        "  pushl %ecx\n"
        "  call pose_enter_log\n"
        "  addl $4, %esp\n"
        "  popal\n"
        "3:\n"
        "  subl $0x20, %esp\n"
        "  pushl %ebx\n"
        "  pushl %edi\n"
        "  pushl $0x1fce35\n"
        "  ret\n"
        ".globl pose_post_stub\n"
        "pose_post_stub:\n"
        "  pushal\n"
        "  pushl %esi\n"
        "  call red_pose_post\n"
        "  addl $4, %esp\n"
        "  popal\n"
        "  movl 0x14(%esi), %eax\n"
        "  testl %eax, %eax\n"
        "  pushl $0x1fd2db\n"
        "  ret\n"
        ".globl pose_end_stub\n"
        "pose_end_stub:\n"              /* replaces 'call 0x2523c0' at 0x1fcf6e, the end of 0x1fce30 */
        "  movl $0x2523c0, %eax\n"
        "  call *%eax\n"
        "  pushal\n"
        "  pushl g_pose_animator\n"
        "  call red_pose_post\n"
        "  addl $4, %esp\n"
        "  popal\n"
        "  ret\n"
        ".globl pose_out_stub\n"
        "pose_out_stub:\n"
        "  movl %ecx, g_pose_out\n"
        "  pushal\n"
        "  call red_pose_c\n"
        "  popal\n"
        "  pushl $0x2119f0\n"
        "  ret\n");
void pose_enter_stub(void);
void pose_out_stub(void);
void pose_post_stub(void);
void pose_end_stub(void);
/* research (XBOX_POSE_TRACE): which sampling functions run while Red's animator updates */
u32 g_tr_cnt[8];
#define TRACE_STUB(n, addr, nbytes, prologue) \
    __asm__(".globl tr_stub" #n "\n" "tr_stub" #n ":\n" \
            "  pushl %eax\n  movl g_pose_animator, %eax\n  cmpl g_red_animator, %eax\n  jne 1f\n" \
            "  incl g_tr_cnt+" #n "*4\n" "1:\n  popl %eax\n" prologue \
            "  pushl $" #addr "+" #nbytes "\n  ret\n"); \
    void tr_stub##n(void);
TRACE_STUB(0, 0x2110b0, 6, "  subl $0xe0, %esp\n")
TRACE_STUB(1, 0x210ad0, 6, "  subl $0x80, %esp\n")
TRACE_STUB(2, 0x216df0, 6, "  subl $0xb0, %esp\n")
TRACE_STUB(3, 0x2166f0, 6, "  subl $0x650, %esp\n")
static void hook6(u32 at, void (*fn)(void))
{
    u8 *p = (u8 *)(uintptr_t)at; u32 rel = (u32)(uintptr_t)fn - (at + 5);
    p[0] = 0xe9; memcpy(p + 1, &rel, 4); p[5] = 0x90;
}
u32 g_red_track, g_adv_cnt[8];
#define ADV_STUB(n, addr, nbytes, reg, prologue) \
    __asm__(".globl adv_stub" #n "\n" "adv_stub" #n ":\n" \
            "  cmpl g_red_track, %" #reg "\n  jne 1f\n  incl g_adv_cnt+" #n "*4\n1:\n" prologue \
            "  pushl $" #addr "+" #nbytes "\n  ret\n"); \
    void adv_stub##n(void);
ADV_STUB(0, 0x217680, 6, ecx, "  subl $8, %esp\n  pushl %esi\n  movl %ecx, %esi\n")
ADV_STUB(1, 0x2176bb, 7, esi, "  movl 0x14(%esp), %edx\n  movl 0x20(%esi), %ecx\n")
ADV_STUB(2, 0x2176ed, 8, esi, "  movss 0x80(%esi), %xmm2\n")
ADV_STUB(3, 0x217820, 6, ecx, "  pushl %esi\n  pushl %edi\n  movl 0xc(%esp), %edi\n")
ADV_STUB(4, 0x217696, 6, esi, "  pushl %edi\n  movl 0xc(%eax), %edi\n  testl %edi, %edi\n")
ADV_STUB(5, 0x2176a2, 5, esi, "  leal 8(%esp), %eax\n  pushl %eax\n")
ADV_STUB(6, 0x2148e7, 7, esi, "  movl 0x60(%esi), %eax\n  movss (%eax), %xmm0\n")
ADV_STUB(7, 0x21491c, 8, esi, "  movswl 0x18(%edi), %eax\n  movswl 0x1a(%edi), %edx\n")
static void hookn(u32 at, void (*fn)(void), int n)
{
    u8 *p = (u8 *)(uintptr_t)at; u32 rel = (u32)(uintptr_t)fn - (at + 5);
    p[0] = 0xe9; memcpy(p + 1, &rel, 4); for (int i = 5; i < n; i++) p[i] = 0x90;
}
void redpose_adv_report(void)
{
    static u32 n, last[8];
    if (n++ % 30) return;
    klog("advtrace 217820 %u 217680 %u res %u data %u | 2148b0: anim %u key %u | past2148b0 %u past70 %u\n", g_adv_cnt[3] - last[3], g_adv_cnt[0] - last[0],
         g_adv_cnt[4] - last[4], g_adv_cnt[5] - last[5], g_adv_cnt[6] - last[6], g_adv_cnt[7] - last[7], g_adv_cnt[1] - last[1], g_adv_cnt[2] - last[2]);
    memcpy(last, g_adv_cnt, sizeof last);
}
void redpose_trace_report(void)
{
    static u32 last[8], n;
    if (!getenv("XBOX_POSE_TRACE") || !g_red_animator || n++ % 60) return;
    klog("posetrace 2110b0 %u 210ad0 %u sampler216df0 %u sampler2166f0 %u\n",
         g_tr_cnt[0] - last[0], g_tr_cnt[1] - last[1], g_tr_cnt[2] - last[2], g_tr_cnt[3] - last[3]);
    memcpy(last, g_tr_cnt, sizeof last);
}

void redpose_install(void)
{
    if (getenv("XBOX_ADV_TRACE")) {
        hookn(0x217680, adv_stub0, 6); hookn(0x2176bb, adv_stub1, 7); hookn(0x2176ed, adv_stub2, 8); hookn(0x217820, adv_stub3, 6);
        hookn(0x217696, adv_stub4, 6); hookn(0x2176a2, adv_stub5, 5); hookn(0x2148e7, adv_stub6, 7); hookn(0x21491c, adv_stub7, 8);
    }
    if (getenv("XBOX_POSE_TRACE")) {
        hook6(0x2110b0, tr_stub0); hook6(0x210ad0, tr_stub1); hook6(0x216df0, tr_stub2); hook6(0x2166f0, tr_stub3);
    }
    u8 *e = (u8 *)(uintptr_t)0x1fce30u, *c = (u8 *)(uintptr_t)0x1fcf65u;
    if (e[0] == 0x83 && e[1] == 0xec && e[2] == 0x20 && e[3] == 0x53 && e[4] == 0x57) {
        u32 rel = (u32)(uintptr_t)pose_enter_stub - (0x1fce30u + 5);
        e[0] = 0xe9; memcpy(e + 1, &rel, 4);
    } else { klog("redpose: unexpected bytes at 0x1fce30\n"); return; }
    if (c[0] == 0xe8 && *(u32 *)(c + 1) == 0x2119f0u - (0x1fcf65u + 5)) {
        u32 rel = (u32)(uintptr_t)pose_out_stub - (0x1fcf65u + 5);
        memcpy(c + 1, &rel, 4);
    } else { klog("redpose: unexpected bytes at 0x1fcf65\n"); return; }
    g_pose_debug = getenv("XBOX_POSE_DEBUG") != NULL;
    u8 *pe = (u8 *)(uintptr_t)0x1fcf6eu;
    if (pe[0] == 0xe8 && *(u32 *)(pe + 1) == 0x2523c0u - (0x1fcf6eu + 5)) {
        u32 rel = (u32)(uintptr_t)pose_end_stub - (0x1fcf6eu + 5);
        memcpy(pe + 1, &rel, 4);
    } else klog("redpose: unexpected bytes at 0x1fcf6e\n");
    if (getenv("XBOX_POSE_BONE")) g_pose_bonetest = atoi(getenv("XBOX_POSE_BONE"));
    klog("redpose: pose layer installed\n");
}
