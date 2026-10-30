/* race.h - racing friends in Carbon's own Quick Races (included by nfsc.c after mp.h).
 *
 * The plan and every address below come from plans/race-together-research.md;
 * read it first. In short: the game keeps a table of up to 30 racers
 * (GRaceStatus *0x00a98284, rows of 0x384 bytes from +0x18, count at +0x6a08),
 * and its race list, minimap, places and results all read that table. Another
 * player's car becomes a racer the way the game's own online mode did it: a
 * class-7 car ("remote racer", the class mp.h already builds for free roam) is
 * added with AddSimablePlayer right after the race has set up its racers, so the
 * Lua race script counts it in NumRacers from the start.
 *
 * THE PAGE. web/race.js runs the lobby over the relay and shares a block of
 * guest memory with this file (RaceShared below, published as
 * globalThis.carbonRaceBlock {memory, ptr} with a "carbon-race" event, the way
 * mp.h publishes its block). The page's part says which race we are in
 * (session), who is in it (peers: relay id, car model, grid slot, name, and
 * their finish time once they have one), when to start the race (launch: the
 * host's race key and its 40-byte Quick Race options) and when to leave the
 * loading screen (go). The game's part says how far the game is (phase), the
 * Start the player pressed in the Quick Race menu while hosting (caught_*), the
 * car this player will race, and the racer table as the game sees it. Each side
 * guards its part with a sequence number, odd while it writes (as in mp.h).
 *
 * WHAT HAPPENS, IN ORDER.
 * 1. Host: the Quick Race Start button calls 007bbed0(race key, 7). While the
 *    page hosts (flags RACE_CATCH) the call is caught instead, with the options
 *    block of that race type (profile + 0x2b0c4 + type * 0x28), and the page
 *    opens the lobby. Everyone in the race, the host too, then gets the same
 *    launch from the page: the block is written into this game's profile and
 *    007bbed0 is called from the front end (flow 0x00a99bbc == 3).
 * 2. InitRacers 00668a10 (AFTER): each other player's car is built at their grid
 *    slot (0063d900) and added with 00624f80; their name goes into the row
 *    (+0x08, at most 15 characters show). Zero AI unless kept: GetNumOpponents
 *    0063c660 answers 0 inside InitRacers, and the crew's wingman (0065de20) is
 *    not added. Other players' cars belong to this file for the whole race:
 *    mp.h leaves those players alone (g_mp_racer) so no second car is built.
 * 3. Loading: GRaceStatus::Update and NotifyScriptWhenLoaded ask 00625b60
 *    whether a racer's car is still loading. Once it says no, this player is
 *    loaded (phase RACE_HOLDING) and the answer stays "still loading" until the
 *    page says go, so everyone leaves the loading screen together and the 3-2-1
 *    countdown (a real-time clock) ends together. Meanwhile this player's car
 *    is put on its own grid slot, keeping the game's heading.
 * 4. Racing: other players' cars follow their poses as mp.h's do (the same
 *    playback, from mp.h's copy of the page's slots). The game does not work
 *    out a class-7 car's progress or count its laps (tested: see race_progress),
 *    so each player's own progress comes over the relay and is written into
 *    their row; places then come from the game's own ranking. Each player's own
 *    finish time (its row's race timer, +0x184, sim seconds) goes to the others,
 *    who write it into that player's row and re-rank (006449d0, 00640d50), so
 *    every results screen has the same order. When this player finishes first,
 *    the game would give the others a projected time (00644bc0); for other
 *    players it does not.
 * 5. Minimap: the game draws racer dots for vehicle list 3 (AI racers) and, only
 *    online, list 7 (remote cars); 005d8d70 is called for list 7 as well while
 *    other players race.
 *
 * TESTS (in the address). RECOMP_RACE_TEST=1, no network needed: a copy of the
 * player's car becomes the racer "TESTER", follows the first AI racer 0.6 s
 * behind along its path with that AI's progress as its own, finishes 0.6 s after
 * it (the way another player's time arrives), and the loading screen holds for
 * 8 s; =2 is the same with zero AI (the test car then stays on its grid slot).
 * Every 2 s the racer table goes to the log. RECOMP_RACE_PILOT=<ms>: in any race
 * this player's own car drives the first AI racer's path that many ms behind it
 * (a race together needs AI kept for that), so a headless test can finish.
 */

#include <stdarg.h>

#define RACE_INIT_RACERS 0x00668a10u    /* Lua Game.InitRacers, cdecl, plain RET */
#define RACE_INIT_END 0x00669a49u       /* end of InitRacers, for return addresses */
#define RACE_ADD_SIMABLE 0x00624f80u    /* thiscall (GRaceStatus *, ISimable *), RET 4 -> row */
#define RACE_GRID 0x0063d900u           /* cdecl (slot, float pos[3], float dir[3], 0), AL */
#define RACE_CARS_LOADED 0x00625b60u    /* thiscall GRaceStatus: AL 1 = a racer's car is still loading */
#define RACE_POLL_RET 0x006473dau       /* its call in GRaceStatus::Update returns here */
#define RACE_NOTIFY_RET 0x0062d95au     /* and its call in NotifyScriptWhenLoaded */
#define RACE_NUM_OPPONENTS 0x0063c660u  /* thiscall GRaceParameters, plain RET */
#define RACE_WINGMAN 0x0065de20u        /* cdecl (which, ...): adds a crew member as a racer */
#define RACE_WINGMAN_RET0 0x00668e39u   /* its two calls in InitRacers */
#define RACE_WINGMAN_RET1 0x00668e4du
#define RACE_MINIMAP_RACERS 0x005d8d70u /* thiscall (minimap, vehicle list, IVehicle *), RET 8 */
#define RACE_START 0x007bbed0u          /* cdecl (race key, mode 7 = Quick Race), plain RET */
#define RACE_RACER_UPDATE 0x00644150u  /* GRacerInfo::Update, thiscall (dt), RET 4 */
#define RACE_PROJECT 0x00644bc0u        /* fastcall (GRacerInfo *): a finish time from progress */
#define RACE_FINISH 0x006449d0u         /* fastcall (GRacerInfo *): FinishRace */
#define RACE_RANK 0x00640d50u           /* fastcall (GRaceStatus *): places from progress and times */
#define RACE_DAL_INT 0x004a0490u        /* thiscall (0x00a8ad30: id, int *out, a, b, c) */
#define RACE_DAL 0x00a8ad30u
#define RACE_PROFILE 0x00572b90u        /* thiscall ([0x00a97a7c], 0) -> profile */
#define RACE_FE_DB 0x00a97a7cu
#define RACE_FIND_CAR 0x0049c020u       /* thiscall (profile + 0x234, handle) -> car record */
#define RACE_FLOW 0x00a99bbcu           /* game flow: 3 = front end */
#define RACE_SIM_CLOCK 0x00b4cfd8u      /* float: the sim clock the race timers run on, s */
#define RACE_QR_BLOCKS 0x2b0c4u         /* the profile's Quick Race options, 0x28 per race type */
#define RACE_ROW_SIZE 0x384u
#define RACE_MAX_ROWS 30u
#define RACE_PEERS 8
#define RACE_ROWS 8
#define RACE_HOLD_MAX 120.0 /* s: the loading screen lets go by itself after this */
#define RACE_FOLLOW 360     /* test car and pilot: frames of the AI's path kept (6 s) */

/* flags (page) */
#define RACE_CATCH 1u   /* catch the Quick Race Start: the page hosts */
#define RACE_KEEP_AI 2u /* keep the race's AI opponents */
/* phase (game) */
#define RACE_IDLE 0u
#define RACE_LAUNCHED 1u /* 007bbed0 called for the session; the race is loading */
#define RACE_SET_UP 2u   /* InitRacers added the other players */
#define RACE_HOLDING 3u  /* every racer's car is loaded; waiting for go */
#define RACE_RACING 4u   /* released */
#define RACE_DONE 5u     /* this player has finished */

