/* lights.h - turn signals (blinkers) and a headlight flash (included by nfsc.c, after mp.h).
 *
 * WHAT THE PLAYER GETS. Q / E (or the d-pad left / right) switch the left / right turn signal on
 * and off: amber lamps at the car's front and rear corners blink 1.5 times a second. A signal
 * switches itself off once the car has turned through a corner and straightened out again
 * (setting turn_signal_auto_off). Holding F (or holding the left stick pressed in, L3) flashes the
 * high beams: a much brighter, bigger, whiter glare at the headlights while held (at least
 * LT_FLASH_MIN_MS for a tap). Two presses within LT_DOUBLE_MS switch the headlights off (or on
 * again): no headlight glare, and the headlight lamps and glass in their unlit look; tail and
 * brake lights stay. A flash still works with the headlights off (flash-to-pass): the lamps light
 * and glare while it lasts. The choice stays for the session, through races and world loads. A
 * key the player has bound to something on Options > Controls is left alone. The d-pad left /
 * right keep their game use too (in a race they switch the leaderboard's columns; in free roam
 * nothing); L3 does nothing else while driving.
 *
 * HOW CARBON DRAWS CAR LIGHTS (v1.4, read in the listing). The game has light ids for turn signals
 * (LIGHT_LFSIGNAL ... in the light table at 0x00a7abc0: {bit, name hash, name, marker hash}), but
 * their marker hash is 0 and no car model has such markers, so it never draws them. A car's lamps
 * are "light flares": CarRenderInfo::CreateCarLightFlares (007cb9f0) walks the position markers of
 * the car's parts and, for LEFT_HEADLIGHT, RIGHT_BRAKELIGHT and the rest, links an eLightFlare
 * (0x30 bytes: +8 marker hash, +0x10 position, +0x1c reflection height, +0x20 direction, all in
 * car space: x forward, y left, z up; +0x2c type, +0x2d flags, 4 = brighter when facing the eye)
 * into the list at CarRenderInfo + 0x458. Every frame CarRenderInfo::RenderFlaresOnCar (007cbc40,
 * thiscall: view, car position, car matrix, 0, reflection pass, only-these, not-the-player;
 * RET 0x1c; called per car by 007ccad0) works out each lamp's intensity from the light bits at
 * +0x30c0 (on) and +0x30c4 (broken) and hands each flare to eRenderLightFlare (0074d330, cdecl, 10
 * arguments: view, flare, car-to-world matrix, intensity, reflection, pass kind, 1.0, road height,
 * 0, size). That one looks up the flare's TYPE in the table at 0x00a6bf88 (two pointers per type,
 * 0x20 types) to a parameter block {size at intensity 0 and 1, red, green, blue, alpha (0..127.5,
 * doubled), facing power, size scale, size clamp low / high, texture}, moves the flare along its
 * direction and queues a glowing quad. Type 1 is the tail/brake light, 0 the headlight (two
 * blocks: glow and halo).
 *
 * WHAT THIS DOES. A BEFORE hook on 007cbc40 draws extra flares for the car being drawn, with the
 * game's own eRenderLightFlare and the arguments the game would pass (so they are reflected on a
 * wet road the same way): the turn signals at the outer corners of the headlights and tail lights
 * (or of the car's box when a model lacks them), facing straight ahead / back, as flares of type 1
 * whose parameter blocks are swapped for this file's amber ones only for the length of the call;
 * and for a flash, each headlight flare again with the "high beam" blocks in type 0's place. The
 * player's car is the one RenderFlaresOnCar is told is the player's (its last argument, 0);
 * another player's car (mp.h) is the remote car whose position matches. Passes that draw only
 * some lamps (light trails, cop lights) get nothing extra.
 *
 * HEADLIGHTS OFF. The game has its own switch for that, a debug one: the dword at 0x00b74c18
 * ("headlights off"; 0x00b74c14 is "on"), read only by RenderFlaresOnCar (headlight intensity 0)
 * and by CarRenderInfo::UpdateLightTextures (007adc90, thiscall, called once a frame per car from
 * 007de460: it picks each headlight's lamp and glass texture from the unlit / lit pairs at
 * CarRenderInfo + 0x3080 / + 0x308c - the *_OFF look - and writes them into the car's texture
 * replacement table). It is set for the length of those two calls for a car whose headlights are
 * off, and put back after (AFTER hooks); during RenderFlaresOnCar the car's headlight "on" bits
 * (+0x30c0 & 7) are cleared too, so the headlight-into-the-camera glare (007be170, only with both
 * on) stays away. Which car is whose is worked out once a frame from the render list (0x00b77db4
 * begin, 0x00b77dbc count; the player's has [[conn + 0x3c] + 0x478] == 0, as 007ccad0 tells
 * RenderFlaresOnCar; another player's is matched by position, conn + 0x30 -> its matrix).
 *
 * NO BEAM ON THE ROAD. The world shaders have a headlight term fed from the headlight view (eView
 * 9, 0x00b48b70: +0x10 direction, +0x20 the car's down, +0x40 position; uploaded by 00713ca0 each
 * frame, 29 effects, in 007311b0), and the sm1.1 ones a projected texture (cmHeadlight1WVP,
 * headlight_colour_scale). Tested in the browser (2026-10-26): raising that light 0.9-6 m, moving
 * it 12 m ahead or 5 km away during the upload, multiplying headlight_colour_scale by 8 and the
 * headlight camera's far plane (camera at [0xb4b390 + 0x40] + 0xd8) by 4 changed no pixel of the
 * road or the walls ahead: the world does not show the car's headlights here, so a flash cannot
 * light it further. (World meshes appear to carry no normals for the analytic term to use.)
 *
 * ONLINE. The page and this file share a block of guest memory (LtShared), announced as
 * globalThis.carbonLightsBlock ({memory, ptr}) with a "carbon-lights" event: the game writes this
 * player's lights (LT_* bits, 8 = headlights off; `seq` + 1 on each change) for the page to send;
 * the page writes each
 * mp.h slot's player's lights into remote[slot]. A page that knows nothing of it leaves
 * remote[] at 0, and nothing changes.
 */
