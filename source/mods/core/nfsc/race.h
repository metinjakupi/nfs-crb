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
/* CANYON DUEL TOGETHER, see race_duel_frame */
#define RACE_DUEL 0xbcb8u            /* GRaceStatus + this: the Canyon Duel */
#define RACE_DUEL_PLACES 0x006278c0u /* fastcall (duel): both racers' places from its state */
#define RACE_DUEL_PLACE 0x00616a10u  /* thiscall (duel, slot vehicle) -> AL 1 or 2 */
#define RACE_DUEL_LOST 0x80000000u
#define RACE_DUEL_WON 0x40000000u
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
/* RACE START (logs/race-start-20261106). The page starts the race when everyone is loaded or when its
 * timer runs out; a player still loading then starts late: */
#define RACE_LATE 0x10u /* this player starts late: let go as soon as loaded, behind the grid, others pass through */
/* peer done bits (page), besides 1 finished and 2 left: */
#define RACE_PEER_LOADING 4u /* late and still loading: their car stays off the road */
#define RACE_PEER_GHOST 8u   /* late: their car comes on the road passing through, solid once clear */
#define RACE_PEER_READY 16u  /* before go: their game has loaded, so their car is shown parked on its grid slot */
#define RACE_TAGS 0x53474154u /* "TAGS": RaceShared.tag[] is written (pages read it only then) */
#define RACE_CLEAR 7.0f      /* m: a car passing through becomes solid once this far from every other car */
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
    uint32_t done;  /* bit 0: their finish time has arrived (or they were given up on). Bit 1: they left */
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
    uint32_t me, spare; /* spare (race start): bit 0 passes through (no contacts yet), bit 1 off the road */
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
    uint32_t drift_score; /* v3: banked native drift points, not race progress */
    uint32_t drift_failed; /* native overboard/too-slow end, not a score loss */
    /* RACE START: before go, where each peer's name tag goes on the grid (page peers order): over their car once it
     * is shown (ready), over their empty grid slot while they load. Screen 0..1, depth 0 = not on screen. */
    uint32_t tags_magic; /* RACE_TAGS */
    struct { float x, y, depth; } tag[RACE_PEERS];
} RaceShared;
#define RACE_MAGIC 0x31435243u /* "CRC1" */
/* The page reads these by offset (web/race.js). */
_Static_assert(sizeof(RacePeer) == 0x40 && offsetof(RaceShared, page) == 0x0c &&
                   offsetof(RaceShared, page.peers) == 0x60 && offsetof(RaceShared, game_seq) == 0x260 &&
                   offsetof(RaceShared, caught_opts) == 0x278 && offsetof(RaceShared, place) == 0x2a0 &&
                   offsetof(RaceShared, lap_pct) == 0x2c0 && offsetof(RaceShared, row) == 0x2d0 &&
                   offsetof(RaceShared, tags_magic) == 0x458 && offsetof(RaceShared, tag) == 0x45c &&
                   sizeof(RaceShared) == 0x4bc,
               "RaceShared layout");

typedef struct RaceCar {
    uint32_t id;    /* relay id; 0 = the test car */
    uint32_t row;   /* its row in the racer table */
    uint32_t key;
    int following;  /* follows the network (after go) */
    int left;       /* their player left (or is still loading, late): the car is off the road */
    int ghost;      /* RACE START: no contacts until clear of every other car (a late start) */
    int ghosted;    /* it has been a ghost: once solid it stays solid */
    int early;      /* RACE START: shown parked on its grid slot before go (its player has loaded) */
    int split;      /* the last whole percent written to its row's split times, see race_splits */
    double ghost_at;
    MpCar car;      /* the car and its playback, as mp.h keeps them */
} RaceCar;

static uint32_t g_race_hooks[18];
static uint32_t g_race_block, g_race_buf;
static int g_race_encounter, g_race_aborting, g_race_active_seen;
static int (*g_race_page_source)(RacePage *);
static void (*g_race_finish_observer)(uint32_t,float);
static RacePage g_race_page;           /* the last whole copy of the page's part */
static RaceCar g_race_cars[RACE_PEERS + 1];
static int g_race_cars_n;
static int g_race_late_start; /* RACE START: this player started late (RACE_LATE at the release) */
static int g_race_switching;  /* RACE START: our own SetPhysicsMode call, not the game's */
static uint32_t g_race_session;        /* the session whose race this game launched */
static uint32_t g_race_launched;       /* the last session launched (never launched twice) */
static uint32_t g_race_type;
static int g_race_cars_shown; /* the other players' cars may be active: go has come */
static uint32_t g_race_caught_key; /* the Start caught for the lobby: the menus left it unfinished */
static uint32_t g_race_helper;     /* the ISimable of the hidden third racer, see race_add_helper */
static int g_race_helper_row = -1; /* its row, until go takes it out of the table */
static const RacePeer *race_peer(uint32_t id);
static int race_is_drift(void);
static uint32_t race_drift_score(void);
static int race_drift_failed(void);
static int race_is_duel(void);
static uint32_t race_duel_report(uint32_t rs, uint32_t player);
static int g_race_duel_forced;
static int g_race_scene_skipped; /* race_no_scene: 1 = the loading screen is still up, see race_service */
static void race_drift_sync(uint32_t rs);
static void race_drift_clear(void);
static void race_drift_results(void);
static int race_drift_defer_enter(void);
static int race_drift_waiting(void);
static void race_drift_finish_local(uint32_t rs,uint32_t player);
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
    for (int n = 0; handle && node && node != MP_CARS && n < 256; ++n, node = get_u32(node))
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
    /* Canyon Drift is one car's event: a touch costs the speed that keeps a run alive, or the
     * run itself over the edge, so the others' cars pass through this player's (the user's
     * decision, 2026-11-04). mp.h only clears this flag on its own free-roam cars. */
    c->car.spawn_overlap = g_race_type == 12u;
    /* Built now because a racer must exist when InitRacers ends, but not shown: each game loads at
     * its own pace, and a car parked on the grid for a player still loading looks like a bot
     * (reported 2026-11-04). All of them appear at go; race_show_racers. */
    if (id && !g_race_encounter) {
        const uint32_t off = 0;
        g_race_cars_shown = 0;
        mp_call(MP_SET_PHYSICS, sim - 0x24u + 0xb8u, &off, 1);
    }
    race_log("core.nfsc: race: player %u (%.15s) is racer %u, model %08x, grid slot %u at %.1f %.1f %.1f",
             id, name, c->row, key, slot, pos[0], pos[1], pos[2]);
    return c;
}

