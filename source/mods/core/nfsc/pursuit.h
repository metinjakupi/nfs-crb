/* pursuit.h - one police chase shared by friends in free roam (included by
 * nfsc.c).
 *
 * "Local cops, shared suspects" (plans/shared-pursuit.md). Every game keeps
 * running its own police near its own player, as it always has; nothing here
 * moves a cop between games.
 *
 * FRIENDS ARE SUSPECTS. mp.h builds each friend's own car as driver class 6
 * (g_mp_friend_kind: RBRemote + AIVehicleHuman, the game's class for an online
 * human player), which carries heat (IPerpetrator) and sits in the players list
 * the patrol cops watch. So this game's cops notice, chase, box and bust a
 * friend's car exactly as they do the local player's. Its heat is written every
 * frame from what the friend's own game says it is.
 *
 * EACH PLAYER'S OWN GAME OWNS THAT PLAYER'S CHASE: its AIPursuit of its own car
 * decides heat, the HUD, the music, busted and escaped. This game's pursuit of
 * a friend's car is an ASSIST:
 *   - it starts when ours is chased and the friend is right beside us
 * (PU_BESIDE), or when a patrol here picks the friend up; a chased friend only
 * passing by is not joined (its own cops are shown here as copies, cops.h);
 *   - it never busts the friend's car here: each frame the rise of its bust
 * meter (+0x138) is taken out, summed, and sent to the friend as bust progress;
 *   - it reports whether its cops see the friend ("seen") and how many it has;
 *   - it is talked into escaping (cooldown done) when the friend's chase ends
 * (busted or escaped). The friend's game folds those reports into its own
 * pursuit: "seen" keeps it chasing (in sight, cooldown back to full), and bust
 * progress is added to its bust meter only while its own car is really slow
 * (PU_SLOW), on its wheels and with another car beside it (pu_boxed): a copy
 * that looks stopped in a lagging game cannot bust a player who is driving
 * away, launched or stuck on top of a car. Bust and escape then run the game's
 * own flow (BUSTED!, the arrest scene; the pursuit's own escape). When a
 * friend's game reports that this player is seen and this player is not in a
 * chase yet, one is started here (PursueAtHeatLevel), and this game's police
 * join in.
 *
 * MESSAGES (web/net.js, relay {t:"pursuit", to, k, a, b, f};
 * tools/carbon-net-relay.mjs header, "Shared police chase"). The page and this
 * file share PuShared (globalThis.carbonPursuitBlock, "carbon-pursuit" event):
 * a ring of records to send and a ring of records received. k 1 STATE  to 0
 * (the players that get this car): a = status (0 none, 1 chase, 2 searching, 3
 * busted, 4 escaped), b = pursuit level; f = heat, bust meter, evade 0..1,
 * cops. Every PU_STATE_MS, and at once when the status changes. k 2 REPORT to
 * the player whose car it is about: a = bit 0 seen, b = cops on it here; f[0] =
 * bust progress (seconds of bust meter since the last report). Every
 *              PU_REPORT_MS while an assist runs.
 *   k 3 ROLE   to 0: a bit 0 = this player is the police (police.h), bit 1 =
 * its siren is on. Every PU_ROLE_MS and at once on a change. A police player's
 * car is built as class 7 by the others (never a suspect there); its game sends
 * REPORTs from its own car: a bit 1 "wanted" (f[2] = why: 1 siren, 2 speeding,
 * 3 hit something, 4 rammed the police car) starts the suspect's chase; once
 * chased, bit 0 "seen" (within PU_POLICE_SEE) keeps it going and bust progress
 * builds beside a slow suspect, taken like any friend's (same lag guard). A
 * police player's "seen" alone starts nothing. Other k values are free for
 * later.
 *
 * FRIENDS' BUST PROGRESS is held, not just added: the game lowers the bust
 * meter quickly while none of its own cops boxes the player in, so friends'
 * progress (PU_HOLD_MS after the last report) keeps the meter at least at what
 * friends' cops have built up. COPS WHEN TOGETHER: while a chased friend is
 * within PU_TOGETHER (or we are chased and one is), this game's cop manager
 * runs at most PU_TOGETHER_COPS cops (its +0x94; the game's value comes back
 * when apart), so two games together show about as many as one alone.
 *
 * THE GAME (NFSC.exe v1.4; addresses in plans/shared-pursuit.md): the cop
 * manager is [0x00a9e874] - 0x4c, its pursuit list at +0x140 (eastl list,
 * AIPursuit * at node + 8); a pursuit's suspect is [[pursuit + 0xd0] + 0x1c].
 * Cops on/off: [0x00a83a50] (1 = off, as in races): nothing starts while it is
 * set.
 */

#define PU_MAGIC 0x31535043u /* "CPS1" */
#define PU_TX 16
#define PU_RX 32
#define PU_STATE 1u
#define PU_REPORT 2u
#define PU_ROLE 3u
#define PU_TRAP                                                                \
  4u /* a police player puts a roadblock (a = 1) or a spike strip (a = 2)      \
        ahead of us */
#define PU_ROADBLOCK                                                           \
  0x00445170u /* AICopManager::CreateRoadBlock, thiscall (mgr, AIPursuit *,    \
                 cops, IVehicle *, 0), RET 0x10 */
#define PU_RB_DESIGN                                                           \
  0x00407040u /* cdecl (road width, cops, spikes): its design (0xa4caa0 with   \
                 spike belts, 0xa4c420 without) */
#define PU_P_PIECES                                                            \
  0x1e0u /* AIPursuit: roadblock pieces put down (CreateRoadBlock counts them) \
          */
#define PU_STATE_MS 500.0
#define PU_REPORT_MS 200.0
#define PU_ROLE_MS 1000.0
#define PU_ROLE_KEEP                                                           \
  5000.0 /* a friend's police role holds this long after its last ROLE */
#define PU_HOLD_MS                                                             \
  450.0 /* friends' bust progress holds the meter this long after a report */
#define PU_TOGETHER                                                            \
  300.0f /* metres: a chased friend this near shares our police budget */
#define PU_TOGETHER_COPS                                                       \
  4u /* our cop manager's maximum while together (the game's: 8) */
#define PU_POLICE_SEE                                                          \
  80.0f /* metres: a police player sees a suspect this near */
#define PU_POLICE_BUST                                                         \
  12.0f /* metres: and builds bust progress this near while both are slow */
#define PU_POLICE_SLOW 8.0f /* m/s: the police car's own speed for that */
/* How a police player makes someone wanted (the suspect's game then starts its
 * chase, heat 1): its siren, or a crime it sees. "In view" is a cone around the
 * police car's nose. */
#define PU_SIREN_RANGE                                                         \
  150.0f /* metres: the siren picks the nearest player ahead this near */
#define PU_SIREN_COS 0.70f    /* ahead: within about 45 degrees of the nose */
#define PU_CRIME_RANGE 100.0f /* metres: crimes are seen this near */
#define PU_VIEW_COS 0.25f     /* in view: within about 75 degrees of the nose */
#define PU_SPEEDING                                                            \
  25.0f /* m/s (90 km/h): speeding (the game's own threshold was not found) */
#define PU_CRASH_DV                                                            \
  7.0f /* m/s lost within PU_CRASH_MS: a hit (braking loses at most ~3.5) */
#define PU_CRASH_MS 300.0
#define PU_RAM_DIST 5.0f /* metres: a hit on the police car itself */
#define PU_RAM_DV                                                              \
  4.5f /* m/s of the police car's own speed lost to it (hard braking: ~3.5) */
