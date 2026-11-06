/* guards.h - crash guards found in NFSCO (nfsco.club, build 2888; its authors gave permission to use
 * their mods). Each is NFSCO's own fix for a crash in the vanilla game, traced in its code
 * (logs/nfsco-2888/hooks-inventory.md, rows O1, V3 and S7). Included by nfsc.c.
 *
 * MINIMAP CHOPS (O1). 005d8880, a load callback for a minimap texture piece ("%s_CHOP%d.dds"),
 * marks the piece loaded with `byte [this + 0x98 + index] = 1`, index = [this + 0x94], unchecked: the
 * table has 0x100 entries, so a bigger minimap (more pieces) writes past it. A piece with an index of
 * 0x100 or more is not marked; the rest runs as before ([this + 0x94] = -1, 005d1b80, 005d8910).
 *
 * RENDER OBJECT (V3). 0071e030 (thiscall, 2 arguments) reads 0xec bytes of its first argument without
 * a check beyond 0. NFSCO skips the call when that object cannot be read (IsBadReadPtr); here an
 * object outside guest memory is skipped.
 *
 * RACE ROW WITHOUT A RACE (S7). The police code at 0044a681 calls 00643ed0 on a racer row it found,
 * which ends in 00640d50 on the race status (0x00a98284) and its race parameters (+0x6a1c). NFSCO
 * calls it only while both exist; so does this (a call-site hook: only that call). */

#define GD_CHOP 0x005d8880u       /* cdecl (this), plain RET */
#define GD_CHOP_INDEX 0x94u
#define GD_CHOP_FLAGS 0x98u
#define GD_CHOP_MAX 0x100u
#define GD_CHOP_NEXT 0x005d1b80u  /* thiscall, no arguments */
#define GD_CHOP_TAIL 0x005d8910u  /* thiscall, no arguments */
#define GD_RENDER 0x0071e030u     /* thiscall, 2 arguments, RET 8 */
#define GD_RENDER_SIZE 0xecu
#define GD_ROW_DONE 0x00643ed0u   /* thiscall on a racer row, plain RET */
#define GD_ROW_RET 0x0044a686u    /* after its call at 0044a681 */
#define GD_RACE_STATUS 0x00a98284u
#define GD_RACE_PARAMS 0x6a1cu

static uint32_t g_gd_hooks[3];

static void gd_chop(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t self = get_u32(cpu->esp + 4);
    if (!self || get_u32(self + GD_CHOP_INDEX) < GD_CHOP_MAX) {
        api->call_original(api, cpu->target, cpu);
        return;
    }
    static unsigned told;
    if (++told <= 10u) {
        char line[96];
        snprintf(line, sizeof line, "core.nfsc: guards: minimap piece %u not marked (the table has %u)",
                 get_u32(self + GD_CHOP_INDEX), GD_CHOP_MAX);
        log_line(line);
    }
    mp_put(self + GD_CHOP_INDEX, 0xffffffffu);
    mp_call(GD_CHOP_NEXT, self, NULL, 0);
    const uint32_t eax = mp_call(GD_CHOP_TAIL, self, NULL, 0);
    api->hook_return(api, cpu, eax, 0);
}

static void gd_render(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t object = get_u32(cpu->esp + 4);
    void *p = NULL;
    if (object && api->guest_ptr(api, object, GD_RENDER_SIZE, &p) != POP_OK) {
        static unsigned told;
        if (++told <= 10u) {
            char line[96];
            snprintf(line, sizeof line, "core.nfsc: guards: render object %08x outside memory, skipped", object);
            log_line(line);
        }
        api->hook_return(api, cpu, cpu->eax, 8);
        return;
    }
    api->call_original(api, cpu->target, cpu);
}

static void gd_row_done(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t status = get_u32(GD_RACE_STATUS);
    if (status && get_u32(status + GD_RACE_PARAMS)) {
        api->call_original(api, cpu->target, cpu);
        return;
    }
    static unsigned told;
    if (++told <= 10u)
        log_line("core.nfsc: guards: a racer row update without a race skipped");
    api->hook_return(api, cpu, cpu->eax, 0);
}

static void guards_install(void) {
    if (g_api->hook_install(g_api, GD_CHOP, gd_chop, POP_HOOK_REPLACE, NULL, &g_gd_hooks[0]) != POP_OK) {
        g_gd_hooks[0] = 0;
        log_line("core.nfsc: guards: no minimap piece guard");
    }
    if (g_api->hook_install(g_api, GD_RENDER, gd_render, POP_HOOK_REPLACE, NULL, &g_gd_hooks[1]) != POP_OK) {
        g_gd_hooks[1] = 0;
        log_line("core.nfsc: guards: no render object guard");
    }
    if (!g_api->hook_install_at_callsite ||
        g_api->hook_install_at_callsite(g_api, GD_ROW_DONE, GD_ROW_RET, gd_row_done, POP_HOOK_REPLACE, NULL,
                                        &g_gd_hooks[2]) != POP_OK) {
        g_gd_hooks[2] = 0;
        log_line("core.nfsc: guards: no race row guard");
    }
}

static void guards_shutdown(void) {
    for (int i = 0; i < 3; ++i) {
        if (g_gd_hooks[i])
            g_api->hook_remove(g_api, g_gd_hooks[i]);
        g_gd_hooks[i] = 0;
    }
}
