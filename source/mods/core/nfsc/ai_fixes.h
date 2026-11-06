/* Native Carbon 1.4 AI corrections. Included after mp.h and mp_drive.h.
 * Route discovery remains owned by GRaceStatus. Only the AI planner's query
 * bypasses that discovery gate; its difficulty/chance calculation still runs.
 * Patrols retain their traffic action, with the existing reverse controller
 * enabled. Arrested native racers park through the game's goal/input APIs.
 */
#define AI_SHORTCUT_KNOWN 0x00610f10u
#define AI_SHORTCUT_RETURN 0x007ed5ebu
#define AI_ON_REVERSE 0x00417ac0u
#define AI_PATROL_GOAL 0x9af1bf40u
#define AI_NONE_NAME 0x009c347cu
#define AI_NOTIFY 0x00568dd0u
#define AI_RACER_BUSTED 0x2692623eu
#define AI_SIM_MESSAGE 0xce28ab70u
static uint32_t g_ai_hooks[4], g_ai_goal_key;
typedef struct AiArrest { uint32_t car, ai, handle; } AiArrest;
static AiArrest g_ai_arrests[32];
static uint32_t g_ai_arrest_count;

static void ai_arrest_notice(const PopModApi *api, pop_cpu_v1 *cpu,
                             PopHookInvocation *inv, void *user) {
    (void)api; (void)inv; (void)user;
    if (get_u32(cpu->esp + 4) != AI_RACER_BUSTED ||
        get_u32(cpu->ecx) != AI_SIM_MESSAGE)
        return;
    const uint32_t owner = get_u32(cpu->ecx + 0x10);
    uint32_t node = get_u32(MP_CARS);
    for (int n = 0; node && node != MP_CARS && n < 256; ++n, node = get_u32(node)) {
        const uint32_t car = node - 0xd0;
        if (get_u32(car + 0x148) != 3 || get_u32(car + 0x7c) ||
            get_u32(car + 0x5c) != owner)
            continue;
        for (uint32_t i = 0; i < g_ai_arrest_count; ++i)
            if (g_ai_arrests[i].car == car) return;
        if (g_ai_arrest_count < 32) {
            AiArrest *a = &g_ai_arrests[g_ai_arrest_count++];
            a->car = car; a->ai = get_u32(car + 0x104);
            a->handle = get_u32(car + 0x2c);
        }
        return;
    }
}

static void ai_shortcut_known(const PopModApi *api, pop_cpu_v1 *cpu,
                              PopHookInvocation *inv, void *user) {
    (void)api; (void)inv; (void)user;
    cpu->eax = (cpu->eax & 0xffffff00u) | 1u;
}

static void ai_patrol_reverse(const PopModApi *api, pop_cpu_v1 *cpu,
                              PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    const uint32_t ai = cpu->ecx + 0x44;
    const uint32_t vehicle = ai >= 4 ? get_u32(ai - 4) : 0;
    /* IVehicle is PVehicle + 0xb8; +0x90 is its native driver class.
     * Class 2 and the patrol goal exclude traffic, pursuits and human players. */
    if (vehicle && get_u32(vehicle + 0x90) == 2 &&
        get_u32(ai + 0xa4) == AI_PATROL_GOAL && get_u32(ai + 0xb0) == 3)
        api->guest_write_u32(api, ai + 0xb0, 7);
}

