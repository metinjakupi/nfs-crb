/* mp_drive.h - other players' cars driven by their owners' own controls (included by nfsc.c
 * after mp.h).
 *
 * WHY. mp.h places a friend's car every frame from its owner's poses, so nothing that comes from
 * the owner's controls showed: a burnout had no smoke and no spinning wheels, the brake and
 * reverse lights stayed off, nitrous had no flames, the engine idled and the front wheels never
 * steered.
 *
 * THE GAME'S OWN WAY. Carbon's online races send each car's state about 15 times a second
 * (OLMSG CarState, server/src/olmsg.rs): its pose and what the recorder 007bc060 reads from the
 * car's IInput - steering, gas, brake, handbrake, nitrous (while its engine has some) - its gear
 * (0-7) and whether it is wrecked. The receiver's 007bc940 puts the pose into the remote car's
 * body and the controls back into its IInput (SetControlSteering / Gas / Brake / HandBrake / NOS,
 * table +0x24 / +0x2c / +0x30 / +0x38 / +0x34), and shifts its gearbox (ITransmission +0xc)
 * into reverse or neutral; the car's own engine, wheels, lights and sound do the rest. This file
 * does the same for friends' cars, through the relay instead of the game's session: each pose
 * carries 8 bytes of controls (DRIVE below), and mp.h's mp_follow - still the one place a remote
 * car is moved - hands each car to drive_car once a frame with its playback time, whose controls
 * go into the car's IInput. A car whose owner sends none (an older page or relay) is left as it
 * was.
 *
 * WHAT ELSE A DRIVEN CAR GETS.
 *  - Its owner's suspension. Remote cars (driver class 3-8, model class CAR: 006d0d40) get
 *    SuspensionSimple, which never spins a wheel: full gas and brake at a standstill took the
 *    test car's engine to its 6000 rpm red line with no smoke at all (2026-10-26). So a driven
 *    car gets SuspensionRacer, the player's own, through the game's behaviour override
 *    (IVehicle::SetBehaviorOverride 006d8e10 with the suspension mechanic's key [00a9e83c] and
 *    SuspensionRacer's [00aaa288], then CommitBehaviorOverrides 006daa60 -> 006da030, which
 *    loads only the behaviours whose name changed). Its wheels then spin, smoke, skid and steer
 *    as the player's do. The override goes when the game changes the car's driver class
 *    (SetDriverClass 006da090 clears it), so it is checked every frame.
 *  - A manual gearbox: a private copy of its IInput table whose IsAutomaticShift (+0x4c) says
 *    no (the AI's says yes, 0043bd60), so it shifts when its owner does (Shift, its owner's
 *    gear) and never on its own; the gear change sounds then come at its owner's moments.
 *  - Nitrous. look.h builds a friend's car without its performance parts, and the nitrous is
 *    one: its engine's nitrous attributes (IEngine + 0xc8, an Attrib::Instance: collection, then
 *    the layout at +0xcc) are all 0, so HasNOS (+0x2c, 006f1070: layout +0x10 and +0x04 above 0)
 *    says no and its nitrous step (006e3df0) never starts, whatever the input. Such a car's layout
 *    pointer is turned to a copy of ours (DRV_NOS_LAYOUT bytes, made once and kept for good, as
 *    the game's layouts are only read; the collection, which the instance owns, is left as it
 *    is). Its tank is kept full while its owner's nitrous is on, so the flames last as long as
 *    theirs; the owner's game sends nitrous only while there is some, as the game's recorder does.
 *  - Revs: its own engine works them out from the gas, the gear and its wheels; within
 *    DRV_RPM_SLACK of its owner's they are left alone, else pulled toward the owner's.
 *
 * DRIVE, 8 bytes (the same on the wire, tools/carbon-net-relay.mjs "Driving"):
 *   +0 i8 steering x127 (-1 left .. 1 right, as IInput's)   +1 u8 gas x255
 *   +2 u8 brake x255   +3 u8 handbrake x255   +4 u8 gear: bits 0-3 the game's gear number
 *   (0 reverse, 1 neutral, 2 first ...)   +5 u8 flags: bit 0 nitrous on   +6 u16 engine rpm
 *
 * THE PAGE. web/net.js shares DrvShared (globalThis.carbonDriveBlock {memory, ptr}, a
 * "carbon-drive" event). The game writes this player's controls (own) every frame with the
 * physics time of the pose it publishes at the same moment (MpShared.ms); the page sends them in
 * that pose. Each other player's arrive with their pose, and the page writes them into the slot
 * of the same number as the player's MpShared slot: a ring of DRV_RING, each with its pose's
 * time. Each side guards its part with a sequence number, odd while it writes, as mp.h does.
 *
 * TEST (RECOMP_MP_TEST, mp.h): the escort test car (2) is driven by this player's own controls of
 * the moment, through the 8-byte form; the replaying one (1) replays them with its poses.
 */

