/* paint.h - paint slots past the game's eight (included by nfsc.c after mp.h).
 *
 * A car's paint (base colour and vinyls, composited on the CPU) goes into the texture its RideInfo
 * names: 007c2a10 (thiscall ride, id, flag) sets ride+0x48 = hash("DUMMY_SKIN<id>") (when the model
 * has a composite skin: byte +0xc7 of its car type record, [0x00b74ccc] + [ride] * 0xd0),
 * ride+0x4c = hash("DUMMY_WHEEL<id>"), ride+0x50 = hash("DUMMY_SPINNER<id>"); a lite car (byte
 * ride+6) uses DUMMY_SKIN_LITE<id>. Ids come from a mask of 30 ([0x00a732a4], 007e6640), but the
 * game data has only DUMMY_SKIN1-8 and DUMMY_WHEEL1-8 (logs/paint-20261105/findings.md), so the
 * compositor (0xb76860, 007d1320) finds no texture for ids 9+ (GetTextureInfo 0055cfd0 returns 0
 * at 007d13a4) and gives up: those cars show no paint of their own.
 *
 * Here ids 9..PAINT_LAST get textures of their own, made the first time 007c2a10 names an id past
 * eight, and kept: a copy of a present slot's TextureInfo (0x7c bytes) and platform record (0x18),
 * with the new name and hash, pixels of zero, and the D3D texture made by the game's own creator
 * 00725c80 (thiscall TextureInfo; CreateTexture(w, h, mips, 0, [plat+0xc], MANAGED, plat+0x10),
 * then each mip copied in through LockRect from TI+0x50). A REPLACE hook on GetTextureInfo hands
 * them out (a 0x2000-bit table of their hashes rejects every other name in one test). The game's
 * compositor then paints into them as into DUMMY_SKIN1-8: 007d7580 locks the D3D texture
 * ([[TI]+0x10], 0070f740) or copies surfaces (00732ca0); it never writes TI+0x50, so all slots
 * share one block of zero pixels (only read while a texture is made).
 *
 * TextureInfo (0x7c; 00725c80, 0055aa60, 0055f6f0, 0070f740, 007d7580):
 *   +0x00 platform record *, +0x0c name hash, +0x10 class hash, +0x14 / +0x18 pixel / palette
 *   offset in its pack, +0x1c pixel bytes (all mips), +0x20 palette bytes, +0x24 mip 0 bytes,
 *   +0x28 / +0x2a width / height (int16), +0x2c / +0x2d log2 width / height, +0x2e format
 *   (0x20 A8R8G8B8, 0x22 DXT, 0x08 paletted), +0x32 mips, +0x37 handed out (GetTextureInfo sets 1),
 *   +0x4c its pack, +0x50 pixels, +0x54 palette, +0x58 name length, +0x59 name.
 * Platform record (0x18, chunk 33310005 in a pack): +0 / +4 links in the list of made textures
 *   0x00b1dbe0, +0x08 sampler state bits (0073b9e0), +0x0c D3DFORMAT, +0x10 IDirect3DTexture9 *.
 *
 * Ids past PAINT_LAST (and past the game's 30) are left for later: the id mask has 30. */

#define PAINT_GAME 8u        /* DUMMY_SKIN1-8, DUMMY_WHEEL1-8 in the game data */
#define PAINT_LAST 72u       /* ids up to this get textures of their own; the game hands out 1-30, paint.h 31-72 */
#define PAINT_GAME_IDS 30u   /* the game's mask [0x00a732a4]: ids 1-30 (007e6640 loop, cmp edx 0x1e) */
#define PAINT_KINDS 2u       /* DUMMY_SKIN, DUMMY_WHEEL (the data has no DUMMY_SPINNER) */
#define PAINT_TI 0x7cu
#define PAINT_PLAT 0x18u
#define PAINT_GET_INFO 0x0055cfd0u /* cdecl (hash, return_default, include_unloaded) -> TextureInfo* */
#define PAINT_CREATE 0x00725c80u   /* thiscall TextureInfo: its D3D texture */
#define PAINT_NAME_IDS 0x007c2a10u /* thiscall (ride, id, flag), RET 8 */
#define PAINT_CAR_TYPES 0x00b74ccc
#define PAINT_COMPOSE 0x007d1320u  /* thiscall compositor (ride) -> al */
#define PAINT_OUTPUT 0x007d7580u   /* thiscall compositor: copy the result into the car's texture -> al */
#define PAINT_RENDER_CONNS 0x00b77db0u
#define PAINT_ID_MASK 0x00a732a4u
#define PAINT_CONN_INIT_RIDE 0x007da770u /* thiscall ride; called by 007e6580 at 007e667c, right after the id is named */
#define PAINT_CONN_INIT_RET 0x007e6681u
#define PAINT_IS_COMPOSITE 0x007c3090u /* cdecl (car type) -> al: the model takes a skin id */
#define PAINT_CONN_FREE 0x007e2fd0u    /* thiscall render connection: frees its id at 007e3031 */
#define PAINT_COMPOSE_STEP 0x007e6e00u /* thiscall car loader (entry): one compositor step a frame */
#define PAINT_COMPOSITOR 0x00b76860u
#define PAINT_STATE_STEP 0x007dba00u   /* thiscall compositor -> 0 failed / 2 done / else more to do */
#define PAINT_LOAD_NEXT 0x007e4bb0u    /* thiscall car loader: what 007e6e00 does when a skin is done */
#define PAINT_QUEUE 0x006997a0u        /* cdecl (fn, arg, flag): run fn(arg) at the next service (007e6e00 queues 007e6ef0) */
#ifndef PAINT_STEP_BUDGET_US
#define PAINT_STEP_BUDGET_US 4000 /* compositor time a frame: another step only while it should still fit (paint_fast.h made steps short) */
#endif
#ifndef PAINT_DIAG
#define PAINT_DIAG 0 /* 1: log the compositor and each car's paint (logs/paint-slots-20261105) */
#endif

typedef struct PaintTex {
    uint32_t hash, ti;
} PaintTex;

/* A slot of the game's to copy (TextureInfo + platform record), per kind: taken from the game's own
 * DUMMY_SKIN1-8 / DUMMY_WHEEL1-8 whenever GetTextureInfo hands one out (they are loaded by the car
 * loader only while a car needs a composite skin, so none may be there when an id past eight first
 * comes up), else the v1.4 data's values below (read from a running game, 2026-11-05: DUMMY_SKIN2
 * 1024x1024 DXT1, one mip, 512 KB, class c509d75c; DUMMY_WHEEL2 256x256 A8R8G8B8, one mip, 256 KB,
 * class 1b81e7b0; pointers left out). */