#define PU_SEEN_MS 700.0 /* a friend's "seen" holds this long */
#define PU_ENDED_MS                                                            \
  8000.0 /* after a friend's chase ends: no new assist on it for this long */
#define PU_ASSIST_RANGE                                                        \
  250.0f /* metres: a police player reports on a friend's car this near */
#define PU_BESIDE                                                              \
  60.0f /* metres: while we are chased, a friend this near is a suspect too */
#define PU_BOXED                                                               \
  10.0f /* metres: friends' bust progress counts only with another car this    \
           near */
#define PP_KEY_OF_NAME 0x00606b60u /* cdecl (const char *) -> attribute key */
#define PU_CALL_MS                                                             \
  3000.0 /* while pinned with no cop of ours: one is sent this often           \
            (pu_call_cop) */
#define PU_PIN_MAX                                                             \
  12000.0 /* pinned this long with still no cop of ours: friends' progress is  \
             let go */
#define PU_UNPIN_MS                                                            \
  15000.0 /* and ignored this long, so the meter drains and the player can     \
             drive off */
#define PU_SLOW                                                                \
  5.0f /* m/s: a friend's bust progress counts only below this own speed */
#define PU_SHARE_RANGE                                                         \
  150.0f /* metres: one of our chasing cops this near a friend may switch to   \
            it */
#define PU_SHARE_MS 4000.0 /* and at most one a friend this often */

#define PU_COP_MGR 0x00a9e874u  /* ICopMgr * = AICopManager + 0x4c */
#define PU_COPS_OFF 0x00a83a50u /* ICopMgr::mDisableCops */
#define PU_PURSUE_AT                                                           \
  0x004358d0u /* ICopMgr +0x3c PursueAtHeatLevel, thiscall (ICopMgr *, int) */
#define PU_PURSUIT_FOR                                                         \
  0x00435200u /* GetPursuitActivity, thiscall (AICopManager *, ISimable *) */
#define PU_ATTACH                                                              \
  0x00405540u /* pursuit + 0x24, vtable +8: Attach (AddVehicle), thiscall      \
                 (iface, IVehicle *) */
#define PU_DETACH 0x00407260u /* vtable +0xc: Detach (RemoveVehicle) */
#define PU_QI                                                                  \
  0x0060cb50u /* UTL::COM::Object::_QueryInterface, thiscall (object, handle)  \
               */
#define PU_H_PERP 0x004061d0u /* IPerpetrator's handle; heat at +0x1c */
/* AIPursuit fields */
#define PU_P_TARGET 0xd0u /* AITarget *: +0x1c the suspect's ISimable */
#define PU_P_COPS 0x54u   /* its cops (vector count) */
#define PU_P_LEVEL 0xf4u
#define PU_P_METER 0x138u    /* bust meter, seconds; busted at 5 */
#define PU_P_BUSTED 0x144u   /* byte */
#define PU_P_EVADE 0x158u    /* 0..1 while searching */
#define PU_P_COOL 0x15cu     /* cooldown left */
#define PU_P_COOL_ALL 0x160u /* cooldown needed (left starts at needed - 7) */
#define PU_P_SIGHT 0x170u    /* byte: suspect in sight */
#define PU_P_UNSEEN 0x184u   /* seconds since any cop saw the suspect */
#define PU_P_STATUS 0x298u   /* 0 chase, 2 searching, 3 busted */

typedef struct PuRec {  /* 0x20 bytes */
  uint32_t id, k, a, b; /* id: to whom (sending) / from whom (received) */
  float f[4];
} PuRec;

typedef struct PuShared {
  uint32_t magic, version;
  uint32_t me; /* the page: our relay id, 0 while not linked */
  uint16_t
      wish; /* the page (F9 menu): 1 = this player wants to be the police */
  uint16_t role; /* the game: 0 normal, 1 police, 2 police wanted but not yet
                    possible */
  uint32_t tx_head, tx_tail, rx_head,
      rx_tail; /* single producer, single consumer */
  PuRec tx[PU_TX], rx[PU_RX];
  uint32_t
      car; /* the page (F9): which police car (police.h PP_CARS), version 3 */
  uint32_t spare2[7];
} PuShared;
_Static_assert(sizeof(PuRec) == 0x20 && offsetof(PuShared, wish) == 0x0c &&
                   offsetof(PuShared, role) == 0x0e &&
                   offsetof(PuShared, tx) == 0x20 &&
                   offsetof(PuShared, rx) == 0x220 &&
                   offsetof(PuShared, car) == 0x620 &&
                   sizeof(PuShared) == 0x640,
               "PuShared layout (web/net.js reads it by offset)");

typedef struct PuFriend {
  uint32_t id;
  uint32_t simable; /* its car here (g_mp_friend_car), 0 = none */
  int known;        /* a STATE came */
  uint32_t status, level;
  float heat;
  double ended_at;  /* its chase ended (busted or escaped) */
  double tried_at;  /* last assist start here */
  double shared_at; /* last time one of our cops switched to it */
  float bust;       /* bust progress gathered for its next report */
  uint32_t seen, cops;
  double report_at;
  double seen_until;   /* it says its cops see us */
  float bust_in;       /* bust progress it sent about us, not yet applied */
  double police_until; /* it is the police (k 3) until then */
  int siren;           /* that police player's siren is on (k 3, a bit 1) */
  double wanted_until; /* a police player says we are wanted (REPORT a bit 1) */
  uint32_t crime; /* and why: 1 siren, 2 speeding, 3 hit something, 4 rammed the
                     police */
  float peak; /* police side: its car's top speed lately, for crash detection */
  double peak_at;
  double wanted_at; /* police side: we told it it is wanted (sent again for a
                       moment) */
  uint32_t wanted_for;
} PuFriend;

static uint32_t g_pu_block, g_pu_hooks[1],
    g_pu_buf; /* g_pu_buf: 0x40 guest bytes for calls */
static PuFriend g_pu_friends[16];
static uint32_t g_pu_mine; /* our pursuit last frame */
static int g_pu_busted;    /* it was busted */
static uint32_t g_pu_status, g_pu_end_status;
static double g_pu_state_at, g_pu_ended_at, g_pu_start_at, g_pu_role_at,
    g_pu_frame_at;
static uint32_t g_pu_logs;
static float
    g_pu_fmeter; /* friends' bust progress on our chase, held (PU_HOLD_MS) */
static double g_pu_fmeter_at;
static int
    g_pu_moved; /* our car drove (over PU_SLOW) since our last chase ended */
static double g_pu_pinned_at, g_pu_called_at,
    g_pu_unpinned_until; /* the 4.9 s hold (pu_own) */
static uint32_t g_pu_max_mgr,
    g_pu_max_saved; /* the cop manager whose maximum we lowered, and its own */
/* police.h: whether this player is the police now, and its per-frame work (it
 * may change the player's car; returns the role for the page: 0, 1 or 2). */
static int g_pu_police,
    g_pu_siren; /* police.h: this player is the police; its siren is on */
static float
    g_pu_own_peak; /* police side: our car's speed lately (ram detection) */
static uint32_t g_pu_siren_heard; /* the loudest friend's siren here, 0-255
                                     (PU_SIREN_RANGE) */
static uint32_t g_pu_trap_kind, g_pu_trap_by,
    g_pu_design_hook; /* a police player's trap, for our chase */
static double g_pu_trap_at, g_pu_trap_done_at, g_pu_trap_tried_at;
static int g_pu_force_spikes; /* CreateRoadBlock running for a spike strip: its
                                 design has spike belts */
static double g_pu_own_peak_at;
static void (*g_pu_police_idle)(void); /* discard cars from an unloaded world */
static uint32_t (*g_pu_police_tick)(uint32_t player, uint32_t wish, double now);