typedef struct RacePeer { /* 0x40 bytes; written by the page */
    uint32_t id;    /* relay id; 0 = empty */
    uint32_t key;   /* the car model they race (Attrib key); 0 = a copy of ours */
    uint32_t grid;  /* grid slot, 0 = front */
    uint32_t done;  /* 1 once their own finish time has arrived */
    float time;     /* that time, race seconds */
    /* Their progress as their own game works it out (GRacerInfo +0x3c, +0x48,
     * +0x4c, +0x50), at their physics clock ms, and how fast % race and % lap
     * grew over the last two reports (per second). */
    float pct, lap_pct;
    uint32_t laps, cp, ms;
    float rate, lap_rate;
    char name[16];  /* as the race list shows it */
} RacePeer;

typedef struct RaceRow { /* 0x30 bytes; written by the game: one row of its racer table */
    uint32_t id;    /* relay id of a player, 0 for AI */
    uint32_t place, laps, done;
    float pct, time;
    uint32_t me, spare;
    char name[16];
} RaceRow;

typedef struct RacePage { /* the page's part */
    uint32_t session;  /* the race we are in; 0 = none */
    uint32_t flags;    /* RACE_CATCH, RACE_KEEP_AI */
    uint32_t me;       /* our relay id */
    uint32_t grid;     /* our grid slot */
    uint32_t count;    /* peers in use (others only) */
    uint32_t go;       /* == session: leave the loading screen */
    uint32_t launch;   /* == session: start the race */
    uint32_t launch_key;
    uint8_t launch_opts[40];
    uint32_t spare[3];
    RacePeer peers[RACE_PEERS];
} RacePage;

typedef struct RaceShared {
    uint32_t magic, version;
    uint32_t page_seq;
    RacePage page;
    uint32_t game_seq;
    uint32_t phase;
    uint32_t car;       /* the car model this player races: the menu's choice, then the car */
    uint32_t caught;    /* counts Quick Race Starts caught while hosting */
    uint32_t caught_key, caught_type;
    uint8_t caught_opts[40];
    uint32_t place, laps, done;
    float pct, time;
    uint32_t rows, loaded, session; /* session: the one the game part describes */
    float lap_pct;
    uint32_t cp, ms; /* checkpoints this lap; the physics clock (mp_ms) of these values */
    uint32_t flow_flags; /* low byte: game flow; 0x100: waiting for measured finishes */
    RaceRow row[RACE_ROWS];
} RaceShared;
#define RACE_MAGIC 0x31435243u /* "CRC1" */
/* The page reads these by offset (web/race.js). */
_Static_assert(sizeof(RacePeer) == 0x40 && offsetof(RaceShared, page) == 0x0c &&
                   offsetof(RaceShared, page.peers) == 0x60 && offsetof(RaceShared, game_seq) == 0x260 &&
                   offsetof(RaceShared, caught_opts) == 0x278 && offsetof(RaceShared, place) == 0x2a0 &&
                   offsetof(RaceShared, lap_pct) == 0x2c0 && offsetof(RaceShared, row) == 0x2d0 &&
                   sizeof(RaceShared) == 0x450,
               "RaceShared layout");

typedef struct RaceCar {
    uint32_t id;    /* relay id; 0 = the test car */
    uint32_t row;   /* its row in the racer table */
    uint32_t key;
    int following;  /* follows the network (after go) */
    MpCar car;      /* the car and its playback, as mp.h keeps them */
} RaceCar;

static uint32_t g_race_hooks[12];
static uint32_t g_race_block, g_race_buf;
static int g_race_encounter, g_race_aborting;
static int (*g_race_page_source)(RacePage *);
static void (*g_race_finish_observer)(uint32_t,float);
static RacePage g_race_page;           /* the last whole copy of the page's part */
static RaceCar g_race_cars[RACE_PEERS + 1];
static int g_race_cars_n;
static uint32_t g_race_session;        /* the session whose race this game launched */
static uint32_t g_race_launched;       /* the last session launched (never launched twice) */
static uint32_t g_race_phase;
static int g_race_replaying;           /* inside our own call of 007bbed0 */
static int g_race_no_ai;               /* this race (and its restarts): no AI, no wingman */
static double g_race_loaded_at;        /* when our racers were first all loaded */
static uint32_t g_race_frame, g_race_car_key, g_race_base;
static int g_race_test;
static int g_race_pilot; /* ms behind the AI that the player's own car drives (tests); 0 = off */
static uint32_t g_race_finish_sent;
/* The script counts finishes seen locally, which can disagree with network
 * racers' measured times. Defer final placement until those times arrive. */
static uint32_t g_race_result_activity, g_race_result_simable;
static int g_race_result_replaying;
/* test car: the followed AI's path */
static MpPose g_race_path[RACE_FOLLOW];
static double g_race_path_at[RACE_FOLLOW];
static int g_race_path_n;
static uint32_t g_race_follow_ai, g_race_follow_writes; /* test: the AI followed, poses written */
static float g_race_path_row[RACE_FOLLOW][4]; /* test: the AI's progress with each pose */
static float g_race_test_row[4];               /* test: the progress given to the test car */
static int g_race_test_row_ok;

/* mp.h asks this for each player: 1 = their car is (or is about to be) one of
 * our racers. From the moment our race launches, every player in it counts:
 * someone who loaded first is already driving on the grid, and mp.h would build
 * a free-roam car for them before InitRacers builds the racer (seen: mp.h's car,
 * then race.h's 3 m from it, then mp.h dropping its car, and the racer's car
 * gone with it). */
static int race_owns(uint32_t id) {
    if (!id)
        return 0;
    for (int i = 0; i < g_race_cars_n; ++i)
        if (g_race_cars[i].id == id)
            return 1;
    if (g_race_session && g_race_session == g_race_page.session && g_race_phase >= RACE_LAUNCHED)
        for (uint32_t k = 0; k < g_race_page.count && k < RACE_PEERS; ++k)
            if (g_race_page.peers[k].id == id)
                return 1;
    return 0;
}

static void race_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void race_log(const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    log_line(line);
}

static uint8_t race_u8(uint32_t addr) {
    uint8_t v = 0;
    g_api->guest_read_u8(g_api, addr, &v);
    return v;
}

static RaceShared *race_shared(void) {
    void *p = NULL;
    return g_race_block && g_api->guest_ptr(g_api, g_race_block, sizeof(RaceShared), &p) == POP_OK ? p : NULL;
}

static uint32_t race_status(void) { return get_u32(MP_RACE_STATUS); }
static uint32_t race_row(uint32_t rs, uint32_t i) { return rs + 0x18u + i * RACE_ROW_SIZE; }
static uint32_t race_count(uint32_t rs) {
    const uint32_t n = rs ? get_u32(rs + 0x6a08) : 0;
    return n > RACE_MAX_ROWS ? 0 : n;
}

/* Whether a car we built still exists: its handle is in the game's sorted
 * table of simables ({handle, ISimable *} at [0x00aa26b4], count [0x00aa26bc];
 * the lookup 0060cba0 that the racer code uses). mp_listed walks only the
 * first 64 cars of the car list, and a race world with traffic has more, so a
 * racer's car could look gone while it was still there. */
