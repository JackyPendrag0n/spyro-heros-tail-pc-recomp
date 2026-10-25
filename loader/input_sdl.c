/* SDL2 game controller / keyboard -> Xbox gamepad state (port 0). */
#include "xbox.h"
#include <SDL2/SDL.h>
#include <string.h>
#include <math.h>
#include <time.h>

struct pad_state { u16 buttons; u8 analog[8]; s16 lx, ly, rx, ry; };
extern struct pad_state g_pads[4];
extern u32 g_pads_connected;

/* Xbox digital buttons */
#define XB_DPAD_UP 0x0001
#define XB_DPAD_DOWN 0x0002
#define XB_DPAD_LEFT 0x0004
#define XB_DPAD_RIGHT 0x0008
#define XB_START 0x0010
#define XB_BACK 0x0020
#define XB_LTHUMB 0x0040
#define XB_RTHUMB 0x0080
/* analog button indices */
enum { XA_A, XA_B, XA_X, XA_Y, XA_BLACK, XA_WHITE, XA_LTRIG, XA_RTRIG };

static SDL_GameController *g_gcs[4];       /* players 1..4's controllers (co-op: in the order connected) */
#define g_gc g_gcs[0]
static int gc_slot(SDL_Joystick *j) { for (int i = 0; i < 4; i++) if (g_gcs[i] && SDL_GameControllerGetJoystick(g_gcs[i]) == j) return i; return -1; }
static int gc_free(void) { for (int i = 0; i < 4; i++) if (!g_gcs[i]) return i; return -1; }

void input_sdl_init(void)
{
    SDL_GameControllerEventState(SDL_ENABLE);
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            int k = gc_free(); if (k < 0) break;
            g_gcs[k] = SDL_GameControllerOpen(i);
            if (g_gcs[k]) klog("input: controller %d '%s'%s\n", k + 1, SDL_GameControllerName(g_gcs[k]), k ? " (co-op player)" : "");
        }
    }
    if (!g_gc) klog("input: no controller, keyboard mapping active (WASD move, arrows camera, Space=A, LShift=B, E=X, Q=Y, Enter=Start, Esc=Back, Z/C=triggers)\n");
}

int debugmenu_open(void);
extern int g_freecam_enabled;
void debugmenu_toggle(void);
void debugmenu_nav(int k);

void input_sdl_event(SDL_Event *e)
{
    if (e->type == SDL_KEYDOWN && !e->key.repeat) {
        SDL_Keycode k = e->key.keysym.sym;
        if (k == SDLK_F1) { debugmenu_toggle(); return; }
        extern int g_noclip;
        if (k == SDLK_F2) { g_freecam_enabled = !g_freecam_enabled; if (g_freecam_enabled) g_noclip = 0; return; }
        if (k == SDLK_F3) { g_noclip = !g_noclip; if (g_noclip) g_freecam_enabled = 0; return; }
        if (debugmenu_open()) {
            switch (k) {
            case SDLK_UP: debugmenu_nav(1); break;
            case SDLK_DOWN: debugmenu_nav(2); break;
            case SDLK_LEFT: debugmenu_nav(3); break;
            case SDLK_RIGHT: debugmenu_nav(4); break;
            case SDLK_RETURN: case SDLK_SPACE: debugmenu_nav(5); break;
            case SDLK_ESCAPE: case SDLK_BACKSPACE: debugmenu_nav(6); break;
            }
            return;
        }
    }
    if (e->type == SDL_KEYDOWN && e->key.repeat && debugmenu_open()) {   /* held arrows scroll */
        SDL_Keycode k = e->key.keysym.sym;
        if (k == SDLK_UP) debugmenu_nav(1); else if (k == SDLK_DOWN) debugmenu_nav(2);
        return;
    }
    if (e->type == SDL_CONTROLLERDEVICEADDED && gc_free() >= 0) {
        SDL_Joystick *j = SDL_JoystickFromInstanceID(SDL_JoystickGetDeviceInstanceID(e->cdevice.which));
        if (j && gc_slot(j) >= 0) return;                 /* already open */
        SDL_GameController *c = SDL_GameControllerOpen(e->cdevice.which);
        int k = gc_free();
        if (c) { g_gcs[k] = c; klog("input: controller %d connected '%s'\n", k + 1, SDL_GameControllerName(c)); }
    } else if (e->type == SDL_CONTROLLERDEVICEREMOVED) {
        for (int k = 0; k < 4; k++) if (g_gcs[k] && e->cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_gcs[k]))) {
            SDL_GameControllerClose(g_gcs[k]);
            for (int i = k; i < 3; i++) g_gcs[i] = g_gcs[i + 1];   /* the next players' pads move up one */
            g_gcs[3] = NULL;
            klog("input: controller %d removed\n", k + 1);
            break;
        }
    }
}

