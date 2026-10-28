/* police.h - "be the police" in free roam: cops and robbers with friends
 * (included by nfsc.c after pursuit.h; plans/shared-pursuit.md section 9).
 *
 * The page's F9 menu (web/net.js, "Be the police") sets PuShared.wish. In free
 * roam, with cops on and this player not chased, the player moves into a police
 * car (k_pp_cars, the F9 choice) at the same spot and speed, light bar on; the
 * own car is parked (inactive, not drawn) where it was. Off again: the own car
 * comes back at the police car's spot and the police car is parked for next
 * time. Cars are never removed here: removing a cop-model car could end the
 * game (plans/police-sync.md, Known problems); the world's own unload removes
 * them.
 *
 * THE SWAP is the one Pursuit Tag does when a player turns cop or racer
 * (0065d620), step by step, because most of those IVehicle methods are not
 * listed functions (no guest_call): old: ISimable::Detach(player) 006c0f70,
 * driver class 4 (IVehicle +0x68 SetDriverClass = PVehicle+0x148 then 006d3fe0,
 * 006da030), Deactivate (= SetPhysicsMode(0), IVehicle +0x38, 006d6950).
 * Pursuit Tag also drops the audio mechanic (ReleaseBehaviorAudio); here it is
 *        kept: a parked car's behaviours are paused and the sound side stops
 * polling its audio (polled 0 times a second against ~30 while driven), while a
 * racer's audio made again later was never polled, so the car came back silent
 * (2026-10-27); new: driver class of the old (0, human), the old's pose
 * (mp_place), ISimable::Attach(player) 006c6740, head lights + light bar
 * (IVehicle +0x98 |= 7 | 0x7000, GlareOn), Activate (SetPhysicsMode(1) unless
 * IVehicle +0xa8). The player (IPlayer, ISimable +0x58 = PVehicle +0x7c)
 * follows the car it is attached to, and mp.h's mp_player() is the car with a
 * player and driver class 0, so friends get the police car's model and poses.
 * While police, this game's own AI cops are off ([0x00a83a50] = 1; the value
 * before comes back after), so nothing here chases the police car; friends'
 * cars are still suspects in their own games, where this player's REPORTs
 * (pursuit.h) count like a cop's. Repairs are explicit F9 requests only,
 * while stopped and 30 seconds clear of nearby chases. Parked cars keep their
 * damage. Native damage can wreck a police car; wrecks cannot hold a chase.
 * Free roam only: not while chased, not in an event (pp_in_race); an event
 * starting around the police car gives the own car back. After the world is
 * loaded again (the Safehouse) the police car is gone with it; if the wish is
 * still on, a new one comes once in free roam again.
 */

/* The police cars the player can pick (F9; PuShared.car), the game's own models
 * (mp.h's mp_cop_model list); the patrol car first, which every free roam has
 * loaded. Another model may need a moment to stream in (built spooled, as mp.h
 * does). */
#define PP_CARS 7
static const struct {
  const char *model, *label;
} k_pp_cars[PP_CARS] = {
    {"copmidsize", "Patrol car"},     {"copsport", "Interceptor"},
    {"copghost", "Undercover"},       {"copgto", "GTO"},
    {"copsuvpatrol", "SUV"},          {"copsuvl", "Rhino"},
    {"copcross", "Cross's Corvette"},
};
#define PP_KEY_OF                                                              \
  0x00606b60u /* cdecl (const char *) -> attribute key (as 00422ef0 uses it)   \
               */
#define PP_PHYSICS                                                             \
  0x006d6950u /* IVehicle +0x38 SetPhysicsMode, thiscall (IVehicle *, int) */
#define PP_ATTACH                                                              \
  0x006c6740u /* PVehicle's ISimable +0x0c Attach, thiscall (ISimable *,       \
                 IUnknown *) */
#define PP_DETACH 0x006c0f70u /* +0x10 Detach */
#define PP_DRIVER_A                                                            \
  0x006d3fe0u /* SetDriverClass's two steps, thiscall (PVehicle *) */
#define PP_DRIVER_B 0x006da030u
#define PP_LIGHT_BAR 0x7000u
#define PP_HEADLIGHTS 0x7u
#define PP_DAMAGE_RESET                                                        \
  0x006edef0u /* IDamageable +0x10 ResetDamage, thiscall (IDamageable *) */