/* The game plays its full race start (the starter between the cars lined up, the burnouts, its
 * sound) for three racers or more: with two it plays a plain solo-style one, which looked fake to
 * players (seen 2026-11-05: 2 players no; 2 + an AI yes; 2 + a third car that is no AI yes). So a
 * race of two gets a third racer that nobody sees: a parked copy of this player's car, physics off
 * (not simulated, not drawn, no contacts), no name. The table lets go of it at go, before any
 * racer launches, so it is in no place, list or result. Drift keeps its own start, and a Canyon
 * Duel is one against one. */
static void race_add_helper(uint32_t rs, uint32_t player) {
    g_race_helper = 0;
    g_race_helper_row = -1;
    if (g_race_encounter || race_count(rs) != 2u || g_race_type == 11u || g_race_type == 12u || g_race_type == 13u)
        return;
    RaceCar *c = race_add(rs, player, 0, 0, 2u, "", 0);
    if (!c)
        return;
    const uint32_t off = 0, row = race_row(rs, (uint32_t)c->row);
    g_race_helper = c->car.simable;
    g_race_helper_row = c->row;
    mp_call(MP_SET_PHYSICS, g_race_helper - 0x24u + 0xb8u, &off, 1);
    for (uint32_t i = 0; i < 32; ++i)
        g_api->guest_write_u8(g_api, row + 8 + i, 0);
    race_log("core.nfsc: race: a race of two: hidden third racer for the game's full start");
}

/* Go, before NotifyCountdownDone launches the racers: the last row goes, its car stays parked. */
static void race_drop_helper(void) {
    const uint32_t rs = race_status();
    if (g_race_helper_row < 0 || !rs)
        return;
    if ((int)race_count(rs) == g_race_helper_row + 1)
        mp_put(rs + 0x6a08u, (uint32_t)g_race_helper_row);
    g_race_helper_row = -1;
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
    race_drift_clear();
    g_race_type = 0;
    g_race_cars_n = 0;
    g_race_session = 0;
    g_race_phase = RACE_IDLE;
    g_race_encounter = g_race_aborting = g_race_active_seen = 0;
    g_race_loaded_at = 0;
    g_race_finish_sent = 0;
    g_race_result_activity = g_race_result_simable = 0;
    g_race_path_n = 0;
    g_race_no_ai = 0; /* kept until now: a restart runs InitRacers again */
    g_race_helper = 0;
    g_race_helper_row = -1;
    g_race_duel_forced = 0;
    g_race_late_start = 0;
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
            if (row && g_race_helper && c->car.simable == g_race_helper)
                g_race_helper_row = c->row; /* back in the table for this start */
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
    race_add_helper(rs, player);
    g_race_phase = RACE_SET_UP;
}

/* No AI racers for a race together, unless the host kept them. */
static void race_opponents(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)inv;
    (void)user;
    if (g_race_no_ai && ((g_race_page.flags & 4u) || (cpu->eip > RACE_INIT_RACERS && cpu->eip < RACE_INIT_END)))
        cpu->eax = 0;
}

static void race_wingman(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t from = get_u32(cpu->esp);
    if ((g_race_no_ai || (g_race_session && g_race_session == g_race_page.session)) &&
        ((g_race_page.flags & 4u) || from == RACE_WINGMAN_RET0 || from == RACE_WINGMAN_RET1)) {
        api->hook_return(api, cpu, 0, 0);
        return;
    }
    api->call_original(api, cpu->target, cpu);
}

/* Whether the loading screen waits for the others. */
static int race_holding(void) {
    if (g_race_phase < RACE_SET_UP || g_race_phase >= RACE_RACING)
        return 0;
    if(g_race_aborting)return 0;
    if(g_race_encounter)return !g_race_aborting && g_race_page.go != g_race_session;
    if (g_race_loaded_at > 0 && mp_now() - g_race_loaded_at > RACE_HOLD_MAX * 1000.0)
        return 0;
    if (g_race_test)
        return !(g_race_loaded_at > 0 && mp_now() - g_race_loaded_at > 8000.0);
    return g_race_page.go != g_race_session;
}

/* RACE START (logs/race-start-20261106): a car that passes through (a late start). Its body gets RBRemote's own
 * method table back, whose CanCollideWith keeps every car off it (as drive through, mp_sync_contacts), and mp.h's
 * shared touches skip it (spawn_overlap); mp_enable_contacts makes it solid again. */
static void race_ghost(RaceCar *c, int on) {
    const uint32_t body = c->car.simable ? get_u32(c->car.simable - 0x24u + 0x70u) : 0;
    if (body < 0x40u || get_u32(body - 0x14u) != c->car.simable)
        return;
    if (on) {
        if (g_mp_solid_vtable && get_u32(body - 0x40u) == g_mp_solid_vtable)
            mp_put(body - 0x40u, MP_REMOTE_VTABLE);
        c->ghost = c->ghosted = 1;
        c->ghost_at = mp_now();
        c->car.spawn_overlap = 1;
    } else {
        if (get_u32(body - 0x40u) == MP_REMOTE_VTABLE)
            mp_enable_contacts(c->car.simable);
        c->ghost = 0;
        c->car.spawn_overlap = g_race_type == 12u;
    }
    race_log("core.nfsc: race: player %u's car %s", c->id, on ? "passes through (late start)" : "is clear: solid again");
}

/* Whether one of our cars is kept off the road (a player who left, or one still loading): the game's own
 * activation of the racers (drift_race.h race_keep_racer) leaves it alone. */
static int race_kept_off(uint32_t simable) {
    for (int i = 0; !g_race_switching && simable && i < g_race_cars_n; ++i)
        if (g_race_cars[i].id && g_race_cars[i].car.simable == simable && g_race_cars[i].left)
            return 1;
    return 0;
}

static void race_set_physics(const RaceCar *c, uint32_t on) {
    g_race_switching = 1;
    mp_call(MP_SET_PHYSICS, c->car.simable - 0x24u + 0xb8u, &on, 1);
    g_race_switching = 0;
}

/* Go: the other players' cars appear, except a player the host started without, and one still loading (a late
 * start: theirs comes when their game has loaded, race_car_left). */
