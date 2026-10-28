/* rewards.h - Local career cars and shop availability (included
 * by nfsc.c). LOCAL ONLY: does nothing until the page turns it on (below),
 * which menu.js does only off the public site.
 *
 * WHY. After the player beats a crew boss, Carbon deals face-down Boss Bonus
 * Cards (FeBonusCards.fng) and lets the player pick two; one of them may be the
 * boss's pink slip. A player who did not pick that card never gets the boss's
 * car. This file gives, on request, the car of each boss ALREADY BEATEN in this
 * career that the player never received, through the game's own pink-slip
 * award. The separate opening BMW option grants M3GTRCAREERSTART on request.
 * No boss that is not beaten, and no award twice. The independent
 * local performance toggle hooks the shop availability predicate; prices and
 * car-tier limits stay in the game.
 *
 * THE GAME'S OWN AWARD (NFSC.exe v1.4; every address checked in this exe):
 * - Picking a card (0084e220) calls 004d53a0 (thiscall, ECX = the marker
 * manager *0x00a8ad04, args: marker type, value; RET 8). Marker types (names
 * table 0x00a59350, read by 0049f460): 1 bodykit, 2 hood, 3 spoiler, 4 rims, 5
 * roofscoop, 6 hud, 7 vinyls, 8 decals, 9 paint, 0xa out_of_jail_free, 0xb
 * pink_slip, 0xc cash_bonus, 0xd add_impound_box, 0xe reduce_heat.
 * - For 0xb the value is the boss car's preset name hashed upper case
 * (005ea6a0), and 004d53a0 calls 004d51c0 on the profile's car list
 * (FEPlayerCarDB, profile
 *   + 0x234; profile = 00572b90 on *0x00a97a7c, index 0). 004d51c0 finds the
 *   preset (007c3050: the list at 0x00b77004, name at +0x28, model at +0x08,
 *   FE key +0x48, vehicle key +0x50), makes its car record if there is none
 *   (004d1700: handle = the preset's hash, filter 0xf0010, the preset's parts),
 *   copies it into the career garage (004d1620 -> 004ccba0, filter 0xf0002,
 * then
 *   |= 0x40) and writes the boss's look (the preset's RideInfo) into the copy's
 *   customisation record. The bonus cards are queued by the post-race reward
 *   (0067b950 -> 004d5430: in a boss race type 0xb gets DAL 0x179e, which is
 *   004a0c50: the boss character's own name text, *GManager+0x11c -> vtable[0]
 *   = 00617ee0, GetLocalizedString(bStringHash("CREW_MEMBER_%03d", its
 * attribute 0xdb89ab5c)). So Kenji's card is the preset "KENJI" (an RX7), and
 * so on.
 * - 004d51c0 does not check room: with 10 career cars (004ada00(0xf0002) > 9)
 *   004d1620 returns 0 and 004d51c0 writes through it. So the checks below come
 *   first (garage room, free car records, customisation records, career
 * records).
 *
 * WHO IS BEATEN. GManager (*0x00a982b8, built by 00646170): crews in a map at
 * +0xa8 (first node +0xac; node key +0x10, value +0x14; next = 00472980), the
 * player's crew at +0xdc; a crew's boss character at crew + 0x90. Territories
 * in a map at +0x54 (first node +0x58): owner crew at +0x14, flags at +0x1c (4
 * = a boss race, 006404c0), attributes at +4 (Attrib instance; 00463a80 reads
 * one): 0x1e6fd2cb = the crew it starts with, 0x7b6499fd = its race. The save
 * writes owner and flags (00632490), so they are right once a career is loaded
 * (Resume Career; before that they are the defaults). A boss counts as beaten
 * when every boss-race territory of the boss's crew belongs to the player's
 * crew. Seen with a real career (Kenji beaten): Kenji's two boss territories
 * belong to the player (flags 0x47/0x67, +0x18 = Kenji's crew), Angie's, Wolf's
 * and Darius's to them. The boss's name text is the game's own (here "KENJI",
 * "ANGIE", "WOLF", "DARIUS"), and the presets are KENJI (RX7), ANGIE
 * (CHARGER69), WOLF (DB9), DARIUS (R8).
 *
 * ALREADY OWNED. A career car (filter has 0xf0000 and 2) with the 0x40 mark and
 * the preset's FE and vehicle keys is the boss car in My Cars. Cars can be
 * sold, so every car given here is also written down in the save folder,
 * boss-cars.txt (RECOMP_PROFILE_DIR; one line per car: career name (profile +
 * 0x10), boss name, preset hash), and is never offered again for that career,
 * even once sold. (The preset's own car record, handle = its hash, is no sign
 * of an award: the main menu's showcase leaves one for DARIUS in a career that
 * has not beaten him.)
 *
 * SAVING. After an award this file posts the game's own EAutoSave event (event
 * memory from 00561590(8, 0), constructor 0066bab0); when it is handled it asks
 * the autosave manager (*0x00a97bd0: +0x15 autosave on, +9 save requested,
 * 00580780) for a save, exactly as the game does after races. The game carries
 * the request out when the player next leaves the front end (seen: Free Roam
 * from the Safehouse, game state 3 -> 6, the save file written then). With
 * autosave off the player saves from the game's menu as usual (the page says
 * so).
 *
 * WHEN. Before cFEng::Service (00600c90), while the game state byte 0x00a99bbc
 * is 3 (front end: menus, Safehouse), never while driving or loading.
 *
 * THE PAGE (web/menu.js). A block of guest memory (RwShared, published as
 * globalThis.carbonRewardsBlock {memory, ptr} with a "carbon-rewards" event):
 *   +0x00 magic "RWD1", +0x04 version 2 for performance, version 3 for visual unlock hooks, version 4 with BMW boost,
 *   +0x08 page: RW_LOCAL turns this file on; anything else leaves it idle,
 *   +0x0c page: request number (+1 per request), +0x10 page: which boss
 * (index), +0x14 game: the request number handled last, +0x18 game: its result
 * (RW_R_*), +0x1c game: status number (odd while the game writes the status
 * below), +0x20 where (RW_W_*), +0x24 career cars in My Cars, +0x28 the
 * garage's size, +0x2c autosave on, +0x30 bosses listed, +0x34 performance
 * toggle (0/1), +0x38 visual toggle (0/1), +0x3c BMW boost (0/1), +0x40 the bosses (0x40 bytes each): state (RW_S_*), preset
 * hash, crew key, spare, name[24], model[24]. The switch is in this block, not
 * an environment variable: the public site serves the same wasm, and only the
 * page knows where it runs (window.recompSite).
 *
 * TESTED 2026-10-26 with a copy of a real career (Kenji beaten, his pink slip
 * not picked): only Kenji was offered; Add put his RX-7 in Car Select with his
 * look (black, white vinyls, green wing), drivable in Free Roam; it was there
 * after a reload; a second request was refused; after selling it, it was not
 * offered again; Saves -> Undo put the save from before Add back. On a page
 * with recompSite set, nothing showed and a request written into the block was
 * never handled.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define RW_MAGIC 0x31445752u /* "RWD1" */