static int race_alive(uint32_t simable) {
    const uint32_t table = get_u32(0x00aa26b4u), handle = simable ? get_u32(simable + 8) : 0;
    uint32_t lo = 0, hi = get_u32(0x00aa26bcu);
    if (!table || !handle || hi > 0x100000u)
        return 0;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2, h = get_u32(table + 8u * mid);
        if (h == handle)
            return get_u32(table + 8u * mid + 4) == simable;
        if (h < handle)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

/* The car (PVehicle) whose ISimable has this handle, or 0. */
static uint32_t race_car_of(uint32_t handle) {
    uint32_t node = get_u32(MP_CARS);
    for (int n = 0; handle && node && node != MP_CARS && n < 64; ++n, node = get_u32(node))
        if (get_u32(node - 0xd0 + 0x24 + 8) == handle)
            return node - 0xd0;
    return 0;
}

/* The row index of the car with this ISimable, or -1. */
static int race_row_of(uint32_t rs, uint32_t simable) {
    const uint32_t handle = simable ? get_u32(simable + 8) : 0, n = race_count(rs);
    for (uint32_t i = 0; handle && i < n; ++i)
        if (get_u32(race_row(rs, i)) == handle)
            return (int)i;
    return -1;
}

static RaceCar *race_car_by_row(uint32_t row_addr) {
    const uint32_t rs = race_status();
    for (int i = 0; rs && i < g_race_cars_n; ++i)
        if (race_row(rs, g_race_cars[i].row) == row_addr)
            return &g_race_cars[i];
    return NULL;
}

/* Grid slot `slot` of the race being set up: position and heading. */
static int race_grid(uint32_t slot, float pos[3], float dir[3]) {
    const uint32_t b = g_race_buf + 0x80;
    for (uint32_t i = 0; i < 0x18; i += 4)
        mp_put(b + i, 0);
    if (g_race_encounter && g_race_session && g_race_session == g_race_page.session) {
        const uint32_t args[4] = {slot == g_race_page.grid ? 0u : 1u, b, b + 0x0c, 0};
        if (!(mp_call(0x00617720u, 0, args, 4) & 0xffu)) return 0;
        for (int i=0;i<3;i++) {pos[i]=bits_float(get_u32(b+4u*i));dir[i]=bits_float(get_u32(b+12+4u*i));}
        return isfinite(pos[0]) && isfinite(pos[2]);
    }
    const uint32_t args[4] = {slot, b, b + 0x0c, 0};
    if (!(mp_call(RACE_GRID, 0, args, 4) & 0xffu))
        return 0;
    for (int i = 0; i < 3; ++i) {
        pos[i] = bits_float(get_u32(b + 4u * i));
        dir[i] = bits_float(get_u32(b + 0x0c + 4u * i));
    }
    return isfinite(pos[0]) && isfinite(pos[2]);
}

/* mp_build, but facing `dir`: a class-7 car of model `key` (0 = a copy of the
 * player's) at pos, in the look at guest address `look` (0 = stock; look.h).
 * Returns its ISimable or 0. */
static uint32_t race_build(uint32_t player, uint32_t key, const float pos[3], const float dir[3], uint32_t flags,
                           uint32_t look) {
    const uint32_t b = g_race_buf;
    for (uint32_t i = 0; i < 0x60; i += 4)
        mp_put(b + i, 0);
    uint32_t arg = b;
    mp_call(MP_PARAMS_TYPE, 0, &arg, 1);
    mp_put(b + 0x04, b);
    mp_put(b + 0x10, MP_REMOTE);
    if (key) {
        const uint32_t args[2] = {key, 0};
        mp_call(MP_ATTRIBUTES, b + 0x14, args, 2);
    } else {
        mp_copy(b + 0x14, player + 0xd8, 0x10);
    }
    for (int i = 0; i < 3; ++i) {
        put_float(b + 0x60 + 4u * i, dir[i]);
        put_float(b + 0x70 + 4u * i, pos[i]);
    }
    mp_put(b + 0x24, b + 0x60);
    mp_put(b + 0x28, b + 0x70);
    mp_put(b + 0x2c, look);
    mp_put(b + 0x30, get_u32(MP_RACE_STATUS));
    mp_put(b + 0x38, flags);
    const uint32_t args[2] = {get_u32(b), b};
    uint32_t car = mp_call(MP_CONSTRUCT, 0, args, 2);
    if (car && !race_alive(car)) {
        race_log("core.nfsc: race: car %08x is not among the game's simables", car);
        car = 0;
    }
    if (car)
        mp_enable_contacts(car);
    return car;
}

/* The name in a racer row: up to 15 characters the HUD font has. */
static void race_name(uint32_t row, const char *name, size_t max) {
    char clean[16];
    size_t n = 0;
    for (size_t i = 0; i < max && name[i] && n < 15; ++i) {
        const char ch = name[i];
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == ' ' ||
            ch == '.' || ch == '-')
            clean[n++] = ch;
    }
    if (!n) {
        memcpy(clean, "FRIEND", 6);
        n = 6;
    }
    for (uint32_t i = 0; i < 32; ++i)
        g_api->guest_write_u8(g_api, row + 8 + i, i < n ? (uint8_t)clean[i] : 0);
}

/* The free-roam slot a player's poses arrive in (look.h keys its bookkeeping by it), or -1. */
static int race_mp_slot(uint32_t id) {
    for (int i = 0; i < MP_SLOTS; ++i)
        if (g_mp_seen_slot[i].id == id)
            return i;
    return -1;
}

/* Builds one other player's car on its grid slot and makes it a racer. */
static RaceCar *race_add(uint32_t rs, uint32_t player, uint32_t id, uint32_t key, uint32_t slot,
                         const char *name, size_t name_max) {
    if (g_race_cars_n >= (int)(sizeof g_race_cars / sizeof g_race_cars[0]) || race_count(rs) >= RACE_MAX_ROWS)
        return NULL;
    float pos[3], dir[3];
    if (!race_grid(slot, pos, dir)) {
        race_log("core.nfsc: race: no grid slot %u", slot);
        return NULL;
    }
    /* In its owner's look, as in free roam (look.h); the copy of ours stays stock. */
    const uint32_t look = key && g_mp_look ? g_mp_look(race_mp_slot(id), id, key) : 0;
    uint32_t sim = race_build(player, key, pos, dir, 2u | 8u, look);
    if (!sim)
        sim = race_build(player, key, pos, dir, 1u | 2u | 8u, look); /* a model not loaded yet: spooled */
    if (!sim && key)
        sim = race_build(player, 0, pos, dir, 2u | 8u, 0); /* their model will not load: a copy of ours */
    if (!sim) {
        race_log("core.nfsc: race: the car of player %u (model %08x) was refused", id, key);
        return NULL;
    }
    const uint32_t row = mp_call(RACE_ADD_SIMABLE, rs, &sim, 1);
    if (!row) {
        mp_kill(sim);
        return NULL;
    }
    race_name(row, name, name_max);
    RaceCar *c = &g_race_cars[g_race_cars_n++];
    memset(c, 0, sizeof *c);
    c->id = id;
    c->key = key;
    c->row = (row - rs - 0x18u) / RACE_ROW_SIZE;
    c->car.simable = sim;
    c->car.id = id;
    c->car.key = key;
    c->car.kind = MP_REMOTE;
    race_log("core.nfsc: race: player %u (%.15s) is racer %u, model %08x, grid slot %u at %.1f %.1f %.1f",
             id, name, c->row, key, slot, pos[0], pos[1], pos[2]);
    return c;
}

