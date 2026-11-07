/* reflect.h - the car reflection cube in turns when many cars are near (goal item 3, 2026-11-05).
 *
 * The game's cube update 007218a0 (thiscall, the cube's six face views at [this + 4 * i]; cdecl args: two positions)
 * runs once a frame. It picks a row of face flags from the setting g_CarEnvironmentMapEnable [0x00a65340] (1 Low,
 * 2 Medium, 3 High) and the frame counter [0x00ab0a78]:
 *     phase = frame % period[setting]   (periods at 0x00a63b18: -, 6, 4, 1)
 *     row   = 0x00a63b28 + ((setting - 1) * 6 + phase) * 24, one dword per face; a face is redrawn when its flag is set
 *     Low:    one face a frame, each face every 6th frame
 *     Medium: faces 3 and 5 every frame, faces 0, 1, 2, 4 in turn (3 faces a frame)
 *     High:   all six faces every frame
 * Nothing else reads the two tables (generated code: 00721a78 and 00721a99 only).
 *
 * What a face draws (logs/reflect-mem-20261105, RECOMP_D3D9_STATS): about 40 draws a face, the world around the
 * player, and the same with 0 or 49 friends' cars near (High: 243-274 cube draws a frame alone and in the meet). The
 * cars are not drawn into the cube, so there is no car cap to put on it; the cube is a fixed cost per face.
 *
 * Here: with many vehicles near the player (RF_ENTER within RF_NEAR_M), or the frame slow (under RF_SLOW_FPS for
 * RF_SLOW_S seconds with at least RF_SLOW_MIN near), the faces are redrawn in turns:
 *     High   -> the game's own Medium schedule (3 a frame: faces 3 and 5 every frame, the other four every 4th)
 *     Medium -> 2 a frame (faces 3 and 5 every other frame, the other four every 4th, as in Medium)
 *     Low    -> unchanged
 * by writing the setting's period and rows; back to the game's own values (saved at the first call) when fewer than
 * RF_LEAVE are near for RF_LEAVE_S seconds. Alone (and in races with a few AI) nothing changes.
 *
 * Page block (globalThis.carbonReflectBlock, 16 dwords): magic 'RFL1', setting, turns (0/1), vehicles near,
 * faces a frame now, A/B phase (test), calls, switches made, fps x 10.
 * Switches (URL query, as getenv): RECOMP_REFLECT=0 off (the game's own always); RECOMP_REFLECT_CARS=<n> (RF_ENTER);
 * TEST ONLY: RECOMP_REFLECT_LEVEL=<1..3> sets the setting at the first call; RECOMP_REFLECT_AB=<s> alternates the
 * game's own and turns every s seconds whatever the crowd (A/B at the same load); RECOMP_REFLECT_FORCE=1 turns always.
 */
#define RF_FN 0x007218a0u
#define RF_SETTING 0x00a65340u
#define RF_PERIODS 0x00a63b18u
#define RF_ROWS 0x00a63b28u
#define RF_CAR_LIST 0x00a9e728u /* list 0's car list (probe.js): node = car + 0xd0 */
#define RF_ENTER 20             /* vehicles near: turns on */
#define RF_LEAVE 14             /* fewer than this for RF_LEAVE_S: the game's own again */
#define RF_LEAVE_S 3.0
#define RF_NEAR_M 120.0f
#define RF_SLOW_FPS 50.0
#define RF_SLOW_S 3.0
#define RF_SLOW_MIN 8
#define RF_CHECK_MS 500.0

static struct {
    uint32_t magic, setting, turns, near, faces, ab, calls, switches, fps10, pad[7];
} g_rf;
static uint32_t g_rf_hook;
static int g_rf_on = 1, g_rf_enter = RF_ENTER, g_rf_level, g_rf_ab_s, g_rf_force;
static int g_rf_saved, g_rf_applied = -1, g_rf_applied_setting;
static uint32_t g_rf_orig_periods[4], g_rf_orig_rows[3 * 6 * 6];
static double g_rf_last_check, g_rf_ab_at, g_rf_slow_since, g_rf_low_since, g_rf_sec_at;
static uint32_t g_rf_sec_frames;
static int g_rf_by_slow;
static uint32_t g_rf_ab_frames; /* A/B: frames of the phase so far */