static void race_show_racers(void) {
    g_race_cars_shown = 1;
    for (int i = 0; i < g_race_cars_n; ++i) {
        RaceCar *c = &g_race_cars[i];
        const RacePeer *peer = race_peer(c->id);
        if (!c->id || !c->car.simable)
            continue;
        if (peer && (peer->done & ~(RACE_PEER_GHOST | RACE_PEER_READY))) {
            if (c->early)
                race_set_physics(c, 0); /* shown on the grid, then left out */
            c->left = 1;
            c->following = 0;
            if (peer->done & RACE_PEER_LOADING)
                race_log("core.nfsc: race: player %u is still loading: car off the road until they start", c->id);
            continue;
        }
        race_set_physics(c, 1);
        if (peer && (peer->done & RACE_PEER_GHOST) && !c->ghosted)
            race_ghost(c, 1);
    }
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
    race_show_racers();
    /* RACE START: the others started without this player (still loading at the end of the page's timer). It starts
     * now, on its own 3-2-1, behind the grid (race_service), and passes through the others until clear of them. */
    g_race_late_start = (g_race_page.flags & RACE_LATE) && !(g_race_page.flags & 4u) && !g_race_encounter;
    if (g_race_late_start) {
        race_log("core.nfsc: race: late start: behind the grid (slot %u), passing through the others until clear",
                 g_race_base + g_race_page.grid);
        for (int i = 0; i < g_race_cars_n; ++i)
            if (g_race_cars[i].id && g_race_cars[i].car.simable && !g_race_cars[i].left)
                race_ghost(&g_race_cars[i], 1);
    }
}

/* Whether mp.h has built another player's car in this world (free roam). */
static int race_any_remote(void) {
    for (int i = 0; i < MP_SLOTS; ++i)
        if (g_mp_remote[i].simable)
            return 1;
    return 0;
}

static int cop_chase_copy(uint32_t simable); /* cops.h: a copy of a friend's cop (not traffic) */

/* Whether simable is another player's own car: a friend's (mp.h) or a racer's (this file). */
static int race_player_car(uint32_t simable) {
    for (int i = 0; i < MP_SLOTS; ++i)
        if (g_mp_remote[i].simable == simable)
            return 1;
    for (int i = 0; i < g_race_cars_n; ++i)
        if (g_race_cars[i].car.simable == simable && g_race_cars[i].id)
            return 1;
    return 0;
}

/* Other players' racer dots on the minimap: list 7 too, after the AI's list 3.
 * The minimap (005e5287) asks for list 3 every frame, races or not, so this
 * also puts friends driving near in free roam on the game's own minimap.
 * List 7 holds every car of driver class 7, and cops.h's copies of friends' traffic and cops are
 * class 7 too: each was an orange arrow where nothing was found (user report 2026-11-05; a test
 * game's list 7: its own car, the friend's and 13 copies). So the game draws a copy of the list
 * with only players' cars and copies of friends' cops in it (traffic copies left out; the vector's
 * begin +4 and count +0xc swapped for the call). */
#define RACE_LIST7 (0x00a9f158u + 7u * 0x88u) /* the vehicle lists, 0x88 bytes each (005d8ded) */
static uint32_t g_race_map_buf; /* 64 IVehicle pointers */
static void race_minimap(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    if (get_u32(cpu->esp + 4) != 3u || (!g_race_cars_n && !race_any_remote()))
        return;
    if (!g_race_map_buf && api->guest_alloc(api, 64 * 4, &g_race_map_buf) != POP_OK) {
        g_race_map_buf = 0;
        return;
    }
    const uint32_t begin = get_u32(RACE_LIST7 + 4), count = get_u32(RACE_LIST7 + 0xc);
    uint32_t kept = 0;
    for (uint32_t i = 0; i < count && kept < 64; ++i) {
        const uint32_t vehicle = get_u32(begin + 4u * i); /* IVehicle = PVehicle + 0xb8 */
        if (vehicle && (get_u32(vehicle - 0xb8u + 0x148) != MP_REMOTE || race_player_car(vehicle - 0xb8u + 0x24u) ||
                        cop_chase_copy(vehicle - 0xb8u + 0x24u))) /* friends' chases stay on the map */
            mp_put(g_race_map_buf + 4u * kept++, vehicle);
    }
    if (!kept)
        return;
    mp_put(RACE_LIST7 + 4, g_race_map_buf);
    mp_put(RACE_LIST7 + 0xc, kept);
    const uint32_t args[2] = {7u, get_u32(cpu->esp + 8)};
    uint32_t eax = 0;
    api->guest_call(api, RACE_MINIMAP_RACERS, cpu->ecx, args, 2, &eax);
    mp_put(RACE_LIST7 + 4, begin);
    mp_put(RACE_LIST7 + 0xc, count);
}

/* The Quick Race options block of a race key's type, in this player's profile. */
static uint32_t race_options(uint32_t key, uint32_t *type_out) {
    const uint32_t out = g_race_buf + 0xa0;
    mp_put(out, 0);
    const uint32_t args[5] = {0xbc3u, out, key, 0xffffffffu, 0xffffffffu};
    mp_call(RACE_DAL_INT, RACE_DAL, args, 5);
    const uint32_t menu_type = get_u32(out);
    /* DAL 0xbc3 uses menu categories (drift=5/6), not gameplay types (11/12). */
    const uint32_t raw = mp_call(0x0061bef0,get_u32(0x00a9828c),&key,1);
    const uint32_t type = raw ? mp_call(0x006136a0,raw,0,0) : 0xffffffffu;
    const uint32_t zero = 0, profile = mp_call(RACE_PROFILE, get_u32(RACE_FE_DB), &zero, 1);
    if (type_out)
        *type_out = type;
    return profile && menu_type < 17 && type < 17 ? profile + RACE_QR_BLOCKS + menu_type * 0x28u : 0;
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
            g_race_caught_key = key;
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
    /* FeMainMenu, FeMainMenu_Sub, FeQuickRaceOptions, FeCarSelect, FeWorldMapQuickList and
     * FeQuickRaceWingman: without the last two a player browsing the race list or choosing a
     * wingman never started, and the others waited for the timeout. */
    const uint32_t names[] = {0x009d0298u, 0x009d0284u, 0x009cfdccu, 0x009cfc50u, 0x009d01a8u, 0x009cfc38u};
    for (uint32_t i = 0; i < sizeof names / sizeof names[0]; ++i)
        if (mp_call(0x00572350u, feng, &names[i], 1) & 0xffu) return 1;
    return 0;
}