/* Forgets the race's cars once the world they were in is gone. */
static void race_forget(const char *why) {
    if (!g_race_cars_n && !g_race_session)
        return;
    for (int i = 0; i < g_race_cars_n; ++i)
        if (g_race_cars[i].car.simable && race_alive(g_race_cars[i].car.simable))
            mp_kill(g_race_cars[i].car.simable);
    if (g_race_cars_n || g_race_phase)
        race_log("core.nfsc: race: over (%s)", why);
    g_race_cars_n = 0;
    g_race_session = 0;
    g_race_phase = RACE_IDLE;
    g_race_encounter = g_race_aborting = 0;
    g_race_loaded_at = 0;
    g_race_finish_sent = 0;
    g_race_result_activity = g_race_result_simable = 0;
    g_race_path_n = 0;
    g_race_no_ai = 0; /* kept until now: a restart runs InitRacers again */
}

/* InitRacers has set up the players, the crew and the AI: the other players join. */
static void race_init_racers(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)cpu;
    (void)inv;
    (void)user;
    const uint32_t rs = race_status(), player = mp_player();
    if (!rs || !player)
        return;
    g_race_result_activity = g_race_result_simable = 0;
    if (g_race_cars_n) {
        /* A restart runs InitRacers again with an empty table: the same cars
         * become racers again. */
        for (int i = 0; i < g_race_cars_n; ++i) {
            RaceCar *c = &g_race_cars[i];
            if (!c->car.simable || !race_alive(c->car.simable))
                continue;
            const uint32_t row = mp_call(RACE_ADD_SIMABLE, rs, &c->car.simable, 1);
            if (row)
                c->row = (row - rs - 0x18u) / RACE_ROW_SIZE;
        }
        race_log("core.nfsc: race: restarted with %d other players", g_race_cars_n);
        return;
    }
    const RacePage *p = &g_race_page;
    const int ours = g_race_session && g_race_session == p->session;
    if (!ours && !g_race_test)
        return;
    const uint32_t base = (p->flags & RACE_KEEP_AI) && ours ? race_count(rs) : 0;
    g_race_base = base;
    if (g_race_test) {
        float pos[3], dir[3];
        const uint32_t ppos = mp_state(get_u32(player + 0x70));
        for (uint32_t s = 0; s < 10; ++s)
            if (race_grid(s, pos, dir))
                race_log("core.nfsc: race test: grid slot %u at %.1f %.1f %.1f facing %.2f %.2f %.2f", s, pos[0],
                         pos[1], pos[2], dir[0], dir[1], dir[2]);
        if (ppos)
            race_log("core.nfsc: race test: player at %.1f %.1f %.1f, model %08x, %u racers so far",
                     bits_float(get_u32(ppos + 0x10)), bits_float(get_u32(ppos + 0x14)),
                     bits_float(get_u32(ppos + 0x18)), mp_model_key(player), race_count(rs));
        race_add(rs, player, 0, 0, race_count(rs) + 1, "TESTER", 6);
        g_race_phase = RACE_SET_UP;
        return;
    }
    race_log("core.nfsc: race: this player races model %08x (announced %08x)", mp_model_key(player), g_race_car_key);
    for (uint32_t i = 0; i < p->count && i < RACE_PEERS; ++i) {
        const RacePeer *peer = &p->peers[i];
        if (peer->id && peer->id != p->me)
            race_add(rs, player, peer->id, peer->key, base + peer->grid, peer->name, sizeof peer->name);
    }
    g_race_phase = RACE_SET_UP;
}

/* No AI racers for a race together, unless the host kept them. */
static void race_opponents(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)inv;
    (void)user;
    if (g_race_no_ai && cpu->eip > RACE_INIT_RACERS && cpu->eip < RACE_INIT_END)
        cpu->eax = 0;
}

static void race_wingman(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t from = get_u32(cpu->esp);
    if ((g_race_no_ai || (g_race_session && g_race_session == g_race_page.session)) &&
        (from == RACE_WINGMAN_RET0 || from == RACE_WINGMAN_RET1)) {
        api->hook_return(api, cpu, 0, 0);
        return;
    }
    api->call_original(api, cpu->target, cpu);
}

/* Whether the loading screen waits for the others. */
static int race_holding(void) {
    if (g_race_phase < RACE_SET_UP || g_race_phase >= RACE_RACING)
        return 0;
    if(g_race_encounter)return !g_race_aborting && g_race_page.go != g_race_session;
    if (g_race_loaded_at > 0 && mp_now() - g_race_loaded_at > RACE_HOLD_MAX * 1000.0)
        return 0;
    if (g_race_test)
        return !(g_race_loaded_at > 0 && mp_now() - g_race_loaded_at > 8000.0);
    return g_race_page.go != g_race_session;
}

/* AFTER 00625b60: "still loading" while we hold. */
static void race_cars_loaded(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)inv;
    (void)user;
    if (cpu->eip != RACE_POLL_RET && cpu->eip != RACE_NOTIFY_RET)
        return;
    if (g_race_phase < RACE_SET_UP || g_race_phase >= RACE_RACING || (cpu->eax & 0xffu))
        return;
    if (!(g_race_loaded_at > 0)) {
        g_race_loaded_at = mp_now();
        g_race_phase = RACE_HOLDING;
        race_log("core.nfsc: race: every racer's car is loaded; %s", race_holding() ? "holding" : "going");
    }
    if (race_holding()) {
        cpu->eax = (cpu->eax & ~0xffu) | 1u;
        return;
    }
    g_race_phase = RACE_RACING;
    race_log("core.nfsc: race: leaving the loading screen after %.1f s", (mp_now() - g_race_loaded_at) / 1000.0);
    for (int i = 0; i < g_race_cars_n; ++i) {
        MpCar *m = &g_race_cars[i].car;
        g_race_cars[i].following = 1;
        m->kept = 0;
        m->taken = 0;
        m->played_at = 0;
    }
}

/* Whether mp.h has built another player's car in this world (free roam). */
static int race_any_remote(void) {
    for (int i = 0; i < MP_SLOTS; ++i)
        if (g_mp_remote[i].simable)
            return 1;
    return 0;
}

/* Other players' racer dots on the minimap: list 7 too, after the AI's list 3.
 * The minimap (005e5287) asks for list 3 every frame, races or not, so this
 * also puts friends driving near in free roam on the game's own minimap. */
static void race_minimap(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    if (get_u32(cpu->esp + 4) != 3u || (!g_race_cars_n && !race_any_remote()))
        return;
    const uint32_t args[2] = {7u, get_u32(cpu->esp + 8)};
    uint32_t eax = 0;
    api->guest_call(api, RACE_MINIMAP_RACERS, cpu->ecx, args, 2, &eax);
}

/* The Quick Race options block of a race key's type, in this player's profile. */
static uint32_t race_options(uint32_t key, uint32_t *type_out) {
    const uint32_t out = g_race_buf + 0xa0;
    mp_put(out, 0);
    const uint32_t args[5] = {0xbc3u, out, key, 0xffffffffu, 0xffffffffu};
    mp_call(RACE_DAL_INT, RACE_DAL, args, 5);
    const uint32_t type = get_u32(out);
    const uint32_t zero = 0, profile = mp_call(RACE_PROFILE, get_u32(RACE_FE_DB), &zero, 1);
    if (type_out)
        *type_out = type;
    return profile && type < 17 ? profile + RACE_QR_BLOCKS + type * 0x28u : 0;
}

/* The Quick Race Start button: caught while the page hosts. */
static void race_start(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t key = get_u32(cpu->esp + 4), mode = get_u32(cpu->esp + 8);
    RaceShared *sh = race_shared();
    if (sh && !g_race_replaying && mode == 7 && (g_race_page.flags & RACE_CATCH) && get_u32(RACE_FLOW) == 3) {
        uint32_t type = 0;
        const uint32_t block = race_options(key, &type);
        if (block) {
            __atomic_fetch_add(&sh->game_seq, 1u, __ATOMIC_SEQ_CST);
            sh->caught_key = key;
            sh->caught_type = type;
            for (uint32_t i = 0; i < 40; ++i)
                sh->caught_opts[i] = race_u8(block + i);
            sh->caught++;
            __atomic_fetch_add(&sh->game_seq, 1u, __ATOMIC_SEQ_CST);
            race_log("core.nfsc: race: Quick Race %08x (type %u, %u laps) caught for the lobby", key, type,
                     race_u8(block + 4));
            api->hook_return(api, cpu, 0, 0);
            return;
        }
    }
    api->call_original(api, cpu->target, cpu);
}