/* Turns: [setting - 1][phase][face] and the period. Only Medium and High change. */
static const uint32_t RF_TURNS_PERIOD[4] = {0, 6, 4, 4};
static const uint8_t RF_TURNS_ROWS[3][6][6] = {
    {{1, 0, 0, 0, 0, 0}, {0, 1, 0, 0, 0, 0}, {0, 0, 1, 0, 0, 0}, {0, 0, 0, 1, 0, 0}, {0, 0, 0, 0, 1, 0}, {0, 0, 0, 0, 0, 1}},
    {{1, 0, 0, 1, 0, 0}, {0, 1, 0, 0, 0, 1}, {0, 0, 1, 1, 0, 0}, {0, 0, 0, 0, 1, 1}, {0}, {0}},
    {{1, 0, 0, 1, 0, 1}, {0, 1, 0, 1, 0, 1}, {0, 0, 1, 1, 0, 1}, {0, 0, 0, 1, 1, 1}, {0}, {0}},
};

static int rf_env(const char *name, int fallback) {
    const char *e = getenv(name);
    return e && *e ? atoi(e) : fallback;
}

static float rf_f(uint32_t a) { return bits_float(get_u32(a)); }

/* The position of a car's rigid body (probe.js): body [car + 0x70], its state [[body + 0x30]], x y z at +0x10. */
static int rf_pos(uint32_t car, float *p) {
    const uint32_t body = get_u32(car + 0x70), s = body ? get_u32(get_u32(body + 0x30)) : 0;
    if (!s)
        return 0;
    p[0] = rf_f(s + 0x10), p[1] = rf_f(s + 0x14), p[2] = rf_f(s + 0x18);
    return p[0] == p[0] && p[1] == p[1] && p[2] == p[2];
}

/* Vehicles within RF_NEAR_M of the player's car (class 0 with a driver, as probe.js), the player not counted. */
static uint32_t rf_near(void) {
    uint32_t me = 0, n = 0, i = 0;
    for (uint32_t node = get_u32(RF_CAR_LIST); node && node != RF_CAR_LIST && i++ < 256u; node = get_u32(node)) {
        const uint32_t c = node - 0xd0u;
        if (get_u32(c + 0x148) == 0 && get_u32(c + 0x7c)) { me = c; break; }
    }
    float m[3];
    if (!me || !rf_pos(me, m))
        return 0;
    i = 0;
    for (uint32_t node = get_u32(RF_CAR_LIST); node && node != RF_CAR_LIST && i++ < 256u; node = get_u32(node)) {
        const uint32_t c = node - 0xd0u;
        float p[3];
        if (c == me || !rf_pos(c, p))
            continue;
        const float dx = p[0] - m[0], dy = p[1] - m[1], dz = p[2] - m[2];
        n += dx * dx + dy * dy + dz * dz < RF_NEAR_M * RF_NEAR_M;
    }
    return n;
}

