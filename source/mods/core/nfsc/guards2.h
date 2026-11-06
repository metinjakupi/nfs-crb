/* guards2.h - unchecked writes that online play reaches, after Step B's lists and vector net
 * (logs/stability-20261105/RESULT.md has the whole audit: what was looked at, what is reachable, what guards it).
 * Included by nfsc.c after carlimit.h; guards2_install after carlimit_install.
 *
 * 1. THE PLAYERS REGISTRY 0x00a99248 (a fixed vector of 20, the game's template: +4 begin, +8 capacity, +0xc count,
 *    inline storage at +0x10; method table 009e248c: alloc 00692590 returns the inline buffer, free 006925a0 does
 *    nothing, grow and max 00692570 / 00692580 return 20). Every human-driven car's AIVehicleHuman (factory 004470a0,
 *    constructor 00441fd0) makes a player object of 0x29c bytes (006958b0) that pushes itself here (006811a0, the only
 *    push; reserve 00681010; removed by 00692680). mp.h builds every friend's car as driver class 6 (AIVehicleHuman),
 *    so the 20th friend's car fills it. Measured: runs/meet-a (64 bots, 50 friends' cars): count 20 of 20 and the
 *    vector net's "FULL VECTOR 00a99248 ... at push site 006811ca": without the net, the 21st player writes past the
 *    table (over 0x00a992a8..); with it, each new player replaces the 20th, so players 20.. are not in the registry
 *    (their per-frame update 006968e0 / 006962b0 skips them, the free-id search 00680260 does not see their ids).
 *    Fix: the vector gets a buffer of G2_CAP entries (as carlimit.h does for the car lists), moved in a BEFORE hook on
 *    the push 006811a0 (and again whenever the game is found to have put it back on its inline storage while it
 *    fits there), with G2_GUARDS guard words after it and the old inline storage filled with a pattern; both checked
 *    at each push and every G2_WATCH_FRAMES frames.
 * 2. A WATCH (no game state is changed): every G2_WATCH_FRAMES frames (AFTER cFEng::Service 00600c90) the fixed tables
 *    the audit found closest to their size are read, and the first time each reaches it (and every 100th time after)
 *    one line is logged: the 24-pair audio set 0x00a8e838 (full from about 61 cars; its push 005434a0 is in the vector
 *    net), the 100-entry list 0x00b7f4a0 (one entry per rigid body, +2), the race racer rows ([0x00a98284] + 0x6a08, 30),
 *    the rigid bodies ([0x00aaa248]: 96 with Step B's bodies patch, 64 without) and simple bodies ([0x00aaa250], 96),
 *    the skid-set pool ([0x00b69b7c], 128, does not grow; allocation checked by 007a65a0) and the players registry.
 * RECOMP_GUARDS2=0 turns both off (tests). */

#define G2_REG 0x00a99248u        /* players registry: method table, begin, capacity, count, 20 inline entries */
#define G2_REG_SIZE 20u
#define G2_REG_VT 0x009e248cu
#define G2_PUSH 0x006811a0u       /* cdecl (player *): pushes it, returns it */
#define G2_CAP 128u
#define G2_GUARDS 4u
#define G2_GUARD 0xc2a5c2a5u
#define G2_FILL 0xc2d0c2d0u
#define G2_WATCH_FRAMES 120u
#define G2_AUDIO_SET 0x00a8e838u  /* 24 x 8 bytes */
#define G2_BODY_LIST 0x00b7f4a0u  /* 100 */
#define G2_RACE_STATUS 0x00a98284u
#define G2_RIGID 0x00aaa248u
#define G2_SIMPLE 0x00aaa250u
#define G2_SKIDS 0x00b69b7cu      /* slot pool: +0x18 used, +0x24 count, +0x8 next chunk */
#define G2_RIGID_MOVED 0x00c40200u /* Step B's bodies patch: the record slots live here (96) */

static uint32_t g_g2_buf, g_g2_hooks[2], g_g2_frames, g_g2_bad, g_g2_moves, g_g2_peak;
static int g_g2_on;
static uint32_t g_g2_hits[8];

static void g2_line(const char *fmt, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    char line[220];
    snprintf(line, sizeof line, fmt, a, b, c, d);
    log_line(line);
}

