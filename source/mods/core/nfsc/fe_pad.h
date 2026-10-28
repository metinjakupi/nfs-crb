/* fe_pad.h - console button prompts in the menus (core.nfsc).
 *
 * After XtendedInput (xan1242 / Berkay Yigit, MIT; see NOTICE), done with
 * hooks instead of code patches. Carbon's menus (FEng) carry both prompt
 * sets: objects flagged 0x40 are the console's, 0x1008 the PC's.
 *
 * - 00583d70 (callback, arg the object, RET 4) runs as each menu loads and
 *   hides every console object; replaced, it hides whichever set is not in
 *   use and keeps a "was meant to be visible" mark in UserParam (+0x28).
 * - 005b0920 (cFEng render object, thiscall, RET 4) never draws a console
 *   object (TEST 0x40 / JNZ at 005b092d, NOP'd in game.toml); the hook skips
 *   whichever set is not in use instead.
 * - cFEng::Service 00600c90 (thiscall, RET 8): before it, every object of
 *   every menu (list at [this + 0xe0], next +4; objects from +0x4c, next +4;
 *   a group's children from +0x64) is set for the current set, as the game's
 *   scripts show and hide prompts while a menu runs.
 *
 * Which set: the controller's after any controller input, the PC's after a
 * key or a mouse click. Console prompt images name the PS2's buttons
 * (CROSS, TRIANGLE...), which the PC data does not have; they are retargeted
 * to the controller's own icons in GLOBAL/XtendedInputButtons.tpk (the
 * Setup app's optional download), loaded once at the first menu. Without
 * that file the PC prompts stay, since the console ones would have no icons.
 *
 * FEObject: +0x10 name hash, +0x18 type (1 image, 5 group), +0x1c flags
 * (bit 0 hidden; 0x2400000 dirty), +0x24 image texture hash, +0x28 UserParam. */
#include <stdatomic.h>
#include <unistd.h>

#define FE_HIDE_CALLBACK 0x00583d70u
#define FE_RENDER_OBJECT 0x005b0920u
#define FE_SERVICE 0x00600c90u
#define FE_CREATE_RESOURCE 0x006b32c0u   /* cdecl (name, type, 0, 0, 0) -> file */
#define FE_BEGIN_LOADING 0x006b5910u     /* thiscall (file, callback, param) */
#define FE_SERVICE_LOADING 0x006b68d0u
#define FE_FLAG_PC 0x1008u
#define FE_FLAG_CONSOLE 0x40u
#define FE_WAS_VISIBLE 0x01000000u
#define FE_SKIP_OBJECT 0x8f095fd5u       /* XtendedInput leaves this one alone */
#define FE_ICON_PACK "GLOBAL/XtendedInputButtons.tpk"
/* The career map keeps its prompts in two plain groups, not flagged objects
 * (XtendedInput's world map handling). */
#define WORLDMAP_INSTANCE 0x00a977f0u   /* WorldMap *, package name at +0xc */
#define WORLDMAP_GROUP_CONSOLE 0x17b1f254u
#define WORLDMAP_GROUP_PC 0xa2601d59u
#define WORLDMAP_QUICKLIST_FOCUS 0x005cdbf0u   /* WorldMap::SetQuickListInFocus */
#define WORLDMAP_QUICKLIST_UNFOCUS 0x005c3880u /* WorldMap::UnfocusQuickList */
#define WORLDMAP_FREE_ROAM_LABELS {0x0ef36117u, 0xe2dacdbcu}
#define GAMEFLOW_STATE 0x00a99bbcu             /* 3 = in the front end */
#define FE_STRING_PRINTF 0x005910e0u           /* cdecl (object, fmt, ...) */
#define GET_LOCALIZED_STRING 0x00578830u       /* cdecl (hash) -> char * */
#define LANG_BACK 0x8cd567b8u
#define CFENG_INSTANCE 0x00a97a78u
#define CFENG_IN_CONTROL 0x00572350u           /* thiscall (cFEng *, const char *package) */

static atomic_int g_fe_console;     /* 1 while the controller was used last */
static int g_fe_enabled, g_fe_have_pack, g_fe_pack_loaded;
static uint32_t g_fe_pack_path, g_fe_fmt, g_fe_quicklist_name;
static int g_fe_quicklist;

static uint32_t fe_hash(const char *s) {
    uint32_t h = 0xffffffffu;
    while (*s)
        h = h * 33u + (unsigned char)*s++;
    return h;
}