static PuShared *pu_shared(void) {
  void *p = NULL;
  return g_pu_block && g_api->guest_ptr(g_api, g_pu_block, sizeof(PuShared),
                                        &p) == POP_OK
             ? p
             : NULL;
}
static float pu_f(uint32_t a) { return bits_float(get_u32(a)); }
static uint8_t pu_b(uint32_t a) { return (uint8_t)(get_u32(a) & 0xffu); }

static uint32_t pu_mgr(void) {
  const uint32_t i = get_u32(PU_COP_MGR);
  return i ? i - 0x4cu : 0;
}
static int pu_cops_on(void) {
  return get_u32(PU_COP_MGR) && get_u32(PU_COPS_OFF) == 0;
}

/* The pursuit whose suspect is this ISimable, or 0. */
static uint32_t pu_pursuit_of(uint32_t simable) {
  const uint32_t mgr = pu_mgr();
  if (!mgr || !simable)
    return 0;
  const uint32_t head = mgr + 0x140;
  uint32_t node = get_u32(head);
  for (int n = 0; node && node != head && n < 16; ++n, node = get_u32(node)) {
    const uint32_t p = get_u32(node + 8), t = p ? get_u32(p + PU_P_TARGET) : 0;
    if (t && get_u32(t + 0x1c) == simable)
      return p;
  }
  return 0;
}

static uint32_t pu_perp(uint32_t simable) {
  const uint32_t object = simable ? get_u32(simable + 4) : 0,
                 handle = PU_H_PERP;
  return object ? mp_call(PU_QI, object, &handle, 1) : 0;
}

static int pu_pos(uint32_t simable, float out[3]) {
  const uint32_t s = simable ? mp_state(get_u32(simable - 0x24 + 0x70)) : 0;
  for (int i = 0; s && i < 3; ++i)
    out[i] = pu_f(s + 0x10 + 4u * i);
  return s != 0;
}
/* Whether our car (a PVehicle) can really be boxed in: on its wheels (PVehicle
 * +0x138 wheels on the ground, +0x130 time in the air; NFSPluginSDK's layout,
 * which matches +0x148 / +0x150) and with another car (a cop, a friend's cop
 * copy, a friend) within PU_BOXED. A car launched or stuck on top of another
 * one is neither; the game does not bust it either (2026-10-26: the meter sat
 * at 6 for a minute with the car in the air). */
static int pu_boxed(uint32_t car, const char **why) {
  if (get_u32(car + 0x138) < 3 || bits_float(get_u32(car + 0x130)) > 0.2f) {
    *why = "not on its wheels";
    return 0;
  }
  float here[3];
  if (!pu_pos(car + 0x24, here)) {
    *why = "no position";
    return 0;
  }
  uint32_t node = get_u32(MP_CARS);
  for (int n = 0; node && node != MP_CARS && n < 64;
       ++n, node = get_u32(node)) {
    const uint32_t other = node - 0xd0;
    float at[3];
    if (other == car || !pu_pos(other + 0x24, at))
      continue;
    float d = 0;
    for (int k = 0; k < 3; ++k)
      d += (at[k] - here[k]) * (at[k] - here[k]);
    if (d < PU_BOXED * PU_BOXED)
      return 1;
  }
  *why = "no car beside it";
  return 0;
}

static float pu_speed(uint32_t simable) {
  const uint32_t s = simable ? mp_state(get_u32(simable - 0x24 + 0x70)) : 0;
  float v = 0;
  for (int i = 0; s && i < 3; ++i) {
    const float x = pu_f(s + 0x20 + 4u * i);
    v += x * x;
  }
  return sqrtf(v);
}

static PuFriend *pu_friend(uint32_t id, int make) {
  PuFriend *free_one = NULL;
  for (int i = 0; i < 16; ++i) {
    if (g_pu_friends[i].id == id)
      return &g_pu_friends[i];
    if (!g_pu_friends[i].id && !free_one)
      free_one = &g_pu_friends[i];
  }
  if (!make || !id || !free_one)
    return NULL;
  memset(free_one, 0, sizeof *free_one);
  free_one->id = id;
  return free_one;
}

static void pu_send(PuShared *sh, uint32_t to, uint32_t k, uint32_t a,
                    uint32_t b, float f0, float f1, float f2, float f3) {
  const uint32_t head = __atomic_load_n(&sh->tx_head, __ATOMIC_RELAXED);
  if (head - __atomic_load_n(&sh->tx_tail, __ATOMIC_ACQUIRE) >= PU_TX)
    return; /* the page is not reading: drop, the next one says it again */
  PuRec *r = &sh->tx[head % PU_TX];
  r->id = to;
  r->k = k;
  r->a = a;
  r->b = b;
  r->f[0] = f0;
  r->f[1] = f1;
  r->f[2] = f2;
  r->f[3] = f3;
  __atomic_store_n(&sh->tx_head, head + 1, __ATOMIC_RELEASE);
}

static void pu_log(const char *text) {
  if (g_pu_logs < 400) {
    ++g_pu_logs;
    log_line(text);
  }
}

static double mp_now(void);

static int pu_is_police(const PuFriend *f, double now) {
  return f && f->police_until > now;
}

/* mp.h: friends' own cars are built as class 6, so they can be suspects here; a
 * police player's as class 7 (never a suspect). mp.h builds the car again when
 * this changes. */
static uint32_t pu_friend_kind(uint32_t id) {
  return pu_is_police(pu_friend(id, 0), mp_now()) ? MP_REMOTE : MP_REMOTE_HUMAN;
}
static void pu_friend_car(int slot, uint32_t id, uint32_t simable) {
  (void)slot;
  PuFriend *f = pu_friend(id, simable != 0);
  if (f)
    f->simable = simable;
}

static void pu_receive(PuShared *sh, double now) {
  uint32_t tail = __atomic_load_n(&sh->rx_tail, __ATOMIC_RELAXED);
  const uint32_t head = __atomic_load_n(&sh->rx_head, __ATOMIC_ACQUIRE);
  if (head - tail > PU_RX)
    tail = head - PU_RX;
  for (; tail != head; ++tail) {
    const PuRec r = sh->rx[tail % PU_RX];
    PuFriend *f = pu_friend(r.id, 1);
    if (!f || !isfinite(r.f[0]))
      continue;
    if (r.k == PU_STATE && r.a <= 4) {
      const uint32_t was = f->known ? f->status : 0;
      if ((r.a == 3 || r.a == 4) && was != r.a)
        f->ended_at = now;
      f->known = 1;
      f->status = r.a;
      f->level = r.b;
      f->heat = r.f[0] >= 0.0f && r.f[0] <= 10.0f ? r.f[0] : f->heat;
    } else if (r.k == PU_REPORT) {
      if (r.a & 1u)
        f->seen_until = now + PU_SEEN_MS;
      if (r.a & 2u) {
        f->wanted_until = now + 1000.0;
        f->crime = isfinite(r.f[2]) && r.f[2] >= 1.0f && r.f[2] <= 4.0f
                       ? (uint32_t)r.f[2]
                       : 0;
      }
      if (r.f[0] > 0.0f && r.f[0] <= 3.0f)
        f->bust_in += r.f[0];
    } else if (r.k == PU_TRAP && (r.a == 1 || r.a == 2) &&
               pu_is_police(f, now) && !g_pu_trap_kind) {
      g_pu_trap_kind = r.a;
      g_pu_trap_by = f->id;
      g_pu_trap_at = now;
    } else if (r.k == PU_ROLE) {
      const int was = pu_is_police(f, now), police = (r.a & 1u) != 0,
                siren = police && (r.a & 2u);
      f->police_until = police ? now + PU_ROLE_KEEP : 0;
      if (was != police) {
        char line[96];
        snprintf(line, sizeof line,
                 "core.nfsc: pursuit: player %u %s the police", f->id,
                 police ? "is now" : "is no longer");
        pu_log(line);
      }
      if (siren != f->siren) {
        char line[96];
        snprintf(line, sizeof line,
                 "core.nfsc: pursuit: police player %u's siren %s", f->id,
                 siren ? "on" : "off");
        pu_log(line);
      }
      f->siren = siren;
    }
  }
  __atomic_store_n(&sh->rx_tail, tail, __ATOMIC_RELEASE);
}

