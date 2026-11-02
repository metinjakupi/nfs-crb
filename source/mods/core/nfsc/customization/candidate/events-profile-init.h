/* Add the three event records absent from a pre-extension save.
 * Mirrors 00645320's defaults without its destructive whole-table reset.
 * Existing records, including partial extension progress, are never changed. */
static uint32_t g_cust_event_init_hook;
static double g_cust_event_init_last;
static const uint32_t cust_event_ids[3] = {3032674889u, 2170281917u, 157229008u};
static const uint32_t cust_event_collections[3] = {2452560040u, 3164566534u, 2778896776u};
static int cust_event_key(uint32_t key) {
    return key == cust_event_ids[0] || key == cust_event_ids[1] || key == cust_event_ids[2];
}
static uint32_t cust_event_existing_digest(uint32_t table, uint32_t count) {
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t p = table + i * 16u, key = get_u32(p);
        if (!key || cust_event_key(key)) continue;
        hash = (hash ^ i) * 16777619u;
        for (uint32_t j = 0; j < 16u; j += 4u) hash = (hash ^ get_u32(p + j)) * 16777619u;
    }
    return hash;
}
static void cust_event_init_service(const PopModApi *api, pop_cpu_v1 *cpu,
                                    PopHookInvocation *inv, void *user) {
    (void)cpu; (void)inv; (void)user;
    const double now = mp_now();
    if (now - g_cust_event_init_last < 1000.0) return;
    g_cust_event_init_last = now;
    if (!cust_pilot_enabled() || !cust_catalog_ready() || get_u32(RACE_FLOW) != 3u || !resume_profile()) return;
    const uint32_t manager = get_u32(0x00a9828cu);
    const uint32_t count = manager ? get_u32(manager) : 0u;
    const uint32_t table = manager ? get_u32(manager + 0x40u) : 0u;
    if (count != 183u || !table) return; /* exact versioned event package */
    uint32_t present = 0, empty[3] = {0}, empties = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t p = table + i * 16u, key = get_u32(p);
        if (cust_event_key(key)) ++present;
        if (!key && empties < 3u) empty[empties++] = p;
    }
    if (present || empties != 3u) return;
    for (uint32_t i = 0; i < 3u; ++i)
        for (uint32_t j = 0; j < 16u; j += 4u) if (get_u32(empty[i] + j)) return;
    uint32_t flags[3] = {0};
    for (uint32_t i = 0; i < 3u; ++i) {
        const uint32_t collection = cust_event_collections[i];
        uint32_t parameters = 0, key = 0;
        if (api->guest_call(api, 0x0061bf80u, manager, &collection, 1u, &parameters) != POP_OK || !parameters) return;
        if (api->guest_call(api, 0x00613890u, parameters, NULL, 0u, &key) != POP_OK || key != cust_event_ids[i]) return;
        const uint32_t getters[5] = {0x0063dfa0u, 0x0063e000u, 0x0063e060u, 0x0063e120u, 0x0063e4e0u};
        uint32_t values[5] = {0};
        for (uint32_t j = 0; j < 5u; ++j)
            if (api->guest_call(api, getters[j], parameters, NULL, 0u, &values[j]) != POP_OK) return;
        const int initial = (values[2] & 255u) && !(values[3] & 255u);
        flags[i] = ((values[0] & 255u) || initial ? 4u : 0u) |
                   (values[1] & 255u ? 16u : 0u) | (values[4] & 255u ? 0u : 8u);
    }
    if (flags[0] != 28u || flags[1] != 24u || flags[2] != 24u) return;
    const uint32_t before = cust_event_existing_digest(table, count);
    uint32_t created = 0;
    for (; created < 3u; ++created) {
        const uint32_t key = cust_event_ids[created];
        uint32_t record = 0;
        if (api->guest_call(api, 0x006133d0u, manager, &key, 1u, &record) != POP_OK ||
            record != empty[created] || get_u32(record) != key) break;
        api->guest_write_u32(api, record + 4u, flags[created]);
    }
    const uint32_t after = cust_event_existing_digest(table, count);
    if (created != 3u || before != after) {
        for (uint32_t i = 0; i < 3u; ++i)
            for (uint32_t j = 0; j < 16u; j += 4u) api->guest_write_u32(api, empty[i] + j, 0u);
        log_line("customization events: missing-record initialization failed; new slots restored");
        return;
    }
    /* Same queued EAutoSave event used by the native reward path. The native
     * manager respects autosave settings and commits at its normal transition. */
    const uint32_t event_args[2] = {8u, 0u};
    const uint32_t event = cust_call(0x00561590u, 0u, event_args, 2u);
    if (event) cust_call(0x0066bab0u, event, NULL, 0u);
    char line[200];
    snprintf(line, sizeof line, "customization events: initialized missing Bronze/Silver/Gold records (28/24/24); existing records digest %08x unchanged; autosave event %08x", before, event);
    log_line(line);
}
static void cust_event_init_install(void) {
    if (g_api->hook_install(g_api, 0x00600c90u, cust_event_init_service, POP_HOOK_BEFORE,
                            NULL, &g_cust_event_init_hook) != POP_OK)
        log_line("customization events: profile initialization hook unavailable");
}