#define DRV_MAGIC 0x31565244u /* "DRV1" */
#define DRV_RING 8
#define DRV_STALE 1000.0      /* ms: controls further than this from the playback time are not used */
#define DRV_SHIFT_MS 120.0    /* ms between two shifts we ask a car for */
#define DRV_SETUP_MS 1000.0   /* ms between two attempts to set a car up */
#define DRV_RPM_SLACK 0.12f   /* its own revs within this share of its owner's are left alone */
#define DRV_RPM_PULL 0.35f    /* else this share of the difference, each frame */
#define DRV_CARS 32
/* PVehicle */
#define DRV_IVEHICLE 0xb8u
#define DRV_INPUT 0xecu  /* IInput * */
#define DRV_SUSP 0xf4u   /* ISuspension * */
#define DRV_ENGINE 0xf8u /* IEngine * */
#define DRV_TRANS 0x100u /* ITransmission * */
/* IInput (AIVehicle's, every AI and human table shares these): the controls at +0xbc */
#define DRV_CTL 0xbcu /* steering, gas, brake, handbrake, clutch, over-steer (floats), nitrous (byte +0x18) */
#define DRV_GET_STEER 0x0043bce0u
#define DRV_GET_GAS 0x0043bd00u
#define DRV_GET_BRAKE 0x0043bcf0u
#define DRV_GET_HAND 0x0043bcc0u
#define DRV_GET_NOS 0x0043bcb0u
#define DRV_SET_STEER 0x00406780u
#define DRV_SET_GAS 0x00406790u
#define DRV_SET_BRAKE 0x004067a0u
#define DRV_SET_HAND 0x004067b0u
#define DRV_SET_NOS 0x004175d0u
#define DRV_NO 0x0043bd40u    /* returns false (AIVehicle's IsLookBackButtonPressed) */
#define DRV_TABLE 0x50u       /* IInput's 20 methods */
/* EngineRacer's IEngine and ITransmission (the engine of every racer model) */
#define DRV_RPM 0x006f0fe0u   /* IEngine +0x04 GetRPM: [+0x118] */
#define DRV_NOS_LEFT 0x006f1030u /* IEngine +0x1c: the nitrous left, [+0xa4] (1 = full) */
#define DRV_GEAR 0x006f11e0u  /* ITransmission +0x04 GetGear: [+0x38] */
#define DRV_TOP_GEAR 0x006f12b0u /* +0x08 the top gear */
#define DRV_SHIFT 0x006ffb10u /* +0x0c Shift(gear) */
#define DRV_SHIFTING 0x006f11f0u /* +0x10: [+0x3c] > 0 while a shift is under way */
#define DRV_HAS_NOS 0x006f1070u /* IEngine +0x2c HasNOS: the nitrous layout's +0x10 and +0x04 above 0 */
#define DRV_NOS_LAYOUT 0x40u    /* bytes of the nitrous layout copied (the nitrous step reads to +0x1c) */
/* Behaviours */
#define DRV_SET_OVERRIDE 0x006d8e10u /* IVehicle::SetBehaviorOverride thiscall (mechanic, behaviour), RET 8 */
#define DRV_COMMIT 0x006daa60u       /* IVehicle::CommitBehaviorOverrides */
#define DRV_MECH_SUSP 0x00a9e83cu    /* the suspension mechanic's key */
#define DRV_SUSP_RACER 0x00aaa288u   /* "SuspensionRacer"'s key (its factory's) */
#define DRV_SUSP_RACER_TABLE 0x009e5e68u