/* Pinned by friends with no cop of ours yet: one of ours is made and put in the
 * chase, so the game's own arrest can follow (it needs one of its cops in the
 * pursuit). Where no cop patrols yet (a fresh free roam) the game has none to
 * send: its cop manager makes new ones at the origin (00422ef0), which the
 * world refuses until a patrol spawns (~84 s, 2026-10-26). So ours is built as
 * mp.h builds cars (driver class 2, the patrol model, AIVehicleCopCar), 10 m
 * beyond the police player's car, and added to the pursuit the game's own way
 * (AddVehicle, as when our cops are shared). It is never removed here
 * (cop-model Kills can crash; police-sync.md): it stays one of the world's cars
 * until the world unloads, and is used again for the next arrest. */
static uint32_t g_pu_cop; /* PVehicle of the cop we made */
static void pu_call_cop(uint32_t pursuit, uint32_t player, double now) {
  float here[3], at[3];
  if (!g_pu_buf || !pu_pos(player + 0x24, here))
    return;
  const PuFriend *police = NULL;
  for (int i = 0; i < 16 && !police; ++i) {
    const PuFriend *f = &g_pu_friends[i];
    float d = 0;
    if (!f->id || !f->simable || !pu_is_police(f, now) ||
        !pu_pos(f->simable, at))
      continue;
    for (int k = 0; k < 3; ++k)
      d += (at[k] - here[k]) * (at[k] - here[k]);
    if (d < 30.0f * 30.0f)
      police = f;
  }
  if (!police)
    return; /* friends' cops (not a police player) pin us: the game's own cops
               are on their way */
  const char *name = "copmidsize";
  for (uint32_t c = 0; c <= strlen(name); ++c)
    g_api->guest_write_u8(g_api, g_pu_buf + c, (uint8_t)name[c]);
  const uint32_t arg = g_pu_buf, key = mp_call(PP_KEY_OF_NAME, 0, &arg, 1);
  if (!(g_pu_cop && mp_listed(g_pu_cop) && mp_model_key(g_pu_cop) == key &&
        get_u32(g_pu_cop + 0x148) == 2)) {
    float dir[3], len = 0, pos[3];
    for (int k = 0; k < 3; ++k) {
      dir[k] = at[k] - here[k];
      len += dir[k] * dir[k];
    }
    len = sqrtf(len) > 0.5f ? sqrtf(len) : 1.0f;
    for (int k = 0; k < 3; ++k)
      pos[k] = at[k] + dir[k] / len * 10.0f;
    pos[1] = at[1] + 1.0f;
    const uint32_t sim = key ? mp_build(player, 2, key, pos, 2u | 8u, 0) : 0;
    g_pu_cop = sim ? sim - 0x24 : 0;
  }
  if (g_pu_cop) {
    const uint32_t vehicle = g_pu_cop + 0xb8;
    mp_call(PU_ATTACH, pursuit + 0x24, &vehicle, 1);
  }
  char line[128];
  snprintf(line, sizeof line,
           "core.nfsc: pursuit: pinned by police player %u; one of our cops %s "
           "(now %u in the chase)",
           police->id, g_pu_cop ? "joins" : "could not be made",
           get_u32(pursuit + PU_P_COPS));
  pu_log(line);
}

/* ROADBLOCKS AND SPIKE STRIPS from a police player: in our own game, the game's
 * own way (as its pursuit asks for one, UpdatePursuits 00445c70):
 * CreateRoadBlock puts it on the road ahead of us from our pursuit's target
 * (place and speed), so here it is solid and its spike belts burst our tyres. A
 * spike strip is the same with the design chooser (00407040) told spikes are
 * fine: its designs with spike belts. At most one every PU_TRAP_MS here,
 * whatever comes. */
#define PU_TRAP_MS 8000.0 /* one at most this often here */
#define PU_TRAP_RETRY_MS                                                       \
  400.0 /* tried again this often while the road ahead will not take it */
#define PU_TRAP_TRIES_MS 12000.0 /* and given up after this */
static void pu_design_before(const PopModApi *api, pop_cpu_v1 *cpu,
                             PopHookInvocation *inv, void *user) {
  (void)inv;
  (void)user;
  if (g_pu_force_spikes)
    api->guest_write_u8(api, cpu->esp + 0xc, 1);
}
static void pu_trap(uint32_t p, double now) {
  const uint32_t mgr = pu_mgr();
  if (!mgr || now - g_pu_trap_tried_at < PU_TRAP_RETRY_MS)
    return;
  if (now - g_pu_trap_at > PU_TRAP_TRIES_MS) {
    pu_log("core.nfsc: pursuit: no road ahead took the police player's trap in "
           "time");
    g_pu_trap_kind = 0;
    return;
  }
  if (now - g_pu_trap_done_at < PU_TRAP_MS)
    return;
  g_pu_trap_tried_at = now;
  const uint32_t kind = g_pu_trap_kind, before = get_u32(p + PU_P_PIECES);
  const uint32_t args[4] = {
      p, 6u, 0, 0}; /* up to 6 cops: the design picks its own number */
  g_pu_force_spikes = kind == 2;
  const uint32_t placed = mp_call(PU_ROADBLOCK, mgr, args, 4) & 0xffu;
  g_pu_force_spikes = 0;
  const uint32_t pieces = get_u32(p + PU_P_PIECES) - before;
  if (!placed)
    return; /* the road 250 m ahead would not take one (a bridge, a
               junction...): again soon */
  g_pu_trap_kind = 0;
  g_pu_trap_done_at = now;
  char line[128];
  snprintf(line, sizeof line,
           "core.nfsc: pursuit: police player %u put a %s ahead of us (%u "
           "pieces, %.1f s after the call)",
           g_pu_trap_by, kind == 2 ? "spike strip" : "roadblock", pieces,
           (now - g_pu_trap_at) / 1000.0);
  pu_log(line);
}

/* Our own chase: started on a friend's sighting, kept in sight while friends
 * see us, and bust progress from friends' cops added while our car is really
 * slow. Returns the status to send. */