#define PP_REPAIR_SPEED 2.0f /* m/s: the F9 repair only this slow */
#define PP_REPAIR_WAIT_MS 30000.0 /* after the last nearby chase */
/* Roadblocks and spike strips: B / N on the keyboard (the game binds Z and X;
 * free in its defaults: B E F H J K N Q U V Y, of which E/Q/F are lights.h's,
 * H/Y chat.js's), d-pad down on a controller (a tap: a spike strip; held
 * PP_HOLD_MS: a roadblock; d-pad up engages events, left / right are the
 * blinkers). Sent (pursuit.h PU_TRAP) to the chased player nearest to the
 * police car, whose own game puts it on the road ahead of them. */
#define PP_DIK_ROADBLOCK 0x30u /* B */
#define PP_DIK_SPIKES 0x31u    /* N */
#define PP_HOLD_MS 700.0
#define PP_ROADBLOCK_MS 60000.0 /* one roadblock this often */
#define PP_SPIKES 3u            /* spike strips per chase (a player's chase) */
#define PP_TRAP_RANGE 300.0f    /* metres: the chased player it goes to */

static uint32_t g_pp_cop,
    g_pp_own; /* PVehicles: the police car, the player's own (parked) */
static uint32_t
    g_pp_presses = 0xffffffffu,
    g_pp_r3; /* the page's siren presses (wish bits 8-15); R3 last frame */
static uint32_t g_pp_repairs =
    0xffffffffu; /* the page's repair presses (wish bits 1-7) */
static double
    g_pp_refused_at; /* a repair was refused then (role bit 3 for a while) */
static _Atomic uint32_t g_pp_trap_keys; /* keys pressed since the last frame: 1
                                           roadblock, 2 spikes */
static uint32_t g_pp_key_down, g_pp_dpad; /* keys held; d-pad down held */
static double g_pp_dpad_since, g_pp_roadblock_at = -1e9;
static uint32_t g_pp_spikes_id[16], g_pp_spikes_status[16];
static uint32_t g_pp_spikes_used[16]; /* per g_pu_friends slot, this chase */
static uint32_t g_pp_key, g_pp_buf, g_pp_cops_was, g_pp_tries, g_pp_own_key;
static uint32_t g_pp_keys[PP_CARS];   /* model keys, worked out once */
static uint32_t g_pp_parked[PP_CARS]; /* a parked police car of each model
                                         (class 4, inactive), or 0 */
static int g_pp_now = -1;             /* which of them the player is in */
static double g_pp_switch_at;
static double g_pp_tried_at;
static double g_pp_chase_until; /* retained when police mode is toggled */
static int g_pp_chasing;

/* A race or other event is loaded (GRaceStatus has race parameters, [+0x6a1c],
 * as 0042a6c0 and 00655510 test it); free roam has none. */
/* Still our car: in the car list and of the model we knew (after the world is
 * loaded again a new car can have the same address; 2026-10-26, the Safehouse:
 * the new player car did). */
static int pp_same(uint32_t car, uint32_t key) {
  return car && key && mp_listed(car) && mp_model_key(car) == key;
}

static int pp_in_race(void) {
  const uint32_t rs = get_u32(MP_RACE_STATUS);
  return rs && get_u32(rs + 0x6a1c) != 0;
}

static void pp_driver(uint32_t car, uint32_t cls) {
  if (get_u32(car + 0x148) == cls)
    return;
  mp_put(car + 0x148, cls);
  mp_call(PP_DRIVER_A, car, NULL, 0);
  mp_call(PP_DRIVER_B, car, NULL, 0);
}
static void pp_physics(uint32_t car, uint32_t on) {
  if (on && get_u32(car + 0xb8 + 0xa8))
    return; /* as Activate (006c0be0) */
  mp_call(PP_PHYSICS, car + 0xb8, &on, 1);
}
static void pp_glare(uint32_t car, uint32_t on, uint32_t off) {
  const uint32_t v = get_u32(car + 0xb8 + 0x98);
  if (((v | on) & ~off) != v)
    mp_put(car + 0xb8 + 0x98, (v | on) & ~off);
}