typedef struct PaintTmpl {
    int have;
    uint8_t ti[PAINT_TI], plat[PAINT_PLAT];
} PaintTmpl;
static PaintTmpl g_paint_tmpl[PAINT_KINDS];
static uint32_t g_paint_game_hash[PAINT_KINDS][PAINT_GAME]; /* DUMMY_SKIN1-8, DUMMY_WHEEL1-8 */
static uint8_t g_paint_game_bits[0x2000u / 8u];
static const uint32_t kPaintFallback[PAINT_KINDS][(PAINT_TI + PAINT_PLAT) / 4] = {
    {0, 0, 0, 0, 0xc509d75cu, 0, 0xffffffffu, 0x00080000u, 0, 0x00080000u, 0x04000400u, 0x00220a0au, 0x00010000u,
     0, 0x01000200u, 0, 0, 0x01000000u, 0x00000100u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     /* platform */ 0, 0, 0x0b00f123u, 0x31545844u /* DXT1 */, 0, 0},
    {0, 0, 0, 0, 0x1b81e7b0u, 0, 0xffffffffu, 0x00040000u, 0, 0x00040000u, 0x01000100u, 0x00200808u, 0x00010000u,
     0, 0x01000000u, 0, 0, 0x01000000u, 0x00000100u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     /* platform */ 0, 0, 0x0b80f123u, 0x00000015u /* A8R8G8B8 */, 0, 0},
};

static PaintTex g_paint_tex[PAINT_KINDS * (PAINT_LAST - PAINT_GAME)];
static int g_paint_ntex;
static int8_t g_paint_ok[PAINT_LAST + 1]; /* id n: 1 its textures are made, -1 they could not be */
static uint8_t g_paint_bits[0x2000u / 8u]; /* hash & 0x1fff of every g_paint_tex */
static uint32_t g_paint_zero, g_paint_zero_size; /* the shared zero pixels */
#define PAINT_HOOKS 8
static uint32_t g_paint_hooks[PAINT_HOOKS];
static uint32_t g_paint_calls, g_paint_hits; /* GetTextureInfo calls seen, answered here */
static uint32_t g_paint_frame;

static uint32_t paint_hash(const char *s) { /* bStringHash 00471050 */
    uint32_t h = 0xffffffffu;
    for (; *s; ++s)
        h = h * 33u + (uint8_t)*s;
    return h;
}

static uint8_t paint_u8(uint32_t a) {
    uint8_t v = 0;
    g_api->guest_read_u8(g_api, a, &v);
    return v;
}

/* The override: one of our textures, or 0 (then the game's own lookup runs). */
static uint32_t paint_find(uint32_t h) {
    if (!(g_paint_bits[(h & 0x1fffu) >> 3] & (1u << (h & 7u))))
        return 0;
    for (int i = 0; i < g_paint_ntex; ++i)
        if (g_paint_tex[i].hash == h)
            return g_paint_tex[i].ti;
    return 0;
}

/* The game handed out ti for h: if it is one of its DUMMY_SKIN1-8 / DUMMY_WHEEL1-8 and no template of
 * that kind is kept yet, keep a copy (it may be unloaded later). */
static void paint_keep_template(uint32_t h, uint32_t ti) {
    for (uint32_t k = 0; k < PAINT_KINDS; ++k) {
        if (g_paint_tmpl[k].have)
            continue;
        for (uint32_t t = 0; t < PAINT_GAME; ++t) {
            if (g_paint_game_hash[k][t] != h)
                continue;
            const uint32_t plat = get_u32(ti);
            void *a = NULL, *b = NULL;
            if (get_u32(ti + 0xc) != h || !plat || g_api->guest_ptr(g_api, ti, PAINT_TI, &a) != POP_OK ||
                g_api->guest_ptr(g_api, plat, PAINT_PLAT, &b) != POP_OK)
                return;
            memcpy(g_paint_tmpl[k].ti, a, PAINT_TI);
            memcpy(g_paint_tmpl[k].plat, b, PAINT_PLAT);
            g_paint_tmpl[k].have = 1;
            return;
        }
    }
}

static void paint_get_info(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    ++g_paint_calls;
    const uint32_t h = get_u32(cpu->esp + 4);
    const uint32_t ti = g_paint_ntex ? paint_find(h) : 0;
    if (!ti) {
        api->call_original(api, cpu->target, cpu);
        if (cpu->eax && (g_paint_game_bits[(h & 0x1fffu) >> 3] & (1u << (h & 7u))))
            paint_keep_template(h, cpu->eax);
        return;
    }
    ++g_paint_hits;
    api->guest_write_u8(api, ti + 0x37, 1); /* as the game marks a texture it hands out */
    api->hook_return(api, cpu, ti, 0);
}

/* Bytes of pixels of a TextureInfo: +0x1c (every mip), at least mip 0's. */
static uint32_t paint_pixel_bytes(const uint8_t *ti) {
    uint32_t all, mip0;
    memcpy(&all, ti + 0x1c, 4);
    memcpy(&mip0, ti + 0x24, 4);
    return all > mip0 ? all : mip0;
}

/* The record to copy for kind k: a slot of the game's that is loaded now, else the kept template,
 * else the fallback. Returns where it came from (for the log). */
static const char *paint_source(uint32_t k, uint8_t *ti, uint8_t *plat) {
#ifndef PAINT_TEST_FALLBACK /* TEST ONLY: -DPAINT_TEST_FALLBACK builds every slot from the built-in values */
    for (uint32_t t = 0; t < PAINT_GAME; ++t) {
        const uint32_t h = g_paint_game_hash[k][t], args[3] = {h, 0, 0};
        uint32_t src = 0;
        void *a = NULL, *b = NULL;
        if (g_api->guest_call(g_api, PAINT_GET_INFO, 0, args, 3, &src) != POP_OK || !src || get_u32(src + 0xc) != h)
            continue;
        const uint32_t pl = get_u32(src);
        if (!pl || g_api->guest_ptr(g_api, src, PAINT_TI, &a) != POP_OK || g_api->guest_ptr(g_api, pl, PAINT_PLAT, &b) != POP_OK)
            continue;
        memcpy(ti, a, PAINT_TI);
        memcpy(plat, b, PAINT_PLAT);
        return "a loaded game slot";
    }
    if (g_paint_tmpl[k].have) {
        memcpy(ti, g_paint_tmpl[k].ti, PAINT_TI);
        memcpy(plat, g_paint_tmpl[k].plat, PAINT_PLAT);
        return "a kept game slot";
    }
#endif
    memcpy(ti, kPaintFallback[k], PAINT_TI);
    memcpy(plat, (const uint8_t *)kPaintFallback[k] + PAINT_TI, PAINT_PLAT);
    return "the built-in v1.4 values";
}

