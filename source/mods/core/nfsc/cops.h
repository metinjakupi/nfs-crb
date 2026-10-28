/* cops.h - a player's police chase, shown in the other players' games (included by nfsc.c after mp.h).
 *
 * Each game runs its own city and its own police; mp.h only sends each player's own car. This
 * file sends the cop cars that chase a player, so the friends near that player see the chase,
 * and sends a friend's hits on those cops back, so ramming them counts in the real chase.
 *
 * THE CHASED PLAYER'S GAME (step 1). Every frame, while the page has the relay link up, the mod
 * lists the cop cars of the player's pursuit (the COP_MAX nearest within COP_RANGE) and writes
 * each one's pose, model, light bar and siren into CopShared, for the page (web/net.js), which
 * sends them to the players near this one about 15 times a second ("cops" frames, the relay
 * header has the format); nothing without a pursuit, one empty frame when it ends.
 *   - The player's car: mp.h's (g_mp_player_simable, so not in the game's own online races).
 *     A PVehicle's IVehicleAI * is at +0x104 (IVehicle::GetAIVehiclePtr 006d8110 reads IVehicle
 *     + 0x4c; IVehicle is PVehicle + 0xb8). AIVehicle keeps its pursuit (IPursuit *) at
 *     IVehicleAI + 0x98 (GetPursuit 0043bd80, the same slot 44 in AIVehicleHuman's table
 *     009c5128 and AIVehicleCopCar's 009c4908): the player's car and the cops chasing it hold
 *     the same IPursuit.
 *   - A cop car: driver class 2 (the class table at 0x00a63370: class 2 with the CHOPPER model is
 *     AIVehicleHelicopter, otherwise AIVehicleCopCar), whose IVehicleAI table is AIVehicleCopCar's
 *     (009c4908; helicopters are left out). AIVehicleCopCar is 0x258 bytes (factory 0043f090,
 *     constructor 0043ef80 over AIVehiclePursuit's 0043cc90); IVehicleAI is at +0x44 and
 *     IPursuitAI at +0x204 (table 009c4820): mInPursuit at IVehicleAI + 0x1c8 (GetInPursuit
 *     0043ce20 reads IPursuitAI + 8), the siren state at + 0x1cc (GetSirenState 0043cf30).
 *   - Lights: PVehicle + 0x150 holds the car's lights (IVehicle::GlareOn 006c0ba0 ORs into
 *     IVehicle + 0x98, GlareOff 006c0bc0 clears, IsGlareOn 006d8080); 0x7000 is the flashing light
 *     bar (LIGHT_COPRED/BLUE/WHITE, named at 009bc74c): AIVehiclePursuit::StartPursuit 004320c0
 *     turns it on, EndPursuit 0040b8e0 off.
 *   - Each cop keeps a number (1-255) while it is in the chase, so friends can tell them apart.
 *
 * A FRIEND'S GAME (step 1). The page puts each cop it gets into one of COP_SLOTS slots, laid out
 * as mp.h's MpSlot: id = the chased player's relay id, mode 1, pad[0] = the cop's number,
 * pad[1] = its flags (COP_F_*). For each the mod builds a car with mp.h's mp_build in the cop's
 * model: driver class 7 like a friend's car (RBRemote with mp.h's collision rule, AIVehicleEmpty,
 * so nothing in this game drives it and this game's police never count it; see-through cars when
 * the page asks for them, class 8, as friends' cars), and moves it with
 * mp.h's mp_follow (the same playback clock and the same push-and-return after a touch). Its
 * light bar follows its owner's. A cop model this game has not loaded: the first try builds it
 * as it is, the second asks for it to be spooled, the third uses the model of a cop car this
 * game has now; never the player's own model (that was mp.h's fallback for friends' cars).
 * Cars go when their slot empties (the chase ended, the cop left it, the friend left or went out
 * of reach), beyond MP_FAR, or with the world.
 *
 * RAMMING THEM (step 2). mp.h solves a touch of our car with one of those cars as with a friend's
 * car (this game's own contact response, then the car is left to the engine and pulled back onto
 * its owner's poses) and tells this file the cop car's change in speed and spin (g_mp_cop_touch).
 * Gathered over a frame, it goes to the cop's owner (CopShared tx, the relay's "cophit"). The
 * owner's game adds it to the real cop's speed and spin, unless its own game already saw that
 * player's car touch that cop (the friend's car is in this game too, and touches the cops here
 * at about the same time): within COP_SEEN before the hit arrived, or COP_HOLD after (mp.h's rule
 * for shared impacts, MP_IMPACT_SEEN / MP_IMPACT_HOLD). g_mp_cop_pair sees those touches: the
 * pair solver 006e9890 with a friend's car and a cop of ours, the cop's speed changed. A touch
 * seen here also damages the cop as any car does (DamageCopCar's own OnImpact, through the
 * collision listener). A hit applied from a message is given to the same damage code: the cop's
 * DamageVehicle::OnImpact 006ed580 (thiscall on the DamageVehicle = IDamageable - 0x44, IDamageable
 * * at PVehicle + 0xfc; args: contact point and normal in the cop's own frame, impulse, closing
 * speed, a SimSurface (constructed once by 0075d7d0 with key 0, the default surface), the other car
 * (0: none)), as OnCollision 006f7830 calls it. Measured in the game's own hits on cops: impulse =
 * about COP_IMPULSE x closing speed; a struck car of the same mass changes speed by about half the
 * closing speed. The game's own rules then decide: damage only above its threshold and not within
 * its pause after the last damage, and at full damage it posts the cop's destruction (the
 * "cop taken out" of the chase, as IDamageable::Destroy 006edae0 does).
 * A cop copy's touches go out at most every COP_HIT_MS, summed (a car resting against a cop touches
 * it every frame), and only when they change its speed or spin by COP_HIT_MIN.
 * The copies themselves take no damage: DamageVehicle::OnImpact 006ed580 and IDamageable::Destroy
 * 006edae0 return at once for them (REPLACE hooks; every other car goes on as before). A copy is
 * built with the cop model's own behaviours, DamageCopCar among them, so without this a friend's
 * rams wrecked the copy here (damage 1.19 after four 35 m/s rams, 2026-10-26) while the real cop
 * drove on: its damage is its owner's business, the real cop is damaged in the chased game.
 *
 * NEAR OUR CAR. mp_follow puts a car that is following its poses exactly where they say, every
 * frame. A cop's owner's game has its cops box in and ram our car there, so their poses can lie
 * inside our car here (it is where our poses said some 150-300 ms earlier, or our car has been
 * pushed since): put there, the two cars overlap and the physics throws ours into the air (seen
 * 2026-10-26: the user's car launched beside two cops; a test tab's car flipped over and over
 * while the other tab's cops boxed it in). The same holds for this game's other cars (its traffic, and
 * its own cops, which step 3's pursuit.h sends after the same friend: two sets of police in one
 * place). So a copy whose place or newest pose is within COP_NEAR_US of our car, or COP_NEAR_CAR of
 * another car here, is never put anywhere: it is switched to mp.h's pull back
 * (MP_RETURNING at full strength), which drives it onto its poses through the physics, so it
 * pushes our car as a car does (at most MP_RETURN_ACCEL) and never lands inside it. mp_follow
 * then only puts it back on its poses from within 3 cm, or from beyond MP_SNAP with its place at
 * least MP_SNAP_CLEAR from ours. A copy is built, or a kept one brought back, only where its owner's
 * newest pose is at least COP_BUILD_CLEAR from our car; until then it waits.
 *
 * KEPT OFF OUR CAR. Driving onto its poses is not enough when they lie inside our car: the pull
 * then shoves our car on and on (several copies boxing it in rolled it over, 2026-10-26). So every
 * place mp_follow aims a copy at (g_mp_follow_target, before it puts or pulls the copy there) is
 * moved out of our car's footprint to COP_GAP from it (each car two circles of COP_CIRCLE_R,
 * COP_CIRCLE_OFF ahead of and behind its middle, side by side on the ground), and its speed toward
 * our car beyond ours is taken out, except for the first COP_HIT_WINDOW after it reaches our car
 * (again after COP_HIT_REARM away): a ram lands as one hit, a car pressing on ours does not push
 * it on (and the bust still comes through pursuit.h). Our car hitting one is unchanged: it is
 * pushed, and for a cop copy the hit goes to its owner.
 * Friends' own cars (mp.h's, race.h's; not see-through ones) get the same rule, for the same
 * reason: a friend playing the police (police.h) rams and boxes our car in his game, where our car
 * cannot move, and here his poses lay inside ours and wedged it (2026-10-26). Kept speed into ours
 * made it hop instead (a car pushed against ours in its owner's game was put at our side, moving
 * into it, every frame), hence the window.
 *
 * PASSING THROUGH. Even kept off and never driven at our car, copies boxing our car in still threw
 * it about (2026-10-26, two test tabs: 18 hops and roll-overs with copies, none without): many
 * bodies pressed together, one of them following poses from a game where our car could not move.
 * So a copy and our car decide how they meet when they first come within COP_MEET of each other
 * (footprints as above): if our car is the one closing in (at least COP_RAM_SPEED, and faster than
 * the copy closes on us) it is a ram and they stay solid, so it lands as any hit and goes to the
 * cop's owner (step 2); otherwise the copy is coming at us and the two pass through each other
 * (006e95a0, the first step of a contact, the one that pushes the bodies apart, answers "no
 * contact" for that pair, and its caller then skips the solve too) until they are apart again. The copy still collides with everything else.
 *
 * TAKING THEM AWAY. Removing (Kill, as mp.h removes a friend's car) a copy of a cop model ended
 * the game now and then, soon after the Kill (ExitProcess 0xC0000005 or 255, a pure virtual call;
 * 2026-10-26, class 7 copies: about 1 in 70 to 1 in 700 removals in stress tests that built and
 * removed 8 copies every 3 s; as driver class 4 (none) nearly every test tab within 3 removals;
 * friends' cars of racer models: 0 in 56). Turning the light bar off first and spreading the
 * Kills, a different sound behaviour, or taking the copy out of the world (physics off, far
 * below) and killing it seconds later did not help, so a copy out of a chase is not removed: it
 * is kept (COP_SPARES of them, light bar off, put COP_PARK_DOWN below where it was and parked with
 * its physics off, as police.h parks the player's own car: not simulated, not drawn; taking no
 * damage) and the next cop of its model takes it (cop_spare_take: physics on, put at its place).
 * Kept copies re-placed under our car every frame instead, physics on, made our car hop (up to
 * 6 m/s, every second or two, nothing near it; 2026-10-26, gone with the physics off). Only beyond COP_SPARES, or
 * when the class changes (see-through cars: a test switch now), is one removed. The world unloading removes them all.
 */