typedef struct DrvSnap { /* 12 bytes */
    uint32_t ms;         /* the sender's clock: the time of the pose these came with */
    uint8_t c[8];        /* DRIVE */
} DrvSnap;

typedef struct DrvSlot { /* 0x70 bytes; written by the page */
    uint32_t id, seq;    /* the player (as MpShared's slot of this number); seq odd while written */
    uint32_t count;      /* controls written; the newest is ring[(count - 1) % DRV_RING] */
    uint32_t spare;
    DrvSnap ring[DRV_RING];
} DrvSlot;

typedef struct DrvShared {
    uint32_t magic, version; /* DRV_MAGIC, 1 */
    uint32_t seq, ms;        /* the game's part: seq odd while written; ms: the pose's physics time */
    uint8_t own[8];          /* this player's controls (DRIVE), 0 while not driving */
    uint32_t page;           /* the page: 1 while the relay passes controls */
    uint32_t spare;
    DrvSlot slots[MP_SLOTS];
} DrvShared;
_Static_assert(sizeof(DrvSnap) == 12 && sizeof(DrvSlot) == 0x70 && offsetof(DrvShared, own) == 0x10 &&
                   offsetof(DrvShared, page) == 0x18 && offsetof(DrvShared, slots) == 0x20 &&
                   sizeof(DrvShared) == 0x3a0,
               "DrvShared layout");

typedef struct DrvCtl {
    float steer, gas, brake, hand, rpm;
    int gear, nos;
} DrvCtl;

typedef struct DrvCar {   /* what this file keeps per car it drives */
    uint32_t simable;
    uint32_t frame;       /* when it was last seen */
    double shift_at, setup_at;
    int setups;           /* attempts in a row that did not give it SuspensionRacer */
    int driven;           /* controls were put in it */
} DrvCar;

static uint32_t g_drv_block;
static uint32_t g_drv_frame;
static DrvCar g_drv_cars[DRV_CARS];
static uint32_t g_drv_tables[4][2]; /* {game's IInput table, our copy} */
static uint8_t g_drv_take[MP_TAKE][8]; /* the test car's take: this player's controls */
static uint8_t g_drv_own[8];
static uint32_t g_drv_nos; /* our copy of a nitrous attribute layout (see the header), 0 = none yet */

static DrvShared *drv_shared(void) {
    void *p = NULL;
    return g_drv_block && g_api->guest_ptr(g_api, g_drv_block, sizeof(DrvShared), &p) == POP_OK ? p : NULL;
}

static float drv_unit(float x) { return !(x > 0.0f) ? 0.0f : x > 1.0f ? 1.0f : x; }

static void drv_encode(const DrvCtl *in, uint8_t c[8]) {
    float s = in->steer;
    s = !(s > -1.0f) ? -1.0f : s > 1.0f ? 1.0f : s;
    c[0] = (uint8_t)(int8_t)lroundf(s * 127.0f);
    c[1] = (uint8_t)lroundf(drv_unit(in->gas) * 255.0f);
    c[2] = (uint8_t)lroundf(drv_unit(in->brake) * 255.0f);
    c[3] = (uint8_t)lroundf(drv_unit(in->hand) * 255.0f);
    c[4] = (uint8_t)(in->gear < 0 ? 0 : in->gear > 15 ? 15 : in->gear);
    c[5] = in->nos ? 1u : 0u;
    const float r = !(in->rpm > 0.0f) ? 0.0f : in->rpm > 65535.0f ? 65535.0f : in->rpm;
    const uint32_t rpm = (uint32_t)lroundf(r);
    c[6] = (uint8_t)rpm;
    c[7] = (uint8_t)(rpm >> 8);
}

