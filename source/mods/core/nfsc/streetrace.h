/* streetrace.h - "Flash to race": street races between friends in free roam
 * (included by nfsc.c after police.h; plans/street-race.md).
 *
 * WHAT THE PLAYERS DO. In free roam online, a player holds the headlight flash
 * (F, or L3 on a controller; lights.h) for SR_HOLD_MS while a friend's car is
 * in front: within SR_REACH metres and within SR_CONE of the nose. The friend's
 * game shows "<name> challenges you: flash back to race". A flash back (held
 * the same way) within SR_ANSWER_MS accepts; anything else is no race. The
 * challenger's game then picks the finish, a node of the game's road network
 * SR_NEAR to SR_FAR metres away (nearest SR_BEST, ahead of its nose when it
 * can), sets its GPS there and sends it to the friend's game, which sets its
 * GPS there too: the game's own route on the minimap and its direction arrows
 * show the way, and the finish is also on the big world map. A 3-2-1-GO runs in
 * the game's own HUD messages; the first to come within SR_RADIUS of the finish
 * wins. Both screens say who won, and the time gap once the other crosses too.
 * Quitting free roam (menus, the Safehouse, an event), leaving the room, being
 * busted or turning police loses; so does going silent for SR_LOST_MS. A police
 * player (police.h) neither challenges nor can be challenged. Free roam only:
 * not in a race of any kind, not in the game's own online.
 *
 * TIMING. Each game times its own player from its own GO. The challenger's GO
 * is SR_COUNT_MS after it sends the start; the friend's game counts the same
 * from the moment the start arrives, less half the page's measured round trip
 * to the relay, so both GOs fall within a few tens of ms. Both games compare
 * the two finish times before announcing the winner and time gap. After the
 * first finish, the other player has 30 seconds to finish; otherwise they
 * forfeit.
 *
 * THE GAME'S OWN PIECES (NFSC.exe v1.4, all listed functions):
 *   - HUD messages: 0065b1b0 (cdecl, const char *), the Lua Hud.ShowMessage
 * that NFSC Extra Options uses ("^text": literal text). It posts the HUD's
 * generic message (0x24-byte message, 0067c290) with the text pointer, so each
 * text keeps its own guest buffer (SR_TEXTS of them).
 *   - The GPS (*0x00a83e3c, a road navigator): GPS_Engage 00433ab0 (cdecl:
 * const UMath::Vector3 *target, float max deviation (below the game's floor:
 * 150 m), bool) -> AL 1 when it found a route from the player's car;
 * GPS_Disengage 0041ecb0; engaged = [gps + 0x6c] != 0. NavigatePlayerTo
 * (0064b8c0) engages it the same way for the career's events.
 *   - The road network (loaded with the track, "RN" attributes, loader
 * 00813980): nodes at [0x00b77ec8], 0x20 bytes each, position (physics space, y
 * up) at +0; the count at [0x00b77e8c]. Segments (0x16 bytes, node indices at
 * +0 / +2) at [0x00b77ecc], count [0x00b77e84].
 *   - Busted: the AIPursuit of this player's car (pursuit.h's pu_pursuit_of),
 * byte +0x144.
 *
 * MESSAGES (web/net.js "street" block; relay {t:"street", to, k, r, a, f}, the
 * relay header's "Street races"): to one player, k the kind, r the race (a
 * number the challenger makes), a a whole number 0-65535, f four numbers. k 1
 * CHALLENGE  a 0 k 2 ACCEPT     a 0 k 3 START      a = ms to GO, f = the finish
 * (x, y, z) k 4 FINISH     f[0] = this player's race time, s k 5 QUIT       a =
 * why (SR_WHY_*): the other wins (or, before the start, no race) k 6 ALIVE
 * every SR_ALIVE_MS from the start to the result k 7 BUSY       a = why not (1
 * racing or asked, 2 police, 3 not in free roam) The page and this file share
 * SrShared (globalThis.carbonStreetBlock, "carbon-street" event): a ring of
 * records to send, a ring received (with the sender's name), the names of the
 * players the page knows, our relay id and the round trip. The game writes
 * `opp` (the other player of a challenge or race): the page then has the relay
 * send that player's car whatever the distance
 * ({t:"follow"}), and puts a QUIT (a = SR_WHY_LEFT, r 0) into the ring if that
 * player leaves.
 */
#include <stdarg.h>

#define SR_GPS_ENGAGE                                                          \
  0x00433ab0u /* cdecl (const float target[3], float max deviation, bool) ->   \
                 AL */
#define SR_GPS_OFF 0x0041ecb0u /* cdecl GPS_Disengage */
#define SR_GPS                                                                 \
  0x00a83e3cu /* the GPS (WRoadNav subclass); +0x6c engaged, +0x40 its target  \
               */
#define SR_NODES 0x00b77ec8u /* road nodes, 0x20 bytes: position at +0 */
#define SR_NODE_COUNT 0x00b77e8cu
#define SR_MAGIC 0x31525453u /* "STR1" */

/* The numbers (plans/street-race.md). */
#define SR_HOLD_MS                                                             \
  350.0 /* the flash held this long (a tap is 200 ms; a double press for       \
           headlights, < 300) */
#define SR_REACH 60.0f /* metres: the friend's car this near ... */
#define SR_CONE 0.82f  /* ... and within ~35 degrees of the challenger's nose  \
                        */
#define SR_ANSWER_MS                                                           \
  10000.0 /* the challenged player flashes back within this */
#define SR_START_WAIT_MS                                                       \
  6000.0 /* accepted: the start comes within this, or no race */
#define SR_COUNT_MS                                                            \
  4500.0 /* from the start to GO: the finish shown, then 3, 2, 1 */
#define SR_NEAR 1000.0f /* metres: the finish is this far ... */
#define SR_FAR 2000.0f  /* ... to this far away (straight line) ... */
#define SR_BEST 1500.0f /* ... best this far */
#define SR_AHEAD                                                               \
  0.5f /* and within 60 degrees of the challenger's nose when there is one */
#define SR_TRIES 8 /* road nodes tried for a GPS route, best first */
#define SR_RADIUS                                                              \
  25.0f /* metres: within this of the finish (and SR_RADIUS_Y up or down) */
#define SR_RADIUS_Y 12.0f
#define SR_ALIVE_MS 2000.0
#define SR_LOST_MS                                                             \
  12000.0 /* nothing from the other player for this long: they left */
#define SR_TAIL_MS                                                             \
  30000.0 /* after the winner crosses, the gap waits this long for the other   \
           */
#define SR_MAX_MS                                                              \
  480000.0 /* a race nobody finishes is called off after 8 minutes */
#define SR_GONE_MS                                                             \
  3000.0 /* no car of this player's this long (menus, loading): it left */

