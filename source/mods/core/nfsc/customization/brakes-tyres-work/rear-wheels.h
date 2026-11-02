/* Narrow native-function port of EC Wheels.h RearWheels patches.
 * Existing custom catalog contains dedicated rear-wheel records (BRAKE group84),
 * while original FRONT_WHEEL uses WHEEL group85. Preserve this distinction.
 * Include after brakes.h and before customization_install. */
/* EC Textures.h AddWheelTexture: the streamed rim texture is base + _WHEEL.
 * The native list already owns the front wheel dependencies. Add missing
 * explicit front/rear requests without changing that list or requiring the
 * texture to be loaded yet. Native eModel texture binding happens later. */
static uint32_t g_cust_wheel_dependency_logs;
static void cust_wheel_used_textures(uint32_t output, uint32_t ride) {
    if (!output || !cust_bt_allowed(ride)) return;
    for (uint32_t slot = 0x54u; slot <= 0x55u; ++slot) {
        const uint32_t part = cust_bt_part(ride, slot);
        const uint32_t base = part ? cust_attr(part, 0x10c98090u, 0u) : 0u;
        if (!base) continue;
        const uint32_t texture = cust_hash_seed("_WHEEL", base);
        if (!texture) continue;
        const uint32_t count = get_u32(output + 0x340u);
        if (count > 104u) return;
        uint32_t i = 0u;
        while (i < count && get_u32(output + i * 4u) != texture) ++i;
        if (i < count) continue;
        if (count == 104u) return;
        g_api->guest_write_u32(g_api, output + count * 4u, texture);
        g_api->guest_write_u32(g_api, output + 0x340u, count + 1u);
        if (g_cust_wheel_dependency_logs < 12u) {
            ++g_cust_wheel_dependency_logs;
            char line[200];
            snprintf(line, sizeof line, "customization wheel texture: ride=%08x slot=%u part=%u base=%08x requested=%08x permCount=%u",
                     ride, slot, cust_extension_index(part), base, texture, count + 1u);
            log_line(line);
        }
    }
}
static void cust_rear_set_part(const PopModApi *api, pop_cpu_v1 *cpu,
                               PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t ride = cpu->ecx, slot = get_u32(cpu->esp + 4u);
    if (slot != 0x55u || !cust_bt_allowed(ride)) { api->call_next(api, inv, cpu); return; }
    const uint32_t part = get_u32(cpu->esp + 8u), update = get_u32(cpu->esp + 12u);
    /* Native rear slot's jump-table case returns without storing anything.
     * EC changes its switch byte007d6c76 from1 to5 (normal assignment path).
     * Port the same assignment and native enabled-part update at function entry. */
    api->guest_write_u32(api, ride + 0x54u + 0x55u * 4u, part);
    if (update & 255u) cust_call(0x007ce290u, ride, NULL, 0u);
    api->hook_return(api, cpu, 0u, 12u);
}
static void cust_rear_record_from_ride(const PopModApi *api, pop_cpu_v1 *cpu,
                                       PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t record = cpu->ecx, ride = get_u32(cpu->esp + 4u);
    const int eligible = record && cust_bt_allowed(ride);
    api->call_next(api, inv, cpu);
    if (!eligible) return;
    /* Native004c47f0 scans106slots but explicitly skips0x55 at004c4818/1d.
     * EC removes that skip. Reproduce its one omitted iteration using the same
     * profile-owned helper004ba610, preserving native purchase/ownership rules. */
    const uint32_t profile_arg[1] = {0u};
    const uint32_t profile = cust_call(0x00572b90u, get_u32(0x00a97a7cu), profile_arg, 1u);
    if (!profile) return;
    const uint32_t part = cust_bt_part(ride, 0x55u);
    const uint32_t args[4] = {record, get_u32(ride), 0x55u, part};
    cust_call(0x004ba610u, profile + 0x234u, args, 4u);
}
static void cust_rear_brake_marker(const PopModApi *api, pop_cpu_v1 *cpu,
                                    PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t caller = get_u32(cpu->esp), marker = get_u32(cpu->esp + 4u);
    /* Dedicated rear-rim solids share front brake marker convention in EC.
     * Only its two known rear-wheel lookups are redirected; body markers remain
     * untouched. Nativecaller preservedESI=CarRenderInfo, pRideInfo+0x3f0. */
    const uint32_t ride = cpu->esi ? get_u32(cpu->esi + 0x3f0u) : 0u;
    const int eligible = (caller == 0x007df71du || caller == 0x007e6062u) &&
                         marker == 0xb2e13a0du && cust_bt_allowed(ride);
    if (eligible) api->guest_write_u32(api, cpu->esp + 4u, 0xa4ccd4acu);
    api->call_next(api, inv, cpu);
}
static int cust_rear_wheels_map(void) {
    const uint32_t front = get_u32(0x00a73398u + 0x54u * 4u);
    const uint32_t rear = get_u32(0x00a73398u + 0x55u * 4u);
    if (front != 0x55u || (rear != 0x55u && rear != 0x54u)) {
        log_line("customization rear wheels: unexpected native slot map"); return 0;
    }
    g_api->guest_write_u32(g_api, 0x00a73398u + 0x55u * 4u, 0x54u);
    return 1;
}
/* Install map once only after private pilot guard; then addWRAProws:
 * 007d67a0 cust_rear_set_part
 * 004c47f0 cust_rear_record_from_ride
 * 0055c140 cust_rear_brake_marker
 * No code bytes/interior instruction patching. Independent rear appearance,
 * stock/default fallback, native cartcancel and save/reload remain to verify. */