/* Flow 3 includes the startup screens, where the vehicle factories are not
 * ready yet. Launch only from loaded race-selection or main-menu screens. */
static int race_menu_ready(void) {
    if (get_u32(RACE_FLOW) != 3) return 0;
    const uint32_t feng = get_u32(0x00a97a78u);
    if (!feng) return 0;
    const uint32_t names[] = {0x009d0298u, 0x009d0284u, 0x009cfdccu, 0x009cfc50u};
    for (uint32_t i = 0; i < sizeof names / sizeof names[0]; ++i)
        if (mp_call(0x00572350u, feng, &names[i], 1) & 0xffu) return 1;
    return 0;
}

/* From the front end: start the session's race, with the host's options. */
static void race_launch(void) {
    const RacePage *p = &g_race_page;
    if (!p->launch || p->launch != p->session || p->launch == g_race_launched || !race_menu_ready())
        return;
    g_race_launched = p->launch;
    uint32_t type = 0;
    const uint32_t block = race_options(p->launch_key, &type);
    if (!block) {
        race_log("core.nfsc: race: race %08x is not a Quick Race here", p->launch_key);
        g_race_session = p->launch;
        return;
    }
    for (uint32_t i = 0; i < 40; ++i)
        g_api->guest_write_u8(g_api, block + i, p->launch_opts[i]);
    mp_put(block, p->launch_key);

    race_forget("a new race");
    g_race_session = p->launch;
    g_race_phase = RACE_LAUNCHED;
    g_race_no_ai = !(p->flags & RACE_KEEP_AI);
    const uint32_t args[2] = {p->launch_key, 7u};
    g_race_replaying = 1;
    mp_call(RACE_START, 0, args, 2);
    g_race_replaying = 0;
    race_log("core.nfsc: race: session %u: Quick Race %08x started (type %u, %u laps, %s AI, %u others)",
             p->session, p->launch_key, type, p->launch_opts[4], g_race_no_ai ? "no" : "with", p->count);
}

/* The car model a Quick Race started now would give this player, so the others
 * can build it before the race loads: the car the race loader (00767d10) takes,
 * DAL 7000 for player 0 (or, with DAL 0x1771 set, the career car 0x1772), as
 * the stable record (0049c020) gives it. Its +8 is the car's own collection
 * (named "R8", "CAMARON", ...), which builds that model in its stock look; a
 * customized car shows another key on the built car (mp_model_key), a
 * collection made for that player's own copy, which no other game has. The DAL
 * may answer differently with the FE database's mode (+8), which 007bbed0 sets
 * to 7 before the race loads, so the same is done here for the question. */
static uint32_t race_menu_car(void) {
    const uint32_t out = g_race_buf + 0xa4, db = get_u32(RACE_FE_DB);
    const uint32_t mode = db ? get_u32(db + 8) : 0;
    if (db)
        mp_put(db + 8, 7u);
    mp_put(out, 0xffffffffu);
    const uint32_t args[5] = {0x1b58u, out, 0, 0xffffffffu, 0xffffffffu};
    mp_call(RACE_DAL_INT, RACE_DAL, args, 5);
    uint32_t handle = get_u32(out);
    mp_put(out, 0);
    const uint32_t career[5] = {0x1771u, out, 0xffffffffu, 0xffffffffu, 0xffffffffu};
    mp_call(RACE_DAL_INT, RACE_DAL, career, 5);
    if (get_u32(out)) {
        const uint32_t car[5] = {0x1772u, out, 0xffffffffu, 0xffffffffu, 0xffffffffu};
        mp_call(RACE_DAL_INT, RACE_DAL, car, 5);
        handle = get_u32(out);
    }
    if (db)
        mp_put(db + 8, mode);
    const uint32_t zero = 0, profile = mp_call(RACE_PROFILE, db, &zero, 1);
    const uint32_t record = profile && handle != 0xffffffffu ? mp_call(RACE_FIND_CAR, profile + 0x234, &handle, 1) : 0;
    return record ? get_u32(record + 8) : 0;
}

#define RACE_PLACEMENT 0x006512e0u /* cdecl (activity, ISimable *, place) */
static void race_placement(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t simable = get_u32(cpu->esp + 8), player = mp_player();
    if(g_race_encounter && g_race_aborting) {api->hook_return(api,cpu,0,0);return;}
    if (g_race_encounter && g_race_session && g_race_session == g_race_page.session) {
        for(int i=0;simable && i<g_race_cars_n;i++)
            if(g_race_cars[i].id && g_race_cars[i].car.simable==simable) {
                race_log("core.nfsc: encounter: ignored a copied racer placement");
                api->hook_return(api,cpu,0,0);return;
            }
    }
    if (!g_race_result_replaying && g_race_session &&
        g_race_page.session == g_race_session && g_race_page.launch == g_race_session &&
        g_race_phase >= RACE_RACING && player && simable == player + 0x24) {
        if(g_race_encounter && g_race_finish_observer) {
            const uint32_t rs=race_status();const int mine=rs?race_row_of(rs,player+0x24):-1;
            if(mine>=0 && race_u8(race_row(rs,(uint32_t)mine)+0x46))
                g_race_finish_observer(g_race_session,bits_float(get_u32(race_row(rs,(uint32_t)mine)+0x184)));
        }
        g_race_result_activity = get_u32(cpu->esp + 4);
        g_race_result_simable = simable;
        race_log("core.nfsc: race: holding local placement %u for shared results", get_u32(cpu->esp + 12));
        api->hook_return(api, cpu, 0, 0);
        return;
    }
    api->call_original(api, cpu->target, cpu);
}

static void race_result(uint32_t rs, uint32_t player) {
    /* Online Crew Challenges commit one first-finisher outcome in encounter.h. */
    if(g_race_encounter)return;
    if (!g_race_result_activity || !rs || !player)
        return;
    if (!g_race_session || g_race_session != g_race_page.session ||
        g_race_result_simable != player + 0x24) {
        g_race_result_activity = g_race_result_simable = 0;
        return;
    }
    /* Every remote time must reach both the page and the native racer table.
     * A missing player must never turn into a fabricated winning result. */
    for (uint32_t k = 0; k < g_race_page.count && k < RACE_PEERS; ++k) {
        const RacePeer *p = &g_race_page.peers[k];
        if (!p->id) continue;
        if (!p->done || !(p->time > 0)) return;
        int finished = 0;
        for (int i = 0; i < g_race_cars_n; ++i)
            if (g_race_cars[i].id == p->id)
                finished = race_u8(race_row(rs, g_race_cars[i].row) + 0x46);
        if (!finished) return;
    }
    const int mine = race_row_of(rs, player + 0x24);
    if (mine < 0 || !race_u8(race_row(rs, (uint32_t)mine) + 0x46)) return;
    mp_call(RACE_RANK, rs, NULL, 0);
    const uint32_t place = get_u32(race_row(rs, (uint32_t)mine) + 0x34);
    const uint32_t args[3] = {g_race_result_activity, g_race_result_simable, place};
    g_race_result_activity = g_race_result_simable = 0;
    g_race_result_replaying = 1;
    mp_call(RACE_PLACEMENT, 0, args, 3);
    g_race_result_replaying = 0;
    race_log("core.nfsc: race: shared placement %u released", place);
}