/* The console prompt images and the button each stands for in this layout
 * (XtendedInput's Carbon table: CROSS accept, TRIANGLE back, SQUARE/CIRCLE
 * buttons 4/5, L1/R1 the menu triggers, SELECT button 0 = RB, R2 and L2
 * button 1 = LB, R3/L3 buttons 2/3 = stick clicks), as Xbox One, PS4 and
 * Switch names in the pack. */
static const char *const g_fe_icons[][4] = {
    {"CROSS", "XBONE_A", "PS4_CROSS", "SWITCH_B"},
    {"TRIANGLE", "XBONE_B", "PS4_CIRCLE", "SWITCH_A"},
    {"SQUARE", "XBONE_X", "PS4_SQUARE", "SWITCH_Y"},
    {"CIRCLE", "XBONE_Y", "PS4_TRIANGLE", "SWITCH_X"},
    {"L1", "XBONE_LT", "PS4_L2", "SWITCH_ZL"},
    {"R1", "XBONE_RT", "PS4_R2", "SWITCH_ZR"},
    {"SELECT", "XBONE_RB", "PS4_R1", "SWITCH_R"},
    {"R2", "XBONE_LB", "PS4_L1", "SWITCH_L"},
    {"L2", "XBONE_LB", "PS4_L1", "SWITCH_L"},
    {"R3", "XBONE_LS", "PS4_L3", "SWITCH_LS"},
    {"L3", "XBONE_RS", "PS4_R3", "SWITCH_RS"},
    {"START", "XBONE_MENU", "PS4_START", "SWITCH_PLUS"},
    {"RIGHT_ANALOG", "XBONE_RANALOG", "PS4_RANALOG", "SWITCH_RANALOG"},
    {"RIGHT_ANALOG_UP", "XBONE_RANALOG_UP", "PS4_RANALOG_UP", "SWITCH_RANALOG_UP"},
    {"RIGHT_ANALOG_UP_SWEDISH", "XBONE_RANALOG_UP", "PS4_RANALOG_UP", "SWITCH_RANALOG_UP"},
    {"RIGHT_ANALOG_UP_FRENCH", "XBONE_RANALOG_UP", "PS4_RANALOG_UP", "SWITCH_RANALOG_UP"},
    {"RIGHT_ANALOG_DOWN", "XBONE_RANALOG_DOWN", "PS4_RANALOG_DOWN", "SWITCH_RANALOG_DOWN"},
    {"RIGHT_ANALOG_DOWN_SWEDISH", "XBONE_RANALOG_DOWN", "PS4_RANALOG_DOWN", "SWITCH_RANALOG_DOWN"},
    {"RIGHT_ANALOG_DOWN_FRENCH", "XBONE_RANALOG_DOWN", "PS4_RANALOG_DOWN", "SWITCH_RANALOG_DOWN"},
    {"LEFT_ANALOG", "XBONE_LANALOG", "PS4_LANALOG", "SWITCH_LANALOG"},
    {"LEFT_ANALOG_UP", "XBONE_LANALOG_UP", "PS4_LANALOG_UP", "SWITCH_LANALOG_UP"},
    {"LEFT_ANALOG_UP_SWEDISH", "XBONE_LANALOG_UP", "PS4_LANALOG_UP", "SWITCH_LANALOG_UP"},
    {"LEFT_ANALOG_UP_FRENCH", "XBONE_LANALOG_UP", "PS4_LANALOG_UP", "SWITCH_LANALOG_UP"},
    {"LEFT_ANALOG_DOWN", "XBONE_LANALOG_DOWN", "PS4_LANALOG_DOWN", "SWITCH_LANALOG_DOWN"},
    {"LEFT_ANALOG_DOWN_SWEDISH", "XBONE_LANALOG_DOWN", "PS4_LANALOG_DOWN", "SWITCH_LANALOG_DOWN"},
    {"LEFT_ANALOG_DOWN_FRENCH", "XBONE_LANALOG_DOWN", "PS4_LANALOG_DOWN", "SWITCH_LANALOG_DOWN"},
    {"DPAD", "XBONE_DPAD", "PS4_DPAD", "SWITCH_DPAD"},
    {"DPAD_UPDOWN", "XBONE_DPAD_Y", "PS4_DPAD_Y", "SWITCH_DPAD_Y"},
    {"DPAD_LEFTRIGHT", "XBONE_DPAD_X", "PS4_DPAD_X", "SWITCH_DPAD_X"},
    {"DPAD_UP", "XBONE_DPAD_UP", "PS4_DPAD_UP", "SWITCH_DPAD_UP"},
    {"DPAD_DOWN", "XBONE_DPAD_DOWN", "PS4_DPAD_DOWN", "SWITCH_DPAD_DOWN"},
    {"DPAD_LEFT", "XBONE_DPAD_LEFT", "PS4_DPAD_LEFT", "SWITCH_DPAD_LEFT"},
    {"DPAD_RIGHT", "XBONE_DPAD_RIGHT", "PS4_DPAD_RIGHT", "SWITCH_DPAD_RIGHT"},
};
#define FE_ICON_COUNT (sizeof g_fe_icons / sizeof g_fe_icons[0])
static uint32_t g_fe_icon_hash[FE_ICON_COUNT][4];