#define COP_MAGIC 0x31504f43u /* "COP1" */
#define COP_CLASS 2u          /* driver class: police (cars and helicopters) */
#define COP_AI 0x104u         /* PVehicle: IVehicleAI * */
#define COP_AI_TABLE 0x009c4908u /* AIVehicleCopCar's IVehicleAI table */
#define COP_AI_PURSUIT 0x98u  /* IVehicleAI: AIVehicle::mPursuit, an IPursuit * */
#define COP_AI_CHASING 0x1c8u /* IVehicleAI: AIVehiclePursuit::mInPursuit (byte) */
#define COP_AI_SIREN 0x1ccu   /* IVehicleAI: AIVehiclePursuit siren state */
#define COP_GLARE 0x150u      /* PVehicle: light bits */
#define COP_LIGHTS 0x7000u    /* the light bar */
#define COP_MAX 8             /* cops sent per player */
#define COP_SLOTS 16          /* friends' cops shown here */
#define COP_HITS 16
#define COP_RANGE 400.0f      /* metres: a cop farther than this from the player is not sent */
#define COP_SEEN 600.0        /* ms: see RAMMING THEM */
#define COP_HOLD 250.0
#define COP_RETRY 240         /* frames between rounds of build attempts */
#define COP_HIT_MS 100.0      /* a cop copy's touches go to its owner at most this often, summed */
#define COP_HIT_MIN 0.3f      /* and only a change in speed (m/s) or spin (rad/s) at least this big */
#define COP_DAMAGE 0x006ed580u /* DamageVehicle::OnImpact, thiscall, RET 0x18 (see RAMMING THEM) */
#define COP_DESTROY 0x006edae0u /* IDamageable::Destroy, thiscall on the IDamageable, RET 0 */
#define COP_SURFACE 0x0075d7d0u /* SimSurface constructor, thiscall (key; 0 = the default surface), RET 4 */
#define COP_IMPULSE 2050.0f   /* the game's own car-to-car hits on cops: impulse / closing speed */
#define COP_SPARES 8          /* copies out of a chase kept out of sight for the next cops (see TAKING THEM AWAY) */
#define COP_PARK_DOWN 400.0f  /* metres below where they were that they are kept */
#define COP_PHYSICS 0x006d6950u /* IVehicle +0x38 SetPhysicsMode, thiscall (IVehicle *, int): 0 parks a car (not simulated, not drawn) */
#define COP_NEAR_US 10.0f     /* metres: a copy this near our car is driven onto its poses, never put there */
#define COP_NEAR_CAR 7.0f     /* and this near any other car of this game (traffic, its own cops) */
#define COP_CIRCLE_R 1.05f    /* a car's footprint for keeping copies off ours: two circles this wide */
#define COP_CIRCLE_OFF 1.6f   /* this far ahead of and behind its middle (5.3 m long in all) */
#define COP_GAP 0.3f          /* metres: the least gap a copy's place keeps to our car */
#define COP_TOUCH_RANGE 6.5f  /* metres between middles: a copy this near our car never moves toward it */
#define COP_CONTACT 0x006e95a0u /* a contact between two bodies, cdecl (RigidBody *a, flag, RigidBody *b, flag, ...8 args):
                                 * 0 = none, and its caller (at 006e9c6e) drops the pair: no push apart, no solve */
#define COP_MEET 0.6f         /* metres between footprints: closer, a copy and our car have "met" (see PASSING THROUGH) */
#define COP_RAM_SPEED 1.0f    /* m/s: our car closing on a copy at least this fast (and faster than it on us) rams it */
#define COP_HIT_WINDOW 150.0  /* ms: a car's speed into ours is kept this long after it reaches ours */
#define COP_HIT_REARM 400.0   /* and again once it has been off ours this long */
#define COP_BUILD_CLEAR 8.0f  /* metres: a copy is built (or a kept one brought) only this far from our car */
#define COP_F_LIGHTS 1u       /* flags: the light bar is on */
#define COP_F_CHASING 2u      /*        the cop is chasing (mInPursuit) */
/* bits 4-7: the siren state + 1 (0 = not known) */

typedef struct CopOut { /* 0x40 bytes: one cop chasing this player */
    uint32_t id;    /* its number, 1-255 */
    uint32_t key;   /* its model key (mp_model_key) */
    uint32_t flags; /* COP_F_* */
    MpPose pose;
} CopOut;

typedef struct CopHit { /* 0x40 bytes */
    uint32_t who;   /* tx: the cop's owner (relay id); rx: the player whose car hit it */
    uint32_t cop;   /* the cop's number in its owner's game */
    uint32_t serial, key; /* key: the cop's model key in its owner's game */
    uint32_t ms;    /* tx: our physics clock at the hit; rx: arrival, in our physics clock */
    float at[3], dv[3], dw[3]; /* where the cop car was, the change in its speed and spin */
    uint32_t spare[2];
} CopHit;

typedef struct CopShared {
    uint32_t magic, version; /* COP_MAGIC, 1 */
    uint32_t page;           /* the page: nonzero while its relay link carries cops */
    uint32_t seq;            /* the game's part, ms to out[]: odd while written */
    uint32_t ms;             /* the physics clock (mp_ms) of these poses */
    uint32_t count;          /* cops in out[]: 0 = no pursuit */
    uint32_t frame, spare;
    CopOut out[COP_MAX];
    MpSlot slots[COP_SLOTS]; /* the page's: friends' cops (see A FRIEND'S GAME) */
    uint32_t tx_head, tx_tail, rx_head, rx_tail;
    CopHit tx[COP_HITS], rx[COP_HITS]; /* single-producer/single-consumer queues */
} CopShared;
/* The page reads these by offset (web/net.js). */
_Static_assert(sizeof(CopOut) == 0x40 && sizeof(CopHit) == 0x40 && offsetof(CopShared, out) == 0x20 &&
                   offsetof(CopShared, slots) == 0x220 && offsetof(CopShared, tx_head) == 0x1220 &&
                   sizeof(CopShared) == 0x1a30,
               "CopShared layout");