static s16 axis_of(SDL_GameController *gc, SDL_GameControllerAxis a, int invert)
{
    int v = SDL_GameControllerGetAxis(gc, a);
    if (invert) v = -v - 1;
    if (v < -32768) v = -32768;
    return (s16)v;
}

/* XBOX_INPUT_SCRIPT="frame:button[:hold],...", e.g. "900:A,1200:START:10,1500:LY+:120".
 * Buttons: A B X Y START BACK UP DOWN LEFT RIGHT WHITE BLACK LT RT LX+ LX- LY+ LY- RX+ RX- RY+ RY-.
 * Used for automated testing; default hold is 6 frames. */
extern u32 gl_frame_number(void);
static void script_apply(struct pad_state *p, const char *var)
{
    extern u32 debugmenu_play_frame(void);          /* frames since gameplay began (0 = not yet) */
    if (var[strlen(var) - 1] != '2' && getenv("XBOX_AUTOSTART") && !debugmenu_play_frame()) {   /* tests: get through menus/intro */
        u32 f = gl_frame_number() % 240;               /* START, then A, then quiet (lets "not paused" be seen) */
        if (f < 6) p->buttons |= XB_START;
        else if (f >= 60 && f < 66) p->analog[XA_A] = 255;
    }
    const char *s = getenv(var);
    if (!s) return;
    u32 now = gl_frame_number(), rel = debugmenu_play_frame();
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", s);
    char *sv = NULL;
    for (char *tok = strtok_r(buf, ",", &sv); tok; tok = strtok_r(NULL, ",", &sv)) {
        char *c1 = strchr(tok, ':');
        if (!c1) continue;
        *c1++ = 0;
        int relative = tok[0] == '+';                 /* +N = N frames after gameplay began */
        u32 at = parse_u32(tok + relative, 10), hold = 6;
        if (relative) { if (!rel) continue; at += now - rel; }
        char *c2 = strchr(c1, ':');
        if (c2) { *c2++ = 0; hold = parse_u32(c2, 10); }
        if (now < at || now >= at + hold) continue;
        const char *b = c1;
        if (!strcmp(b, "A")) p->analog[XA_A] = 255;
        else if (!strcmp(b, "B")) p->analog[XA_B] = 255;
        else if (!strcmp(b, "X")) p->analog[XA_X] = 255;
        else if (!strcmp(b, "Y")) p->analog[XA_Y] = 255;
        else if (!strcmp(b, "WHITE")) p->analog[XA_WHITE] = 255;
        else if (!strcmp(b, "BLACK")) p->analog[XA_BLACK] = 255;
        else if (!strcmp(b, "LT")) p->analog[XA_LTRIG] = 255;
        else if (!strcmp(b, "RT")) p->analog[XA_RTRIG] = 255;
        else if (!strcmp(b, "START")) p->buttons |= XB_START;
        else if (!strcmp(b, "BACK")) p->buttons |= XB_BACK;
        else if (!strcmp(b, "L3")) p->buttons |= XB_LTHUMB;
        else if (!strcmp(b, "R3")) p->buttons |= XB_RTHUMB;
        else if (!strcmp(b, "L3R3")) p->buttons |= XB_LTHUMB | XB_RTHUMB;
        else if (!strcmp(b, "UP")) p->buttons |= XB_DPAD_UP;
        else if (!strcmp(b, "DOWN")) p->buttons |= XB_DPAD_DOWN;
        else if (!strcmp(b, "LEFT")) p->buttons |= XB_DPAD_LEFT;
        else if (!strcmp(b, "RIGHT")) p->buttons |= XB_DPAD_RIGHT;
        else if (!strcmp(b, "LX+")) p->lx = 32767;
        else if (!strcmp(b, "LXh+")) p->lx = 14000;                /* half stick: a walk */
        else if (!strcmp(b, "LXh-")) p->lx = -14000;
        else if (!strcmp(b, "LX-")) p->lx = -32768;
        else if (!strcmp(b, "LY+")) p->ly = 32767;
        else if (!strcmp(b, "LY-")) p->ly = -32768;
        else if (!strcmp(b, "RX+")) p->rx = 32767;
        else if (!strcmp(b, "RX-")) p->rx = -32768;
        else if (!strcmp(b, "RY+")) p->ry = 32767;
        else if (!strcmp(b, "RY-")) p->ry = -32768;
    }
}