static uint32_t pu_own(uint32_t player, double now) {
  const uint32_t simable = player + 0x24;
  const uint32_t p = pu_pursuit_of(simable);
  uint32_t status = 0;
  if (p) {
    const int busted = pu_b(p + PU_P_BUSTED) != 0;
    g_pu_busted |= busted;
    status = busted ? 3u : get_u32(p + PU_P_STATUS) == 2 ? 2u : 1u;
  } else if (g_pu_mine) {
    g_pu_end_status = g_pu_busted ? 3u : 4u;
    g_pu_ended_at = now;
    g_pu_busted = 0;
    /* What friends' cops saw belongs to the chase that just ended; their
     * assists end when they hear our status. */
    for (int i = 0; i < 16; ++i) {
      g_pu_friends[i].seen_until = 0;
      g_pu_friends[i].bust_in = 0;
    }
    g_pu_fmeter = 0;
    g_pu_moved = 0;
    g_pu_pinned_at = g_pu_unpinned_until = 0;
    g_pu_trap_kind = 0;
    char line[96];
    snprintf(line, sizeof line, "core.nfsc: pursuit: our chase ended (%s)",
             g_pu_end_status == 3 ? "busted" : "escaped");
    pu_log(line);
  }
  g_pu_mine = p;
  if (!p && now - g_pu_ended_at < 3000.0)
    status = g_pu_end_status;
  /* Friends' cops seeing us start our chase; a police player's sightings only
   * keep one going: its siren or a crime it saw (REPORT bit 1, "wanted") starts
   * it. */
  int seen = 0, starts = 0;
  uint32_t by = 0, crime = 0;
  float bust = 0;
  for (int i = 0; i < 16; ++i) {
    PuFriend *f = &g_pu_friends[i];
    if (!f->id)
      continue;
    const int police = pu_is_police(f, now);
    seen |= f->seen_until > now;
    starts |= f->seen_until > now && !police;
    if (f->wanted_until > now && police) {
      starts = 1;
      by = f->id;
      crime = f->crime;
    }
    bust += f->bust_in;
    f->bust_in = 0;
  }
  if (!p) {
    /* A friend's cops see us: our chase starts, and our own police join it. Not
     * right after a bust (the arrest is still on the screen), nor in the moment
     * after an escape (friends' assists end when they hear of it). */
    const double since = now - g_pu_ended_at;
    /* After a bust also until our car has driven again: the arrest, the fine
     * and the menus after it are not a getaway, even with a friend's police car
     * still beside us. */
    g_pu_moved |= pu_speed(simable) > PU_SLOW;
    const int resting =
        (g_pu_end_status == 3 && (since < 10000.0 || !g_pu_moved)) ||
        (g_pu_end_status == 4 && since < 3000.0);
    if (starts && pu_cops_on() && !g_pu_police && !resting &&
        now - g_pu_start_at > 2000.0) {
      g_pu_start_at = now;
      const uint32_t icop = get_u32(PU_COP_MGR), level = 1;
      mp_call(PU_PURSUE_AT, icop, &level, 1);
      static const char *const why[] = {"", "its siren", "speeding",
                                        "hitting something",
                                        "ramming the police"};
      char line[112];
      if (by)
        snprintf(line, sizeof line,
                 "core.nfsc: pursuit: wanted by police player %u (%s); our "
                 "chase starts",
                 by, why[crime <= 4 ? crime : 0]);
      else
        snprintf(
            line, sizeof line,
            "core.nfsc: pursuit: a friend's cops see us; our chase starts");
      pu_log(line);
    }
    return status;
  }
  if (status == 3)
    return status;
  if (g_pu_trap_kind)
    pu_trap(p, now);
  if (seen) {
    g_api->guest_write_u8(g_api, p + PU_P_SIGHT, 1);
    if (pu_f(p + PU_P_UNSEEN) > -0.25f)
      put_float(p + PU_P_UNSEEN, -0.25f);
    if (get_u32(p + PU_P_STATUS) ==
        2) { /* searching: seen again, as when our own cops find us */
      mp_put(p + PU_P_STATUS, 0);
      put_float(p + PU_P_EVADE, 0.0f);
      put_float(p + PU_P_COOL, pu_f(p + PU_P_COOL_ALL) - 7.0f);
    }
  }
  /* Friends' bust progress: built up while our car is really slow, and held
   * over the meter for PU_HOLD_MS after each report (the game lowers it fast
   * while none of its own cops boxes us in). Our own cops' progress counts too:
   * the hold starts from the higher of the two. */
  const float speed = pu_speed(simable), meter = pu_f(p + PU_P_METER);
  const char *why = "too fast";
  int counts = speed < PU_SLOW && pu_boxed(player, &why);
  if (counts && now < g_pu_unpinned_until) {
    counts = 0;
    why = "let go after a long pin";
  }
  if (bust > 0.0f) {
    if (counts) {
      g_pu_fmeter = (g_pu_fmeter > meter ? g_pu_fmeter : meter) + bust;
      g_pu_fmeter = g_pu_fmeter < 5.5f ? g_pu_fmeter : 5.5f;
      g_pu_fmeter_at = now;
    } else {
      g_pu_fmeter = 0;
    }
    static double logged_at;
    if (now - logged_at >=
        1000.0) { /* about once a second, not once a report */
      logged_at = now;
      char line[144];
      const int held =
          counts && g_pu_fmeter > 4.9f && get_u32(p + PU_P_COPS) == 0;
      snprintf(line, sizeof line,
               "core.nfsc: pursuit: friends' bust progress %.2f s %s (%s, "
               "speed %.1f m/s, meter %.2f%s)",
               bust, counts ? "applied" : "ignored", counts ? "boxed in" : why,
               speed,
               held     ? 4.9f
               : counts ? g_pu_fmeter
                        : meter,
               held ? ", held until one of our cops is in the chase" : "");
      pu_log(line);
    }
  }
  if (g_pu_fmeter > 0.0f) {
    if (now - g_pu_fmeter_at > PU_HOLD_MS || !counts)
      g_pu_fmeter = 0;
    else if (meter < g_pu_fmeter) {
      /* The arrest (BUSTED!, the arrest scene, the fine) needs one of this
       * game's own cops in the chase: without one the game ends the chase
       * quietly (career free roam, 2026-10-26). So friends alone pin us just
       * short of 5 s until our own police have a car in it (a cop called with
       * ICopMgr +0x20 SpawnCop came no sooner: in a fresh free roam at the
       * Safehouse the game's first cop took ~84 s both ways). */
      const float hold = get_u32(p + PU_P_COPS) == 0 && g_pu_fmeter > 4.9f
                             ? 4.9f
                             : g_pu_fmeter;
      put_float(p + PU_P_METER, hold);
      if (hold == 4.9f) {
        /* Our first cop is called at once: the pursuit asks for a cop when its
         * request timer runs out (in a fresh free roam that took ~84 s). */
        if (!g_pu_pinned_at)
          g_pu_pinned_at = now;
        if (now - g_pu_called_at > PU_CALL_MS) {
          g_pu_called_at = now;
          pu_call_cop(p, player, now);
        }
        /* Still none after PU_PIN_MAX: let go, so the player is not left
         * frozen. */
        if (now - g_pu_pinned_at > PU_PIN_MAX) {
          g_pu_unpinned_until = now + PU_UNPIN_MS;
          g_pu_fmeter = 0;
          g_pu_pinned_at = 0;
          pu_log("core.nfsc: pursuit: pinned by friends with no cop of ours "
                 "for 12 s: let go");
        }
      } else {
        g_pu_pinned_at = 0;
      }
    }
  }
  if (g_pu_fmeter <= 0.0f)
    g_pu_pinned_at = 0;
  return status;
}

/* A police player's game: what its own car sees of a friend's car (no AI cops
 * run here). Not yet chased: it becomes wanted (REPORT bit 1, f[2] = why) when
 * the siren is on and it is the nearest player ahead within PU_SIREN_RANGE
 * (pu_police_siren), or when it is seen within PU_CRIME_RANGE speeding, losing
 * PU_CRASH_DV of speed at once (it hit something), or hitting the police car.
 * Chased: "seen" within PU_POLICE_SEE and in view, and bust progress beside it.
 */
static int pu_police_ready(uint32_t car) {
  const uint32_t damage = car ? get_u32(car + 0xfc) : 0;
  return damage && pu_f(damage + 0x34) < 1.0f;
}