/* From the front end: start the session's race, with the host's options. */
static void race_launch(void) {
    const RacePage *p = &g_race_page;
    if (p->flags & 4u) return; /* world-map sessions never launch from the frontend */
    /* A host's caught Start: the menus have already gone, some without leaving a screen
     * race_menu_ready knows (after the Automatic or Manual question), and go on waiting for the
     * race that Start asked for. Its own launch is what that Start would have done. */
    if (!p->launch || p->launch != p->session || p->launch == g_race_launched ||
        !(race_menu_ready() || (get_u32(RACE_FLOW) == 3 && p->launch_key == g_race_caught_key)))
        return;
    g_race_launched = p->launch;
    g_race_caught_key = 0;
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
    g_race_type = type;
    g_race_phase = RACE_LAUNCHED;
    g_race_no_ai = race_is_drift() || !(p->flags & RACE_KEEP_AI);
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
    if(g_race_aborting) {api->hook_return(api,cpu,0,0);return;}
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
        if(race_is_drift())race_drift_finish_local(race_status(),player);
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
        if (!(p->done & 1u) || !(p->time > 0)) return;
        int finished = 0;
        for (int i = 0; i < g_race_cars_n; ++i)
            if (g_race_cars[i].id == p->id)
                finished = race_u8(race_row(rs, g_race_cars[i].row) + 0x46);
        if (!finished) return;
    }
    const int mine = race_row_of(rs, player + 0x24);
    if (mine < 0 || !race_u8(race_row(rs, (uint32_t)mine) + 0x46)) return;
    if(race_is_drift())race_drift_sync(rs);else mp_call(RACE_RANK, rs, NULL, 0);
    if (race_is_duel() && race_u8(rs + RACE_DUEL))
        mp_call(RACE_DUEL_PLACES, rs + RACE_DUEL, NULL, 0); /* CANYON DUEL TOGETHER: the duel's places */
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
    if (!race_is_duel()) { /* CANYON DUEL TOGETHER: at 100 % this game's meter showed ~4,000 M and its player lost */
        put_float(r + 0x3c, 100.0f);
        put_float(r + 0x48, 100.0f);
    }
    const RacePeer *peer = race_peer(c->id);
    if (peer && peer->laps > get_u32(r + 0x4c) && peer->laps < 100)
        mp_put(r + 0x4c, peer->laps);
    mp_call(RACE_RANK, rs, NULL, 0);
    if (race_is_duel() && race_u8(rs + RACE_DUEL))
        mp_call(RACE_DUEL_PLACES, rs + RACE_DUEL, NULL, 0); /* the duel's places, not times */
    race_log("core.nfsc: race: player %u finished in %.3f s (%s)", c->id, time,
             was ? "their time replaces ours" : "from their game");
}

/* RACE START: before go a player whose game has loaded has their car shown parked on its grid slot (the page's
 * RACE_PEER_READY), so the grid fills as the players arrive; a player still loading has none, only a name tag over
 * their slot (race_tags). Not for drift (its own start), a Canyon Duel or a world race. */
static int race_early_ok(void) {
    return g_race_session && !g_race_encounter && !(g_race_page.flags & 4u) && g_race_type != 11u &&
           g_race_type != 12u && g_race_type != 13u;
}
static void race_early_cars(void) {
    /* Once the go has come they stay as they are: the release shows every car (race_show_racers), and hiding them
     * first flickered them for a frame (run r3). */
    if (g_race_cars_shown || g_race_phase < RACE_SET_UP || g_race_phase >= RACE_RACING || g_race_page.go == g_race_session)
        return;
    for (int i = 0; i < g_race_cars_n; ++i) {
        RaceCar *c = &g_race_cars[i];
        const RacePeer *peer = c->id ? race_peer(c->id) : NULL;
        const int want = race_early_ok() && peer && (peer->done & RACE_PEER_READY) && !(peer->done & 2u);
        if (!c->id || !c->car.simable || want == c->early)
            continue;
        c->early = want;
        race_set_physics(c, (uint32_t)want);
        race_log("core.nfsc: race: player %u %s", c->id, want ? "has loaded: car shown on its grid slot" : "is not ready: car hidden");
    }
}
static int race_pos(uint32_t body, float out[3]);
/* After the main view's matrices: each peer's tag on the grid, before go. */
static void race_tags(void) {
    RaceShared *sh = race_shared();
    if (!sh)
        return;
    /* Only while this game holds on the grid: while its own cars load it may still show the loading screen, and tags
     * were drawn over it (run r3). */
    const int pre = g_mp_view && !g_race_cars_shown && g_race_phase == RACE_HOLDING && race_early_ok();
    for (uint32_t k = 0; k < RACE_PEERS; ++k) {
        float x = 0, y = 0, depth = 0, p[3], dir[3];
        const RacePeer *peer = pre && k < g_race_page.count ? &g_race_page.peers[k] : NULL;
        const RaceCar *c = NULL;
        for (int i = 0; peer && peer->id && i < g_race_cars_n; ++i)
            if (g_race_cars[i].id == peer->id)
                c = &g_race_cars[i];
        /* Its car once shown (parked on its slot); before that the game's own grid slot (0063d900): a hidden car's
         * body is not known to be there. */
        if (c && c->car.simable && ((c->early && race_pos(get_u32(c->car.simable - 0x24u + 0x70u), p)) ||
                                    race_grid(g_race_base + peer->grid, p, dir))) {
            float s[2];
            const float above[3] = {p[0], p[1] + MP_LABEL_UP, p[2]};
            depth = mp_project(g_mp_view + 0x80, above, 1, s);
            if (depth > 0.5f) { /* in front of the camera: at the screen's edge if off to the side (a neighbour on the grid) */
                x = s[0] < 0.05f ? 0.05f : s[0] > 0.95f ? 0.95f : s[0];
                y = s[1] < 0.2f ? 0.2f : s[1] > 0.85f ? 0.85f : s[1];
            } else
                depth = 0;
        }
        sh->tag[k].x = x;
        sh->tag[k].y = y;
        sh->tag[k].depth = depth;
    }
}

/* A player who left: mp.h does not remove a racer's car (g_mp_racer), so it would stay parked on
 * the road until the race ended (seen 2026-11-05). It goes the way it waited for go, and comes
 * back if the player does. Their row stays, so results still count them. */
static void race_car_left(RaceCar *c) {
    const RacePeer *peer = race_peer(c->id);
    /* RACE START: a late player still loading is off the road too; their car comes when their game has loaded. */
    const int left = g_race_cars_shown && peer && (peer->done & (2u | RACE_PEER_LOADING));
    if (left == c->left)
        return;
    c->left = left;
    c->following = !left;
    race_set_physics(c, !left);
    const int ghost = !left && peer && (peer->done & RACE_PEER_GHOST) && !c->ghosted;
    race_log("core.nfsc: race: player %u %s", c->id,
             left ? (peer->done & 2u) ? "left: car off the road" : "is still loading: car off the road"
                  : ghost ? "has loaded (late start): car on the road" : "is back: car on the road");
    if (ghost)
        race_ghost(c, 1);
}

/* RACE START: a car passing through becomes solid once it is RACE_CLEAR from this player's car and every other
 * car on the road (and has been on its poses a second). */