static int fe_console(void) {
    return g_fe_enabled && g_fe_have_pack && atomic_load(&g_fe_console);
}
static int fe_excluded(uint32_t flags) {
    return (flags & (fe_console() ? FE_FLAG_PC : FE_FLAG_CONSOLE)) != 0;
}

/* An image showing a console prompt: the current controller's icon while the
 * controller is in use, the game's own image otherwise (some of these images
 * sit in parts of a menu both prompt sets share, e.g. the career map's race
 * filter bar, where the PC game shows no button). */
static void fe_icon(uint32_t object, int family) {
    uint32_t tex = get_u32(object + 0x24);
    for (unsigned i = 0; i < FE_ICON_COUNT; ++i)
        for (unsigned k = 0; k < 4; ++k)
            if (tex == g_fe_icon_hash[i][k]) {
                uint32_t want = fe_console() ? g_fe_icon_hash[i][1 + family] : g_fe_icon_hash[i][0];
                if (tex != want) {
                    g_api->guest_write_u32(g_api, object + 0x24, want);
                    g_api->guest_write_u32(g_api, object + 0x1c, get_u32(object + 0x1c) | 0x400000u);
                }
                return;
            }
}

static void fe_set_hidden(uint32_t object, int hidden) {
    uint32_t flags = get_u32(object + 0x1c);
    uint32_t want = (hidden ? (flags | 1u) : (flags & ~1u)) | 0x2400000u;
    if ((flags & 1u) != (want & 1u))
        g_api->guest_write_u32(g_api, object + 0x1c, want);
}

/* One object: the set not in use hidden (remembering it was meant to show),
 * the set in use shown again if it was meant to. */
static void fe_object(uint32_t object, int family) {
    if (!object || get_u32(object + 0x10) == FE_SKIP_OBJECT)
        return;
    uint32_t flags = get_u32(object + 0x1c), user = get_u32(object + 0x28);
    if (flags & (FE_FLAG_PC | FE_FLAG_CONSOLE)) {
        if (fe_excluded(flags)) {
            if (!(flags & 1u)) {
                g_api->guest_write_u32(g_api, object + 0x28, user | FE_WAS_VISIBLE);
                fe_set_hidden(object, 1);
            }
        } else if (user & FE_WAS_VISIBLE) {
            g_api->guest_write_u32(g_api, object + 0x28, user & ~FE_WAS_VISIBLE);
            fe_set_hidden(object, 0);
        }
    }
    if (get_u32(object + 0x18) == 1 && g_fe_have_pack)
        fe_icon(object, family);
}

static void fe_tree(uint32_t object, unsigned depth, int family, unsigned *budget) {
    for (; object && *budget; object = get_u32(object + 4), --*budget) {
        fe_object(object, family);
        if (get_u32(object + 0x18) == 5 && depth < 16)
            fe_tree(get_u32(object + 0x64), depth + 1, family, budget);
    }
}

/* 00583d70: a menu's object as it loads. */
static void fe_hide_hook(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    if (!g_fe_enabled || !g_fe_have_pack) {
        api->call_original(api, cpu->target, cpu);
        return;
    }
    uint32_t object = get_u32(cpu->esp + 4);
    if (object) {
        uint32_t flags = get_u32(object + 0x1c);
        if (flags & FE_FLAG_CONSOLE)   /* the game's own clearing of IsButton */
            api->guest_write_u32(api, object + 0x1c, flags & ~0x10000000u);
        if ((flags & (FE_FLAG_PC | FE_FLAG_CONSOLE)) && !(flags & 1u))
            api->guest_write_u32(api, object + 0x28, get_u32(object + 0x28) | FE_WAS_VISIBLE);
        fe_object(object, pad_family());
    }
    api->hook_return(api, cpu, (cpu->eax & 0xffffff00u) | 1u, 4);
}

/* 005b0920: draws one object; the set not in use is not drawn. */
static void fe_render_hook(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    uint32_t object = get_u32(cpu->esp + 4);
    if (object && fe_excluded(get_u32(object + 0x1c))) {
        api->hook_return(api, cpu, cpu->eax, 4);
        return;
    }
    api->call_original(api, cpu->target, cpu);
}