/* Copies the page's part when it is not being written. */
static void race_read_page(RaceShared *sh) {
    if(g_race_page_source && g_race_page_source(&g_race_page))return;
    const uint32_t before = __atomic_load_n(&sh->page_seq, __ATOMIC_ACQUIRE);
    if (before & 1)
        return;
    RacePage copy;
    memcpy(&copy, &sh->page, sizeof copy);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&sh->page_seq, __ATOMIC_ACQUIRE) == before)
        g_race_page = copy;
}

/* The slot mp.h copied this frame for a player, or NULL. */
static const MpSlot *race_slot(uint32_t id, int *index) {
    for (int i = 0; id && i < MP_SLOTS; ++i)
        if (g_mp_seen_slot[i].id == id && g_mp_seen_slot[i].mode == 1) {
            if (index)
                *index = i;
            return &g_mp_seen_slot[i];
        }
    return NULL;
}

/* Moves each other player's car to where their poses say it is now, through mp.h's
 * mp_follow as free roam does: a touch moves it the way it moves an AI car (mp.h's
 * contact code finds it with race_racer_car) and it is then steered back onto its
 * owner's poses rather than placed there, which made racers as heavy as walls.
 * `late`: the main view's set-up, where the pull back is applied (as mp_place_all). */
static void race_place_all(int late) {
    const double now = mp_now();
    for (int i = 0; i < g_race_cars_n; ++i) {
        RaceCar *c = &g_race_cars[i];
        if (!c->following || !c->car.simable || !c->car.kept)
            continue;
        MpPose pose;
        mp_follow(&c->car, now, late, &pose);
        memcpy(c->car.drawn, pose.p, sizeof c->car.drawn);
    }
}

/* mp.h's g_mp_racer_car: the race car another player owns, for its contact code. */
static MpCar *race_racer_car(uint32_t owner) {
    for (int i = 0; i < g_race_cars_n; ++i)
        if (g_race_cars[i].following && g_race_cars[i].car.simable == owner)
            return &g_race_cars[i].car;
    return NULL;
}

static void race_finish_peer(uint32_t rs, RaceCar *c, float time);
static const RacePeer *race_peer(uint32_t id);

/* Test car: the first AI racer's path, 0.6 s behind it. */
/* The first AI racer's path, recorded once a frame with its progress. Returns its row, or 0. */
static uint32_t race_ai_path(uint32_t rs) {
    static uint32_t frame;
    static uint32_t row;
    if (frame == g_race_frame)
        return row;
    frame = g_race_frame;
    uint32_t ai = 0;
    row = 0;
    for (uint32_t i = 0, n = race_count(rs); i < n && !ai; ++i)
        if (get_u32(race_row(rs, i) + 4)) {
            ai = race_car_of(get_u32(race_row(rs, i)));
            row = race_row(rs, i);
        }
    g_race_follow_ai = ai;
    const uint32_t from = ai ? mp_state(get_u32(ai + 0x70)) : 0;
    MpPose p;
    if (!from)
        return row = 0;
    mp_read(from, &p);
    if (!mp_pose_ok(&p))
        return row;
    if (g_race_path_n == RACE_FOLLOW) {
        memmove(g_race_path, g_race_path + 1, sizeof g_race_path[0] * (RACE_FOLLOW - 1));
        memmove(g_race_path_at, g_race_path_at + 1, sizeof g_race_path_at[0] * (RACE_FOLLOW - 1));
        memmove(g_race_path_row, g_race_path_row + 1, sizeof g_race_path_row[0] * (RACE_FOLLOW - 1));
        --g_race_path_n;
    }
    g_race_path[g_race_path_n] = p;
    g_race_path_row[g_race_path_n][0] = bits_float(get_u32(row + 0x3c));
    g_race_path_row[g_race_path_n][1] = bits_float(get_u32(row + 0x48));
    g_race_path_row[g_race_path_n][2] = (float)get_u32(row + 0x4c);
    g_race_path_row[g_race_path_n][3] = (float)get_u32(row + 0x50);
    g_race_path_at[g_race_path_n++] = mp_now();
    return row;
}

/* Puts a car where the AI was `lag` ms ago; the index of that pose, or -1 while the AI is not that far ahead. */
static int race_follow(uint32_t body, double lag) {
    const uint32_t state = mp_state(body);
    const double t = mp_now() - lag;
    if (!state || !g_race_path_n || g_race_path_at[0] > t)
        return -1;
    int k = 0;
    while (k + 1 < g_race_path_n && g_race_path_at[k + 1] <= t)
        ++k;
    mp_write(body, state, &g_race_path[k]);
    ++g_race_follow_writes;
    return k;
}

/* Test car: the first AI racer's path, 0.6 s behind it, with the AI's progress as its own. */
static void race_test_follow(uint32_t rs, RaceCar *c) {
    const uint32_t ai_row = race_ai_path(rs);
    if (!ai_row)
        return;
    if (race_u8(ai_row + 0x46) && !race_u8(race_row(rs, c->row) + 0x46)) {
        /* The AI's own finish, 0.6 s later: the way another player's time arrives. */
        race_finish_peer(rs, c, bits_float(get_u32(ai_row + 0x184)) + 0.6f);
        return;
    }
    const int k = race_follow(get_u32(c->car.simable - 0x24 + 0x70), 600.0);
    if (k < 0)
        return; /* stays on its grid slot until the AI is 0.6 s ahead */
    memcpy(g_race_test_row, g_race_path_row[k], sizeof g_race_test_row); /* as if it came from its own game */
    g_race_test_row_ok = 1;
}

/* Pilot (RECOMP_RACE_PILOT=<ms>, tests without a driver): this player's own car
 * drives the first AI racer's path that many ms behind it, until it finishes. */
static void race_pilot(uint32_t rs, uint32_t player) {
    const int mine = race_row_of(rs, player + 0x24);
    if (mine < 0 || race_u8(race_row(rs, (uint32_t)mine) + 0x46))
        return;
    if (race_ai_path(rs))
        race_follow(get_u32(player + 0x70), (double)g_race_pilot);
}

static void race_dump(uint32_t rs) {
    const uint32_t n = race_count(rs);
    char line[512];
    float tp[3] = {0, 0, 0}, ap[3] = {0, 0, 0};
    for (int i = 0; i < g_race_cars_n && !g_race_cars[i].id; ++i) {
        const uint32_t st = mp_state(get_u32(g_race_cars[i].car.simable - 0x24 + 0x70));
        for (int k = 0; st && k < 3; ++k)
            tp[k] = bits_float(get_u32(st + 0x10 + 4u * k));
    }
    const uint32_t ast = g_race_follow_ai ? mp_state(get_u32(g_race_follow_ai + 0x70)) : 0;
    for (int k = 0; ast && k < 3; ++k)
        ap[k] = bits_float(get_u32(ast + 0x10 + 4u * k));
    race_log("core.nfsc: race test: phase %u, test car at %.1f %.1f %.1f, AI %08x at %.1f %.1f %.1f, %u poses written, "
             "play mode %u",
             g_race_phase, tp[0], tp[1], tp[2], g_race_follow_ai, ap[0], ap[1], ap[2], g_race_follow_writes,
             get_u32(rs + 0x6a14));
    int at = snprintf(line, sizeof line, "core.nfsc: race: t %.1f |",
                      bits_float(get_u32(RACE_SIM_CLOCK)) - bits_float(get_u32(rs + 0x6a24)));
    for (uint32_t i = 0; i < n && at < (int)sizeof line - 80; ++i) {
        const uint32_t r = race_row(rs, i);
        char name[16];
        for (int k = 0; k < 15; ++k)
            name[k] = (char)race_u8(r + 8 + k);
        name[15] = 0;
        at += snprintf(line + at, sizeof line - at, " %.10s p%u %.1f%% L%u cp%u%s %.2f |", name, get_u32(r + 0x34),
                       bits_float(get_u32(r + 0x3c)), get_u32(r + 0x4c), get_u32(r + 0x50),
                       race_u8(r + 0x46) ? " FIN" : "", bits_float(get_u32(r + 0x184)));
    }
    log_line(line);
}

