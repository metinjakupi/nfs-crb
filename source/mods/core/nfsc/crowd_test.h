/* crowd_test.h - TEST ONLY: how many extra cars the game holds (included by nfsc.c after mp.h and cops.h).
 *
 * Off unless the URL has RECOMP_CROWD=<max> (getenv, as mp.h's RECOMP_MP_TEST). In career free roam,
 * CT_START frames after the player's car is in the world, the mod follows the nearest of the game's
 * own traffic cars (driver class 1: it keeps to a lane) and records the road it drives (a crumb every
 * CT_CRUMB metres, with the car's orientation; a guide that stands still for CT_STUCK frames is
 * dropped and the road recorded again from the next one), and when CT_TRAIL_LEN metres are recorded
 * puts the player's car at the start of it, standing. (Driving the player's car there by keys ran it
 * into walls.) Then every CT_EVERY ms it builds one more car (mp.h's mp_build) on that road ahead
 * (CT_SPOT_ROWS rows CT_SPOT_STEP metres apart from CT_SPOT_FROM on: 30..150 m), in one of three
 * lanes, never within CT_CLEAR of another car, facing the way the road was driven, until
 * PVehicle::Construct 006daa80 refuses, makes room by freeing a car (below), or <max> cars are built.
 * Each attempt logs one line (crowd_line):
 *   - list 0's count [0x00a9f164] (the 0x1e gate of 006d86f0 at 006d87f8);
 *   - the car memory: the sum 006d41d0 returns (every car's estimate [car + 0x1a8], in bytes; a car
 *     whose model's memory class ([0x00b74ccc] + model index * 0xd0 + 0x94) is 1 or 2, traffic or
 *     police, counted once per model) against the budget [0x00b77084] (007b11e0). 006d86f0 refuses
 *     when that sum plus the new cars' estimates is above the budget, or list 0's count plus the new
 *     cars is above 30, and its cache (the params' + 0x30; mp_build passes GRaceStatus) cannot free
 *     enough. When the cache can, it frees cars and Construct succeeds: seen 2026-11-05 from the 9th
 *     AI racer on (list 0 and the sum stayed put, and the freed PVehicle came back as the new car), so
 *     a build after which one of our cars is gone, or list 0 grew by less than one, counts as the gate
 *     and ends the attempts too;
 *   - the rigid bodies [0x00aaa248] (00761270 lets 64 in);
 *   - the free composite skin ids: the bits of [0x00a732a4];
 *   - the counts of the render list 0x00b77db0, the car sound list 0x00a8de88 and the ISimable list
 *     0x00a83ae8 (each count at +0xc), and the fullest of every fixed 30-entry list in CT_LISTS;
 *   - the frame time: the median and 95th percentile of the intervals between frames (FE service,
 *     once a game frame) over the last CT_WINDOW ms, and the frames a second.
 * Which gate refused is read from two call-site hooks inside Construct: 006d86f0 (count and memory,
 * returns AL, called at 006dad59) and 00761270 (rigid bodies, called at 006dadbd); a refusal before
 * either is the model (006c08f0) or the params. Then the cars are left driving for CT_HOLD ms, a
 * line every CT_HOLD_EVERY ms.
 *
 * The 30-entry lists (logs/vehicle-limit-20261105/findings.md) are never overfilled: before each
 * attempt the mod stops when any of them holds 30 or the rigid bodies reach CT_BODIES_STOP.
 * Cars that drive more than CT_FAR from the player are put back on a free place of the road (off
 * with RECOMP_CROWD_KEEP=0), so the count measured is the count near the player.
 * Then (off with RECOMP_CROWD_AB=0) CT_AB_PERIODS periods of CT_HOLD_EVERY ms with our cars driving
 * and parked (SetPhysicsMode 0: not simulated, not drawn) by turns: their cost under the same load.
 * Other switches: RECOMP_CROWD_TRAFFIC=0 (from the moment the player is on the road the traffic
 * manager's spawn check 00444d90 answers "no spawn", and the first car waits until list 0 holds 3 or
 * CT_CLEAR_MS has passed; it did not work on 2026-11-05: list 0 stayed at 16 for the 90 s and traffic
 * still came: this AFTER hook's answer does not reach the caller past traffic.h's REPLACE, open),
 * RECOMP_CROWD_CLASS (driver class, default CT_CLASS), RECOMP_CROWD_LITE=1 (params flag 0x20,
 * RacingLite: a smaller memory estimate), RECOMP_CROWD_POOL=<KB> (written to the Cars pool
 * size [0x00a62c48] at mod start, before the game sizes its pools from it; 006bbdf0 compares it with
 * the budget at each world load and resizes the pool through 007e4910). Cars are never removed: a
 * Kill of a copy ended the game now and then (cops.h, TAKING THEM AWAY); the world unload removes them.
 */