#define RW_LOCAL 0x4c41434fu /* "OCAL": the page's local-only switch */
#define RW_MAX 6

#define RW_W_NONE 0  /* no career read yet */
#define RW_W_SAFE 1  /* front end: an award can be made now */
#define RW_W_LATER 2 /* driving, a race or loading: not now */

#define RW_S_NOT_BEATEN 0
#define RW_S_MISSING 1 /* beaten, the car was never given: can be added */
#define RW_S_OWNED 2   /* beaten, the car is in My Cars */
#define RW_S_GIVEN                                                             \
  3 /* beaten, the car was given before and is no longer in My Cars */
#define RW_S_NO_CAR                                                            \
  4 /* beaten, but the game has no preset car by the boss's name */

#define RW_R_OK 1
#define RW_R_NOT_NOW 2 /* not in the front end */
#define RW_R_NOT_DUE                                                           \
  3                 /* that boss is not beaten, or the car was given already */
#define RW_R_FULL 4 /* My Cars is full */
#define RW_R_FAILED 5 /* the game's award did not add the car */

#define RW_GAME_STATE 0x00a99bbcu
#define RW_FEDATABASE 0x00a97a7cu
#define RW_GET_PROFILE 0x00572b90u /* thiscall (int index) -> UserProfile * */
#define RW_CAR_DB 0x234u
#define RW_GMANAGER 0x00a982b8u
#define RW_MARKERS 0x00a8ad04u
#define RW_AWARD 0x004d53a0u /* thiscall (marker type, value), RET 8 */
#define RW_PINK_SLIP 0xbu
#define RW_ATTR                                                                \
  0x00463a80u /* thiscall Attrib::Instance::GetAttributePointer(key, index) */
