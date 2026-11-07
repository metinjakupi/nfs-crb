/* Tests for the crash guards in mods/core/nfsc/guards.h that need no game: a fake guest memory and API.
 *   clang -std=gnu11 -Wall -Wno-unused-function -I kit/mods tests/guards_tests.c -o $TMPDIR/guards_tests && $TMPDIR/guards_tests
 * Covers the attribute-collection chain check (valid, empty, garbage, loop), the two hooked call sites that
 * answer "not found" for a dead collection, and the register put-back after the wrecked-cop event. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pop_mod_api.h"

#define MEM 0x1000000u
static uint8_t g_mem[MEM];
static const PopModApi *g_api;
static unsigned g_logs, g_orig_calls, g_returns;
static uint32_t g_ret_eax, g_ret_bytes;
static int g_fail;

static PopModStatus t_ptr(const PopModApi *a, uint32_t addr, uint32_t len, void **out) {
    (void)a;
    if ((uint64_t)addr + len > MEM) return POP_E_RANGE;
    *out = g_mem + addr;
    return POP_OK;
}
static PopModStatus t_r32(const PopModApi *a, uint32_t addr, uint32_t *v) {
    (void)a;
    if ((uint64_t)addr + 4 > MEM) return POP_E_RANGE;
    memcpy(v, g_mem + addr, 4);
    return POP_OK;
}
static PopModStatus t_w32(const PopModApi *a, uint32_t addr, uint32_t v) {
    (void)a;
    if ((uint64_t)addr + 4 > MEM) return POP_E_RANGE;
    memcpy(g_mem + addr, &v, 4);
    return POP_OK;
}
static PopModStatus t_log(const PopModApi *a, const char *t) { (void)a; (void)t; ++g_logs; return POP_OK; }
static PopModStatus t_orig(const PopModApi *a, uint32_t addr, pop_cpu_v1 *cpu) {
    (void)a; (void)addr;
    ++g_orig_calls;
    cpu->esi = 0x006d7ec0u; /* the clobber seen in the reports */
    cpu->edi ^= 0x55u;
    return POP_OK;
}
static uint32_t g_gc_addr, g_gc_ecx, g_gc_arg, g_gc_ret, g_gc_fail;
static PopModStatus t_gcall(const PopModApi *a, uint32_t addr, uint32_t ecx, const uint32_t *args, uint32_t n, uint32_t *out) {
    (void)a;
    if (g_gc_fail) return POP_E_STATE;
    g_gc_addr = addr; g_gc_ecx = ecx; g_gc_arg = n ? args[0] : 0;
    if (addr == 0x00606b60u) { *out = 0x1234abcdu; return POP_OK; } /* the string hash */
    *out = g_gc_ret;
    return POP_OK;
}
static PopModStatus t_ret(const PopModApi *a, pop_cpu_v1 *cpu, uint32_t eax, uint32_t bytes) {
    (void)a; (void)cpu;
    ++g_returns; g_ret_eax = eax; g_ret_bytes = bytes;
    return POP_OK;
}

static uint32_t get_u32(uint32_t addr) { uint32_t v = 0; g_api->guest_read_u32(g_api, addr, &v); return v; }
static void mp_put(uint32_t addr, uint32_t v) { g_api->guest_write_u32(g_api, addr, v); }
static void log_line(const char *t) { g_api->log(g_api, t); }
static uint32_t mp_call(uint32_t fn, uint32_t a, const uint32_t *args, uint32_t n) { (void)fn; (void)a; (void)args; (void)n; return 0; }

#include "../mods/core/nfsc/guards.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while (0)

/* node at `at`: table, count, layout words, parent at +0xc */
static void node(uint32_t at, uint32_t table, uint16_t count, uint32_t parent) {
    memset(g_mem + at, 0, 0x20);
    memcpy(g_mem + at, &table, 4);
    memcpy(g_mem + at + 4, &count, 2);
    memcpy(g_mem + at + 0xc, &parent, 4);
}

