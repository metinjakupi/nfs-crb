/* headlight_road.h - your own car's headlights light the road in front of it, the way every other
 * car's already do (included by nfsc.c after lights.h; installed after lights_install). Setting
 * own_headlight_road (Graphics, live): 0 Off (the game's own look), 1 Like other cars, 2 Bright
 * (the default).
 *
 * HOW CARBON LIGHTS THE ROAD AT NIGHT (v1.4, read in the listing and measured in the browser):
 *  1. The world shaders' headlight term. IDI_WORLD_FX, IDI_WORLDREFLECT_FX (the road, "dryroad"),
 *     IDI_WORLDNORMALMAP_FX ... light each pixel from cavWorldHeadlightPosition / Direction /
 *     UpDirection and the HEADLIGHT_XENON cookie, uploaded every frame by 00713ca0 from eView 9
 *     (0x00b48b70: the player's lamps) and eView 10 (one other car). The browser draws it with the
 *     game's exact values: a road pixel in the beam is about twice as bright as with the term
 *     switched off. But it is soft, wide and follows the car, so it reads as the street's own light.
 *     The ps_2_0 shaders (Shader Detail 2, what the "High" preset picks) use the player's lamp only;
 *     the ps_3_0 ones (Shader Detail 3, "Maximum") add the other car's and a wet sheen.
 *  2. A road-light decal. CarRenderInfo::RenderFlaresOnCar (007cbc40, thiscall: view, position,
 *     matrix, 0, reflection pass, only-these, not-the-player; RET 0x1c) calls 007be170 for a car
 *     whose headlights are on (+0x30c0 bits 1 and 2) while drawing the road's reflection: a quad
 *     lying on the road 3 to 23 m ahead of the car and 9.8 m wide (globals 0x00a731fc centre 13,
 *     0x00a73214 / 0x00a73218 far and 0x00a73224 / 0x00a73228 near half-lengths 10, 0x00a7321c /
 *     0x00a73220 / 0x00a7322c / 0x00a73230 half-widths 4.9), texture 2PLAYERHEADLIGHT1, colour
 *     the headlight block at 0x00a6ba00 (red, green, blue at +8, +0xc, +0x10, times 3), fading
 *     from 90 to 110 m from the camera. The wet road shows it through its reflection: it is the
 *     bright patch in front of an oncoming car. The game leaves it out for the local player: at
 *     007cbd2e it reads the not-the-player argument (its only use) unless the split-screen list at
 *     0x00b4b054 has entries.
 *
 * WHAT THIS DOES. A BEFORE hook on 007cbc40 turns the not-the-player argument to 1 for the player's
 * car in the reflection pass, so the game draws the player's road light itself, with its own
 * checks, size and colour - nothing else in the function reads that argument (Like other cars).
 * Seen from the chase camera through a dry road's reflection it is faint, so Bright draws the same
 * decal HLR_BRIGHT_DRAWS more times with 007be170 (it adds light: three draws, three times the
 * light; its colour is already full white, and drawing it straight on the road instead gave a
 * hard-edged white slab). Headlights switched off (lights.h clears the car's light bits during
 * this call; its hook runs before this one) leave no light on the road.
 *
 * THE FLASH (lights.h's F / L3, read from its g_lt_state & LT_FLASH; rename both here if they
 * change there). While the player flashes, the decal reaches further and is brighter: for the
 * player's call only its far edge goes from 23 to HLR_FLASH_FAR m (the near edge stays at 3 m), it
 * gets HLR_FLASH_WIDE times wider, and it is drawn HLR_FLASH_DRAWS more times. An AFTER hook puts
 * the size back. Cost: 2-4 more quads a frame in the reflection pass.
 */
#define HLR_CAR_FLARES 0x007cbc40u /* thiscall CarRenderInfo::RenderFlaresOnCar, RET 0x1c */
#define HLR_ROAD_LIGHT 0x007be170u /* thiscall: view, car-to-world matrix, 0; RET 0xc */
#define HLR_FLASH_FAR 38.0f        /* m: the flashed decal's far edge (the game's: 23) */
#define HLR_FLASH_WIDE 1.25f       /* the flashed decal's width, times the game's 9.8 m */
#define HLR_BRIGHT_DRAWS 2         /* Bright: this many more of the game's decal (it adds light) */
#define HLR_FLASH_DRAWS 2          /* a flash: this many more */

/* The decal's size: centre, far half-lengths (2), near half-lengths (2), half-widths (4). */
static const uint32_t k_hlr_size[9] = {0x00a731fcu, 0x00a73214u, 0x00a73218u, 0x00a73224u, 0x00a73228u,
                                       0x00a7321cu, 0x00a73220u, 0x00a7322cu, 0x00a73230u};
