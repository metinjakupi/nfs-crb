/* Extend Carbon 1.4's original Photo Mode orbit distance. The garage uses
 * the same mover, so lend the larger clamp only to the live Photo Mode owner.
 * The native update still handles input, interpolation and camera placement. */
#define PHOTO_ZOOM_UPDATE 0x004938e0u
#define PHOTO_ZOOM_OWNER 0x00a97b10u
#define PHOTO_ZOOM_OWNER_VT 0x009d4fbcu
#define PHOTO_ZOOM_MOVER_VT 0x009c7af8u
#define PHOTO_ZOOM_FRONTEND_CAMERA 0x00ab0478u
#define PHOTO_ZOOM_FRONTEND_ACTIVE 0x00a5ec7cu
#define PHOTO_ZOOM_MAX 0x009c7364u
static uint32_t g_photo_zoom_hook;
static void photo_zoom_update(const PopModApi *api, pop_cpu_v1 *cpu,
                              PopHookInvocation *inv, void *user) {
    (void)user;
    const uint32_t owner = get_u32(PHOTO_ZOOM_OWNER);
    const uint32_t saved = get_u32(PHOTO_ZOOM_MAX);
    const int active = owner && get_u32(owner) == PHOTO_ZOOM_OWNER_VT &&
        (get_u32(owner + 0x28) == cpu->ecx ||
         (!get_u32(owner + 0x28) &&
          (get_u32(PHOTO_ZOOM_FRONTEND_ACTIVE) & 0xffu) &&
          get_u32(PHOTO_ZOOM_FRONTEND_CAMERA) == cpu->ecx)) &&
        get_u32(cpu->ecx) == PHOTO_ZOOM_MOVER_VT && saved == 0x40c80000u;
    if (active)
        api->guest_write_u32(api, PHOTO_ZOOM_MAX, 0x41a00000u); /* 6.25 -> 20 */
    api->call_next(api, inv, cpu);
    if (active)
        api->guest_write_u32(api, PHOTO_ZOOM_MAX, saved);
}
static void photo_zoom_install(void) {
    if (g_api->hook_install(g_api, PHOTO_ZOOM_UPDATE, photo_zoom_update,
                            POP_HOOK_WRAP, NULL, &g_photo_zoom_hook) != POP_OK)
        log_line("core.nfsc: extended native Photo Mode zoom unavailable");
}
static void photo_zoom_shutdown(void) {
    if (g_photo_zoom_hook) {
        g_api->hook_remove(g_api, g_photo_zoom_hook);
        g_photo_zoom_hook = 0;
    }
}