static void pu_police_report(PuShared *sh, uint32_t player, PuFriend *f,
                             float d, double now, double dt) {
  float here[3], there[3];
  const uint32_t state = mp_state(get_u32(player + 0x70));
  if (!state || !pu_pos(player + 0x24, here) || !pu_pos(f->simable, there))
    return;
  float cosv = 0;
  for (int k = 0; k < 3; ++k)
    cosv += pu_f(state + 0x90 + 4u * k) * (there[k] - here[k]);
  cosv = d > 0.5f ? cosv / d : 1.0f;
  const float speed = pu_speed(f->simable);
  /* its recent top speed: a drop of PU_CRASH_DV from it within PU_CRASH_MS is a
   * hit */
  const float drop = now - f->peak_at < PU_CRASH_MS ? f->peak - speed : 0.0f;
  if (speed >= f->peak || now - f->peak_at >= PU_CRASH_MS) {
    f->peak = speed;
    f->peak_at = now;
  }
  if ((f->ended_at > 0 && now - f->ended_at < 10000.0) || d > PU_SIREN_RANGE)
    return; /* just busted or escaped: give the arrest (or the getaway) a moment
             */
  const int chased = f->known && (f->status == 1 || f->status == 2);
  if (!chased) {
    uint32_t why = 0;
    if (d < PU_CRIME_RANGE && cosv > PU_VIEW_COS) {
      const float mine = pu_speed(player + 0x24);
      if (speed > PU_SPEEDING)
        why = 2;
      else if (drop > PU_CRASH_DV)
        why =
            d < PU_RAM_DIST + 3.0f && g_pu_own_peak - mine > PU_RAM_DV ? 4 : 3;
    }
    if (d < PU_RAM_DIST &&
        g_pu_own_peak - pu_speed(player + 0x24) > PU_RAM_DV && speed > 2.0f)
      why = 4; /* our car was knocked while it was this close and moving */
    if (why && now - f->wanted_at > 2000.0) {
      f->wanted_at = now;
      f->wanted_for = why;
      static const char *const what[] = {"", "the siren", "speeding",
                                         "hitting something", "ramming us"};
      char line[128];
      snprintf(line, sizeof line,
               "core.nfsc: police: player %u is wanted: %s (%.0f m, %.0f km/h)",
               f->id, what[why], d, speed * 3.6f);
      pu_log(line);
    }
  }
  const int wanted = !chased && f->wanted_at > 0 &&
                     now - f->wanted_at < 2000.0; /* said again for a moment */
  f->seen |= d < PU_POLICE_SEE && cosv > -0.2f;
  if (d < PU_POLICE_BUST && speed < PU_SLOW &&
      pu_speed(player + 0x24) < PU_POLICE_SLOW)
    f->bust += (float)dt;
  if (now - f->report_at >= PU_REPORT_MS ||
      (wanted && f->report_at < f->wanted_at)) {
    f->report_at = now;
    pu_send(sh, f->id, PU_REPORT, (chased ? f->seen : 0u) | (wanted ? 2u : 0u),
            1,
            chased && f->bust < 3.0f ? f->bust
            : chased                 ? 3.0f
                                     : 0.0f,
            d, wanted ? (float)f->wanted_for : 0.0f, 0);
    f->bust = 0;
    f->seen = 0;
  }
}

/* The siren: the nearest player ahead within PU_SIREN_RANGE, not chased yet, is
 * wanted. */
static void pu_police_siren(uint32_t player, double now) {
  float here[3];
  const uint32_t state = mp_state(get_u32(player + 0x70));
  if (!g_pu_siren || !state || !pu_pos(player + 0x24, here))
    return;
  PuFriend *best = NULL;
  float best_d = PU_SIREN_RANGE;
  for (int i = 0; i < 16; ++i) {
    PuFriend *f = &g_pu_friends[i];
    float there[3], d = 0, cosv = 0;
    if (!f->id || !f->simable || pu_is_police(f, now) ||
        (f->known && (f->status == 1 || f->status == 2)) ||
        (f->ended_at > 0 && now - f->ended_at < 10000.0) ||
        !pu_pos(f->simable, there))
      continue;
    for (int k = 0; k < 3; ++k) {
      d += (there[k] - here[k]) * (there[k] - here[k]);
      cosv += pu_f(state + 0x90 + 4u * k) * (there[k] - here[k]);
    }
    d = sqrtf(d);
    if (d < best_d && d > 0.5f && cosv / d > PU_SIREN_COS) {
      best_d = d;
      best = f;
    }
  }
  if (best && now - best->wanted_at > 2000.0) {
    best->wanted_at = now;
    best->wanted_for = 1;
    char line[96];
    snprintf(line, sizeof line,
             "core.nfsc: police: player %u is wanted: the siren (%.0f m ahead)",
             best->id, best_d);
    pu_log(line);
  }
}

/* Our cop manager's maximum: PU_TOGETHER_COPS while together, the game's own
 * otherwise. */
static void pu_budget(int together) {
  const uint32_t mgr = pu_mgr();
  if (mgr != g_pu_max_mgr) { /* another world: its manager starts with the
                                game's own value */
    g_pu_max_mgr = mgr;
    g_pu_max_saved = 0;
  }
  if (!mgr)
    return;
  const uint32_t max = get_u32(mgr + 0x94);
  if (together && !g_pu_max_saved && max > PU_TOGETHER_COPS && max <= 16) {
    g_pu_max_saved = max;
    pu_log(
        "core.nfsc: pursuit: chased together: our police run at most 4 cops");
  }
  if (together && g_pu_max_saved && max != PU_TOGETHER_COPS)
    mp_put(mgr + 0x94, PU_TOGETHER_COPS);
  if (!together && g_pu_max_saved) {
    mp_put(mgr + 0x94, g_pu_max_saved);
    g_pu_max_saved = 0;
    pu_log("core.nfsc: pursuit: apart again: our police back to the game's "
           "number of cops");
  }
}