#define CT_LIST0_COUNT 0x00a9f164u /* list 0 of the 12 vehicle lists at 0x00a9f158 */
#define CT_MEMORY_SUM 0x006d41d0u  /* cdecl (): the cars' memory estimate, bytes */
#define CT_MEMORY_BUDGET 0x00b77084u
#define CT_BODIES 0x00aaa248u
#define CT_SKINS_FREE 0x00a732a4u
#define CT_POOL_KB 0x00a62c48u     /* the "Cars" pool, KB */
#define CT_POOL_ALT_KB 0x00a62c4cu /* used instead when 006999c0 returns 1 (006bbe85) */
#define CT_CACHE_GATE 0x006d86f0u
#define CT_CACHE_RET 0x006dad5eu
#define CT_BODY_GATE 0x00761270u
#define CT_BODY_RET 0x006dadc2u
#define CT_RENDER_LIST 0x00b77db0u
#define CT_SOUND_LIST 0x00a8de88u
#define CT_SIMABLE_LIST 0x00a83ae8u
#define CT_CLASS 3u          /* AI racer: drives on its own in free roam; traffic (1) mostly stood (RESULT.md) */
#define CT_LITE 0x20u        /* VehicleParams + 0x38: RacingLite */
#define CT_EVERY 2000.0      /* ms between attempts */
#define CT_WINDOW 2000.0     /* ms of frames each frame-time figure covers */
#define CT_HOLD 60000.0      /* ms the cars are left driving after the last attempt */
#define CT_HOLD_EVERY 5000.0 /* a line this often meanwhile */
#define CT_AB_PERIODS 12     /* then CT_HOLD_EVERY periods, our cars driving and parked by turns */
#define CT_CLEAR_MS 90000.0  /* RECOMP_CROWD_TRAFFIC=0: at most this long for the traffic to go */
#define CT_TRAFFIC_SPAWN 0x00444d90u /* the traffic manager's "no spawn now" check, AL (traffic.h) */
#define CT_CRUMB 4.0f        /* metres between recorded points of the road */
#define CT_CRUMBS 64
#define CT_START 300         /* frames in the world before the road is looked for */
#define CT_STUCK 180         /* frames a guide may stand still */
#define CT_TRAIL_LEN 170.0f  /* metres of road recorded before the cars come */
#define CT_SPOT_FROM 30.0f   /* metres along the road from the player */
#define CT_SPOT_STEP 8.0f
#define CT_SPOT_ROWS 16      /* 30..150 m */
#define CT_LANE 2.4f         /* metres beside the recorded line: the other two lanes */
#define CT_CLEAR 7.0f        /* metres: no car is built this near another */
#define CT_FAR 220.0f        /* metres: a car this far from the player is put back on the road */
#define CT_BODIES_STOP 62u
#define CT_CARS 128
#define CT_FRAMES 1024

enum { CT_WAIT, CT_RECORD, CT_SETTLE, CT_QUIET, CT_BUILD, CT_HOLDING, CT_AB, CT_DONE };

typedef struct CtCrumb {
    MpPose pose;
    float right[3], along; /* the car's right row; metres from the first crumb */
} CtCrumb;

/* Every fixed 30-entry list behind a car (findings §1); count at +0xc, capacity at +8. */
static const uint32_t CT_LISTS[] = {
    0x00a9f158u, 0x00a9f1e0u, 0x00a9f268u, 0x00a9f2f0u, 0x00a9f378u, 0x00a9f400u, 0x00a9f488u, 0x00a9f510u,
    0x00a9f598u, 0x00a9f620u, 0x00a9f6a8u, 0x00a9f730u, 0x00a83ae8u, 0x00b77db0u, 0x00a8de88u, 0x00a8df10u,
    0x00a8df98u, 0x00a8e020u, 0x00aa2610u, 0x00aa7710u};

/* Racer models; a model counts as loaded when Construct gets past the model without spooling it. */
static const char *const CT_MODELS[] = {"bmwm3gtre46", "911turbo", "murcielago", "rx8", "supra", "skyline",
                                        "is300", "eclipsegt", "lancerevo9", "camaro", "mustanggt", "gto",
                                        "charger69", "chevelle", "corvettez06", "350z", "gallardo", "rx7",
                                        "imprezawrxsti", "cuda", "db9", "sl65", "clio", "golf"};
#define CT_MODEL_COUNT (int)(sizeof CT_MODELS / sizeof CT_MODELS[0])