int main(void) {
    static PopModApi api;
    memset(&api, 0, sizeof api);
    api.guest_ptr = t_ptr; api.guest_read_u32 = t_r32; api.guest_write_u32 = t_w32; api.log = t_log;
    api.call_original = t_orig; api.hook_return = t_ret; api.guest_call = t_gcall;
    g_api = &api;

    /* chain: child (2 entries) -> parent (empty, no table) -> grandparent (1 entry) */
    node(0x20000, 0x30000, 2, 0x21000);
    node(0x21000, 0, 0, 0x22000);
    node(0x22000, 0x31000, 1, 0);
    CHECK(gd_chain_ok(&api, 0x20000));
    CHECK(gd_chain_ok(&api, 0x21000)); /* an empty node alone is fine */

    /* the dead ones from the reports: 5, fee70117, cb01e454, 9e8910ef; and unaligned, null table with entries */
    CHECK(!gd_chain_ok(&api, 5));
    CHECK(!gd_chain_ok(&api, 0xfee70117u));
    CHECK(!gd_chain_ok(&api, 0xcb01e454u));
    CHECK(!gd_chain_ok(&api, 0x20002));
    node(0x23000, 0, 3, 0);
    CHECK(!gd_chain_ok(&api, 0x23000));
    /* a good first node whose parent is garbage */
    node(0x24000, 0x30000, 1, 0xfee70117u);
    CHECK(!gd_chain_ok(&api, 0x24000));
    /* a loop */
    node(0x25000, 0x30000, 1, 0x26000);
    node(0x26000, 0x30000, 1, 0x25000);
    CHECK(!gd_chain_ok(&api, 0x25000));
    /* the class table: [coll+0x18] != 0 -> [[coll+0x14]+4]+8 */
    node(0x27000, 0x30000, 1, 0);
    uint32_t v = 0x27100; memcpy(g_mem + 0x27000 + 0x14, &v, 4);
    v = 1;                memcpy(g_mem + 0x27000 + 0x18, &v, 4);
    v = 0x27200;          memcpy(g_mem + 0x27100 + 4, &v, 4);
    node(0x27208, 0x30000, 1, 0); /* [0x27200] + 8 */
    CHECK(gd_chain_ok(&api, 0x27000));
    v = 0xfee70117u;      memcpy(g_mem + 0x27100 + 4, &v, 4);
    CHECK(!gd_chain_ok(&api, 0x27000));

    /* attribute read: holder -> collection */
    pop_cpu_v1 cpu; pop_cpu_v1_init(&cpu);
    cpu.esp = 0x40000;
    mp_put(0x28000, 0x20000); /* holder with a good collection */
    cpu.ecx = 0x28000; g_orig_calls = g_returns = 0;
    gd_attr_get(&api, &cpu, NULL, NULL);
    CHECK(g_orig_calls == 1 && g_returns == 0);
    mp_put(0x28004, 0xfee70117u); /* holder with a dead collection */
    cpu.ecx = 0x28004; g_orig_calls = g_returns = 0; g_logs = 0;
    gd_attr_get(&api, &cpu, NULL, NULL);
    CHECK(g_orig_calls == 0 && g_returns == 1 && g_ret_eax == 0 && g_ret_bytes == 8 && g_logs == 1);
    mp_put(0x28008, 0); /* no collection: the original answers 0 itself */
    cpu.ecx = 0x28008; g_orig_calls = g_returns = 0;
    gd_attr_get(&api, &cpu, NULL, NULL);
    CHECK(g_orig_calls == 1 && g_returns == 0);
    cpu.ecx = 0xfffffff0u; g_orig_calls = g_returns = 0; /* holder outside memory */
    gd_attr_get(&api, &cpu, NULL, NULL);
    CHECK(g_orig_calls == 0 && g_returns == 1);

    /* direct lookup: the out pointer is cleared */
    mp_put(0x40008, 0x28000); /* out pointer at esp + 8 */
    mp_put(0x28000, 0x1234);
    cpu.ecx = 5; g_orig_calls = g_returns = 0;
    gd_attr_lookup(&api, &cpu, NULL, NULL);
    CHECK(g_orig_calls == 0 && g_returns == 1 && g_ret_eax == 0 && g_ret_bytes == 8 && get_u32(0x28000) == 0);
    cpu.ecx = 0x20000; g_orig_calls = g_returns = 0;
    gd_attr_lookup(&api, &cpu, NULL, NULL);
    CHECK(g_orig_calls == 1 && g_returns == 0);

    /* wrecked-cop event: a clobbered ESI/EDI are put back, EAX and the rest untouched */
    cpu.ebx = 0x1111; cpu.ebp = 0x2222; cpu.esi = 0x01038000u; cpu.edi = 0x3333; cpu.eax = 7;
    g_logs = 0;
    gd_pursuit_event(&api, &cpu, NULL, NULL);
    CHECK(cpu.esi == 0x01038000u && cpu.edi == 0x3333 && cpu.ebx == 0x1111 && cpu.ebp == 0x2222 && g_logs == 1);


    /* instance list 0061bf80: {A=2, B=1, ?, base, B entry ptr} ; entries {?, coll, holder, ...} 0x14 bytes */
    {
        uint32_t list = 0x30000;
        memset(g_mem + list, 0, 0x40);
        uint32_t A = 2, B = 1, base = 0x31000, entryB = 0x32000;
        memcpy(g_mem + list, &A, 4); memcpy(g_mem + list + 4, &B, 4); memcpy(g_mem + list + 0xc, &base, 4);
        memcpy(g_mem + list + 0x10, &entryB, 4);
        memset(g_mem + 0x31000, 0, 0x80); memset(g_mem + 0x32000, 0, 0x20);
        uint32_t coll = 0x20000; /* a good collection */
        memcpy(g_mem + 0x31000 + 4, &coll, 4);          /* entry 0: collection */
        memcpy(g_mem + 0x31000 + 0x14 + 4, &coll, 4);   /* entry 1: collection */
        memcpy(g_mem + 0x32000 + 4, &coll, 4);          /* entry B */
        CHECK(gd_list_ok(&api, list));
        uint32_t dead = 0xfee70117u; memcpy(g_mem + 0x31000 + 0x14 + 4, &dead, 4); /* entry 1: dead collection */
        CHECK(!gd_list_ok(&api, list));
        memcpy(g_mem + 0x31000 + 0x14 + 4, &coll, 4);
        /* no collection: the holder path (004613e0): holder 0 is fine, a dead holder is not */
        uint32_t zero = 0; memcpy(g_mem + 0x32000 + 4, &zero, 4);
        CHECK(gd_list_ok(&api, list));
        memcpy(g_mem + 0x32000 + 8, &dead, 4);
        CHECK(!gd_list_ok(&api, list));
        memset(g_mem + 0x32000 + 8, 0, 4);
        uint32_t big = 0x5000; memcpy(g_mem + list, &big, 4); /* absurd count */
        CHECK(!gd_list_ok(&api, list));
        CHECK(!gd_list_ok(&api, 5));
        /* the hook: a dead entry answers 0 and pops 4 */
        memcpy(g_mem + list, &A, 4); memcpy(g_mem + 0x32000 + 8, &dead, 4);
        cpu.ecx = list; g_orig_calls = g_returns = 0;
        gd_find_instance(&api, &cpu, NULL, NULL);
        CHECK(g_orig_calls == 0 && g_returns == 1 && g_ret_eax == 0 && g_ret_bytes == 4);
        memset(g_mem + 0x32000 + 8, 0, 4); g_orig_calls = g_returns = 0;
        gd_find_instance(&api, &cpu, NULL, NULL);
        CHECK(g_orig_calls == 1 && g_returns == 0);
    }

    /* list teardown 0046b140 -> 00469270 (fastcall, RET 0) */
    {
        uint32_t obj = 0x33000, link = 0x34000, inner = 0x35000;
        memset(g_mem + obj, 0, 0x40); memset(g_mem + link, 0, 0x20); memset(g_mem + inner, 0, 0x40);
        memcpy(g_mem + obj + 0x14, &link, 4); memcpy(g_mem + link + 4, &inner, 4);
        cpu.ecx = obj; g_orig_calls = g_returns = 0;
        gd_teardown(&api, &cpu, NULL, NULL);
        CHECK(g_orig_calls == 1 && g_returns == 0);
        cpu.ecx = 0x004b0018u; g_orig_calls = g_returns = 0; /* the node that pointed into the game's code */
        gd_teardown(&api, &cpu, NULL, NULL);
        CHECK(g_orig_calls == 0 && g_returns == 1 && g_ret_bytes == 0);
        cpu.ecx = 0xaaaaaaaau; g_orig_calls = g_returns = 0;
        gd_teardown(&api, &cpu, NULL, NULL);
        CHECK(g_orig_calls == 0 && g_returns == 1);
        uint32_t deadlink = 0xeeeeeeeeu; memcpy(g_mem + obj + 0x14, &deadlink, 4);
        cpu.ecx = obj; g_orig_calls = g_returns = 0;
        gd_teardown(&api, &cpu, NULL, NULL);
        CHECK(g_orig_calls == 0 && g_returns == 1);
    }


    /* the pursuit target call runs on its own frame and returns with RET 4 */
    {
        pop_cpu_v1 c; pop_cpu_v1_init(&c);
        c.esp = 0x40000; c.ecx = 0x00c0ffeeu; c.esi = 0x006d7ec0u;
        mp_put(0x40004, 0x0123abcdu);
        mp_put(0x40000, 0x00445ec4u); /* called from UpdatePursuits */
        g_gc_ret = 0x77; g_orig_calls = g_returns = 0; g_gc_fail = 0;
        gd_ai_set_target(&api, &c, NULL, NULL);
        CHECK(g_gc_addr == 0x00417800u && g_gc_ecx == 0x00c0ffeeu && g_gc_arg == 0x0123abcdu);
        CHECK(g_orig_calls == 0 && g_returns == 1 && g_ret_eax == 0x77 && g_ret_bytes == 4);
        CHECK(c.esi == 0x006d7ec0u); /* the caller's own registers stay */
        g_gc_fail = 1; g_orig_calls = g_returns = 0; /* no guest_call: the original runs as before */
        gd_ai_set_target(&api, &c, NULL, NULL);
        CHECK(g_orig_calls == 1 && g_returns == 0);
        g_gc_fail = 0;
        mp_put(0x40000, 0x00500000u); g_orig_calls = g_returns = 0; /* another caller: as before */
        gd_ai_set_target(&api, &c, NULL, NULL);
        CHECK(g_orig_calls == 1 && g_returns == 0);
        g_gd_ai_all = 1; g_orig_calls = g_returns = 0;              /* the test switch: every caller */
        gd_ai_set_target(&api, &c, NULL, NULL);
        CHECK(g_orig_calls == 0 && g_returns == 1);
        g_gd_ai_all = 0;
        mp_put(0x40000, 0x00445ec4u); g_gd_ai_busy = 1; g_orig_calls = g_returns = 0; /* back through the hook from guest_call */
        gd_ai_set_target(&api, &c, NULL, NULL);
        CHECK(g_orig_calls == 1 && g_returns == 0);
        g_gd_ai_busy = 0;
    }

    /* the loader note: registry with PVehicle, without it, and with an unreadable node */
    {
        pop_cpu_v1 c; pop_cpu_v1_init(&c);
        const uint32_t head = 0x00a9e850u, key = 0x1234abcdu;
        mp_put(0x60000, 0x1111u); mp_put(0x60004, 0x500u); mp_put(0x60008, 0x60100);        /* other class */
        mp_put(0x60100, key);     mp_put(0x60104, 0x600u); mp_put(0x60108, 0);              /* PVehicle */
        mp_put(head, 0x60000);
        unsigned before = g_logs;
        gd_race_loader(&api, &c, NULL, NULL);                 /* found: the first loads are noted */
        CHECK(g_logs == before + 1);
        mp_put(0x60104, 0);                                   /* creator missing */
        before = g_logs;
        gd_race_loader(&api, &c, NULL, NULL);
        CHECK(g_logs == before + 1);
        mp_put(0x60008, 0xfee70117u);                         /* link to garbage: stops, no fault */
        before = g_logs;
        gd_race_loader(&api, &c, NULL, NULL);
        CHECK(g_logs == before + 1);
        mp_put(head, 0);                                      /* empty registry */
        before = g_logs;
        gd_race_loader(&api, &c, NULL, NULL);
        CHECK(g_logs == before + 1);
        g_gc_fail = 1; before = g_logs;                       /* no guest_call: silent */
        gd_race_loader(&api, &c, NULL, NULL);
        CHECK(g_logs == before);
        g_gc_fail = 0;
    }

    /* the race database: a dead custom entry comes off the list, the slots with it, the live one stays */
    {
        const uint32_t db = 0x70000, live = 0x71000, dead = 0x72000;
        mp_put(0x00a9828cu, db);
        mp_put(db, 0); mp_put(db + 4, 2); mp_put(db + 0x10, dead); mp_put(db + 0x14, live);
        mp_put(db + 0x2c, dead); mp_put(db + 0x30, dead); mp_put(db + 0x34, 3);
        mp_put(live + 4, live + 0x24);
        mp_put(dead + 4, 0xfee70117u);
        CHECK(gd_heal_races(&api) == 1);
        CHECK(get_u32(db + 4) == 1 && get_u32(db + 0x10) == live && get_u32(db + 0x14) == 0);
        CHECK(get_u32(db + 0x2c) == 0 && get_u32(db + 0x30) == 0 && get_u32(db + 0x34) == 0);
        CHECK(gd_heal_races(&api) == 0);                 /* healthy: nothing changes */
        CHECK(get_u32(db + 4) == 1 && get_u32(db + 0x10) == live);
        /* the find heals and then runs the original */
        mp_put(db + 4, 2); mp_put(db + 0x14, dead);
        pop_cpu_v1 c; pop_cpu_v1_init(&c); c.esp = 0x40000; c.ecx = db;
        g_orig_calls = g_returns = 0;
        gd_find_instance(&api, &c, NULL, NULL);
        CHECK(g_orig_calls == 1 && g_returns == 0 && get_u32(db + 4) == 1);
        mp_put(db + 4, 99);                              /* not a sane count: hands off */
        CHECK(gd_heal_races(&api) == 0 && get_u32(db + 4) == 99);
        mp_put(0x00a9828cu, 0);
    }

    /* the pursuit list: good nodes stay, a bad pursuit or a bad next is cut, the list stays closed */
    {
        pop_cpu_v1 c; pop_cpu_v1_init(&c);
        const uint32_t mgr = 0x80000, head = mgr + 0x140, a = 0x81000, b = 0x81100, d = 0x81200;
        const uint32_t pa = 0x82000, pd = 0x83000;
        c.ecx = mgr;
        mp_put(head, a); mp_put(head + 4, d);
        mp_put(a, b); mp_put(a + 4, head); mp_put(a + 8, pa);
        mp_put(b, d); mp_put(b + 4, a);    mp_put(b + 8, 0xfee70117u); /* a dead pursuit */
        mp_put(d, head); mp_put(d + 4, b); mp_put(d + 8, pd);
        gd_update_pursuits(&api, &c, NULL, NULL);
        CHECK(get_u32(a) == d && get_u32(d + 4) == a && get_u32(head) == a && get_u32(d) == head && g_gd_pursuit_cut == 1);
        gd_update_pursuits(&api, &c, NULL, NULL);                       /* healthy: nothing */
        CHECK(g_gd_pursuit_cut == 1);
        mp_put(a, 0xfee70117u);                                         /* next is junk: the list ends after a */
        gd_update_pursuits(&api, &c, NULL, NULL);
        CHECK(get_u32(a) == head && get_u32(head + 4) == a && g_gd_pursuit_cut == 2);
        mp_put(head, 0); mp_put(head + 4, 0);                           /* a zero next (walked into a free chunk) */
        gd_update_pursuits(&api, &c, NULL, NULL);
        CHECK(get_u32(head) == head && g_gd_pursuit_cut == 3);
        mp_put(head, head); mp_put(head + 4, head);                     /* empty */
        gd_update_pursuits(&api, &c, NULL, NULL);
        CHECK(g_gd_pursuit_cut == 3);
    }

    /* the central lookup counts and logs the first, then every million */
    CHECK(g_gd_lookups >= 1 && g_gd_refused >= 1);

    puts(g_fail ? "FAILED" : "all guard tests passed");
    return g_fail;
}