#include <stdatomic.h>

#define LT_RENDER_FLARE 0x0074d330u /* cdecl eRenderLightFlare, 10 arguments */
#define LT_CAR_FLARES 0x007cbc40u   /* thiscall CarRenderInfo::RenderFlaresOnCar, RET 0x1c */
#define LT_FLARE_TYPES 0x00a6bf88u  /* two parameter-block pointers per flare type */
#define LT_LIGHT_TEXTURES 0x007adc90u /* thiscall CarRenderInfo::UpdateLightTextures */
#define LT_HEADLIGHTS_OFF 0x00b74c18u /* the game's "headlights off" switch (a dword) */
#define LT_RENDER_LIST 0x00b77db4u    /* render connections: begin; count at + 8 */
#define LT_HASH_LHEAD 0x9db90133u   /* bStringHash("LEFT_HEADLIGHT") */
#define LT_HASH_RHEAD 0xd09091c6u
#define LT_HASH_CHEAD 0x7a5bcf69u
#define LT_HASH_LBRAKE 0x31a66786u  /* LEFT_BRAKELIGHT */
#define LT_HASH_RBRAKE 0xbf700a79u

#define LT_LEFT 1u  /* left turn signal on */
#define LT_RIGHT 2u /* right turn signal on */
#define LT_FLASH 4u /* high beams flashed */
#define LT_OFF 8u   /* headlights switched off */
#define LT_ALL 15u
#define LT_MAGIC 0x3154474cu /* "LGT1" */

#define LT_PERIOD_MS 667.0    /* 1.5 blinks a second */
#define LT_ON_MS 380.0
#define LT_FLASH_MIN_MS 200.0 /* a tap still shows */
#define LT_DOUBLE_MS 300.0    /* a second press this soon after the first: headlights on / off */
#define LT_TURNED 0.60f       /* rad (~35 deg): a corner taken ... */
#define LT_STRAIGHT 0.12f     /* rad/s: ... and the car going straight again: signal off */

/* Lamp geometry and strength; in the block, so a test can try values on a running game. */
enum {
    LT_T_SIDE,       /* m: a front signal sits this far outside the headlight's centre */
    LT_T_BACK,       /* m: and this far behind it */
    LT_T_REAR_SIDE,  /* m: a rear signal this far outside the tail light */
    LT_T_REAR_FWD,   /* m: and this far in front of it */
    LT_T_INTENSITY,  /* turn signal intensity (the facing power shapes it; capped at 1) */
    LT_T_SIZE,       /* turn signal size scale */
    LT_T_FLASH_I,    /* high beam glare intensity */
    LT_T_FLASH_SIZE, /* high beam glare size scale */
    LT_T_COUNT
};
static const float k_lt_tune[LT_T_COUNT] = {0.06f, 0.06f, 0.04f, 0.02f, 3.0f, 1.0f, 1.0f, 1.0f};