/* Moves the registry to g_g2_buf (copying its entries); 1 when it is there. */
static int g2_move(int quiet) {
    const uint32_t vt = get_u32(G2_REG), begin = get_u32(G2_REG + 4), cap = get_u32(G2_REG + 8),
                   count = get_u32(G2_REG + 0xc), inl = G2_REG + 0x10;
    if (begin == g_g2_buf)
        return 1;
    if (vt != G2_REG_VT || (begin && begin != inl) || (cap && cap != G2_REG_SIZE) || count > G2_REG_SIZE || (!begin && count)) {
        if (++g_g2_bad <= 20u)
            g2_line("core.nfsc: guards2: players registry not moved: table %08x begin %08x capacity %u count %u", vt, begin, cap, count);
        return 0;
    }
    for (uint32_t k = 0; k < G2_CAP + G2_GUARDS; ++k)
        mp_put(g_g2_buf + 4u * k, k < count ? get_u32(begin + 4u * k) : k < G2_CAP ? 0u : G2_GUARD);
    mp_put(G2_REG + 8, G2_CAP);
    mp_put(G2_REG + 4, g_g2_buf);
    for (uint32_t k = 0; k < G2_REG_SIZE; ++k)
        mp_put(inl + 4u * k, G2_FILL);
    ++g_g2_moves;
    if (!quiet)
        g2_line("core.nfsc: guards2: players registry %08x moved: %u entries, room for %u at %08x", G2_REG, count, G2_CAP, g_g2_buf);
    return 1;
}

/* The registry on its buffer, under its capacity, guards and old storage untouched; logs (at most 50 lines) and
 * repairs the guard words / pattern. */
static void g2_check(void) {
    const uint32_t begin = get_u32(G2_REG + 4), cap = get_u32(G2_REG + 8), count = get_u32(G2_REG + 0xc);
    if (count > g_g2_peak)
        g_g2_peak = count;
    if (begin != g_g2_buf) {
        if (!g_g2_moves) { /* the first move */
            g2_move(0);
            return;
        }
        if (count <= G2_REG_SIZE && g2_move(1)) {
            if (++g_g2_bad <= 50u)
                g2_line("core.nfsc: guards2: players registry was back at %08x (capacity %u, count %u): moved again", begin, cap, count, 0);
        } else if (++g_g2_bad <= 50u) {
            g2_line("core.nfsc: guards2: PLAYERS REGISTRY LEFT ITS BUFFER: begin %08x capacity %u count %u", begin, cap, count, 0);
        }
        return;
    }
    if (cap != G2_CAP || count > G2_CAP) {
        if (++g_g2_bad <= 50u)
            g2_line("core.nfsc: guards2: PLAYERS REGISTRY capacity %u count %u", cap, count, 0, 0);
        return;
    }
    for (uint32_t k = 0; k < G2_GUARDS; ++k)
        if (get_u32(g_g2_buf + 4u * (G2_CAP + k)) != G2_GUARD) {
            if (++g_g2_bad <= 50u)
                g2_line("core.nfsc: guards2: GUARD %u AFTER THE PLAYERS REGISTRY OVERWRITTEN: %08x (count %u)", k,
                        get_u32(g_g2_buf + 4u * (G2_CAP + k)), count, 0);
            mp_put(g_g2_buf + 4u * (G2_CAP + k), G2_GUARD);
        }
    for (uint32_t k = 0; k < G2_REG_SIZE; ++k)
        if (get_u32(G2_REG + 0x10 + 4u * k) != G2_FILL) {
            if (++g_g2_bad <= 50u)
                g2_line("core.nfsc: guards2: OLD STORAGE OF THE PLAYERS REGISTRY WRITTEN: entry %u = %08x", k,
                        get_u32(G2_REG + 0x10 + 4u * k), 0, 0);
            mp_put(G2_REG + 0x10 + 4u * k, G2_FILL);
        }
}

/* BEFORE 006811a0 (the registry's only push): moved before the push writes. */
static void g2_push(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)cpu;
    (void)inv;
    (void)user;
    if (g_g2_on)
        g2_check();
}