enum {
  SR_CHALLENGE = 1,
  SR_ACCEPT,
  SR_START,
  SR_FINISH,
  SR_QUIT,
  SR_ALIVE,
  SR_BUSY
};
enum {
  SR_WHY_QUIT = 1,
  SR_WHY_LEFT,
  SR_WHY_BUSTED,
  SR_WHY_POLICE,
  SR_WHY_NO_ROUTE,
  SR_WHY_OFF
};
enum { SR_IDLE, SR_ASKING, SR_ASKED, SR_ACCEPTED, SR_COUNTDOWN, SR_RACING };

#define SR_TX 8
#define SR_RX 16
#define SR_NAMES 16
#define SR_TEXTS 8
#define SR_TEXT 128

typedef struct SrRec { /* 0x30 bytes */
  uint32_t id;         /* to whom (sending) / from whom (received) */
  uint32_t k, r, a;
  float f[4];
  char name[16]; /* received: the sender's name, as the page knows it */
} SrRec;

typedef struct SrName { /* 0x14 bytes; written by the page, id last (0 while it
                           writes) */
  uint32_t id;
  char name[16];
} SrName;

typedef struct SrShared {
  uint32_t magic, version;
  uint32_t me; /* +0x08 the page: our relay id, 0 while not linked */
  uint32_t
      opp; /* +0x0c the game: the other player of a challenge or race, 0 none */
  uint32_t ping; /* +0x10 the page: round trip to the relay, ms (0 not known) */
  uint32_t phase;       /* +0x14 the game: SR_IDLE ... */
  uint32_t races, wins; /* +0x18 the game: races started, won */
  uint32_t tx_head, tx_tail, rx_head,
      rx_tail;            /* +0x20 single producer, single consumer */
  SrRec tx[SR_TX];        /* +0x30 */
  SrRec rx[SR_RX];        /* +0x1b0 */
  SrName names[SR_NAMES]; /* +0x4b0 the page */
  float finish[3];        /* +0x5f0 the game: the finish of the race now */
  uint32_t reserved;      /* +0x5fc reserved; no gameplay commands */
  char said[64];          /* +0x600 the game: the last HUD message */
} SrShared;
_Static_assert(sizeof(SrRec) == 0x30 && offsetof(SrShared, tx) == 0x30 &&
                   offsetof(SrShared, rx) == 0x1b0 &&
                   offsetof(SrShared, names) == 0x4b0 &&
                   offsetof(SrShared, finish) == 0x5f0 &&
                   offsetof(SrShared, said) == 0x600 &&
                   sizeof(SrShared) == 0x640,
               "SrShared layout (web/net.js reads it by offset)");

static struct {
  int phase;
  uint32_t opp, race;
  char name[16]; /* the other player's name */
  int host;      /* this game picked the finish */
  double at;     /* when the phase began (mp_now, ms) */
  double go;     /* GO, on our clock */
  float finish[3];
  float dir[3]; /* the way to the finish from where this player was at the start
                   (unit) */
  double mine, theirs; /* race times, s; < 0 not yet */
  double crossed_at;   /* our clock when this player crossed */
  int result;          /* 0 not known, 1 this player won, 2 the other won */
  int gap_said;        /* the gap was shown */
  double heard;        /* the other player's last record */
  double alive_at;
  int shown;         /* countdown step shown: 4 the intro, 3, 2, 1, 0 GO */
  double gone_since; /* no car of ours since */
} g_sr;
static uint32_t g_sr_block, g_sr_text, g_sr_texts, g_sr_vec, g_sr_hooks[3];
static double g_sr_flash_from; /* when the current flash began, 0 none */
static int g_sr_flash_used;    /* this flash already did something */
static double g_sr_link_lost;  /* the page's link has been down since */

static SrShared *sr_shared(void) {
  void *p = NULL;
  return g_sr_block && g_api->guest_ptr(g_api, g_sr_block, sizeof(SrShared),
                                        &p) == POP_OK
             ? p
             : NULL;
}

static void sr_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void sr_log(const char *fmt, ...) {
  char line[256] = "core.nfsc: street: ";
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line + 19, sizeof line - 19, fmt, ap);
  va_end(ap);
  log_line(line);
}

/* The game's HUD messages: IGenericMessage requests, posted the way the game
 * posts them (0067c290, thiscall on a 0x24-byte message: player, text, icon,
 * kind, then 2 for one that stays up (1 for a timed one), 1, a priority). Kinds
 * seen (2026-10-27): 0x9847b3e3 cyan, 0x75a0bdb0 cyan (the career's
 * CHALLENGE_RACE_NOTE), 0xa6797f23 orange (Hud.ShowLoopingMessage's); the last
 * two stay up until a take-down (0067c640, thiscall on 0x14 bytes: player, 1,
 * priority, as HideLoopingMessage does with 3), and a new one of that kind
 * waits behind the one up, so ours are taken down after `hold` ms or before the
 * next. The game's territory banner ("MORGAN BEACH") holds back a priority-3
 * one, so the countdown and the result are priority 1. Hud.ShowMessage
 * (0065b1b0) is kind 0x8ab83edb, white. */
typedef struct SrStyle {
  uint32_t kind, loop, prio;
  double hold; /* ms until we take it down; 0: the game's own time (or until the
                  next) */
} SrStyle;
static const SrStyle k_sr_info = {0x9847b3e3u, 1, 3, 0.0}; /* cyan */
static const SrStyle k_sr_prompt = {0x75a0bdb0u, 2, 3,
                                    0.0}; /* cyan, until answered */
static const SrStyle k_sr_count = {0xa6797f23u, 2, 1,
                                   1500.0}; /* orange: 3, 2, 1 */
static const SrStyle k_sr_go = {0xa6797f23u, 2, 1, 2500.0}; /* orange: GO! */
static const SrStyle k_sr_win = {0xa6797f23u, 2, 1,
                                 6000.0}; /* orange: who won */
static uint32_t g_sr_msg;  /* 0x24 guest bytes: a message (the game never frees
                              its own either) */
static uint32_t g_sr_kill; /* 0x14 guest bytes: a take-down */
static int g_sr_looping;   /* a looping message of ours is up (its priority) */
static double g_sr_unsay_at; /* and is taken down then (0: when the next comes
                                or the race ends) */
#define SR_SAY_GAP                                                             \
  1100.0 /* ms between two messages (the HUD drops one that comes much sooner) \
          */