typedef struct LtShared { /* 0x58 bytes */
    uint32_t magic, version;   /* LT_MAGIC, 1 */
    uint32_t mine;             /* +0x08 the game: this player's lights (LT_*) */
    uint32_t seq;              /* +0x0c the game: + 1 each time `mine` changes */
    uint32_t remote[MP_SLOTS]; /* +0x10 the page: the lights of each mp.h slot's player (LT_*) */
    float tune[LT_T_COUNT];    /* +0x30 the lamp values in use (tests may change them) */
    uint32_t params;           /* +0x50 guest address of the four parameter blocks (tests) */
    uint32_t draws;            /* +0x54 flares this file has drawn (tests) */
} LtShared;
_Static_assert(offsetof(LtShared, remote) == 0x10 && sizeof(LtShared) == 0x58, "LtShared layout");

/* Parameter blocks, as the game's (0x2c bytes): size at intensity 0 and 1, red, green, blue,
 * alpha, facing power, size scale, size clamp low / high, texture index. Tuned on screenshots
 * against the game's own tail lights (type 1) and headlights (type 0). */
typedef struct LtParams {
    float lo, hi, r, g, b, a, power, scale, clamp_lo, clamp_hi;
    uint32_t texture;
} LtParams;
static const LtParams k_lt_params[4] = {
    {20.0f, 24.0f, 128.0f, 66.0f, 4.0f, 130.0f, 0.4f, 0.22f, 0.012f, 0.018f, 2}, /* amber core */
    {30.0f, 30.0f, 110.0f, 48.0f, 0.0f, 14.0f, 1.0f, 0.8f, 0.01f, 0.014f, 1},    /* amber halo */
    {0.0f, 56.0f, 120.0f, 126.0f, 140.0f, 64.0f, 1.2f, 0.8f, 0.012f, 0.04f, 0},  /* high beam glow */
    {40.0f, 40.0f, 105.0f, 115.0f, 150.0f, 20.0f, 2.5f, 0.8f, 0.012f, 0.012f, 1}, /* high beam halo */
};

static uint32_t g_lt_mem;   /* guest: 4 x LtParams, a flare (0x30), a matrix (0x40) */
static uint32_t g_lt_block; /* guest address of the LtShared */
static uint32_t g_lt_hooks[6];
static atomic_uint g_lt_keys_down;    /* bit per key (LT_KEY_*), from the key callback */
static atomic_uint g_lt_keys_pressed; /* presses the frame has not taken yet */
#define LT_KEY_Q 1u
#define LT_KEY_E 2u
#define LT_KEY_F 4u
static uint32_t g_lt_state;    /* this player's lights */
static double g_lt_since;      /* when the current turn signal went on (its blink phase) */
static double g_lt_flash_at;   /* when the last flash started */
static uint32_t g_lt_pad_prev; /* d-pad left 1, right 2, L3 4 at the last frame */
static int g_lt_off;           /* headlights switched off (kept for the session) */
typedef struct LtTap {         /* the flash button of one device: F or L3 */
    double press_at;           /* its last press that could start a double press */
    int toggled;               /* the press held now switched the headlights: no flash */
} LtTap;
static LtTap g_lt_tap[2];
typedef struct LtCarLights {   /* a car with lights to draw this frame */
    uint32_t ri, state;
    double since;
} LtCarLights;
static LtCarLights g_lt_cars[MP_SLOTS + 1];
static int g_lt_ncars;
static uint32_t g_lt_saved_ri, g_lt_saved_on, g_lt_saved_switch; /* put back after the game's call */
static int g_lt_saved_flares, g_lt_saved_textures;
static float g_lt_yaw, g_lt_turned, g_lt_peak, g_lt_rate;
static int g_lt_yaw_ok;
static double g_lt_yaw_at;

static LtShared *lt_shared(void) {
    void *p = NULL;
    return g_lt_block && g_api->guest_ptr(g_api, g_lt_block, sizeof(LtShared), &p) == POP_OK ? p : NULL;
}
static float lt_tune(int i) {
    LtShared *sh = lt_shared();
    const float v = sh ? sh->tune[i] : k_lt_tune[i];
    return isfinite(v) ? v : k_lt_tune[i];
}
static uint32_t lt_params(int i) {
    return g_lt_mem + (uint32_t)i * sizeof(LtParams);
}
#define LT_FLARE (g_lt_mem + 4u * sizeof(LtParams))
#define LT_MATRIX (LT_FLARE + 0x30u)

/* Whether a turn signal on since `since` (ms) is lit at `now`. */
static int lt_lit(uint32_t state, double since, double now) {
    if (!(state & (LT_LEFT | LT_RIGHT)))
        return 0;
    return fmod(now - since + LT_PERIOD_MS * 64.0, LT_PERIOD_MS) < LT_ON_MS;
}

/* ---- input ----------------------------------------------------------------------------------- */

