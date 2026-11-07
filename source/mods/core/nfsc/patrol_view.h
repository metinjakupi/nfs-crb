/* patrol_view.h - the game's own police cars are not taken away while they are on this player's screen
 * (included by nfsc.c after mp.h and cops.h; logs/patrol-view-20261106).
 *
 * HOW CARBON MOVES ITS PATROLS. The cop manager's update (004463c0) runs 0043a9d0 every frame. It counts the cars
 * whose goal is AIGoalPatrol, asks for a new patrol (0043a260) while there are fewer than the game wants, and then
 * walks its cops (list at +0xbc, count +0xc4). For each cop it asks the zone test 0041dc20 (thiscall on the cop
 * manager + 0x40, args: the cop's position, 0; RET 8), whose only caller this is. The zone test answers 1 ("outside")
 * when the cop is farther from the camera than (inner + outer) / 2 (the floats at +4 and +8 of that block), or farther
 * than inner and outside a cone of the camera's forward direction: dot(direction to the cop, camera forward) below 0.8
 * (37 degrees off the camera's axis) and above -0.707 (more than 45 degrees off straight behind). The camera is the
 * game's player view (0x00b4afd0 +0x40 position, +0x50 forward, 0040d7d0 / 0040d840). The cop's AI then decides
 * (IVehicleAI +0x9c, 0040a0f0): a cop that has been inside once (AI +0x90) goes when it is outside and older than 8 s
 * (AI +0x8c), one that never was when older than 10 s. A patrol that goes is released (004136a0: out of its pursuit,
 * IVehicleAI +0x98 004186b0 = the car is taken out of the world, the patrol count +0x90 down by one); the next frames'
 * 0043a260 asks the cop factory (0042d2a0) for a car, gets the same car back and puts it on a road spawn point
 * (00438740, IVehicleAI +0x58 = 00427ba0). That is the "patrol moved 100-300 m every ~10 s, same address" of
 * logs/traffic-stable-20261105, and the patrols that vanished on screen at 188-442 m in logs/no-vanish-20261106:
 * the game's cone is 74 degrees wide, but the widescreen view (Hor+) shows far more to the sides, so a patrol at the
 * side of the picture is "outside" and goes in plain view, and beyond (inner + outer) / 2 it goes even dead ahead.
 * A chasing cop goes the same way only when it is also more than 375 m (600 m for one model) from where its target
 * will be in 75 s; a wrecked one (IVehicle +0x90) after 8-10 s when farther than 50 m and still moving; every cop when
 * the game turns the police off (cop manager + 0x4c, +4).
 *
 * THE CHANGE. The zone test answers "inside" (0) for a cop that is on this player's screen (mp_seen_at: projected with
 * the main view's matrix, as the name tags; or within 25 m of our car) within PV_RANGE of our car; otherwise the game's
 * own answer. So a patrol is kept while it is shown, and goes (and comes back elsewhere) once it is out of view, as the
 * game meant. The patrol count, the spawner, chases, heat, roadblocks and wrecked cops are untouched; a patrol that was
 * seen goes 8 s after it leaves the view at the earliest, as in the game.
 * Diagnostics: every release from 0043a9d0 is logged when it happens on screen ("patrol view: released on screen"),
 * and a summary each PV_NOTE_MS. */
#define PV_ZONE_TEST 0x0041dc20u   /* thiscall (zone block, const float pos[3], 0), AL 1 = outside, RET 8 */
#define PV_RELEASE 0x004136a0u     /* thiscall (cop manager, IVehicle *), RET 4 */
#define PV_RELEASE_RET 0x0043afb1u /* after its call in 0043a9d0 */
#define PV_RANGE 600.0f            /* metres from our car */
#define PV_NOTE_MS 60000.0
#ifndef PV_KEEP
#define PV_KEEP 1 /* 0: diagnostics only (the control build of logs/patrol-view-20261106) */
#endif

static uint32_t g_pv_hooks[2];
static unsigned g_pv_asked, g_pv_kept, g_pv_released, g_pv_released_seen;
static double g_pv_note_at;
static uint32_t g_pv_zone_inner, g_pv_zone_outer; /* the zone last logged */

/* Whether p is on our screen within PV_RANGE (1), not (0), or nothing to tell (-1: no car of ours). */
static int pv_shown(const float p[3], float *dist) {
    float here[3];
    if (!mp_here(here))
        return -1;
    float d2 = 0;
    for (int k = 0; k < 3; ++k)
        d2 += (p[k] - here[k]) * (p[k] - here[k]);
    *dist = sqrtf(d2);
    return d2 <= PV_RANGE * PV_RANGE && mp_seen_at(p, here);
}