/* controller: L3 + R3 together opens/closes the debug menu; while it is open the d-pad / left
 * stick navigate, A chooses, B goes back, and the game sees a released controller. */
static struct pad_state g_raw_pad;

/* free camera controls: left stick / WASD move, right stick / arrows look, triggers / Space-C
 * up-down, hold A / Shift for speed */
void freecam_read_controls(float *mx, float *my, float *mz, float *lx, float *ly, int *fast)
{
    const struct pad_state *p = &g_raw_pad;
    float dz = 0.15f;
#define AX(v) (fabsf((v) / 32767.0f) < dz ? 0.0f : (v) / 32767.0f)
    *mx = AX(p->lx); *mz = AX(p->ly); *lx = AX(p->rx); *ly = AX(p->ry);
#undef AX
    *my = (p->analog[XA_RTRIG] - p->analog[XA_LTRIG]) / 255.0f;
    *fast = p->analog[XA_A] > 128;
    const u8 *k = SDL_GetKeyboardState(NULL);
    if (k[SDL_SCANCODE_W]) *mz = 1; if (k[SDL_SCANCODE_S]) *mz = -1;
    if (k[SDL_SCANCODE_D]) *mx = 1; if (k[SDL_SCANCODE_A]) *mx = -1;
    if (k[SDL_SCANCODE_SPACE]) *my = 1; if (k[SDL_SCANCODE_C] || k[SDL_SCANCODE_LCTRL]) *my = -1;
    if (k[SDL_SCANCODE_RIGHT]) *lx = 1; if (k[SDL_SCANCODE_LEFT]) *lx = -1;
    if (k[SDL_SCANCODE_UP]) *ly = 1; if (k[SDL_SCANCODE_DOWN]) *ly = -1;
    if (k[SDL_SCANCODE_LSHIFT]) *fast = 1;
}