/* Puts the player from car `from` into car `to` at from's pose. 0 if it did not
 * take. */
static int pp_swap(uint32_t from, uint32_t to) {
  const uint32_t from_sim = from + 0x24, to_sim = to + 0x24,
                 player = get_u32(from_sim + 0x58);
  const uint32_t state = mp_state(get_u32(from + 0x70));
  if (!player || !state)
    return 0;
  MpPose pose;
  mp_read(state, &pose);
  const uint32_t cls = get_u32(from + 0x148);
  mp_call(PP_DETACH, from_sim, &player, 1);
  pp_driver(from, 4);
  pp_physics(from, 0);
  pp_driver(to, cls);
  mp_place(to_sim, &pose);
  mp_call(PP_ATTACH, to_sim, &player, 1);
  pp_glare(to, PP_HEADLIGHTS, 0);
  pp_physics(to, 1);
  return get_u32(to_sim + 0x58) == player;
}

/* The police car as new: IDamageable::ResetDamage (006edef0; 006edc00 clears
 * the damage zones, the damage value and the smashed look), as an event start
 * does. */
static void pp_repair(uint32_t car) {
  const uint32_t damage = get_u32(car + 0xfc); /* IDamageable * */
  if (damage)
    mp_call(PP_DAMAGE_RESET, damage, NULL, 0);
}

/* A nearby active suspect blocks repairs and replacement cars. Remember the
 * chase across role toggles and brief lost poses so leaving police mode cannot
 * provide a fresh car while the racer is still escaping. */
static int pp_near_chase(uint32_t car, double now) {
  float here[3], there[3];
  if (!pu_pos(car + 0x24, here)) return 0;
  for (int i = 0; i < 16; ++i) {
    const PuFriend *f = &g_pu_friends[i];
    if (!f->id || !f->simable || pu_is_police(f, now) ||
        !((f->known && (f->status == 1 || f->status == 2)) ||
          (f->wanted_at > 0 && now - f->wanted_at < 2000.0)) ||
        !pu_pos(f->simable, there)) continue;
    float d = 0;
    for (int k = 0; k < 3; ++k) d += (there[k]-here[k])*(there[k]-here[k]);
    if (d <= PP_TRAP_RANGE * PP_TRAP_RANGE) return 1;
  }
  return 0;
}
static const char *pp_repair_refused(uint32_t car, double now) {
  if (g_pp_chasing) return "chase active";
  if (now < g_pp_chase_until) return "wait 30 seconds after the chase";
  if (pu_speed(car + 0x24) > PP_REPAIR_SPEED) return "moving";
  return NULL;
}

static uint32_t pp_key(int i) {
  if (!g_pp_keys[i]) {
    const char *name = k_pp_cars[i].model;
    for (uint32_t c = 0; c <= strlen(name); ++c)
      g_api->guest_write_u8(g_api, g_pp_buf + c, (uint8_t)name[c]);
    const uint32_t arg = g_pp_buf;
    g_pp_keys[i] = mp_call(PP_KEY_OF, 0, &arg, 1);
  }
  return g_pp_keys[i];
}

/* A parked police car of model i (kept from before, or built now at near's
 * pose, parked); 0 while the game refuses it (the model streaming in: tried
 * again, spooled every other time). */
static uint32_t pp_car(int i, uint32_t near) {
  const uint32_t key = pp_key(i);
  if (!pp_same(g_pp_parked[i], key) ||
      get_u32(g_pp_parked[i] + 0x148) != 4) /* parked ones only */
    g_pp_parked[i] = 0;
  if (g_pp_parked[i] || !key)
    return g_pp_parked[i];
  float pos[3];
  if (!pu_pos(near + 0x24, pos))
    return 0;
  const uint32_t sim =
      mp_build(near, 4, key, pos, 2u | 8u | (g_pp_tries & 1u), 0);
  ++g_pp_tries;
  if (!sim) {
    if (g_pp_tries <= 6) {
      char line[112];
      snprintf(
          line, sizeof line,
          "core.nfsc: police: no %s yet (the game refused it; trying again)",
          k_pp_cars[i].model);
      log_line(line);
    }
    return 0;
  }
  g_pp_parked[i] = sim - 0x24;
  pp_physics(g_pp_parked[i], 0); /* parked until the swap, in the same frame */
  return g_pp_parked[i];
}