typedef struct CopTrack { /* a cop chasing this player, as sent */
    uint32_t car;         /* PVehicle, 0 = free */
    uint32_t number;
    int seen;
    double touched[MP_SLOTS]; /* our ms: its last touch by the car of g_mp_remote[i] here */
    CopHit held;          /* a received hit waiting for our own touch */
    int holding, from;
    double held_at;
} CopTrack;

typedef struct CopCar { /* a friend's cop shown here */
    MpCar car;            /* id = its owner, key = its model in its owner's game */
    uint32_t number;      /* its number in its owner's game */
    uint32_t built;       /* the model it was built in */
    uint32_t flags;       /* COP_F_* */
    int round;            /* rounds of build attempts */
    CopHit hit;           /* touches with our car not sent yet, for its owner */
    int pending;
    double sent_at;       /* our ms: the last hit sent */
    int met;              /* with our car: 0 apart, 1 solid (we ran into it), 2 passing through (see PASSING THROUGH) */
} CopCar;

static uint32_t g_cop_block;
static uint32_t g_cop_buf; /* 0x30 guest bytes: a SimSurface at +0, a point at +0x10, a normal at +0x20 */
static uint32_t g_cop_hooks[4];
static CopTrack g_cop_track[COP_MAX];
static int g_cop_tracked; /* tracks in use */
static uint32_t g_cop_next, g_cop_serial;
static CopCar g_cop_cars[COP_SLOTS];
typedef struct CopSpare { uint32_t simable, kind, key; } CopSpare; /* a copy kept for reuse */
static CopSpare g_cop_spares[COP_SPARES];
static uint32_t g_cop_reused; /* copies taken from the kept ones (for the log) */
static MpSlot g_cop_seen[COP_SLOTS];

/* The driver class a copy is built with now: that of friends' cars (mp.h). */
static uint32_t cop_kind(void) {
    return mp_kind();
}

static CopShared *cop_shared(void) {
    void *p = NULL;
    return g_cop_block && g_api->guest_ptr(g_api, g_cop_block, sizeof(CopShared), &p) == POP_OK ? p : NULL;
}

/* Whether car is one of the cop cars (not a helicopter) of pursuit. */
static int cop_of(uint32_t car, uint32_t pursuit) {
    if (get_u32(car + 0x148) != COP_CLASS)
        return 0;
    const uint32_t ai = get_u32(car + COP_AI);
    return ai && get_u32(ai) == COP_AI_TABLE && get_u32(ai + COP_AI_PURSUIT) == pursuit;
}

static uint32_t cop_flags(uint32_t car) {
    const uint32_t ai = get_u32(car + COP_AI);
    const uint32_t siren = (get_u32(ai + COP_AI_SIREN) + 1u) & 15u;
    return (get_u32(car + COP_GLARE) & COP_LIGHTS ? COP_F_LIGHTS : 0) |
           (get_u32(ai + COP_AI_CHASING) & 0xffu ? COP_F_CHASING : 0) | siren << 4;
}

/* ---- the chased player's game ---- */

/* The cops of the player's pursuit, nearest first, into found[]; returns how many. */
static int cop_find(uint32_t player, uint32_t found[COP_MAX]) {
    const uint32_t ai = player ? get_u32(player + COP_AI) : 0;
    const uint32_t pursuit = ai ? get_u32(ai + COP_AI_PURSUIT) : 0;
    const uint32_t own = player ? mp_state(get_u32(player + 0x70)) : 0;
    if (!pursuit || !own)
        return 0;
    float here[3], dist[COP_MAX];
    for (int k = 0; k < 3; ++k)
        here[k] = bits_float(get_u32(own + 0x10 + 4u * k));
    int n = 0;
    uint32_t node = get_u32(MP_CARS);
    for (int guard = 0; node && node != MP_CARS && guard < 64; ++guard, node = get_u32(node)) {
        const uint32_t car = node - 0xd0, state = car != player ? mp_state(get_u32(car + 0x70)) : 0;
        if (!state || !cop_of(car, pursuit))
            continue;
        float d = 0;
        for (int k = 0; k < 3; ++k) {
            const float x = bits_float(get_u32(state + 0x10 + 4u * k)) - here[k];
            d += x * x;
        }
        if (!(d <= COP_RANGE * COP_RANGE))
            continue;
        int at = n < COP_MAX ? n++ : COP_MAX;
        if (at == COP_MAX && d >= dist[COP_MAX - 1])
            continue;
        if (at == COP_MAX)
            at = COP_MAX - 1;
        for (; at > 0 && dist[at - 1] > d; --at) {
            found[at] = found[at - 1];
            dist[at] = dist[at - 1];
        }
        found[at] = car;
        dist[at] = d;
    }
    return n;
}

/* Keeps each cop's number while it stays in the chase; a new one takes a number not in use. */
static CopTrack *cop_track(uint32_t car) {
    CopTrack *free_track = NULL;
    for (int i = 0; i < COP_MAX; ++i) {
        if (g_cop_track[i].car == car)
            return &g_cop_track[i];
        if (!g_cop_track[i].car && !free_track)
            free_track = &g_cop_track[i];
    }
    if (!free_track)
        return NULL;
    for (int guard = 0; guard < 256; ++guard) {
        g_cop_next = g_cop_next % 255u + 1u;
        int used = 0;
        for (int i = 0; i < COP_MAX; ++i)
            used |= g_cop_track[i].car && g_cop_track[i].number == g_cop_next;
        if (!used)
            break;
    }
    memset(free_track, 0, sizeof *free_track);
    free_track->car = car;
    free_track->number = g_cop_next;
    return free_track;
}

/* This player's chase, for the page to send. */
static void cop_publish(CopShared *sh, uint32_t player) {
    static uint32_t last_count;
    uint32_t found[COP_MAX];
    const int n = __atomic_load_n(&sh->page, __ATOMIC_ACQUIRE) ? cop_find(player, found) : 0;
    for (int i = 0; i < COP_MAX; ++i)
        g_cop_track[i].seen = 0;
    CopTrack *tracks[COP_MAX];
    for (int i = 0; i < n; ++i) {
        tracks[i] = cop_track(found[i]);
        if (tracks[i])
            tracks[i]->seen = 1;
    }
    g_cop_tracked = 0;
    for (int i = 0; i < COP_MAX; ++i) {
        if (g_cop_track[i].car && !g_cop_track[i].seen)
            g_cop_track[i].car = 0;
        g_cop_tracked += g_cop_track[i].car != 0;
    }
    __atomic_fetch_add(&sh->seq, 1u, __ATOMIC_SEQ_CST);
    sh->ms = mp_ms();
    sh->frame = g_mp_frame;
    uint32_t count = 0;
    for (int i = 0; i < n; ++i) {
        CopOut *o = &sh->out[count];
        const uint32_t state = mp_state(get_u32(found[i] + 0x70));
        if (!tracks[i] || !state)
            continue;
        mp_read(state, &o->pose);
        if (!mp_pose_ok(&o->pose))
            continue;
        o->id = tracks[i]->number;
        o->key = mp_model_key(found[i]);
        o->flags = cop_flags(found[i]);
        ++count;
    }
    sh->count = count;
    __atomic_fetch_add(&sh->seq, 1u, __ATOMIC_SEQ_CST);
    if (!count != !last_count) {
        char line[96];
        snprintf(line, sizeof line, count ? "core.nfsc: cops: in a chase, %u cops sent to friends"
                                          : "core.nfsc: cops: chase over (was %u cops)", count ? count : last_count);
        log_line(line);
    }
    last_count = count;
}

static int cop_vector_ok(const float v[3], float limit) {
    float length = 0;
    for (int k = 0; k < 3; ++k) {
        if (!isfinite(v[k]))
            return 0;
        length += v[k] * v[k];
    }
    return length <= limit * limit;
}

