/* Less traffic by default (the user's decision, 2026-11-04): half the traffic everywhere - free
 * roam, career events and Quick Race - with no setting to turn it back.
 *
 * 0x00422cc0 (fastcall, the traffic manager in ECX; returns a float in ST0) is the one place the
 * game works out how dense traffic is, as a fraction 0..1: in a race the race's density / 100
 * (Quick Race Low/Medium/High = 3/10/50 through the setter 006262b0, career events from their
 * own race data, read in Game_StartRace 00655f70 into [game+0xbc9c]); in free roam a fixed 1.0
 * (009c2478); none in some modes. Its only caller, the traffic manager's update 00445080, spawns
 * cars at a rate proportional to it and lets a car go when it is above that share (00412d70), so
 * a halved fraction means half the cars. Traffic cars are a large share of a busy frame's draws
 * on slow machines, and a car that is not there costs nothing. Each player's traffic is their
 * own, so players online need not agree on it.
 *
 * A race together, Drift and Canyon Drift have none at all (traffic_none).
 *
 * Until 2026-11-04 afternoon this halved the Quick Race setter's value instead, which left free
 * roam and career events at the game's own density.
 *
 * SHARED TRAFFIC (cops.h, plans/shared-traffic.md): while this game shows a nearer friend's
 * traffic, it spawns none of its own. The traffic manager's update 00445080 spawns a car
 * (0042c950) only when the check 00444d90 (thiscall on the manager, no arguments, plain RET)
 * answers 0, so that check answers 1 then (REPLACE). NFSCO replaces the same check.
 *
 * FEWER WITH FRIENDS (Step A, 2026-11-05). The game holds 30 cars (mp.h ROOM) and free-roam traffic
 * took 16 of them (14 traffic cars and 2 trucks' trailers, crowd test 2026-11-05), so while a friend's
 * car is within TRAFFIC_FRIEND of ours the same check also answers 1 once this game has TRAFFIC_CAP
 * traffic cars (driver class 1; trailers come with their trucks and are not counted). The cars there
 * already stay until the game lets them go. Alone, traffic is as before. With shared traffic (cops.h)
 * the owner's capped traffic is what its friends see; a quiet game spawns none at all, as before. */
#define TRAFFIC_DENSITY_FN 0x00422cc0u
#define TRAFFIC_SPAWN_CHECK 0x00444d90u
#define TRAFFIC_CAP 8
#define TRAFFIC_FRIEND TRF_FRIEND /* metres, as cops.h shares its traffic (300) */

static uint32_t g_traffic_hook, g_traffic_spawn_hook;

/* Whether a friend's car (built or only its name tag, by its newest pose) is within TRAFFIC_FRIEND. */
static int traffic_friend_near(void) {
    const uint32_t own = g_mp_player_simable ? mp_state(get_u32(g_mp_player_simable - 0x24u + 0x70)) : 0;
    for (int i = 0; own && i < MP_SLOTS; ++i) {
        const MpCar *c = &g_mp_remote[i];
        if (!c->id || !c->kept)
            continue;
        float d = 0;
        for (int k = 0; k < 3; ++k) {
            const float x = c->pose[c->kept - 1].p[k] - bits_float(get_u32(own + 0x10 + 4u * k));
            d += x * x;
        }
        if (d <= TRAFFIC_FRIEND * TRAFFIC_FRIEND)
            return 1;
    }
    return 0;
}

static void traffic_spawn_check(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    static int was = -1;
    static uint32_t denied;
    const int near = traffic_friend_near(), cars = near ? mp_traffic_count(NULL) : 0;
    const int capped = near && cars >= TRAFFIC_CAP;
    if (near != was) {
        char line[112];
        snprintf(line, sizeof line, near ? "core.nfsc: traffic: a friend is near: at most %d traffic cars of ours (%d now)"
                                         : "core.nfsc: traffic: no friend near: our own traffic as the game makes it",
                 TRAFFIC_CAP, cars);
        log_line(line);
        was = near;
    }
    if (capped && (++denied <= 3u || denied % 1000u == 0)) {
        char line[96];
        snprintf(line, sizeof line, "core.nfsc: traffic: no new traffic car (%d of ours, a friend near): %u so far", cars, denied);
        log_line(line);
    }
    if (cop_traffic_quiet() || capped)
        api->hook_return(api, cpu, (cpu->eax & 0xffffff00u) | 1u, 0);
    else
        api->call_original(api, cpu->target, cpu);
}

/* Whether this race has no traffic at all: a race together, because each player's traffic exists
 * in their own game alone and a car only one driver can see has no place in a shared race; and
 * Drift (11) and Canyon Drift (12), roads meant for one car. The Quick Race options apply their
 * traffic level (007a9dc0) to every race type, so these got it too (seen 2026-11-04: High grew
 * four traffic cars in 13 s). */
static int traffic_none(uint32_t rs) {
    static uint32_t params, type;
    const uint32_t now = rs ? get_u32(rs + 0x6a1cu) : 0;
    if (now != params) {
        params = now;
        type = now ? mp_call(0x006136a0u, now, NULL, 0) : 0;
    }
    return (g_race_session && g_race_session == g_race_page.session) || type == 11u || type == 12u;
}

static void traffic_density_less(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv,
                                 void *user) {
    (void)api; (void)inv; (void)user;
    double *st0 = &cpu->st[cpu->fpu_top & 7u];
    const double asked = *st0;
    if (!(asked > 0.0))
        return; /* none (or not a number): the game's own */
    const uint32_t rs = get_u32(0x00a98284u);
    if (rs && traffic_none(rs)) {
        /* The stored density, as the options' own None leaves it: a value changed in ST0 here does
         * not reach the caller's check at 00445090 (traffic came at the same rate with 0.0 as
         * without), so this frame's update may still spawn, and the next reads 0. */
        mp_put(rs + 0xbc9cu, 0);
        return;
    }
    *st0 = asked * 0.5;
    static int said = -1; /* each new density once, so a log shows the hook at work */
    const int pct = (int)(asked * 100.0 + 0.5);
    if (pct != said) {
        char line[96];
        snprintf(line, sizeof line, "core.nfsc: traffic density %d%% -> %d%%", pct,
                 (int)(asked * 50.0 + 0.5));
        log_line(line);
        said = pct;
    }
}

static void traffic_install(void) {
    if (g_api->hook_install(g_api, TRAFFIC_DENSITY_FN, traffic_density_less, POP_HOOK_AFTER, NULL,
                            &g_traffic_hook) != POP_OK) {
        g_traffic_hook = 0;
        log_line("core.nfsc: less traffic unavailable (could not hook the traffic density)");
        return;
    }
    log_line("core.nfsc: less traffic: traffic density halved (free roam, career, Quick Race)");
    if (g_api->hook_install(g_api, TRAFFIC_SPAWN_CHECK, traffic_spawn_check, POP_HOOK_REPLACE, NULL,
                            &g_traffic_spawn_hook) != POP_OK) {
        g_traffic_spawn_hook = 0;
        log_line("core.nfsc: shared traffic unavailable (could not hook the traffic spawn check)");
    }
}

static void traffic_shutdown(void) {
    if (g_traffic_hook) {
        g_api->hook_remove(g_api, g_traffic_hook);
        g_traffic_hook = 0;
    }
    if (g_traffic_spawn_hook) {
        g_api->hook_remove(g_api, g_traffic_spawn_hook);
        g_traffic_spawn_hook = 0;
    }
}