/* Textures of id n (PAINT_GAME < n <= PAINT_LAST): made once, kept. 1 when they are there. */
static int paint_make(uint32_t n) {
    static const char *const kind[PAINT_KINDS] = {"DUMMY_SKIN%u", "DUMMY_WHEEL%u"};
    if (n <= PAINT_GAME)
        return 1;
    if (n > PAINT_LAST)
        return 0;
    if (g_paint_ok[n])
        return g_paint_ok[n] > 0;
    uint8_t rec[PAINT_KINDS][PAINT_TI], prec[PAINT_KINDS][PAINT_PLAT];
    const char *from[PAINT_KINDS];
    uint32_t need = 0;
    char line[240];
    for (uint32_t k = 0; k < PAINT_KINDS; ++k) {
        from[k] = paint_source(k, rec[k], prec[k]);
        const uint32_t px = paint_pixel_bytes(rec[k]);
        if (!px || px > (16u << 20)) {
            snprintf(line, sizeof line, "core.nfsc: paint slot %u: a source with %u bytes of pixels, not copied", n, px);
            log_line(line);
            g_paint_ok[n] = -1;
            return 0;
        }
        need = px > need ? px : need;
    }
    if (need > g_paint_zero_size) { /* the shared zero pixels; an older, smaller block stays (a texture may name it) */
        uint32_t z = 0;
        if (g_api->guest_alloc(g_api, need, &z) != POP_OK) {
            snprintf(line, sizeof line, "core.nfsc: paint slot %u: no mod memory for %u bytes of zero pixels", n, need);
            log_line(line);
            g_paint_ok[n] = -1;
            return 0;
        }
        g_paint_zero = z;
        g_paint_zero_size = need;
    }
    int made = 0;
    for (uint32_t k = 0; k < PAINT_KINDS; ++k) {
        char name[24];
        uint32_t ti = 0;
        if (g_api->guest_alloc(g_api, PAINT_TI + PAINT_PLAT, &ti) != POP_OK) {
            log_line("core.nfsc: paint slot: no mod memory for a texture record");
            break;
        }
        const uint32_t plat = ti + PAINT_TI;
        snprintf(name, sizeof name, kind[k], n);
        const uint32_t h = paint_hash(name), len = (uint32_t)strlen(name);
        uint8_t *t = rec[k], *pl = prec[k];
        uint32_t src_hash;
        memcpy(&src_hash, t + 0xc, 4);
        memset(pl + 0x0, 0, 8);   /* not in the made list yet: 00725c80 links it */
        memset(pl + 0x10, 0, 4);  /* no D3D texture yet (00725c80 would release one) */
        memcpy(t + 0x0, &plat, 4);
        memcpy(t + 0xc, &h, 4);
        memset(t + 0x14, 0, 4);   /* no place in a pack */
        memcpy(t + 0x50, &g_paint_zero, 4);
        if (t[0x2e] != 0x08)
            memset(t + 0x54, 0, 4); /* a palette only for a paletted texture (then the source's) */
        t[0x37] = 0;
        /* the full size: 00725c80 halves +0x28/+0x2a in place for a lower texture detail, so a copy of a
         * made texture may hold the smaller size; log2 width / height (+0x2c/+0x2d) are never changed */
        if (t[0x2c] < 16 && t[0x2d] < 16) {
            const uint16_t w = (uint16_t)(1u << t[0x2c]), hh = (uint16_t)(1u << t[0x2d]);
            memcpy(t + 0x28, &w, 2);
            memcpy(t + 0x2a, &hh, 2);
        }
        memset(t + 0x58, 0, PAINT_TI - 0x58);
        t[0x58] = (uint8_t)(((len + 5u) >> 2 << 2) - 1u); /* as 0055ab0c */
        memcpy(t + 0x59, name, len);
        void *dst = NULL;
        if (g_api->guest_ptr(g_api, ti, PAINT_TI + PAINT_PLAT, &dst) != POP_OK) {
            log_line("core.nfsc: paint slot: texture record not writable");
            break;
        }
        memcpy(dst, t, PAINT_TI);
        memcpy((uint8_t *)dst + PAINT_TI, pl, PAINT_PLAT);
        uint32_t eax = 0;
        if (g_api->guest_call(g_api, PAINT_CREATE, ti, NULL, 0, &eax) != POP_OK || !get_u32(plat + 0x10)) {
            snprintf(line, sizeof line, "core.nfsc: paint slot %u: the game made no texture for %s", n, name);
            log_line(line);
            break; /* the record stays allocated, unused (0x94 bytes) */
        }
        g_paint_tex[g_paint_ntex].hash = h;
        g_paint_tex[g_paint_ntex].ti = ti;
        ++g_paint_ntex;
        g_paint_bits[(h & 0x1fffu) >> 3] |= (uint8_t)(1u << (h & 7u));
        ++made;
        snprintf(line, sizeof line,
                 "core.nfsc: paint slot %u: %s made from %s (%08x; %ux%u, format %02x / d3d %08x, %u mips, %u bytes of pixels), "
                 "TextureInfo %08x, texture %08x",
                 n, name, from[k], src_hash, get_u32(ti + 0x28) & 0xffff, get_u32(ti + 0x28) >> 16, paint_u8(ti + 0x2e),
                 get_u32(plat + 0xc), paint_u8(ti + 0x32), paint_pixel_bytes(t), ti, get_u32(plat + 0x10));
        log_line(line);
    }
    g_paint_ok[n] = made == (int)PAINT_KINDS ? 1 : -1;
    if (g_paint_ok[n] < 0) {
        snprintf(line, sizeof line, "core.nfsc: paint slot %u: textures not made (%d of %u)", n, made, PAINT_KINDS);
        log_line(line);
    }
    return g_paint_ok[n] > 0;
}

/* Before 007c2a10 names a ride's textures: id n past eight gets its textures first. */
static void paint_name_ids(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)inv;
    (void)user;
    const uint32_t ride = cpu->ecx, id = get_u32(cpu->esp + 4);
    if (id <= PAINT_GAME || id > PAINT_LAST || !ride || paint_u8(ride + 6))
        return; /* the game's own, out of range, or a lite car (DUMMY_SKIN_LITE1-30 exist) */
    const uint32_t types = get_u32(PAINT_CAR_TYPES);
    if (!types || !paint_u8(types + get_u32(ride) * 0xd0u + 0xc7))
        return; /* no composite skin for this model */
    paint_make(id);
}