static uint32_t g_ct_hooks[4];
static uint32_t g_ct_name;    /* 0x20 guest bytes for a model name */
static int g_ct_max;          /* RECOMP_CROWD; 0 = off */
static uint32_t g_ct_bodies_stop = CT_BODIES_STOP; /* RECOMP_CROWD_BODIES (a build with more rigid bodies) */
static uint32_t g_ct_class, g_ct_flags;
static int g_ct_keep = 1, g_ct_traffic = 1, g_ct_ab = 1, g_ct_period;
static int g_ct_state;
static CtCrumb g_ct_trail[CT_CRUMBS];
static int g_ct_crumbs;
static uint32_t g_ct_car[CT_CARS];
static int g_ct_cars, g_ct_attempts, g_ct_recycled, g_ct_settles, g_ct_seen, g_ct_still;
static uint32_t g_ct_guide; /* the traffic car whose road is recorded */
static double g_ct_at;         /* ms: the phase's next step */
static double g_ct_hold_from;
static double g_ct_frame[CT_FRAMES];
static unsigned g_ct_frames;
static int8_t g_ct_loaded[CT_MODEL_COUNT]; /* 0 not tried, 1 loaded, -1 not loaded */
static uint32_t g_ct_key[CT_MODEL_COUNT];
static int g_ct_model;
/* Set by the call-site hooks during our own Construct: 1 passed, 0 refused, -1 not reached. */
static int g_ct_building, g_ct_cache_ok, g_ct_body_ok;

static uint32_t ct_count(uint32_t list) {
    return get_u32(list + 0xc);
}

static double ct_dist(const float a[3], const float b[3]) {
    const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return sqrt(x * x + y * y + z * z);
}

static int ct_car_pose(uint32_t car, MpPose *out) {
    const uint32_t state = mp_state(get_u32(car + 0x70));
    if (!state)
        return 0;
    mp_read(state, out);
    return 1;
}

static int ct_cmp(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* Frame time over the last CT_WINDOW ms: median and 95th percentile (ms), and frames a second. */
static void ct_frame_stats(double now, double *p50, double *p95, double *fps) {
    static double gap[CT_FRAMES];
    int n = 0;
    double first = now;
    for (unsigned i = 1; i < CT_FRAMES && i < g_ct_frames; ++i) {
        const double t = g_ct_frame[(g_ct_frames - i) % CT_FRAMES], before = g_ct_frame[(g_ct_frames - i - 1) % CT_FRAMES];
        if (now - before > CT_WINDOW)
            break;
        gap[n++] = t - before;
        first = before;
    }
    *p50 = *p95 = *fps = 0.0;
    if (!n)
        return;
    qsort(gap, (size_t)n, sizeof gap[0], ct_cmp);
    *p50 = gap[n / 2];
    *p95 = gap[(int)ceil(0.95 * n) - 1];
    *fps = now > first ? n * 1000.0 / (now - first) : 0.0;
}

/* The fullest fixed list, and whether any holds 30 (or its own capacity). */
static uint32_t ct_fullest(int *full) {
    uint32_t most = 0;
    *full = 0;
    for (size_t i = 0; i < sizeof CT_LISTS / sizeof CT_LISTS[0]; ++i) {
        const uint32_t n = ct_count(CT_LISTS[i]), cap = get_u32(CT_LISTS[i] + 8);
        if (n > most)
            most = n;
        if (cap ? n >= cap : n >= 30u) /* a list carlimit.h moved: its own capacity */
            *full = 1;
    }
    return most;
}

/* Our cars still in the car list, how many move faster than 2 m/s, and their mean distance. */
static int ct_survey(const float here[3], int *moving, double *mean) {
    int alive = 0;
    *moving = 0;
    *mean = 0.0;
    for (int i = 0; i < g_ct_cars; ++i) {
        MpPose p;
        if (!g_ct_car[i] || !mp_listed(g_ct_car[i] - 0x24) || !ct_car_pose(g_ct_car[i] - 0x24, &p))
            continue; /* gone (freed by the game); counted out */
        ++alive;
        *mean += ct_dist(p.p, here);
        if (sqrt(p.v[0] * p.v[0] + p.v[1] * p.v[1] + p.v[2] * p.v[2]) > 2.0)
            ++*moving;
    }
    if (alive)
        *mean /= alive;
    return alive;
}

static void crowd_line(const char *head, const float here[3], double now) {
    double p50, p95, fps, mean;
    int moving, full;
    const int alive = ct_survey(here, &moving, &mean);
    const uint32_t sum = mp_call(CT_MEMORY_SUM, 0, NULL, 0), budget = get_u32(CT_MEMORY_BUDGET);
    const uint32_t skins = (uint32_t)__builtin_popcount(get_u32(CT_SKINS_FREE)), most = ct_fullest(&full);
    ct_frame_stats(now, &p50, &p95, &fps);
    char line[400];
    snprintf(line, sizeof line,
             "core.nfsc: crowd: %s | ours %d (moving %d, mean %.0f m) | list0 %u | memory %u/%u KB | bodies %u | "
             "skins free %u | render %u sound %u simable %u | fullest list %u | frame p50 %.1f p95 %.1f ms, %.1f fps",
             head, alive, moving, mean, get_u32(CT_LIST0_COUNT), sum >> 10, budget >> 10, get_u32(CT_BODIES), skins,
             ct_count(CT_RENDER_LIST), ct_count(CT_SOUND_LIST), ct_count(CT_SIMABLE_LIST), most, p50, p95, fps);
    log_line(line);
}

/* The cars of the car list by driver class (0 player, 1 traffic, 2 police, 3 racer, 4 none/trailer,
 * 7 remote), so a log shows who holds list 0 before ours come. */
static void ct_classes(void) {
    unsigned n[10] = {0}, cars = 0;
    uint32_t node = get_u32(MP_CARS);
    for (int i = 0; node && node != MP_CARS && i < 256; ++i, node = get_u32(node), ++cars)
        ++n[get_u32(node - 0xd0 + 0x148) < 9u ? get_u32(node - 0xd0 + 0x148) : 9u];
    char line[200];
    snprintf(line, sizeof line,
             "core.nfsc: crowd: car list %u: class 0 %u, 1 %u, 2 %u, 3 %u, 4 %u, 5 %u, 6 %u, 7 %u, 8 %u, other %u", cars,
             n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7], n[8], n[9]);
    log_line(line);
}