/* A friend's hit on a cop of ours, to the cop's own damage code (RAMMING THEM in the header). */
static void cop_damage(uint32_t car, const CopHit *hit) {
    const uint32_t damageable = get_u32(car + 0xfc), state = mp_state(get_u32(car + 0x70));
    float dv = 0;
    for (int k = 0; k < 3; ++k)
        dv += hit->dv[k] * hit->dv[k];
    dv = sqrtf(dv);
    if (!damageable || !state || dv < 1.0f)
        return;
    if (!g_cop_buf) {
        const uint32_t key = 0;
        if (g_api->guest_alloc(g_api, 0x30, &g_cop_buf) != POP_OK) {
            g_cop_buf = 0;
            return;
        }
        for (uint32_t i = 0; i < 0x30; i += 4)
            mp_put(g_cop_buf + i, 0);
        mp_call(COP_SURFACE, g_cop_buf, &key, 1);
        if (!get_u32(g_cop_buf)) {
            log_line("core.nfsc: cops: no surface for the damage of friends' hits");
            return;
        }
    }
    /* Where it was hit, in its own frame (rows: right +0x70, up +0x80, forward +0x90): the side the
     * push came from, at the edge of a car's box. */
    float n[3];
    for (int r = 0; r < 3; ++r) {
        n[r] = 0;
        for (int k = 0; k < 3; ++k)
            n[r] -= hit->dv[k] / dv * bits_float(get_u32(state + 0x70 + 0x10u * r + 4u * k));
    }
    put_float(g_cop_buf + 0x10, n[0] * 0.9f);
    put_float(g_cop_buf + 0x14, -0.7f);
    put_float(g_cop_buf + 0x18, n[2] * 2.4f);
    for (int k = 0; k < 3; ++k)
        put_float(g_cop_buf + 0x20 + 4u * k, n[k]);
    const float speed = 2.0f * dv;
    const uint32_t args[6] = {g_cop_buf + 0x10, g_cop_buf + 0x20, float_bits(COP_IMPULSE * speed), float_bits(speed),
                              g_cop_buf, 0};
    const float before = bits_float(get_u32(damageable + 0x34));
    mp_call(COP_DAMAGE, damageable - 0x44, args, 6);
    const float after = bits_float(get_u32(damageable + 0x34));
    if (after != before) {
        char line[128];
        snprintf(line, sizeof line, "core.nfsc: cops: cop %u damaged by player %u's hit: %.2f -> %.2f%s", hit->cop, hit->who,
                 before, after, after >= 1.0f ? " (taken out)" : "");
        log_line(line);
    }
}

/* A friend's hit on a cop of ours: its speed and spin change, the body woken as the solver does. */
static void cop_push(uint32_t car, const CopHit *hit) {
    const uint32_t body = get_u32(car + 0x70), state = mp_state(body);
    if (!state)
        return;
    MpPose own;
    mp_read(state, &own);
    for (int k = 0; k < 3; ++k) {
        put_float(state + 0x20 + 4u * k, own.v[k] + hit->dv[k]);
        put_float(state + 0x30 + 4u * k, own.w[k] + hit->dw[k]);
    }
    g_api->guest_write_u8(g_api, state + 0x5d, 0);
    g_api->guest_write_u8(g_api, state + 0x1c, (get_u32(state + 0x1c) & 0xffu) | 0x40u);
    mp_call(MP_WAKE_BODY, body - 0x40u, NULL, 0);
}

static void cop_hit_log(const char *what, const CopHit *hit) {
    static int lines;
    if (lines++ < 40) {
        char line[160];
        snprintf(line, sizeof line, "core.nfsc: cops: hit %u on cop %u by player %u (dv %.1f %.1f %.1f): %s",
                 hit->serial, hit->cop, hit->who, hit->dv[0], hit->dv[1], hit->dv[2], what);
        log_line(line);
    }
}

/* Friends' hits on our cops (RAMMING THEM in the header). */
static void cop_received(CopShared *sh, uint32_t player) {
    const double now = mp_now();
    uint32_t tail = __atomic_load_n(&sh->rx_tail, __ATOMIC_RELAXED);
    const uint32_t head = __atomic_load_n(&sh->rx_head, __ATOMIC_ACQUIRE);
    for (; tail != head; ++tail) {
        const CopHit hit = sh->rx[tail % COP_HITS];
        CopTrack *t = NULL;
        for (int i = 0; i < COP_MAX; ++i)
            if (g_cop_track[i].car && g_cop_track[i].number == hit.cop)
                t = &g_cop_track[i];
        const uint32_t state = t ? mp_state(get_u32(t->car + 0x70)) : 0;
        if (!player || !state || hit.key != mp_model_key(t->car) || (uint32_t)(mp_ms() - hit.ms) > 1000u ||
            !cop_vector_ok(hit.dv, 100.0f) || !cop_vector_ok(hit.dw, 50.0f)) {
            cop_hit_log("not for a cop chasing us now", &hit);
            continue;
        }
        MpPose cop;
        mp_read(state, &cop);
        float apart[3];
        for (int k = 0; k < 3; ++k)
            apart[k] = cop.p[k] - hit.at[k];
        if (!cop_vector_ok(apart, 25.0f)) {
            cop_hit_log("too far from where the cop is", &hit);
            continue;
        }
        int from = -1;
        for (int i = 0; i < MP_SLOTS; ++i)
            if (g_mp_remote[i].id == hit.who && g_mp_remote[i].simable)
                from = i;
        if (from >= 0 && t->touched[from] > 0 && now - t->touched[from] < COP_SEEN) {
            cop_hit_log("seen here already", &hit);
            continue;
        }
        if (t->holding) { /* two in one wait: one push of both */
            for (int k = 0; k < 3; ++k) {
                t->held.dv[k] += hit.dv[k];
                t->held.dw[k] += hit.dw[k];
            }
            t->held.serial = hit.serial;
        } else {
            t->held = hit;
            t->held_at = now;
            t->holding = 1;
            t->from = from;
        }
    }
    __atomic_store_n(&sh->rx_tail, tail, __ATOMIC_RELEASE);
    for (int i = 0; i < COP_MAX; ++i) {
        CopTrack *t = &g_cop_track[i];
        if (!t->holding)
            continue;
        if (!t->car || !player) {
            t->holding = 0;
        } else if (t->from >= 0 && t->touched[t->from] >= t->held_at) {
            t->holding = 0;
            cop_hit_log("seen here too", &t->held);
        } else if (now - t->held_at >= COP_HOLD) {
            const uint32_t state = mp_state(get_u32(t->car + 0x70));
            float v0 = 0, v1 = 0;
            for (int k = 0; state && k < 3; ++k)
                v0 += bits_float(get_u32(state + 0x20 + 4u * k)) * bits_float(get_u32(state + 0x20 + 4u * k));
            cop_push(t->car, &t->held);
            cop_damage(t->car, &t->held);
            for (int k = 0; state && k < 3; ++k)
                v1 += bits_float(get_u32(state + 0x20 + 4u * k)) * bits_float(get_u32(state + 0x20 + 4u * k));
            t->holding = 0;
            char what[80];
            snprintf(what, sizeof what, "applied to the cop (speed %.1f -> %.1f m/s)", sqrtf(v0), sqrtf(v1));
            cop_hit_log(what, &t->held);
        }
    }
}

/* mp.h's pair solver, for pairs without our car: a friend's car here touching a cop of ours. */
static int g_cop_pair_track = -1, g_cop_pair_from = -1;
static float g_cop_pair_v[6];

static void cop_pair(uint32_t a, uint32_t b, int after) {
    if (!after) {
        g_cop_pair_track = -1;
        if (!g_cop_tracked)
            return;
        const uint32_t owner[2] = {get_u32(a + 0x2c), get_u32(b + 0x2c)};
        for (int side = 0; side < 2 && g_cop_pair_track < 0; ++side)
            for (int i = 0; i < COP_MAX; ++i) {
                if (!g_cop_track[i].car || g_cop_track[i].car + 0x24u != owner[side])
                    continue;
                for (int j = 0; j < MP_SLOTS; ++j)
                    if (g_mp_remote[j].simable && g_mp_remote[j].simable == owner[1 - side]) {
                        g_cop_pair_track = i;
                        g_cop_pair_from = j;
                    }
                break;
            }
        const uint32_t state =
            g_cop_pair_track >= 0 ? mp_state(get_u32(g_cop_track[g_cop_pair_track].car + 0x70)) : 0;
        if (!state) {
            g_cop_pair_track = -1;
            return;
        }
        for (int k = 0; k < 3; ++k) {
            g_cop_pair_v[k] = bits_float(get_u32(state + 0x20 + 4u * k));
            g_cop_pair_v[3 + k] = bits_float(get_u32(state + 0x30 + 4u * k));
        }
        return;
    }
    if (g_cop_pair_track < 0)
        return;
    CopTrack *t = &g_cop_track[g_cop_pair_track];
    const uint32_t state = t->car ? mp_state(get_u32(t->car + 0x70)) : 0;
    g_cop_pair_track = -1;
    float change = 0;
    for (int k = 0; state && k < 3; ++k)
        change += fabsf(bits_float(get_u32(state + 0x20 + 4u * k)) - g_cop_pair_v[k]) +
                  fabsf(bits_float(get_u32(state + 0x30 + 4u * k)) - g_cop_pair_v[3 + k]);
    if (change >= 0.0001f)
        t->touched[g_cop_pair_from] = mp_now();
}