static int pp_choice(void) {
  PuShared *sh = pu_shared();
  const uint32_t c = sh ? __atomic_load_n(&sh->car, __ATOMIC_ACQUIRE) : 0;
  return c < PP_CARS ? (int)c : 0;
}

static int pp_become(uint32_t player, double now) {
  g_pp_tried_at = now;
  const int i = pp_choice();
  const uint32_t car = pp_car(i, player);
  if (!car)
    return 0;
  g_pp_cops_was = get_u32(PU_COPS_OFF);
  mp_put(PU_COPS_OFF, 1); /* this game's own AI cops stand down */
  if (!pp_swap(player, car)) {
    mp_put(PU_COPS_OFF, g_pp_cops_was);
    log_line("core.nfsc: police: the player did not move into the police car");
    return 0;
  }
  g_pp_parked[i] = 0; /* in use */
  g_pp_cop = car;
  g_pp_key = pp_key(i);
  g_pp_now = i;
  g_pp_own = player;
  g_pp_own_key = mp_model_key(player);
  /* Reused cars keep their damage; only an explicit permitted repair resets it. */
  pp_glare(g_pp_cop, PP_LIGHT_BAR, 0);
  g_pu_police = 1;
  char line[128];
  snprintf(line, sizeof line,
           "core.nfsc: police: this player is the police (%s, light bar on, "
           "our AI cops off)",
           k_pp_cars[i].label);
  log_line(line);
  return 1;
}

/* Another police car picked while police: taken over here once the player is
 * nearly stopped. */
static void pp_switch(double now) {
  static int seen = -1;
  static double seen_at;
  const int i = pp_choice();
  if (i != seen) { /* only a choice left alone for a moment: none of the cars
                      clicked past */
    seen = i;
    seen_at = now;
  }
  if (i == g_pp_now || now - seen_at < 1500.0 ||
      now - g_pp_switch_at < 1000.0 || pp_repair_refused(g_pp_cop, now))
    return;
  g_pp_switch_at = now;
  const uint32_t car = pp_car(i, g_pp_cop);
  if (!car)
    return;
  const uint32_t old = g_pp_cop;
  pp_glare(old, 0, PP_LIGHT_BAR | PP_HEADLIGHTS);
  if (!pp_swap(old, car)) {
    log_line(
        "core.nfsc: police: the player did not move into the other police car");
    return;
  }
  if (g_pp_now >= 0)
    g_pp_parked[g_pp_now] = old;
  g_pp_parked[i] = 0;
  g_pp_cop = car;
  g_pp_key = pp_key(i);
  g_pp_now = i;
  pp_glare(car, PP_LIGHT_BAR, 0);
  char line[96];
  snprintf(line, sizeof line, "core.nfsc: police: now in the %s",
           k_pp_cars[i].label);
  log_line(line);
}

static void pp_leave(int restore_cops) {
  g_pu_police = 0;
  g_pu_siren = 0;
  if (pp_same(g_pp_own, g_pp_own_key) && pp_same(g_pp_cop, g_pp_key)) {
    pp_glare(g_pp_cop, 0, PP_LIGHT_BAR | PP_HEADLIGHTS);
    if (pp_swap(g_pp_cop, g_pp_own)) {
      if (g_pp_now >= 0)
        g_pp_parked[g_pp_now] =
            g_pp_cop; /* kept for next time (never removed) */
      log_line("core.nfsc: police: back in the player's own car");
    } else {
      log_line(
          "core.nfsc: police: the player did not move back into its own car");
    }
  }
  g_pp_now = -1;
  if (restore_cops)
    mp_put(PU_COPS_OFF, g_pp_cops_was);
  g_pp_own = 0;
}

/* The controller's R3 (right stick pressed in; port 0, read as lights.h reads
 * L3): free while driving (pad.h: R1 camera, L1 look back; freelook.h: the
 * stick itself; lights.h: L3, d-pad). */