/* Our pursuits of friends' cars. */
static void pu_assists(PuShared *sh, uint32_t player, double now) {
  float here[3] = {0, 0, 0};
  pu_pos(player + 0x24, here);
  const double dt = g_pu_frame_at > 0 && now - g_pu_frame_at < 250.0
                        ? (now - g_pu_frame_at) / 1000.0
                        : 0.0;
  g_pu_frame_at = now;
  if (g_pu_police) {
    if (pu_police_ready(player)) pu_police_siren(player, now);
    const float mine = pu_speed(player + 0x24);
    if (mine >= g_pu_own_peak || now - g_pu_own_peak_at >= PU_CRASH_MS) {
      g_pu_own_peak = mine;
      g_pu_own_peak_at = now;
    }
  }
  int together = 0;
  uint32_t heard = 0;
  for (int i = 0; i < 16; ++i) {
    PuFriend *f = &g_pu_friends[i];
    if (!f->id || !f->simable)
      continue;
    if (!mp_listed(f->simable - 0x24)) {
      f->simable = 0;
      continue;
    }
    const int police = pu_is_police(f, now);
    if (police) { /* a police player's car shows its light bar here too
                     (PVehicle +0x150, GlareOn) */
      const uint32_t lights = f->simable - 0x24 + 0x150, v = get_u32(lights);
      if ((v & 0x7000u) != 0x7000u)
        mp_put(lights, v | 0x7000u);
    }
    if (police && f->siren) {
      float at[3], d2 = 0;
      if (pu_pos(f->simable, at)) {
        for (int k = 0; k < 3; ++k)
          d2 += (at[k] - here[k]) * (at[k] - here[k]);
        const float q = 1.0f - sqrtf(d2) / PU_SIREN_RANGE;
        const uint32_t level = q > 0 ? (uint32_t)(q * q * 255.0f) : 0;
        heard = level > heard ? level : heard;
      }
    }
    if (f->known && !police) {
      const uint32_t perp = pu_perp(f->simable);
      if (perp)
        put_float(perp + 0x1c, f->heat);
    }
    float there[3];
    if (!pu_pos(f->simable, there))
      continue;
    float d = 0;
    for (int k = 0; k < 3; ++k)
      d += (there[k] - here[k]) * (there[k] - here[k]);
    d = sqrtf(d);
    if (g_pu_police) {
      if (!police && pu_police_ready(player))
        pu_police_report(sh, player, f, d, now, dt);
      else { f->seen = 0; f->bust = 0; f->wanted_at = 0; }
      continue;
    }
    together |= !police && d < PU_TOGETHER &&
                ((f->known && (f->status == 1 || f->status == 2)) ||
                 (g_pu_mine && !g_pu_busted));
    /* A police player's car is never chased here; one picked up before its role
     * came escapes. */
    const int ended =
        police || (f->ended_at > 0 && now - f->ended_at < PU_ENDED_MS);
    uint32_t a = pu_pursuit_of(f->simable);
    /* Our police join a friend's chase only where it meets ours (we are chased
     * and it is right beside us): a chased friend merely passing by is already
     * shown with its own cops (cops.h's copies), and ours on top read as police
     * appearing out of nowhere (user, 2026-10-26). Patrols here may still pick
     * a friend's car up on their own. */
    if (!a && !ended && pu_cops_on() && now - f->tried_at > 3000.0 &&
        g_pu_mine && !g_pu_busted && d < PU_BESIDE) {
      f->tried_at = now;
      const uint32_t mgr = pu_mgr(), car = f->simable;
      a = mp_call(PU_PURSUIT_FOR, mgr, &car, 1);
      char line[128];
      snprintf(line, sizeof line,
               "core.nfsc: pursuit: our police join the chase of player %u "
               "(%.0f m away)",
               f->id, d);
      pu_log(line);
      a = pu_pursuit_of(f->simable);
    }
    if (!a)
      continue;
    if (ended) {
      /* Its chase is over (busted or escaped in its own game): ours of its car
       * escapes. */
      mp_put(a + PU_P_STATUS, 2);
      g_api->guest_write_u8(g_api, a + PU_P_SIGHT, 0);
      put_float(a + PU_P_UNSEEN, 60.0f);
      put_float(a + PU_P_EVADE, 1.0f);
      put_float(a + PU_P_COOL, 0.0f);
      put_float(a + PU_P_METER, 0.0f);
      continue;
    }
    /* The same nearby cops chase both: while the assist has no cop and ours has
     * two or more, our cop nearest the friend (within PU_SHARE_RANGE) switches
     * to it (the game's own RemoveVehicle / AddVehicle: the cop ends one chase
     * and starts the other). */
    if (g_pu_mine && !g_pu_busted && get_u32(a + PU_P_COPS) == 0 &&
        get_u32(g_pu_mine + PU_P_COPS) >= 2 &&
        now - f->shared_at > PU_SHARE_MS) {
      const uint32_t list = get_u32(g_pu_mine + 0x4c),
                     n = get_u32(g_pu_mine + PU_P_COPS);
      uint32_t best = 0;
      float best_d = PU_SHARE_RANGE;
      for (uint32_t c = 0; list && c < n && c < 16; ++c) {
        const uint32_t cop = get_u32(list + 4u * c);
        float at[3];
        if (!cop || !pu_pos(cop - 0xb8u + 0x24u, at))
          continue;
        float e = 0;
        for (int k = 0; k < 3; ++k)
          e += (at[k] - there[k]) * (at[k] - there[k]);
        e = sqrtf(e);
        if (e < best_d) {
          best_d = e;
          best = cop;
        }
      }
      if (best) {
        f->shared_at = now;
        mp_call(PU_DETACH, g_pu_mine + 0x24, &best, 1);
        mp_call(PU_ATTACH, a + 0x24, &best, 1);
        char line[128];
        snprintf(line, sizeof line,
                 "core.nfsc: pursuit: one of our cops (%.0f m from it) now "
                 "chases player %u",
                 best_d, f->id);
        pu_log(line);
      }
    }
    /* Never busted here: its own game decides. */
    const float m = pu_f(a + PU_P_METER);
    if (m > 0.0f) {
      f->bust += m;
      put_float(a + PU_P_METER, 0.0f);
    }
    f->seen |= pu_b(a + PU_P_SIGHT) != 0;
    f->cops = get_u32(a + PU_P_COPS);
    if (now - f->report_at >= PU_REPORT_MS) {
      f->report_at = now;
      const float bust = f->bust < 3.0f ? f->bust : 3.0f;
      pu_send(sh, f->id, PU_REPORT, f->seen, f->cops, bust, d, 0, 0);
      f->bust = 0;
      f->seen = 0;
    }
  }
  pu_budget(together && !g_pu_police);
  g_pu_siren_heard = heard;
}

/* MINIMAP. A police player's car shows like the game's cops: its dot (the
 * game's racer dots for vehicle list 7, which race.h asks for in free roam too;
 * 005d8d70) takes the minimap's flashing cop colour of this frame (005d2200:
 * blue, white, red, as the game colours a cop player in Pursuit Tag). The dots
 * are handed out in the list's order, one per active car but ours, from the
 * minimap's counter (+0xd0) to its icons (+0xe0), so the dot of each car is
 * known. */
#define PU_MINIMAP_DOTS                                                        \
  0x005d8d70u /* thiscall (minimap, vehicle list, IVehicle *ours), RET 8 */
#define PU_MINIMAP_COP                                                         \
  0x005d2200u /* thiscall (minimap): the cop colour of this frame */
#define PU_ICON_COLOUR 0x00570c90u /* cdecl (FE object, ARGB) */
#define PU_VEHICLE_LISTS                                                       \
  0x00a9f158u /* 0x88 bytes each: +4 IVehicle * begin, +0xc count */
static uint32_t g_pu_dots_mm, g_pu_dots_list, g_pu_dots_from, g_pu_dots_ours,
    g_pu_dots_hooks[2];