static void drv_decode(const uint8_t c[8], DrvCtl *out) {
    out->steer = (float)(int8_t)c[0] / 127.0f;
    out->steer = out->steer < -1.0f ? -1.0f : out->steer;
    out->gas = c[1] / 255.0f;
    out->brake = c[2] / 255.0f;
    out->hand = c[3] / 255.0f;
    out->gear = c[4] & 15;
    out->nos = c[5] & 1;
    out->rpm = (float)(c[6] | (uint32_t)c[7] << 8);
}

/* A guest table's method n, or 0. */
static uint32_t drv_method(uint32_t object, uint32_t n) {
    const uint32_t table = object ? get_u32(object) : 0;
    return table ? get_u32(table + 4u * n) : 0;
}

/* car's controls, gear, revs and nitrous now (what the game's own recorder reads); 0 if they
 * cannot be read (an input, engine or gearbox of another kind). */
static int drv_read(uint32_t car, DrvCtl *out) {
    const uint32_t in = get_u32(car + DRV_INPUT), engine = get_u32(car + DRV_ENGINE), trans = get_u32(car + DRV_TRANS);
    memset(out, 0, sizeof *out);
    out->gear = 1;
    if (!in || drv_method(in, 4) != DRV_GET_STEER || drv_method(in, 3) != DRV_GET_GAS ||
        drv_method(in, 2) != DRV_GET_BRAKE || drv_method(in, 7) != DRV_GET_HAND || drv_method(in, 6) != DRV_GET_NOS)
        return 0;
    out->steer = bits_float(get_u32(in + DRV_CTL));
    out->gas = bits_float(get_u32(in + DRV_CTL + 4));
    out->brake = bits_float(get_u32(in + DRV_CTL + 8));
    out->hand = bits_float(get_u32(in + DRV_CTL + 12));
    const int nos = (get_u32(in + DRV_CTL + 0x18) & 0xffu) != 0;
    if (engine && drv_method(engine, 1) == DRV_RPM)
        out->rpm = bits_float(get_u32(engine + 0x118));
    /* nitrous only while there is some, as the game's own recorder sends it */
    out->nos = nos && engine && drv_method(engine, 7) == DRV_NOS_LEFT && bits_float(get_u32(engine + 0xa4)) > 0.0f;
    if (trans && drv_method(trans, 1) == DRV_GEAR)
        out->gear = (int)get_u32(trans + 0x38);
    if (!isfinite(out->steer) || !isfinite(out->gas) || !isfinite(out->brake) || !isfinite(out->hand) ||
        !isfinite(out->rpm))
        memset(out, 0, sizeof *out);
    return 1;
}

static DrvCar *drv_car(uint32_t simable) {
    DrvCar *free_one = NULL;
    for (int i = 0; i < DRV_CARS; ++i) {
        DrvCar *d = &g_drv_cars[i];
        if (d->simable == simable)
            return d;
        if (!free_one && (!d->simable || g_drv_frame - d->frame > 120u))
            free_one = d;
    }
    if (free_one) {
        memset(free_one, 0, sizeof *free_one);
        free_one->simable = simable;
    }
    return free_one;
}