static int pp_r3(void) {
  const uint32_t device = get_u32(PAD_DEVICES);
  uint32_t state = 0;
  if (!device)
    return 0;
  const uint32_t getter = get_u32(get_u32(device) + 4);
  if (!getter ||
      g_api->guest_call(g_api, getter, device, NULL, 0, &state) != POP_OK ||
      !state)
    return 0;
  return get_u32(state + 0xc + 11u * 4u) != 0;
}

/* The siren: the page's key (the horn key, H, while police: wish bits 8-15
 * count presses) or R3 toggles it. Its sound is the page's (web/net.js): a
 * human-driven police car has no cop AI, and the game's siren follows that. */
static void pp_siren_input(uint32_t wish) {
  const uint32_t presses = (wish >> 8) & 0xffu, r3 = (uint32_t)pp_r3();
  int toggle = g_pp_presses != 0xffffffffu && presses != g_pp_presses;
  toggle |= r3 && !g_pp_r3;
  g_pp_presses = presses;
  g_pp_r3 = r3;
  if (toggle && g_pu_police) {
    g_pu_siren = !g_pu_siren;
    log_line(g_pu_siren ? "core.nfsc: police: siren on"
                        : "core.nfsc: police: siren off");
  }
}

static int32_t pp_on_key(const PopModApi *api, int32_t dik, int32_t vk,
                         int32_t down, void *user) {
  (void)api;
  (void)vk;
  (void)user;
  const uint32_t bit = dik == (int32_t)PP_DIK_ROADBLOCK ? 1u
                       : dik == (int32_t)PP_DIK_SPIKES  ? 2u
                                                        : 0u;
  if (!bit)
    return 0;
  if (down) {
    if (!(g_pp_key_down & bit))
      atomic_fetch_or(&g_pp_trap_keys, bit);
    g_pp_key_down |= bit;
  } else {
    g_pp_key_down &= ~bit;
  }
  return 0; /* never taken from the game */
}

/* A letter the game has bound on the keyboard (mp.h fills the word, as lights.h
 * reads it). */
static int pp_bound(char letter) {
  MpShared *mp = mp_shared();
  const uint32_t keys = mp ? __atomic_load_n(&mp->keys, __ATOMIC_RELAXED) : 0;
  return (keys & MP_KEYS_KNOWN) && (keys & (1u << (letter - 'A')));
}

/* The controller's d-pad down (port 0, POV in hundredths of a degree): a tap or
 * a hold. */
static uint32_t pp_dpad(double now) {
  const uint32_t device = get_u32(PAD_DEVICES);
  uint32_t state = 0, held = 0;
  if (device) {
    const uint32_t getter = get_u32(get_u32(device) + 4);
    if (getter &&
        g_api->guest_call(g_api, getter, device, NULL, 0, &state) == POP_OK &&
        state) {
      const uint32_t pov = get_u32(state + 0x220);
      held = pov >= 13500u && pov <= 22500u;
    }
  }
  uint32_t out = 0;
  if (held && !g_pp_dpad)
    g_pp_dpad_since = now;
  if (held && g_pp_dpad_since > 0 && now - g_pp_dpad_since >= PP_HOLD_MS) {
    out = 1; /* held: a roadblock, once */
    g_pp_dpad_since = -1;
  }
  if (!held && g_pp_dpad && g_pp_dpad_since > 0)
    out = 2; /* let go early: a spike strip */
  g_pp_dpad = held;
  return out;
}

/* A roadblock (1) or a spike strip (2) for the chased player nearest to our
 * police car. */
