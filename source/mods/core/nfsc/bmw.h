/* Local custom boost for the opening BMW. Included after rewards.h.
 * No saved tuning or shared VLT data is edited. Each active player's engine,
 * gearbox and tyre instance points at a private layout while enabled.
 * The game's Attrib destructor (00469870) releases the collection, not layout.
 * Off/car replacement restores only pointers still owned by this module.
 * Verified Carbon 1.4 paths: torque curve 006c8480; RPM/gear calculation
 * 006e3240 (final drive +0x80); wheel forces 006ee920 / 006efe10 (static +0x58,
 * dynamic +0x48, lateral +0x70, front/rear pairs). FE ratings 004b32e0
 * return 0.2 + 0.8 * normalized performance. Custom mode rates all three 1.
 */
#define BMW_KEY 0xd4421696u
static uint32_t g_bmw_mem, g_bmw_car, g_bmw_engine, g_bmw_susp;
static uint32_t g_bmw_original[3], g_bmw_hooks[2];
static int g_bmw_ready;
static int bmw_enabled(void) {
  RwShared *s = rw_shared();
  return g_bmw_ready && s && s->local == RW_LOCAL && s->bmw == 1;
}
static void bmw_restore(void) {
  if (g_bmw_car && mp_listed(g_bmw_car)) {
    if (get_u32(g_bmw_car + 0xf8) == g_bmw_engine + 0x4c &&
        get_u32(g_bmw_engine) == 0x009e5a18u) {
      if (get_u32(g_bmw_engine + 0x138) == g_bmw_mem)
        mp_put(g_bmw_engine + 0x138, g_bmw_original[0]);
      if (get_u32(g_bmw_engine + 0x148) == g_bmw_mem + 0x60)
        mp_put(g_bmw_engine + 0x148, g_bmw_original[1]);
    }
    if (get_u32(g_bmw_car + 0xf4) == g_bmw_susp + 0x44 &&
        get_u32(g_bmw_susp) == 0x009e5ee8u &&
        get_u32(g_bmw_susp + 0x90) == g_bmw_mem + 0xf0)
      mp_put(g_bmw_susp + 0x90, g_bmw_original[2]);
  }
  g_bmw_car = g_bmw_engine = g_bmw_susp = 0;
}
static void bmw_scale(uint32_t p, float factor) {
  put_float(p, bits_float(get_u32(p)) * factor);
}
static void bmw_service(const PopModApi *api, pop_cpu_v1 *cpu,
                        PopHookInvocation *inv, void *user) {
  (void)api; (void)cpu; (void)inv; (void)user;
  uint32_t car = bmw_enabled() ? mp_player() : 0;
  if (car && (get_u32(car + 0x148) != 0 || mp_model_key(car) != BMW_KEY)) car = 0;
  const uint32_t engine = car ? get_u32(car + 0xf8) - 0x4c : 0;
  const uint32_t susp = car ? get_u32(car + 0xf4) - 0x44 : 0;
  if (!car || get_u32(engine) != 0x009e5a18u || get_u32(susp) != 0x009e5ee8u) {
    bmw_restore(); return;
  }
  if (g_bmw_car == car && g_bmw_engine == engine && g_bmw_susp == susp &&
      get_u32(engine + 0x138) == g_bmw_mem &&
      get_u32(engine + 0x148) == g_bmw_mem + 0x60 &&
      get_u32(susp + 0x90) == g_bmw_mem + 0xf0) return;
  bmw_restore();
  const uint32_t eng = get_u32(engine + 0x138), trans = get_u32(engine + 0x148),
                 tyres = get_u32(susp + 0x90);
  void *a = NULL, *b = NULL, *c = NULL;
  if (!eng || !trans || !tyres ||
      g_api->guest_ptr(g_api, eng, 0x60, &a) != POP_OK ||
      g_api->guest_ptr(g_api, trans, 0x84, &b) != POP_OK ||
      g_api->guest_ptr(g_api, tyres, 0x90, &c) != POP_OK) return;
  const uint32_t count = get_u32(eng) >> 16;
  if (count < 2 || count > 9) return;
  mp_copy(g_bmw_mem, eng, 0x60);
  mp_copy(g_bmw_mem + 0x60, trans, 0x84);
  mp_copy(g_bmw_mem + 0xf0, tyres, 0x90);
  for (uint32_t i = 0; i < count; ++i) bmw_scale(g_bmw_mem + 8 + i * 4, 2.0f);
  bmw_scale(g_bmw_mem + 0x54, 1.10f); /* max RPM */
  bmw_scale(g_bmw_mem + 0x58, 1.10f); /* redline */
  bmw_scale(g_bmw_mem + 0x60 + 0x80, 0.80f); /* taller final drive */
  bmw_scale(g_bmw_mem + 0x60 + 0x7c, 0.60f); /* quicker shifts */
  for (uint32_t i = 0; i < 2; ++i) {
    bmw_scale(g_bmw_mem + 0xf0 + 0x48 + i * 4, 1.50f);
    bmw_scale(g_bmw_mem + 0xf0 + 0x58 + i * 4, 1.50f);
    bmw_scale(g_bmw_mem + 0xf0 + 0x70 + i * 4, 1.25f);
  }
  g_bmw_car = car; g_bmw_engine = engine; g_bmw_susp = susp;
  g_bmw_original[0] = eng; g_bmw_original[1] = trans; g_bmw_original[2] = tyres;
  mp_put(engine + 0x138, g_bmw_mem);
  mp_put(engine + 0x148, g_bmw_mem + 0x60);
  mp_put(susp + 0x90, g_bmw_mem + 0xf0);
}
static void bmw_rating(const PopModApi *api, pop_cpu_v1 *cpu,
                       PopHookInvocation *inv, void *user) {
  (void)inv; (void)user;
  const uint32_t out = get_u32(cpu->esp + 4), stat = get_u32(cpu->esp + 8),
                 handle = get_u32(cpu->esp + 12), profile = get_u32(cpu->esp + 16);
  api->call_original(api, cpu->target, cpu);
  if (!bmw_enabled() || !(cpu->eax & 0xff) || stat > 2 || profile != 0 || !out) return;
  const uint32_t p = rw_profile();
  if (!p) return;
  for (uint32_t i = 0; i < RW_CARS; ++i) {
    const uint32_t car = p + RW_CAR_DB + i * 0x14;
    if (get_u32(car) != handle) continue;
    if (get_u32(car + 8) == BMW_KEY && (get_u32(car + 0xc) & 0xf0002u) == 0xf0002u)
      put_float(out, 1.0f);
    break;
  }
}
static void bmw_install(void) {
  if (g_api->guest_alloc(g_api, 0x200, &g_bmw_mem) != POP_OK) return;
  const PopModStatus a = g_api->hook_install(g_api, 0x00600c90u,
      bmw_service, POP_HOOK_BEFORE, NULL, &g_bmw_hooks[0]);
  const PopModStatus b = g_api->hook_install(g_api, 0x004b32e0u,
      bmw_rating, POP_HOOK_REPLACE, NULL, &g_bmw_hooks[1]);
  g_bmw_ready = a == POP_OK && b == POP_OK;
  RwShared *s = rw_shared();
  if (g_bmw_ready && s && s->version == 3) s->version = 4;
  log_line(g_bmw_ready ? "core.nfsc: local BMW custom boost ready" : "core.nfsc: BMW custom boost unavailable");
}