/* ---- ids past the game's 30 ----
 * 007e6580 (a car's render connection) takes the lowest free bit of [0x00a732a4] (bits 0-29: ids 1-30)
 * for a model that 007c3090 says has a composite skin, stores it at conn+0x68 and names the textures
 * (007c2a10); with no bit free conn+0x68 stays 0 and the car has no skin of its own. 007e2fd0 gives
 * the bit back (007e3031: or [0xa732a4], 1 << (id - 1), only while conn+0x24 is set), which for an id
 * past 32 would free a wrong bit (x86 shifts by the count mod 32). Here, once the game's 30 are gone,
 * a car gets an id from 31-PAINT_LAST of our own table (a call-site hook on the call after the
 * naming, 007da770 from 007e667c: ESI = the connection, ECX = its ride, EBX = 007e6580's fifth
 * argument), and 007e2fd0 is preceded by a hook that frees our id and clears conn+0x68 so the game's
 * own free skips it. The game's own ids are left to the game. */
static uint32_t g_paint_owner[PAINT_LAST + 1]; /* our ids: the connection holding it, 0 free */
static uint32_t g_paint_extra, g_paint_extra_max, g_paint_extra_none;

static void paint_conn_named(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t conn = cpu->esi, ride = cpu->ecx;
    if (conn && ride && get_u32(conn + 0x3c) == ride && !get_u32(conn + 0x68) &&
        !(get_u32(PAINT_ID_MASK) & ((1u << PAINT_GAME_IDS) - 1u))) {
        const uint32_t args[1] = {get_u32(conn + 0x28)};
        uint32_t composite = 0;
        if (api->guest_call(api, PAINT_IS_COMPOSITE, 0, args, 1, &composite) == POP_OK && (composite & 0xff)) {
            uint32_t id = 0;
            for (uint32_t n = PAINT_GAME_IDS + 1; n <= PAINT_LAST && !id; ++n)
                if (!g_paint_owner[n])
                    id = n;
            if (!id) {
                if (++g_paint_extra_none <= 5u)
                    log_line("core.nfsc: paint: no skin id left (the game's 30 and paint.h's all in use)");
            } else {
                g_paint_owner[id] = conn;
                mp_put(conn + 0x68, id);
                const uint32_t name[2] = {id, cpu->ebx == 0}; /* as 007e666a..74 */
                uint32_t eax = 0;
                api->guest_call(api, PAINT_NAME_IDS, ride, name, 2, &eax);
                ++g_paint_extra;
                if (g_paint_extra > g_paint_extra_max)
                    g_paint_extra_max = g_paint_extra;
                char line[128];
                snprintf(line, sizeof line, "core.nfsc: paint: skin id %u for a car past the game's 30 (%u such now)", id,
                         g_paint_extra);
                log_line(line);
            }
        }
    }
    api->call_original(api, cpu->target, cpu);
}

static void paint_conn_free(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)inv;
    (void)user;
    const uint32_t conn = cpu->ecx, id = conn ? get_u32(conn + 0x68) : 0;
    if (id <= PAINT_GAME_IDS || id > PAINT_LAST || g_paint_owner[id] != conn || !get_u32(conn + 0x24))
        return;
    g_paint_owner[id] = 0;
    mp_put(conn + 0x68, 0);
    if (g_paint_extra)
        --g_paint_extra;
}

/* ---- compositor speed ----
 * The compositor (0xb76860) makes one car's skin at a time, one state step (007dba00) a frame: the car
 * loader's per-frame callback 007e6e00 runs one step and queues itself again (006997a0) until the
 * step says 0 (failed) or 2 (done), then marks the car's skin entry done (+0x19 = 1), clears
 * loader+0x14 and goes on loading (007e4bb0). With [0xb7688c] set it runs every step at once.
 * Here 007e6e00 runs steps while they should fit in PAINT_STEP_BUDGET_US of this frame: one always,
 * as the game does, and another only when the time used so far plus that state's recent step time is
 * within the budget, so a frame's compositor time stays near the budget instead of passing it by a whole
 * step (logs/paint-speed-20261105). Then it does what the original's end does: done (entry +0x19, loader
 * +0x14 = 0, 007e4bb0) or queue itself again (006997a0(007e6ef0, entry, 1), loader +0x14 = 1). */
static uint64_t g_paint_step_us, g_paint_step_max_us;
static uint32_t g_paint_budget_us = PAINT_STEP_BUDGET_US; /* TEST ONLY: RECOMP_PAINT_STEP_US=<us> */
static uint32_t g_paint_steps, g_paint_skins;

static uint64_t paint_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static uint32_t g_paint_cb, g_paint_frames; /* compositor callbacks; frames (TEST crowd only) */
static int g_paint_legacy; /* TEST: RECOMP_PAINT_PACING=0, Step B's pacing */
/* How long each compositor state's step takes, per state ([0xb76860 + 0x34], 0-13): a running average
 * that moves a quarter of the way to each new time, a new time counted at most as twice the average (a
 * step stretched by the machine, e.g. this thread descheduled, then barely moves it). A state not seen
 * yet is taken as 3 ms. */
static uint64_t g_paint_state_us[16] = {3000, 3000, 3000, 3000, 3000, 3000, 3000, 3000, 3000, 3000, 3000, 3000, 3000, 3000, 3000, 3000};
static void paint_step_seen(uint32_t st, uint64_t us) {
    uint64_t *e = &g_paint_state_us[st & 15u];
    const uint64_t v = us < 2u * *e ? us : 2u * *e;
    *e = v >= *e ? *e + (v - *e) / 4u : *e - (*e - v) / 4u;
    if (*e < 20u)
        *e = 20u;
}
/* TEST ONLY (RECOMP_PAINT_TIMELINE=1): per skin, what happened since the last one was done: the file
 * loads the game asked for (00560050), when the compositor started on it, and when it was done. */
