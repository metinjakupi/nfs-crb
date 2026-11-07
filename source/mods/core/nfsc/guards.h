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
 * calls it only while both exist; so does this (a call-site hook: only that call).
 *
 * ATTRIBUTE LOOKUP ON A DEAD COLLECTION (player reports 2026-11-06, 8 of 13 "memory access out of bounds"
 * traps, 4633ec / 0062f5d4: logs/reports-review-20261106b/REPORT.md). 004633d0 (Attrib collection lookup,
 * thiscall, RET 8) walks the collection chain ([c + 0xc] parent, [c] table, [c + 4] count) and read a pointer
 * that is not memory any more (5, fee70117, cb01e454): the collection of a vehicle/race object that was freed
 * and its memory used again. The callers that crashed (0062f590's three reads, 0061bb60's one, 00465340) get a "not found"
 * (0, as the lookup returns for a missing key; 0062f590 then takes its default, 00678410(4)) when the chain
 * is not readable. Only these call sites are hooked: the lookup itself is called thousands of times a second.
 * The first few are logged with the caller, the holder and the collection, to name the object that died.
 *
 * CENTRAL LOOKUP GUARD. Reports since then show the same dead collection reached from other callers (0061bb60, 00465340,
 * 0062f590), and 00463a80 has 865 callers, so 004633d0 itself is hooked (REPLACE, the one place all of them end in)
 * with the same check. A lookup costs a few readable-memory checks and a counter; the count is logged every million
 * lookups (rate and refusals), to show what the hook costs and how often it saves a crash.
 *
 * INSTANCE LIST LOOKUP (0061bf80, thiscall, RET 4: finds the entry whose collection has a key; reports 00625f4f,
 * 00635ca4: an entry's collection pointer [entry + 4] was dead). Its array is checked first; a dead entry means
 * "not found" (0).
 *
 * LIST TEARDOWN (0046b140, report 0046b1ad): it calls 00469270 (fastcall, RET 0) on each node's object; one node
 * pointed into the game's own code (004b0018). An object in .text, or whose two links are not readable, is skipped.
 *
 * REGISTERS AFTER THE WRECKED-COP EVENT (same reports, 3 traps at 00445f65). UpdatePursuits 00445c70 calls
 * 00631810 at 00445f60 and then reads [ESI + 0x144]; ESI (callee-saved) was not a car any more (reports show
 * 006d7ec0, a virtual-function thunk, in a stale snapshot). Here EBX, EBP, ESI and EDI are put back after
 * that call if it changed them, and the change is logged (which register, before, after): the first sign of
 * who clobbers it. Nothing changes when the call keeps them, as it should. */

#include <stdlib.h>

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

#define GD_ATTR_GET 0x00463a80u   /* thiscall (holder), 2 stack arguments, RET 8: [holder] = the collection */
#define GD_ATTR_LOOKUP 0x004633d0u /* thiscall (collection), (key, out collection*), RET 8 */
#define GD_PURSUIT_EVENT 0x00631810u /* thiscall, 1 argument, RET 4 */
#define GD_PURSUIT_EVENT_RET 0x00445f65u /* after its call at 00445f60 in UpdatePursuits */
static const uint32_t g_gd_attr_rets[4] = {0x0062f5d4u, 0x0062f64du, 0x0062f699u, /* in 0062f590 */
                                           0x0061bb92u};                        /* in 0061bb60 (key 6ce23062) */
#define GD_ATTR_LOOKUP_RET 0x00465359u /* in 00465340 */
#define GD_FIND_INSTANCE 0x0061bf80u /* thiscall (list), (key), RET 4 */
#define GD_TEARDOWN_CALL 0x00469270u /* fastcall (object), RET 0 */
#define GD_TEARDOWN_RET 0x0046b161u  /* after its call at 0046b15c in 0046b140 */
#define GD_AI_SET_TARGET 0x00417800u /* thiscall (AI vehicle interface), 1 argument, RET 4 */
#define GD_AI_SET_TARGET_RETS 3
static const uint32_t g_gd_ai_rets[GD_AI_SET_TARGET_RETS] = {0x00445ec4u, 0x0044635au, /* UpdatePursuits */
                                                              0x0041287du};            /* in 00412850 */
#define GD_RACE_LOADER 0x00767d10u /* the race / world car loader */
#define GD_CLASS_LIST 0x00a9e850u  /* head of the game's class registry (nodes: key, creator, next at +8) */
#define GD_HASH_STR 0x00606b60u    /* cdecl (string) -> key */
#define GD_PVEHICLE_STR 0x009c2ef4u
/* the walks of the race database by index: its count (00613260, called first by 13 functions, 004d2930 among them, which
 * runs at the end of a pursuit) and the two that use 00613280 without it */
static const uint32_t g_gd_race_walks[3] = {0x00613260u, 0x005c8380u, 0x005b4ac0u};
#define GD_UPDATE_PURSUITS 0x00445c70u /* thiscall (AICopManager), walks its pursuit list at +0x140 */
#define GD_NHOOKS 19
static uint64_t g_gd_lookups, g_gd_refused;

static uint32_t g_gd_hooks[GD_NHOOKS];

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

/* Which 1 MiB pages of guest memory can be read, found once with the API's own check (a page counts if its first or
 * last byte can be: never refuses a good pointer). The lookup guard runs about 24,000 times a second in a race, so
 * a check is two array reads, not a call into the host. */
static uint8_t g_gd_page[4096];
static int g_gd_page_ready;
static int gd_valid(const PopModApi *api, uint32_t addr, uint32_t len) {
    if (!g_gd_page_ready) {
        void *p = NULL;
        for (uint32_t pg = 0; pg < 4096u; ++pg)
            g_gd_page[pg] = api->guest_ptr(api, pg << 20, 1u, &p) == POP_OK ||
                            api->guest_ptr(api, (pg << 20) + 0xfffffu, 1u, &p) == POP_OK;
        g_gd_page_ready = 1;
    }
    if (len == 0)
        len = 1;
    const uint32_t last = addr + len - 1u;
    if (last < addr)
        return 0;
    for (uint32_t pg = addr >> 20; pg <= (last >> 20); ++pg)
        if (!g_gd_page[pg])
            return 0;
    return 1;
}

/* A node is {table, count (word at +4), ..., parent at +0xc}; the table is read only for an index below the
 * count, so an empty node may have no table. Every entry is 12 bytes. */
static int gd_table_ok(const PopModApi *api, uint32_t node) {
    void *p = NULL;
    if (node < 0x10000u || !gd_valid(api, node, 8u))
        return 0;
    const uint32_t table = get_u32(node), count = get_u32(node + 4u) & 0xffffu;
    return count == 0 || (table >= 0x10000u && gd_valid(api, table, count * 12u));
}

/* A collection that the lookup 004633d0 can walk without leaving memory: every link readable, no loop. */
static int gd_chain_ok(const PopModApi *api, uint32_t coll) {
    void *p = NULL;
    uint32_t node = coll;
    for (int step = 0; node; ++step) {
        if (step >= 32 || (node & 3u) || !gd_valid(api, node, 0x20u) || !gd_table_ok(api, node))
            return 0;
        node = get_u32(node + 0xcu);
    }
    if (get_u32(coll + 0x18u)) { /* the class's own table: [[coll + 0x14] + 4] + 8 */
        const uint32_t layout = get_u32(coll + 0x14u);
        if (layout < 0x10000u || !gd_valid(api, layout, 8u))
            return 0;
        const uint32_t cls = get_u32(layout + 4u) + 8u;
        if (!gd_table_ok(api, cls))
            return 0;
    }
    return 1;
}

static void gd_attr_bad(const PopModApi *api, pop_cpu_v1 *cpu, const char *what, uint32_t holder, uint32_t coll) {
    static unsigned told;
    if (++told <= 12u) {
        char line[160];
        snprintf(line, sizeof line,
                 "core.nfsc: guards: %s on a dead collection %08x (holder %08x, caller %08x): not found, skipped",
                 what, coll, holder, get_u32(cpu->esp));
        log_line(line);
    }
}

static void gd_attr_get(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t holder = cpu->ecx;
    void *p = NULL;
    if (holder) {
        if (!gd_valid(api, holder, 4u)) {
            gd_attr_bad(api, cpu, "attribute read", holder, 0);
            api->hook_return(api, cpu, 0, 8);
            return;
        }
        const uint32_t coll = get_u32(holder);
        if (coll && !gd_chain_ok(api, coll)) {
            gd_attr_bad(api, cpu, "attribute read", holder, coll);
            api->hook_return(api, cpu, 0, 8);
            return;
        }
    }
    api->call_original(api, cpu->target, cpu);
}

static void gd_attr_lookup(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t coll = cpu->ecx;
    if ((++g_gd_lookups & 0xfffffu) == 1u) {
        char line[120];
        snprintf(line, sizeof line, "core.nfsc: guards: attribute lookups checked %llu, refused %llu",
                 (unsigned long long)g_gd_lookups, (unsigned long long)g_gd_refused);
        log_line(line);
    }
    if (coll && !gd_chain_ok(api, coll)) {
        ++g_gd_refused;
        gd_attr_bad(api, cpu, "attribute lookup", 0, coll);
        const uint32_t out = get_u32(cpu->esp + 8u);
        if (out)
            mp_put(out, 0);
        api->hook_return(api, cpu, 0, 8);
        return;
    }
    api->call_original(api, cpu->target, cpu);
}

/* The instance list of 0061bf80: {count A, count B, ?, base of A's entries (0x14 each), B's entry pointers from +0x10}. */
static int gd_entry_ok(const PopModApi *api, uint32_t entry) {
    void *p = NULL;
    if (entry < 0x10000u || !gd_valid(api, entry, 0x14u))
        return 0;
    const uint32_t coll = get_u32(entry + 4u);
    if (coll)
        return gd_valid(api, coll, 4u);
    const uint32_t holder = get_u32(entry + 8u); /* 004613e0: if ([holder]) return [[holder] + 0x10] */
    if (!gd_valid(api, holder, 4u))
        return 0;
    const uint32_t inner = get_u32(holder);
    return inner == 0 || gd_valid(api, inner, 0x14u);
}

static int gd_list_ok(const PopModApi *api, uint32_t list) {
    void *p = NULL;
    if (list < 0x10000u || !gd_valid(api, list, 0x10u))
        return 0;
    const uint32_t a = get_u32(list), b = get_u32(list + 4u), base = get_u32(list + 0xcu);
    if (a > 0x1000u || b > 0x1000u)
        return 0;
    if (b && !gd_valid(api, list + 0x10u, b * 4u))
        return 0;
    for (uint32_t i = 0; i < a + b; ++i) {
        const uint32_t entry = i < a ? base + i * 0x14u : get_u32(list + 0x10u + (i - a) * 4u);
        if (!gd_entry_ok(api, entry))
            return 0;
    }
    return 1;
}

/* The race database (the list 0061bf80 searches): a static array, then up to 7 pointers to custom races at +0x10,
 * [db + 4] of them; [db + 0x2c] and [db + 0x30] are the custom slots in use. Player reports showed a custom race left
 * on this list after it died (a world-map race, then the trip back to the safe house): loops that walk the list by
 * index (004d2930 at the end of a pursuit) do not come through the find, so answering "not found" there is not
 * enough. A dead custom entry is taken off the list here as 006132c0 would; its memory is left alone. */
#define GD_RACE_DB 0x00a9828cu
#define GD_RACE_DB_SLOTS 7u
static int gd_heal_races(const PopModApi *api) {
    const uint32_t db = get_u32(GD_RACE_DB);
    if (db < 0x10000u || !gd_valid(api, db, 0x38u))
        return 0;
    uint32_t n = get_u32(db + 4u);
    if (n > GD_RACE_DB_SLOTS)
        return 0;
    int healed = 0;
    for (uint32_t i = n; i-- > 0;) {
        const uint32_t entry = get_u32(db + 0x10u + i * 4u);
        if (gd_entry_ok(api, entry))
            continue;
        static unsigned told;
        if (++told <= 12u) {
            char line[200];
            snprintf(line, sizeof line,
                     "core.nfsc: guards: custom race %08x (slot %u of %u, +4 %08x, +8 %08x) is dead: off the race list "
                     "(slots +2c %08x, +30 %08x)",
                     entry, i, n, gd_valid(api, entry, 0xcu) ? get_u32(entry + 4u) : 0u,
                     gd_valid(api, entry, 0xcu) ? get_u32(entry + 8u) : 0u, get_u32(db + 0x2cu), get_u32(db + 0x30u));
            log_line(line);
        }
        --n;
        mp_put(db + 0x10u + i * 4u, get_u32(db + 0x10u + n * 4u));
        mp_put(db + 0x10u + n * 4u, 0);
        mp_put(db + 4u, n);
        if (get_u32(db + 0x2cu) == entry)
            mp_put(db + 0x2cu, 0);
        if (get_u32(db + 0x30u) == entry) {
            mp_put(db + 0x30u, 0);
            mp_put(db + 0x34u, 0);
        }
        healed = 1;
    }
    return healed;
}

static void gd_find_instance(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    if (gd_list_ok(api, cpu->ecx) ||
        (cpu->ecx == get_u32(GD_RACE_DB) && gd_heal_races(api) && gd_list_ok(api, cpu->ecx))) {
        api->call_original(api, cpu->target, cpu);
        return;
    }
    static unsigned told;
    if (++told <= 12u) {
        char line[140];
        snprintf(line, sizeof line, "core.nfsc: guards: instance list %08x has a dead entry (caller %08x): not found",
                 cpu->ecx, get_u32(cpu->esp));
        log_line(line);
    }
    api->hook_return(api, cpu, 0, 4);
}

static void gd_teardown(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t obj = cpu->ecx;
    void *p = NULL;
    int ok = obj >= 0x10000u && !(obj >= 0x00401000u && obj < 0x009c1000u) && gd_valid(api, obj, 0x20u);
    if (ok) {
        const uint32_t link = get_u32(obj + 0x14u);
        ok = link >= 0x10000u && gd_valid(api, link, 8u);
        if (ok) {
            const uint32_t inner = get_u32(link + 4u);
            ok = inner >= 0x10000u && gd_valid(api, inner, 0x14u);
        }
    }
    if (ok) {
        api->call_original(api, cpu->target, cpu);
        return;
    }
    static unsigned told;
    if (++told <= 12u) {
        char line[140];
        snprintf(line, sizeof line, "core.nfsc: guards: list teardown skipped a dead node %08x", obj);
        log_line(line);
    }
    api->hook_return(api, cpu, 0, 0);
}

static void gd_pursuit_event(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t ebx = cpu->ebx, ebp = cpu->ebp, esi = cpu->esi, edi = cpu->edi;
    api->call_original(api, cpu->target, cpu);
    if (cpu->ebx == ebx && cpu->ebp == ebp && cpu->esi == esi && cpu->edi == edi)
        return;
    static unsigned told;
    if (++told <= 12u) {
        char line[200];
        snprintf(line, sizeof line,
                 "core.nfsc: guards: the wrecked-cop event changed callee-saved registers, put back: "
                 "ebx %08x->%08x ebp %08x->%08x esi %08x->%08x edi %08x->%08x",
                 ebx, cpu->ebx, ebp, cpu->ebp, esi, cpu->esi, edi, cpu->edi);
        log_line(line);
    }
    cpu->ebx = ebx;
    cpu->ebp = ebp;
    cpu->esi = esi;
    cpu->edi = edi;
}

/* UpdatePursuits (00445c70) crashed 12 times right after its virtual call [obj + 0xb4] (00445ebe): every
 * vehicle class of the AI family answers that slot with 00417800, which finds a second interface of its
 * owner and calls it. Back from that call the loop's ESI was out of the arena. The call is run here on its
 * own stack frame (guest_call puts every register back afterwards), so a callee that leaves ESP or a
 * callee-saved register changed cannot take the caller's loop with it. Called from other places the
 * function runs as before. */
static int g_gd_ai_busy, g_gd_ai_all; /* g_gd_ai_all: RECOMP_GUARD_AI_ALL=1, every caller (to test it in a real chase) */
static void gd_ai_set_target(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t ret = get_u32(cpu->esp), arg = get_u32(cpu->esp + 4u);
    int ours = g_gd_ai_all;
    for (int i = 0; i < GD_AI_SET_TARGET_RETS; ++i)
        ours |= ret == g_gd_ai_rets[i];
    /* one REPLACE hook per function, so the caller is checked here; guest_call below comes back through this hook */
    uint32_t eax = 0;
    PopModStatus st = POP_E_STATE;
    if (ours && !g_gd_ai_busy && api->guest_call) {
        ++g_gd_ai_busy;
        st = api->guest_call(api, GD_AI_SET_TARGET, cpu->ecx, &arg, 1u, &eax);
        --g_gd_ai_busy;
    }
    if (st != POP_OK) {
        api->call_original(api, cpu->target, cpu);
        return;
    }
    static unsigned said;
    if (++said <= 3u) {
        char line[170];
        snprintf(line, sizeof line, "core.nfsc: guards: pursuit target call %u kept on its own frame (this %08x, arg %08x, from %08x)",
                 said, cpu->ecx, arg, ret);
        log_line(line);
    }
    api->hook_return(api, cpu, eax, 4u);
}

static void gd_race_walk(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu;
    (void)inv;
    (void)user;
    gd_heal_races(api);
}

/* The loader 00767d10 asks the class registry for "PVehicle" and calls a method of what it finds
 * without a null check; one report (a click on Back from the car lot) ended the game there with a call
 * to address 0. Say what the registry holds at the first loads and whenever the class is missing, so
 * the next report names the cause. */
static void gd_race_loader(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu;
    (void)inv;
    (void)user;
    static unsigned said;
    gd_heal_races(api);
    uint32_t key = 0, hash_arg = GD_PVEHICLE_STR;
    if (!api->guest_call || api->guest_call(api, GD_HASH_STR, 0, &hash_arg, 1u, &key) != POP_OK)
        return;
    uint32_t node = 0, creator = 0, n = 0;
    int bad = 0;
    if (gd_valid(api, GD_CLASS_LIST, 4u)) {
        node = get_u32(GD_CLASS_LIST);
        while (node && n < 4096u) {
            if (node < 0x10000u || !gd_valid(api, node, 12u)) {
                bad = 1;
                break;
            }
            if (get_u32(node) == key) {
                creator = get_u32(node + 4u);
                break;
            }
            node = get_u32(node + 8u);
            ++n;
        }
    }
    const int missing = !creator || bad;
    if (missing || said < 3u) {
        static unsigned missing_said;
        if (missing && ++missing_said > 12u)
            return;
        ++said;
        char line[220];
        snprintf(line, sizeof line,
                 "core.nfsc: guards: race loader: class registry head %08x, PVehicle key %08x %s (node %08x, creator %08x, %u nodes read%s)",
                 gd_valid(api, GD_CLASS_LIST, 4u) ? get_u32(GD_CLASS_LIST) : 0u, key, missing ? "MISSING" : "found",
                 node, creator, n, bad ? ", a node was unreadable" : "");
        log_line(line);
    }
}

/* UpdatePursuits (00445c70) crashed 14 times reading [pursuit + 0x144] with a pursuit pointer outside memory, most of them
 * as a chase ended. The list (eastl: head at +0x140, node = next, prev, AIPursuit * at +8) is checked before the game
 * walks it, and a node that cannot be a pursuit is cut out. A pursuit is read at +0x24 ... +0x298, so all of that must
 * exist. Cause open: this stops the crash only if the list is bad when the walk starts. */
static uint32_t g_gd_pursuit_cut;
static int gd_pursuit_node_ok(const PopModApi *api, uint32_t node) {
    if (node < 0x10000u || (node & 3u) || !gd_valid(api, node, 12u))
        return 0;
    const uint32_t p = get_u32(node + 8u);
    return p >= 0x10000u && !(p & 3u) && gd_valid(api, p, 0x2a0u);
}
static void gd_update_pursuits(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t head = cpu->ecx + 0x140u;
    if (head < 0x10000u || !gd_valid(api, head, 8u))
        return;
    uint32_t prev = head, node = get_u32(head);
    for (int n = 0; node != head && n < 64; ++n) {
        if (gd_pursuit_node_ok(api, node)) {
            prev = node;
            node = get_u32(node);
            continue;
        }
        /* cut: the node may be unreadable, so its next is only used when it can be read and leads somewhere sane;
         * otherwise the list ends here, which leaves the later pursuits unwalked but the game running */
        const uint32_t next = node >= 0x10000u && gd_valid(api, node, 4u) ? get_u32(node) : head;
        const uint32_t to = next == head || gd_pursuit_node_ok(api, next) ? next : head;
        char line[150];
        if (++g_gd_pursuit_cut <= 12u) {
            snprintf(line, sizeof line, "core.nfsc: guards: pursuit list node %08x (pursuit %08x) is bad: cut out, %u so far",
                     node, gd_valid(api, node, 12u) ? get_u32(node + 8u) : 0u, g_gd_pursuit_cut);
            log_line(line);
        }
        mp_put(prev, to);
        mp_put(to + 4u, prev);
        node = to;
    }
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
    if (g_api->hook_install_at_callsite) {
        for (int i = 0; i < 4; ++i)
            if (g_api->hook_install_at_callsite(g_api, GD_ATTR_GET, g_gd_attr_rets[i], gd_attr_get, POP_HOOK_REPLACE,
                                                NULL, &g_gd_hooks[3 + i]) != POP_OK)
                g_gd_hooks[3 + i] = 0;
        const char *off = getenv("RECOMP_GUARD_LOOKUP"); /* 0 = no central lookup guard (A/B of its cost) */
        if (off && *off == '0') {
            log_line("core.nfsc: guards: central lookup guard off (RECOMP_GUARD_LOOKUP=0)");
        } else if (g_api->hook_install(g_api, GD_ATTR_LOOKUP, gd_attr_lookup, POP_HOOK_REPLACE, NULL, &g_gd_hooks[7]) != POP_OK) {
            g_gd_hooks[7] = 0;
            log_line("core.nfsc: guards: no central lookup guard");
        }
        if (g_api->hook_install(g_api, GD_FIND_INSTANCE, gd_find_instance, POP_HOOK_REPLACE, NULL, &g_gd_hooks[9]) != POP_OK) {
            g_gd_hooks[9] = 0;
            log_line("core.nfsc: guards: no instance list guard");
        }
        if (g_api->hook_install_at_callsite(g_api, GD_TEARDOWN_CALL, GD_TEARDOWN_RET, gd_teardown, POP_HOOK_REPLACE, NULL,
                                            &g_gd_hooks[10]) != POP_OK) {
            g_gd_hooks[10] = 0;
            log_line("core.nfsc: guards: no list teardown guard");
        }
        if (g_api->hook_install_at_callsite(g_api, GD_PURSUIT_EVENT, GD_PURSUIT_EVENT_RET, gd_pursuit_event,
                                            POP_HOOK_REPLACE, NULL, &g_gd_hooks[8]) != POP_OK) {
            g_gd_hooks[8] = 0;
            log_line("core.nfsc: guards: no wrecked-cop register guard");
        }
        const char *all = getenv("RECOMP_GUARD_AI_ALL");
        g_gd_ai_all = all && *all == '1';
        if (g_api->hook_install(g_api, GD_AI_SET_TARGET, gd_ai_set_target, POP_HOOK_REPLACE, NULL, &g_gd_hooks[11]) != POP_OK) {
            g_gd_hooks[11] = 0;
            log_line("core.nfsc: guards: no pursuit target call guard");
        }
    }
    for (int i = 0; i < 3; ++i)
        if (g_api->hook_install(g_api, g_gd_race_walks[i], gd_race_walk, POP_HOOK_BEFORE, NULL, &g_gd_hooks[15 + i]) != POP_OK) {
            g_gd_hooks[15 + i] = 0;
            log_line("core.nfsc: guards: no race list walk guard");
        }
    if (g_api->hook_install(g_api, GD_UPDATE_PURSUITS, gd_update_pursuits, POP_HOOK_BEFORE, NULL, &g_gd_hooks[18]) != POP_OK) {
        g_gd_hooks[18] = 0;
        log_line("core.nfsc: guards: no pursuit list guard");
    }
    if (g_api->hook_install(g_api, GD_RACE_LOADER, gd_race_loader, POP_HOOK_BEFORE, NULL, &g_gd_hooks[14]) != POP_OK) {
        g_gd_hooks[14] = 0;
        log_line("core.nfsc: guards: no race loader note");
    }
}

static void guards_shutdown(void) {
    for (int i = 0; i < GD_NHOOKS; ++i) {
        if (g_gd_hooks[i])
            g_api->hook_remove(g_api, g_gd_hooks[i]);
        g_gd_hooks[i] = 0;
    }
}