#define RW_NEXT 0x00472980u     /* cdecl (node) -> the next node */
#define RW_LOCALIZE 0x00578830u /* cdecl (hash) -> const char * */
#define RW_FIND_PRESET                                                         \
  0x007c3050u /* cdecl (upper-case name hash) -> FEPresetCar * */
#define RW_EVENT_NEW 0x00561590u /* cdecl (size, 0) -> queued event memory */
#define RW_AUTOSAVE_EVENT 0x0066bab0u /* thiscall EAutoSave::EAutoSave() */
#define RW_AUTOSAVE_MGR 0x00a97bd0u

#define RW_CARS 200 /* car records, 0x14 bytes each */
#define RW_CUSTOM_AT                                                           \
  0x1058u /* customisation records: in-use byte, 0x470 apart */
#define RW_CUSTOMS 0x4b
#define RW_CAREER_AT                                                           \
  0x15c70u /* career car records: in-use byte, 0x34 apart (004ad4f0) */
#define RW_CAREERS 10
#define RW_GARAGE 10 /* 004d1620: no more than 10 career cars */

typedef struct {
  uint32_t state, preset, crew_key, spare;
  char name[24];
  char model[24];
} RwBoss;
typedef struct {
  uint32_t magic, version;
  uint32_t local, request, want;
  uint32_t done, result;
  uint32_t seq, where, garage, garage_max, autosave, count;
  uint32_t performance, visuals, bmw; /* +0x34/+0x38 shops, +0x3c custom BMW boost */
  RwBoss boss[RW_MAX];
} RwShared;
_Static_assert(sizeof(RwBoss) == 0x40 && offsetof(RwShared, seq) == 0x1c &&
                   offsetof(RwShared, count) == 0x30 &&
                   offsetof(RwShared, boss) == 0x40,
               "RwShared layout");

static uint32_t g_rw_block;
static uint32_t g_rw_hooks[7];
static uint32_t g_rw_frames, g_rw_done;
static char g_rw_logged[RW_MAX * 64];

static RwShared *rw_shared(void) {
  void *p = NULL;
  return g_rw_block && g_api->guest_ptr(g_api, g_rw_block, sizeof(RwShared),
                                        &p) == POP_OK
             ? p
             : NULL;
}
static uint32_t rw_call(uint32_t addr, uint32_t ecx, const uint32_t *args,
                        uint32_t n) {
  uint32_t eax = 0;
  return g_api->guest_call(g_api, addr, ecx, args, n, &eax) == POP_OK ? eax : 0;
}
/* An attribute of a game object whose Attrib instance is at +4 (0 if it has
 * none). */
static uint32_t rw_attr(uint32_t object, uint32_t key) {
  const uint32_t args[2] = {key, 0};
  const uint32_t p = object ? rw_call(RW_ATTR, object + 4, args, 2) : 0;
  return p ? get_u32(p) : 0;
}
static void rw_text(uint32_t addr, char *out, size_t size) {
  size_t n = 0;
  uint8_t c = 0;
  while (addr && n + 1 < size &&
         g_api->guest_read_u8(g_api, addr + (uint32_t)n, &c) == POP_OK && c)
    out[n++] = (char)c;
  out[n] = 0;
}
/* bStringHash (00471050), or with a-z in upper case as 005ea6a0 does. */
static uint32_t rw_hash(const char *t, int upper) {
  uint32_t h = 0xffffffffu;
  for (; *t; ++t) {
    uint8_t c = (uint8_t)*t;
    if (upper && c >= 'a' && c <= 'z')
      c -= 0x20;
    h = h * 33u + c;
  }
  return h;
}