/* The place `along` metres down the recorded road, `lane` metres to its right; v = the unit
 * direction the player drove there. */
static void ct_spot(float along, float lane, MpPose *out) {
    int i = 0;
    while (i + 2 < g_ct_crumbs && g_ct_trail[i + 1].along <= along)
        ++i;
    const CtCrumb *a = &g_ct_trail[i], *b = &g_ct_trail[i + 1];
    const float span = b->along - a->along, t = span > 0.0f ? (along - a->along) / span : 0.0f;
    const float *v = a->pose.v, len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    *out = a->pose;
    for (int k = 0; k < 3; ++k) {
        out->p[k] = a->pose.p[k] + (b->pose.p[k] - a->pose.p[k]) * t + a->right[k] * lane;
        out->v[k] = len > 0.0f ? v[k] / len : 0.0f; /* the way the road was driven, unit length */
        out->w[k] = 0.0f;
    }
}

/* Whether no car of the world is within CT_CLEAR of p. */
static int ct_clear(const float p[3]) {
    uint32_t node = get_u32(MP_CARS);
    for (int n = 0; node && node != MP_CARS && n < 256; ++n, node = get_u32(node)) {
        MpPose c;
        if (ct_car_pose(node - 0xd0, &c) && ct_dist(c.p, p) < CT_CLEAR)
            return 0;
    }
    return 1;
}

/* A free place on the road, spread along it: rows in a stride of 5 over 16, centre lane first. */
static int ct_free_spot(MpPose *out) {
    static int next;
    const float lanes[3] = {0.0f, CT_LANE, -CT_LANE};
    for (int tried = 0; tried < CT_SPOT_ROWS * 3; ++tried) {
        const int k = (next + tried) % (CT_SPOT_ROWS * 3);
        const float along = CT_SPOT_FROM + CT_SPOT_STEP * (float)((k * 5) % CT_SPOT_ROWS);
        if (along > g_ct_trail[g_ct_crumbs - 1].along)
            continue;
        ct_spot(along, lanes[k / CT_SPOT_ROWS], out);
        if (ct_clear(out->p)) {
            next = k + 1;
            return 1;
        }
    }
    return 0;
}

static void ct_cache_done(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)inv;
    (void)user;
    if (g_ct_building)
        g_ct_cache_ok = (cpu->eax & 0xffu) != 0;
}

static void ct_body_done(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)inv;
    (void)user;
    if (g_ct_building)
        g_ct_body_ok = (cpu->eax & 0xffu) != 0;
}

/* One Construct of model key (0 = the player's) at spot; the car's ISimable or 0. */
static uint32_t ct_construct(uint32_t player, uint32_t key, const MpPose *spot) {
    g_ct_building = 1;
    g_ct_cache_ok = g_ct_body_ok = -1;
    const uint32_t car = mp_build(player, g_ct_class, key, spot->p, 2u | 8u | g_ct_flags, 0);
    g_ct_building = 0;
    if (car) {
        MpPose at = *spot;
        memset(at.v, 0, sizeof at.v);
        mp_place(car, &at); /* the road's way, not the player's */
    }
    return car;
}

/* The next racer model this game has loaded (trying the untried ones on the way), or 0. */
static uint32_t ct_next_model(int *index) {
    for (int tried = 0; tried < CT_MODEL_COUNT; ++tried) {
        const int i = (g_ct_model + tried) % CT_MODEL_COUNT;
        if (g_ct_loaded[i] < 0)
            continue;
        if (!g_ct_key[i]) {
            uint32_t k = 0;
            for (; CT_MODELS[i][k] && k < 0x1f; ++k)
                g_api->guest_write_u8(g_api, g_ct_name + k, (uint8_t)CT_MODELS[i][k]);
            g_api->guest_write_u8(g_api, g_ct_name + k, 0);
            g_ct_key[i] = mp_call(MP_STRING_KEY, 0, &g_ct_name, 1);
            if (!g_ct_key[i]) {
                g_ct_loaded[i] = -1;
                continue;
            }
        }
        g_ct_model = i + 1;
        *index = i;
        return g_ct_key[i];
    }
    *index = -1;
    return 0;
}