static int race_pos(uint32_t body, float out[3]) {
    const uint32_t state = mp_state(body);
    MpPose pose;
    if (!state)
        return 0;
    mp_read(state, &pose);
    if (!mp_pose_ok(&pose))
        return 0;
    memcpy(out, pose.p, sizeof pose.p);
    return 1;
}
static void race_ghosts(uint32_t player) {
    const double now = mp_now();
    for (int i = 0; i < g_race_cars_n; ++i) {
        RaceCar *c = &g_race_cars[i];
        float at[3], other[3];
        if (!c->ghost || !c->car.simable || c->left || now - c->ghost_at < 1000.0 ||
            !race_pos(get_u32(c->car.simable - 0x24u + 0x70u), at))
            continue;
        int clear = player && race_pos(get_u32(player + 0x70u), other);
        for (int k = 0; clear && k < 1 + g_race_cars_n; ++k) {
            const RaceCar *o = k ? &g_race_cars[k - 1] : NULL;
            if (o && (o == c || !o->id || !o->car.simable || o->left ||
                      !race_pos(get_u32(o->car.simable - 0x24u + 0x70u), other)))
                continue;
            const float dx = at[0] - other[0], dy = at[1] - other[1], dz = at[2] - other[2];
            clear = dx * dx + dy * dy + dz * dz > RACE_CLEAR * RACE_CLEAR;
        }
        if (clear)
            race_ghost(c, 0);
    }
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
static void race_splits(RaceCar *c, uint32_t r, float pct);
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
        if (!p)
            return;
        if (!p->ms || (p->done & 2u)) {
            /* Nothing reported yet, or they left (off the road): last among those racing. Left to the
             * game, a car parked off the road counted as ahead of the grid (a player left out at the
             * start was 1st, 2026-11-05). */
            put_float(r + 0x3c, 0);
            put_float(r + 0x48, 0);
            mp_put(r + 0x4c, 0);
            mp_put(r + 0x50, 0);
            return;
        }
        double dt = c->car.played_at > 0 ? (c->car.play - (double)p->ms) / 1000.0 : 0.0;
        dt = dt < -1.0 ? -1.0 : dt > 1.0 ? 1.0 : dt;
        pct = p->pct + p->rate * (float)dt;
        lap_pct = p->lap_pct + p->lap_rate * (float)dt;
        laps = p->laps;
        cp = race_is_drift() || race_is_duel() ? 0 : p->cp; /* drift and duel: cp carries points */
    }
    pct = pct < 0 ? 0 : pct > 99.99f ? 99.99f : pct;
    lap_pct = lap_pct < 0 ? 0 : lap_pct > 99.99f ? 99.99f : lap_pct;
    put_float(r + 0x3c, pct);
    put_float(r + 0x48, lap_pct);
    mp_put(r + 0x4c, laps);
    mp_put(r + 0x50, cp);
    if (c->id)
        race_splits(c, r, pct);
}

/* The gap column of the race list (DAL 0x3ec -> 004ca750): row A's time at the last whole percent both rows passed minus
 * row B's, from the rows' split times at +0x1d4 (100 floats, written by 00644150 from the game's own progress). Another
 * player's row has its progress from the network, so its splits were 0 or written at the game's wrong percent, and the
 * column read minutes ("-10:18.0"). Each percent they pass gets the row's race timer, here, as 00644150 would. */
