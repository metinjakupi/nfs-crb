/* Carbon's model helpers 0070eda0 and 0070ede0 release vertex buffers by
 * calling Lock a second time. The renderer commits staged edits on the final
 * Unlock, so those unmatched locks hide Autosculpt changes. Balance both
 * paths using the real COM Unlock slot. Preserve native vertex layout and
 * ownership: a reader must not release a lock held by its caller.
 */
static uint32_t g_sculpt_hooks[4];

static int sculpt_release(const PopModApi *api, uint32_t entry) {
    const uint32_t vb = get_u32(entry + 0x74u);
    const uint32_t vt = vb ? get_u32(vb) : 0u;
    const uint32_t unlock = vt ? get_u32(vt + 0x30u) : 0u;
    const uint32_t args[] = {vb};
    uint32_t result = 0u;
    if (!unlock || api->guest_call(api, unlock, 0, args, 1, &result) != POP_OK ||
        (int32_t)result < 0) return 0;
    api->guest_write_u32(api, entry + 0x88u, 0u);
    return 1;
}

static void sculpt_read(const PopModApi *api, pop_cpu_v1 *cpu,
                        PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t model = get_u32(cpu->ecx), out = get_u32(cpu->esp + 4u);
    const uint32_t sub = get_u32(cpu->esp + 8u), vertex = get_u32(cpu->esp + 12u);
    const uint32_t count = model ? get_u32(model + 0x10u) : 0u;
    const uint32_t meshes = model ? get_u32(model + 0x14u) : 0u;
    if (!out || !meshes || sub >= count) { api->call_next(api, inv, cpu); return; }
    const uint32_t entry = meshes + sub * 0x90u, vb = get_u32(entry + 0x74u);
    uint32_t bytes = get_u32(entry + 0x88u), result = 0u, index = vertex;
    const int own = !bytes;
    if (own) {
        const uint32_t vt = vb ? get_u32(vb) : 0u, lock = vt ? get_u32(vt + 0x2cu) : 0u;
        const uint32_t args[] = {vb, 0, 0, entry + 0x88u, 0};
        if (!lock || api->guest_call(api, lock, 0, args, 5, &result) != POP_OK ||
            (int32_t)result < 0) { api->call_next(api, inv, cpu); return; }
        bytes = get_u32(entry + 0x88u);
        index += get_u32(entry + 0x8cu);
    }
    const uint32_t at = bytes + index * 48u;
    /* Native output: position, UV, then normal. Leave other fields intact. */
    static const uint32_t src[] = {0, 4, 8, 0x1c, 0x20, 0xc, 0x10, 0x14};
    static const uint32_t dst[] = {0, 4, 8, 0xc, 0x10, 0x14, 0x18, 0x1c};
    for (unsigned i = 0; i < 8; ++i)
        api->guest_write_u32(api, out + dst[i], get_u32(at + src[i]));
    if (own && !sculpt_release(api, entry))
        log_line("core.nfsc: model vertex read could not release its buffer");
    api->hook_return(api, cpu, cpu->eax, 12u);
}

static void sculpt_unlock(const PopModApi *api, pop_cpu_v1 *cpu,
                          PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t model = get_u32(cpu->ecx), sub = get_u32(cpu->esp + 4u);
    const uint32_t count = model ? get_u32(model + 0x10u) : 0u;
    const uint32_t meshes = model ? get_u32(model + 0x14u) : 0u;
    if (!meshes || sub >= count) { api->call_next(api, inv, cpu); return; }
    const uint32_t entry = meshes + sub * 0x90u;
    if (get_u32(entry + 0x88u) && !sculpt_release(api, entry)) {
        api->call_next(api, inv, cpu); return;
    }
    api->hook_return(api, cpu, cpu->eax, 4u);
}

/* Projected model edges must use the same horizontal units as FEng. */
static void sculpt_project(const PopModApi *api, pop_cpu_v1 *cpu,
                           PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t a = get_u32(cpu->esp + 20u), b = get_u32(cpu->esp + 24u);
    if (api->call_next(api, inv, cpu) != POP_OK) return;
    const float width = bits_float(get_u32(SLOT_CURSOR_WIDTH));
    if (width > 0.0f && a && b) {
        const float scale = width / 640.0f;
        put_float(a, bits_float(get_u32(a)) * scale);
        put_float(b, bits_float(get_u32(b)) * scale);
    }
}

/* The editor resets model defaults when this part is absent from the cart.
 * That includes an already-fitted part: restore its selected car record instead.
 * Pending edits bypass this native reset; a different part keeps native defaults.
 */
static void sculpt_editor_defaults(const PopModApi *api, pop_cpu_v1 *cpu,
                                   PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t slot = get_u32(cpu->esp + 4u);
    const uint32_t control = get_u32(0x00bbb050u);
    if (get_u32(cpu->esp) == 0x0085b49eu && cpu->ecx == 0x00bbabc0u &&
        slot < 21u && control && get_u32(control) == 0x009f9f48u &&
        get_u32(control + 0x1cu) == slot) {
        const uint32_t fe = get_u32(0x00a97a7cu);
        const uint32_t profiles = fe ? get_u32(fe + 0xd4u) : 0u;
        const uint32_t profile = profiles ? get_u32(profiles) : 0u;
        const uint32_t car = get_u32(0x00bbabacu);
        const uint32_t db = profile ? profile + 0x234u : 0u;
        uint8_t custom = 0xffu;
        if (db && car >= db && car < db + 200u * 0x14u &&
            (car - db) % 0x14u == 0u && get_u32(car) != 0xffffffffu &&
            api->guest_read_u8(api, car + 0x10u, &custom) == POP_OK && custom < 75u) {
            const uint32_t owned = db + 0xfa0u + (uint32_t)custom * 0x470u;
            uint32_t vehicle = 0u, fitted = 0u;
            if (api->guest_call(api, 0x004ae150u, car, NULL, 0u, &vehicle) == POP_OK) {
                const uint32_t args[] = {owned, vehicle};
                if (api->guest_call(api, 0x00842770u, control, args, 2u, &fitted) == POP_OK &&
                    (fitted & 0xffu)) {
                    const uint32_t offset = 0xccu + slot * 44u;
                    for (uint32_t i = 0u; i < 11u; ++i)
                        api->guest_write_u32(api, cpu->ecx + offset + i * 4u,
                                            get_u32(owned + offset + i * 4u));
                    api->hook_return(api, cpu, cpu->eax, 4u);
                    return;
                }
            }
        }
    }
    api->call_next(api, inv, cpu);
}

static void sculpt_install(void) {
    if (g_api->hook_install(g_api, 0x0070ede0u, sculpt_read, POP_HOOK_REPLACE,
                            NULL, &g_sculpt_hooks[0]) != POP_OK) return;
    if (g_api->hook_install(g_api, 0x0070eda0u, sculpt_unlock, POP_HOOK_REPLACE,
                            NULL, &g_sculpt_hooks[1]) != POP_OK) {
        g_api->hook_remove(g_api, g_sculpt_hooks[0]); return;
    }
    if (g_api->hook_install(g_api, 0x007c9150u, sculpt_project, POP_HOOK_REPLACE, NULL, &g_sculpt_hooks[2]) == POP_OK)
        log_line("core.nfsc: Autosculpt outline uses the menu width");
    if (g_api->hook_install(g_api, 0x0049c8f0u, sculpt_editor_defaults, POP_HOOK_REPLACE,
                            NULL, &g_sculpt_hooks[3]) == POP_OK)
        log_line("core.nfsc: Autosculpt editor restores fitted-car zones");
    log_line("core.nfsc: balanced Autosculpt model buffers");
}