static void ai_park_busted(const PopModApi *api, pop_cpu_v1 *cpu,
                           PopHookInvocation *inv, void *user) {
    (void)cpu; (void)inv; (void)user;
    const uint32_t count = g_ai_arrest_count;
    if (!count) return;
    AiArrest pending[32];
    memcpy(pending, g_ai_arrests, count * sizeof *pending);
    g_ai_arrest_count = 0;
    if (!g_ai_goal_key || get_u32(0x00a99bbcu) != 6)
        return;
    uint32_t none = get_u32(g_ai_goal_key);
    if (!none) {
        const uint32_t name = AI_NONE_NAME;
        none = mp_call(MP_STRING_KEY, 0, &name, 1);
        if (!none) return;
        mp_put(g_ai_goal_key, none);
    }
    for (uint32_t n = 0; n < count; ++n) {
        const AiArrest arrest = pending[n];
        const uint32_t car = arrest.car, ai = arrest.ai;
        /* A pursuit detaches before frame end. Retain the native arrest
         * notice, then validate the instance handle before parking. */
        if (!ai || !mp_listed(car) || get_u32(car + 0x148) != 3 ||
            get_u32(car + 0x7c) || get_u32(car + 0x104) != ai ||
            get_u32(car + 0x2c) != arrest.handle)
            continue;
        uint32_t input = get_u32(car + DRV_INPUT);
        if (!input || drv_method(input, 3) != DRV_GET_GAS ||
            drv_method(input, 2) != DRV_GET_BRAKE || drv_method(input, 7) != DRV_GET_HAND)
            continue;
        if (get_u32(ai + 0xa4) != none) {
            const uint32_t method = get_u32(get_u32(ai) + 0xf0);
            if (!method || api->guest_call(api, method, ai, &g_ai_goal_key, 1, NULL) != POP_OK)
                continue;
        } else if (get_u32(input + DRV_CTL + 4) == 0 &&
                   get_u32(input + DRV_CTL + 8) == 0x3f800000u &&
                   get_u32(input + DRV_CTL + 12) == 0x3f800000u &&
                   !(get_u32(input + DRV_CTL + 0x18) & 0xffu)) {
            continue;
        }
        input = get_u32(car + DRV_INPUT);
        /* A goal change alone leaves the previous throttle latched. Use the
         * same native input setters as ordinary vehicle playback, not a
         * velocity/position write. Keep the parked car present and collidable. */
        const uint32_t zero = 0, one = 0x3f800000u;
        mp_call(drv_method(input, 11), input, &zero, 1);
        mp_call(drv_method(input, 12), input, &one, 1);
        mp_call(drv_method(input, 14), input, &one, 1);
        mp_call(drv_method(input, 13), input, &zero, 1);
    }
}

static void ai_fixes_shutdown(void) {
    for (int i = 0; i < 4; ++i) {
        if (g_ai_hooks[i]) g_api->hook_remove(g_api, g_ai_hooks[i]);
        g_ai_hooks[i] = 0;
    }
    if (g_ai_goal_key) g_api->guest_free(g_api, g_ai_goal_key);
    g_ai_goal_key = 0;
    g_ai_arrest_count = 0;
}

static void ai_fixes_install(void) {
    if (!g_api->hook_install_at_callsite) {
        log_line("core.nfsc: native AI call-site fixes unavailable");
    } else {
        if (g_api->hook_install_at_callsite(g_api, AI_SHORTCUT_KNOWN, AI_SHORTCUT_RETURN,
                ai_shortcut_known, POP_HOOK_AFTER, NULL, &g_ai_hooks[0]) != POP_OK)
            log_line("core.nfsc: AI shortcut correction unavailable");
    }
    if (g_api->hook_install(g_api, AI_ON_REVERSE, ai_patrol_reverse, POP_HOOK_BEFORE, NULL,
            &g_ai_hooks[1]) != POP_OK)
        log_line("core.nfsc: patrol reverse correction unavailable");
    if (g_api->guest_alloc(g_api, 4, &g_ai_goal_key) != POP_OK)
        return;
    /* Native calls require a running hook context. Resolve the goal key
     * on the first arrest, rather than during module installation. */
    mp_put(g_ai_goal_key, 0);
    if (g_api->hook_install(g_api, AI_NOTIFY, ai_arrest_notice, POP_HOOK_BEFORE, NULL,
            &g_ai_hooks[2]) != POP_OK)
        log_line("core.nfsc: AI arrest notification unavailable");
    /* The before chain is shared by UI and online modules. Run after the
     * service, still outside AI updates, without consuming that chain. */
    if (g_api->hook_install(g_api, MP_FE_SERVICE, ai_park_busted, POP_HOOK_AFTER, NULL,
            &g_ai_hooks[3]) != POP_OK) {
        g_api->guest_free(g_api, g_ai_goal_key); g_ai_goal_key = 0;
        log_line("core.nfsc: busted racer correction unavailable");
    }
}