static void race_splits(RaceCar *c, uint32_t r, float pct) {
    const int at = (int)pct;
    if (at <= c->split)
        return;
    float t = bits_float(get_u32(r + 0x184)); /* 00615960: the row's timer at +0x180, clock 0x00b4cfd8 */
    if (race_u8(r + 0x188))
        t += bits_float(get_u32(0x00b4cfd8u)) - bits_float(get_u32(r + 0x180));
    for (int k = c->split + 1; k <= at && k < 100; ++k)
        put_float(r + 0x1d4u + 4u * (uint32_t)k, t);
    c->split = at;
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
    const uint32_t flow = get_u32(RACE_FLOW);
    const int roam = flow == 6 && rs && player && !get_u32(rs + 0x6a14) &&
                     !get_u32(rs + 0x6a1c) && !mp_online_game();
    int racing = (rs && (get_u32(rs + 0x6a14) == 1 || get_u32(rs + 0x6a1c))) ||
                 (g_race_session && g_race_phase >= RACE_LAUNCHED) || mp_online_game();
    /* Keep race privacy through loading/results gaps until native free roam is
     * confirmed. 0x400 advertises these activity flags to new pages. A free-roam
     * player entering the front end retains the existing parked-car behavior. */
    if (!racing && !roam && (sh->flow_flags & 0x200u)) racing = 1;
    const int parked = flow == 3 && (sh->flow_flags & 0x800u);
    sh->flow_flags = flow | ((g_race_result_activity || race_drift_waiting()) ? 0x100u : 0) | 0x400u |
                     (racing ? 0x200u : 0) | ((roam || parked) && !racing ? 0x800u : 0);
    sh->drift_score = race_drift_score();
    sh->drift_failed = race_drift_failed();
    if (race_is_duel()) { /* CANYON DUEL TOGETHER: this player's duel points; 1 lost, 2 won once it is over */
        const uint32_t d = race_duel_report(rs, player);
        sh->drift_score = d & 0x3fffffffu;
        sh->drift_failed = d & RACE_DUEL_LOST ? 1u : d & RACE_DUEL_WON ? 2u : 0u;
    }
    sh->session = g_race_session;
    /* After a caught Start the menus show no screen race_menu_ready knows, and the car announced
     * in the lobby was 0: the others then saw a copy of their own car for the host. */
    sh->car = player ? mp_model_key(player) : race_menu_ready() || g_race_caught_key ? g_race_car_key : 0;
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
        /* RACE START: bit 0 passes through, bit 1 off the road; this player's own row: bit 0 while it started late
         * and still passes through any car. */
        o->spare = c ? (c->ghost ? 1u : 0u) | (c->left ? 2u : 0u) : 0u;
        if (o->me && g_race_late_start)
            for (int k = 0; k < g_race_cars_n; ++k)
                if (g_race_cars[k].ghost)
                    o->spare |= 1u;
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

/* CANYON DUEL TOGETHER (logs/duel-trucks-20261106). The game's duel is GRaceStatus + 0xbcb8 (CanyonRaceUpdate
 * 0062bd50, set up by 00645f40): two slots of 0x54 bytes at +0x30 and +0x84 (+0 points, +4 racer row, +8 its
 * vehicle, +0x18 crashed), the state at +0x2c (1 running; 3 crashed, 4 the chaser passed and held the lead,
 * 5 the leader got away: TOO SLOW) and 006278c0, which writes both racers' places from that state (00616a10).
 * In every game the duel is heat 1 with this player as the chaser and the other player as the leader, so each game
 * judges its own player against the other's progress (race_progress). Measured on the public m5 build:
 *   - when one game's duel ended (a crash, TOO SLOW, a win), its finish came here as a finish time, and
 *     race_finish_peer put that player at 100 %: this game's meter showed ~4,000 M and FALLING BEHIND, and this
 *     player lost TOO SLOW even when the other had crashed (both lost);
 *   - each game's points were its own player's only; the other's row had none.
 * Now the page sends each player's duel points with the progress, and the outcome with the finish (RacePeer.cp:
 * points, bit 31 lost, bit 30 won; done bit 1 = they left). The other player's points go into their slot, so the end
 * of the route (00616a10 compares the two slots' points) is judged the same in both games; the other player's end
 * decides this one at once (they lost or left: this player wins, state 4; they won: state 5), and their row is not
 * put at 100 % (their finish is written only once this player's own duel is over). */
/* (RACE_DUEL* are defined with the other addresses at the top.) */
/* g_race_duel_forced: the state this game was given by the other player's end, 0 = none */
static int race_is_duel(void) { return g_race_session && !g_race_encounter && g_race_type == 13u; }
/* The duel's slot of row `row`, or 0. */
static uint32_t race_duel_slot(uint32_t rs, int row) {
    for (uint32_t k = 0; k < 2 && row >= 0; ++k)
        if ((int)get_u32(rs + RACE_DUEL + 0x30u + 0x54u * k + 4u) == row)
            return rs + RACE_DUEL + 0x30u + 0x54u * k;
    return 0;
}
/* This player's duel is over (its state or its row). */
static int race_duel_over(uint32_t rs, int mine) {
    const uint32_t st = get_u32(rs + RACE_DUEL + 0x2cu);
    return (st > 1u) || (mine >= 0 && race_u8(race_row(rs, (uint32_t)mine) + 0x46));
}
/* What the page sends: this player's duel points, and once it is over bit 30 won or bit 31 lost. */
static uint32_t race_duel_report(uint32_t rs, uint32_t player) {
    const int mine = rs && player && race_is_duel() && race_u8(rs + RACE_DUEL) ? race_row_of(rs, player + 0x24) : -1;
    const uint32_t slot = mine >= 0 ? race_duel_slot(rs, mine) : 0;
    if (!slot)
        return 0;
    const float pts = bits_float(get_u32(slot));
    uint32_t out = pts > 0 && pts < 1e9f ? (uint32_t)pts & 0x3fffffffu : 0;
    if (race_duel_over(rs, mine)) {
        const uint32_t veh = get_u32(slot + 8);
        out |= (mp_call(RACE_DUEL_PLACE, rs + RACE_DUEL, &veh, 1) & 0xffu) == 1u ? RACE_DUEL_WON : RACE_DUEL_LOST;
    }
    return out;
}
/* The results' Standings and Stages (DAL 0x402 / 0x405 -> 004b4700 (out, row, heat, mode), RET 0x10): the game gives points
 * only to heat 1's chaser, the other row reads 0 (0x402) or minus the chaser's points (0x405). In a duel together each game
 * has its own player as the chaser, so the other player always showed 0 (logs/public-all2-20261107). Once this player's
 * race is over, each row reads its own slot's points: row 0 / 1 the two slots, rows 2 / 3 first and second place. */
static void race_duel_score(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t out = get_u32(cpu->esp + 4), row = get_u32(cpu->esp + 8), heat = get_u32(cpu->esp + 12);
    api->call_original(api, cpu->target, cpu);
    const uint32_t rs = race_status();
    if (!rs || !race_is_duel() || g_race_phase < RACE_DONE || !race_u8(rs + RACE_DUEL) || !(cpu->eax & 0xffu) || row > 3u ||
        (heat != 0u && heat != 0xffffffffu))
        return;
    const uint32_t duel = rs + RACE_DUEL, flip = race_u8(duel + 2) ? 1u : 0u;
    const uint32_t a = duel + 0x30u + 0x54u * flip, b = duel + 0x30u + 0x54u * (1u - flip);
    uint32_t slot = row == 0u ? a : b;
    if (row >= 2u) { /* as 004b4700: is the second slot's car first? */
        const uint32_t veh = get_u32(b + 8);
        const int b_first = (mp_call(RACE_DUEL_PLACE, duel, &veh, 1) & 0xffu) == 1u;
        slot = (row == 2u) == b_first ? b : a;
    }
    const float pts = bits_float(get_u32(slot));
    put_float(out, pts > 0 && pts < 1e9f ? pts : 0.0f);
}

/* The canyon meter's distance on the HUD (DAL 0x40a -> 004a6e00 (out, 3), RET 8; shown by 005e1c50 as "%0.0f %s", duel +0x104:
 * the leader's progress minus the chaser's). Each game has its own player as the chaser, so this player ahead read as
 * negative metres (user, 2026-11-07: first in a duel, the meter negative). A gap is shown as a distance; the game's own
 * rules still read the signed value, only this display getter changes. */
static void race_duel_meter(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t out = get_u32(cpu->esp + 4);
    api->call_original(api, cpu->target, cpu);
    if (race_is_duel() && (cpu->eax & 0xffu) && out)
        put_float(out, fabsf(bits_float(get_u32(out))));
}

/* DIAGNOSTIC: the race script's states (ChangeState -> 00624640 (name), cdecl) while in a race together. */
static void race_state_note(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)inv;
    (void)user;
    const uint32_t s = get_u32(cpu->esp + 4);
    char k[24] = "";
    for (uint32_t n = 0; s && n + 1 < sizeof k && race_u8(s + n); ++n)
        k[n] = (char)race_u8(s + n);
    if (g_race_session)
        race_log("core.nfsc: race: script state %s (phase %u)", k, g_race_phase);
}

/* No start scene in a Quick Race together (the flag girl's scene shows only one car, the user, 2026-11-07). The race script
 * plays it with NIS.Play (racestart, name, "Intro" or "Countdown", ...) = 00650db0 (cdecl); with no name that function's
 * own path says "scene over" at once, so the script's states run on as ever and the hold, the go and the 3-2-1 are
 * unchanged. The script warps the player to racestart itself when a race has no intro scene (Game.WarpPlayerToTrigger,
 * 0065b610 (trigger, 0)): that also moves the streaming and waits for the world. It decided before this hook, so it is done
 * here: without it the cars fell through a world not loaded (runs/g4). Not the outro; not drift, a duel or a world race, as
 * for the early cars (race_early_ok); offline and career are not in a session, so nothing changes. */
