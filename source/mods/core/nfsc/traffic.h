/* Less traffic by default (the user's decision, 2026-11-04): the traffic density a race asks for
 * is halved, so Low/Medium/High become 2/5/25 instead of 3/10/50, and none stays none.
 *
 * 0x006262b0 is the race parameters' traffic-density setter (thiscall, one int, RET 4): it clamps
 * to 0..100 and stores race attribute 0xc64bc341. Its callers are the quick race options
 * (007a9dc0: the Traffic setting as 3/10/50, the values NFSC Extra Options also patches) and the
 * online race setup (0075e3b0). Traffic cars are a large share of a busy frame's draws on slow
 * machines, and a car that is not there costs nothing. Career events read their density from the
 * game's own data and are unchanged. Each player's traffic is their own, so players online need
 * not agree on it. */
#define TRAFFIC_SET_DENSITY 0x006262b0u

static uint32_t g_traffic_hook;

static void traffic_density_less(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv,
                                 void *user) {
    (void)inv; (void)user;
    const int32_t asked = (int32_t)get_u32(cpu->esp + 4u);
    static int32_t said = -2;
    if (asked != said) { /* each new setting once, so a log shows the hook at work */
        char line[96];
        snprintf(line, sizeof line, "core.nfsc: race traffic density %d -> %d", (int)asked,
                 asked <= 1 ? (int)asked : (int)((asked + 1) / 2));
        log_line(line);
        said = asked;
    }
    if (asked <= 1)
        return; /* none, one, or out of range: the setter's own handling */
    api->guest_write_u32(api, cpu->esp + 4u, (uint32_t)((asked + 1) / 2));
}

static void traffic_install(void) {
    if (g_api->hook_install(g_api, TRAFFIC_SET_DENSITY, traffic_density_less, POP_HOOK_BEFORE, NULL,
                            &g_traffic_hook) != POP_OK) {
        g_traffic_hook = 0;
        log_line("core.nfsc: less traffic unavailable (could not hook the density setter)");
        return;
    }
    log_line("core.nfsc: less traffic: race traffic density halved");
}

static void traffic_shutdown(void) {
    if (g_traffic_hook) {
        g_api->hook_remove(g_api, g_traffic_hook);
        g_traffic_hook = 0;
    }
}