/* Writes a finish time the other player's own game measured into their row. */
static void race_finish_peer(uint32_t rs, RaceCar *c, float time) {
    const uint32_t r = race_row(rs, c->row);
    if (race_u8(r + 0x46) && fabsf(bits_float(get_u32(r + 0x184)) - time) < 0.0005f)
        return;
    const int was = race_u8(r + 0x46);
    put_float(r + 0x184, time);
    g_api->guest_write_u8(g_api, r + 0x188, 0);
    if (!was)
        mp_call(RACE_FINISH, r, NULL, 0);
    put_float(r + 0x184, time);
    put_float(r + 0x3c, 100.0f);
    put_float(r + 0x48, 100.0f);
    const RacePeer *peer = race_peer(c->id);
    if (peer && peer->laps > get_u32(r + 0x4c) && peer->laps < 100)
        mp_put(r + 0x4c, peer->laps);
    mp_call(RACE_RANK, rs, NULL, 0);
    race_log("core.nfsc: race: player %u finished in %.3f s (%s)", c->id, time,
             was ? "their time replaces ours" : "from their game");
}

/* The page's entry for a player, or NULL. */
static const RacePeer *race_peer(uint32_t id) {
    for (uint32_t k = 0; id && k < g_race_page.count && k < RACE_PEERS; ++k)
        if (g_race_page.peers[k].id == id)
            return &g_race_page.peers[k];
    return NULL;
}

/* GRacerInfo::Update (00644150, thiscall (dt), RET 4) works out a racer's
 * progress from its AI road navigator. A class-7 car's AIVehicleEmpty gives it
 * none: a test car driven exactly along an AI racer's path stayed at 0 % and no
 * checkpoint, lap or finish trigger counted it. So another player's progress is
 * the one their own game worked out, sent over the relay and carried on to the
 * moment their car is drawn at (its playback clock, in their physics ms). */
static void race_progress(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t r = cpu->ecx;
    RaceCar *c = g_race_cars_n ? race_car_by_row(r) : NULL;
    api->call_original(api, cpu->target, cpu);
    if (!c)
        return;
    if (race_u8(r + 0x46))
        return; /* finished: the row keeps its time */
    float pct, lap_pct;
    uint32_t laps, cp;
    if (!c->id) {
        if (!g_race_test_row_ok)
            return;
        pct = g_race_test_row[0];
        lap_pct = g_race_test_row[1];
        laps = (uint32_t)g_race_test_row[2];
        cp = (uint32_t)g_race_test_row[3];
    } else {
        const RacePeer *p = race_peer(c->id);
        if (!p || !p->ms)
            return;
        double dt = c->car.played_at > 0 ? (c->car.play - (double)p->ms) / 1000.0 : 0.0;
        dt = dt < -1.0 ? -1.0 : dt > 1.0 ? 1.0 : dt;
        pct = p->pct + p->rate * (float)dt;
        lap_pct = p->lap_pct + p->lap_rate * (float)dt;
        laps = p->laps;
        cp = p->cp;
    }
    pct = pct < 0 ? 0 : pct > 99.99f ? 99.99f : pct;
    lap_pct = lap_pct < 0 ? 0 : lap_pct > 99.99f ? 99.99f : lap_pct;
    put_float(r + 0x3c, pct);
    put_float(r + 0x48, lap_pct);
    mp_put(r + 0x4c, laps);
    mp_put(r + 0x50, cp);
}

/* No projected finish time for another player who has not finished: their real one will come. */
static void race_project(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const RaceCar *c = g_race_cars_n ? race_car_by_row(cpu->ecx) : NULL;
    if (((g_race_encounter && g_race_session) || (c && c->id)) && !race_u8(cpu->ecx + 0x46)) {
        api->hook_return(api, cpu, cpu->eax, 0);
        return;
    }
    api->call_original(api, cpu->target, cpu);
}

/* The game's part: phase, this player's progress, and the racer table. */
static void race_publish(RaceShared *sh, uint32_t rs, uint32_t player) {
    const int mine = player ? race_row_of(rs, player + 0x24) : -1;
    __atomic_fetch_add(&sh->game_seq, 1u, __ATOMIC_SEQ_CST);
    sh->phase = g_race_phase;
    sh->flow_flags = get_u32(RACE_FLOW) | (g_race_result_activity ? 0x100u : 0);
    sh->session = g_race_session;
    sh->car = player ? mp_model_key(player) : race_menu_ready() ? g_race_car_key : 0;
    sh->loaded = g_race_loaded_at > 0;
    sh->ms = mp_ms();
    const uint32_t n = race_count(rs);
    sh->rows = n < RACE_ROWS ? n : RACE_ROWS;
    for (uint32_t i = 0; i < sh->rows; ++i) {
        const uint32_t r = race_row(rs, i);
        RaceRow *o = &sh->row[i];
        const RaceCar *c = race_car_by_row(r);
        o->id = c ? c->id : 0;
        o->me = (int)i == mine;
        o->place = get_u32(r + 0x34);
        o->laps = get_u32(r + 0x4c);
        o->done = race_u8(r + 0x46);
        o->pct = bits_float(get_u32(r + 0x3c));
        o->time = race_u8(r + 0x188) ? bits_float(get_u32(RACE_SIM_CLOCK)) - bits_float(get_u32(r + 0x180)) +
                                           bits_float(get_u32(r + 0x184))
                                     : bits_float(get_u32(r + 0x184));
        for (int k = 0; k < 16; ++k)
            o->name[k] = k < 15 ? (char)race_u8(r + 8 + k) : 0;
    }
    if (mine >= 0) {
        const RaceRow *o = &sh->row[mine < RACE_ROWS ? mine : 0];
        const uint32_t r = race_row(rs, (uint32_t)mine);
        sh->place = get_u32(r + 0x34);
        sh->laps = get_u32(r + 0x4c);
        sh->done = race_u8(r + 0x46);
        sh->pct = bits_float(get_u32(r + 0x3c));
        sh->lap_pct = bits_float(get_u32(r + 0x48));
        sh->cp = get_u32(r + 0x50);
        sh->time = mine < RACE_ROWS ? o->time : bits_float(get_u32(r + 0x184));
    } else {
        sh->place = sh->laps = sh->done = sh->cp = 0;
        sh->pct = sh->time = sh->lap_pct = 0;
    }
    __atomic_fetch_add(&sh->game_seq, 1u, __ATOMIC_SEQ_CST);
}