static int g_tl_on;
static uint32_t g_tl_req, g_tl_cb, g_tl_frames0, g_tl_steps0, g_tl_n;
static uint64_t g_tl_t0, g_tl_req_first, g_tl_req_last, g_tl_start;
static void paint_tl_load(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)cpu;
    (void)inv;
    (void)user;
    const uint64_t t = paint_us();
    if (!g_tl_req++)
        g_tl_req_first = t;
    g_tl_req_last = t;
}
/* the events of skins 11-14 one by one: which function, frame, ms since that skin's start */
static void paint_tl_event(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)inv;
    if (g_tl_n < 10u || g_tl_n > 13u || !g_tl_t0)
        return;
    char line[200];
    snprintf(line, sizeof line, "core.nfsc: paint timeline event %08x (ecx %08x, arg %08x %08x) frame +%u, +%.1f ms", (uint32_t)(uintptr_t)user, cpu->ecx,
             get_u32(cpu->esp + 4), get_u32(cpu->esp + 8), g_paint_frames - g_tl_frames0, (paint_us() - g_tl_t0) / 1000.0);
    log_line(line);
}
static void paint_tl_done(void) {
    if (!g_tl_on)
        return;
    const uint64_t t = paint_us();
    if (g_tl_t0 && ++g_tl_n <= 80u) {
        char line[260];
        snprintf(line, sizeof line,
                 "core.nfsc: paint timeline skin %u: %.1f ms since the last; %u file loads (first +%.1f, last +%.1f ms); "
                 "compositor from +%.1f ms, %u callbacks, %u steps, %u frames",
                 g_tl_n, (t - g_tl_t0) / 1000.0, g_tl_req, g_tl_req ? (g_tl_req_first - g_tl_t0) / 1000.0 : 0.0,
                 g_tl_req ? (g_tl_req_last - g_tl_t0) / 1000.0 : 0.0, g_tl_start ? (g_tl_start - g_tl_t0) / 1000.0 : -1.0, g_tl_cb,
                 g_paint_steps - g_tl_steps0, g_paint_frames - g_tl_frames0);
        log_line(line);
    }
    g_tl_t0 = t;
    g_tl_req = g_tl_cb = 0;
    g_tl_start = 0;
    g_tl_frames0 = g_paint_frames;
    g_tl_steps0 = g_paint_steps;
}
static void paint_compose_step(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    ++g_paint_cb;
    const uint32_t loader = cpu->ecx, entry = get_u32(cpu->esp + 4);
    const uint64_t t0 = paint_us();
    if (g_tl_on) {
        if (!g_tl_t0)
            g_tl_t0 = t0;
        if (!g_tl_cb++)
            g_tl_start = t0;
    }
    if (g_paint_legacy && !paint_u8(0xb7688c)) { /* TEST: Step B's pacing (RECOMP_PAINT_PACING=0) */
        for (int i = 0; i < 64; ++i) {
            uint32_t r = 1;
            const uint64_t s0 = paint_us();
            if (s0 - t0 >= 3000u / 2u || api->guest_call(api, PAINT_STATE_STEP, PAINT_COMPOSITOR, NULL, 0, &r) != POP_OK)
                break;
            ++g_paint_steps;
            if (r == 0 || r == 2) {
                api->guest_write_u8(api, entry + 0x19, 1);
                mp_put(loader + 0x14, 0);
                uint32_t eax = 0;
                api->guest_call(api, PAINT_LOAD_NEXT, loader, NULL, 0, &eax);
                ++g_paint_skins;
                const uint64_t us = paint_us() - t0;
                g_paint_step_us += us;
                if (us > g_paint_step_max_us)
                    g_paint_step_max_us = us;
                paint_tl_done();
                api->hook_return(api, cpu, eax, 4);
                return;
            }
        }
        goto original;
    }
    if (!paint_u8(0xb7688c)) {
        int n = 0;
        for (;;) {
            uint32_t r = 1;
            const uint32_t st = get_u32(PAINT_COMPOSITOR + 0x34) & 15u; /* the state 007dba00 runs next */
            const uint64_t s0 = paint_us();
            if (n && (n >= 64 || s0 - t0 + g_paint_state_us[st] > g_paint_budget_us))
                break;
            if (api->guest_call(api, PAINT_STATE_STEP, PAINT_COMPOSITOR, NULL, 0, &r) != POP_OK) {
                if (!n)
                    goto original;
                break;
            }
            ++n;
            ++g_paint_steps;
            paint_step_seen(st, paint_us() - s0);
#if PAINT_DIAG
            static unsigned step_told;
            if (++step_told <= 60u) {
                char line[96];
                snprintf(line, sizeof line, "core.nfsc: paint step: state %u -> %u, %llu us", st, r, (unsigned long long)(paint_us() - s0));
                log_line(line);
            }
#endif
            if (r == 0 || r == 2) { /* done: what 007e6e1f..30 does */
                api->guest_write_u8(api, entry + 0x19, 1);
                mp_put(loader + 0x14, 0);
                uint32_t eax = 0;
                api->guest_call(api, PAINT_LOAD_NEXT, loader, NULL, 0, &eax);
                ++g_paint_skins;
                const uint64_t us = paint_us() - t0;
                g_paint_step_us += us;
                if (us > g_paint_step_max_us)
                    g_paint_step_max_us = us;
                paint_tl_done();
                api->hook_return(api, cpu, eax, 4);
                return;
            }
        }
        /* not done: what 007e6e4c..68 does, queue this again for the next frame and mark the loader busy */
        const uint32_t args[3] = {0x007e6ef0u, entry, 1};
        uint32_t eax = 0;
        if (api->guest_call(api, PAINT_QUEUE, 0, args, 3, &eax) == POP_OK) {
            mp_put(loader + 0x14, 1);
            const uint64_t us = paint_us() - t0;
            g_paint_step_us += us;
            if (us > g_paint_step_max_us)
                g_paint_step_max_us = us;
            cpu->ecx = 0x007e6ef0u; /* as 006997a0 leaves them */
            cpu->edx = entry;
            api->hook_return(api, cpu, eax, 4);
            return;
        }
        /* (the queue call failed: the original runs one more step and queues itself) */
    }
original:;
    const uint64_t s1 = paint_us();
    const uint32_t st = get_u32(PAINT_COMPOSITOR + 0x34) & 15u;
    api->call_original(api, cpu->target, cpu); /* one step (or all, with [0xb7688c]), and the game's own done / again */
    ++g_paint_steps;
    paint_step_seen(st, paint_us() - s1);
    const uint64_t us = paint_us() - t0;
    g_paint_step_us += us;
    if (us > g_paint_step_max_us)
        g_paint_step_max_us = us;
    if (paint_u8(entry + 0x19)) {
        ++g_paint_skins;
        paint_tl_done();
    }
}


/* ---- TEST ONLY: a painted crowd (RECOMP_PAINT_CROWD=<n>) ----
 * In free roam, PC_START_MS after the player's car is in the world, builds n parked cars (class 7, the
 * player's model) in the player's own look with body colour and vinyl colour varied per car (the look
 * record's paint floats +0xe0/+0xe4 and layer 0's colour bytes +0x144/+0x145), RECOMP_PAINT_CROWD_BATCH
 * (default 4) every PC_EVERY_MS, in two lanes 7 m apart ahead of where the player stood, PC_ROW m apart. Logs
 * each build and how long the compositor takes to give them all their skins (paint_skins). Needs a
 * look with a vinyl to show anything (vinyl layers make the compositor run). */