/* ---- a friend's game ---- */

static CopCar *cop_car(const MpCar *c) {
    for (int i = 0; i < COP_SLOTS; ++i)
        if (&g_cop_cars[i].car == c)
            return &g_cop_cars[i];
    return NULL;
}

static MpCar *cop_car_of(uint32_t owner) {
    for (int i = 0; i < COP_SLOTS; ++i)
        if (g_cop_cars[i].car.simable == owner)
            return &g_cop_cars[i].car;
    return NULL;
}

/* Whether owner is a kept copy. */
static int cop_spare_of(uint32_t owner) {
    for (int i = 0; owner && i < COP_SPARES; ++i)
        if (g_cop_spares[i].simable == owner)
            return 1;
    return 0;
}

/* A car's physics on (1) or off (0, parked: not simulated, not drawn), as police.h does it. */
static void cop_physics(uint32_t car, uint32_t on) {
    if (on && get_u32(car + 0xb8 + 0xa8))
        return; /* as Activate (006c0be0) */
    mp_call(COP_PHYSICS, car + 0xb8, &on, 1);
}

/* A copy goes: kept for the next cop of its model (light bar off, parked with its physics off,
 * see TAKING THEM AWAY), or, with COP_SPARES kept already, removed (mp_drop: Kill). */
static void cop_drop(CopCar *cc, const char *why) {
    MpCar *c = &cc->car;
    if (c->simable && why) {
        char line[128];
        snprintf(line, sizeof line, "core.nfsc: cops: cop %u of player %u removed (%s)", cc->number, c->id, why);
        log_line(line);
    }
    for (int i = 0; c->simable && i < COP_SPARES; ++i) {
        CopSpare *sp = &g_cop_spares[i];
        if (sp->simable || !mp_listed(c->simable - 0x24) || !mp_kind_kept(c->simable - 0x24, c->kind))
            continue;
        const uint32_t car = c->simable - 0x24, state = mp_state(get_u32(car + 0x70));
        mp_put(car + COP_GLARE, get_u32(car + COP_GLARE) & ~COP_LIGHTS);
        if (state) { /* out of the way, then parked as police.h parks the player's own car */
            MpPose park;
            mp_read(state, &park);
            if (mp_pose_ok(&park)) {
                park.p[1] -= COP_PARK_DOWN;
                memset(park.v, 0, sizeof park.v);
                memset(park.w, 0, sizeof park.w);
                mp_place(c->simable, &park);
            }
        }
        cop_physics(car, 0);
        sp->simable = c->simable;
        sp->kind = c->kind;
        sp->key = cc->built;
        c->simable = 0;
        c->pending = c->holding = 0;
        c->follow = MP_FOLLOWING;
        break;
    }
    mp_drop(c, NULL); /* nothing left to do when it was kept */
    cc->pending = 0;
}

/* Whether a kept copy is still ours: in the car list, its class, and not (its memory given to)
 * another car of ours or of mp.h's. */
static int cop_spare_ok(const CopSpare *sp) {
    const uint32_t simable = sp->simable;
    if (!simable || !mp_listed(simable - 0x24) || !mp_kind_kept(simable - 0x24, sp->kind) ||
        cop_car_of(simable) || g_mp_test.simable == simable || (g_mp_racer_car && g_mp_racer_car(simable)))
        return 0;
    for (int i = 0; i < MP_SLOTS; ++i)
        if (g_mp_remote[i].simable == simable)
            return 0;
    return 1;
}

/* A kept copy of model key and class kind for a new cop, or 0. */
static uint32_t cop_spare_take(uint32_t key, uint32_t kind) {
    for (int i = 0; i < COP_SPARES; ++i) {
        CopSpare *sp = &g_cop_spares[i];
        if (sp->simable && sp->key == key && sp->kind == kind && cop_spare_ok(sp)) {
            const uint32_t simable = sp->simable;
            sp->simable = 0;
            cop_physics(simable - 0x24, 1);
            return simable;
        }
    }
    return 0;
}

/* The kept copies, every frame: one gone with the world is forgotten, one of a class no longer
 * used removed. They stay parked where they were put (cop_drop). */
static void cop_spares(uint32_t player) {
    (void)player;
    for (int i = 0; i < COP_SPARES; ++i) {
        CopSpare *sp = &g_cop_spares[i];
        if (!sp->simable)
            continue;
        if (!cop_spare_ok(sp)) {
            static unsigned told;
            if (++told <= 20u) {
                char line[96];
                snprintf(line, sizeof line, "core.nfsc: cops: a kept copy is gone (with the world)");
                log_line(line);
            }
            sp->simable = 0;
            continue;
        }
        if (sp->kind != cop_kind()) {
            mp_kill(sp->simable);
            sp->simable = 0;
        }
    }
}

/* A cop model this game has now (one of its own cop cars), other than not, or 0. */
static uint32_t cop_model_here(uint32_t not) {
    uint32_t node = get_u32(MP_CARS);
    for (int guard = 0; node && node != MP_CARS && guard < 64; ++guard, node = get_u32(node)) {
        const uint32_t car = node - 0xd0, ai = get_u32(car + COP_AI);
        const uint32_t key = get_u32(car + 0x148) == COP_CLASS && ai && get_u32(ai) == COP_AI_TABLE ? mp_model_key(car) : 0;
        if (key && key != not)
            return key;
    }
    return 0;
}

/* Our car touched one of these cars (mp.h's pair solver): gathered for its owner. */
static void cop_touch(MpCar *c, const MpPose *before, const MpPose *after) {
    CopCar *cc = cop_car(c);
    if (!cc)
        return;
    if (!cc->pending) {
        memset(&cc->hit, 0, sizeof cc->hit);
        memcpy(cc->hit.at, before->p, sizeof cc->hit.at);
        cc->pending = 1;
    }
    for (int k = 0; k < 3; ++k) {
        cc->hit.dv[k] += after->v[k] - before->v[k];
        cc->hit.dw[k] += after->w[k] - before->w[k];
    }
}

/* The touches gathered, to their owners (at most every COP_HIT_MS a cop, see the header). */
static void cop_send_hits(CopShared *sh) {
    const double now = mp_now();
    for (int i = 0; i < COP_SLOTS; ++i) {
        CopCar *cc = &g_cop_cars[i];
        if (!cc->pending || now - cc->sent_at < COP_HIT_MS)
            continue;
        if (!cc->car.simable || !cc->car.id ||
            (cop_vector_ok(cc->hit.dv, COP_HIT_MIN) && cop_vector_ok(cc->hit.dw, COP_HIT_MIN))) {
            cc->pending = 0;
            continue;
        }
        const uint32_t head = __atomic_load_n(&sh->tx_head, __ATOMIC_RELAXED);
        if (head - __atomic_load_n(&sh->tx_tail, __ATOMIC_ACQUIRE) >= COP_HITS)
            break;
        cc->hit.who = cc->car.id;
        cc->hit.cop = cc->number;
        cc->hit.serial = ++g_cop_serial;
        cc->hit.key = cc->car.key;
        cc->hit.ms = mp_ms();
        sh->tx[head % COP_HITS] = cc->hit;
        cc->pending = 0;
        cc->sent_at = now;
        __atomic_store_n(&sh->tx_head, head + 1u, __ATOMIC_RELEASE);
        if (g_cop_serial <= 20u) {
            char line[128];
            snprintf(line, sizeof line, "core.nfsc: cops: hit %u on cop %u of player %u sent (dv %.1f %.1f %.1f)",
                     g_cop_serial, cc->number, cc->car.id, cc->hit.dv[0], cc->hit.dv[1], cc->hit.dv[2]);
            log_line(line);
        }
    }
}