static void menu_pad(struct pad_state *p)
{
    g_raw_pad = *p;
    static u16 prev; static u8 prev_a, prev_b; static int prev_combo, hold_dir, hold_t;
    u16 b = p->buttons;
    int combo = (b & XB_LTHUMB) && (b & XB_RTHUMB);
    if (combo && !prev_combo) debugmenu_toggle();
    prev_combo = combo;
    if (debugmenu_open()) {
        int dir = 0;
        if ((b & XB_DPAD_UP) || p->ly > 20000) dir = 1;
        else if ((b & XB_DPAD_DOWN) || p->ly < -20000) dir = 2;
        else if ((b & XB_DPAD_LEFT) || p->lx < -20000) dir = 3;
        else if ((b & XB_DPAD_RIGHT) || p->lx > 20000) dir = 4;
        if (dir && dir != hold_dir) { debugmenu_nav(dir); hold_t = 0; }
        else if (dir && (dir == 1 || dir == 2) && ++hold_t > 20 && hold_t % 4 == 0) debugmenu_nav(dir);   /* auto-repeat */
        hold_dir = dir;
        u8 a = p->analog[XA_A] > 128, bb = p->analog[XA_B] > 128;
        if (a && !prev_a) debugmenu_nav(5);
        if (bb && !prev_b) debugmenu_nav(6);
        prev_a = a; prev_b = bb;
        memset(p, 0, sizeof *p);
    } else {
        prev_a = p->analog[XA_A] > 128; prev_b = p->analog[XA_B] > 128; hold_dir = 0;
        extern int g_noclip;
        if (g_freecam_enabled || g_noclip) {             /* the game stands still; pause still works */
            u16 keep = p->buttons & (XB_START | XB_BACK);
            memset(p, 0, sizeof *p); p->buttons = keep;
        }
    }
    prev = b; (void)prev;
}

static void read_controller(SDL_GameController *c, struct pad_state *pp)
{
    struct pad_state p;
    memset(&p, 0, sizeof p);
    {
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP)) p.buttons |= XB_DPAD_UP;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) p.buttons |= XB_DPAD_DOWN;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) p.buttons |= XB_DPAD_LEFT;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) p.buttons |= XB_DPAD_RIGHT;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START)) p.buttons |= XB_START;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK)) p.buttons |= XB_BACK;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSTICK)) p.buttons |= XB_LTHUMB;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) p.buttons |= XB_RTHUMB;
        p.analog[XA_A] = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A) ? 255 : 0;
        p.analog[XA_B] = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B) ? 255 : 0;
        p.analog[XA_X] = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X) ? 255 : 0;
        p.analog[XA_Y] = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y) ? 255 : 0;
        p.analog[XA_WHITE] = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? 255 : 0;
        p.analog[XA_BLACK] = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 255 : 0;
        p.analog[XA_LTRIG] = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7;
        p.analog[XA_RTRIG] = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7;
        /* SDL: +y is down; Xbox: +y is up */
        p.lx = axis_of(c, SDL_CONTROLLER_AXIS_LEFTX, 0);  p.ly = axis_of(c, SDL_CONTROLLER_AXIS_LEFTY, 1);
        p.rx = axis_of(c, SDL_CONTROLLER_AXIS_RIGHTX, 0); p.ry = axis_of(c, SDL_CONTROLLER_AXIS_RIGHTY, 1);
    }
    *pp = p;
}
static void read_keyboard(struct pad_state *pp)
{
    struct pad_state p;
    memset(&p, 0, sizeof p);
    {
        const u8 *k = SDL_GetKeyboardState(NULL);
        if (k[SDL_SCANCODE_W]) p.ly = 32767;
        if (k[SDL_SCANCODE_S]) p.ly = -32768;
        if (k[SDL_SCANCODE_A]) p.lx = -32768;
        if (k[SDL_SCANCODE_D]) p.lx = 32767;
        if (k[SDL_SCANCODE_UP]) p.ry = 32767;
        if (k[SDL_SCANCODE_DOWN]) p.ry = -32768;
        if (k[SDL_SCANCODE_LEFT]) p.rx = -32768;
        if (k[SDL_SCANCODE_RIGHT]) p.rx = 32767;
        if (k[SDL_SCANCODE_I]) p.buttons |= XB_DPAD_UP;
        if (k[SDL_SCANCODE_K]) p.buttons |= XB_DPAD_DOWN;
        if (k[SDL_SCANCODE_J]) p.buttons |= XB_DPAD_LEFT;
        if (k[SDL_SCANCODE_L]) p.buttons |= XB_DPAD_RIGHT;
        if (k[SDL_SCANCODE_RETURN]) p.buttons |= XB_START;
        if (k[SDL_SCANCODE_ESCAPE]) p.buttons |= XB_BACK;
        p.analog[XA_A] = k[SDL_SCANCODE_SPACE] ? 255 : 0;
        p.analog[XA_B] = k[SDL_SCANCODE_LSHIFT] ? 255 : 0;
        p.analog[XA_X] = k[SDL_SCANCODE_E] ? 255 : 0;
        p.analog[XA_Y] = k[SDL_SCANCODE_Q] ? 255 : 0;
        p.analog[XA_WHITE] = k[SDL_SCANCODE_1] ? 255 : 0;
        p.analog[XA_BLACK] = k[SDL_SCANCODE_2] ? 255 : 0;
        p.analog[XA_LTRIG] = k[SDL_SCANCODE_Z] ? 255 : 0;
        p.analog[XA_RTRIG] = k[SDL_SCANCODE_C] ? 255 : 0;
    }
    *pp = p;
}

