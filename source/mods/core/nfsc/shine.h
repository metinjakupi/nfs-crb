/* shine.h - how shiny cars are (included by nfsc.c). Setting car_shine: 0 Original (the
 * game's own look), 1 Less, 2 Matte; F10 Graphics, live.
 *
 * HOW CARBON DRAWS CAR PAINT. Every car part (paint, chrome, glass, tyres) is drawn with
 * the effect IDI_CAR_FX, technique "car" (IDI_CARNORMALMAP_FX for normal-mapped parts,
 * same parameters), with the values of the part's material (GLOBALB's light materials)
 * in eight float4 parameters. Read from the shaders' bytecode:
 *   vertex shader: f = N.V (facing the camera = 1)
 *     reflection strength = cvEnvmapMin + min(f^cvPowers.z, cvClampAndScales.y)
 *                                         * cvClampAndScales.w * cvEnvmapRange
 *     (the fresnel falloff), and the reflection vector into the car's cube map;
 *   pixel shader:
 *     colour = paint (lighting x texture, vinyls)
 *            + 2 x cube map x reflection strength                  (the "wet" mirror look)
 *            + for each of 3 lights: (cvSpecularMin + s x cvSpecularRange) x s x light,
 *              s = saturate(L.R)^cvPowers.y                         (the highlights)
 * The cube map is the game's own 256x256 A8R8G8B8 render target, one level (0070de00),
 * redrawn around the car; the bloom of the visual treatment adds glow to whatever is
 * bright afterwards.
 *
 * WHAT THIS DOES. The kit's effect shim multiplies parameters on their way to the shaders
 * (PopModApi::fx_param_scale), keyed by the effect's resource name, so only these two car
 * effects change; the world's effects (IDI_WORLD*, IDI_SKY_FX ...) share some parameter
 * names but not the effect name. Scaling Min and Range together turns the whole fresnel
 * curve down, so grazing angles keep their shape. Matte also halves the highlight's
 * exponent (cvPowers.y), so what is left of the highlight is a broad soft sheen rather
 * than a small hot spot. The paint colour itself (cvDiffuse*) is not touched. Original
 * removes every factor: the shim then binds the game's values unchanged.
 */
static const char *const k_shine_effects[] = {"IDI_CAR_FX", "IDI_CARNORMALMAP_FX"};

/* {reflection, highlights, highlight exponent} per level. */
static const float k_shine_levels[3][3] = {
    {1.0f, 1.0f, 1.0f},   /* Original */
    {0.55f, 0.6f, 1.0f},  /* Less */
    {0.12f, 0.25f, 0.5f}, /* Matte */
};

static int g_shine_applied = -1;
static uint32_t g_shine_hook;

static void shine_apply(void) {
    int64_t level = setting("car_shine", 0);
    if (level < 0 || level > 2)
        level = 0;
    if (level == g_shine_applied)
        return;
    if (g_api->size < offsetof(PopModApi, fx_param_scale) + sizeof(void *) || !g_api->fx_param_scale) {
        if (g_shine_applied < 0)
            log_line("core.nfsc: car shine needs a newer kit (no fx_param_scale)");
        g_shine_applied = (int)level;
        return;
    }
    const float *k = k_shine_levels[level];
    const float env[4] = {k[0], k[0], k[0], k[0]}, spec[4] = {k[1], k[1], k[1], k[1]};
    const float powers[4] = {1.0f, k[2], 1.0f, 1.0f};
    int ok = 1;
    for (unsigned i = 0; i < sizeof k_shine_effects / sizeof k_shine_effects[0]; ++i) {
        const char *fx = k_shine_effects[i];
        /* Four 1.0s remove the entry, so Original is the game's own values exactly. */
        ok &= g_api->fx_param_scale(g_api, fx, "cvEnvmapMin", env) == POP_OK;
        ok &= g_api->fx_param_scale(g_api, fx, "cvEnvmapRange", env) == POP_OK;
        ok &= g_api->fx_param_scale(g_api, fx, "cvSpecularMin", spec) == POP_OK;
        ok &= g_api->fx_param_scale(g_api, fx, "cvSpecularRange", spec) == POP_OK;
        ok &= g_api->fx_param_scale(g_api, fx, "cvPowers", powers) == POP_OK;
    }
    char line[96];
    snprintf(line, sizeof line, "core.nfsc: car shine %s%s", level == 0 ? "original" : level == 1 ? "less" : "matte",
             ok ? "" : " (some parameters refused)");
    log_line(line);
    g_shine_applied = (int)level;
}

/* cFEng::Service runs every frame, in menus and in races (the HUD is FEng too), so a
 * change on the F10 page shows at the next frame. */
static void shine_service(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)cpu;
    (void)inv;
    (void)user;
    shine_apply();
}

static void shine_install(void) {
    shine_apply();
    if (g_api->hook_install(g_api, 0x00600c90u, shine_service, POP_HOOK_BEFORE, NULL, &g_shine_hook) != POP_OK)
        log_line("core.nfsc: car shine changes apply after a restart (hook 00600c90 unavailable)");
}