/* Our copy of an IInput table, IsAutomaticShift answering no; 0 if the table is not an AI one. */
static uint32_t drv_table(uint32_t table) {
    for (int i = 0; i < 4; ++i)
        if (g_drv_tables[i][1] == table)
            return table; /* already ours */
    if (get_u32(table + 0x24) != DRV_SET_STEER || get_u32(table + 0x2c) != DRV_SET_GAS ||
        get_u32(table + 0x30) != DRV_SET_BRAKE || get_u32(table + 0x38) != DRV_SET_HAND ||
        get_u32(table + 0x34) != DRV_SET_NOS)
        return 0;
    for (int i = 0; i < 4; ++i)
        if (g_drv_tables[i][0] == table)
            return g_drv_tables[i][1];
    for (int i = 0; i < 4; ++i) {
        if (g_drv_tables[i][0])
            continue;
        uint32_t copy = 0;
        if (g_api->guest_alloc(g_api, DRV_TABLE, &copy) != POP_OK || !copy)
            return 0;
        mp_copy(copy, table, DRV_TABLE);
        mp_put(copy + 0x4c, DRV_NO);
        g_drv_tables[i][0] = table;
        g_drv_tables[i][1] = copy;
        return copy;
    }
    return 0;
}

/* Whether a nitrous layout has nitrous (HasNOS's test). */
static int drv_nos_ok(uint32_t layout) {
    return layout && bits_float(get_u32(layout + 0x10)) > 0.0f && bits_float(get_u32(layout + 0x04)) > 0.0f;
}

/* car's engine has no nitrous: point it at our copy of a nitrous layout (made from our own car's,
 * the first time our car has nitrous). 0 if there is none to give yet. */
static int drv_give_nos(uint32_t car) {
    const uint32_t engine = get_u32(car + DRV_ENGINE);
    if (!engine || drv_method(engine, 11) != DRV_HAS_NOS)
        return 0;
    if (drv_nos_ok(get_u32(engine + 0xcc)))
        return 1;
    if (!g_drv_nos) {
        const uint32_t player = mp_player(), mine = player ? get_u32(player + DRV_ENGINE) : 0;
        const uint32_t layout = mine && drv_method(mine, 11) == DRV_HAS_NOS ? get_u32(mine + 0xcc) : 0;
        uint32_t copy = 0;
        if (!drv_nos_ok(layout) || g_api->guest_alloc(g_api, DRV_NOS_LAYOUT, &copy) != POP_OK || !copy)
            return 0;
        mp_copy(copy, layout, DRV_NOS_LAYOUT);
        g_drv_nos = copy;
    }
    mp_put(engine + 0xcc, g_drv_nos);
    log_line("core.nfsc: drive: a friend's car built without nitrous got this car's nitrous layout");
    return 1;
}

/* Sets car up to be driven (SuspensionRacer, a manual gearbox); 0 while it cannot be. */
static int drv_setup(uint32_t car, DrvCar *d, double now) {
    const uint32_t in = get_u32(car + DRV_INPUT), susp = get_u32(car + DRV_SUSP);
    const uint32_t table = in ? get_u32(in) : 0, ours = table ? drv_table(table) : 0;
    if (!ours)
        return 0;
    if (ours != table)
        mp_put(in, ours);
    if ((susp ? get_u32(susp) : 0) != DRV_SUSP_RACER_TABLE && d->setups < 3 && now - d->setup_at >= DRV_SETUP_MS) {
        d->setup_at = now;
        const uint32_t args[2] = {get_u32(DRV_MECH_SUSP), get_u32(DRV_SUSP_RACER)};
        mp_call(DRV_SET_OVERRIDE, car + DRV_IVEHICLE, args, 2);
        mp_call(DRV_COMMIT, car + DRV_IVEHICLE, NULL, 0);
        const uint32_t now_susp = get_u32(car + DRV_SUSP);
        const int took = now_susp && get_u32(now_susp) == DRV_SUSP_RACER_TABLE;
        d->setups = took ? 0 : d->setups + 1; /* three misses in a row: it is left with its own */
        char line[128];
        snprintf(line, sizeof line, "core.nfsc: drive: car %08x %s SuspensionRacer", car, took ? "has" : "did not take");
        log_line(line);
    }
    return 1;
}

