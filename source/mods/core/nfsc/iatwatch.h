/* iatwatch.h - who writes over the game's import table (logs/zeroed-iat-20261106).
 *
 * Player reports end in "no block entry for indirect jump to 0x00000000 from 0x0087f224": the import slots of
 * msvcr71 (_except_handler3 009c1384, free 009c1258, __CxxFrameHandler 009c121c) and winmm (timeGetTime 009c149c,
 * timeEndPeriod 009c1498) read 0 in the same session. The import table is NFSC.exe's 009c1000..009c15a8, the start of
 * .rdata (009c1000..00a4a000); nothing writes it after the loader binds it.
 *
 * Here: a copy of .text and .rdata (00401000..00a4a000) is taken on the first front-end frame with the table bound.
 * The import table is compared with the copy in BEFORE/AFTER hooks on functions every guest thread calls often
 * (the game's malloc/free thunks, its small-block free, the audio mixer's 10 ms tick, the frame), so a change is seen
 * within microseconds to a frame of the write, on whichever thread calls next. The rest of .rdata is compared every
 * 30 frames (m6-local, as logs/zeroed-iat-20261106/RESULT.md recommends: 548 KB a frame was its largest cost) and .text every 2 s (changes there are logged once, then accepted: the widescreen code writes a few .rdata
 * floats). On a change to the import table: one report (where it was seen, the thread's guest registers and call
 * chain from recomp_crash_probe, the last good check, every changed run of bytes in .text/.rdata with old and new
 * words), then the changed runs that touch the table are copied back from the copy, so the game keeps running.
 * Also: the game's malloc (006a1560) returning, or its small-block free (00609e80) taking, an address inside the
 * image is reported, with the caller.
 * RECOMP_IATWATCH=0 turns all of it off; RECOMP_IATWATCH_REPAIR=0 reports without repairing.
 * The mixer tick is NOT hooked unless RECOMP_IATWATCH_MIXER=1: the sound thread runs without the baton (game.toml
 * free_run), and a hook on a function it calls makes every call take the baton, about 300 a second, with waits of up
 * to a second - frame freezes and late sound (Dell, 2026-11-06). The other four hooks are on the game's own thread.
 */
#include <stdlib.h>
#include <time.h>

#define IW_TEXT 0x00401000u
#define IW_RDATA 0x009c1000u
#define IW_IAT_END 0x009c15a8u
#define IW_END 0x00a4a000u
#define IW_IMAGE_END 0x00c2c000u /* SizeOfImage end: .data, .rsrc and the padding the mods use */
#define IW_LEN (IW_END - IW_TEXT)
#define IW_FE_FRAME 0x006b7930u   /* called once a pass by the main loop 006b8e70 (at 006b8f93): main thread */
#define IW_FE_SERVICE 0x00600c90u /* MP_FE_SERVICE: the fallback (AFTER) when the loop function cannot be hooked */
#define IW_MALLOC 0x006a1560u     /* JMP [malloc] */
#define IW_FREE 0x006a1590u       /* JMP [free] */
#define IW_SB_FREE 0x00609e80u    /* thiscall (allocator, block, size): onto a size bucket's free list */
#define IW_MIXER 0x008a703du      /* the audio mixer thread, every 10 ms */

extern const char *recomp_crash_probe(void) __attribute__((weak));

static uint8_t *g_iw_snap;
static uint8_t *g_iw_live;
static int g_iw_state;  /* 0 off/not yet, 2 armed */
static int g_iw_busy;   /* an incident being written */
static int g_iw_repair = 1;
static double g_iw_test_s; /* TEST ONLY: RECOMP_IATWATCH_TEST=<s> zeroes 009c121c..009c14a0 once, s seconds after arming */
static uint32_t g_iw_hooks[6];
static uint64_t g_iw_checks;
static uint32_t g_iw_incidents, g_iw_notes, g_iw_bad_ptrs, g_iw_frames;
static double g_iw_t0, g_iw_last_beat;
static const char *volatile g_iw_good_site;
static volatile double g_iw_good_t;

static double iw_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int iw_iat_same(void) {
    const uint8_t *a = g_iw_live + (IW_RDATA - IW_TEXT), *b = g_iw_snap + (IW_RDATA - IW_TEXT);
    for (uint32_t i = 0; i < IW_IAT_END - IW_RDATA; i += 8) {
        uint64_t x, y;
        memcpy(&x, a + i, 8), memcpy(&y, b + i, 8);
        if (x != y) return 0;
    }
    return 1;
}