/* Controller input switches to the console prompts (state as 0067a340 reads
 * it: axes X/Y at +0/+4, buttons from +0xc, POV at +0x220). */
static void fe_pad_activity(const PopModApi *api) {
    static uint32_t previous_buttons, previous_pov = 0xffffffffu;
    uint32_t device = get_u32(PAD_DEVICES), state = 0;
    if (!device)
        return;
    uint32_t getter = get_u32(get_u32(device) + 4);
    if (!getter || api->guest_call(api, getter, device, NULL, 0, &state) != POP_OK || !state)
        return;
    uint32_t buttons = 0;
    for (unsigned i = 0; i < 13; ++i)
        if (get_u32(state + 0xc + i * 4))
            buttons |= 1u << i;
    uint32_t pov = get_u32(state + 0x220);
    int32_t x = (int32_t)get_u32(state), y = (int32_t)get_u32(state + 4);
    int moved = x < 12768 || x > 52768 || y < 12768 || y > 52768;
    if ((buttons & ~previous_buttons) || (pov < 36000 && pov != previous_pov) || moved)
        if (!atomic_exchange(&g_fe_console, 1))
            log_line("core.nfsc: controller prompts");
    previous_buttons = buttons;
    previous_pov = pov;
}
static int32_t fe_key(const PopModApi *api, int32_t dik, int32_t vk, int32_t down, void *user) {
    (void)api;
    (void)vk;
    (void)user;
    if (down && dik != 0x44 && dik != 0x57) /* F10/F11 belong to the port's pages */
        atomic_store(&g_fe_console, 0);
    return 0;
}
static int32_t fe_mouse(const PopModApi *api, int32_t x, int32_t y, int32_t dx, int32_t dy, int32_t buttons,
                        int32_t wheel, void *user) {
    (void)api;
    (void)x;
    (void)y;
    (void)dx;
    (void)dy;
    (void)wheel;
    (void)user;
    if (buttons)
        atomic_store(&g_fe_console, 0);
    return 0;
}

static void fe_quicklist_focus(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api; (void)cpu; (void)inv;
    g_fe_quicklist = user != NULL;
}

/* The career map: the console group or the PC group (neither while the quick
 * list has focus); from the menus its console "Free Roam" button goes back. */
static void fe_world_map(const PopModApi *api) {
    uint32_t map = get_u32(WORLDMAP_INSTANCE), pkg = map ? get_u32(map + 0xc) : 0;
    if (!pkg)
        return;
    /* Only while the map or its quick list is the screen in control. */
    uint32_t feng = get_u32(CFENG_INSTANCE), map_on = 0, list_on = 0;
    if (!feng || api->guest_call(api, CFENG_IN_CONTROL, feng, &pkg, 1, &map_on) != POP_OK)
        return;
    if (g_fe_quicklist_name)
        api->guest_call(api, CFENG_IN_CONTROL, feng, &g_fe_quicklist_name, 1, &list_on);
    if (!(map_on & 0xff) && !(list_on & 0xff))
        return;
    uint32_t args[2] = {pkg, WORLDMAP_GROUP_CONSOLE}, console = 0, pc = 0;
    api->guest_call(api, FE_FIND_OBJECT, 0, args, 2, &console);
    args[1] = WORLDMAP_GROUP_PC;
    api->guest_call(api, FE_FIND_OBJECT, 0, args, 2, &pc);
    int use_console = fe_console();
    if (console)
        fe_set_hidden(console, g_fe_quicklist || !use_console);
    if (pc)
        fe_set_hidden(pc, g_fe_quicklist || use_console);
    if (use_console && get_u32(GAMEFLOW_STATE) == 3 && g_fe_fmt) {
        static const uint32_t labels[] = WORLDMAP_FREE_ROAM_LABELS;
        uint32_t hash = LANG_BACK, text = 0;
        if (api->guest_call(api, GET_LOCALIZED_STRING, 0, &hash, 1, &text) != POP_OK || !text)
            return;
        for (unsigned i = 0; i < 2; ++i) {
            uint32_t find[2] = {pkg, labels[i]}, object = 0;
            if (api->guest_call(api, FE_FIND_OBJECT, 0, find, 2, &object) == POP_OK && object) {
                uint32_t print[3] = {object, g_fe_fmt, text};
                api->guest_call(api, FE_STRING_PRINTF, 0, print, 3, NULL);
            }
        }
    }
}

