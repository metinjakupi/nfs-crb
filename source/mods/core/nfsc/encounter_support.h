/* Shared HUD messages, car eligibility and compatibility mailbox for native
 * crew-rival encounters. The retired flash/GPS race has no trigger or state
 * machine here. Legacy challenges receive BUSY; other old race records drop. */
#define SR_MAGIC 0x31525453u
#define SR_CHALLENGE 1u
#define SR_BUSY 7u
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

static uint32_t g_sr_block, g_sr_text, g_sr_texts, g_sr_hook;
static SrShared *sr_shared(void) {
  void *p = NULL;
  return g_sr_block && g_api->guest_ptr(g_api, g_sr_block, sizeof(SrShared),
                                        &p) == POP_OK
             ? p
             : NULL;
}

static void sr_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void sr_log(const char *fmt, ...) {
  char line[256];
  const int used=snprintf(line,sizeof line,"core.nfsc: crew: ");
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line + used, sizeof line - (size_t)used, fmt, ap);
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

static void sr_send_race(uint32_t to,uint32_t race,uint32_t kind,uint32_t reason) {
    SrShared *sh=sr_shared();if(!sh||!to||to==0xffffffffu)return;
    const uint32_t head=__atomic_load_n(&sh->tx_head,__ATOMIC_RELAXED);
    if(head-__atomic_load_n(&sh->tx_tail,__ATOMIC_ACQUIRE)>=SR_TX)return;
    SrRec *r=&sh->tx[head%SR_TX];memset(r,0,sizeof *r);
    r->id=to;r->r=race;r->k=kind;r->a=reason;
    __atomic_store_n(&sh->tx_head,head+1,__ATOMIC_RELEASE);
}
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


static void encounter_support_service(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
    (void)api;(void)cpu;(void)inv;(void)user;
    SrShared *sh=sr_shared();if(!sh)return;
    uint32_t tail=__atomic_load_n(&sh->rx_tail,__ATOMIC_RELAXED);
    const uint32_t head=__atomic_load_n(&sh->rx_head,__ATOMIC_ACQUIRE);
    if(head-tail>SR_RX)tail=head-SR_RX;
    for(;tail!=head;++tail){const SrRec *r=&sh->rx[tail%SR_RX];if(r->id&&r->k==SR_CHALLENGE)sr_send_race(r->id,r->r,SR_BUSY,3);}
    __atomic_store_n(&sh->rx_tail,tail,__ATOMIC_RELEASE);
    __atomic_store_n(&sh->opp,0,__ATOMIC_RELEASE);
    __atomic_store_n(&sh->phase,0,__ATOMIC_RELEASE);
    sr_say_waiting(mp_now());
}
static void encounter_support_install(void) {
    if(!g_mp_block||!g_api->guest_call||
       g_api->guest_alloc(g_api,SR_TEXTS*SR_TEXT,&g_sr_text)!=POP_OK||
       g_api->guest_alloc(g_api,0x24,&g_sr_msg)!=POP_OK||
       g_api->guest_alloc(g_api,0x14,&g_sr_kill)!=POP_OK||
       g_api->guest_alloc(g_api,sizeof(SrShared),&g_sr_block)!=POP_OK)return;
    SrShared *sh=sr_shared();if(!sh){g_sr_block=0;return;}
    memset(sh,0,sizeof *sh);sh->magic=SR_MAGIC;sh->version=1;
    if(g_api->hook_install(g_api,MP_FE_SERVICE,encounter_support_service,POP_HOOK_BEFORE,NULL,&g_sr_hook)!=POP_OK)return;
#ifdef __EMSCRIPTEN__
    MAIN_THREAD_ASYNC_EM_ASM({var b={};b.memory=wasmMemory;b.ptr=$0;globalThis.carbonStreetBlock=b;globalThis.dispatchEvent(new Event("carbon-street"));},(uint32_t)(uintptr_t)sh);
#endif
}
