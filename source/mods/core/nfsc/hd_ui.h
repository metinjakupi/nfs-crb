/* hd_ui.h - HD fonts and cursor (included by nfsc.c).
 *
 * The HD UI texture pack (Remastered UI, installed by the Setup app) replaces
 * the fonts and the cursor with bigger textures. Carbon lays text and the
 * cursor out from the texture's stored size (TextureInfo +0x28 width, +0x2a
 * height: 00585d20 in 00585d10 and 0071ca06 in 0071c770 for fonts, 005853a9..
 * 005853f1 in 00585290 for the cursor), so bigger textures would draw bigger.
 * On Windows Aero_'s HD Content Support plugin makes those reads use the
 * original sizes. Here the original sizes are written back into the records
 * once a frame: the GPU texture was already made from the real pixels and is
 * sampled with normalised coordinates, so it stays sharp. Without the pack the
 * sizes already match and nothing is written.
 */
#define GET_TEXTURE_INFO 0x0055cfd0u /* cdecl (hash, return_default, include_unloaded) -> TextureInfo* */

static const struct {
    uint32_t hash;
    int16_t w, h;
} kHdOriginal[] = {
    {0x94AF4029u, 256, 256}, /* FONT_NFS_BODY */
    {0x2AB5F1C2u, 256, 256}, /* FONT_NFS_DEBUG */
    {0x2BD9F6BDu, 512, 256}, /* FONT_NFS_TITLE */
    {0xAFC18AC5u, 512, 512}, /* FONT_NFS_MOVIE_LARGE */
    {0xEA23AE36u, 128, 64},  /* FONT_NFS_TACH_NUMBERS */
    {0x6B6A294Fu, 16, 16},   /* PC_CURSOR */
};
static uint32_t g_hd_info[sizeof kHdOriginal / sizeof kHdOriginal[0]];
static unsigned g_hd_fixed;

/* Before cFEng::Service (00600c90), once a frame while menus or the HUD run. */
static void hd_ui_service(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)cpu;
    (void)inv;
    (void)user;
    for (unsigned i = 0; i < sizeof kHdOriginal / sizeof kHdOriginal[0]; ++i) {
        const uint32_t args[3] = {kHdOriginal[i].hash, 0, 0};
        uint32_t info = 0;
        if (api->guest_call(api, GET_TEXTURE_INFO, 0, args, 3, &info) != POP_OK || !info ||
            get_u32(info + 0x0c) != kHdOriginal[i].hash)
            continue;
        const uint32_t size = get_u32(info + 0x28);
        const uint32_t original = (uint16_t)kHdOriginal[i].w | (uint32_t)(uint16_t)kHdOriginal[i].h << 16;
        if (size == original)
            continue;
        api->guest_write_u32(api, info + 0x28, original);
        if (g_hd_info[i] != info) {
            g_hd_info[i] = info;
            char line[112];
            snprintf(line, sizeof line, "core.nfsc: HD texture %08x (%ux%u) laid out at its original %dx%d", kHdOriginal[i].hash,
                     size & 0xffff, size >> 16, kHdOriginal[i].w, kHdOriginal[i].h);
            log_line(line);
            ++g_hd_fixed;
        }
    }
}

static void hd_ui_install(void) {
    if (install(0x00600c90u, hd_ui_service, POP_HOOK_BEFORE, 16) != POP_OK)
        log_line("core.nfsc: HD font/cursor sizes unavailable");
}