#define SR_QUEUE 4
static struct {
  const SrStyle *style;
  char text[SR_TEXT - 1];
} g_sr_queue[SR_QUEUE];
static int g_sr_queued;
static double g_sr_said_at = -1e9;
/* The HUD's player number of the first player (IPlayer +0x1c), or -1. */
static int sr_hud_player(void) {
  const uint32_t list = get_u32(0x00a9ff5cu), n = get_u32(0x00a9ff64u),
                 player = n && list ? get_u32(list) : 0;
  const uint32_t index_of = player ? get_u32(get_u32(player) + 0x1c) : 0;
  return index_of ? (int)mp_call(index_of, player, NULL, 0) : -1;
}

/* Takes our looping message down. */
static void sr_unsay(void) {
  if (!g_sr_looping)
    return;
  const int index = sr_hud_player();
  if (index < 0 || !g_sr_kill)
    return;
  const uint32_t args[3] = {(uint32_t)index, 1, (uint32_t)g_sr_looping};
  g_sr_looping = 0;
  mp_call(0x0067c640u, g_sr_kill, args, 3);
}

/* Posts one message now ("^": literal text, not a language key). */
static void sr_post(const SrStyle *style, const char *text) {
  const int index = sr_hud_player();
  if (!g_sr_text || !g_sr_msg || index < 0)
    return;
  sr_unsay(); /* the next replaces ours */
  const uint32_t at = g_sr_text + (g_sr_texts++ % SR_TEXTS) * SR_TEXT;
  g_api->guest_write_u8(g_api, at, '^');
  for (uint32_t i = 0; i + 1 < SR_TEXT; ++i) {
    g_api->guest_write_u8(g_api, at + 1 + i, (uint8_t)text[i]);
    if (!text[i])
      break;
  }
  const uint32_t args[7] = {(uint32_t)index, at, 0,          style->kind,
                            style->loop,     1,  style->prio};
  mp_call(0x0067c290u, g_sr_msg, args, 7);
  g_sr_said_at = mp_now();
  g_sr_looping = style->loop == 2 ? (int)style->prio : 0;
  g_sr_unsay_at =
      g_sr_looping && style->hold > 0 ? g_sr_said_at + style->hold : 0;
  SrShared *sh = sr_shared();
  if (sh) {
    memset(sh->said, 0, sizeof sh->said);
    strncpy(sh->said, text, sizeof sh->said - 1);
  }
  sr_log("says \"%s\"", text);
}

/* A HUD message. The HUD drops a message that comes too soon after the last
 * one, so messages wait SR_SAY_GAP after the previous one (a queue of
 * SR_QUEUE); an urgent one (the countdown) goes at once and drops the waiting
 * ones. */
static void sr_say_as(int urgent, const SrStyle *style, const char *fmt,
                      va_list ap) {
  char text[SR_TEXT - 1];
  vsnprintf(text, sizeof text, fmt, ap);
  if (urgent || (!g_sr_queued && mp_now() - g_sr_said_at >= SR_SAY_GAP)) {
    g_sr_queued = 0;
    sr_post(style, text);
    return;
  }
  if (g_sr_queued == SR_QUEUE) { /* full: the oldest goes */
    memmove(g_sr_queue, g_sr_queue + 1, sizeof g_sr_queue[0] * (SR_QUEUE - 1));
    --g_sr_queued;
  }
  g_sr_queue[g_sr_queued].style = style;
  memcpy(g_sr_queue[g_sr_queued].text, text, sizeof text);
  ++g_sr_queued;
}
static void sr_say(const SrStyle *style, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void sr_say(const SrStyle *style, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  sr_say_as(0, style, fmt, ap);
  va_end(ap);
}
static void sr_say_now(const SrStyle *style, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void sr_say_now(const SrStyle *style, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  sr_say_as(1, style, fmt, ap);
  va_end(ap);
}
/* Once a frame: the next waiting message, when its time has come. */
static void sr_say_waiting(double now) {
  if (g_sr_looping && g_sr_unsay_at > 0 && now >= g_sr_unsay_at)
    sr_unsay();
  if (!g_sr_queued || now - g_sr_said_at < SR_SAY_GAP)
    return;
  const SrStyle *style = g_sr_queue[0].style;
  char text[SR_TEXT - 1];
  memcpy(text, g_sr_queue[0].text, sizeof text);
  memmove(g_sr_queue, g_sr_queue + 1, sizeof g_sr_queue[0] * (SR_QUEUE - 1));
  --g_sr_queued;
  sr_post(style, text);
}

/* A name the HUD font has: letters, digits, space . - _ (15 at most); "FRIEND"
 * for none. */
static void sr_clean(char out[16], const char *in, size_t max) {
  size_t n = 0;
  for (size_t i = 0; i < max && in[i] && n < 15; ++i) {
    const char c = in[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == ' ' || c == '.' || c == '-' || c == '_')
      out[n++] = c;
  }
  if (!n) {
    memcpy(out, "FRIEND", 6);
    n = 6;
  }
  out[n] = 0;
}

/* The page's name for a player, else "FRIEND". */
static void sr_name_of(const SrShared *sh, uint32_t id, char out[16]) {
  for (int i = 0; sh && id && i < SR_NAMES; ++i)
    if (__atomic_load_n(&sh->names[i].id, __ATOMIC_ACQUIRE) == id) {
      char copy[16];
      memcpy(copy, sh->names[i].name, sizeof copy);
      if (__atomic_load_n(&sh->names[i].id, __ATOMIC_ACQUIRE) == id) {
        sr_clean(out, copy, sizeof copy);
        return;
      }
    }
  sr_clean(out, "", 0);
}

static void sr_send(uint32_t to, uint32_t k, uint32_t a, float f0, float f1,
                    float f2) {
  SrShared *sh = sr_shared();
  if (!sh || !to || to == 0xffffffffu)
    return;
  const uint32_t head = __atomic_load_n(&sh->tx_head, __ATOMIC_RELAXED);
  if (head - __atomic_load_n(&sh->tx_tail, __ATOMIC_ACQUIRE) >= SR_TX) {
    sr_log("send ring full: k %u to %u dropped", k, to);
    return;
  }
  SrRec *r = &sh->tx[head % SR_TX];
  memset(r, 0, sizeof *r);
  r->id = to;
  r->k = k;
  r->r = g_sr.race;
  r->a = a;
  r->f[0] = f0;
  r->f[1] = f1;
  r->f[2] = f2;
  __atomic_store_n(&sh->tx_head, head + 1, __ATOMIC_RELEASE);
}
/* A record about another race (an answer to a challenge we do not take). */
static void sr_send_race(uint32_t to, uint32_t race, uint32_t k, uint32_t a) {
  const uint32_t keep = g_sr.race;
  g_sr.race = race;
  sr_send(to, k, a, 0, 0, 0);
  g_sr.race = keep;
}

/* A car's place and the way its nose points (physics space, y up); 0 if it has
 * none. */
static int sr_where(uint32_t simable, float p[3], float f[3]) {
  const uint32_t s = simable ? mp_state(get_u32(simable - 0x24 + 0x70)) : 0;
  if (!s)
    return 0;
  for (int k = 0; k < 3; ++k) {
    p[k] = bits_float(get_u32(s + 0x10 + 4u * k));
    if (f)
      f[k] = bits_float(get_u32(s + 0x90 + 4u * k));
  }
  return isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2]);
}