/* Which of 006d86f0's two tests a new car of our last one's estimate fails, from list 0's count and the
 * memory sum as they were before the build. */
static const char *ct_gate_by(uint32_t list0, uint32_t used) {
    const uint32_t budget = get_u32(CT_MEMORY_BUDGET);
    const uint32_t need = g_ct_cars ? get_u32(g_ct_car[g_ct_cars - 1] - 0x24 + 0x1a8) : 0;
    const int count = list0 + 1u > g_mp_list0_max, memory = need && used + need > budget;
    return count && memory ? "count and memory (006d86f0)"
           : count         ? "car count (list 0 at its limit)"
           : memory        ? "car memory (006d86f0, budget [0x00b77084])"
                           : "006d86f0 (neither count nor memory by our reading)";
}

/* Which gate refused our last Construct. */
static const char *ct_gate(void) {
    if (g_ct_cache_ok < 0)
        return "before the gates (model or params)";
    if (!g_ct_cache_ok)
        return ct_gate_by(get_u32(CT_LIST0_COUNT), mp_call(CT_MEMORY_SUM, 0, NULL, 0));
    if (g_ct_body_ok == 0)
        return "rigid bodies (00761270)";
    return "after the gates (allocation or constructor)";
}

static void ct_attempt(uint32_t player, const float here[3], double now) {
    int full;
    ct_fullest(&full);
    char head[200];
    if (full || get_u32(CT_BODIES) >= g_ct_bodies_stop || g_ct_cars >= g_ct_max || g_ct_cars >= CT_CARS) {
        snprintf(head, sizeof head, "stop before attempt %d: %s", g_ct_attempts + 1,
                 full ? "a 30-entry list is full (not overfilled)"
                 : get_u32(CT_BODIES) >= g_ct_bodies_stop ? "rigid bodies near their limit"
                                                         : "RECOMP_CROWD cars built");
        crowd_line(head, here, now);
        g_ct_state = CT_HOLDING;
        g_ct_hold_from = now;
        g_ct_at = now + CT_HOLD_EVERY;
        return;
    }
    MpPose spot;
    if (!ct_free_spot(&spot)) {
        log_line("core.nfsc: crowd: no free place on the road; next try in 2 s");
        return;
    }
    /* What 006d86f0 will see: list 0's count and the memory sum, before the build. */
    const uint32_t list0 = get_u32(CT_LIST0_COUNT), memory = mp_call(CT_MEMORY_SUM, 0, NULL, 0);
    /* A model not loaded is refused before the gates; that is not an attempt, the next one is tried. */
    int index = -1;
    uint32_t key = ct_next_model(&index), car = 0;
    while (key) {
        car = ct_construct(player, key, &spot);
        if (car || g_ct_cache_ok >= 0)
            break;
        if (!g_ct_loaded[index]) {
            char line[96];
            snprintf(line, sizeof line, "core.nfsc: crowd: model %s not loaded", CT_MODELS[index]);
            log_line(line);
        }
        g_ct_loaded[index] = -1;
        key = ct_next_model(&index);
    }
    if (!key) {
        index = -1;
        car = ct_construct(player, 0, &spot);
    } else if (car) {
        g_ct_loaded[index] = 1;
    }
    ++g_ct_attempts;
    if (car) {
        /* 006d86f0 makes room through the params' vehicle cache (GRaceStatus) instead of refusing: a car
         * it frees leaves the car list, and its PVehicle can come back at once as the new car. */
        int ours = 0;
        for (int i = 0; i < g_ct_cars; ++i)
            if (g_ct_car[i] == car || !mp_listed(g_ct_car[i] - 0x24)) {
                g_ct_car[i--] = g_ct_car[--g_ct_cars];
                ++ours;
            }
        const int others = (int)list0 + 1 - ours - (int)get_u32(CT_LIST0_COUNT);
        g_ct_car[g_ct_cars++] = car;
        snprintf(head, sizeof head, "attempt %d car (%s, class %u%s, estimate %u KB, %.0f m away)%s", g_ct_attempts,
                 index >= 0 ? CT_MODELS[index] : "player's model", g_ct_class, g_ct_flags & CT_LITE ? ", lite" : "",
                 get_u32(car - 0x24 + 0x1a8) >> 10, ct_dist(spot.p, here), ours || others > 0 ? " FREED" : "");
        crowd_line(head, here, now);
        if (!ours && others <= 0)
            return;
        char line[200];
        snprintf(line, sizeof line,
                 "core.nfsc: crowd: the gate made room instead of refusing: the vehicle cache freed %d of our cars "
                 "and %d other (list 0 was %u); %s",
                 ours, others > 0 ? others : 0, list0, ct_gate_by(list0, memory));
        log_line(line);
        g_ct_state = CT_HOLDING;
        g_ct_hold_from = now;
        g_ct_at = now + CT_HOLD_EVERY;
        return;
    }
    snprintf(head, sizeof head, "attempt %d refused (%s): %s", g_ct_attempts,
             index >= 0 ? CT_MODELS[index] : "player's model", ct_gate());
    crowd_line(head, here, now);
    g_ct_state = CT_HOLDING;
    g_ct_hold_from = now;
    g_ct_at = now + CT_HOLD_EVERY;
}