#define PC_START_MS 8000.0
#define PC_EVERY_MS 400.0
#define PC_ROW 8.0f
#define PC_LOOK 0x42cu
static int g_pc_want = -1, g_pc_built, g_pc_tries, g_pc_batch = 4;
static double g_pc_seen_at, g_pc_last, g_pc_first;
static float g_pc_at[3], g_pc_fwd[3];
static uint32_t g_pc_look[8], g_pc_skins_at_start, g_pc_said, g_pc_calls0, g_pc_frames0, g_pc_gap_n;
static uint64_t g_pc_frame_last, g_pc_gap_max;

static void paint_crowd(void) {
    if (g_pc_want < 0) {
        const char *e = getenv("RECOMP_PAINT_CROWD"), *b = getenv("RECOMP_PAINT_CROWD_BATCH");
        g_pc_want = e && *e ? atoi(e) : 0;
        if (b && *b && atoi(b) > 0)
            g_pc_batch = atoi(b);
    }
    if (g_pc_want <= 0)
        return;
    const double now = mp_now();
    static int told;
    if (!told) {
        told = 1;
        char line[96];
        snprintf(line, sizeof line, "core.nfsc: paint crowd: waiting for the player's car (%d cars)", g_pc_want);
        log_line(line);
    }
    if (g_pc_built > 0) { /* how far the compositor is: a line every 4 skins and at the last */
        const uint32_t done = g_paint_skins - g_pc_skins_at_start;
        if (done != g_pc_said && (done % 4u == 0 || (int)done >= g_pc_built)) {
            g_pc_said = done;
            char line[320];
            snprintf(line, sizeof line, "core.nfsc: paint crowd: %u skins made %.1f s after the first build (%d cars built); "
                     "compositor %u steps, %llu us in all, worst frame %llu us; %u compositor frames of %u; "
                     "longest frame %.1f ms, %u over 33 ms",
                     done, (now - g_pc_first) / 1000.0, g_pc_built, g_paint_steps, (unsigned long long)g_paint_step_us,
                     (unsigned long long)g_paint_step_max_us, g_paint_cb - g_pc_calls0, g_paint_frames - g_pc_frames0,
                     g_pc_gap_max / 1000.0, g_pc_gap_n);
            log_line(line);
        }
    }
    if (g_pc_built >= g_pc_want || g_pc_tries > g_pc_want * 3)
        return;
    const uint32_t player = mp_player(), state = player ? mp_state(get_u32(player + 0x70)) : 0;
    if (!state) {
        g_pc_seen_at = 0;
        return;
    }
    if (!g_pc_seen_at) {
        g_pc_seen_at = now;
        log_line("core.nfsc: paint crowd: the player's car is in the world");
    }
    if (now - g_pc_seen_at < PC_START_MS || now - g_pc_last < PC_EVERY_MS)
        return;
    g_pc_last = now;
    if (!g_pc_look[0]) { /* the player's look and seven variants of it */
        const uint32_t cust = get_u32(player + 0xe8);
        if (!cust) {
            log_line("core.nfsc: paint crowd: the player's car has no look record; nothing built");
            g_pc_want = 0;
            return;
        }
        static const float sb[8][2] = {{-1, -1}, {1, 1}, {1, 0.35f}, {0, 0.05f}, {0.5f, 1}, {0, 0.5f}, {1, 0.7f}, {0.2f, 0.9f}};
        static const uint8_t vc[8][2] = {{0, 0}, {255, 255}, {0, 255}, {255, 255}, {255, 120}, {0, 255}, {255, 60}, {128, 200}};
        for (int v = 0; v < 8; ++v) {
            if (g_api->guest_alloc(g_api, PC_LOOK, &g_pc_look[v]) != POP_OK) {
                g_pc_want = 0;
                return;
            }
            mp_copy(g_pc_look[v], cust, PC_LOOK & ~3u);
            for (uint32_t i = 0; i < 0x38; i += 4)
                mp_put(g_pc_look[v] + 0xf8 + i, 0); /* no performance package (look.h) */
            if (v) {
                put_float(g_pc_look[v] + 0xe0, sb[v][0]);
                put_float(g_pc_look[v] + 0xe4, sb[v][1]);
                g_api->guest_write_u8(g_api, g_pc_look[v] + 0x138 + 0xc, vc[v][0]);
                g_api->guest_write_u8(g_api, g_pc_look[v] + 0x138 + 0xd, vc[v][1]);
            }
        }
        for (int i = 0; i < 3; ++i) {
            g_pc_at[i] = bits_float(get_u32(state + 0x10 + 4u * i));
            g_pc_fwd[i] = bits_float(get_u32(state + 0x90 + 4u * i));
        }
        char line[160];
        snprintf(line, sizeof line, "core.nfsc: paint crowd: %d cars, %d at a time, from %.1f %.1f %.1f, %u vinyl layers in the look",
                 g_pc_want, g_pc_batch, g_pc_at[0], g_pc_at[1], g_pc_at[2], paint_u8(cust + 0x344));
        log_line(line);
    }
    for (int k = 0; k < g_pc_batch && g_pc_built < g_pc_want; ++k) {
        const int i = g_pc_built;
        /* the body state's frame: x, z horizontal, y up (probe.js, tp.js); the side is (z, 0, -x) */
        const float along = 16.0f + PC_ROW * (float)(i / 2), across = i % 2 ? 3.5f : -3.5f; /* the middle lane stays free */
        const float pos[3] = {g_pc_at[0] + g_pc_fwd[0] * along + g_pc_fwd[2] * across, g_pc_at[1] + 0.5f,
                              g_pc_at[2] + g_pc_fwd[2] * along - g_pc_fwd[0] * across};
        ++g_pc_tries;
        const uint32_t car = mp_build(player, MP_REMOTE, 0, pos, 2u | 8u, g_pc_look[i % 8]);
        char line[160];
        if (!car) {
            snprintf(line, sizeof line, "core.nfsc: paint crowd: car %d not built (try %d)", i + 1, g_pc_tries);
            log_line(line);
            break;
        }
        if (!g_pc_built) {
            g_pc_first = now;
            g_pc_skins_at_start = g_paint_skins;
            g_pc_calls0 = g_paint_cb;
            g_pc_frames0 = g_paint_frames;
        }
        ++g_pc_built;
        snprintf(line, sizeof line, "core.nfsc: paint crowd: car %d built (look %d), list 0 %u", g_pc_built, i % 8, mp_list0());
        log_line(line);
    }
}


/* ---- diagnostics (PAINT_DIAG): what the compositor does, and each car's paint ---- */
#if PAINT_DIAG
static unsigned g_paint_told;