static int32_t lt_key(const PopModApi *api, int32_t dik, int32_t vk, int32_t down, void *user) {
    (void)api;
    (void)vk;
    (void)user;
    const unsigned bit = dik == 0x10 ? LT_KEY_Q : dik == 0x12 ? LT_KEY_E : dik == 0x21 ? LT_KEY_F : 0u;
    if (!bit)
        return 0;
    if (down) {
        if (!(atomic_fetch_or(&g_lt_keys_down, bit) & bit)) /* not a key repeat */
            atomic_fetch_or(&g_lt_keys_pressed, bit);
    } else {
        atomic_fetch_and(&g_lt_keys_down, ~bit);
    }
    return 0; /* never taken from the game: a binding the player made keeps working */
}

/* The game's controller (port 0, read as fe_pad.h reads it): d-pad left 1, right 2, L3 4. */
static uint32_t lt_pad(const PopModApi *api) {
    uint32_t device = get_u32(PAD_DEVICES), state = 0;
    if (!device)
        return 0;
    const uint32_t getter = get_u32(get_u32(device) + 4);
    if (!getter || api->guest_call(api, getter, device, NULL, 0, &state) != POP_OK || !state)
        return 0;
    const uint32_t pov = get_u32(state + 0x220); /* hundredths of a degree, 0 = up */
    uint32_t held = 0;
    if (pov >= 22500u && pov <= 31500u)
        held |= 1u;
    else if (pov >= 4500u && pov <= 13500u)
        held |= 2u;
    if (get_u32(state + 0xc + 10u * 4u)) /* button 10: left stick click */
        held |= 4u;
    return held;
}

/* A letter bound in the game's keyboard bindings (mp.h fills the word each frame). */
static int lt_bound(char letter) {
    MpShared *mp = mp_shared();
    const uint32_t keys = mp ? __atomic_load_n(&mp->keys, __ATOMIC_RELAXED) : 0;
    return (keys & MP_KEYS_KNOWN) && (keys & (1u << (letter - 'A')));
}

static uint32_t lt_remote(const LtShared *sh, const float p[3]);

/* ---- the player's lights, once a frame ------------------------------------------------------- */

static void lt_set(uint32_t state) {
    if (state == g_lt_state)
        return;
    g_lt_state = state;
    LtShared *sh = lt_shared();
    if (sh) {
        __atomic_store_n(&sh->mine, state, __ATOMIC_RELAXED);
        __atomic_fetch_add(&sh->seq, 1u, __ATOMIC_RELEASE);
    }
}

/* A press: that side on (the other off), or off when it was on. */
static void lt_signal(uint32_t side, double now) {
    uint32_t s = g_lt_state & ~(LT_LEFT | LT_RIGHT);
    if (!(g_lt_state & side)) {
        s |= side;
        g_lt_since = now;
        g_lt_turned = g_lt_peak = g_lt_rate = 0.0f;
        g_lt_yaw_ok = 0;
    }
    lt_set(s);
}

/* How far the car has turned towards the lit side since the signal went on, and how fast it
 * turns that way now (smoothed). Heading from the forward row of the player's rigid body
 * (physics space, y up; render space is (z, -x, y) of it), left turns counting up. */
static void lt_heading(uint32_t player, double now) {
    const uint32_t state = player ? mp_state(get_u32(player + 0x70)) : 0;
    if (!state) {
        g_lt_yaw_ok = 0;
        return;
    }
    const float fx = bits_float(get_u32(state + 0x90)), fz = bits_float(get_u32(state + 0x98));
    const float yaw = atan2f(-fx, fz);
    if (!isfinite(yaw)) {
        g_lt_yaw_ok = 0;
        return;
    }
    if (g_lt_yaw_ok) {
        float d = yaw - g_lt_yaw;
        if (d > 3.14159265f)
            d -= 6.2831853f;
        else if (d < -3.14159265f)
            d += 6.2831853f;
        const double dt = (now - g_lt_yaw_at) / 1000.0;
        if (dt > 0.0005 && dt < 0.5) {
            const float side = (g_lt_state & LT_LEFT) ? 1.0f : -1.0f;
            g_lt_turned += d * side;
            if (g_lt_turned < -0.5f)
                g_lt_turned = -0.5f; /* swinging out first does not count against the turn */
            if (g_lt_turned > g_lt_peak)
                g_lt_peak = g_lt_turned;
            g_lt_rate += (float)((d * side / dt - g_lt_rate) * (dt / (dt + 0.15)));
        }
    }
    g_lt_yaw = yaw;
    g_lt_yaw_at = now;
    g_lt_yaw_ok = 1;
}

/* One device's flash button this frame: a press flashes at once (for at least LT_FLASH_MIN_MS);
 * a second press within LT_DOUBLE_MS of the first switches the headlights instead and gives no
 * flash. Returns whether it holds a flash now. */