static void pu_dots_before(const PopModApi *api, pop_cpu_v1 *cpu,
                           PopHookInvocation *inv, void *user) {
  (void)api;
  (void)inv;
  (void)user;
  g_pu_dots_mm = cpu->ecx;
  g_pu_dots_list = get_u32(cpu->esp + 4);
  g_pu_dots_ours = get_u32(cpu->esp + 8);
  g_pu_dots_from = cpu->ecx ? get_u32(cpu->ecx + 0xd0) : 0;
}
static void pu_dots_after(const PopModApi *api, pop_cpu_v1 *cpu,
                          PopHookInvocation *inv, void *user) {
  (void)api;
  (void)cpu;
  (void)inv;
  (void)user;
  const uint32_t mm = g_pu_dots_mm;
  if (!mm || g_pu_dots_list != 7u)
    return;
  const double now = mp_now();
  int any = 0;
  for (int i = 0; i < 16 && !any; ++i)
    any = g_pu_friends[i].simable && pu_is_police(&g_pu_friends[i], now);
  if (!any)
    return;
  uint32_t ours = 0; /* the minimap skips the car its AI says is ours (as
                        005d8d70 works it out) */
  if (g_pu_dots_ours) {
    const uint32_t ai = mp_call(get_u32(get_u32(g_pu_dots_ours) + 0xc0),
                                g_pu_dots_ours, NULL, 0);
    ours = ai ? mp_call(get_u32(get_u32(ai) + 0xc8), ai, NULL, 0) : 0;
  }
  const uint32_t list = PU_VEHICLE_LISTS + 7u * 0x88u,
                 begin = get_u32(list + 4), n = get_u32(list + 0xc);
  uint32_t dot = g_pu_dots_from;
  const uint32_t colour = mp_call(PU_MINIMAP_COP, mm, NULL, 0);
  for (uint32_t k = 0; begin && k < n && k < 64 && dot < 0x1eu; ++k) {
    const uint32_t v = get_u32(begin + 4u * k);
    if (!v || !get_u32(v + 0xa8) ||
        v == ours) /* IsActive (006d80c0), not ours */
      continue;
    const uint32_t icon = get_u32(mm + 0xe0 + 4u * dot++),
                   simable = v - 0xb8u + 0x24u;
    for (int i = 0; icon && i < 16; ++i) {
      const PuFriend *f = &g_pu_friends[i];
      if (f->simable == simable && pu_is_police(f, now)) {
        const uint32_t args[2] = {icon, colour};
        mp_call(PU_ICON_COLOUR, 0, args, 2);
        static int told;
        if (!told++) {
          char line[112];
          snprintf(line, sizeof line,
                   "core.nfsc: pursuit: police player %u on the minimap in the "
                   "cops' colours (dot %u)",
                   f->id, dot - 1);
          pu_log(line);
        }
      }
    }
  }
}

/* WORLD MAP (worldmap.h's g_wm_paint): a police player's arrow (worldmap.h
 * draws friends on the map's own MMICON_COPCAR images) flashes blue, white, red
 * like the game's cops. */
static uint32_t pu_map_paint(uint32_t id, uint32_t argb) {
  if (!pu_is_police(pu_friend(id, 0), mp_now()))
    return argb;
  /* by the map's frames (the game is paused under the map, so its clock
   * stands): blue, white, red, white, as 005d2200 cycles the minimap's cops */
  static const uint32_t flash[4] = {0xff0000ffu, 0xffccccccu, 0xffa00000u,
                                    0xffccccccu};
  static uint32_t frames;
  return flash[(frames++ / 5u) % 4u];
}

static void pu_service(const PopModApi *api, pop_cpu_v1 *cpu,
                       PopHookInvocation *inv, void *user) {
  (void)api;
  (void)cpu;
  (void)inv;
  (void)user;
  PuShared *sh = pu_shared();
  if (!sh)
    return;
  const double now = mp_now();
  pu_receive(sh, now);
  uint32_t player = mp_player();
  if (player && mp_online_game())
    player = 0;
  if (!player || !__atomic_load_n(&sh->me, __ATOMIC_ACQUIRE)) {
    if (!player && g_pu_police_idle)
      g_pu_police_idle();
    g_pu_mine = 0;
    g_pu_busted = 0;
    g_pu_trap_kind = 0;
    memset(sh->spare2, 0, sizeof sh->spare2);
    __atomic_store_n(&sh->role,
                     g_pu_police                                           ? 1
                     : (__atomic_load_n(&sh->wish, __ATOMIC_ACQUIRE) & 1u) ? 2
                                                                           : 0,
                     __ATOMIC_RELEASE);
    return;
  }
  if (g_pu_police_tick) { /* police.h may put the player in another car */
    const uint32_t role = g_pu_police_tick(
        player, __atomic_load_n(&sh->wish, __ATOMIC_ACQUIRE), now);
    /* bits 8-15: the loudest police siren of a friend near us (0-255), for the
     * page's sound */
    __atomic_store_n(&sh->role,
                     (uint16_t)((role & 0xffu) | (g_pu_siren_heard << 8)),
                     __ATOMIC_RELEASE);
    if (!(player = mp_player()))
      return;
  }
  static int sent_police = -1;
  const int role_now = g_pu_police | (g_pu_siren << 1);
  if (now - g_pu_role_at >= PU_ROLE_MS || sent_police != role_now) {
    /* At once on a change: friends must know before this game's first REPORT as
     * the police. */
    g_pu_role_at = now;
    sent_police = role_now;
    pu_send(sh, 0, PU_ROLE,
            (g_pu_police ? 1u : 0u) | (g_pu_police && g_pu_siren ? 2u : 0u), 0,
            0, 0, 0, 0);
  }
  const uint32_t status = pu_own(player, now);
  const uint32_t p = g_pu_mine, perp = pu_perp(player + 0x24);
  if (status != g_pu_status || now - g_pu_state_at >= PU_STATE_MS) {
    if (status != g_pu_status) {
      char line[96];
      snprintf(line, sizeof line, "core.nfsc: pursuit: our status %u -> %u",
               g_pu_status, status);
      pu_log(line);
    }
    g_pu_status = status;
    g_pu_state_at = now;
    pu_send(sh, 0, PU_STATE, status, p ? get_u32(p + PU_P_LEVEL) : 0,
            perp ? pu_f(perp + 0x1c) : 0.0f, p ? pu_f(p + PU_P_METER) : 0.0f,
            p ? pu_f(p + PU_P_EVADE) : 0.0f,
            p ? (float)get_u32(p + PU_P_COPS) : 0.0f);
  }
  pu_assists(sh, player, now);
}

static void pursuit_install(void) {
  if (g_api->guest_alloc(g_api, sizeof(PuShared), &g_pu_block) != POP_OK) {
    log_line("core.nfsc: shared police chases unavailable (no guest memory)");
    return;
  }
  PuShared *sh = pu_shared();
  if (!sh) {
    g_pu_block = 0;
    return;
  }
  memset(sh, 0, sizeof *sh);
  sh->magic = PU_MAGIC;
  sh->version = 3; /* 2: wish/role at +0x0c/+0x0e (police.h); 3: the police car
                      choice at +0x620 */
  if (g_api->hook_install(g_api, MP_FE_SERVICE, pu_service, POP_HOOK_BEFORE,
                          NULL, &g_pu_hooks[0]) != POP_OK) {
    log_line("core.nfsc: shared police chases unavailable");
    return;
  }
  if (g_api->guest_alloc(g_api, 0x40, &g_pu_buf) != POP_OK)
    g_pu_buf = 0;
  if (g_api->hook_install(g_api, PU_MINIMAP_DOTS, pu_dots_before,
                          POP_HOOK_BEFORE, NULL,
                          &g_pu_dots_hooks[0]) != POP_OK ||
      g_api->hook_install(g_api, PU_MINIMAP_DOTS, pu_dots_after, POP_HOOK_AFTER,
                          NULL, &g_pu_dots_hooks[1]) != POP_OK)
    log_line("core.nfsc: pursuit: police players' minimap colour unavailable");
  g_wm_paint = pu_map_paint;
  if (g_api->hook_install(g_api, PU_RB_DESIGN, pu_design_before,
                          POP_HOOK_BEFORE, NULL, &g_pu_design_hook) != POP_OK)
    log_line(
        "core.nfsc: pursuit: spike strips from police players unavailable");
  g_mp_friend_kind = pu_friend_kind;
  g_mp_friend_car = pu_friend_car;
#ifdef __EMSCRIPTEN__
  MAIN_THREAD_ASYNC_EM_ASM(
      {
        var block = {};
        block.memory = wasmMemory;
        block.ptr = $0;
        globalThis.carbonPursuitBlock = block;
        globalThis.dispatchEvent(new Event("carbon-pursuit"));
      },
      (uint32_t)(uintptr_t)sh);
#endif
}
