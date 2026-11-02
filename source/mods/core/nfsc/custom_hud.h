/* Local World HUD bridge, Carbon 1.4. Included after mp.h.
 * CHD1 v1: native seqlock +8, frame +12, visible/available/features/active
 * +16..28, six floats +32..52, gear/metric +56..60; page heartbeat/request
 * +64/+68. Native owns output. Page owns only its two atomic words.
 * A live, ready page leases the original gauge replacement for 750 ms.
 * Missing/stale page, invalid telemetry or missing HUD restores native bits.
 * Does not write car state, settings, profiles or saves.
 */
typedef struct CustomHudShared {
    uint32_t magic, version, seq, frame;
    uint32_t visible, available, features, active;
    float speed, rpm, redline, nos, boost, breaker;
    uint32_t gear, metric;
    uint32_t heartbeat, request;
} CustomHudShared;
static uint32_t g_ch_block, g_ch_scratch, g_ch_hook, g_ch_heartbeat;
static double g_ch_page_at;
static CustomHudShared *custom_hud_shared(void) {
    void *p = NULL;
    return g_ch_block && g_api->guest_ptr(g_api, g_ch_block, sizeof(CustomHudShared), &p) == POP_OK
        ? (CustomHudShared *)p : NULL;
}
/* DAL getters return bool in AL and put a value in their first argument.
 * Always clear scratch: some unsuccessful getters leave output untouched. */
static int custom_hud_read(uint32_t fn, uint32_t *bits) {
    const uint32_t args[2] = {g_ch_scratch, 0};
    uint32_t result = 0;
    g_api->guest_write_u32(g_api, g_ch_scratch, 0);
    if (g_api->guest_call(g_api, fn, 0, args, 2, &result) != POP_OK || !(result & 0xffu))
        return 0;
    *bits = get_u32(g_ch_scratch);
    return 1;
}
static int custom_hud_float(uint32_t fn, float *value) {
    uint32_t bits = 0;
    if (!custom_hud_read(fn, &bits)) return 0;
    const float v = bits_float(bits);
    if (!isfinite(v)) return 0;
    *value = v;
    return 1;
}
/* Only this caller uses the game feature result to update the local FEngHud.
 * Preserve the full EDX half of its 64-bit result and every unrelated bit. */
static void custom_hud_features(const PopModApi *api, pop_cpu_v1 *cpu,
                                PopHookInvocation *inv, void *user) {
    (void)api; (void)inv; (void)user;
    CustomHudShared *sh = custom_hud_shared();
    if (!sh) return;
    const uint32_t native_flags = cpu->eax;
    const double now = mp_now();
    const uint32_t heartbeat = __atomic_load_n(&sh->heartbeat, __ATOMIC_SEQ_CST);
    if (heartbeat != g_ch_heartbeat) {
        g_ch_heartbeat = heartbeat;
        g_ch_page_at = now;
    }
    CustomHudShared next = {0};
    next.features = native_flags;
    const int requested = __atomic_load_n(&sh->request, __ATOMIC_SEQ_CST) == 1u;
    if (requested && heartbeat && now - g_ch_page_at < 750.0 && (native_flags & 2u)) {
        uint32_t shown = 0, controlled = 0;
        const int visible = custom_hud_read(0x004b6da0u, &shown) && shown &&
            custom_hud_read(0x004c3540u, &controlled) && controlled &&
            !(mp_call(MP_IS_PAUSED, 0, NULL, 0) & 0xffu);
        if (visible) {
            const int speed = custom_hud_float(0x004b6500u, &next.speed);
            const int rpm = custom_hud_float(0x004b65f0u, &next.rpm);
            const int gear = custom_hud_read(0x004b68c0u, &next.gear);
            custom_hud_float(0x004b6690u, &next.redline);
            if (custom_hud_float(0x004b67f0u, &next.nos)) next.available |= 1u;
            if (custom_hud_float(0x004b6730u, &next.boost)) next.available |= 2u;
            if (custom_hud_float(0x004b6870u, &next.breaker)) next.available |= 4u;
            uint32_t result = 0;
            const uint32_t args[1] = {g_ch_scratch};
            g_api->guest_write_u32(g_api, g_ch_scratch, 0);
            const int units = g_api->guest_call(g_api, 0x004a4550u, 0, args, 1, &result) == POP_OK &&
                (result & 0xffu);
            next.metric = get_u32(g_ch_scratch) == 1u;
            next.visible = speed && rpm && gear && units && next.rpm >= 0 &&
                next.rpm < 30000 && fabsf(next.speed) < 1500 && next.gear <= 12u;
            if (next.visible) {
                next.active = 1;
                /* Same three gauge bits used by Custom HUD's Carbon adapter. */
                cpu->eax = native_flags & ~((1u << 1) | (1u << 11) | (1u << 18));
            }
        }
    }
    __atomic_fetch_add(&sh->seq, 1u, __ATOMIC_SEQ_CST);
    ++sh->frame;
    memcpy(&sh->visible, &next.visible, offsetof(CustomHudShared, heartbeat) - offsetof(CustomHudShared, visible));
    __atomic_fetch_add(&sh->seq, 1u, __ATOMIC_SEQ_CST);
}
static void custom_hud_install(void) {
#ifdef __EMSCRIPTEN__
    if (g_api->size < offsetof(PopModApi, hook_install_at_callsite) + sizeof(void *) ||
        !g_api->hook_install_at_callsite ||
        g_api->guest_alloc(g_api, sizeof(CustomHudShared), &g_ch_block) != POP_OK ||
        g_api->guest_alloc(g_api, 16, &g_ch_scratch) != POP_OK) return;
    CustomHudShared *sh = custom_hud_shared();
    if (!sh) return;
    memset(sh, 0, sizeof *sh);
    sh->magic = 0x31444843u;
    sh->version = 1;
    if (g_api->hook_install_at_callsite(g_api, 0x005dc4b0u, 0x005e6f80u,
            custom_hud_features, POP_HOOK_AFTER, NULL, &g_ch_hook) != POP_OK) {
        log_line("core.nfsc: custom HUD unavailable; original gauges retained");
        return;
    }
    MAIN_THREAD_ASYNC_EM_ASM({
        var block = {};
        block.memory = wasmMemory;
        block.ptr = $0;
        globalThis.carbonHudBlock = block;
        dispatchEvent(new Event("carbon-hud-ready"));
    }, (uint32_t)(uintptr_t)sh);
    log_line("core.nfsc: local custom HUD bridge ready (CHD1); original gauges until page ready");
#endif
}
static void custom_hud_shutdown(void) {
    if (g_ch_hook) {
        g_api->hook_remove(g_api, g_ch_hook);
        g_ch_hook = 0;
    }
    CustomHudShared *sh = custom_hud_shared();
    if (sh) {
        __atomic_store_n(&sh->request, 0u, __ATOMIC_SEQ_CST);
        __atomic_store_n(&sh->magic, 0u, __ATOMIC_SEQ_CST);
    }
    /* Keep the mailbox allocated until process teardown: the page can still
     * hold a view of it while its next animation frame observes magic == 0. */
}