static int lt_flash_button(LtTap *t, int pressed, int down, double now) {
    if (pressed) {
        if (now - t->press_at <= LT_DOUBLE_MS) {
            g_lt_off = !g_lt_off;
            t->toggled = 1;
            t->press_at = -1e9;
            g_lt_flash_at = -1e9; /* the first press's flash ends now */
        } else {
            t->toggled = 0;
            t->press_at = now;
            g_lt_flash_at = now;
        }
    }
    if (!down)
        t->toggled = 0;
    return down && !t->toggled;
}

/* The cars to draw lights for this frame: the player's, and other players' with lights on. */
static void lt_scan(const LtShared *sh, uint32_t mine) {
    g_lt_ncars = 0;
    uint32_t any = 0;
    for (int i = 0; sh && i < MP_SLOTS; ++i)
        any |= __atomic_load_n(&sh->remote[i], __ATOMIC_RELAXED);
    if (!mine && !any)
        return;
    const uint32_t list = get_u32(LT_RENDER_LIST), count = get_u32(LT_RENDER_LIST + 8);
    for (uint32_t i = 0; list && i < count && i < 96 && g_lt_ncars < MP_SLOTS + 1; ++i) {
        const uint32_t conn = get_u32(list + 4u * i), ri = conn ? get_u32(conn + 0x40) : 0;
        if (!ri)
            continue;
        const uint32_t who = get_u32(conn + 0x3c);
        if (who && get_u32(who + 0x478) == 0) { /* the player's car */
            if (mine)
                g_lt_cars[g_lt_ncars++] = (LtCarLights){ri, mine, g_lt_since};
            continue;
        }
        const uint32_t m = any ? get_u32(conn + 0x30) : 0;
        if (!m)
            continue;
        float p[3];
        for (int k = 0; k < 3; ++k)
            p[k] = bits_float(get_u32(m + 0x30 + 4u * k));
        const uint32_t state = lt_remote(sh, p);
        if (state)
            g_lt_cars[g_lt_ncars++] = (LtCarLights){ri, state, 0.0};
    }
}

static const LtCarLights *lt_car(uint32_t ri) {
    for (int i = 0; i < g_lt_ncars; ++i)
        if (g_lt_cars[i].ri == ri)
            return &g_lt_cars[i];
    return NULL;
}

static void lt_service(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu;
    (void)inv;
    (void)user;
    const double now = mp_now();
    uint32_t player = mp_player();
    if (player && mp_online_game())
        player = 0;
    const unsigned pressed = atomic_exchange(&g_lt_keys_pressed, 0u), down = atomic_load(&g_lt_keys_down);
    const uint32_t pad = lt_pad(api), pad_new = pad & ~g_lt_pad_prev;
    g_lt_pad_prev = pad;
    if (!player) { /* menus, loading, a race the game drives: nothing to draw (the on / off stays) */
        lt_set(0);
        g_lt_yaw_ok = 0;
        g_lt_ncars = 0;
        return;
    }
    if (((pressed & LT_KEY_Q) && !lt_bound('Q')) || (pad_new & 1u))
        lt_signal(LT_LEFT, now);
    if (((pressed & LT_KEY_E) && !lt_bound('E')) || (pad_new & 2u))
        lt_signal(LT_RIGHT, now);
    const int key_f = !lt_bound('F');
    int flash = lt_flash_button(&g_lt_tap[0], key_f && (pressed & LT_KEY_F), key_f && (down & LT_KEY_F), now);
    flash |= lt_flash_button(&g_lt_tap[1], (pad_new & 4u) != 0, (pad & 4u) != 0, now);
    flash |= now - g_lt_flash_at < LT_FLASH_MIN_MS;
    lt_set((g_lt_state & (LT_LEFT | LT_RIGHT)) | (flash ? LT_FLASH : 0u) | (g_lt_off ? LT_OFF : 0u));
    if (g_lt_state & (LT_LEFT | LT_RIGHT)) {
        lt_heading(player, now);
        if (setting("turn_signal_auto_off", 1) && g_lt_peak > LT_TURNED && g_lt_turned > LT_TURNED * 0.5f &&
            g_lt_rate < LT_STRAIGHT)
            lt_set(g_lt_state & ~(LT_LEFT | LT_RIGHT));
    }
    lt_scan(lt_shared(), g_lt_state);
}

/* ---- drawing ----------------------------------------------------------------------------------- */

typedef struct LtCar {
    uint32_t ri, view, refl;
    uint32_t road; /* the reflection pass's road height argument, as the game passes it */
} LtCar;

