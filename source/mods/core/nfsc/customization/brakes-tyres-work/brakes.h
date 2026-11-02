#include <math.h>
#include <string.h>
/* Private EC brake behavior port. Include after customization helpers.
 * Source: EC Customization.h HandleBrakes/StandardSelectablePart_Install.
 * Call originals normally; this header never changes ownership, prices or cash.
 * Integrator must define CUST_BT_RIDE_ALLOWED(ride) and update catalog gate.
 */
#ifndef CUST_BT_CAR_ALLOWED
#define CUST_BT_CAR_ALLOWED(car_id) ((car_id) == 17u)
#endif
#ifndef CUST_BT_RIDE_ALLOWED
#define CUST_BT_RIDE_ALLOWED(ride) CUST_BT_CAR_ALLOWED(get_u32(ride))
#endif
static uint32_t cust_bt_part(uint32_t ride, uint32_t slot) {
    const uint32_t args[1] = {slot};
    return cust_call(0x007b06f0u, ride, args, 1u);
}
static int cust_bt_allowed(uint32_t ride) {
    return ride && cust_pilot_enabled() && cust_catalog_ready() && CUST_BT_RIDE_ALLOWED(ride);
}
static uint32_t cust_bt_kit(uint32_t part) {
    return part ? cust_attr(part, 0x796c0cb0u, 0u) : 0u;
}
static uint32_t cust_bt_bykit(uint32_t ride, uint32_t slot, uint32_t kit) {
    uint32_t part = 0;
    for (uint32_t i = 0; i < 4096u; ++i) {
        part = cust_next(slot, get_u32(ride), part);
        if (!part || cust_bt_kit(part) == kit) return part;
    }
    return 0;
}
static void cust_bt_install(uint32_t ride, uint32_t record, uint32_t slot, uint32_t part) {
    if (!part) return;
    const uint32_t set_ride[3] = {slot, part, 1u};
    cust_call(0x007d67a0u, ride, set_ride, 3u);
    if (record) {
        const uint32_t set_record[4] = {slot, part, get_u32(ride), 1u};
        cust_call(0x004bac50u, record, set_record, 4u);
    }
}
static uint32_t cust_bt_current(uint32_t slot) {
    const uint32_t args[1] = {slot};
    const uint32_t cart = cust_call(0x00850190u, 0x00bbaba8u, args, 1u);
    if (cart) {
        const uint32_t selectable = get_u32(cart + 12u);
        if (selectable) return get_u32(selectable + 16u);
    }
    return cust_call(0x0083f850u, 0x00bbaba8u, args, 1u);
}
static void cust_bt_handle(uint32_t ride, uint32_t record) {
    if (!cust_bt_allowed(ride)) return;
    uint32_t brake = cust_bt_part(ride, 0x18u), rotor = cust_bt_part(ride, 0x19u);
    if (brake && !rotor) {
        uint32_t part = 0;
        if (cust_bt_kit(brake)) {
            part = cust_bt_current(0x19u);
            if (!part || !cust_bt_kit(part)) part = cust_bt_bykit(ride, 0x19u, 1u);
        } else part = cust_bt_bykit(ride, 0x19u, 0u);
        cust_bt_install(ride, record, 0x19u, part);
    }
    if (!brake && rotor) {
        uint32_t part = 0;
        if (cust_bt_kit(rotor)) {
            part = cust_bt_current(0x18u);
            if (!part || !cust_bt_kit(part)) part = cust_bt_bykit(ride, 0x18u, 1u);
        } else part = cust_bt_bykit(ride, 0x18u, 0u);
        cust_bt_install(ride, record, 0x18u, part);
    }
    brake = cust_bt_part(ride, 0x18u); rotor = cust_bt_part(ride, 0x19u);
    if (brake) cust_bt_install(ride, record, 0x23u, cust_bt_bykit(ride, 0x23u, cust_bt_kit(brake)));
    if (rotor) cust_bt_install(ride, record, 0x24u, cust_bt_bykit(ride, 0x24u, cust_bt_kit(rotor)));
}
/* WRAP native FeGarageMain::HandleSpecialCustomization. Capture arguments before
 * call_next because native RET 8 removes them. Only author call-site is changed. */
static void cust_bt_garage(const PopModApi *api, pop_cpu_v1 *cpu,
                           PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t caller = get_u32(cpu->esp), ride = get_u32(cpu->esp + 4u);
    const uint32_t record = get_u32(cpu->esp + 8u);
    api->call_next(api, inv, cpu);
    if (caller == 0x0085eaf1u) {cust_bt_handle(ride, record);cust_body_pair_mirrors(ride,record);cust_body_tail_badging(ride,record);cust_generic_vinyl_reconcile(ride,record);}
}
/* WRAP native selectable Install. EC removes only a conflicting paired part;
 * garage's HandleBrakes then chooses the corresponding stock/custom fallback. */