/* Our cars that drove beyond CT_FAR are put back on a free place of the road, at their speed
 * (at most 20 m/s) along it; checked every 30th frame. */
static void ct_keep_near(const float here[3]) {
    for (int i = 0; g_ct_keep && i < g_ct_cars; ++i) {
        MpPose p, spot;
        if (!g_ct_car[i] || !mp_listed(g_ct_car[i] - 0x24) || !ct_car_pose(g_ct_car[i] - 0x24, &p) ||
            ct_dist(p.p, here) < CT_FAR || !ct_free_spot(&spot))
            continue;
        float speed = sqrtf(p.v[0] * p.v[0] + p.v[1] * p.v[1] + p.v[2] * p.v[2]);
        if (speed > 20.0f)
            speed = 20.0f;
        for (int k = 0; k < 3; ++k)
            spot.v[k] *= speed;
        mp_place(g_ct_car[i], &spot);
        ++g_ct_recycled;
    }
}

static int ct_ours(uint32_t car) {
    for (int i = 0; i < g_ct_cars; ++i)
        if (g_ct_car[i] == car + 0x24)
            return 1;
    return 0;
}

/* The nearest traffic car of the game's own (driver class 1, not ours) within 300 m, or 0. */
static uint32_t ct_find_guide(const float here[3]) {
    uint32_t node = get_u32(MP_CARS), best = 0;
    double nearest = 300.0;
    for (int n = 0; node && node != MP_CARS && n < 256; ++n, node = get_u32(node)) {
        const uint32_t car = node - 0xd0;
        MpPose p;
        if (get_u32(car + 0x148) != 1u || ct_ours(car) || !ct_car_pose(car, &p) || ct_dist(p.p, here) >= nearest)
            continue;
        nearest = ct_dist(p.p, here);
        best = car;
    }
    return best;
}

/* One more crumb of the guide's road when it has gone CT_CRUMB metres; 0 once the guide is lost. */
static int ct_record(const float here[3]) {
    MpPose now_pose;
    if (!g_ct_guide || !mp_listed(g_ct_guide) || get_u32(g_ct_guide + 0x148) != 1u ||
        !ct_car_pose(g_ct_guide, &now_pose) || !mp_pose_ok(&now_pose)) {
        g_ct_guide = ct_find_guide(here);
        g_ct_crumbs = g_ct_still = 0;
        if (g_ct_guide) {
            char line[96];
            snprintf(line, sizeof line, "core.nfsc: crowd: recording the road of traffic car %08x", g_ct_guide);
            log_line(line);
        }
        return 0;
    }
    const float v2 = now_pose.v[0] * now_pose.v[0] + now_pose.v[1] * now_pose.v[1] + now_pose.v[2] * now_pose.v[2];
    g_ct_still = v2 < 1.0f ? g_ct_still + 1 : 0;
    if (g_ct_still > CT_STUCK) {
        g_ct_guide = 0;
        return 0;
    }
    const uint32_t state = mp_state(get_u32(g_ct_guide + 0x70));
    const float last = g_ct_crumbs ? (float)ct_dist(now_pose.p, g_ct_trail[g_ct_crumbs - 1].pose.p) : CT_CRUMB;
    if (last < CT_CRUMB || g_ct_crumbs >= CT_CRUMBS)
        return 1;
    CtCrumb *c = &g_ct_trail[g_ct_crumbs];
    c->pose = now_pose;
    for (int k = 0; k < 3; ++k)
        c->right[k] = bits_float(get_u32(state + 0x70 + 4u * k));
    c->along = g_ct_crumbs ? g_ct_trail[g_ct_crumbs - 1].along + last : 0.0f;
    ++g_ct_crumbs;
    return 1;
}

/* All our cars parked the game's way (SetPhysicsMode 0: not simulated, not drawn; still counted in
 * every list and the memory sum) or active again (1). */
static void ct_park(uint32_t mode) {
    for (int i = 0; i < g_ct_cars; ++i)
        if (mp_listed(g_ct_car[i] - 0x24))
            mp_call(MP_SET_PHYSICS, g_ct_car[i] - 0x24 + 0xb8, &mode, 1);
}