/* One flare, with parameter blocks a and b (b < 0: none) in `type`'s place for the call. */
static void lt_draw(const LtCar *car, const float pos[3], const float dir[3], uint32_t type, uint32_t flags,
                    int a, int b, float intensity, float size) {
    const uint32_t f = LT_FLARE;
    put_float(f + 0x10, pos[0]);
    put_float(f + 0x14, pos[1]);
    put_float(f + 0x18, pos[2]);
    put_float(f + 0x1c, 0.0f);
    put_float(f + 0x20, dir[0]);
    put_float(f + 0x24, dir[1]);
    put_float(f + 0x28, dir[2]);
    g_api->guest_write_u8(g_api, f + 0x2c, (uint8_t)type);
    g_api->guest_write_u8(g_api, f + 0x2d, (uint8_t)flags);
    const uint32_t slot = LT_FLARE_TYPES + type * 8u;
    const uint32_t keep0 = get_u32(slot), keep1 = get_u32(slot + 4);
    g_api->guest_write_u32(g_api, slot, lt_params(a));
    g_api->guest_write_u32(g_api, slot + 4, b >= 0 ? lt_params(b) : 0u);
    const uint32_t args[10] = {car->view, f, LT_MATRIX, float_bits(intensity), car->refl ? 1u : 0u,
                               car->refl ? 2u : 0u, float_bits(1.0f), car->refl ? car->road : 0u,
                               0u, float_bits(size)};
    uint32_t eax = 0;
    g_api->guest_call(g_api, LT_RENDER_FLARE, 0, args, 10, &eax);
    g_api->guest_write_u32(g_api, slot, keep0);
    g_api->guest_write_u32(g_api, slot + 4, keep1);
    LtShared *sh = lt_shared();
    if (sh)
        ++sh->draws;
}

/* Where a car's left turn signals go (car space; the right ones mirror y): beside its outermost
 * headlight and tail light flares, else at the corners of its box. */
static void lt_corners(uint32_t ri, float front[3], float rear[3]) {
    int have_f = 0, have_r = 0;
    const uint32_t head = ri + 0x458;
    uint32_t node = get_u32(head);
    for (int n = 0; node && node != head && n < 40; ++n, node = get_u32(node)) {
        const uint32_t hash = get_u32(node + 8);
        const int which = hash == LT_HASH_LHEAD || hash == LT_HASH_RHEAD     ? 1
                          : hash == LT_HASH_LBRAKE || hash == LT_HASH_RBRAKE ? 2
                                                                             : 0;
        if (!which)
            continue;
        const float x = bits_float(get_u32(node + 0x10)), y = fabsf(bits_float(get_u32(node + 0x14))),
                    z = bits_float(get_u32(node + 0x18));
        if (!isfinite(x) || !isfinite(y) || !isfinite(z) || y > 1.5f)
            continue;
        float *c = which == 1 ? front : rear;
        int *have = which == 1 ? &have_f : &have_r;
        if (!*have || y > c[1]) {
            c[0] = x;
            c[1] = y;
            c[2] = z;
            *have = 1;
        }
    }
    float lo[3], hi[3];
    for (int k = 0; k < 3; ++k) {
        lo[k] = bits_float(get_u32(ri + 0x370 + 4u * k));
        hi[k] = bits_float(get_u32(ri + 0x380 + 4u * k));
    }
    const int box = isfinite(lo[0]) && isfinite(hi[0]) && isfinite(lo[2]) && isfinite(hi[2]) &&
                    hi[0] - lo[0] > 2.0f && hi[0] - lo[0] < 10.0f && hi[1] - lo[1] > 1.0f && hi[1] - lo[1] < 4.0f;
    const float half = box ? (hi[1] - lo[1]) * 0.5f - 0.2f : 0.65f;
    if (have_f) {
        front[0] -= lt_tune(LT_T_BACK);
        front[1] += lt_tune(LT_T_SIDE);
    } else {
        front[0] = box ? hi[0] - 0.15f : 2.0f;
        front[1] = half;
        front[2] = box ? lo[2] + (hi[2] - lo[2]) * 0.4f : 0.5f;
    }
    if (have_r) {
        rear[0] += lt_tune(LT_T_REAR_FWD);
        rear[1] += lt_tune(LT_T_REAR_SIDE);
    } else {
        rear[0] = box ? lo[0] + 0.15f : -2.0f;
        rear[1] = half;
        rear[2] = box ? lo[2] + (hi[2] - lo[2]) * 0.5f : 0.6f;
    }
}