/* The tree maps GManager keeps: first node at head + 4, the head ends the walk.
 */
#define RW_EACH(node, head)                                                    \
  for (uint32_t node = get_u32((head) + 4), node##_n = 0;                      \
       node && node != (head) && node##_n < 512;                               \
       node = rw_call(RW_NEXT, 0, &node, 1), ++node##_n)

typedef struct {
  uint32_t crew_key, crew, boss_turfs, owned_turfs;
} RwCrew;

static uint32_t rw_profile(void) {
  const uint32_t db = get_u32(RW_FEDATABASE);
  const uint32_t zero = 0;
  return db ? rw_call(RW_GET_PROFILE, db, &zero, 1) : 0;
}

/* The boss crews of this career, from the boss-race territories. */
static int rw_crews(uint32_t gm, RwCrew *out) {
  const uint32_t player = get_u32(gm + 0xdc);
  int n = 0;
  RW_EACH(node, gm + 0x54) {
    const uint32_t turf = get_u32(node + 0x14);
    uint8_t flags = 0;
    if (!turf || g_api->guest_read_u8(g_api, turf + 0x1c, &flags) != POP_OK ||
        !(flags & 4))
      continue;
    const uint32_t key = rw_attr(turf, 0x1e6fd2cbu);
    if (!key)
      continue;
    int i = 0;
    while (i < n && out[i].crew_key != key)
      ++i;
    if (i == n) {
      if (n == RW_MAX)
        continue;
      memset(&out[n++], 0, sizeof *out);
      out[i].crew_key = key;
    }
    ++out[i].boss_turfs;
    out[i].owned_turfs += player && get_u32(turf + 0x14) == player;
  }
  /* Their crew objects; the player's own crew is never a boss. */
  int kept = 0;
  for (int i = 0; i < n; ++i) {
    RW_EACH(node, gm + 0xa8) {
      if (get_u32(node + 0x10) == out[i].crew_key) {
        out[i].crew = get_u32(node + 0x14);
        break;
      }
    }
    if (out[i].crew && out[i].crew != player && get_u32(out[i].crew + 0x90))
      out[kept++] = out[i];
  }
  return kept;
}

/* What the car list says about one preset. */
typedef struct {
  int in_garage, garage, free_cars, free_customs, free_careers,
      custom_preset_cars;
} RwCars;
static RwCars rw_cars(uint32_t db, uint32_t preset) {
  RwCars r = {0};
  const uint32_t fe_key = preset ? get_u32(preset + 0x48) : 0,
                 vehicle = preset ? get_u32(preset + 0x50) : 0;
  for (uint32_t i = 0; i < RW_CARS; ++i) {
    const uint32_t car = db + 0x14u * i, handle = get_u32(car),
                   filter = get_u32(car + 0xc);
    uint8_t custom = 0xff;
    g_api->guest_read_u8(g_api, car + 0x10, &custom);
    if (handle == 0xffffffffu) {
      ++r.free_cars;
      continue;
    }
    const int career = (filter & 0xf0000u) && (filter & 2u);
    r.garage += career;
    if (preset && career && (filter & 0x40u) && get_u32(car + 4) == fe_key &&
        get_u32(car + 8) == vehicle)
      r.in_garage = 1;
    if (custom != 0xff && (filter & 0xf0000u) && (filter & 0x138u))
      ++r.custom_preset_cars; /* 004ad880 */
  }
  for (uint32_t i = 0; i < RW_CUSTOMS; ++i) {
    uint8_t used = 0;
    g_api->guest_read_u8(g_api, db + RW_CUSTOM_AT + 0x470u * i, &used);
    r.free_customs += used == 0xff;
  }
  for (uint32_t i = 0; i < RW_CAREERS; ++i) {
    uint8_t used = 0;
    g_api->guest_read_u8(g_api, db + RW_CAREER_AT + 0x34u * i, &used);
    r.free_careers += used == 0xff;
  }
  return r;
}

/* boss-cars.txt: the cars this file gave, so a sold one is not given again.
 * Read once (only this file writes it while the game runs). */
static char g_rw_given[2048];
static int g_rw_given_read;
static int rw_given(const char *career, uint32_t preset) {
  if (!g_rw_given_read) {
    g_rw_given_read = 1;
    const char *dir = getenv("RECOMP_PROFILE_DIR");
    char path[512];
    FILE *f = dir && snprintf(path, sizeof path, "%s/boss-cars.txt", dir) <
                          (int)sizeof path
                  ? fopen(path, "r")
                  : NULL;
    if (f) {
      const size_t n = fread(g_rw_given, 1, sizeof g_rw_given - 1, f);
      g_rw_given[n] = 0;
      fclose(f);
    }
  }
  char line[96];
  snprintf(line, sizeof line, "%s\t", career);
  for (const char *at = g_rw_given; at && *at;) {
    const char *end = strchr(at, '\n');
    if (!strncmp(at, line, strlen(line))) {
      const char *hex =
          memchr(at + strlen(line), '\t',
                 (size_t)((end ? end : at + strlen(at)) - at) - strlen(line));
      if (hex && strtoul(hex + 1, NULL, 16) == preset)
        return 1;
    }
    at = end ? end + 1 : NULL;
  }
  return 0;
}
static void rw_note_given(const char *career, const RwBoss *b) {
  char line[96];
  snprintf(line, sizeof line, "%s\t%s\t%08x\n", career, b->name, b->preset);
  const size_t have = strlen(g_rw_given);
  if (have + strlen(line) < sizeof g_rw_given)
    memcpy(g_rw_given + have, line, strlen(line) + 1);
  const char *dir = getenv("RECOMP_PROFILE_DIR");
  char path[512];
  FILE *f = dir && snprintf(path, sizeof path, "%s/boss-cars.txt", dir) <
                        (int)sizeof path
                ? fopen(path, "a")
                : NULL;
  if (f) {
    fputs(line, f);
    fclose(f);
  } else
    log_line("core.nfsc: boss-cars.txt could not be written");
}
static void rw_career(uint32_t profile, char *out, size_t size) {
  rw_text(profile ? profile + 0x10 : 0, out, size);
  for (char *c = out; *c; ++c)
    if (*c == '\t' || *c == '\n')
      *c = ' ';
}

/* One boss: its name as the game shows it, its car's preset, what the player
 * has. */
static void rw_boss(uint32_t db, const char *career, const RwCrew *c,
                    RwBoss *b) {
  memset(b, 0, sizeof *b);
  b->crew_key = c->crew_key;
  const uint32_t boss = get_u32(c->crew + 0x90);
  char key[32];
  snprintf(key, sizeof key, "CREW_MEMBER_%03u", rw_attr(boss, 0xdb89ab5cu));
  const uint32_t hash = rw_hash(key, 0);
  rw_text(rw_call(RW_LOCALIZE, 0, &hash, 1), b->name, sizeof b->name);
  b->preset = rw_hash(b->name, 1);
  const uint32_t preset =
      b->name[0] ? rw_call(RW_FIND_PRESET, 0, &b->preset, 1) : 0;
  if (preset)
    rw_text(preset + 0x08, b->model, sizeof b->model);
  if (!c->boss_turfs || c->owned_turfs < c->boss_turfs)
    b->state = RW_S_NOT_BEATEN;
  else if (!preset)
    b->state = RW_S_NO_CAR;
  else {
    const RwCars cars = rw_cars(db, preset);
    b->state = cars.in_garage                ? RW_S_OWNED
               : rw_given(career, b->preset) ? RW_S_GIVEN
                                             : RW_S_MISSING;
  }
}

/* The game's opening BMW, with its original preset parts and appearance.
 * Uses the same bounded garage award and per-career duplicate record as boss cars. */
static void rw_opening_bmw(uint32_t db, const char *career, RwBoss *b) {
  memset(b, 0, sizeof *b);
  b->spare = 1; /* page: an opening car, not a beaten boss */
  snprintf(b->name, sizeof b->name, "Opening BMW");
  b->preset = rw_hash("M3GTRCAREERSTART", 1);
  const uint32_t preset = rw_call(RW_FIND_PRESET, 0, &b->preset, 1);
  if (!preset) {
    b->state = RW_S_NO_CAR;
    return;
  }
  rw_text(preset + 0x08, b->model, sizeof b->model);
  const RwCars cars = rw_cars(db, preset);
  b->state = cars.in_garage ? RW_S_OWNED
             : rw_given(career, b->preset) ? RW_S_GIVEN : RW_S_MISSING;
}

static int rw_status(RwShared *sh, uint32_t *db_out, char *career,
                     RwBoss *bosses) {
  uint8_t state = 0;
  g_api->guest_read_u8(g_api, RW_GAME_STATE, &state);
  const uint32_t gm = get_u32(RW_GMANAGER),
                 profile = state == 3 ? rw_profile() : 0;
  int n = 0;
  uint32_t where = RW_W_LATER, garage = 0, autosave = 0;
  if (state == 3 && (!gm || !profile))
    where = RW_W_NONE;
  if (state == 3 && gm && profile) {
    const uint32_t db = profile + RW_CAR_DB;
    RwCrew crews[RW_MAX];
    rw_career(profile, career, 32);
    n = rw_crews(gm, crews);
    for (int i = 0; i < n; ++i)
      rw_boss(db, career, &crews[i], &bosses[i]);
    if (n < RW_MAX)
      rw_opening_bmw(db, career, &bosses[n++]);
    garage = (uint32_t)rw_cars(db, 0).garage;
    const uint32_t saver = get_u32(RW_AUTOSAVE_MGR);
    uint8_t on = 0;
    if (saver)
      g_api->guest_read_u8(g_api, saver + 0x15, &on);
    autosave = on != 0;
    where = RW_W_SAFE;
    *db_out = db;
  }
  if (where == RW_W_LATER && sh->count) /* keep the last list while driving */
    n = -1;
  __atomic_add_fetch(&sh->seq, 1, __ATOMIC_ACQ_REL);
  sh->where = where;
  if (n >= 0) {
    sh->garage = garage;
    sh->garage_max = RW_GARAGE;
    sh->autosave = autosave;
    sh->count = (uint32_t)n;
    memcpy(sh->boss, bosses, sizeof(RwBoss) * (size_t)n);
  }
  __atomic_add_fetch(&sh->seq, 1, __ATOMIC_ACQ_REL);
  /* One log line per boss whenever what it says changes. */
  for (int i = 0; i < n; ++i) {
    char line[64];
    snprintf(line, sizeof line, "%s %s %u", bosses[i].name, bosses[i].model,
             bosses[i].state);
    if (strncmp(g_rw_logged + 64 * i, line, 63)) {
      strncpy(g_rw_logged + 64 * i, line, 63);
      char out[160];
      static const char *const says[] = {
          "not beaten", "beaten, car never given", "beaten, car in My Cars",
          "beaten, car given before (not in My Cars now)",
          "beaten, no preset car by that name"};
      snprintf(out, sizeof out,
               "core.nfsc: boss %s (crew %08x): %s; car %s (preset %08x)",
               bosses[i].name, bosses[i].crew_key,
               says[bosses[i].state < 5 ? bosses[i].state : 0],
               bosses[i].model[0] ? bosses[i].model : "-", bosses[i].preset);
      log_line(out);
    }
  }
  return n;
}

/* The game's pink-slip award for one beaten boss whose car was never given. */
static uint32_t rw_award(uint32_t db, const char *career, const RwBoss *b) {
  const uint32_t preset = rw_call(RW_FIND_PRESET, 0, &b->preset, 1),
                 markers = get_u32(RW_MARKERS);
  if (b->state != RW_S_MISSING || !preset)
    return RW_R_NOT_DUE;
  const RwCars cars = rw_cars(db, preset);
  if (cars.in_garage || rw_given(career, b->preset))
    return RW_R_NOT_DUE;
  if (cars.garage >= RW_GARAGE)
    return RW_R_FULL;
  /* Room for the preset's record and the career copy (004d1700, 004ccba0,
   * 004d1620). */
  if (!markers || cars.free_cars < 2 || cars.free_customs < 2 ||
      cars.free_careers < 1 || cars.custom_preset_cars > 0x2c)
    return RW_R_FAILED;
  const uint32_t args[2] = {RW_PINK_SLIP, b->preset};
  rw_call(RW_AWARD, markers, args, 2);
  if (!rw_cars(db, preset).in_garage)
    return RW_R_FAILED;
  rw_note_given(career, b);
  /* Let the game save as it does after a race. */
  const uint32_t ev_args[2] = {8, 0};
  const uint32_t ev = rw_call(RW_EVENT_NEW, 0, ev_args, 2);
  if (ev)
    rw_call(RW_AUTOSAVE_EVENT, ev, NULL, 0);
  char line[128];
  snprintf(line, sizeof line,
           "core.nfsc: boss %s's car (%s, preset %08x) added to My Cars by the "
           "pink-slip award%s",
           b->name, b->model, b->preset, ev ? "; autosave asked" : "");
  log_line(line);
  return RW_R_OK;
}

static void rw_service(const PopModApi *api, pop_cpu_v1 *cpu,
                       PopHookInvocation *inv, void *user) {
  (void)api;
  (void)cpu;
  (void)inv;
  (void)user;
  RwShared *sh = rw_shared();
  if (!sh || __atomic_load_n(&sh->local, __ATOMIC_ACQUIRE) != RW_LOCAL)
    return; /* the public site never turns it on */
  const uint32_t request = __atomic_load_n(&sh->request, __ATOMIC_ACQUIRE);
  const int asked = request != g_rw_done;
  if (!asked && ++g_rw_frames % 30)
    return; /* the list twice a second, a request at once */
  RwBoss bosses[RW_MAX];
  uint32_t db = 0;
  char career[32] = "";
  const int n = rw_status(sh, &db, career, bosses);
  if (!asked)
    return;
  const uint32_t want = __atomic_load_n(&sh->want, __ATOMIC_ACQUIRE);
  uint32_t result = RW_R_NOT_NOW;
  if (sh->where == RW_W_SAFE && db)
    result = (int)want < n ? rw_award(db, career, &bosses[want]) : RW_R_NOT_DUE;
  if (result == RW_R_OK)
    rw_status(sh, &db, career, bosses);
  sh->result = result;
  g_rw_done = request;
  __atomic_store_n(&sh->done, request, __ATOMIC_RELEASE);
}

/* Performance-shop package availability only; purchases still use the game's
 * prices and save path. 004b0d50 is the shop's unlock predicate. Preserve the
 * original result unless local page opted in. */
static void rw_performance(const PopModApi *api, pop_cpu_v1 *cpu,
                           PopHookInvocation *inv, void *user) {
  (void)api;
  (void)inv;
  (void)user;
  RwShared *sh = rw_shared();
  if (sh && __atomic_load_n(&sh->local, __ATOMIC_ACQUIRE) == RW_LOCAL &&
      __atomic_load_n(&sh->performance, __ATOMIC_ACQUIRE) == 1)
    cpu->eax = 1;
}

/* Fitted-part availability: 00842730 -> 004b0b70(CarPart *).
 * Vinyl choice availability: 00577e40 -> 004b0e70(slot, key).
 * Paint finish lists: 00861aa0 -> 004b0fc0 (PAINT_%d), 004b10d0 (RIM_PAINT_%d).
 * These checks read progression unlocks; the shop still owns fitting/prices. */
static void rw_visuals(const PopModApi *api, pop_cpu_v1 *cpu,
                       PopHookInvocation *inv, void *user) {
  (void)api;
  (void)inv;
  (void)user;
  RwShared *sh = rw_shared();
  if (sh && __atomic_load_n(&sh->local, __ATOMIC_ACQUIRE) == RW_LOCAL &&
      __atomic_load_n(&sh->visuals, __ATOMIC_ACQUIRE) == 1)
    cpu->eax = (cpu->eax & 0xffffff00u) | 1u;
}

/* Car Select (008627c0) reads DAL 0x1b5b via 004b3250, which calls
 * 004b1240(handle). The ordinary unlock rule hides the opening BMW even after
 * the pink-slip award. Preserve that rule except for an owned career copy of
 * this exact preset on a local page. No unlock flags are written to the save. */
static void rw_bmw_available(const PopModApi *api, pop_cpu_v1 *cpu,
                             PopHookInvocation *inv, void *user) {
  (void)inv;
  (void)user;
  const uint32_t handle = get_u32(cpu->esp + 4);
  api->call_original(api, cpu->target, cpu);
  RwShared *sh = rw_shared();
  if ((cpu->eax & 0xffu) || !sh ||
      __atomic_load_n(&sh->local, __ATOMIC_ACQUIRE) != RW_LOCAL ||
      handle == 0xffffffffu)
    return;
  const uint32_t profile = rw_profile();
  if (!profile)
    return;
  const uint32_t db = profile + RW_CAR_DB;
  for (uint32_t i = 0; i < RW_CARS; ++i) {
    const uint32_t car = db + 0x14u * i;
    if (get_u32(car) != handle)
      continue;
    const uint32_t flags = get_u32(car + 0xc);
    if (!(flags & 0xf0000u) || (flags & 0x42u) != 0x42u)
      return;
    const uint32_t key = rw_hash("M3GTRCAREERSTART", 1);
    const uint32_t preset = rw_call(RW_FIND_PRESET, 0, &key, 1);
    if (preset && get_u32(car + 4) == get_u32(preset + 0x48) &&
        get_u32(car + 8) == get_u32(preset + 0x50))
      cpu->eax = (cpu->eax & 0xffffff00u) | 1u;
    return;
  }
}

static void rewards_install(void) {
  if (g_api->guest_alloc(g_api, sizeof(RwShared), &g_rw_block) != POP_OK) {
    log_line("core.nfsc: missed boss cars unavailable (no guest memory)");
    g_rw_block = 0;
    return;
  }
  RwShared *sh = rw_shared();
  if (!sh)
    return;
  memset(sh, 0, sizeof *sh);
  sh->magic = RW_MAGIC;
  sh->version = 1;
  if (g_api->hook_install(g_api, 0x00600c90u, rw_service, POP_HOOK_BEFORE, NULL,
                          &g_rw_hooks[0]) != POP_OK) {
    log_line("core.nfsc: missed boss cars unavailable");
    return;
  }
  if (g_api->hook_install(g_api, 0x004b0d50u, rw_performance, POP_HOOK_AFTER,
                          NULL, &g_rw_hooks[1]) != POP_OK)
    log_line("core.nfsc: performance shop unlock unavailable");
  else
    sh->version = 2;
  if (g_api->hook_install(g_api, 0x004b1240u, rw_bmw_available,
                          POP_HOOK_REPLACE, NULL, &g_rw_hooks[2]) != POP_OK)
    log_line("core.nfsc: opening BMW availability unavailable");
  const PopModStatus parts = g_api->hook_install(
      g_api, 0x004b0b70u, rw_visuals, POP_HOOK_AFTER, NULL, &g_rw_hooks[3]);
  const PopModStatus paints = g_api->hook_install(
      g_api, 0x004b0e70u, rw_visuals, POP_HOOK_AFTER, NULL, &g_rw_hooks[4]);
  const PopModStatus body_paint = g_api->hook_install(
      g_api, 0x004b0fc0u, rw_visuals, POP_HOOK_AFTER, NULL, &g_rw_hooks[5]);
  const PopModStatus rim_paint = g_api->hook_install(
      g_api, 0x004b10d0u, rw_visuals, POP_HOOK_AFTER, NULL, &g_rw_hooks[6]);
  if (parts == POP_OK && paints == POP_OK && body_paint == POP_OK &&
      rim_paint == POP_OK && sh->version == 2)
    sh->version = 3;
  else
    log_line("core.nfsc: visual shop unlock unavailable");
#ifdef __EMSCRIPTEN__
  MAIN_THREAD_ASYNC_EM_ASM(
      {
        var block = {};
        block.memory = wasmMemory;
        block.ptr = $0;
        globalThis.carbonRewardsBlock = block;
        globalThis.dispatchEvent(new Event("carbon-rewards"));
      },
      (uint32_t)(uintptr_t)sh);
#endif
}