/* RECOMP_CROWD_TRAFFIC=0: once the player is on the road, the traffic manager spawns no more. */
static void ct_traffic_spawn(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)inv;
    (void)user;
    if (!g_ct_traffic && g_ct_state >= CT_QUIET)
        cpu->eax = (cpu->eax & 0xffffff00u) | 1u;
}

static void ct_service(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)cpu;
    (void)inv;
    (void)user;
    const double now = mp_now();
    const uint32_t player = mp_player();
    MpPose me;
    if (!player || !ct_car_pose(player, &me) || !mp_pose_ok(&me)) {
        if (g_ct_state != CT_WAIT)
            log_line("core.nfsc: crowd: the player left the world; the test starts again in the next one");
        g_ct_state = CT_WAIT;
        g_ct_crumbs = g_ct_cars = g_ct_attempts = g_ct_seen = 0;
        g_ct_guide = 0;
        g_ct_frames = 0;
        return;
    }
    if (mp_call(MP_IS_PAUSED, 0, NULL, 0) & 0xffu) {
        g_ct_frames = 0; /* a pause is not a frame time */
        return;
    }
    g_ct_frame[g_ct_frames++ % CT_FRAMES] = now;
    const float speed = sqrtf(me.v[0] * me.v[0] + me.v[1] * me.v[1] + me.v[2] * me.v[2]);
    char line[160];
    switch (g_ct_state) {
    case CT_WAIT:
        if (++g_ct_seen >= CT_START) {
            g_ct_state = CT_RECORD;
            log_line("core.nfsc: crowd: looking for a traffic car to record the road from");
        }
        break;
    case CT_RECORD:
        if (ct_record(me.p) && g_ct_crumbs > 1 && g_ct_trail[g_ct_crumbs - 1].along >= CT_TRAIL_LEN) {
            snprintf(line, sizeof line, "core.nfsc: crowd: %.0f m of road recorded (%d points); moving the player there",
                     g_ct_trail[g_ct_crumbs - 1].along, g_ct_crumbs);
            log_line(line);
            g_ct_state = CT_SETTLE;
            g_ct_at = now + 1000.0;
            g_ct_settles = 0;
        }
        break;
    case CT_SETTLE: /* the player's car back at the start of the road, standing, until it stays there */
        if (now < g_ct_at)
            break;
        if (ct_dist(me.p, g_ct_trail[0].pose.p) > 3.0f || speed > 0.5f) {
            MpPose start = g_ct_trail[0].pose;
            memset(start.v, 0, sizeof start.v);
            memset(start.w, 0, sizeof start.w);
            mp_place(player + 0x24, &start);
            g_ct_at = now + 2000.0;
            if (++g_ct_settles <= 10)
                break;
        }
        snprintf(line, sizeof line, "core.nfsc: crowd: player at the start of the road; class %u%s, up to %d cars",
                 g_ct_class, g_ct_flags & CT_LITE ? ", lite" : "", g_ct_max);
        log_line(line);
        crowd_line("before the first car", me.p, now);
        ct_classes();
        g_ct_state = g_ct_traffic ? CT_BUILD : CT_QUIET;
        g_ct_at = now + CT_EVERY;
        g_ct_hold_from = now;
        break;
    case CT_QUIET: /* no traffic spawns now; the cars there leave as the manager lets them go */
        if (now < g_ct_at)
            break;
        g_ct_at = now + CT_HOLD_EVERY;
        if (get_u32(CT_LIST0_COUNT) > 3u && now - g_ct_hold_from < CT_CLEAR_MS) {
            crowd_line("waiting for the traffic to go", me.p, now);
            break;
        }
        crowd_line("traffic gone (or waited long enough); before the first car", me.p, now);
        g_ct_state = CT_BUILD;
        g_ct_at = now + CT_EVERY;
        break;
    case CT_BUILD:
        if (g_ct_frames % 30u == 0)
            ct_keep_near(me.p);
        if (now >= g_ct_at) {
            g_ct_at = now + CT_EVERY;
            ct_attempt(player, me.p, now);
        }
        break;
    case CT_HOLDING:
        if (g_ct_frames % 30u == 0)
            ct_keep_near(me.p);
        if (now >= g_ct_at) {
            g_ct_at += CT_HOLD_EVERY;
            char head[96];
            snprintf(head, sizeof head, "hold %.0f s (%d cars put back so far)", (now - g_ct_hold_from) / 1000.0,
                     g_ct_recycled);
            crowd_line(head, me.p, now);
            if (now - g_ct_hold_from >= CT_HOLD - 1.0) {
                ct_classes();
                g_ct_state = g_ct_ab ? CT_AB : CT_DONE;
                g_ct_period = 0;
                log_line(g_ct_ab ? "core.nfsc: crowd: a/b: our cars driving and parked by turns, 5 s each"
                                 : "core.nfsc: crowd: done");
            }
        }
        break;
    case CT_AB: /* the cars' own cost under the same machine load: parked (not simulated, not drawn) by turns */
        if (!(g_ct_period & 1) && g_ct_frames % 30u == 0)
            ct_keep_near(me.p);
        if (now >= g_ct_at) {
            g_ct_at += CT_HOLD_EVERY;
            char head[64];
            snprintf(head, sizeof head, "a/b %d %s", g_ct_period + 1, g_ct_period & 1 ? "parked" : "driving");
            crowd_line(head, me.p, now);
            if (++g_ct_period >= CT_AB_PERIODS) {
                ct_park(1);
                log_line("core.nfsc: crowd: done");
                g_ct_state = CT_DONE;
                break;
            }
            ct_park(g_ct_period & 1 ? 0u : 1u);
            g_ct_frames = 0; /* each period's figures cover that period alone */
        }
        break;
    default:
        break;
    }
}