/* Puts controls into car, as the game's 007bc940 does, and its owner's gear and revs. */
static void drv_apply(uint32_t car, DrvCar *d, const DrvCtl *ctl, double now) {
    const uint32_t in = get_u32(car + DRV_INPUT), engine = get_u32(car + DRV_ENGINE), trans = get_u32(car + DRV_TRANS);
    if (!in)
        return;
    const uint32_t f[4] = {float_bits(ctl->steer), float_bits(ctl->gas), float_bits(ctl->brake), float_bits(ctl->hand)};
    const uint32_t nos = ctl->nos ? 1u : 0u;
    mp_call(drv_method(in, 9), in, &f[0], 1);  /* SetControlSteering */
    mp_call(drv_method(in, 11), in, &f[1], 1); /* SetControlGas */
    mp_call(drv_method(in, 12), in, &f[2], 1); /* SetControlBrake */
    mp_call(drv_method(in, 14), in, &f[3], 1); /* SetControlHandBrake */
    mp_call(drv_method(in, 13), in, &nos, 1);  /* SetControlNOS */
    d->driven = 1;
    if (engine && drv_method(engine, 7) == DRV_NOS_LEFT && ctl->nos && drv_give_nos(car))
        put_float(engine + 0xa4, 1.0f); /* its owner has nitrous: so has this copy */
    if (trans && drv_method(trans, 1) == DRV_GEAR && drv_method(trans, 2) == DRV_TOP_GEAR &&
        drv_method(trans, 3) == DRV_SHIFT && drv_method(trans, 4) == DRV_SHIFTING) {
        const int top = (int)mp_call(DRV_TOP_GEAR, trans, NULL, 0); /* its highest gear's number */
        const int want = top >= 1 && ctl->gear > top ? top : ctl->gear; /* a friend's car in our model may have fewer */
        const int gear = (int)get_u32(trans + 0x38);
        const int shifting = bits_float(get_u32(trans + 0x3c)) > 0.0f;
        if (want != gear && !shifting && now - d->shift_at >= DRV_SHIFT_MS) {
            const uint32_t arg = (uint32_t)want;
            mp_call(DRV_SHIFT, trans, &arg, 1);
            d->shift_at = now;
        }
    }
    if (engine && drv_method(engine, 1) == DRV_RPM && ctl->rpm > 0.0f) {
        const float own = bits_float(get_u32(engine + 0x118));
        if (isfinite(own) && fabsf(own - ctl->rpm) > DRV_RPM_SLACK * ctl->rpm)
            put_float(engine + 0x118, own + (ctl->rpm - own) * DRV_RPM_PULL);
    }
}

/* Lets go of a car whose owner's controls stopped coming: nothing pressed. */
static void drv_release(uint32_t car, DrvCar *d) {
    const uint32_t in = get_u32(car + DRV_INPUT);
    if (in && d->driven)
        mp_call(drv_method(in, 1), in, NULL, 0); /* ClearInput */
    d->driven = 0;
}

/* The controls slot holds for player id at sender time t, or 0 (none near that time). */
static int drv_sample(const DrvShared *sh, int slot, uint32_t id, double t, uint8_t out[8]) {
    const DrvSlot *s = &sh->slots[slot];
    DrvSlot copy;
    const uint32_t before = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
    if (before & 1)
        return 0;
    memcpy(&copy, s, sizeof copy);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&s->seq, __ATOMIC_ACQUIRE) != before || copy.id != id || !copy.count)
        return 0;
    /* The newest at or before t; else the oldest after it. */
    const DrvSnap *best = NULL, *after = NULL;
    const uint32_t n = copy.count < DRV_RING ? copy.count : DRV_RING;
    for (uint32_t k = 0; k < n; ++k) {
        const DrvSnap *x = &copy.ring[(copy.count - 1 - k) % DRV_RING];
        if ((double)x->ms <= t) {
            if (!best || x->ms > best->ms)
                best = x;
        } else if (!after || x->ms < after->ms) {
            after = x;
        }
    }
    if (!best)
        best = after;
    if (!best || fabs((double)best->ms - t) > DRV_STALE)
        return 0;
    memcpy(out, best->c, 8);
    return 1;
}

