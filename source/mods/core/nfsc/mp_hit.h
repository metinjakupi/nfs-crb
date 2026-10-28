/* mp_hit.h - a repeatable car-to-car collision test (included at the end of mp.h).
 *
 * Off unless the address carries RECOMP_MP_HIT. The page (or a test script) starts
 * it by writing 1 to globalThis.carbonMpHit ({memory, ptr}) +0; the game writes the
 * run number at +4, the phase at +8 (3 = recording) and the run's code at +12.
 *
 * RECOMP_MP_HIT=ai, free or remote: this player's car strikes a target, once per run
 * of RECOMP_MP_HIT_RUNS (default h30,l30,h80,l80,h30,l30,h80,l80: h = into the
 * target's nose, s = into its right side, l = its left side; then km/h). The target's
 * home is 20 m in front of where this player's car stood at the start:
 *   ai     the nearest AI car, held at home until the striker is about to touch it;
 *   free   a class-7 copy of this player's car that nothing drives, held the same way
 *          (the engine's own response to a remote car that no pose overwrites);
 *   remote the first other player's car (its owner runs RECOMP_MP_HIT=target).
 * Each run: 1 s lifted HIT_UP metres above its start (another game's playback then
 * never draws it sweeping through the target), 1 s held still at its start, then
 * launched at the target at the run's speed and left to the physics for 3 s.
 * RECOMP_MP_HIT=target: this player's car waits at that home; whenever another
 * player's car is lifted over it, it goes home again, standing still.
 *
 * Both log every drawn frame of a run ("hit" lines: car states after the frame's
 * physics, as drawn), every contact the pair solver 006e9890 handles for this
 * player's car ("hitC": each car's velocity and spin change) and every shared impact
 * sent or applied ("hitI").
 * Included by mp.h before its pair solver hook, which calls the hitC functions.
 */

#define HIT_UP 15.0f        /* metres above its start while lifted */
#define HIT_HOME 20.0f      /* the target's home: metres in front of the start spot */
#define HIT_LIFT_MS 1000.0
#define HIT_SETTLE_MS 1000.0
#define HIT_RECORD_MS 3000.0
#define HIT_RUNS 16

enum { HIT_OFF, HIT_AI, HIT_FREE, HIT_REMOTE, HIT_TARGET };

static struct {
    int mode;
    uint32_t block;       /* guest: go, run, phase, code */
    int runs, run, phase; /* phase: 0 waiting, 1 lifted, 2 held at the start, 3 launched */
    char cfg[HIT_RUNS][8];
    double at;            /* mp_now() when the phase began */
    uint32_t target;      /* ai/free: the target PVehicle */
    MpCar free_car;
    MpPose home, start, drawn_last;
    int home_set, released, let_go, quat_ok, frames;
    float dir[3], speed, release, let_go_at;
    double rec_at, drawn_at;
    int armed, recording, contacts;
    float min_gap;
} g_hit;

static uint32_t hit_body(uint32_t car) {
    return car ? get_u32(car + 0x70) : 0;
}

static int hit_pose_of(uint32_t car, MpPose *out) {
    const uint32_t state = mp_state(hit_body(car));
    if (!state)
        return 0;
    mp_read(state, out);
    return mp_pose_ok(out);
}

static void hit_place(uint32_t car, const MpPose *pose) {
    const uint32_t body = hit_body(car), state = mp_state(body);
    if (!state)
        return;
    mp_write(body, state, pose);
    g_api->guest_write_u8(g_api, state + 0x5d, 0); /* wake it, as mp_contacts does */
    g_api->guest_write_u8(g_api, state + 0x1c, (get_u32(state + 0x1c) & 0xffu) | 0x40u);
    mp_call(MP_WAKE_BODY, body - 0x40u, NULL, 0);
}

static void hit_row(uint32_t car, uint32_t row, float out[3]) {
    const uint32_t state = mp_state(hit_body(car));
    for (int k = 0; k < 3; ++k)
        out[k] = state ? bits_float(get_u32(state + row + 4u * k)) : 0.0f;
}