/* Friends' cops from the page's slots: built near us, moved in mp_place_all (cop_place). */
static void cop_cars(CopShared *sh, uint32_t player) {
    const double now = mp_now();
    const uint32_t state = player ? mp_state(get_u32(player + 0x70)) : 0;
    float here[3] = {0, 0, 0};
    for (int k = 0; k < 3 && state; ++k)
        here[k] = bits_float(get_u32(state + 0x10 + 4u * k));
    for (int i = 0; i < COP_SLOTS; ++i) {
        MpSlot *src = &sh->slots[i], copy;
        const uint32_t before = __atomic_load_n(&src->seq, __ATOMIC_ACQUIRE);
        if (!(before & 1)) {
            memcpy(&copy, src, sizeof copy);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (__atomic_load_n(&src->seq, __ATOMIC_ACQUIRE) == before)
                g_cop_seen[i] = copy;
        }
        const MpSlot *s = &g_cop_seen[i];
        CopCar *cc = &g_cop_cars[i];
        MpCar *c = &cc->car;
        /* Gone with the world, or its memory given to another car (mp_remotes). */
        if (c->simable && (!mp_listed(c->simable - 0x24) || get_u32(c->simable - 0x24 + 0x148) != c->kind))
            c->simable = 0;
        const int active = s->id && s->mode == 1 && s->pad[0] && state;
        const int same = c->id == s->id && cc->number == s->pad[0] && c->key == s->key;
        if (c->simable && (!active || !same || c->kind != cop_kind()))
            cop_drop(cc, active ? "another cop in its slot" : "out of the chase or out of reach");
        if (!active) {
            c->id = 0;
            cc->number = 0;
            continue;
        }
        if (!same) {
            c->id = s->id;
            c->key = s->key;
            cc->number = s->pad[0];
            c->wait = 0;
            c->tries = 0;
            cc->round = 0;
            c->taken = s->count + 1; /* start over in mp_take */
            c->follow = MP_FOLLOWING;
            c->contact_at = 0;
            cc->pending = 0;
            cc->met = 0;
        }
        cc->flags = s->pad[1];
        c->contacts = 0; /* mp.h's shared impacts are for players' own cars */
        mp_take(c, s, now);
        if (!c->kept)
            continue;
        const MpPose *last = &c->pose[c->kept - 1];
        float d2 = 0;
        for (int k = 0; k < 3; ++k)
            d2 += (last->p[k] - here[k]) * (last->p[k] - here[k]);
        float near = (MP_NEAR_FROM - sqrtf(d2)) / (MP_NEAR_FROM - MP_NEAR_TO); /* as mp_remotes */
        near = c->kind == MP_GHOST || near < 0 ? 0 : near > 1 ? 1 : near;
        c->lag = MP_DELAY + (MP_NEAR_DELAY - MP_DELAY) * near;
        if (c->simable) {
            if (d2 > MP_FAR * MP_FAR)
                cop_drop(cc, "far away");
            continue;
        }
        if (d2 > MP_NEAR * MP_NEAR || (c->wait && --c->wait))
            continue;
        if (d2 < COP_BUILD_CLEAR * COP_BUILD_CLEAR) { /* not on our car (see NEAR OUR CAR) */
            c->wait = 6;
            continue;
        }
        /* Its own model, then spooled, then a cop model this game has (see the header). */
        const int step = c->tries % 3;
        const uint32_t key = step < 2 ? s->key : cop_model_here(s->key); /* 0 = model 0 is the player's own */
        ++c->tries;
        if (!key) { /* no cop car here to borrow a model from: the next round */
            c->wait = step == 2 ? COP_RETRY : 2;
            cc->round += step == 2;
            continue;
        }
        const uint32_t flags = 2u | 8u | (step == 1 ? 1u : 0u);
        c->kind = cop_kind();
        c->follow = MP_FOLLOWING;
        memcpy(c->drawn, last->p, sizeof c->drawn);
        cc->met = 0;
        c->simable = cop_spare_take(key, c->kind);
        if (c->simable) {
            mp_place(c->simable, last);
            cc->built = key;
            if (++g_cop_reused <= 40u || g_cop_reused % 100u == 0) {
                char line[128];
                snprintf(line, sizeof line, "core.nfsc: cops: cop %u of player %u (model %08x) took a kept copy (%u so far)",
                         cc->number, c->id, key, g_cop_reused);
                log_line(line);
            }
            continue;
        }
        c->simable = mp_build(player, c->kind, key, last->p, flags, 0);
        cc->built = key;
        if (cc->round < 1 || c->simable) {
            char line[160];
            snprintf(line, sizeof line, "core.nfsc: cops: cop %u of player %u (model %08x) %s%s%s", cc->number, c->id,
                     s->key, c->simable ? "built" : "refused", flags & 1 ? " (spooled)" : "",
                     key != s->key ? " in a cop model this game has (its own is not here)" : "");
            log_line(line);
        }
        if (!c->simable) {
            c->wait = step == 2 ? COP_RETRY : 30;
            if (step == 2)
                ++cc->round;
        }
    }
}

/* Our car for cop_keep_off, read each cop_place: place, speed, forward on the ground. */
static struct { int have; float p[3], v[3], f[2]; } g_cop_us;
static int g_cop_axis = -1, g_cop_axis_sign = 1; /* which body axis a pose's rotation turns to forward */
static uint32_t g_cop_kept_off;

/* A rotation's forward direction on the ground (unit), or 0 if it has none. */
static int cop_forward(const float q[4], float out[2]) {
    if (g_cop_axis < 0)
        return 0;
    float e[3] = {0, 0, 0}, w[3];
    e[g_cop_axis] = (float)g_cop_axis_sign;
    mp_rotate(q, e, w);
    const float len = sqrtf(w[0] * w[0] + w[2] * w[2]);
    if (!(len > 0.2f))
        return 0;
    out[0] = w[0] / len;
    out[1] = w[2] / len;
    return 1;
}

static void cop_us_read(void) {
    g_cop_us.have = 0;
    const uint32_t state = g_mp_player_simable ? mp_state(get_u32(g_mp_player_simable - 0x24 + 0x70)) : 0;
    if (!state)
        return;
    MpPose own;
    mp_read(state, &own);
    if (!mp_pose_ok(&own))
        return;
    static int tries;
    if (g_cop_axis < 0 && tries < 5) { /* the body axis whose turn by the rotation is the forward row (state + 0x90) */
        ++tries;
        float row[3], best = 0;
        for (int k = 0; k < 3; ++k)
            row[k] = bits_float(get_u32(state + 0x90 + 4u * k));
        for (int a = 0; a < 3; ++a) {
            float e[3] = {0, 0, 0}, w[3];
            e[a] = 1.0f;
            mp_rotate(own.q, e, w);
            const float d = w[0] * row[0] + w[1] * row[1] + w[2] * row[2];
            if (fabsf(d) > best) {
                best = fabsf(d);
                g_cop_axis = a;
                g_cop_axis_sign = d < 0 ? -1 : 1;
            }
        }
        if (best < 0.9f)
            g_cop_axis = -1;
        char line[96];
        snprintf(line, sizeof line, "core.nfsc: cops: a car's forward is its %s%c axis (%.2f)",
                 g_cop_axis_sign < 0 ? "-" : "+", g_cop_axis < 0 ? '?' : "xyz"[g_cop_axis], best);
        log_line(line);
    }
    if (!cop_forward(own.q, g_cop_us.f))
        return;
    memcpy(g_cop_us.p, own.p, sizeof own.p);
    memcpy(g_cop_us.v, own.v, sizeof own.v);
    g_cop_us.have = 1;
}

/* A car at our car now: whether it is still in its hit window (see KEPT OFF OUR CAR). */
static int cop_hit_window(const MpCar *c, double now) {
    static struct { const MpCar *c; double since, last; } at[COP_SLOTS + 16];
    int k = -1;
    for (int i = 0; i < COP_SLOTS + 16 && k < 0; ++i)
        if (at[i].c == c)
            k = i;
    for (int i = 0; i < COP_SLOTS + 16 && k < 0; ++i)
        if (!at[i].c || now - at[i].last > 10000.0)
            k = i;
    if (k < 0)
        k = 0;
    if (at[k].c != c || now - at[k].last > COP_HIT_REARM) {
        at[k].c = c;
        at[k].since = now;
    }
    at[k].last = now;
    return now - at[k].since <= COP_HIT_WINDOW;
}