/* Whether this player is in free roam now: a car, no event or race, not the
 * game's own online. */
static int sr_free_roam(uint32_t player) {
  return player && !pp_in_race() && !mp_online_game() &&
         get_u32(RACE_FLOW) != 3;
}

static int sr_busted(uint32_t player) {
  const uint32_t p = player ? pu_pursuit_of(player + 0x24) : 0;
  return p && pu_b(p + PU_P_BUSTED);
}

/* The friend whose car is in front of ours (SR_REACH, SR_CONE), as its mp.h
 * slot, or -1. */
static int sr_ahead(uint32_t player) {
  float p[3], f[3], q[3];
  if (!sr_where(player + 0x24, p, f))
    return -1;
  const float fl = sqrtf(f[0] * f[0] + f[2] * f[2]);
  if (!(fl > 0.1f))
    return -1;
  int best = -1;
  float best_d = SR_REACH;
  for (int i = 0; i < MP_SLOTS; ++i) {
    const MpCar *c = &g_mp_remote[i];
    if (!c->simable || !c->id || !sr_where(c->simable, q, NULL))
      continue;
    const float dx = q[0] - p[0], dz = q[2] - p[2],
                d = sqrtf(dx * dx + dz * dz);
    if (d < 0.5f || d > best_d || fabsf(q[1] - p[1]) > 10.0f)
      continue;
    if ((dx * f[0] + dz * f[2]) / (d * fl) < SR_CONE)
      continue;
    best = i;
    best_d = d;
  }
  return best;
}

/* ---- the finish
 * ------------------------------------------------------------------------------
 */

/* The GPS to point p; 1 if the game found a route from our car. */
static int sr_gps(const float p[3]) {
  if (!get_u32(SR_GPS) || !g_sr_vec)
    return 0;
  for (int k = 0; k < 3; ++k)
    put_float(g_sr_vec + 4u * k, p[k]);
  const uint32_t args[3] = {g_sr_vec, 0, 0};
  return (mp_call(SR_GPS_ENGAGE, 0, args, 3) & 0xffu) != 0;
}
static void sr_gps_off(void) {
  if (get_u32(SR_GPS))
    mp_call(SR_GPS_OFF, 0, NULL, 0);
}

/* A road node SR_NEAR..SR_FAR from p (ahead along f when one is) that the GPS
 * finds a route to: the finish, into out. 0 if there is none. */
static int sr_pick_finish(const float p[3], const float f[3], float out[3]) {
  const uint32_t nodes = get_u32(SR_NODES), n = get_u32(SR_NODE_COUNT);
  const float *node = NULL;
  if (!nodes || !n || n > 200000u ||
      g_api->guest_ptr(g_api, nodes, n * 0x20u, (void **)&node) != POP_OK ||
      !node) {
    sr_log("no road network (%08x, %u nodes)", nodes, n);
    return 0;
  }
  const float fl = sqrtf(f[0] * f[0] + f[2] * f[2]);
  for (int pass = 0; pass < 2; ++pass) {
    /* the best SR_TRIES by score: off SR_BEST, and turned away from the nose */
    uint32_t pick[SR_TRIES];
    float score[SR_TRIES];
    int have = 0;
    for (uint32_t i = 0; i < n; ++i) {
      const float *q = node + 8u * i;
      const float dx = q[0] - p[0], dz = q[2] - p[2],
                  d = sqrtf(dx * dx + dz * dz);
      if (!isfinite(d) || d < SR_NEAR || d > SR_FAR || !isfinite(q[1]) ||
          fabsf(q[1] - p[1]) > 150.0f)
        continue;
      const float cosine =
          fl > 0.1f ? (dx * f[0] + dz * f[2]) / (d * fl) : 1.0f;
      if (pass == 0 && cosine < SR_AHEAD)
        continue;
      const float s = fabsf(d - SR_BEST) / (SR_FAR - SR_BEST) + (1.0f - cosine);
      int at = have < SR_TRIES ? have++ : SR_TRIES;
      if (at == SR_TRIES) {
        if (s >= score[SR_TRIES - 1])
          continue;
        at = SR_TRIES - 1;
      }
      while (at > 0 && score[at - 1] > s) {
        score[at] = score[at - 1];
        pick[at] = pick[at - 1];
        --at;
      }
      score[at] = s;
      pick[at] = i;
    }
    for (int t = 0; t < have; ++t) {
      const float *q = node + 8u * pick[t];
      if (sr_gps(q)) {
        memcpy(out, q, 3 * sizeof(float));
        const float dx = q[0] - p[0], dz = q[2] - p[2];
        sr_log("finish: road node %u at %.1f %.1f %.1f, %.0f m away (%s, try "
               "%d of %d)",
               pick[t], q[0], q[1], q[2], sqrtf(dx * dx + dz * dz),
               pass ? "any way" : "ahead", t + 1, have);
        return 1;
      }
    }
    sr_log("finish: no GPS route to %d road nodes %s", have,
           pass ? "around" : "ahead");
  }
  return 0;
}

/* ---- the race
 * --------------------------------------------------------------------------------
 */

static void sr_phase(int phase, double now) {
  if (g_sr.phase != phase)
    sr_log("phase %d -> %d (player %u, race %08x)", g_sr.phase, phase, g_sr.opp,
           g_sr.race);
  g_sr.phase = phase;
  g_sr.at = now;
}

/* Back to nothing: the GPS off if a race had set it, the messages reset. */
static void sr_end(double now) {
  if (g_sr_looping && !g_sr_unsay_at)
    sr_unsay();
  /* Discard old prompts/countdowns; a queued result still deserves its display
   * time. */
  int kept = 0;
  for (int i = 0; i < g_sr_queued; ++i)
    if (g_sr_queue[i].style == &k_sr_win)
      g_sr_queue[kept++] = g_sr_queue[i];
  g_sr_queued = kept;
  if (g_sr.phase >= SR_COUNTDOWN)
    sr_gps_off();
  sr_phase(SR_IDLE, now);
  g_sr.opp = 0;
  g_sr.race = 0;
}

static double sr_clock(double now) { return (now - g_sr.go) / 1000.0; }