static void cust_bt_selectable_install(const PopModApi *api, pop_cpu_v1 *cpu,
                                       PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t item = cpu->ecx, slot = get_u32(item + 24u), part = get_u32(item + 16u);
    const uint32_t car = get_u32(0x00b77970u);
    const uint32_t car_id = car && car != 0x00b77970u ? get_u32(car + 8u) : 0xffffffffu;
    uint32_t record = get_u32(cpu->esp + 4u);
    if (!record) record = 0x00bbabc0u;
    if (part && cust_pilot_enabled() && cust_catalog_ready() && CUST_BT_CAR_ALLOWED(car_id) &&
        (slot == 0x18u || slot == 0x19u)) {
        const uint32_t pair = slot == 0x18u ? 0x19u : 0x18u;
        const uint32_t args[2] = {car_id, pair};
        const uint32_t current = cust_call(0x004ae350u, record, args, 2u);
        if (current && (!!cust_bt_kit(current) != !!cust_bt_kit(part)))
            cust_call(0x004ae380u, record, args, 2u);
    }
    api->call_next(api, inv, cpu);
}
/* EC RotorGlow.cpp AdjustBrakeScale. Native wheel transforms are affine
 * orthogonal basis matrices. Scaling their basis rows is equivalent to EC's
 * decompose -> uniform size adjustment -> rotation -> original translation.
 * Clamp uses EC's1.15 limit for each axis, retaining mirrored wheel signs. */
static void cust_bt_draw_scaled(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv) {
    const uint32_t render = cpu->esi, ride = get_u32(render + 0x3f0u);
    const uint32_t wheel = get_u32(cpu->ebp + 0x28u), matrix = get_u32(cpu->esp + 8u);
    if (!matrix || wheel > 3u || !cust_bt_allowed(ride)) { api->call_next(api, inv, cpu); return; }
    uint32_t rim = cust_bt_part(ride, wheel > 1u ? 0x55u : 0x54u);
    const uint32_t brake = cust_bt_part(ride, wheel > 1u ? 0x23u : 0x18u);
    if (wheel > 1u && (!rim || cust_attr(rim, 0x2c420d64u, 0u))) rim = cust_bt_part(ride, 0x54u);
    const uint32_t radius = rim ? cust_attr(rim, 0xeb0101e2u, 0u) : 0u;
    if (!rim || !brake || radius <= 16u || !cust_bt_kit(brake) ||
        cust_attr(rim, 0x03b83203u, 0u) || cust_attr(rim, 0x2850a03bu, 0u)) {
        api->call_next(api, inv, cpu); return;
    }
    uint32_t original[16], scale_bits = get_u32(render + 0x490u + wheel * 4u);
    float m[16], native_radius_scale;
    memcpy(&native_radius_scale, &scale_bits, 4u);
    for (uint32_t i = 0; i < 16u; ++i) original[i] = get_u32(matrix + i * 4u);
    memcpy(m, original, sizeof(m));
    const float adjustment = 0.85f * native_radius_scale * (1.0f + (radius - 17u) * 0.07f);
    float lengths[3];
    for (uint32_t row = 0; row < 3u; ++row) {
        const uint32_t base = row * 4u;
        lengths[row] = sqrtf(m[base] * m[base] + m[base+1u] * m[base+1u] + m[base+2u] * m[base+2u]);
    }
    if (!isfinite(adjustment) || !(adjustment > 0.0f) || !(lengths[0] > 0.0f) ||
        !(lengths[1] > 0.0f) || !(lengths[2] > 0.0f)) { api->call_next(api, inv, cpu); return; }
    const int clipped = lengths[0] * adjustment > 1.15f;
    for (uint32_t row = 0; row < 3u; ++row) {
        const float factor = clipped ? 1.15f / lengths[row] : adjustment;
        for (uint32_t col = 0; col < 3u; ++col) {
            const uint32_t i = row * 4u + col; float value = m[i] * factor; uint32_t bits;
            memcpy(&bits, &value, 4u); g_api->guest_write_u32(g_api, matrix + i * 4u, bits);
        }
    }
    api->call_next(api, inv, cpu);
    for (uint32_t i = 0; i < 16u; ++i) g_api->guest_write_u32(g_api, matrix + i * 4u, original[i]);
}
/* Add hook rows: {0x008597f0u,cust_bt_garage,POP_HOOK_WRAP},
 *                {0x008420f0u,cust_bt_selectable_install,POP_HOOK_WRAP}.
 * Additional front/rear/stock fitment and record/cancel runtime checks required. */