static void rf_apply(uint32_t setting, int turns) {
    if (setting < 1u || setting > 3u)
        return;
    if (g_rf_applied == turns && g_rf_applied_setting == (int)setting)
        return;
    /* A setting changed while turns were on: the old setting's rows go back first. */
    if (g_rf_applied == 1 && g_rf_applied_setting != (int)setting && g_rf_applied_setting >= 1 && g_rf_applied_setting <= 3) {
        const uint32_t o = (uint32_t)g_rf_applied_setting;
        g_api->guest_write_u32(g_api, RF_PERIODS + 4u * o, g_rf_orig_periods[o]);
        for (uint32_t k = 0; k < 36u; ++k)
            g_api->guest_write_u32(g_api, RF_ROWS + ((o - 1u) * 36u + k) * 4u, g_rf_orig_rows[(o - 1u) * 36u + k]);
    }
    g_api->guest_write_u32(g_api, RF_PERIODS + 4u * setting, turns ? RF_TURNS_PERIOD[setting] : g_rf_orig_periods[setting]);
    for (uint32_t k = 0; k < 36u; ++k)
        g_api->guest_write_u32(g_api, RF_ROWS + ((setting - 1u) * 36u + k) * 4u,
                               turns ? RF_TURNS_ROWS[setting - 1u][k / 6u][k % 6u] : g_rf_orig_rows[(setting - 1u) * 36u + k]);
    if (g_rf_applied != -1 || turns)
        ++g_rf.switches;
    g_rf_applied = turns;
    g_rf_applied_setting = (int)setting;
}

static uint32_t rf_faces_now(uint32_t setting) {
    if (setting < 1u || setting > 3u)
        return 0;
    const uint32_t period = get_u32(RF_PERIODS + 4u * setting), frame = get_u32(0x00ab0a78u);
    const uint32_t phase = period ? frame % period : 0, row = RF_ROWS + ((setting - 1u) * 6u + phase) * 24u;
    uint32_t n = 0;
    for (uint32_t f = 0; f < 6u; ++f)
        n += get_u32(row + 4u * f) != 0;
    return n;
}

static void rf_hook(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api, (void)cpu, (void)inv, (void)user;
    const double now = mp_now();
    ++g_rf.calls;
    ++g_rf_ab_frames;
    if (!g_rf_saved) {
        for (uint32_t k = 0; k < 4u; ++k)
            g_rf_orig_periods[k] = get_u32(RF_PERIODS + 4u * k);
        for (uint32_t k = 0; k < 3u * 36u; ++k)
            g_rf_orig_rows[k] = get_u32(RF_ROWS + 4u * k);
        g_rf_saved = 1;
        g_rf_ab_at = g_rf_sec_at = g_rf_last_check = now;
        if (g_rf_level >= 1 && g_rf_level <= 3)
            g_api->guest_write_u32(g_api, RF_SETTING, (uint32_t)g_rf_level);
        char line[200];
        snprintf(line, sizeof line, "core.nfsc: reflect: setting %u, periods %u/%u/%u (enter %d near within %.0f m, test level %d, A/B %d s)",
                 get_u32(RF_SETTING), g_rf_orig_periods[1], g_rf_orig_periods[2], g_rf_orig_periods[3], g_rf_enter,
                 (double)RF_NEAR_M, g_rf_level, g_rf_ab_s);
        log_line(line);
    }
    /* Frame rate from this once-a-frame call (a gap over 1 s is a load or a pause: restart the second). */
    ++g_rf_sec_frames;
    if (now - g_rf_sec_at >= 1000.0) {
        const double fps = now - g_rf_sec_at > 2000.0 ? 60.0 : g_rf_sec_frames * 1000.0 / (now - g_rf_sec_at);
        g_rf.fps10 = (uint32_t)(fps * 10.0);
        g_rf_sec_at = now;
        g_rf_sec_frames = 0;
    }
    const uint32_t setting = get_u32(RF_SETTING);
    g_rf.setting = setting;
    if (now - g_rf_last_check >= RF_CHECK_MS) {
        g_rf_last_check = now;
        g_rf.near = rf_near();
        int turns = g_rf.turns;
        if (g_rf_ab_s > 0) {
            if (now - g_rf_ab_at >= g_rf_ab_s * 1000.0) {
                char line[200];
                snprintf(line, sizeof line, "core.nfsc: reflect: A/B phase %s done: %u frames in %.0f ms = %.2f fps (%u near, setting %u); now %s",
                         g_rf.ab ? "turns" : "own", g_rf_ab_frames, now - g_rf_ab_at, g_rf_ab_frames * 1000.0 / (now - g_rf_ab_at),
                         g_rf.near, setting, g_rf.ab ? "own" : "turns");
                log_line(line);
                g_rf_ab_at = now;
                g_rf_ab_frames = 0;
                g_rf.ab = !g_rf.ab;
            }
            turns = (int)g_rf.ab;
        } else if (!g_rf_on) {
            turns = 0;
        } else if (g_rf_force) {
            turns = 1;
        } else {
            const double fps = g_rf.fps10 / 10.0;
            const int n = (int)g_rf.near;
            if (fps < RF_SLOW_FPS && n >= RF_SLOW_MIN) {
                if (!g_rf_slow_since) g_rf_slow_since = now;
            } else
                g_rf_slow_since = 0;
            const int slow = g_rf_slow_since && now - g_rf_slow_since >= RF_SLOW_S * 1000.0;
            if (!turns) {
                if (n >= g_rf_enter || slow) {
                    turns = 1;
                    g_rf_by_slow = n < g_rf_enter;
                    g_rf_low_since = 0;
                }
            } else {
                const int keep = g_rf_by_slow ? n >= RF_SLOW_MIN : n >= RF_LEAVE;
                if (keep)
                    g_rf_low_since = 0;
                else if (!g_rf_low_since)
                    g_rf_low_since = now;
                else if (now - g_rf_low_since >= RF_LEAVE_S * 1000.0)
                    turns = 0;
                if (g_rf_by_slow && n >= g_rf_enter)
                    g_rf_by_slow = 0;
            }
            if (turns != (int)g_rf.turns) {
                char line[160];
                snprintf(line, sizeof line, "core.nfsc: reflect: %s (%u near, %.1f fps, setting %u)",
                         turns ? (g_rf_by_slow ? "in turns (slow frames)" : "in turns (many cars near)") : "the game's own again",
                         g_rf.near, fps, setting);
                log_line(line);
            }
        }
        g_rf.turns = (uint32_t)turns;
    }
    rf_apply(setting, (int)g_rf.turns);
    g_rf.faces = rf_faces_now(setting);
}