static void pp_trap(uint32_t kind, double now) {
  PuShared *sh = pu_shared();
  float here[3];
  if (!sh ||
      __atomic_load_n(&sh->tx_head, __ATOMIC_RELAXED) -
              __atomic_load_n(&sh->tx_tail, __ATOMIC_ACQUIRE) >=
          PU_TX ||
      !pu_pos(g_pp_cop + 0x24, here))
    return;
  int best = -1;
  float best_d = PP_TRAP_RANGE;
  for (int i = 0; i < 16; ++i) {
    const PuFriend *f = &g_pu_friends[i];
    float at[3], d = 0;
    const int chased = f->known && (f->status == 1 || f->status == 2);
    if (!chased)
      g_pp_spikes_used[i] = 0; /* a new chase, new strips */
    if (!f->id || !f->simable || !chased || pu_is_police(f, now) ||
        !pu_pos(f->simable, at))
      continue;
    for (int k = 0; k < 3; ++k)
      d += (at[k] - here[k]) * (at[k] - here[k]);
    d = sqrtf(d);
    if (d < best_d) {
      best_d = d;
      best = i;
    }
  }
  const char *refused = best < 0 ? "nobody chased near" : NULL;
  if (!refused && kind == 1 && now - g_pp_roadblock_at < PP_ROADBLOCK_MS)
    refused = "one a minute";
  if (!refused && kind == 2 && g_pp_spikes_used[best] >= PP_SPIKES)
    refused = "3 spike strips a chase";
  char line[128];
  if (refused) {
    snprintf(line, sizeof line, "core.nfsc: police: no %s now (%s)",
             kind == 1 ? "roadblock" : "spike strip", refused);
    log_line(line);
    return;
  }
  if (kind == 1)
    g_pp_roadblock_at = now;
  else
    ++g_pp_spikes_used[best];
  pu_send(sh, g_pu_friends[best].id, PU_TRAP, kind, 0, 0, best_d, 0, 0);
  snprintf(
      line, sizeof line, "core.nfsc: police: %s for player %u (%.0f m away)",
      kind == 1 ? "roadblock" : "spike strip", g_pu_friends[best].id, best_d);
  log_line(line);
}

/* Publish the closest suspect for the page's police HUD. Values use spare
 * version-3 words. */
static void pp_hud(double now) {
  PuShared *sh = pu_shared();
  if (!sh)
    return;
  uint32_t target = 0, status = 0, heat = 0, distance = 0;
  float here[3], there[3], best = PP_TRAP_RANGE * PP_TRAP_RANGE;
  if (g_pu_police && pu_pos(g_pp_cop + 0x24, here)) {
    for (int i = 0; i < 16; ++i) {
      const PuFriend *f = &g_pu_friends[i];
      if (!f->id || !f->simable || !f->known || !f->status ||
          pu_is_police(f, now) || !pu_pos(f->simable, there))
        continue;
      float d = 0;
      for (int k = 0; k < 3; ++k)
        d += (there[k] - here[k]) * (there[k] - here[k]);
      if (d < best) {
        best = d;
        target = f->id;
        status = f->status;
        heat = (uint32_t)f->heat;
        distance = (uint32_t)sqrtf(d);
      }
    }
  }
  sh->spare2[0] = target;
  sh->spare2[1] = status;
  sh->spare2[2] = heat;
  sh->spare2[3] = distance;
  sh->spare2[5] = g_pp_chasing ? 31u : now < g_pp_chase_until ? (uint32_t)ceil((g_pp_chase_until-now)/1000.0) : 0u;
  sh->spare2[6] = g_pu_police && !pu_police_ready(g_pp_cop);
  sh->spare2[4] =
      now - g_pp_roadblock_at >= PP_ROADBLOCK_MS
          ? 0
          : (uint32_t)((PP_ROADBLOCK_MS - (now - g_pp_roadblock_at)) / 1000.0) +
                1;
}

/* pursuit.h, once a frame with the player's car: returns the role for the page.
 */