static uint32_t g_hlr_hooks[2];
static uint32_t g_hlr_matrix; /* guest: a car-to-world matrix (0x40) for our own draws */
static uint32_t g_hlr_saved[9];
static int g_hlr_restore;

static int hlr_flashing(void) {
    return (g_lt_state & LT_FLASH) != 0;
}

/* One more road light for car `ri` in view `view`, from the car's position `pos` and matrix `mat`. */
static void hlr_draw(const PopModApi *api, uint32_t ri, uint32_t view, uint32_t pos, uint32_t mat) {
    if (!g_hlr_matrix || !api->guest_call)
        return;
    /* as 007cbc40 builds its matrix: the car's rotation, its position, w 1 */
    for (uint32_t k = 0; k < 12; ++k)
        api->guest_write_u32(api, g_hlr_matrix + 4u * k, get_u32(mat + 4u * k));
    for (uint32_t k = 0; k < 3; ++k)
        api->guest_write_u32(api, g_hlr_matrix + 0x30 + 4u * k, get_u32(pos + 4u * k));
    put_float(g_hlr_matrix + 0x3c, 1.0f);
    const uint32_t args[3] = {view, g_hlr_matrix, 0u};
    uint32_t eax = 0;
    api->guest_call(api, HLR_ROAD_LIGHT, ri, args, 3, &eax);
}

static void hlr_put_back(const PopModApi *api) {
    for (int i = 0; i < 9; ++i)
        api->guest_write_u32(api, k_hlr_size[i], g_hlr_saved[i]);
}

static void hlr_car_flares(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t esp = cpu->esp;
    /* esp + 0x14: the road reflection pass, where the game draws the decal; + 0x1c: not the player */
    if (!get_u32(esp + 0x14) || get_u32(esp + 0x1c))
        return;
    const int64_t mode = setting("own_headlight_road", 2);
    if (mode <= 0)
        return;
    api->guest_write_u32(api, esp + 0x1c, 1u); /* the game draws the player's decal too */
    const uint32_t ri = cpu->ecx, pos = get_u32(esp + 8), mat = get_u32(esp + 0xc);
    if ((get_u32(ri + 0x30c0) & 3u) != 3u || !pos || !mat || g_hlr_restore)
        return;
    const int flash = hlr_flashing();
    const int extra = (mode >= 2 ? HLR_BRIGHT_DRAWS : 0) + (flash ? HLR_FLASH_DRAWS : 0);
    if (flash) {
        /* longer (same near edge) and wider, for this call only: hlr_after puts it back */
        for (int i = 0; i < 9; ++i)
            g_hlr_saved[i] = get_u32(k_hlr_size[i]);
        g_hlr_restore = 1;
        const float centre = bits_float(g_hlr_saved[0]), near_edge = centre - bits_float(g_hlr_saved[3]);
        const float half = (HLR_FLASH_FAR - near_edge) * 0.5f;
        if (isfinite(near_edge) && half > 0.0f && half < 100.0f) {
            put_float(k_hlr_size[0], near_edge + half);
            for (int i = 1; i < 5; ++i)
                put_float(k_hlr_size[i], half);
            for (int i = 5; i < 9; ++i)
                put_float(k_hlr_size[i], bits_float(g_hlr_saved[i]) * HLR_FLASH_WIDE);
        }
    }
    for (int i = 0; i < extra; ++i)
        hlr_draw(api, ri, get_u32(esp + 4), pos, mat);
}

static void hlr_after(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu;
    (void)inv;
    (void)user;
    if (!g_hlr_restore)
        return;
    hlr_put_back(api);
    g_hlr_restore = 0;
}

static void headlight_road_install(void) {
    const uint32_t quiet = POP_HOOK_NO_GAME_VIEW;
#define HLR_HOOK(fn, mode, slot)                                                                        \
    (g_api->hook_install_ex ? g_api->hook_install_ex(g_api, HLR_CAR_FLARES, 0, fn, mode, quiet, NULL, &g_hlr_hooks[slot]) \
                            : g_api->hook_install(g_api, HLR_CAR_FLARES, fn, mode, NULL, &g_hlr_hooks[slot]))
    if (HLR_HOOK(hlr_car_flares, POP_HOOK_BEFORE, 0) != POP_OK || HLR_HOOK(hlr_after, POP_HOOK_AFTER, 1) != POP_OK) {
        log_line("core.nfsc: own road light unavailable (hook 007cbc40)");
        return;
    }
#undef HLR_HOOK
    if (g_api->guest_alloc(g_api, 0x40, &g_hlr_matrix) != POP_OK)
        g_hlr_matrix = 0; /* the flash then only lengthens the light */
    log_line("core.nfsc: your headlights light the road (own_headlight_road)");
}