static void reflect_install(void) {
    g_rf.magic = 0x314c4652u; /* 'RFL1' */
    g_rf_on = rf_env("RECOMP_REFLECT", 1) != 0;
    g_rf_enter = rf_env("RECOMP_REFLECT_CARS", RF_ENTER);
    g_rf_level = rf_env("RECOMP_REFLECT_LEVEL", 0);
    g_rf_ab_s = rf_env("RECOMP_REFLECT_AB", 0);
    g_rf_force = rf_env("RECOMP_REFLECT_FORCE", 0);
#ifdef __EMSCRIPTEN__
    MAIN_THREAD_ASYNC_EM_ASM({var b={};b.memory=wasmMemory;b.ptr=$0;globalThis.carbonReflectBlock=b;}, (uint32_t)(uintptr_t)&g_rf);
#endif
    if (!g_rf_on && !g_rf_level) {
        log_line("core.nfsc: reflect off (RECOMP_REFLECT=0): the game's own cube schedule");
        return;
    }
    if (!g_rf_on) /* only the test level: the game's own schedule, the hook sets the level and counts */
        g_rf_ab_s = 0, g_rf_force = 0;
    if (g_api->hook_install(g_api, RF_FN, rf_hook, POP_HOOK_BEFORE, NULL, &g_rf_hook) != POP_OK) {
        g_rf_hook = 0;
        log_line("core.nfsc: reflect: hook unavailable: the game's own cube schedule");
    }
}

static void reflect_shutdown(void) {
    if (g_rf_hook) {
        if (g_rf_saved && g_rf_applied == 1)
            rf_apply((uint32_t)g_rf_applied_setting, 0);
        g_api->hook_remove(g_api, g_rf_hook);
        g_rf_hook = 0;
    }
}