static int ct_env(const char *name, int fallback) {
    const char *e = getenv(name);
    return e && *e ? atoi(e) : fallback;
}

static void crowd_install(void) {
    g_ct_max = ct_env("RECOMP_CROWD", 0);
    const int pool = ct_env("RECOMP_CROWD_POOL", 0);
    if (pool > 0) {
        const uint32_t was = get_u32(CT_POOL_KB);
        const int ok = g_api->guest_write_u32(g_api, CT_POOL_KB, (uint32_t)pool) == POP_OK;
        char line[160];
        snprintf(line, sizeof line, "core.nfsc: crowd: Cars pool %u KB -> %d KB at mod start (%s; the other size %u KB)",
                 was, pool, ok && get_u32(CT_POOL_KB) == (uint32_t)pool ? "written" : "not written",
                 get_u32(CT_POOL_ALT_KB));
        log_line(line);
    }
    if (g_ct_max <= 0)
        return;
    g_ct_class = (uint32_t)ct_env("RECOMP_CROWD_CLASS", (int)CT_CLASS);
    g_ct_flags = ct_env("RECOMP_CROWD_LITE", 0) ? CT_LITE : 0u;
    g_ct_keep = ct_env("RECOMP_CROWD_KEEP", 1) != 0;
    g_ct_bodies_stop = (uint32_t)ct_env("RECOMP_CROWD_BODIES", (int)CT_BODIES_STOP);
    g_ct_traffic = ct_env("RECOMP_CROWD_TRAFFIC", 1) != 0;
    g_ct_ab = ct_env("RECOMP_CROWD_AB", 1) != 0;
    if (g_api->guest_alloc(g_api, 0x20, &g_ct_name) != POP_OK) {
        log_line("core.nfsc: crowd: no guest memory; test off");
        return;
    }
    if (g_api->hook_install(g_api, MP_FE_SERVICE, ct_service, POP_HOOK_BEFORE, NULL, &g_ct_hooks[0]) != POP_OK) {
        g_ct_hooks[0] = 0;
        log_line("core.nfsc: crowd: no frame hook; test off");
        return;
    }
    if (!g_api->hook_install_at_callsite ||
        g_api->hook_install_at_callsite(g_api, CT_CACHE_GATE, CT_CACHE_RET, ct_cache_done, POP_HOOK_AFTER, NULL,
                                        &g_ct_hooks[1]) != POP_OK ||
        g_api->hook_install_at_callsite(g_api, CT_BODY_GATE, CT_BODY_RET, ct_body_done, POP_HOOK_AFTER, NULL,
                                        &g_ct_hooks[2]) != POP_OK)
        log_line("core.nfsc: crowd: no gate hooks; a refusal's gate is not known");
    if (!g_ct_traffic && g_api->hook_install(g_api, CT_TRAFFIC_SPAWN, ct_traffic_spawn, POP_HOOK_AFTER, NULL,
                                             &g_ct_hooks[3]) != POP_OK) {
        g_ct_hooks[3] = 0;
        log_line("core.nfsc: crowd: traffic cannot be stopped (no hook on its spawn check)");
    }
    char line[160];
    snprintf(line, sizeof line,
             "core.nfsc: crowd test on: up to %d cars, class %u%s, keep near %s, traffic %s, a/b %s, Cars pool %u KB",
             g_ct_max, g_ct_class, g_ct_flags ? ", lite" : "", g_ct_keep ? "on" : "off", g_ct_traffic ? "on" : "stopped",
             g_ct_ab ? "on" : "off", get_u32(CT_POOL_KB));
    log_line(line);
}

static void crowd_shutdown(void) {
    for (int i = 0; i < 4; ++i) {
        if (g_ct_hooks[i])
            g_api->hook_remove(g_api, g_ct_hooks[i]);
        g_ct_hooks[i] = 0;
    }
}
