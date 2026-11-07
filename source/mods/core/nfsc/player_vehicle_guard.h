/* Player access only. Do not prevent AI police, traffic or cinematic creation. */
static uint32_t g_player_vehicle_hooks[3];

/* 00572b90 returns UserProfile before callers read Career's selected handle.
 * 004a0890 reads +242e8; 004a08b0 and Car Select 00865d9f write that field.
 * Change only the selection, never the blocked car/customization records. */
static void player_vehicle_profile(const PopModApi *api, pop_cpu_v1 *cpu,
                                   PopHookInvocation *inv, void *user) {
  (void)inv; (void)user;
  const uint32_t profile = cpu->eax;
  if (!profile) return;
  const uint32_t db = profile + RW_CAR_DB;
  const uint32_t selected = get_u32(profile + 0x242e8u);
  if (selected == 0xffffffffu) return;
  static uint32_t cached_profile, cached_record;
  uint32_t record = selected < RW_CARS ? db + selected * 0x14u : 0;
  if (!record || get_u32(record) != selected) {
    record = cached_profile == profile && cached_record &&
        get_u32(cached_record) == selected ? cached_record : 0;
    for (uint32_t i = 0; !record && i < RW_CARS; ++i)
      if (get_u32(db + i * 0x14u) == selected) record = db + i * 0x14u;
  }
  cached_profile = profile;
  cached_record = record;
  if (!record || !player_vehicle_blocked(get_u32(record + 8))) return;
  for (uint32_t i = 0; i < RW_CARS; ++i) {
    const uint32_t candidate = db + i * 0x14u;
    const uint32_t handle = get_u32(candidate), flags = get_u32(candidate + 0xc);
    uint8_t custom = 0xff;
    api->guest_read_u8(api, candidate + 0x10, &custom);
    if (handle != 0xffffffffu && (flags & 0xf0002u) == 0xf0002u &&
        custom < RW_CUSTOMS && !player_vehicle_blocked(get_u32(candidate + 8))) {
      api->guest_write_u32(api, profile + 0x242e8u, handle);
      log_line("core.nfsc: selected a normal owned car; blocked saved vehicle retained");
      return;
    }
  }
  /* No legitimate owned car: keep the record intact and deny driving it.
   * Do not invent ownership, charge cash, or remove a saved vehicle. */
}

static void player_vehicle_purchase(const PopModApi *api, pop_cpu_v1 *cpu,
                                    PopHookInvocation *inv, void *user) {
  (void)inv; (void)user;
  if (player_vehicle_record_blocked(get_u32(cpu->esp + 4))) {
    cpu->eax = 0;
    return;
  }
  api->call_original(api, cpu->target, cpu);
}

static void player_vehicle_construct(const PopModApi *api, pop_cpu_v1 *cpu,
                                     PopHookInvocation *inv, void *user) {
  (void)inv; (void)user;
  const uint32_t params = get_u32(cpu->esp + 8);
  const uint32_t driver = params ? get_u32(params + 0x10) : 0xffffffffu;
  if (driver == 0u || driver == 6u) { /* local player / remote human */
    uint32_t collection = get_u32(params + 0x14);
    if (collection && (get_u32(params + 0x20) & 1u))
      collection = get_u32(collection + 0xc);
    if (collection && player_vehicle_blocked(get_u32(collection + 0x10))) {
      cpu->eax = 0;
      return;
    }
  }
  api->call_original(api, cpu->target, cpu);
}

static void player_vehicle_guard_install(void) {
  const PopModStatus purchase = g_api->hook_install(g_api, 0x004d1620u,
      player_vehicle_purchase, POP_HOOK_REPLACE, NULL, &g_player_vehicle_hooks[0]);
  const PopModStatus construct = g_api->hook_install(g_api, 0x006daa80u,
      player_vehicle_construct, POP_HOOK_REPLACE, NULL, &g_player_vehicle_hooks[1]);
  const PopModStatus profile = g_api->hook_install(g_api, 0x00572b90u,
      player_vehicle_profile, POP_HOOK_AFTER, NULL, &g_player_vehicle_hooks[2]);
  if (purchase != POP_OK || construct != POP_OK || profile != POP_OK)
    log_line("core.nfsc: player vehicle guard could not be fully installed");
}