/* 00600c90: cFEng::Service, once a frame while menus run. */
static void fe_service_hook(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    if (!g_fe_enabled)
        return;
    if (g_fe_have_pack && !g_fe_pack_loaded && g_fe_pack_path) {
        uint32_t args[5] = {g_fe_pack_path, 0, 0, 0, 0}, file = 0;
        const uint32_t begin[2] = {0, 0};
        g_fe_pack_loaded = 1;
        if (api->guest_call(api, FE_CREATE_RESOURCE, 0, args, 5, &file) == POP_OK && file &&
            api->guest_call(api, FE_BEGIN_LOADING, file, begin, 2, NULL) == POP_OK &&
            api->guest_call(api, FE_SERVICE_LOADING, 0, NULL, 0, NULL) == POP_OK)
            log_line("core.nfsc: controller button icons loaded (" FE_ICON_PACK ")");
        else
            log_line("core.nfsc: controller button icons could not be loaded");
    }
    if (!g_fe_have_pack)
        return;
    static int was_console = -1;
    fe_pad_activity(api);
    int console = fe_console();
    if (console != was_console && was_console >= 0)
        log_line(console ? "core.nfsc: menus show controller prompts" : "core.nfsc: menus show keyboard prompts");
    was_console = console;
    int family = pad_family();
    unsigned budget = 20000;
    for (uint32_t pkg = get_u32(cpu->ecx + 0xe0), n = 0; pkg && n < 64; pkg = get_u32(pkg + 4), ++n)
        fe_tree(get_u32(pkg + 0x4c), 0, family, &budget);
    fe_world_map(api);
}

static void fe_pad_install(const PopModApi *api) {
    static const char path[] = "GLOBAL\\XtendedInputButtons.tpk";
    g_fe_enabled = (int)setting("console_prompts", 1);
    g_fe_have_pack = access(FE_ICON_PACK, R_OK) == 0;
    for (unsigned i = 0; i < FE_ICON_COUNT; ++i)
        for (unsigned k = 0; k < 4; ++k)
            g_fe_icon_hash[i][k] = fe_hash(g_fe_icons[i][k]);
    void *data = NULL;
    if (api->guest_alloc(api, sizeof path, &g_fe_pack_path) == POP_OK &&
        api->guest_ptr(api, g_fe_pack_path, sizeof path, &data) == POP_OK && data)
        memcpy(data, path, sizeof path);
    else
        g_fe_pack_path = 0;
    if (api->guest_alloc(api, 3, &g_fe_fmt) != POP_OK || api->guest_ptr(api, g_fe_fmt, 3, &data) != POP_OK || !data)
        g_fe_fmt = 0;
    else
        memcpy(data, "%s", 3);
    static const char quicklist[] = "FeWorldMapQuickList.fng";
    if (api->guest_alloc(api, sizeof quicklist, &g_fe_quicklist_name) != POP_OK ||
        api->guest_ptr(api, g_fe_quicklist_name, sizeof quicklist, &data) != POP_OK || !data)
        g_fe_quicklist_name = 0;
    else
        memcpy(data, quicklist, sizeof quicklist);
    uint32_t id;
    api->on_key(api, fe_key, NULL, &id);
    api->on_mouse(api, fe_mouse, NULL, &id);
    /* All three or none: the renderer's console check is NOP'd in game.toml,
     * so the render hook is what keeps the console set from drawing. */
    int ok = install(FE_RENDER_OBJECT, fe_render_hook, POP_HOOK_REPLACE, 11) == POP_OK;
    if (ok && (install(FE_HIDE_CALLBACK, fe_hide_hook, POP_HOOK_REPLACE, 10) != POP_OK ||
               install(FE_SERVICE, fe_service_hook, POP_HOOK_BEFORE, 12) != POP_OK)) {
        g_fe_enabled = 0;  /* the render hook then keeps the PC prompts only */
        ok = 0;
    }
    if (!ok)
        log_line("core.nfsc: controller prompts unavailable");
    if (ok && (api->hook_install(api, WORLDMAP_QUICKLIST_FOCUS, fe_quicklist_focus, POP_HOOK_BEFORE, (void *)1,
                                 &g_hooks[13]) != POP_OK ||
               api->hook_install(api, WORLDMAP_QUICKLIST_UNFOCUS, fe_quicklist_focus, POP_HOOK_BEFORE, NULL,
                                 &g_hooks[14]) != POP_OK))
        log_line("core.nfsc: career map quick list focus unavailable");
    log_line(g_fe_have_pack ? "core.nfsc: controller icon pack found"
                            : "core.nfsc: no controller icon pack (" FE_ICON_PACK "); menus keep keyboard prompts");
}