static void pv_note(void) {
    const double now = mp_now();
    if (g_pv_note_at > 0 && now - g_pv_note_at < PV_NOTE_MS)
        return;
    if (g_pv_note_at > 0 && g_pv_asked) {
        char line[200];
        snprintf(line, sizeof line,
                 "core.nfsc: patrol view: %u zone tests, %u outside but on screen (%s), %u cops released (%u on screen)",
                 g_pv_asked, g_pv_kept, PV_KEEP ? "kept" : "not kept: control", g_pv_released, g_pv_released_seen);
        log_line(line);
    }
    g_pv_note_at = now;
    g_pv_asked = g_pv_kept = g_pv_released = g_pv_released_seen = 0;
}

/* REPLACE 0041dc20: the game's answer, but "inside" for a cop on our screen. */
static void pv_zone(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t zone = cpu->ecx, pos = get_u32(cpu->esp + 4);
    float p[3] = {0, 0, 0};
    for (int k = 0; pos && k < 3; ++k)
        p[k] = bits_float(get_u32(pos + 4u * k));
    api->call_original(api, cpu->target, cpu);
    ++g_pv_asked;
    if (zone && (get_u32(zone + 4) != g_pv_zone_inner || get_u32(zone + 8) != g_pv_zone_outer)) {
        g_pv_zone_inner = get_u32(zone + 4);
        g_pv_zone_outer = get_u32(zone + 8);
        char line[128];
        snprintf(line, sizeof line, "core.nfsc: patrol view: the game's zone: inner %.0f m, outer %.0f m",
                 bits_float(get_u32(zone + 4)), bits_float(get_u32(zone + 8)));
        log_line(line);
    }
    float d = 0;
    if (pos && (cpu->eax & 0xffu) && pv_shown(p, &d) == 1) {
        ++g_pv_kept;
        if (PV_KEEP)
            cpu->eax &= ~0xffu;
    }
    pv_note();
}

/* BEFORE 004136a0 from 0043a9d0 (diagnostic): a cop released, where. */
static void pv_release(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)inv;
    (void)user;
    const uint32_t iveh = get_u32(cpu->esp + 4), car = iveh ? iveh - 0xb8u : 0;
    const uint32_t st = car ? mp_state(get_u32(car + 0x70)) : 0;
    ++g_pv_released;
    if (!st)
        return;
    float p[3], d = 0;
    for (int k = 0; k < 3; ++k)
        p[k] = bits_float(get_u32(st + 0x10 + 4u * k));
    float here[3];
    if (!mp_here(here))
        return;
    for (int k = 0; k < 3; ++k)
        d += (p[k] - here[k]) * (p[k] - here[k]);
    d = sqrtf(d);
    if (!mp_seen_at(p, here))
        return;
    ++g_pv_released_seen;
    const uint32_t ai = get_u32(car + 0x104);
    const float speed = bits_float(get_u32(iveh + 0xe0));
    char line[200];
    snprintf(line, sizeof line,
             "core.nfsc: patrol view: released on screen at %.0f m (speed %.1f m/s, age %.1f s, pursuit %s)", d, speed,
             ai ? bits_float(get_u32(ai + 0x8c)) : -1.0f, ai && get_u32(ai + 0x98) ? "yes" : "no");
    log_line(line);
}

static void patrol_view_install(void) {
    if (g_api->hook_install(g_api, PV_ZONE_TEST, pv_zone, POP_HOOK_REPLACE, NULL, &g_pv_hooks[0]) != POP_OK) {
        g_pv_hooks[0] = 0;
        log_line("core.nfsc: patrol view unavailable (could not hook the cop zone test)");
        return;
    }
    if (!g_api->hook_install_at_callsite ||
        g_api->hook_install_at_callsite(g_api, PV_RELEASE, PV_RELEASE_RET, pv_release, POP_HOOK_BEFORE, NULL,
                                        &g_pv_hooks[1]) != POP_OK)
        g_pv_hooks[1] = 0;
    log_line(PV_KEEP ? "core.nfsc: patrol view: police cars on screen are kept" : "core.nfsc: patrol view: diagnostics only");
}

static void patrol_view_shutdown(void) {
    for (int i = 0; i < 2; ++i)
        if (g_pv_hooks[i]) {
            g_api->hook_remove(g_api, g_pv_hooks[i]);
            g_pv_hooks[i] = 0;
        }
}