/* The countdown has begun: this game's GO at go. */
static void sr_begin(const float finish[3], double go, double now, int route) {
  memcpy(g_sr.finish, finish, sizeof g_sr.finish);
  {
    float p[3];
    const uint32_t player = mp_player();
    g_sr.dir[0] = 1.0f;
    g_sr.dir[1] = g_sr.dir[2] = 0.0f;
    if (player && sr_where(player + 0x24, p, NULL)) {
      const float dx = finish[0] - p[0], dz = finish[2] - p[2],
                  d = sqrtf(dx * dx + dz * dz);
      if (d > 1.0f) {
        g_sr.dir[0] = dx / d;
        g_sr.dir[2] = dz / d;
      }
    }
  }
  g_sr.go = go;
  g_sr.mine = g_sr.theirs = -1;
  g_sr.result = g_sr.gap_said = 0;
  g_sr.heard = g_sr.alive_at = now;
  g_sr.shown = 5;
  g_sr.gone_since = 0;
  sr_phase(SR_COUNTDOWN, now);
  SrShared *sh = sr_shared();
  if (sh) {
    memcpy(sh->finish, finish, sizeof sh->finish);
    ++sh->races;
  }
  sr_log("race %08x vs %u (%s): finish %.1f %.1f %.1f, GO in %.0f ms, %s",
         g_sr.race, g_sr.opp, g_sr.name, finish[0], finish[1], finish[2],
         go - now, route ? "GPS route set" : "no GPS route here");
}

/* The other player won (2) or this one (1); said once. */
static void sr_result(int who, const char *why) {
  if (g_sr.result)
    return;
  g_sr.result = who;
  SrShared *sh = sr_shared();
  if (sh && who == 1)
    ++sh->wins;
  sr_log("result: %s (%s); times: ours %.3f, theirs %.3f",
         who == 1 ? "this player won" : "the other won", why, g_sr.mine,
         g_sr.theirs);
}

/* The winner by the times (both known), said with the gap. */
static void sr_by_times(void) {
  if (g_sr.result)
    return;
  SrShared *sh = sr_shared();
  const uint32_t me = sh ? sh->me : 0; /* a tie to the ms: the lower relay id */
  sr_result(g_sr.mine < g_sr.theirs ||
                    (g_sr.mine == g_sr.theirs && me < g_sr.opp)
                ? 1
                : 2,
            "both times");
  g_sr.gap_said = 1;
  const double gap = fabs(g_sr.mine - g_sr.theirs);
  if (g_sr.result == 1)
    sr_say_now(&k_sr_win, "You win by %.2f s", gap);
  else
    sr_say_now(&k_sr_win, "%s wins by %.2f s", g_sr.name, gap);
}

/* This player gives up the race (it lost): the other is told why. */
static void sr_forfeit(uint32_t why, double now) {
  sr_send(g_sr.opp, SR_QUIT, why, 0, 0, 0);
  if (g_sr.phase >= SR_COUNTDOWN && !g_sr.result) {
    sr_result(2, "this player left");
    sr_say_now(&k_sr_win,
               why == SR_WHY_BUSTED   ? "Busted: %s wins the race"
               : why == SR_WHY_POLICE ? "Police now: %s wins the race"
                                      : "You left the race: %s wins",
               g_sr.name);
  }
  sr_end(now);
}

/* A record from the page. */
static void sr_receive(const SrRec *r, uint32_t player, double now) {
  SrShared *sh = sr_shared();
  char name[16];
  sr_clean(name, r->name, sizeof r->name);
  if (r->k == SR_CHALLENGE) {
    const int mutual = g_sr.phase == SR_ASKING && g_sr.opp == r->id;
    if (mutual && sh && sh->me < r->id)
      return; /* both flashed: the lower id's challenge stands, the other
                 accepts it */
    if (!mutual && g_sr.phase != SR_IDLE) {
      sr_send_race(r->id, r->r, SR_BUSY, 1);
      return;
    }
    if (g_pu_police || !sr_free_roam(player)) {
      sr_send_race(r->id, r->r, SR_BUSY, g_pu_police ? 2 : 3);
      sr_log("challenge from %u (%s) refused: %s", r->id, name,
             g_pu_police ? "police" : "not in free roam");
      return;
    }
    g_sr.opp = r->id;
    g_sr.race = r->r;
    g_sr.host = 0;
    memcpy(g_sr.name, name, sizeof g_sr.name);
    if (mutual) {
      sr_send(g_sr.opp, SR_ACCEPT, 0, 0, 0, 0);
      sr_phase(SR_ACCEPTED, now);
      sr_say(&k_sr_info, "%s flashed you too: race on!", name);
      return;
    }
    sr_phase(SR_ASKED, now);
    if (g_sr_flash_from > 0)
      g_sr_flash_from = now; /* a flash held from before counts from now */
    sr_say(&k_sr_prompt, "%s challenges you: flash back to race", name);
    return;
  }
  if (r->id != g_sr.opp ||
      (r->r != g_sr.race &&
       !(r->r == 0 && r->k == SR_QUIT && r->a == SR_WHY_LEFT)))
    return; /* about another race */
  g_sr.heard = now;
  switch (r->k) {
  case SR_ACCEPT:
    if (g_sr.phase != SR_ASKING || !sr_free_roam(player) || g_pu_police)
      return;
    {
      float p[3], f[3], finish[3];
      if (!sr_where(player + 0x24, p, f) || !sr_pick_finish(p, f, finish)) {
        sr_send(g_sr.opp, SR_QUIT, SR_WHY_NO_ROUTE, 0, 0, 0);
        sr_say_now(&k_sr_win, "No finish found from here: race off");
        sr_end(now);
        return;
      }
      sr_send(g_sr.opp, SR_START, (uint32_t)SR_COUNT_MS, finish[0], finish[1],
              finish[2]);
      sr_begin(finish, now + SR_COUNT_MS, now, 1);
    }
    return;
  case SR_START:
    if (g_sr.phase != SR_ACCEPTED || !sr_free_roam(player) || g_pu_police ||
        !isfinite(r->f[0]) || !isfinite(r->f[1]) || !isfinite(r->f[2]))
      return;
    {
      const double half = sh && sh->ping < 2000 ? sh->ping / 2.0 : 0.0;
      const double in =
          (r->a > 0 && r->a <= 10000 ? (double)r->a : SR_COUNT_MS) - half;
      const float finish[3] = {r->f[0], r->f[1], r->f[2]};
      if (!sr_gps(finish)) {
        sr_send(g_sr.opp, SR_QUIT, SR_WHY_NO_ROUTE, 0, 0, 0);
        sr_say_now(&k_sr_win, "No GPS route: race off");
        sr_end(now);
        return;
      }
      sr_begin(finish, now + (in > 0 ? in : 0), now, 1);
    }
    return;
  case SR_FINISH:
    if (g_sr.phase != SR_RACING || g_sr.theirs >= 0 || !isfinite(r->f[0]) ||
        r->f[0] < 0 || r->f[0] > SR_MAX_MS / 1000.0)
      return;
    g_sr.theirs = r->f[0];
    sr_log("%s finished in %.3f s", g_sr.name, g_sr.theirs);
    if (!g_sr.result && g_sr.mine >= 0)
      sr_by_times();
    return;
  case SR_QUIT:
    if (r->a == SR_WHY_OFF || r->a == SR_WHY_NO_ROUTE) {
      sr_say_now(&k_sr_win, "Race called off");
      sr_end(now);
      return;
    }
    if (g_sr.phase == SR_ASKING || g_sr.phase == SR_ACCEPTED ||
        g_sr.phase == SR_ASKED) {
      if (r->a == SR_WHY_NO_ROUTE)
        sr_say_now(&k_sr_win, "No finish found: race off");
      else if (g_sr.phase != SR_ASKED)
        sr_say_now(&k_sr_win, "%s left: race off", g_sr.name);
      sr_end(now);
      return;
    }
    if (g_sr.phase >= SR_COUNTDOWN && !g_sr.result) {
      sr_result(1, "the other left");
      sr_say_now(&k_sr_win,
                 r->a == SR_WHY_BUSTED   ? "%s got busted: you win!"
                 : r->a == SR_WHY_POLICE ? "%s is the police now: you win!"
                                         : "%s left the race: you win!",
                 g_sr.name);
    }
    sr_end(now);
    return;
  case SR_BUSY:
    if (g_sr.phase != SR_ASKING)
      return;
    sr_say_now(&k_sr_win,
               r->a == 2 ? "%s is the police: no race" : "%s can't race now",
               g_sr.name);
    sr_end(now);
    return;
  default:
    return; /* ALIVE: heard */
  }
}