static void lt_draw_car(const LtCar *car, uint32_t state, double since, double now) {
    const uint32_t ri = car->ri, broken = get_u32(ri + 0x30c4);
    if (lt_lit(state, since, now)) {
        float front[3], rear[3];
        lt_corners(ri, front, rear);
        /* straight ahead / back: eRenderLightFlare moves a flare along its direction, so a
         * sideways one would sit beside the car */
        const float s = (state & LT_LEFT) ? 1.0f : -1.0f;
        const float fp[3] = {front[0], front[1] * s, front[2]}, rp[3] = {rear[0], rear[1] * s, rear[2]};
        const float fd[3] = {1.0f, 0.0f, 0.0f}, rd[3] = {-1.0f, 0.0f, 0.0f};
        const float i = lt_tune(LT_T_INTENSITY), size = lt_tune(LT_T_SIZE);
        if (!(broken & (s > 0 ? 1u : 2u))) /* a smashed headlight takes its signal with it */
            lt_draw(car, fp, fd, 1u, 4u, 0, 1, i, size);
        if (!(broken & (s > 0 ? 8u : 0x10u))) /* and a smashed tail light */
            lt_draw(car, rp, rd, 1u, 4u, 0, 1, i, size);
    }
    if (state & LT_FLASH) {
        const uint32_t head = ri + 0x458;
        uint32_t node = get_u32(head);
        for (int n = 0; node && node != head && n < 40; ++n, node = get_u32(node)) {
            const uint32_t hash = get_u32(node + 8);
            const uint32_t bit = hash == LT_HASH_LHEAD ? 1u : hash == LT_HASH_RHEAD ? 2u : hash == LT_HASH_CHEAD ? 4u : 0u;
            if (!bit || (broken & bit))
                continue;
            float p[3], d[3];
            for (int k = 0; k < 3; ++k) {
                p[k] = bits_float(get_u32(node + 0x10 + 4u * k));
                d[k] = bits_float(get_u32(node + 0x20 + 4u * k));
            }
            lt_draw(car, p, d, 0u, (get_u32(node + 0x2c) >> 8) & 0xffu, 2, 3, lt_tune(LT_T_FLASH_I),
                    lt_tune(LT_T_FLASH_SIZE));
        }
    }
}

/* The lights of the other player's car drawn at render-space position p, or 0. */
static uint32_t lt_remote(const LtShared *sh, const float p[3]) {
    for (int i = 0; i < MP_SLOTS; ++i) {
        const uint32_t state = __atomic_load_n(&sh->remote[i], __ATOMIC_RELAXED) & LT_ALL;
        if (!state || !g_mp_remote[i].simable)
            continue;
        const uint32_t st = mp_state(get_u32(g_mp_remote[i].simable - 0x24 + 0x70));
        if (!st)
            continue;
        const float sx = bits_float(get_u32(st + 0x10)), sy = bits_float(get_u32(st + 0x14)),
                    sz = bits_float(get_u32(st + 0x18));
        const float dx = p[0] - sz, dy = p[1] + sx, dz = p[2] - sy; /* render = (z, -x, y) */
        if (dx * dx + dy * dy + dz * dz < 9.0f)
            return state;
    }
    return 0;
}

/* Puts back what the last of the game's calls below changed (AFTER hooks). */
static void lt_put_back(void) {
    if (g_lt_saved_flares)
        g_api->guest_write_u32(g_api, g_lt_saved_ri + 0x30c0, g_lt_saved_on);
    if (g_lt_saved_flares || g_lt_saved_textures)
        g_api->guest_write_u32(g_api, LT_HEADLIGHTS_OFF, g_lt_saved_switch);
    g_lt_saved_flares = g_lt_saved_textures = 0;
}

/* The game's "headlights off" switch on for one of its calls about car ri. */
static void lt_switch_off(uint32_t ri, int flares) {
    lt_put_back();
    g_lt_saved_ri = ri;
    g_lt_saved_switch = get_u32(LT_HEADLIGHTS_OFF);
    g_api->guest_write_u32(g_api, LT_HEADLIGHTS_OFF, 1);
    if (flares) {
        g_lt_saved_on = get_u32(ri + 0x30c0);
        g_api->guest_write_u32(g_api, ri + 0x30c0, g_lt_saved_on & ~7u);
        g_lt_saved_flares = 1;
    } else {
        g_lt_saved_textures = 1;
    }
}

/* Headlights shown off: switched off and not flashing. */
static int lt_dark(uint32_t state) {
    return (state & (LT_OFF | LT_FLASH)) == LT_OFF;
}