/* A fixed table at (or over) its size: the first time and every 100th. */
static void g2_watch_one(int i, const char *what, uint32_t n, uint32_t size) {
    if (n < size) {
        if (g_g2_hits[i] & 0x80000000u)
            g_g2_hits[i] &= 0x7fffffffu; /* below again: the next time it fills is logged too */
        return;
    }
    if (g_g2_hits[i] & 0x80000000u)
        return;
    const uint32_t k = ++g_g2_hits[i];
    g_g2_hits[i] |= 0x80000000u;
    if (k <= 3u || k % 100u == 0) {
        char line[200];
        snprintf(line, sizeof line, "core.nfsc: guards2: watch: %s at %u of %u (%u times so far)", what, n, size, k);
        log_line(line);
    }
}

static uint32_t g2_pool_total(uint32_t pool) {
    uint32_t n = 0;
    for (uint32_t c = pool, i = 0; c && i < 64u; c = get_u32(c + 8), ++i)
        n += get_u32(c + 0x24);
    return n;
}

static void g2_frame(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)cpu;
    (void)inv;
    (void)user;
    if (!g_g2_on || ++g_g2_frames % G2_WATCH_FRAMES)
        return;
    g2_check();
    g2_watch_one(0, "players registry 00a99248", get_u32(G2_REG + 0xc), get_u32(G2_REG + 8));
    g2_watch_one(1, "audio set 00a8e838 (fixed, in the vector net)", get_u32(G2_AUDIO_SET + 0xc), get_u32(G2_AUDIO_SET + 8));
    g2_watch_one(2, "body list 00b7f4a0 (fixed, in the vector net)", get_u32(G2_BODY_LIST + 0xc), 100u);
    const uint32_t rs = get_u32(G2_RACE_STATUS);
    if (rs)
        g2_watch_one(3, "race racer rows", get_u32(rs + 0x6a08), 30u);
    const uint32_t rigid_max = get_u32(G2_RIGID_MOVED) ? 96u : 64u;
    g2_watch_one(4, "rigid bodies", get_u32(G2_RIGID), rigid_max);
    g2_watch_one(5, "simple bodies", get_u32(G2_SIMPLE), 96u);
    const uint32_t skids = get_u32(G2_SKIDS);
    if (skids)
        g2_watch_one(6, "skid-set pool (allocation checked)", get_u32(skids + 0x18), g2_pool_total(skids));
}

static void guards2_install(void) {
    const char *e = getenv("RECOMP_GUARDS2");
    if (e && *e && atoi(e) == 0) {
        log_line("core.nfsc: guards2 off (RECOMP_GUARDS2=0)");
        return;
    }
    if (g_api->guest_alloc(g_api, (G2_CAP + G2_GUARDS) * 4u, &g_g2_buf) != POP_OK) {
        log_line("core.nfsc: guards2: no guest memory: the players registry keeps its 20 (the vector net still guards it)");
        return;
    }
    const PopModStatus a = g_api->hook_install(g_api, G2_PUSH, g2_push, POP_HOOK_BEFORE, NULL, &g_g2_hooks[0]);
    const PopModStatus b = g_api->hook_install(g_api, MP_FE_SERVICE, g2_frame, POP_HOOK_AFTER, NULL, &g_g2_hooks[1]);
    if (a != POP_OK)
        g_g2_hooks[0] = 0;
    if (b != POP_OK)
        g_g2_hooks[1] = 0;
    if (a != POP_OK) {
        g2_line("core.nfsc: guards2: hooks unavailable (push %d, frame %d): the players registry keeps its 20", (uint32_t)a, (uint32_t)b, 0, 0);
        if (g_g2_hooks[1])
            g_api->hook_remove(g_api, g_g2_hooks[1]);
        g_g2_hooks[1] = 0;
        return;
    }
    g_g2_on = 1;
    /* The registry is a static object whose constructor (009bf8f0) may not have run yet: it is moved at its first
     * push or the first watch, whichever comes first. */
    g2_line("core.nfsc: guards2 on: players registry %u -> %u entries, watch every %u frames", G2_REG_SIZE, G2_CAP,
            G2_WATCH_FRAMES, 0);
}

static void guards2_shutdown(void) {
    for (int i = 0; i < 2; ++i)
        if (g_g2_hooks[i]) {
            g_api->hook_remove(g_api, g_g2_hooks[i]);
            g_g2_hooks[i] = 0;
        }
    g_g2_on = 0;
}