static void race_no_scene(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t trigger = get_u32(cpu->esp + 4), name = get_u32(cpu->esp + 8), kind = get_u32(cpu->esp + 12);
    char k[11] = "";
    for (uint32_t n = 0; kind && n + 1 < sizeof k && race_u8(kind + n); ++n)
        k[n] = (char)race_u8(kind + n);
    const char *keep = getenv("RECOMP_RACE_SCENE"); /* =1: the scene as ever */
    const int intro = !strcmp(k, "Intro"), skip = !(keep && *keep == '1') && name && (intro || !strcmp(k, "Countdown")) &&
                                                  race_early_ok() && g_race_page.session == g_race_session;
    if (skip) {
        race_log("core.nfsc: race: %s scene skipped", k);
        g_race_scene_skipped = 1;
        mp_put(cpu->esp + 8, 0);
    }
    api->call_original(api, cpu->target, cpu);
    if (skip && intro && trigger) {
        const uint32_t args[2] = {trigger, 0};
        mp_call(0x0065b610u, 0, args, 2);
    }
}

/* Once a frame in a duel together: the other player's points into their slot, and their end decides ours. */
static void race_duel_frame(uint32_t rs, uint32_t player) {
    if (!rs || !player || !race_is_duel() || !race_u8(rs + RACE_DUEL) || g_race_phase < RACE_RACING)
        return;
    const int mine = race_row_of(rs, player + 0x24);
    const uint32_t me = race_duel_slot(rs, mine);
    for (int i = 0; me && i < g_race_cars_n; ++i) {
        const RaceCar *c = &g_race_cars[i];
        const RacePeer *p = c->id ? race_peer(c->id) : NULL;
        const uint32_t them = p ? race_duel_slot(rs, (int)c->row) : 0;
        if (!them || them == me)
            continue;
        put_float(them, (float)(p->cp & 0x3fffffffu));
        put_float(race_row(rs, (uint32_t)c->row) + 0x144, (float)(p->cp & 0x3fffffffu)); /* their row's points: the Standings */
        const int left = (p->done & 2u) != 0, ended = (p->done & 1u) && p->time > 0;
        const int lost = left || (ended && (p->cp & RACE_DUEL_LOST)), won = !left && ended && (p->cp & RACE_DUEL_WON);
        const uint32_t st = get_u32(rs + RACE_DUEL + 0x2cu);
        if (!lost && !won)
            continue; /* still racing, or an older game's finish that does not say how it ended */
        if (!g_race_duel_forced && race_duel_over(rs, mine))
            continue; /* our own duel ended first: it stands */
        const int want = lost ? 4 : 5;
        if (st != (uint32_t)want) {
            mp_put(rs + RACE_DUEL + 0x2cu, (uint32_t)want);
            mp_call(RACE_DUEL_PLACES, rs + RACE_DUEL, NULL, 0);
            if (!g_race_duel_forced)
                race_log("core.nfsc: race: duel: player %u %s, so this player %s", c->id,
                         left ? "left" : lost ? "lost" : "won", lost ? "wins" : "loses");
        }
        g_race_duel_forced = want;
    }
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
    if ((g_race_page.flags & 12u) == 12u && g_race_session == g_race_page.session) g_race_aborting = 1;
    const uint32_t rs = race_status(), flow = get_u32(RACE_FLOW), player = mp_player();
    /* Releasing the loading hold precedes SetActivelyRacing. A world race can
     * still report roaming in that gap. Observe the active native state before
     * treating a later roaming state as an exit. */
    if (rs && g_race_session && get_u32(rs + 0x6a14) == 1u) g_race_active_seen = 1;
    /* Results also leave play mode 1. Keep ownership until actual free roam. */
    const int ended = rs && g_race_active_seen && ((g_race_page.flags & 4u)
        ? !get_u32(rs + 0x6a14) && !get_u32(rs + 0x6a1c)
        : get_u32(rs + 0x6a14) != 1u);
    const int abandoned = rs && g_race_aborting && g_race_phase >= RACE_SET_UP && !get_u32(rs + 0x6a1c);
    if (flow == 3 || !rs || ended || abandoned) {
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
    race_duel_frame(rs, player); /* CANYON DUEL TOGETHER */
    if (rs && g_race_cars_n)
        race_early_cars(); /* RACE START */
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
            race_car_left(c);
            const MpSlot *s = c->following ? race_slot(c->id, NULL) : NULL;
            if (s)
                mp_take(&c->car, s, now);
            /* CANYON DUEL TOGETHER: the other's finish is written once this player's own duel is over. */
            const int mine = race_is_duel() && player ? race_row_of(rs, player + 0x24) : -1;
            if (race_is_duel() && !race_duel_over(rs, mine))
                continue;
            for (uint32_t k = 0; k < g_race_page.count && k < RACE_PEERS; ++k) {
                const RacePeer *peer = &g_race_page.peers[k];
                if (peer->id == c->id && (peer->done & 1u) && peer->time > 0)
                    race_finish_peer(rs, c, peer->time);
            }
        }
        race_place_all(0);
        if (g_race_test && g_race_frame % 120 == 0)
            race_dump(rs);
    }
    /* Finished, and held at the results until the others finish: offline those show at once, but
     * here the game's after-race AI went on driving this player's car (reported 2026-11-04: it
     * "accelerates until the others come"). It brakes to a stop; a fall still falls. The AI's gas
     * stayed on, so the stopped car revved on the line (reported 2026-11-05): its controls are set
     * to no gas, full brake and handbrake, no nitrous, through the game's own setters (as
     * mp_drive.h's drv_apply; ai_fixes.h parks AI cars the same way). */
    if (rs && player && g_race_session && g_race_session == g_race_page.session &&
        (g_race_result_activity || race_drift_waiting())) {
        const uint32_t in = get_u32(player + DRV_INPUT);
        if (in && drv_method(in, 3) == DRV_GET_GAS && drv_method(in, 2) == DRV_GET_BRAKE &&
            drv_method(in, 7) == DRV_GET_HAND) {
            const uint32_t zero = 0, one = 0x3f800000u;
            mp_call(drv_method(in, 11), in, &zero, 1); /* SetControlGas */
            mp_call(drv_method(in, 12), in, &one, 1);  /* SetControlBrake */
            mp_call(drv_method(in, 14), in, &one, 1);  /* SetControlHandBrake */
            mp_call(drv_method(in, 13), in, &zero, 1); /* SetControlNOS */
        }
        const uint32_t body = get_u32(player + 0x70), state = mp_state(body);
        MpPose pose;
        if (state) {
            mp_read(state, &pose);
            if (mp_pose_ok(&pose)) {
                pose.v[0] *= 0.9f;
                pose.v[2] *= 0.9f;
                mp_write(body, state, &pose);
            }
        }
    }
    /* RACE START: a late start (g_race_late_start 1) waits behind the grid, on the slot the page gives it, until its
     * own GO starts its race timer (row +0x188); then it is on its way (2). */
    int late_wait = 0;
    if (rs && player && g_race_late_start == 1 && g_race_phase >= RACE_RACING && g_race_session) {
        const int mine = race_row_of(rs, player + 0x24);
        if (mine >= 0 && race_u8(race_row(rs, (uint32_t)mine) + 0x188)) {
            g_race_late_start = 2;
            race_log("core.nfsc: race: late start: GO");
        } else
            late_wait = mine >= 0;
    }
    /* With no start scene nothing takes the loading screen away before the script's own call after go: do it here, so the
     * wait is on the grid (0064ca70 = Game.HideLoadingScreen). */
    if (g_race_scene_skipped == 1 && g_race_phase == RACE_HOLDING) {
        g_race_scene_skipped = 2;
        mp_call(0x0064ca70u, 0, NULL, 0);
    }
    /* A shown peer's car too: while loading, the game put every racer on its own start formation (this player first, then
     * the rows), which differs in every game; only this player's car went back on its slot. So a READY car stood 3-6 m
     * off its slot, behind the camera in one game and under it in the other (logs/public-all-20261107 runs/g3). */
    if (rs && g_race_phase == RACE_HOLDING && g_race_session && !g_race_test) {
        for (int i = 0; i < g_race_cars_n; ++i) {
            const RaceCar *c = &g_race_cars[i];
            const RacePeer *peer = c->id && c->early && c->car.simable ? race_peer(c->id) : NULL;
            const uint32_t body = peer ? get_u32(c->car.simable - 0x24u + 0x70u) : 0, state = body ? mp_state(body) : 0;
            float pos[3], dir[3];
            MpPose pose;
            if (!state || !race_grid(g_race_base + peer->grid, pos, dir))
                continue;
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
    /* This player's own grid slot while everyone loads. */
    if (rs && player && (g_race_phase == RACE_HOLDING || late_wait) && g_race_session && !g_race_test) {
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
    if (rs && g_race_cars_n && g_race_phase >= RACE_RACING)
        race_ghosts(player); /* RACE START */
    if (rs && player && g_race_phase == RACE_RACING) {
        const int mine = race_row_of(rs, player + 0x24);
        if (mine >= 0 && race_u8(race_row(rs, (uint32_t)mine) + 0x46)) {
            g_race_phase = RACE_DONE;
            race_log("core.nfsc: race: finished in %.3f s, place %u",
                     bits_float(get_u32(race_row(rs, (uint32_t)mine) + 0x184)),
                     get_u32(race_row(rs, (uint32_t)mine) + 0x34));
        }
    }
    if(race_is_drift()) {race_drift_sync(rs);race_drift_results();}
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
    race_tags(); /* RACE START */
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
        MpLabel *l = mp_label(sh, slot);
        l->x = s[0];
        l->y = s[1];
        l->depth = depth > 0.5f && s[0] > -0.2f && s[0] < 1.2f && s[1] > -0.2f && s[1] < 1.2f ? depth : 0.0f;
    }
}

/* THE LIST ON THE RIGHT, ROWS 5-8 (logs/duel-trucks-20261106: "the last players are ALWAYS greyed out" in a race
 * together). The race list is Hud_LeaderBoard (constructor 005e6520, vtable 0x009d6ed4; update 005db4f0): 8 rows of FE
 * objects RACER_%d (+0x34), RACER_POSITION_%d (+0x54, the name or gap) and POSITION_%d (+0x74, the place). Every
 * row in use gets the same "other racer" script (0x6ebbfb68), and that script's colour track fades the lower rows:
 * alpha 255 for rows 1-4, then 120, 100, 80, 60 for rows 5-8 (read from the live objects' colour, FE object + 0x2c ->
 * B,G,R,A; three real games and three relay racers, 2026-11-06). Offline a race has at most four racers, so it never
 * showed; with five players or more, places 5+ looked greyed out though they were racing, this player's own row too
 * when it was 5th or lower. In a race together the rows in use (fewer than the racer count) are drawn at full alpha
 * after every FE update. */
#define RACE_BOARD_CTOR 0x005e6520u   /* Hud_LeaderBoard::Hud_LeaderBoard, thiscall */
#define RACE_BOARD_VTABLE 0x009d6ed4u
#define RACE_FE_TEXT_VTABLE 0x009d7644u /* the rows' text objects */
static uint32_t g_race_board;
static void race_board_made(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)inv;
    (void)user;
    g_race_board = cpu->ecx;
}
static void race_board_rows(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)cpu;
    (void)inv;
    (void)user;
    const uint32_t rs = race_status(), b = g_race_board;
    if (!b || !rs || !g_race_session || g_race_session != g_race_page.session || g_race_phase < RACE_RACING ||
        get_u32(b) != RACE_BOARD_VTABLE)
        return;
    const uint32_t n = race_count(rs);
    for (uint32_t i = 4; i < n && i < 8; ++i)
        for (uint32_t k = 0; k < 2; ++k) {
            const uint32_t obj = get_u32(b + (k ? 0x74u : 0x54u) + 4u * i), data = obj ? get_u32(obj + 0x2c) : 0;
            if (obj && get_u32(obj) == RACE_FE_TEXT_VTABLE && data && get_u32(data + 0xc) && get_u32(data + 0xc) < 255u)
                mp_put(data + 0xc, 255u);
        }
}

static PopModStatus race_hook(uint32_t addr, PopHookFn fn, int32_t mode, int slot) {
    PopModStatus s = g_api->hook_install(g_api, addr, fn, mode, 0, &g_race_hooks[slot]);
    if (s != POP_OK)
        race_log("core.nfsc: race: hook %08x failed (%d)", addr, (int)s);
    return s;
}

#include "drift_race.h"

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
    sh->tags_magic = RACE_TAGS;
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
    race_hook(RACE_BOARD_CTOR, race_board_made, POP_HOOK_BEFORE, 12);
    race_hook(MP_FE_SERVICE, race_board_rows, POP_HOOK_AFTER, 13);
    race_hook(0x004b4700u, race_duel_score, POP_HOOK_REPLACE, 14); /* CANYON DUEL TOGETHER: the results' scores */
    race_hook(0x004a6e00u, race_duel_meter, POP_HOOK_REPLACE, 15); /* and the meter's distance, never negative */
    race_hook(0x00650db0u, race_no_scene, POP_HOOK_REPLACE, 16);    /* NIS.Play: no start scene in a race together */
    race_hook(0x00624640u, race_state_note, POP_HOOK_BEFORE, 17);
    race_drift_install();
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