static void paint_compose(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t ride = get_u32(cpu->esp + 4);
    const uint32_t skin = ride ? get_u32(ride + 0x48) : 0;
    const uint32_t a = ride ? get_u32(ride + 0x1c8) : 0, b = ride ? get_u32(ride + 0x1cc) : 0,
                   c = ride ? get_u32(ride + 0x1d0) : 0, d = ride ? paint_u8(ride + 0x7f8) : 0;
    api->call_original(api, cpu->target, cpu);
    if (++g_paint_told <= 200u) {
        char line[200];
        snprintf(line, sizeof line,
                 "core.nfsc: paint compose ride %08x skin %08x (+1c8 %08x +1cc %08x +1d0 %08x +7f8 %u): %u, state %u",
                 ride, skin, a, b, c, d, cpu->eax & 0xff, get_u32(0xb76860 + 0x4));
        log_line(line);
    }
}

static void paint_output(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t self = cpu->ecx, ride = get_u32(self), to = get_u32(self + 8), from = get_u32(self + 4);
    api->call_original(api, cpu->target, cpu);
    if (++g_paint_told <= 200u) {
        char line[160];
        snprintf(line, sizeof line, "core.nfsc: paint output ride %08x: %08x into %08x%s: %u", ride, from, to,
                 paint_find(to) ? " (ours)" : "", cpu->eax & 0xff);
        log_line(line);
    }
}

/* Every 300 frames, when it changed: each render connection's id, skin and wheel hashes and what
 * GetTextureInfo finds for them (g = the game's, o = ours, - = none), and the free ids. */
static void paint_diag_frame(void) {
    if (++g_paint_frame == 1200u) { /* GetTextureInfo with and without this file's hook, 2000 calls each */
        const uint32_t args[3] = {g_paint_game_hash[1][0], 0, 0};
        uint64_t with = 0, without = 0;
        uint32_t ti = 0;
        for (int pass = 0; pass < 2; ++pass) {
            if (pass == 1) {
                g_api->hook_remove(g_api, g_paint_hooks[0]);
                g_paint_hooks[0] = 0;
            }
            const uint64_t t0 = paint_us();
            for (int i = 0; i < 2000; ++i)
                g_api->guest_call(g_api, PAINT_GET_INFO, 0, args, 3, &ti);
            *(pass ? &without : &with) = paint_us() - t0;
        }
        if (g_api->hook_install(g_api, PAINT_GET_INFO, paint_get_info, POP_HOOK_REPLACE, NULL, &g_paint_hooks[0]) != POP_OK)
            g_paint_hooks[0] = 0;
        char line[160];
        snprintf(line, sizeof line, "core.nfsc: paint: GetTextureInfo %.2f us a call with the hook, %.2f us without (2000 calls each)%s",
                 (double)with / 2000.0, (double)without / 2000.0, g_paint_hooks[0] ? "" : "; HOOK NOT BACK");
        log_line(line);
    }
    if (g_paint_frame % 300u)
        return;
    static char said[2400];
    char line[2400];
    int at = snprintf(line, sizeof line, "core.nfsc: paint cars:");
    const uint32_t list = get_u32(PAINT_RENDER_CONNS + 4), count = get_u32(PAINT_RENDER_CONNS + 0xc);
    for (uint32_t i = 0; list && i < count && i < 96 && at < (int)sizeof line - 260; ++i) {
        const uint32_t conn = get_u32(list + 4u * i), id = conn ? get_u32(conn + 0x68) : 0;
        if (!id)
            continue;
        const uint32_t ride = get_u32(conn + 0x3c), skin = ride ? get_u32(ride + 0x48) : 0, wheel = ride ? get_u32(ride + 0x4c) : 0;
        char found[2] = {'-', '-'};
        const uint32_t hs[2] = {skin, wheel};
        for (int k = 0; k < 2; ++k) {
            if (!hs[k])
                continue;
            if (paint_find(hs[k])) {
                found[k] = 'o';
                continue;
            }
            const uint32_t args[3] = {hs[k], 0, 0};
            uint32_t ti = 0;
            if (g_api->guest_call(g_api, PAINT_GET_INFO, 0, args, 3, &ti) == POP_OK && ti && get_u32(ti + 0xc) == hs[k])
                found[k] = 'g';
        }
        at += snprintf(line + at, sizeof line - at, " %u:%08x%c/%08x%c%s", id, skin, found[0], wheel, found[1],
                       ride && paint_u8(ride + 6) ? "(lite)" : "");
    }
    uint32_t free = 0;
    for (uint32_t m = get_u32(PAINT_ID_MASK); m; m &= m - 1)
        ++free;
    at += snprintf(line + at, sizeof line - at, "; ids free %u (+%u of ours); slots made %d; skins %u; GetTextureInfo %u calls, %u ours; "
                   "compositor %u steps, %llu us, worst frame %llu us",
                   free, PAINT_LAST - PAINT_GAME_IDS - g_paint_extra, g_paint_ntex / (int)PAINT_KINDS, g_paint_skins, g_paint_calls,
                   g_paint_hits, g_paint_steps, (unsigned long long)g_paint_step_us, (unsigned long long)g_paint_step_max_us);
    /* the call count changes every time: compare without it */
    const char *cut = strstr(line, "; skins");
    const size_t keep = cut ? (size_t)(cut - line) : strlen(line);
    static uint32_t skins_said;
    if (strncmp(line, said, keep) || said[keep] != ';' || g_paint_skins != skins_said) {
        skins_said = g_paint_skins;
        memcpy(said, line, keep);
        said[keep] = ';';
        said[keep + 1] = 0;
        log_line(line);
    }
}
#endif /* PAINT_DIAG */

/* Once a frame (after cFEng::Service: its BEFORE chain can be full, 16 hooks): the test crowd, the diagnostics. */
static void paint_frame(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)cpu;
    (void)inv;
    (void)user;
    ++g_paint_frames;
    { /* the longest gap between two frames while the crowd is painted (TEST crowd) */
        const uint64_t t = paint_us();
        if (g_pc_frame_last && g_pc_built > 0 && g_paint_skins - g_pc_skins_at_start < (uint32_t)g_pc_built) {
            const uint64_t gap = t - g_pc_frame_last;
            if (gap > g_pc_gap_max)
                g_pc_gap_max = gap;
            g_pc_gap_n += gap > 33333u;
        }
        g_pc_frame_last = t;
    }
    paint_crowd();
#if PAINT_DIAG
    paint_diag_frame();
#endif
}