/* ---- the finish on the game's maps
 * ------------------------------------------------------------ */

/* Whether the finish is shown: from the start until this player crosses. */
static int sr_finish_shown(void) {
  return (g_sr.phase == SR_COUNTDOWN || g_sr.phase == SR_RACING) &&
         g_sr.mine < 0;
}

/* THE MINIMAP. Its update (in 005e50f0) places the race's finish-line icon
 * (MINIMAP_ICON_FINISHLINE, minimap + 0x168) every frame with 005e1af0
 * (thiscall minimap), at minimap +0x16c / +0x170 (the finish, render x and y:
 * physics z and -x) turned along +0x174 / +0x178 (the way into it, the same
 * way), as the race route code (005d23e0) sets them; 005d1f80 shows it on the
 * minimap, at its edge when farther, and hides it when much farther. A BEFORE
 * hook writes our finish there while it is shown, and puts the game's values
 * back after. */
static uint32_t g_sr_mm;         /* the minimap whose finish we set, 0 none */
static uint32_t g_sr_mm_kept[4]; /* its own values */
static void sr_minimap(const PopModApi *api, pop_cpu_v1 *cpu,
                       PopHookInvocation *inv, void *user) {
  (void)api;
  (void)inv;
  (void)user;
  const uint32_t mm = cpu->ecx;
  if (!mm)
    return;
  if (sr_finish_shown()) {
    if (g_sr_mm != mm) {
      for (uint32_t k = 0; k < 4; ++k)
        g_sr_mm_kept[k] = get_u32(mm + 0x16c + 4u * k);
      g_sr_mm = mm;
    }
    put_float(mm + 0x16c, g_sr.finish[2]);
    put_float(mm + 0x170, -g_sr.finish[0]);
    put_float(mm + 0x174, g_sr.dir[2]);
    put_float(mm + 0x178, -g_sr.dir[0]);
  } else if (g_sr_mm == mm) {
    for (uint32_t k = 0; k < 4; ++k)
      mp_put(mm + 0x16c + 4u * k, g_sr_mm_kept[k]);
    g_sr_mm = 0;
  }
}

/* THE WORLD MAP (worldmap.h has how it works). It has no marker for a point
 * that is not an event, so the finish is a map item of our own, as worldmap.h's
 * friends are: one of the package's icons nobody uses (an event star,
 * k_sr_wm_icons, from the last down), type 1 so every filter shows it, tinted
 * SR_WM_TINT, with a private copy of the plain item's methods so we know it
 * again (and worldmap.h leaves it alone). It is there while the finish is shown
 * and the player's own arrow is (the free-roam map). */
#define SR_WM_TINT                                                             \
  0xffff3030u /* red: no region's events wear it (blue, teal, orange, purple;  \
                 yellow done) */
static const char *const k_sr_wm_icons[] = {
    "MMICON_CHECKPOINT_%d", "MMICON_SPRINT_%d", "MMICON_DRAG_%d"};
static uint32_t g_sr_wm_methods;
static void sr_worldmap(const PopModApi *api, pop_cpu_v1 *cpu,
                        PopHookInvocation *inv, void *user) {
  (void)api;
  (void)inv;
  (void)user;
  const uint32_t map = cpu->ecx;
  if (!map || map != get_u32(WM_MAP) || !g_wm_block || !g_sr_wm_methods)
    return;
  const uint32_t pkg = get_u32(map + 0xc),
                 player = wm_find(pkg, WM_PLAYER_ICON);
  enum { MAX = 512 };
  uint32_t used[MAX], ours = 0;
  int n_used = 0, has_player = 0;
  const uint32_t head = map + 0x64;
  uint32_t node = get_u32(head);
  for (int guard = 0; node && node != head && guard < 512;
       node = get_u32(node), ++guard) {
    const uint32_t item = node - 4, image = get_u32(item + 0xc);
    if (get_u32(item) == g_sr_wm_methods)
      ours = ours ? ours : item;
    else if (image && image == player)
      has_player = 1;
    if (image && n_used < MAX)
      used[n_used++] = image;
  }
  if (!sr_finish_shown() || !has_player) {
    if (ours)
      wm_drop(ours);
    return;
  }
  float x, y, degrees;
  const WmWhere where = {g_sr.finish[0], g_sr.finish[2], g_sr.dir[0],
                         g_sr.dir[2]};
  if (!wm_place(map, &where, &x, &y, &degrees))
    return;
  if (!ours) {
    uint32_t image = 0;
    for (size_t c = 0;
         c < sizeof k_sr_wm_icons / sizeof k_sr_wm_icons[0] && !image; ++c)
      for (int a = 15; a >= 0 && !image; --a) {
        char name[32];
        snprintf(name, sizeof name, k_sr_wm_icons[c], a);
        const uint32_t candidate = wm_find(pkg, fe_hash(name));
        int taken = 0;
        for (int u = 0; u < n_used && !taken; ++u)
          taken = used[u] == candidate;
        if (candidate && !taken)
          image = candidate;
      }
    uint32_t size = WM_ITEM_SIZE,
             item = image ? wm_call(WM_MALLOC, 0, &size, 1) : 0;
    float *buf = NULL;
    if (!item ||
        g_api->guest_ptr(g_api, g_wm_buf, 8, (void **)&buf) != POP_OK || !buf)
      return;
    buf[0] = x;
    buf[1] = y;
    const uint32_t args[6] = {WM_TYPE_PLAYER,   image, g_wm_buf,
                              float_bits(0.0f), 0,     0};
    wm_call(WM_ITEM_INIT, item, args, 6);
    mp_put(item, g_sr_wm_methods);
    mp_put(item + 0x30, float_bits(0.0f));
    mp_put(item + 0x34, 0);
    wm_link(map, item);
    ours = item;
    sr_log("world map: the finish shown (%.0f, %.0f)", x, y);
  }
  mp_put(ours + 0x10, float_bits(x));
  mp_put(ours + 0x14, float_bits(y));
  wm_paint(ours, get_u32(ours + 0xc), SR_WM_TINT);
}