/* mp_follow's aim for a car (g_mp_follow_target): off our car (see KEPT OFF OUR CAR); our copies
 * also lose their speed toward it. */
static void cop_keep_off(MpCar *c, MpPose *t) {
    CopCar *cc = cop_car(c);
    /* Shared-spawn pairs already have contacts disabled until they separate.
     * Moving the visual copy aside here fights that protection and makes it
     * jump around the footprint as its owner begins to accelerate. */
    if (c->spawn_overlap && !cc)
        return;
    float fc[2];
    if (c->kind == MP_GHOST || !g_cop_us.have || fabsf(t->p[1] - g_cop_us.p[1]) > 2.5f || !cop_forward(t->q, fc))
        return;
    const float need = 2.0f * COP_CIRCLE_R + COP_GAP;
    float moved = 0, n[2] = {0, 0};
    for (int round = 0; round < 3; ++round) {
        float worst = 0, wn[2] = {0, 0};
        for (int i = -1; i <= 1; i += 2) {
            const float ax = g_cop_us.p[0] + i * COP_CIRCLE_OFF * g_cop_us.f[0], az = g_cop_us.p[2] + i * COP_CIRCLE_OFF * g_cop_us.f[1];
            for (int j = -1; j <= 1; j += 2) {
                const float dx = t->p[0] + j * COP_CIRCLE_OFF * fc[0] - ax, dz = t->p[2] + j * COP_CIRCLE_OFF * fc[1] - az;
                const float d = sqrtf(dx * dx + dz * dz);
                if (need - d > worst) {
                    worst = need - d;
                    if (d > 0.01f) {
                        wn[0] = dx / d;
                        wn[1] = dz / d;
                    } else { /* on top of each other: out sideways, away from our middle */
                        const float sx = t->p[0] - g_cop_us.p[0], sz = t->p[2] - g_cop_us.p[2], sl = sqrtf(sx * sx + sz * sz);
                        wn[0] = sl > 0.01f ? sx / sl : -g_cop_us.f[1];
                        wn[1] = sl > 0.01f ? sz / sl : g_cop_us.f[0];
                    }
                }
            }
        }
        if (worst <= 0)
            break;
        t->p[0] += wn[0] * worst;
        t->p[2] += wn[1] * worst;
        moved += worst;
        n[0] = wn[0];
        n[1] = wn[1];
    }
    const int window = cop_hit_window(c, mp_now());
    const float closing = (t->v[0] - g_cop_us.v[0]) * n[0] + (t->v[2] - g_cop_us.v[2]) * n[1];
    if (!cc) {
        static unsigned told;
        if (++told <= 10u || told % 1000u == 0) {
            char line[128];
            snprintf(line, sizeof line, "core.nfsc: cops: player %u's car kept off ours (%.2f m; %u so far)", c->id, moved, told);
            log_line(line);
        }
    }
    if (closing < 0 && !window) {
        t->v[0] -= closing * n[0];
        t->v[2] -= closing * n[1];
    }
    if (cc && (++g_cop_kept_off <= 10u || g_cop_kept_off % 1000u == 0)) {
        char line[128];
        snprintf(line, sizeof line, "core.nfsc: cops: cop %u of player %u kept off our car (%.2f m; %u so far)",
                 cc->number, c->id, moved, g_cop_kept_off);
        log_line(line);
    }
}

/* How far apart two footprints are (negative: overlapping), with the copy at pose p, rotation q. */
static float cop_footprint_gap(const float p[3], const float q[4]) {
    float fc[2];
    if (!cop_forward(q, fc))
        return 0.0f;
    float least = 1e9f;
    for (int i = -1; i <= 1; i += 2)
        for (int j = -1; j <= 1; j += 2) {
            const float dx = p[0] + j * COP_CIRCLE_OFF * fc[0] - g_cop_us.p[0] - i * COP_CIRCLE_OFF * g_cop_us.f[0];
            const float dz = p[2] + j * COP_CIRCLE_OFF * fc[1] - g_cop_us.p[2] - i * COP_CIRCLE_OFF * g_cop_us.f[1];
            const float d = sqrtf(dx * dx + dz * dz) - 2.0f * COP_CIRCLE_R;
            if (d < least)
                least = d;
        }
    return least;
}

/* A contact between two bodies (REPLACE on 006e95a0): none for our car and a copy passing through
 * it (see PASSING THROUGH); every other pair as the game does it. */
static void cop_contact(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t a = get_u32(cpu->esp + 4), b = get_u32(cpu->esp + 0xc);
    const uint32_t oa = a ? get_u32(a + 0x2c) : 0, ob = b ? get_u32(b + 0x2c) : 0;
    if (g_mp_visual_car && (g_mp_visual_car(oa) || g_mp_visual_car(ob))) {
        api->hook_return(api, cpu, cpu->eax & 0xffffff00u, 0);
        return;
    }
    /* Traffic and native racers belong to this city alone. A remote player cannot
     * have hit them in the owner's game. Reject before contact separation, not
     * just in the later impulse solver. Shared pursuit cops remain interactive. */
    MpCar *ra = oa ? mp_remote_owner(oa) : NULL;
    MpCar *rb = ob ? mp_remote_owner(ob) : NULL;
    const int pa = ra && !cop_car_of(oa), pb = rb && !cop_car_of(ob);
    const uint32_t local = pa && !rb ? ob : pb && !ra ? oa : 0;
    const uint32_t cls = local && mp_listed(local - 0x24) ? get_u32(local - 0x24 + 0x148) : 0;
    if ((pa && pb) || ((pa || pb) && (cls == 1u || cls == 3u))) {
        api->hook_return(api, cpu, cpu->eax & 0xffffff00u, 0);
        return;
    }
    const uint32_t mine = g_mp_player_simable;
    MpCar *friend = mine && oa == mine ? mp_remote_owner(ob) : mine && ob == mine ? mp_remote_owner(oa) : NULL;
    if (friend && friend->spawn_overlap) {
        api->hook_return(api, cpu, cpu->eax & 0xffffff00u, 0);
        return;
    }
    MpCar *c = mine && oa == mine ? cop_car_of(ob) : mine && ob == mine ? cop_car_of(oa) : NULL;
    CopCar *cc = c ? cop_car(c) : NULL;
    if (!cc || !g_cop_us.have) {
        api->call_original(api, cpu->target, cpu);
        return;
    }
    const uint32_t state = mp_state(get_u32(c->simable - 0x24 + 0x70));
    if (state) {
        MpPose it;
        mp_read(state, &it);
        const float gap = mp_pose_ok(&it) ? cop_footprint_gap(it.p, it.q) : 0.0f;
        if (gap > COP_MEET) {
            cc->met = 0;
        } else if (!cc->met) {
            float n[2] = {it.p[0] - g_cop_us.p[0], it.p[2] - g_cop_us.p[2]};
            const float d = sqrtf(n[0] * n[0] + n[1] * n[1]);
            const float ours = d > 0.01f ? (g_cop_us.v[0] * n[0] + g_cop_us.v[2] * n[1]) / d : 0.0f;
            const float its = d > 0.01f ? -(it.v[0] * n[0] + it.v[2] * n[1]) / d : 0.0f;
            cc->met = ours >= COP_RAM_SPEED && ours >= its ? 1 : 2;
            static unsigned told;
            if (++told <= 20u || told % 500u == 0) {
                char line[160];
                snprintf(line, sizeof line, "core.nfsc: cops: cop %u of player %u met our car: %s (ours %.1f m/s, its %.1f m/s; %u so far)",
                         cc->number, c->id, cc->met == 1 ? "a ram, solid" : "passing through", ours, its, told);
                log_line(line);
            }
        }
    }
    if (cc->met == 2)
        api->hook_return(api, cpu, cpu->eax & 0xffffff00u, 0); /* cdecl */
    else
        api->call_original(api, cpu->target, cpu);
}

/* A copy being driven (not put) right at our car: its own speed toward our car beyond ours is
 * taken out, after its hit window (see KEPT OFF OUR CAR): mp.h's pull drives it onto its poses,
 * and a pull that ends just inside our car would push our car on and on. Our car running into it
 * is unchanged (that is our speed, not the copy's). */