/* playing as Red (either player: called with that player's Red state live, redctx.c) */
static void red_pad(struct pad_state *p)
{
    {   /* playing as Red: square (X) is his staff swing, not breath; the swing's hit is a
         * short charge tap (B) handed to the game */
        extern u32 g_redchar_anim; extern int g_red_x, g_red_melee_b;
        extern int g_red_hide_a, g_red_pass_x, g_raw_a_pub;
        g_raw_a_pub = p->analog[XA_A] > 128;
        if (g_redchar_anim) {
            g_red_x = p->analog[XA_X] > 128;
            if (!g_red_pass_x) p->analog[XA_X] = 0;          /* square held = his magic: the game's breath */
            if (g_red_hide_a) p->analog[XA_A] = 0;           /* flying: the game stays out of its glide */
            {   extern int g_raw_b_pub, g_red_hide_b;           /* ground pound / tail-spin: his own B */
                g_raw_b_pub = p->analog[XA_B] > 128; if (g_red_hide_b) p->analog[XA_B] = 0; }
            {   extern int g_raw_y_pub;                         /* triangle / Y: his shrink bolt (redshrink.c) */
                g_raw_y_pub = !debugmenu_open() && p->analog[XA_Y] > 128; p->analog[XA_Y] = 0; }
            {   /* left trigger / left shoulder: his aim (redcast.c); never Spyro's first-person view */
                extern int g_red_aim_in, g_red_aiming; extern float g_aim_rx, g_aim_ry, g_aim_lx, g_aim_ly;
                extern int g_red_flying, g_red_shield, g_raw_rt_pub;
                g_red_aim_in = !debugmenu_open() && (p->analog[XA_LTRIG] > 60 || p->analog[XA_WHITE] > 128);
                g_raw_rt_pub = !debugmenu_open() && p->analog[XA_RTRIG] > 60;        /* his wing shield */
                p->analog[XA_LTRIG] = 0; p->analog[XA_WHITE] = 0; p->analog[XA_RTRIG] = 0;
#define DZ(v) (fabsf((v) / 32767.0f) < 0.15f ? 0.0f : ((v) / 32767.0f - ((v) > 0 ? 0.15f : -0.15f)) / 0.85f)
                g_aim_lx = DZ(p->lx); g_aim_ly = DZ(p->ly);             /* the left stick as he reads it (aiming, flying) */
                if (g_red_aiming) {
                    g_aim_rx = DZ(p->rx); g_aim_ry = DZ(p->ry);
                    p->lx = p->ly = 0; p->rx = p->ry = 0;
                } else g_aim_rx = g_aim_ry = 0;
#undef DZ
                if (g_red_flying || g_red_shield) p->lx = p->ly = 0;    /* flying: he steers; shielding: he stands his ground */
            }
            {   /* the hit lasts a tenth of a second whatever the frame rate (g_red_melee_b > 0 starts it) */
                static RS double until; struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
                double now = ts.tv_sec + ts.tv_nsec * 1e-9;
                if (g_red_melee_b > 0) { until = now + 0.1; g_red_melee_b = 0; }
                if (now < until) p->analog[XA_B] = 255;
            }
        } else { extern int g_red_aiming, g_red_aim_in; g_red_x = 0; g_red_aiming = g_red_aim_in = 0; }
    }
}