/* The flash: held SR_HOLD_MS, once per flash. */
static int sr_flashed(double now) {
  const int on = (g_lt_state & LT_FLASH) != 0;
  if (!on) {
    g_sr_flash_from = 0;
    g_sr_flash_used = 0;
    return 0;
  }
  if (!(g_sr_flash_from > 0))
    g_sr_flash_from = now;
  if (g_sr_flash_used || now - g_sr_flash_from < SR_HOLD_MS)
    return 0;
  g_sr_flash_used = 1;
  return 1;
}

static void sr_service(const PopModApi *api, pop_cpu_v1 *cpu,
                       PopHookInvocation *inv, void *user) {
  (void)api;
  (void)cpu;
  (void)inv;
  (void)user;
  SrShared *sh = sr_shared();
  if (!sh)
    return;
  const double now = mp_now();
  uint32_t player = mp_player();
  const int roam = sr_free_roam(player);
  const uint32_t me = __atomic_load_n(&sh->me, __ATOMIC_ACQUIRE);

  /* what came */
  uint32_t tail = __atomic_load_n(&sh->rx_tail, __ATOMIC_RELAXED);
  const uint32_t head = __atomic_load_n(&sh->rx_head, __ATOMIC_ACQUIRE);
  if (head - tail > SR_RX)
    tail = head - SR_RX;
  for (; tail != head; ++tail) {
    SrRec r = sh->rx[tail % SR_RX];
    r.name[15] = 0;
    if (r.id && r.k >= SR_CHALLENGE && r.k <= SR_BUSY)
      sr_receive(&r, player, now);
  }
  __atomic_store_n(&sh->rx_tail, tail, __ATOMIC_RELEASE);

  /* the link */
  if (me)
    g_sr_link_lost = 0;
  else if (!(g_sr_link_lost > 0))
    g_sr_link_lost = now;

  const int flash = sr_flashed(now);
  switch (g_sr.phase) {
  case SR_IDLE:
    if (!flash || !roam || !me)
      break;
    {
      const int slot = sr_ahead(player);
      if (slot < 0)
        break;
      const uint32_t id = g_mp_remote[slot].id;
      char name[16];
      sr_name_of(sh, id, name);
      if (g_pu_police) {
        sr_say(&k_sr_info, "The police don't street race");
        break;
      }
      if (pu_is_police(pu_friend(id, 0), now)) {
        sr_say(&k_sr_info, "%s is the police: no race", name);
        break;
      }
      g_sr.opp = id;
      {
        static uint32_t made;
        const uint64_t t = (uint64_t)now;
        g_sr.race = ((uint32_t)(t ^ (t >> 32)) * 2654435761u) ^
                    (++made * 0x9e3779b9u) ^ (me << 16) ^ id;
      }
      if (!g_sr.race)
        g_sr.race = 1;
      g_sr.host = 1;
      memcpy(g_sr.name, name, sizeof g_sr.name);
      sr_send(id, SR_CHALLENGE, 0, 0, 0, 0);
      sr_phase(SR_ASKING, now);
      sr_say(&k_sr_info, "Race challenge sent to %s", name);
    }
    break;
  case SR_ASKING:
    if (now - g_sr.at > SR_ANSWER_MS + 2000.0) {
      sr_say_now(&k_sr_win, "No answer from %s", g_sr.name);
      sr_end(now);
    } else if (!roam || g_pu_police) {
      sr_send(g_sr.opp, SR_QUIT, SR_WHY_QUIT, 0, 0, 0);
      sr_end(now);
    }
    break;
  case SR_ASKED:
    if (now - g_sr.at > SR_ANSWER_MS || !roam || g_pu_police) {
      sr_log("challenge from %u not answered", g_sr.opp);
      sr_end(now); /* no race; nothing to say */
    } else if (flash) {
      sr_send(g_sr.opp, SR_ACCEPT, 0, 0, 0, 0);
      sr_phase(SR_ACCEPTED, now);
      sr_unsay();
      sr_say(&k_sr_info, "Race on! Get ready");
    }
    break;
  case SR_ACCEPTED:
    if (now - g_sr.at > SR_START_WAIT_MS) {
      sr_say_now(&k_sr_win, "No start from %s: race off", g_sr.name);
      sr_end(now);
    } else if (!roam || g_pu_police) {
      sr_send(g_sr.opp, SR_QUIT, SR_WHY_QUIT, 0, 0, 0);
      sr_end(now);
    }
    break;
  default: /* COUNTDOWN, RACING */
  {
    /* A local link outage cannot establish an opponent's withdrawal. */
    if (!me) {
      if (g_sr_link_lost > 0 && now - g_sr_link_lost > SR_LOST_MS) {
        sr_say_now(&k_sr_win, "Lost the link: race off");
        sr_end(now);
      }
      break;
    }
    if (now - g_sr.heard > SR_LOST_MS && !g_sr.result) {
      sr_result(1, "the other went silent");
      sr_say_now(&k_sr_win, "%s left the race: you win!", g_sr.name);
      sr_end(now);
      break;
    }
    if (now - g_sr.alive_at > SR_ALIVE_MS) {
      g_sr.alive_at = now;
      sr_send(g_sr.opp, SR_ALIVE, 0, 0, 0, 0);
      if (g_sr.mine >= 0)
        sr_send(g_sr.opp, SR_FINISH, 0, (float)g_sr.mine, 0, 0);
    }
  }
    /* this player quitting: no car for a while, an event, police, busted */
    if (!player) {
      if (!(g_sr.gone_since > 0))
        g_sr.gone_since = now;
    } else {
      g_sr.gone_since = 0;
    }
    if (g_sr.mine < 0) {
      if (player && sr_busted(player)) {
        sr_forfeit(SR_WHY_BUSTED, now);
        break;
      }
      if (g_pu_police) {
        sr_forfeit(SR_WHY_POLICE, now);
        break;
      }
      if ((g_sr.gone_since > 0 && now - g_sr.gone_since > SR_GONE_MS) ||
          (player && !roam)) {
        sr_forfeit(SR_WHY_QUIT, now);
        break;
      }
    }
    if (g_sr.phase == SR_COUNTDOWN) {
      const double left = g_sr.go - now;
      const int step = left > 3000.0   ? 4
                       : left > 2000.0 ? 3
                       : left > 1000.0 ? 2
                       : left > 0.0    ? 1
                                       : 0;
      if (step < g_sr.shown) {
        g_sr.shown = step;
        if (step == 4) {
          float p[3];
          const float d =
              player && sr_where(player + 0x24, p, NULL)
                  ? sqrtf((g_sr.finish[0] - p[0]) * (g_sr.finish[0] - p[0]) +
                          (g_sr.finish[2] - p[2]) * (g_sr.finish[2] - p[2]))
                  : 0.0f;
          sr_say(&k_sr_info, "Race %s to the GPS finish, %.1f km", g_sr.name,
                 d / 1000.0f);
        } else if (step) {
          sr_say_now(&k_sr_count, "%d", step);
        } else {
          sr_say_now(&k_sr_go, "GO!");
        }
      }
      if (step == 0) {
        sr_phase(SR_RACING, now);
      }
      break;
    }
    if (g_sr.phase == SR_RACING && g_sr.mine < 0 && player) {
      float p[3];
      if (sr_where(player + 0x24, p, NULL)) {
        const float dx = g_sr.finish[0] - p[0], dz = g_sr.finish[2] - p[2];
        if (dx * dx + dz * dz < SR_RADIUS * SR_RADIUS &&
            fabsf(g_sr.finish[1] - p[1]) < SR_RADIUS_Y) {
          g_sr.mine = (float)(rint(sr_clock(now) * 1000.0) /
                              1000.0); /* as the other game gets it */
          g_sr.crossed_at = now;
          sr_gps_off();
          sr_log("crossed the finish in %.3f s", g_sr.mine);
          sr_send(g_sr.opp, SR_FINISH, 0, (float)g_sr.mine, 0, 0);
          if (g_sr.theirs < 0)
            sr_say(&k_sr_info, "Finished! Waiting for %s", g_sr.name);
          if (g_sr.theirs >= 0) {
            /* their time came first (we were behind it, or the clocks differ a
             * little) */
            if (!g_sr.result)
              sr_by_times();
          }
        }
      }
    }
    /* Wait for both times: network delay must not create two different winners.
     */
    if (!g_sr.result && g_sr.mine >= 0 && g_sr.theirs >= 0)
      sr_by_times();
    if (g_sr.result && g_sr.gap_said) {
      sr_end(now);
      break;
    }
    if (!g_sr.result && g_sr.mine >= 0 && now - g_sr.crossed_at > SR_TAIL_MS) {
      sr_result(1, "opponent did not finish");
      sr_say_now(&k_sr_win, "You win: %s did not finish", g_sr.name);
      sr_end(now);
      break;
    }
    if (!g_sr.result && g_sr.theirs >= 0 && g_sr.mine < 0 &&
        sr_clock(now) - g_sr.theirs > SR_TAIL_MS / 1000.0) {
      sr_result(2, "this player did not finish");
      sr_say_now(&k_sr_win, "%s wins: you did not finish", g_sr.name);
      sr_send(g_sr.opp, SR_QUIT, SR_WHY_QUIT, 0, 0, 0);
      sr_end(now);
      break;
    }
    if (sr_clock(now) > SR_MAX_MS / 1000.0 && !g_sr.result) {
      sr_say_now(&k_sr_win, "Race called off");
      sr_send(g_sr.opp, SR_QUIT, SR_WHY_OFF, 0, 0, 0);
      sr_end(now);
    }
    break;
  }
  sr_say_waiting(now);
  __atomic_store_n(
      &sh->opp, g_sr.phase != SR_IDLE && g_sr.opp != 0xffffffffu ? g_sr.opp : 0,
      __ATOMIC_RELEASE);
  __atomic_store_n(&sh->phase, (uint32_t)g_sr.phase, __ATOMIC_RELEASE);
}