static void lt_car_flares(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t esp = cpu->esp;
    if (get_u32(esp + 0x18) & 3u) /* the cop-lights-only and light-trail passes */
        return;
    const LtCarLights *c = g_lt_ncars ? lt_car(cpu->ecx) : NULL;
    if (!c)
        return;
    const uint32_t state = c->state;
    if (lt_dark(state))
        lt_switch_off(cpu->ecx, 1); /* the game's own headlight flares and glare: off */
    const double now = mp_now();
    if (!(state & LT_FLASH) && !lt_lit(state, c->since, now))
        return;
    const uint32_t pos = get_u32(esp + 8), mat = get_u32(esp + 0xc);
    if (!pos || !mat)
        return;
    /* the car-to-world matrix the game builds: the car's rotation, its position, w 1 */
    for (uint32_t k = 0; k < 12; ++k)
        api->guest_write_u32(api, LT_MATRIX + 4u * k, get_u32(mat + 4u * k));
    for (uint32_t k = 0; k < 3; ++k)
        api->guest_write_u32(api, LT_MATRIX + 0x30 + 4u * k, get_u32(pos + 4u * k));
    put_float(LT_MATRIX + 0x3c, 1.0f);
    const LtCar car = {cpu->ecx, get_u32(esp + 4), get_u32(esp + 0x14), get_u32(cpu->ecx + 0x13d4)};
    lt_draw_car(&car, state, c->since, now);
}

static void lt_after(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)cpu;
    (void)inv;
    (void)user;
    lt_put_back();
}

/* UpdateLightTextures for car ecx: the unlit headlight lamps and glass when it is dark. */
static void lt_textures(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)inv;
    (void)user;
    const LtCarLights *c = g_lt_ncars ? lt_car(cpu->ecx) : NULL;
    if (c && lt_dark(c->state))
        lt_switch_off(cpu->ecx, 0);
}

/* ---- install --------------------------------------------------------------------------------- */

static void lights_install(void) {
    if (!g_mp_block || !g_api->guest_call ||
        g_api->guest_alloc(g_api, 4u * sizeof(LtParams) + 0x30u + 0x40u, &g_lt_mem) != POP_OK ||
        g_api->guest_alloc(g_api, sizeof(LtShared), &g_lt_block) != POP_OK) {
        log_line("core.nfsc: turn signals unavailable (no guest memory or calls)");
        return;
    }
    for (int i = 0; i < 4; ++i) {
        const LtParams *p = &k_lt_params[i];
        const float f[10] = {p->lo, p->hi, p->r, p->g, p->b, p->a, p->power, p->scale, p->clamp_lo, p->clamp_hi};
        for (uint32_t k = 0; k < 10; ++k)
            put_float(lt_params(i) + 4u * k, f[k]);
        g_api->guest_write_u32(g_api, lt_params(i) + 0x28, p->texture);
    }
    for (uint32_t k = 0; k < 0x30u; k += 4)
        g_api->guest_write_u32(g_api, LT_FLARE + k, 0);
    LtShared *sh = lt_shared();
    if (!sh) {
        g_lt_block = 0;
        return;
    }
    memset(sh, 0, sizeof *sh);
    sh->magic = LT_MAGIC;
    sh->version = 1;
    memcpy(sh->tune, k_lt_tune, sizeof sh->tune);
    sh->params = g_lt_mem;
    const uint32_t quiet = POP_HOOK_NO_GAME_VIEW;
#define LT_HOOK(addr, fn, mode, slot)                                                                   \
    (g_api->hook_install_ex ? g_api->hook_install_ex(g_api, addr, 0, fn, mode, quiet, NULL, &g_lt_hooks[slot]) \
                            : g_api->hook_install(g_api, addr, fn, mode, NULL, &g_lt_hooks[slot]))
    if (LT_HOOK(LT_CAR_FLARES, lt_car_flares, POP_HOOK_BEFORE, 1) != POP_OK ||
        LT_HOOK(LT_CAR_FLARES, lt_after, POP_HOOK_AFTER, 2) != POP_OK ||
        g_api->hook_install(g_api, MP_FE_SERVICE, lt_service, POP_HOOK_BEFORE, NULL, &g_lt_hooks[0]) != POP_OK) {
        log_line("core.nfsc: turn signals unavailable (hooks)");
        return;
    }
    if (LT_HOOK(LT_LIGHT_TEXTURES, lt_textures, POP_HOOK_BEFORE, 3) != POP_OK ||
        LT_HOOK(LT_LIGHT_TEXTURES, lt_after, POP_HOOK_AFTER, 4) != POP_OK)
        log_line("core.nfsc: headlights off keep their lit glass (hook 007adc90 unavailable)");
#undef LT_HOOK
    uint32_t id;
    g_api->on_key(g_api, lt_key, NULL, &id);
    log_line("core.nfsc: turn signals on Q / E (d-pad left / right), headlight flash on F (L3), double press: headlights on / off");
#ifdef __EMSCRIPTEN__
    MAIN_THREAD_ASYNC_EM_ASM({
        var block = {};
        block.memory = wasmMemory;
        block.ptr = $0;
        globalThis.carbonLightsBlock = block;
        globalThis.dispatchEvent(new Event("carbon-lights"));
    }, (uint32_t)(uintptr_t)sh);
#endif
}