/* mp.h's g_mp_drive_car: a remote car mp_follow moves now, shown at its owner's time play. */
static void drive_car(MpCar *c, double play, int late) {
    DrvShared *sh = drv_shared();
    if (late || !sh || !c->simable || (g_mp_cop_car && g_mp_cop_car(c->simable) == c))
        return; /* once a frame; a friend's cop copy has no driver here */
    int slot = -1;
    for (int i = 0; i < MP_SLOTS && slot < 0; ++i)
        if (g_mp_seen_slot[i].id == c->id && g_mp_seen_slot[i].mode == 1)
            slot = i;
    const uint32_t car = c->simable - 0x24;
    DrvCar *d = drv_car(c->simable);
    if (!d)
        return;
    d->frame = g_drv_frame;
    uint8_t bytes[8];
    DrvCtl ctl;
    const double now = mp_now();
    if (slot < 0 || !drv_sample(sh, slot, c->id, play, bytes)) {
        drv_release(car, d);
        return;
    }
    if (!drv_setup(car, d, now))
        return;
    drv_decode(bytes, &ctl);
    drv_apply(car, d, &ctl, now);
}

/* mp.h's g_mp_drive: this player's controls for the page, with its pose's time; the test car. */
static void drive_service(MpShared *mp, uint32_t player) {
    DrvShared *sh = drv_shared();
    ++g_drv_frame;
    if (!sh)
        return;
    DrvCtl own;
    const int have = player && mp->state == 1 && drv_read(player, &own);
    __atomic_fetch_add(&sh->seq, 1u, __ATOMIC_SEQ_CST);
    sh->ms = mp->ms;
    if (have)
        drv_encode(&own, sh->own);
    else
        memset(sh->own, 0, sizeof sh->own);
    __atomic_fetch_add(&sh->seq, 1u, __ATOMIC_SEQ_CST);
    memcpy(g_drv_own, sh->own, sizeof g_drv_own);
    /* The test car (mp.h's mp_test runs after this): escort = our controls now; record and replay
     * = our controls kept with each recorded pose, then replayed with it. */
    const int mode = g_mp_test.simable ? mp_test_mode() : 0;
    if (!mode || !have)
        return;
    const uint8_t *bytes = g_drv_own;
    if (mode == 1) {
        if (g_mp_recorded < MP_TAKE && !g_mp_played) {
            memcpy(g_drv_take[g_mp_recorded], g_drv_own, 8);
            return;
        }
        bytes = g_drv_take[g_mp_played % MP_TAKE];
    }
    DrvCar *d = drv_car(g_mp_test.simable);
    const uint32_t car = g_mp_test.simable - 0x24;
    if (!d || !drv_setup(car, d, mp_now()))
        return;
    d->frame = g_drv_frame;
    DrvCtl ctl;
    drv_decode(bytes, &ctl);
    drv_apply(car, d, &ctl, mp_now());
}

static void drive_install(void) {
    if (!g_mp_block || g_api->guest_alloc(g_api, sizeof(DrvShared), &g_drv_block) != POP_OK) {
        log_line("core.nfsc: other players' controls unavailable (no guest memory)");
        g_drv_block = 0;
        return;
    }
    DrvShared *sh = drv_shared();
    if (!sh) {
        g_drv_block = 0;
        return;
    }
    memset(sh, 0, sizeof *sh);
    sh->magic = DRV_MAGIC;
    sh->version = 1;
    g_mp_drive = drive_service;
    g_mp_drive_car = drive_car;
    log_line("core.nfsc: other players' cars follow their owners' controls (drive)");
#ifdef __EMSCRIPTEN__
    MAIN_THREAD_ASYNC_EM_ASM({
        var block = {};
        block.memory = wasmMemory;
        block.ptr = $0;
        globalThis.carbonDriveBlock = block;
        globalThis.dispatchEvent(new Event("carbon-drive"));
    }, (uint32_t)(uintptr_t)sh);
#endif
}