/* Once a frame, after mp.h's. */
static void race_service(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)cpu;
    (void)inv;
    (void)user;
    ++g_race_frame;
    RaceShared *sh = race_shared();
    if (sh)
        race_read_page(sh);
    const uint32_t rs = race_status(), flow = get_u32(RACE_FLOW), player = mp_player();
    if (flow == 3 || !rs || ((g_race_phase >= RACE_RACING || (g_race_encounter && g_race_aborting && g_race_phase >= RACE_SET_UP)) && get_u32(rs + 0x6a14) != 1u)) {
        /* In the menus, or the race has turned into free roam (play mode 0). */
        if (g_race_cars_n || g_race_phase > RACE_LAUNCHED)
            race_forget(flow == 3 ? "back in the menus" : "left the race");
        if (flow == 3) {
            if (g_race_frame % 30 == 0) {
                const uint32_t key = race_menu_car();
                if (key != g_race_car_key)
                    race_log("core.nfsc: race: the Quick Race car is model %08x", key);
                g_race_car_key = key;
            }
            race_launch();
        }
    }
    for (int i = 0; i < g_race_cars_n; ++i) {
        RaceCar *c = &g_race_cars[i];
        if (c->car.simable && !race_alive(c->car.simable)) {
            race_log("core.nfsc: race: the car of player %u went with the world", c->id);
            c->car.simable = 0;
        }
    }
    if (rs && g_race_cars_n) {
        const double now = mp_now();
        for (int i = 0; i < g_race_cars_n; ++i) {
            RaceCar *c = &g_race_cars[i];
            if (!c->car.simable)
                continue;
            if (!c->id) {
                if (g_race_test == 1 && g_race_phase >= RACE_RACING)
                    race_test_follow(rs, c);
                continue;
            }
            const MpSlot *s = c->following ? race_slot(c->id, NULL) : NULL;
            if (s)
                mp_take(&c->car, s, now);
            for (uint32_t k = 0; k < g_race_page.count && k < RACE_PEERS; ++k) {
                const RacePeer *peer = &g_race_page.peers[k];
                if (peer->id == c->id && peer->done && peer->time > 0)
                    race_finish_peer(rs, c, peer->time);
            }
        }
        race_place_all(0);
        if (g_race_test && g_race_frame % 120 == 0)
            race_dump(rs);
    }
    /* This player's own grid slot while everyone loads. */
    if (rs && player && g_race_phase == RACE_HOLDING && g_race_session && !g_race_test) {
        float pos[3], dir[3];
        const uint32_t body = get_u32(player + 0x70), state = mp_state(body);
        if (state && race_grid(g_race_base + g_race_page.grid, pos, dir)) {
            MpPose pose;
            mp_read(state, &pose);
            if (mp_pose_ok(&pose)) {
                pose.p[0] = pos[0];
                pose.p[2] = pos[2];
                for (int k = 0; k < 3; ++k)
                    pose.v[k] = pose.w[k] = 0;
                mp_write(body, state, &pose);
            }
        }
    }
    if (rs && player && g_race_pilot && g_race_phase >= RACE_RACING)
        race_pilot(rs, player);
    if (rs && player && g_race_phase == RACE_RACING) {
        const int mine = race_row_of(rs, player + 0x24);
        if (mine >= 0 && race_u8(race_row(rs, (uint32_t)mine) + 0x46)) {
            g_race_phase = RACE_DONE;
            race_log("core.nfsc: race: finished in %.3f s, place %u",
                     bits_float(get_u32(race_row(rs, (uint32_t)mine) + 0x184)),
                     get_u32(race_row(rs, (uint32_t)mine) + 0x34));
        }
    }
    race_result(rs, player);
    if (sh)
        race_publish(sh, rs, player);
}

/* Each view as it is set up: the main view (1) puts the cars where they are now. */
static void race_view(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)inv;
    (void)user;
    const uint32_t view = get_u32(cpu->esp + 4);
    if (g_race_cars_n && view && get_u32(view + 8) == 1u)
        race_place_all(1);
}

/* After the main view's matrices: name tags over the racers' cars (net.js draws
 * them). AFTER hooks run newest first, so this runs before mp_view_ready, which
 * leaves the slots of players race.h owns alone. */
static void race_view_ready(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)cpu;
    (void)inv;
    (void)user;
    MpShared *sh = mp_shared();
    if (!sh || !g_mp_view || !g_race_cars_n)
        return;
    for (int i = 0; i < g_race_cars_n; ++i) {
        const RaceCar *c = &g_race_cars[i];
        int slot = -1;
        if (!c->id || !c->car.simable || !race_slot(c->id, &slot) || slot < 0)
            continue;
        float s[2];
        const float above[3] = {c->car.drawn[0], c->car.drawn[1] + MP_LABEL_UP, c->car.drawn[2]};
        const float depth = c->following && c->car.kept ? mp_project(g_mp_view + 0x80, above, 1, s) : 0.0f;
        MpLabel *l = &sh->labels[slot];
        l->x = s[0];
        l->y = s[1];
        l->depth = depth > 0.5f && s[0] > -0.2f && s[0] < 1.2f && s[1] > -0.2f && s[1] < 1.2f ? depth : 0.0f;
    }
}

static PopModStatus race_hook(uint32_t addr, PopHookFn fn, int32_t mode, int slot) {
    PopModStatus s = g_api->hook_install(g_api, addr, fn, mode, 0, &g_race_hooks[slot]);
    if (s != POP_OK)
        race_log("core.nfsc: race: hook %08x failed (%d)", addr, (int)s);
    return s;
}

static void race_install(void) {
    const char *test = getenv("RECOMP_RACE_TEST");
    g_race_test = test && *test ? atoi(test) : 0;
    const char *pilot = getenv("RECOMP_RACE_PILOT");
    g_race_pilot = pilot && *pilot ? atoi(pilot) : 0;
    if (g_api->guest_alloc(g_api, 0x100, &g_race_buf) != POP_OK ||
        g_api->guest_alloc(g_api, sizeof(RaceShared), &g_race_block) != POP_OK) {
        log_line("core.nfsc: racing together unavailable (no guest memory)");
        return;
    }
    RaceShared *sh = race_shared();
    if (!sh) {
        g_race_block = 0;
        return;
    }
    memset(sh, 0, sizeof *sh);
    sh->magic = RACE_MAGIC;
    sh->version = 1;
    if (race_hook(RACE_INIT_RACERS, race_init_racers, POP_HOOK_AFTER, 0) != POP_OK ||
        race_hook(MP_FE_SERVICE, race_service, POP_HOOK_BEFORE, 1) != POP_OK ||
        race_hook(RACE_CARS_LOADED, race_cars_loaded, POP_HOOK_AFTER, 2) != POP_OK) {
        log_line("core.nfsc: racing together unavailable");
        return;
    }
    race_hook(RACE_NUM_OPPONENTS, race_opponents, POP_HOOK_AFTER, 3);
    race_hook(RACE_WINGMAN, race_wingman, POP_HOOK_REPLACE, 4);
    race_hook(RACE_MINIMAP_RACERS, race_minimap, POP_HOOK_BEFORE, 5);
    race_hook(RACE_START, race_start, POP_HOOK_REPLACE, 6);
    race_hook(RACE_PROJECT, race_project, POP_HOOK_REPLACE, 7);
    race_hook(RACE_RACER_UPDATE, race_progress, POP_HOOK_REPLACE, 10);
    race_hook(0x0071b780u, race_view, POP_HOOK_BEFORE, 8);
    race_hook(0x0071b780u, race_view_ready, POP_HOOK_AFTER, 9);
    race_hook(RACE_PLACEMENT, race_placement, POP_HOOK_REPLACE, 11);
    g_mp_racer = race_owns;
    g_mp_racer_car = race_racer_car;
    if (g_race_test || g_race_pilot)
        race_log("core.nfsc: race test %d, pilot %d ms", g_race_test, g_race_pilot);
    if (g_race_test == 2)
        g_race_no_ai = 1;
#ifdef __EMSCRIPTEN__
    MAIN_THREAD_ASYNC_EM_ASM({
        var block = {};
        block.memory = wasmMemory;
        block.ptr = $0;
        globalThis.carbonRaceBlock = block;
        globalThis.dispatchEvent(new Event("carbon-race"));
    }, (uint32_t)(uintptr_t)sh);
#endif
}