static uint32_t iw_word(const uint8_t *base, uint32_t addr) {
    uint32_t v;
    memcpy(&v, base + (addr - IW_TEXT), 4);
    return v;
}

/* Every changed run in [lo, hi) (runs closer than 16 bytes are merged). For each: logged (at most `say` runs);
 * a run touching the import table is restored when `repair`; other runs are accepted into the copy. */
static uint32_t iw_runs(uint32_t lo, uint32_t hi, int repair, int say, const char *tag) {
    uint32_t runs = 0, a = lo;
    while (a < hi) {
        uint64_t x, y;
        memcpy(&x, g_iw_live + (a - IW_TEXT), 8), memcpy(&y, g_iw_snap + (a - IW_TEXT), 8);
        if (x == y) { a += 8; continue; }
        uint32_t s = a, e = a + 8, gap = 0;
        while (e < hi && gap < 16) {
            memcpy(&x, g_iw_live + (e - IW_TEXT), 8), memcpy(&y, g_iw_snap + (e - IW_TEXT), 8);
            if (x == y) gap += 8; else gap = 0;
            e += 8;
        }
        e -= gap;
        uint32_t zero = 0, was_zero = 0;
        for (uint32_t b = s; b < e; ++b) {
            zero += g_iw_live[b - IW_TEXT] == 0;
            was_zero += g_iw_snap[b - IW_TEXT] == 0;
        }
        const int iat = s < IW_IAT_END && e > IW_RDATA;
        if ((int)runs < say) {
            char line[512];
            int n = snprintf(line, sizeof line,
                             "core.nfsc: iatwatch: %s %08x..%08x (%u bytes, %u zero now, %u zero before)%s; old", tag, s,
                             e, e - s, zero, was_zero, iat ? " IMPORT TABLE" : "");
            for (uint32_t w = s & ~3u, k = 0; w < e && k < 8 && n < (int)sizeof line - 12; w += 4, ++k)
                n += snprintf(line + n, sizeof line - n, " %08x", iw_word(g_iw_snap, w));
            n += snprintf(line + n, sizeof line - n, "; new");
            for (uint32_t w = s & ~3u, k = 0; w < e && k < 8 && n < (int)sizeof line - 12; w += 4, ++k)
                n += snprintf(line + n, sizeof line - n, " %08x", iw_word(g_iw_live, w));
            if (e - s > 32) {
                n += snprintf(line + n, sizeof line - n, "; last new");
                for (uint32_t w = (e - 16) & ~3u, k = 0; w < e && k < 4 && n < (int)sizeof line - 12; w += 4, ++k)
                    n += snprintf(line + n, sizeof line - n, " %08x", iw_word(g_iw_live, w));
            }
            log_line(line);
        }
        if (iat && repair)
            memcpy(g_iw_live + (s - IW_TEXT), g_iw_snap + (s - IW_TEXT), e - s);
        else
            memcpy(g_iw_snap + (s - IW_TEXT), g_iw_live + (s - IW_TEXT), e - s);
        ++runs;
        a = e;
    }
    return runs;
}

static void iw_incident(const char *site, const pop_cpu_v1 *cpu) {
    if (__atomic_exchange_n(&g_iw_busy, 1, __ATOMIC_ACQ_REL))
        return; /* another thread is reporting it */
    if (iw_iat_same()) { /* repaired meanwhile */
        __atomic_store_n(&g_iw_busy, 0, __ATOMIC_RELEASE);
        return;
    }
    const double now = iw_now();
    char line[1800];
    ++g_iw_incidents;
    snprintf(line, sizeof line,
             "core.nfsc: iatwatch: IMPORT TABLE CHANGED (incident %u) seen at %s, %.3f s after arming, check %llu; "
             "last good check at %s %.3f ms before; hook cpu EIP=%08x ESP=%08x ECX=%08x [esp]=%08x",
             g_iw_incidents, site, now - g_iw_t0, (unsigned long long)g_iw_checks,
             g_iw_good_site ? g_iw_good_site : "?", (now - g_iw_good_t) * 1e3, cpu ? cpu->eip : 0,
             cpu ? cpu->esp : 0, cpu ? cpu->ecx : 0, cpu ? get_u32(cpu->esp) : 0);
    log_line(line);
    if (recomp_crash_probe) {
        snprintf(line, sizeof line, "core.nfsc: iatwatch: probe: %s", recomp_crash_probe());
        log_line(line);
    }
    const uint32_t runs = iw_runs(IW_TEXT, IW_END, g_iw_repair, 24, "changed");
    snprintf(line, sizeof line, "core.nfsc: iatwatch: %u changed runs in .text/.rdata; import table %s", runs,
             iw_iat_same() ? "restored from the copy" : "NOT restored (RECOMP_IATWATCH_REPAIR=0)");
    log_line(line);
    __atomic_store_n(&g_iw_busy, 0, __ATOMIC_RELEASE);
}