static void streetrace_install(void) {
  if (!g_mp_block || !g_lt_block || !g_api->guest_call ||
      g_api->guest_alloc(g_api, SR_TEXTS * SR_TEXT, &g_sr_text) != POP_OK ||
      g_api->guest_alloc(g_api, 16, &g_sr_vec) != POP_OK ||
      g_api->guest_alloc(g_api, 0x24, &g_sr_msg) != POP_OK ||
      g_api->guest_alloc(g_api, 0x14, &g_sr_kill) != POP_OK ||
      g_api->guest_alloc(g_api, sizeof(SrShared), &g_sr_block) != POP_OK) {
    log_line("core.nfsc: street races unavailable (no guest memory, lights or "
             "calls)");
    return;
  }
  SrShared *sh = sr_shared();
  if (!sh) {
    g_sr_block = 0;
    return;
  }
  memset(sh, 0, sizeof *sh);
  sh->magic = SR_MAGIC;
  sh->version = 1;
  if (g_api->hook_install(g_api, MP_FE_SERVICE, sr_service, POP_HOOK_BEFORE,
                          NULL, &g_sr_hooks[0]) != POP_OK) {
    log_line("core.nfsc: street races unavailable (hook)");
    return;
  }
  if (g_api->hook_install(g_api, 0x005e1af0u, sr_minimap, POP_HOOK_BEFORE, NULL,
                          &g_sr_hooks[1]) != POP_OK)
    sr_log("no finish on the minimap (hook)");
  if (g_wm_block &&
      g_api->guest_alloc(g_api, 7 * 4, &g_sr_wm_methods) == POP_OK) {
    for (uint32_t i = 0; i < 7; ++i)
      mp_put(g_sr_wm_methods + i * 4, get_u32(WM_ITEM_METHODS + i * 4));
    if (g_api->hook_install(g_api, WM_UPDATE, sr_worldmap, POP_HOOK_BEFORE,
                            NULL, &g_sr_hooks[2]) != POP_OK)
      sr_log("no finish on the world map (hook)");
  }
  sr_log("ready: flash a friend ahead (F or L3, held %.0f ms) to challenge",
         SR_HOLD_MS);
#ifdef __EMSCRIPTEN__
  MAIN_THREAD_ASYNC_EM_ASM(
      {
        var block = {};
        block.memory = wasmMemory;
        block.ptr = $0;
        globalThis.carbonStreetBlock = block;
        globalThis.dispatchEvent(new Event("carbon-street"));
      },
      (uint32_t)(uintptr_t)sh);
#endif
}