static uint32_t pp_tick(uint32_t player, uint32_t wish_word, double now) {
  pp_siren_input(wish_word);
  for (int i = 0; i < 16; ++i) {
    const PuFriend *f = &g_pu_friends[i];
    const uint32_t active = f->known && (f->status == 1 || f->status == 2);
    if (g_pp_spikes_id[i] != f->id || !active || !g_pp_spikes_status[i])
      g_pp_spikes_used[i] = 0;
    g_pp_spikes_id[i] = f->id;
    g_pp_spikes_status[i] = active;
  }
  g_pp_chasing = pp_near_chase(player, now);
  if (g_pp_chasing) g_pp_chase_until = now + PP_REPAIR_WAIT_MS;
  const uint32_t wish = wish_word & 1u;
  if (g_pu_police && (player != g_pp_cop || !pp_same(g_pp_cop, g_pp_key))) {
    /* The world was loaded again (Safehouse, an event, ...): its cars are new.
     * Back in free roam our cops switch goes back as it was (an event sets its
     * own, so it is left there). */
    g_pu_police = 0;
    g_pu_siren = 0;
    g_pp_cop = g_pp_own = 0;
    g_pp_now = -1;
    memset(g_pp_parked, 0, sizeof g_pp_parked); /* gone with the world */
    if (!pp_in_race() && get_u32(PU_COPS_OFF) == 1)
      mp_put(PU_COPS_OFF, g_pp_cops_was);
    log_line("core.nfsc: police: the world changed; police car gone");
  }
  if (g_pu_police && pp_in_race()) { /* an event started around the police car:
                                        the own car back */
    pp_leave(0);
    return wish ? 2u : 0u;
  }
  if (wish && !g_pu_police) {
    /* Only in free roam with the game's cops running, and not while chased. */
    if (!pu_cops_on() || g_pu_mine || pp_in_race() || now < g_pp_chase_until) {
      pp_hud(now);
      return 2;
    }
    if (now - g_pp_tried_at > 2000.0 && !pp_become(player, now))
      return 2;
  } else if (!wish && g_pu_police) {
    pp_leave(1);
  }
  const uint32_t repairs = (wish_word >> 1) & 0x7fu;
  if (g_pp_repairs != 0xffffffffu && repairs != g_pp_repairs && g_pu_police) {
    const char *why = pp_repair_refused(g_pp_cop, now);
    if (!why) {
      pp_repair(g_pp_cop);
      log_line("core.nfsc: police: police car repaired");
    } else {
      g_pp_refused_at = now;
      char line[96];
      snprintf(line, sizeof line, "core.nfsc: police: no repair now (%s)", why);
      log_line(line);
    }
  }
  g_pp_repairs = repairs;
  if (g_pu_police)
    pp_switch(now);
  {
    uint32_t traps = atomic_exchange(&g_pp_trap_keys, 0u);
    if ((traps & 1u) && pp_bound('B')) {
      traps &= ~1u; /* the game's own key */
      log_line("core.nfsc: police: B is bound in the game's controls: the "
               "roadblock is on d-pad down (hold) only");
    }
    if ((traps & 2u) && pp_bound('N')) {
      traps &= ~2u;
      log_line("core.nfsc: police: N is bound in the game's controls: the "
               "spike strip is on d-pad down (tap) only");
    }
    traps |= pp_dpad(now); /* 1 held: a roadblock, 2 tapped: a spike strip */
    if (g_pu_police && pu_police_ready(g_pp_cop) && (traps & 1u))
      pp_trap(1, now);
    else if (g_pu_police && pu_police_ready(g_pp_cop) && (traps & 2u))
      pp_trap(2, now);
  }
  if (g_pu_police) {
    pp_glare(g_pp_cop, PP_LIGHT_BAR | PP_HEADLIGHTS, 0);
    /* Native damage may wreck the car. A wreck cannot call or hold a chase. */
    if (!pu_police_ready(g_pp_cop)) g_pu_siren = 0;
  }
  pp_hud(now);
  return (g_pu_police ? 1u
          : wish      ? 2u
                      : 0u) |
         (g_pu_police && g_pu_siren ? 4u : 0u) |
         (g_pu_police && now - g_pp_refused_at < 4000.0 ? 8u : 0u) |
         (g_pu_police && g_pp_now >= 0 ? (uint32_t)(g_pp_now + 1) << 4
                                       : 0u); /* bits 4-7: the car in use + 1 */
}

static void pp_idle(void) {
  memset(g_pp_parked, 0, sizeof g_pp_parked);
  atomic_store(&g_pp_trap_keys, 0u);
  g_pp_dpad = 0;
  g_pp_dpad_since = 0;
}

static void police_install(void) {
  if (!g_pu_block || g_api->guest_alloc(g_api, 0x40, &g_pp_buf) != POP_OK) {
    log_line("core.nfsc: police players unavailable");
    return;
  }
  g_pu_police_tick = pp_tick;
  g_pu_police_idle = pp_idle;
  uint32_t id = 0;
  if (g_api->on_key)
    g_api->on_key(g_api, pp_on_key, NULL, &id);
}