static void hit_qmul(const float a[4], const float b[4], float o[4]) { /* (x, y, z, w) */
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

/* The target car now: ai/free its own body, remote the first other player's car. */
static uint32_t hit_target_car(void) {
    if (g_hit.mode == HIT_REMOTE) {
        for (int i = 0; i < MP_SLOTS; ++i)
            if (g_mp_remote[i].simable && g_mp_remote[i].kept)
                return g_mp_remote[i].simable - 0x24;
        return 0;
    }
    return g_hit.target && mp_listed(g_hit.target) ? g_hit.target : 0;
}

static int hit_ours(uint32_t car) {
    for (int i = 0; i < MP_SLOTS; ++i)
        if (g_mp_remote[i].simable && g_mp_remote[i].simable - 0x24 == car)
            return 1;
    return (g_mp_test.simable && g_mp_test.simable - 0x24 == car) ||
           (g_hit.free_car.simable && g_hit.free_car.simable - 0x24 == car);
}

/* The nearest AI car to p. */
static uint32_t hit_nearest_ai(uint32_t player, const float p[3]) {
    uint32_t node = get_u32(MP_CARS), best = 0;
    float bd = 1e30f;
    for (int n = 0; node && node != MP_CARS && n < 64; ++n, node = get_u32(node)) {
        const uint32_t car = node - 0xd0;
        MpPose pose;
        if (car == player || hit_ours(car) || !hit_pose_of(car, &pose))
            continue;
        float d = 0;
        for (int k = 0; k < 3; ++k)
            d += (pose.p[k] - p[k]) * (pose.p[k] - p[k]);
        if (d < bd) {
            bd = d;
            best = car;
        }
    }
    return best;
}

/* The striker's start for run r against a target at t: where, which way, its rotation. */
static void hit_setup(uint32_t player, const MpPose *t) {
    const char *c = g_hit.cfg[g_hit.run];
    float right[3], fwd[3];
    /* Rows of the target's rotation, from the engine itself: set them on our car and read. */
    MpPose probe = *t;
    hit_place(player, &probe);
    hit_row(player, 0x70, right);
    hit_row(player, 0x90, fwd);
    const float sign = c[0] == 'l' ? -1.0f : 1.0f;
    const float *axis = c[0] == 'h' ? fwd : right;
    const float dist = c[0] == 'h' ? 10.0f : 8.0f;
    g_hit.speed = (float)atof(c + 1) / 3.6f;
    for (int k = 0; k < 3; ++k) {
        g_hit.dir[k] = -sign * axis[k];
        g_hit.start.p[k] = t->p[k] + sign * axis[k] * dist;
    }
    g_hit.start.p[1] = t->p[1] + 0.05f;
    /* The rotation about the up axis that points the striker along dir: the engine's
     * quaternion convention is not documented, so try both orders and signs and keep the
     * one whose forward row matches. */
    const float half = c[0] == 'h' ? 1.5707963f : 0.7853982f;
    float best = -2.0f, q[4];
    for (int order = 0; order < 2; ++order)
        for (int s = -1; s <= 1; s += 2) {
            const float r[4] = {0.0f, s * sinf(half), 0.0f, cosf(half)};
            float f[3], dot = 0;
            if (order)
                hit_qmul(r, t->q, q);
            else
                hit_qmul(t->q, r, q);
            memcpy(probe.q, q, sizeof q);
            hit_place(player, &probe);
            hit_row(player, 0x90, f);
            for (int k = 0; k < 3; ++k)
                dot += f[k] * g_hit.dir[k];
            if (dot > best) {
                best = dot;
                memcpy(g_hit.start.q, q, sizeof q);
            }
        }
    g_hit.quat_ok = best > 0.98f;
    /* The quaternion convention, for mp.h's spin correction: the right row against the
     * x axis turned by q as (x, y, z, w) with v' = q v q* (1.0 = that convention). */
    float xr[3];
    mp_rotate(t->q, (const float[3]){1.0f, 0.0f, 0.0f}, xr);
    const float conv = xr[0] * right[0] + xr[1] * right[1] + xr[2] * right[2];
    memset(g_hit.start.v, 0, sizeof g_hit.start.v);
    memset(g_hit.start.w, 0, sizeof g_hit.start.w);
    /* Centres 4.65 m apart nose to nose and 3.2 m nose to side at the first touch (AI runs). */
    g_hit.let_go_at = (c[0] == 'h' ? 4.65f : 3.2f) + 0.6f + g_hit.speed * 0.04f;
    g_hit.release = g_hit.let_go_at + 0.3f;
    char line[200];
    snprintf(line, sizeof line,
             "hitS r=%d c=%s start=%.2f,%.2f,%.2f dir=%.3f,%.3f,%.3f speed=%.2f facing=%.3f tgt=%.2f,%.2f,%.2f conv=%.3f",
             g_hit.run, c, g_hit.start.p[0], g_hit.start.p[1], g_hit.start.p[2], g_hit.dir[0], g_hit.dir[1],
             g_hit.dir[2], g_hit.speed, best, t->p[0], t->p[1], t->p[2], conv);
    log_line(line);
}

static void hit_publish(void) {
    if (!g_hit.block)
        return;
    mp_put(g_hit.block + 4, (uint32_t)g_hit.run);
    mp_put(g_hit.block + 8, (uint32_t)(g_hit.recording ? 3 : g_hit.phase));
    const char *c = g_hit.run < g_hit.runs ? g_hit.cfg[g_hit.run] : "";
    uint32_t code = 0;
    memcpy(&code, c, strlen(c) < 4 ? strlen(c) : 4);
    mp_put(g_hit.block + 12, code);
}

/* Home: HIT_HOME metres in front of where the player's car is now, standing still. */
static void hit_home(uint32_t player) {
    MpPose own;
    float fwd[3];
    if (!hit_pose_of(player, &own))
        return;
    hit_row(player, 0x90, fwd);
    g_hit.home = own;
    for (int k = 0; k < 3; ++k)
        g_hit.home.p[k] += fwd[k] * HIT_HOME;
    g_hit.home.p[1] += 0.3f;
    memset(g_hit.home.v, 0, sizeof g_hit.home.v);
    memset(g_hit.home.w, 0, sizeof g_hit.home.w);
    g_hit.home_set = 1;
}

/* The striker's runs, once a frame. */
static void hit_strike(uint32_t player) {
    const double now = mp_now();
    if (!g_hit.home_set) {
        hit_home(player);
        if (g_hit.mode == HIT_FREE) {
            g_hit.free_car.kind = MP_REMOTE;
            g_hit.free_car.simable = mp_build(player, MP_REMOTE, 0, g_hit.home.p, 2u | 8u, 0);
            g_hit.target = g_hit.free_car.simable ? g_hit.free_car.simable - 0x24 : 0;
        } else if (g_hit.mode == HIT_AI) {
            g_hit.target = hit_nearest_ai(player, g_hit.home.p);
        }
        log_line(g_hit.mode == HIT_REMOTE || g_hit.target ? "hit: target found" : "hit: no target");
        g_hit.phase = 0;
        g_hit.at = now;
        return;
    }
    const uint32_t target = hit_target_car();
    const int held = g_hit.mode != HIT_REMOTE && target && !(g_hit.phase == 3 && g_hit.released);
    if (held && (g_hit.phase != 0 || now - g_hit.at > 500.0))
        hit_place(target, &g_hit.home);
    if (g_hit.run >= g_hit.runs) {
        if (g_hit.phase != 4)
            log_line("hit: done");
        g_hit.phase = 4;
        return;
    }
    if (!target) {
        if (now - g_hit.at > 2000.0)
            g_hit.at = now; /* wait for the other player's car */
        return;
    }
    MpPose t, own, aim;
    if (!hit_pose_of(target, &t) || !hit_pose_of(player, &own))
        return;
    aim = t; /* remote: where its owner's newest pose has it (the drawn car may still be on its way) */
    for (int i = 0; i < MP_SLOTS && g_hit.mode == HIT_REMOTE; ++i)
        if (g_mp_remote[i].simable && g_mp_remote[i].simable - 0x24 == target && g_mp_remote[i].kept)
            aim = g_mp_remote[i].pose[g_mp_remote[i].kept - 1];
    if (g_hit.phase == 0 && now - g_hit.at > 1500.0) {
        g_hit.phase = 1;
        g_hit.at = now;
        hit_setup(player, g_hit.mode == HIT_REMOTE ? &aim : &g_hit.home);
    }
    if (g_hit.phase == 1) {
        MpPose lifted = g_hit.start;
        lifted.p[1] += HIT_UP;
        hit_place(player, &lifted);
        if (now - g_hit.at > HIT_LIFT_MS) {
            hit_setup(player, g_hit.mode == HIT_REMOTE ? &aim : &g_hit.home); /* where it stands now */
            g_hit.phase = 2;
            g_hit.at = now;
        }
    }
    if (g_hit.phase == 2) {
        hit_place(player, &g_hit.start);
        if (now - g_hit.at > HIT_SETTLE_MS) {
            MpPose go = g_hit.start;
            for (int k = 0; k < 3; ++k)
                go.v[k] = g_hit.dir[k] * g_hit.speed;
            hit_place(player, &go);
            g_hit.phase = 3;
            g_hit.at = g_hit.rec_at = now;
            g_hit.released = g_hit.mode == HIT_REMOTE;
            g_hit.let_go = 0;
            g_hit.recording = 1;
            g_hit.frames = g_hit.contacts = 0;
            g_hit.min_gap = 1e9f;
            g_hit.drawn_at = 0;
            char line[96];
            snprintf(line, sizeof line, "hitL r=%d c=%s launched", g_hit.run, g_hit.cfg[g_hit.run]);
            log_line(line);
        }
    } else if (g_hit.phase == 3) {
        float d = 0;
        for (int k = 0; k < 3; k += 2)
            d += (own.p[k] - t.p[k]) * (own.p[k] - t.p[k]);
        /* Driven at the run's speed until just before the touch, so every run hits at the
         * same speed: a car given a speed with its wheels standing loses some of it, a
         * different amount each time. */
        if (!g_hit.let_go && sqrtf(d) > g_hit.let_go_at) {
            const uint32_t state = mp_state(hit_body(player));
            for (int k = 0; k < 3 && state; ++k) {
                if (k != 1)
                    put_float(state + 0x20 + 4u * k, g_hit.dir[k] * g_hit.speed);
                put_float(state + 0x30 + 4u * k, 0.0f);
            }
        } else if (!g_hit.let_go) {
            g_hit.let_go = 1;
            char line[96];
            snprintf(line, sizeof line, "hitG r=%d let go at %.2f m", g_hit.run, sqrtf(d));
            log_line(line);
        }
        if (!g_hit.released && sqrtf(d) < g_hit.release) {
            g_hit.released = 1;
            char line[96];
            snprintf(line, sizeof line, "hitR r=%d released at %.2f m", g_hit.run, sqrtf(d));
            log_line(line);
        }
        if (now - g_hit.at > HIT_RECORD_MS) {
            g_hit.recording = 0;
            ++g_hit.run;
            g_hit.phase = 0;
            g_hit.at = now - 1000.0;
        }
    }
}

/* The target player: home, and home again whenever another player's car is lifted over it. */
static void hit_wait(uint32_t player) {
    const double now = mp_now();
    if (!g_hit.home_set) {
        hit_home(player);
        g_hit.at = now;
        g_hit.armed = 1;
    }
    MpPose own;
    if (!hit_pose_of(player, &own))
        return;
    if (now - g_hit.at < 500.0)
        hit_place(player, &g_hit.home);
    for (int i = 0; i < MP_SLOTS; ++i) {
        const MpCar *c = &g_mp_remote[i];
        if (!c->simable || !c->kept)
            continue;
        const MpPose p = c->pose[c->kept - 1]; /* its owner's newest: the drawn car may lag */
        const float dx = p.p[0] - own.p[0], dz = p.p[2] - own.p[2], dy = p.p[1] - own.p[1];
        const float flat = sqrtf(dx * dx + dz * dz);
        const float speed = sqrtf(p.v[0] * p.v[0] + p.v[2] * p.v[2]);
        if (dy > 8.0f && flat < 40.0f) {
            hit_place(player, &g_hit.home);
            g_hit.armed = 1;
            g_hit.phase = 1;
        } else if (g_hit.armed && flat < 15.0f && dy < 3.0f && speed > 4.0f && !g_hit.recording) {
            g_hit.armed = 0;
            g_hit.recording = 1;
            g_hit.rec_at = now;
            g_hit.frames = g_hit.contacts = 0;
            g_hit.min_gap = 1e9f;
            g_hit.drawn_at = 0;
            char line[96];
            snprintf(line, sizeof line, "hitL r=%d c=target incoming", g_hit.run);
            log_line(line);
        }
    }
    if (g_hit.recording && now - g_hit.rec_at > HIT_RECORD_MS) {
        g_hit.recording = 0;
        g_hit.phase = 0;
        ++g_hit.run;
    }
}

static void mp_hit_service(uint32_t player) {
    if (!g_hit.mode || !g_hit.block || !player)
        return;
    if (!get_u32(g_hit.block)) /* not started yet */
        return;
    if (g_hit.mode == HIT_TARGET)
        hit_wait(player);
    else
        hit_strike(player);
    hit_publish();
}

static void hit_fmt(char *at, size_t n, const char *tag, const MpPose *p) {
    snprintf(at, n, " %s=%.3f,%.3f,%.3f|%.3f,%.3f,%.3f|%.3f,%.3f,%.3f", tag, p->p[0], p->p[1], p->p[2], p->v[0],
             p->v[1], p->v[2], p->w[0], p->w[1], p->w[2]);
}

/* As each frame is drawn: this player's car and the target as they are on screen. */
static void mp_hit_frame(void) {
    if (!g_hit.mode || !g_hit.recording)
        return;
    const uint32_t player = mp_player();
    uint32_t target = g_hit.mode == HIT_TARGET ? 0 : hit_target_car();
    const MpCar *rc = NULL;
    for (int i = 0; i < MP_SLOTS && (g_hit.mode == HIT_TARGET || g_hit.mode == HIT_REMOTE); ++i)
        if (g_mp_remote[i].simable && g_mp_remote[i].kept) {
            rc = &g_mp_remote[i];
            target = rc->simable - 0x24;
            break;
        }
    MpPose own, t;
    if (!hit_pose_of(player, &own) || !target || !hit_pose_of(target, &t))
        return;
    const double now = mp_now(), ms = now - g_hit.rec_at;
    ++g_hit.frames;
    if (ms > 1500.0 && g_hit.frames % 4)
        return;
    char line[512];
    int at = snprintf(line, sizeof line, "hit r=%d f=%d t=%.1f st=%u", g_hit.run, g_hit.frames, ms,
                      (unsigned)(g_mp_steps & 0xffffffu));
    hit_fmt(line + at, sizeof line - at, "own", &own);
    at += (int)strlen(line + at);
    hit_fmt(line + at, sizeof line - at, "tgt", &t);
    at += (int)strlen(line + at);
    if (rc)
        snprintf(line + at, sizeof line - at, " rc=%d,%.0f,%u,%u,%u", rc->follow, rc->lag, rc->sent, rc->ack,
                 rc->applied);
    log_line(line);
}

/* Around the pair solver: each contact of this player's car while a run records. */
static MpPose g_hit_pre[2];
static int g_hit_pre_ok;

static void mp_hit_pair_before(uint32_t a, uint32_t b) {
    g_hit_pre_ok = 0;
    if (!g_hit.mode || !g_hit.recording)
        return;
    const uint32_t sa = mp_state(a + 0x40u), sb = mp_state(b + 0x40u);
    if (!sa || !sb)
        return;
    mp_read(sa, &g_hit_pre[0]);
    mp_read(sb, &g_hit_pre[1]);
    g_hit_pre_ok = 1;
}

static void mp_hit_pair_after(uint32_t a, uint32_t b, const char *how) {
    if (!g_hit_pre_ok)
        return;
    g_hit_pre_ok = 0;
    const uint32_t oa = get_u32(a + 0x2c), ob = get_u32(b + 0x2c);
    if (oa != g_mp_player_simable && ob != g_mp_player_simable)
        return;
    const int mine = oa == g_mp_player_simable ? 0 : 1;
    MpPose post[2];
    mp_read(mp_state(a + 0x40u), &post[0]);
    mp_read(mp_state(b + 0x40u), &post[1]);
    const MpPose *o0 = &g_hit_pre[mine], *o1 = &post[mine], *t0 = &g_hit_pre[1 - mine], *t1 = &post[1 - mine];
    float gap = 0;
    for (int k = 0; k < 3; k += 2)
        gap += (o0->p[k] - t0->p[k]) * (o0->p[k] - t0->p[k]);
    char line[400];
    snprintf(line, sizeof line,
             "hitC r=%d t=%.1f st=%u %s gap=%.3f dvo=%.3f,%.3f,%.3f dwo=%.3f,%.3f,%.3f dvt=%.3f,%.3f,%.3f "
             "dwt=%.3f,%.3f,%.3f dpo=%.3f,%.3f,%.3f dpt=%.3f,%.3f,%.3f",
             g_hit.run, mp_now() - g_hit.rec_at, (unsigned)(g_mp_steps & 0xffffffu), how, sqrtf(gap),
             o1->v[0] - o0->v[0], o1->v[1] - o0->v[1], o1->v[2] - o0->v[2], o1->w[0] - o0->w[0],
             o1->w[1] - o0->w[1], o1->w[2] - o0->w[2], t1->v[0] - t0->v[0], t1->v[1] - t0->v[1],
             t1->v[2] - t0->v[2], t1->w[0] - t0->w[0], t1->w[1] - t0->w[1], t1->w[2] - t0->w[2],
             o1->p[0] - o0->p[0], o1->p[1] - o0->p[1], o1->p[2] - o0->p[2], t1->p[0] - t0->p[0],
             t1->p[1] - t0->p[1], t1->p[2] - t0->p[2]);
    log_line(line);
}

static void mp_hit_install(void) {
    const char *e = getenv("RECOMP_MP_HIT");
    if (!e || !*e)
        return;
    g_hit.mode = !strcmp(e, "ai") ? HIT_AI : !strcmp(e, "free") ? HIT_FREE : !strcmp(e, "remote") ? HIT_REMOTE
               : !strcmp(e, "target") ? HIT_TARGET : HIT_OFF;
    const char *runs = getenv("RECOMP_MP_HIT_RUNS");
    if (!runs || !*runs)
        runs = "h30,l30,h80,l80,h30,l30,h80,l80";
    for (const char *p = runs; *p && g_hit.runs < HIT_RUNS;) {
        int n = 0;
        while (p[n] && p[n] != ',' && n < 7)
            ++n;
        memcpy(g_hit.cfg[g_hit.runs], p, (size_t)n);
        g_hit.cfg[g_hit.runs++][n] = 0;
        p += n;
        while (*p == ',')
            ++p;
    }
    if (!g_hit.mode || g_api->guest_alloc(g_api, 16, &g_hit.block) != POP_OK) {
        g_hit.mode = 0;
        return;
    }
    for (int i = 0; i < 16; i += 4)
        mp_put(g_hit.block + (uint32_t)i, 0);
    log_line("hit: collision test ready (waiting for carbonMpHit go)");
    void *host = NULL; /* the page sees the block at its address in the wasm memory */
    if (g_api->guest_ptr(g_api, g_hit.block, 16, &host) != POP_OK)
        return;
#ifdef __EMSCRIPTEN__
    MAIN_THREAD_ASYNC_EM_ASM({
        var hit = {};
        hit.memory = wasmMemory;
        hit.ptr = $0;
        globalThis.carbonMpHit = hit;
    }, (uint32_t)(uintptr_t)host);
#else
    (void)host;
#endif
}
