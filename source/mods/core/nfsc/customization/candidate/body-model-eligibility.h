/* Include after cust_call/get_u32. No new hook required.
 * Use only for car-specific hood/spoiler/exhaust extension candidates.
 * Original007cddf0 performs this same seven-model ownership check.
 * Universal new wheels/brakes/tyres/neon have separate explicitoverride policy.
 */
static int cust_body_model_matches(uint32_t part, uint32_t slot, uint32_t car) {
    if (!part) return 0;
    const uint32_t args[2] = {slot, car};
    const uint32_t allowed = cust_call(0x007b0360u, 0u, args, 2u);
    const uint32_t models = get_u32(0x00b74cb8u);
    if (!allowed || !models) return 0;
    const uint32_t model_index = (get_u32(part) >> 8u) & 255u;
    const uint32_t model_hash = get_u32(models + model_index * 4u);
    for (uint32_t i = 0; i < 7u; ++i)
        if (model_hash && get_u32(allowed + i * 4u) == model_hash) return 1;
    return 0;
}
/* Extension slots to add: HOOD0x4e, SPOILER0x30, EXHAUST0x2f.
 * While scanning extension candidate part, require this helper for those slots.
 * Hood entries are RX7-specific. Spoiler variants must follow native generic /
 * HATCH / CARRERA / PORSCHES family. Exhaust new records areAutosculpt; do not
 * strip MORPHTARGET_NUM, remap slots or exposemissingSTYLE17_CENTER geometry.
 * Camera/menu/buy/cancel/save/render checks remain required. */
