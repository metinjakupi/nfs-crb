/* online.h - hide the main menu's online entries (setting hide_online, default on).
 *
 * The main menu carousel is built by 0083c420: ten items, each made by 005869c0
 * (label hash, choice id) and added with 005b2320 (thiscall scroller, item; RET 4;
 * its result is ignored). Quick Match (MENU_MAIN_QUICK_MATCH, id 4) and Custom
 * Match (MENU_MAIN_CUSTOM_MATCH, id 5) are EA Online modes whose servers are gone;
 * picking either asks to connect. Their two add calls (returning to 0083c611 and
 * 0083c5c3) are skipped, so the carousel never has them; every other menu built
 * with 005b2320 is untouched.
 *   In LAN mode (RECOMP_LAN set: the start page's Online, or ?lan=) they lead to our server
 * (online_net.h, server/), so they stay, whatever the setting says. */
#include <stdlib.h>

#define FE_ADD_ITEM 0x005b2320u
#define ADD_QUICK_MATCH_RET 0x0083c611u
#define ADD_CUSTOM_MATCH_RET 0x0083c5c3u

/* One REPLACE hook filtering on the return address: the kit takes a single
 * REPLACE per function, so two callsite hooks could not both be installed. */
static void online_add_item(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t ret = get_u32(cpu->esp);
    if (ret == ADD_QUICK_MATCH_RET || ret == ADD_CUSTOM_MATCH_RET) {
        api->hook_return(api, cpu, 0, 4); /* thiscall, one argument */
        return;
    }
    api->call_original(api, cpu->target, cpu);
}

static void online_install(void) {
    const char *lan = getenv("RECOMP_LAN");
    if (lan && *lan && strcmp(lan, "0")) {
        log_line("core.nfsc: online menu entries shown (virtual LAN mode)");
        return;
    }
    if (!setting("hide_online", 1))
        return;
    if (install(FE_ADD_ITEM, online_add_item, POP_HOOK_REPLACE, 18) != POP_OK)
        log_line("core.nfsc: could not hide the online menu entries");
    else
        log_line("core.nfsc: online menu entries hidden");
}