/* co-op (coop.c): players 2..4 play on the second to fourth controllers - player 2 on the
 * keyboard while player 1 has the only controller. Each goes through Red's controls with their own
 * Red state, and to the port the game reads for them. */
int g_coop_back[4];
static void coop_pad(void)
{
    extern int g_coop, g_coop_players; u32 coop_player(int k); u8 coop_port_of(int k); int red_ctx_set(int p);
    static const char *scripts[4] = { NULL, "XBOX_INPUT_SCRIPT2", "XBOX_INPUT_SCRIPT3", "XBOX_INPUT_SCRIPT4" };
    u32 conn = 1u << (coop_port_of(0) & 3);
    for (int k = 1; k < 4; k++) {
        struct pad_state p;
        if (g_gcs[k]) read_controller(g_gcs[k], &p);
        else if (k == 1 && g_gc) read_keyboard(&p);
        else memset(&p, 0, sizeof p);
        script_apply(&p, scripts[k]);
        int on = g_coop && k < (g_coop_players < 2 ? 2 : g_coop_players);
        if (debugmenu_open() || !on || !coop_player(k)) memset(&p, 0, sizeof p);
        {   /* Back: they jump back to player 1 (coop.c) */
            static int prev[4]; int b = (p.buttons & XB_BACK) != 0;
            if (b && !prev[k]) g_coop_back[k] = 1;
            prev[k] = b; p.buttons &= ~XB_BACK;
        }
        int was = red_ctx_set(k); red_pad(&p); red_ctx_set(was);
        u8 port = coop_port_of(k) & 3;
        g_pads[port] = p;
        if (on) conn |= 1u << port;
    }
    g_pads_connected = conn;
}

void input_sdl_poll(void)
{
    struct pad_state p;
    if (g_gc) read_controller(g_gc, &p); else read_keyboard(&p);
    script_apply(&p, "XBOX_INPUT_SCRIPT");
    menu_pad(&p);
    red_pad(&p);
    {   /* modern camera (debugmenu.c): the right stick is read here; the game gets a right-stick x
         * that steers its own camera to the yaw the player chose, and no right-stick y */
        extern int g_modern_cam_active; extern float g_cam_rx, g_cam_ry, g_cam_rx_out;
        extern int g_red_aiming;
        if (g_red_aiming) { g_cam_rx = g_cam_ry = 0; if (g_modern_cam_active) p.rx = (s16)(g_cam_rx_out * 32767.0f); }
        else if (g_modern_cam_active && !debugmenu_open()) {
#define DZ(v) (fabsf((v) / 32767.0f) < 0.15f ? 0.0f : ((v) / 32767.0f - ((v) > 0 ? 0.15f : -0.15f)) / 0.85f)
            g_cam_rx = DZ(p.rx); g_cam_ry = DZ(p.ry);
#undef DZ
            p.rx = (s16)(g_cam_rx_out * 32767.0f); p.ry = 0;
        } else g_cam_rx = g_cam_ry = 0;
    }
    {   extern int g_freeze_player;                 /* flattened by Red: no control until you pop back */
        if (g_freeze_player) { u16 keep = p.buttons & (XB_START | XB_BACK); p.lx = p.ly = 0; memset(p.analog, 0, sizeof p.analog); p.buttons = keep; } }
    g_pads[0] = p;
    coop_pad();
}