static void iw_arm(void);
static inline void iw_check(const char *site, const pop_cpu_v1 *cpu) {
    int st = __atomic_load_n(&g_iw_state, __ATOMIC_ACQUIRE);
    if (st == 0) {
        int zero = 0;
        if (__atomic_compare_exchange_n(&g_iw_state, &zero, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            iw_arm(); /* sets 2, or back to 0 when the table is not bound yet, or -1 */
            if (__atomic_load_n(&g_iw_state, __ATOMIC_ACQUIRE) == 1) __atomic_store_n(&g_iw_state, 0, __ATOMIC_RELEASE);
        }
        return;
    }
    if (st != 2)
        return;
    __atomic_fetch_add(&g_iw_checks, 1, __ATOMIC_RELAXED);
    if (iw_iat_same()) {
        g_iw_good_site = site;
        g_iw_good_t = iw_now();
        return;
    }
    iw_incident(site, cpu);
}

static void iw_arm(void) {
    uint8_t *live = NULL;
    if (g_api->guest_ptr(g_api, IW_TEXT, IW_LEN, (void **)&live) != POP_OK || !live)
        return;
    uint32_t tgt;
    memcpy(&tgt, live + (0x009c149cu - IW_TEXT), 4);
    if (!tgt) /* not bound yet */
        return;
    g_iw_snap = (uint8_t *)malloc(IW_LEN);
    if (!g_iw_snap) {
        g_iw_state = -1;
        log_line("core.nfsc: iatwatch: no memory for the copy");
        return;
    }
    memcpy(g_iw_snap, live, IW_LEN);
    g_iw_live = live;
    g_iw_t0 = g_iw_last_beat = g_iw_good_t = iw_now();
    g_iw_good_site = "arming";
    __atomic_store_n(&g_iw_state, 2, __ATOMIC_RELEASE);
    char line[200];
    snprintf(line, sizeof line,
             "core.nfsc: iatwatch: armed: copy of %08x..%08x taken (import table %08x..%08x; timeGetTime -> %08x)",
             IW_TEXT, IW_END, IW_RDATA, IW_IAT_END, tgt);
    log_line(line);
}

static void iw_frame(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api, (void)inv, (void)user;
    iw_check("frame", cpu);
    if (__atomic_load_n(&g_iw_state, __ATOMIC_ACQUIRE) != 2)
        return;
    ++g_iw_frames;
    if (g_iw_test_s > 0 && iw_now() - g_iw_t0 >= g_iw_test_s) {
        g_iw_test_s = 0;
        log_line("core.nfsc: iatwatch: TEST: zeroing 009c121c..009c14a0 (RECOMP_IATWATCH_TEST)");
        memset(g_iw_live + (0x009c121cu - IW_TEXT), 0, 0x009c14a0u - 0x009c121cu);
        return; /* the next check on any thread has to find it */
    }
    if (__atomic_load_n(&g_iw_busy, __ATOMIC_ACQUIRE))
        return;
    /* .rdata past the table every 30 frames, .text every 120 frames: changes there are news, logged once. */
    uint32_t n = 0;
    if (g_iw_frames % 30 == 0)
        n += iw_runs(IW_IAT_END, IW_END, 0, g_iw_notes < 40 ? 4 : 0, "note: .rdata changed");
    if (g_iw_frames % 120 == 0)
        n += iw_runs(IW_TEXT, IW_RDATA, 0, g_iw_notes < 40 ? 4 : 0, "note: .text changed");
    g_iw_notes += n;
    const double now = iw_now();
    if (now - g_iw_last_beat >= 60.0) {
        g_iw_last_beat = now;
        char line[200];
        snprintf(line, sizeof line,
                 "core.nfsc: iatwatch: %.0f s armed, %llu checks, %u frames, %u incidents, %u notes, %u image pointers",
                 now - g_iw_t0, (unsigned long long)g_iw_checks, g_iw_frames, g_iw_incidents, g_iw_notes,
                 g_iw_bad_ptrs);
        log_line(line);
    }
}

static void iw_bad_ptr(const char *what, uint32_t p, const pop_cpu_v1 *cpu, uint32_t caller) {
    if (++g_iw_bad_ptrs > 16)
        return;
    char line[600];
    snprintf(line, sizeof line, "core.nfsc: iatwatch: %s %08x (caller %08x, ESP %08x)%s%s", what, p, caller,
             cpu->esp, recomp_crash_probe ? "; probe: " : "", recomp_crash_probe ? recomp_crash_probe() : "");
    log_line(line);
}

static void iw_malloc_after(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api, (void)inv, (void)user;
    if (cpu->eax >= 0x00400000u && cpu->eax < IW_IMAGE_END)
        iw_bad_ptr("malloc returned an image address", cpu->eax, cpu, get_u32(cpu->esp - 4));
    iw_check("malloc", cpu);
}
static void iw_free_before(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api, (void)inv, (void)user;
    const uint32_t p = get_u32(cpu->esp + 4);
    if (p >= 0x00400000u && p < IW_IMAGE_END)
        iw_bad_ptr("free of an image address", p, cpu, get_u32(cpu->esp));
    iw_check("free", cpu);
}
static void iw_sbfree_before(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api, (void)inv, (void)user;
    const uint32_t p = get_u32(cpu->esp + 4);
    if (p >= 0x00400000u && p < IW_IMAGE_END)
        iw_bad_ptr("small-block free of an image address", p, cpu, get_u32(cpu->esp));
    iw_check("sbfree", cpu);
}
static void iw_mixer_before(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api, (void)inv, (void)user;
    iw_check("mixer", cpu);
}

static void iatwatch_install(void) {
    const char *e = getenv("RECOMP_IATWATCH");
    if (e && *e && atoi(e) == 0) {
        log_line("core.nfsc: iatwatch off (RECOMP_IATWATCH=0)");
        return;
    }
    e = getenv("RECOMP_IATWATCH_REPAIR");
    g_iw_repair = !(e && *e && atoi(e) == 0);
    e = getenv("RECOMP_IATWATCH_TEST");
    g_iw_test_s = e && *e ? atof(e) : 0;
    static const struct { uint32_t addr; PopHookFn fn; int32_t mode; const char *name; } k[] = {
        {IW_FE_FRAME, iw_frame, POP_HOOK_BEFORE, "frame"},     {IW_MALLOC, iw_malloc_after, POP_HOOK_AFTER, "malloc"},
        {IW_FREE, iw_free_before, POP_HOOK_BEFORE, "free"},    {IW_SB_FREE, iw_sbfree_before, POP_HOOK_BEFORE, "sbfree"},
        {IW_MIXER, iw_mixer_before, POP_HOOK_BEFORE, "mixer"},
    };
    char line[200];
    int n = snprintf(line, sizeof line, "core.nfsc: iatwatch installed (repair %s):", g_iw_repair ? "on" : "off");
    const char *mixer = getenv("RECOMP_IATWATCH_MIXER");
    const int hook_mixer = mixer && *mixer && atoi(mixer) != 0;
    for (size_t i = 0; i < sizeof k / sizeof k[0]; ++i) {
        if (k[i].addr == IW_MIXER && !hook_mixer) {
            n += snprintf(line + n, sizeof line - n, " mixer skipped (RECOMP_IATWATCH_MIXER=1 hooks it)");
            continue;
        }
        PopModStatus s = g_api->hook_install(g_api, k[i].addr, k[i].fn, k[i].mode, NULL, &g_iw_hooks[i]);
        if (s != POP_OK && k[i].addr == IW_FE_FRAME) {
            s = g_api->hook_install(g_api, IW_FE_SERVICE, iw_frame, POP_HOOK_AFTER, NULL, &g_iw_hooks[i]);
            n += snprintf(line + n, sizeof line - n, " (loop hook refused: FE service after)");
        }
        if (s != POP_OK) g_iw_hooks[i] = 0;
        n += snprintf(line + n, sizeof line - n, " %s %s", k[i].name, s == POP_OK ? "ok" : "UNAVAILABLE");
    }
    log_line(line);
}

static void iatwatch_shutdown(void) {
    for (size_t i = 0; i < sizeof g_iw_hooks / sizeof g_iw_hooks[0]; ++i)
        if (g_iw_hooks[i]) g_api->hook_remove(g_api, g_iw_hooks[i]), g_iw_hooks[i] = 0;
    __atomic_store_n(&g_iw_state, 0, __ATOMIC_RELEASE);
}