static void paint_install(void) {
    static const char *const kind[PAINT_KINDS] = {"DUMMY_SKIN%u", "DUMMY_WHEEL%u"};
    for (uint32_t k = 0; k < PAINT_KINDS; ++k)
        for (uint32_t t = 0; t < PAINT_GAME; ++t) {
            char name[24];
            snprintf(name, sizeof name, kind[k], t + 1);
            const uint32_t h = paint_hash(name);
            g_paint_game_hash[k][t] = h;
            g_paint_game_bits[(h & 0x1fffu) >> 3] |= (uint8_t)(1u << (h & 7u));
        }
    if (g_api->hook_install(g_api, PAINT_GET_INFO, paint_get_info, POP_HOOK_REPLACE, NULL, &g_paint_hooks[0]) != POP_OK ||
        g_api->hook_install(g_api, PAINT_NAME_IDS, paint_name_ids, POP_HOOK_BEFORE, NULL, &g_paint_hooks[1]) != POP_OK) {
        log_line("core.nfsc: paint slots past 8 unavailable");
        return;
    }
    if (!g_api->hook_install_at_callsite ||
        g_api->hook_install_at_callsite(g_api, PAINT_CONN_INIT_RIDE, PAINT_CONN_INIT_RET, paint_conn_named, POP_HOOK_REPLACE,
                                        NULL, &g_paint_hooks[5]) != POP_OK ||
        g_api->hook_install(g_api, PAINT_CONN_FREE, paint_conn_free, POP_HOOK_BEFORE, NULL, &g_paint_hooks[6]) != POP_OK) {
        if (g_paint_hooks[5])
            g_api->hook_remove(g_api, g_paint_hooks[5]);
        g_paint_hooks[5] = g_paint_hooks[6] = 0;
        log_line("core.nfsc: paint: no skin ids past the game's 30");
    }
    /* TEST ONLY: RECOMP_PAINT_TEST_IDS=<n> leaves the game n of its 30 ids (the mask is a data value of
     * 0x3fffffff that only 007e6580 and 007e2fd0 touch), so paint.h's ids 31+ are reached with few cars. */
    const char *test_ids = getenv("RECOMP_PAINT_TEST_IDS");
    if (test_ids && *test_ids && atoi(test_ids) >= 0 && atoi(test_ids) < 30) {
        const uint32_t before = get_u32(PAINT_ID_MASK), n = (uint32_t)atoi(test_ids);
        mp_put(PAINT_ID_MASK, before & ((1u << n) - 1u));
        char t[120];
        snprintf(t, sizeof t, "core.nfsc: paint: TEST the game's id mask %08x -> %08x", before, get_u32(PAINT_ID_MASK));
        log_line(t);
    }
    {
        const char *tl = getenv("RECOMP_PAINT_TIMELINE");
        uint32_t id = 0;
        if (tl && atoi(tl) > 0 && g_api->hook_install(g_api, 0x00560050u, paint_tl_load, POP_HOOK_BEFORE, NULL, &id) == POP_OK) {
            g_tl_on = 1;
            log_line("core.nfsc: paint: TEST timeline of each skin (RECOMP_PAINT_TIMELINE=1)");
            static const uint32_t ev[] = {0x00560050u, 0x007e4bb0u, 0x007e3e20u, 0x007d0700u, 0x007e46c0u, 0x007e4090u, 0x007e3f30u, 0x007e6580u};
            for (unsigned i = 0; i < sizeof ev / sizeof ev[0]; ++i)
                if (g_api->hook_install(g_api, ev[i], paint_tl_event, POP_HOOK_BEFORE, (void *)(uintptr_t)ev[i], &id) != POP_OK) {
                    char t[80];
                    snprintf(t, sizeof t, "core.nfsc: paint timeline: no hook on %08x", ev[i]);
                    log_line(t);
                }
        }
    }
    {
        const char *pc = getenv("RECOMP_PAINT_PACING");
        if (pc && *pc == '0') {
            g_paint_legacy = 1;
            log_line("core.nfsc: paint: TEST Step B's compositor pacing (RECOMP_PAINT_PACING=0)");
        }
    }
    const char *budget = getenv("RECOMP_PAINT_STEP_US"), *at_once = getenv("RECOMP_PAINT_ALL_AT_ONCE");
    if (budget && *budget)
        g_paint_budget_us = (uint32_t)atoi(budget);
    if (at_once && atoi(at_once) > 0) { /* TEST ONLY: the game's own every-step-at-once switch */
        g_api->guest_write_u8(g_api, 0xb7688c, 1);
        log_line("core.nfsc: paint: TEST [0xb7688c] = 1 (each skin in one frame)");
    }
    if (g_paint_budget_us > 0 &&
        g_api->hook_install(g_api, PAINT_COMPOSE_STEP, paint_compose_step, POP_HOOK_REPLACE, NULL, &g_paint_hooks[7]) != POP_OK) {
        g_paint_hooks[7] = 0;
        log_line("core.nfsc: paint: compositor at the game's speed (one step a frame)");
    }
#if PAINT_DIAG
    {
        if (g_api->hook_install(g_api, PAINT_COMPOSE, paint_compose, POP_HOOK_REPLACE, NULL, &g_paint_hooks[2]) != POP_OK)
            g_paint_hooks[2] = 0;
        if (g_api->hook_install(g_api, PAINT_OUTPUT, paint_output, POP_HOOK_REPLACE, NULL, &g_paint_hooks[3]) != POP_OK)
            g_paint_hooks[3] = 0;
    }
#endif
    {
        const char *e = getenv("RECOMP_PAINT_CROWD");
        if ((PAINT_DIAG || (e && atoi(e) > 0)) &&
            g_api->hook_install(g_api, MP_FE_SERVICE, paint_frame, POP_HOOK_AFTER, NULL, &g_paint_hooks[4]) != POP_OK) {
            g_paint_hooks[4] = 0;
            log_line("core.nfsc: paint: no per-frame hook (cFEng::Service's chain is full?): no diagnostics or test crowd");
        }
    }
    char line[160];
    snprintf(line, sizeof line, "core.nfsc: paint slots %u-%u ready (made when first used), skin ids %u-%u, compositor %u us a frame",
             PAINT_GAME + 1, PAINT_LAST, PAINT_GAME_IDS + 1, PAINT_LAST, g_paint_budget_us);
    log_line(line);
}

/* The hooks go; the textures stay with the game (their records are in the mod heap, which the kit frees
 * when the mod unloads: only at exit, as the web build has its mods built in). */
static void paint_shutdown(void) {
    for (int i = 0; i < PAINT_HOOKS; ++i) {
        if (g_paint_hooks[i])
            g_api->hook_remove(g_api, g_paint_hooks[i]);
        g_paint_hooks[i] = 0;
    }
    g_paint_ntex = 0;
    memset(g_paint_bits, 0, sizeof g_paint_bits);
}
