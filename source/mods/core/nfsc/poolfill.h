/* poolfill.h - the game's memory pools no longer fill their free memory with 0xEE (goal item 6, 2026-11-05).
 *
 * Found (logs/reflect-mem-20261105/RESULT.md): every bMalloc pool (bMemoryPool, made by 00476470) starts with its
 * debug fill on ([pool + 0x40] = 1), and 004758e0 (thiscall: a block of memory going onto the pool's free list, at the
 * pool's creation and at every free) then writes 0xEEEEEEEE over the whole block. So each pool's whole size is written
 * when it is made: the Cars pool (phone 63.5 MB, desktop 156 MB), the Audio Memory Pool (32 MB), and the main pool's
 * memory, out of which the track streamer takes its pool (phone 136 MB, desktop 228 MB, the Cars pool inside it).
 * In the browser a written page costs the tab memory for good (the arena itself is reserved without a fill since the
 * phone memory fix), so every pool cost its full size whatever it held: a page scan after a drive found every pool
 * 100% written, the Cars pool too with a few cars in it.
 *
 * The fill is only a debug aid: the game's check 00476830 compares free memory with 0xEE only while the flag is on,
 * and the game itself turns it off for the Cars pool right after making it (007e49d8: 00470de0 (pool, 0), i.e.
 * bSetMemoryPoolDebugFill) - by then the pool has been filled once. Here a BEFORE hook on 004758e0 clears the flag
 * byte of the pool it is called for (ECX), so nothing is filled: no pool changes size (no fewer cars, no less track
 * streaming memory); pages are written only when the game really puts something there.
 * RECOMP_POOLFILL=1 keeps the game's fill (A/B, troubleshooting).
 */
#define PF_ADD_FREE 0x004758e0u /* thiscall (pool), (memory, size, ...): onto the free list, filled when [pool + 0x40] */
#define PF_FLAG 0x40u
static uint32_t g_pfill_hook, g_pfill_cleared, g_pfill_calls;

static void pfill_hook(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api, (void)inv, (void)user;
    const uint32_t pool = cpu->ecx;
    ++g_pfill_calls;
    if (pool < 0x00400000u)
        return;
    const uint32_t w = get_u32(pool + PF_FLAG);
    if (w & 0xffu) { /* the fill flag is the low byte; +0x41.. are kept */
        g_api->guest_write_u32(g_api, pool + PF_FLAG, w & ~0xffu);
        if (++g_pfill_cleared <= 16u) {
            char line[160], name[24] = "";
            const uint32_t s = get_u32(pool);
            for (uint32_t i = 0; s && i + 1 < sizeof name; ++i) {
                const char ch = (char)((get_u32(s + (i & ~3u)) >> (8u * (i & 3u))) & 0xffu);
                if (ch < 32 || ch > 126) break;
                name[i] = ch, name[i + 1] = 0;
            }
            snprintf(line, sizeof line, "core.nfsc: poolfill: pool %08x \"%s\" (%u KB): no 0xEE fill", pool, name,
                     get_u32(pool + 0x20) >> 10);
            log_line(line);
        }
    }
}

static void poolfill_install(void) {
    const char *e = getenv("RECOMP_POOLFILL");
    if (e && *e && atoi(e) != 0) {
        log_line("core.nfsc: poolfill off (RECOMP_POOLFILL=1): the game's 0xEE fill of free pool memory");
        return;
    }
    if (g_api->hook_install(g_api, PF_ADD_FREE, pfill_hook, POP_HOOK_BEFORE, NULL, &g_pfill_hook) != POP_OK) {
        g_pfill_hook = 0;
        log_line("core.nfsc: poolfill: hook unavailable: the game's 0xEE fill stays");
    }
}

static void poolfill_shutdown(void) {
    if (g_pfill_hook)
        g_api->hook_remove(g_api, g_pfill_hook);
    g_pfill_hook = 0;
}