static void cop_no_push(MpCar *c, double now) {
    const uint32_t state = mp_state(get_u32(c->simable - 0x24 + 0x70));
    if (!state)
        return;
    float p[3], v[3], n[2];
    for (int k = 0; k < 3; ++k) {
        p[k] = bits_float(get_u32(state + 0x10 + 4u * k));
        v[k] = bits_float(get_u32(state + 0x20 + 4u * k));
    }
    n[0] = p[0] - g_cop_us.p[0];
    n[1] = p[2] - g_cop_us.p[2];
    const float d = sqrtf(n[0] * n[0] + n[1] * n[1]);
    if (!(d > 0.01f) || d > COP_TOUCH_RANGE || fabsf(p[1] - g_cop_us.p[1]) > 2.5f || cop_hit_window(c, now))
        return;
    n[0] /= d;
    n[1] /= d;
    const float mine = v[0] * n[0] + v[2] * n[1], ours = g_cop_us.v[0] * n[0] + g_cop_us.v[2] * n[1];
    const float floor = ours < 0 ? ours : 0; /* toward our car no faster than our car moves away */
    if (!(mine < floor))
        return;
    put_float(state + 0x20, v[0] + (floor - mine) * n[0]);
    put_float(state + 0x28, v[2] + (floor - mine) * n[1]);
}

/* Where our car is (0 if there is none). */
static int cop_our_place(float out[3]) {
    const uint32_t state = g_mp_player_simable ? mp_state(get_u32(g_mp_player_simable - 0x24 + 0x70)) : 0;
    for (int k = 0; k < 3 && state; ++k)
        out[k] = bits_float(get_u32(state + 0x10 + 4u * k));
    return state && isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]);
}

static float cop_dist2(const float a[3], const float b[3]) {
    float d2 = 0;
    for (int k = 0; k < 3; ++k)
        d2 += (a[k] - b[k]) * (a[k] - b[k]);
    return d2;
}

/* Each frame, both calls (mp_place_all): every cop car where its owner's poses say (driven there
 * near our car, see NEAR OUR CAR), its light bar on while its owner's is. */
static void cop_place(double now, int late) {
    float ours[3], cars[64][3];
    const int have = cop_our_place(ours);
    cop_us_read();
    int n = 0, any = 0;
    for (int i = 0; i < COP_SLOTS; ++i)
        any |= g_cop_cars[i].car.simable && g_cop_cars[i].car.kept;
    /* Every other car of this game (not the copies, kept or not, and not ours: see above). */
    uint32_t node = any ? get_u32(MP_CARS) : 0;
    for (int guard = 0; node && node != MP_CARS && guard < 64; ++guard, node = get_u32(node)) {
        const uint32_t car = node - 0xd0, simable = car + 0x24, state = mp_state(get_u32(car + 0x70));
        if (!state || simable == g_mp_player_simable || cop_car_of(simable) || cop_spare_of(simable))
            continue;
        for (int k = 0; k < 3; ++k)
            cars[n][k] = bits_float(get_u32(state + 0x10 + 4u * k));
        n += isfinite(cars[n][0]) && isfinite(cars[n][1]) && isfinite(cars[n][2]);
    }
    for (int i = 0; i < COP_SLOTS; ++i) {
        CopCar *cc = &g_cop_cars[i];
        MpCar *c = &cc->car;
        if (!c->simable || !c->kept)
            continue;
        const float *next = c->pose[c->kept - 1].p;
        int near = have && (cop_dist2(c->drawn, ours) < COP_NEAR_US * COP_NEAR_US ||
                            cop_dist2(next, ours) < COP_NEAR_US * COP_NEAR_US);
        for (int j = 0; j < n && !near && c->follow == MP_FOLLOWING; ++j)
            near = cop_dist2(c->drawn, cars[j]) < COP_NEAR_CAR * COP_NEAR_CAR ||
                   cop_dist2(next, cars[j]) < COP_NEAR_CAR * COP_NEAR_CAR;
        if (near && c->follow == MP_FOLLOWING) {
            c->follow = MP_RETURNING; /* see NEAR OUR CAR */
            c->return_at = now - MP_RETURN_MS;
            c->pull_at = now;
        }
        MpPose pose;
        mp_follow(c, now, late, &pose);
        memcpy(c->drawn, pose.p, sizeof c->drawn);
        if (late && c->follow != MP_FOLLOWING && g_cop_us.have)
            cop_no_push(c, now);
        const uint32_t car = c->simable - 0x24, glare = get_u32(car + COP_GLARE);
        const uint32_t want = cc->flags & COP_F_LIGHTS ? glare | COP_LIGHTS : glare & ~COP_LIGHTS;
        if (want != glare)
            mp_put(car + COP_GLARE, want);
    }
}

/* Damage for every car (DamageVehicle::OnImpact, thiscall on the DamageVehicle, 6 args), except
 * the cop copies here (see the header). The behaviour's ISimable is at DamageVehicle + 0x2c. */
static void cop_no_damage(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t owner = get_u32(cpu->ecx + 0x2c);
    if (owner && (cop_car_of(owner) || cop_spare_of(owner)))
        api->hook_return(api, cpu, cpu->eax, 0x18);
    else
        api->call_original(api, cpu->target, cpu);
}

/* IDamageable::Destroy (thiscall on the IDamageable = DamageVehicle + 0x44): not for the copies. */
static void cop_no_destroy(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const uint32_t owner = get_u32(cpu->ecx - 0x44 + 0x2c);
    if (owner && (cop_car_of(owner) || cop_spare_of(owner)))
        api->hook_return(api, cpu, cpu->eax, 0);
    else
        api->call_original(api, cpu->target, cpu);
}

static void cops_service(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)cpu;
    (void)inv;
    (void)user;
    CopShared *sh = cop_shared();
    if (!sh)
        return;
    const uint32_t player = g_mp_player_simable ? g_mp_player_simable - 0x24u : 0;
    cop_publish(sh, player);
    cop_received(sh, player);
    cop_cars(sh, player);
    cop_spares(player);
    cop_send_hits(sh);
}

static void cops_install(void) {
    if (!g_mp_block || g_api->guest_alloc(g_api, sizeof(CopShared), &g_cop_block) != POP_OK) {
        log_line("core.nfsc: friends' police chases unavailable (no guest memory)");
        g_cop_block = 0;
        return;
    }
    CopShared *sh = cop_shared();
    if (!sh) {
        g_cop_block = 0;
        return;
    }
    memset(sh, 0, sizeof *sh);
    sh->magic = COP_MAGIC;
    sh->version = 1;
    if (g_api->hook_install(g_api, MP_FE_SERVICE, cops_service, POP_HOOK_BEFORE, NULL, &g_cop_hooks[0]) != POP_OK) {
        log_line("core.nfsc: friends' police chases unavailable");
        return;
    }
    if (g_api->hook_install(g_api, COP_DAMAGE, cop_no_damage, POP_HOOK_REPLACE, NULL, &g_cop_hooks[1]) != POP_OK ||
        g_api->hook_install(g_api, COP_DESTROY, cop_no_destroy, POP_HOOK_REPLACE, NULL, &g_cop_hooks[2]) != POP_OK) {
        log_line("core.nfsc: friends' police chases unavailable (no damage guard)");
        return; /* friends' cops are only built with the guard (see the header) */
    }
    const PopModStatus through = g_api->hook_install(g_api, COP_CONTACT, cop_contact, POP_HOOK_REPLACE, NULL, &g_cop_hooks[3]);
    if (through != POP_OK) {
        char line[96];
        snprintf(line, sizeof line, "core.nfsc: cops: copies cannot pass through our car (contact hook: %d)", (int)through);
        log_line(line);
    }
    g_mp_cop_car = cop_car_of;
    g_mp_cop_place = cop_place;
    g_mp_cop_touch = cop_touch;
    g_mp_cop_pair = cop_pair;
    g_mp_follow_target = cop_keep_off;
#ifdef __EMSCRIPTEN__
    MAIN_THREAD_ASYNC_EM_ASM({
        var block = {};
        block.memory = wasmMemory;
        block.ptr = $0;
        globalThis.carbonCopBlock = block;
        globalThis.dispatchEvent(new Event("carbon-cops"));
    }, (uint32_t)(uintptr_t)sh);
#endif
}
