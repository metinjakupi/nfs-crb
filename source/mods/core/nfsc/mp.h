/* mp.h - other players' cars in the world (included by nfsc.c).
 *
 * ONLINE. The page's online client (web/net.js, loaded with ?mp=<relay>) and
 * this file share a block of guest memory (MpShared below). Every frame the
 * game writes the player's car pose, model key and its physics time (MainLoop
 * 00761150 adds each frame's real time to the float at 0x00b4d7ec and takes
 * whole 1/60 s steps out of it, 1 to 8; mp_main_loop counts them), so a pose is
 * stamped with the moment it describes, not the moment it was read; the
 * page sends that to the other players and puts each pose it receives, with
 * its sender's time, into that player's slot (one of MP_SLOTS, a ring of the
 * last MP_RING poses). The mod keeps the recent poses and plays them back on a
 * clock of its own, in the sender's time, that it keeps MP_DELAY ms behind the
 * newest pose by running up to 10% fast or slow (a sender's physics time can
 * run slower than real time when its frame rate drops); it places the car
 * where that clock says, blended between the two poses around it (or carried
 * on by its speed for up to MP_EXTRAPOLATE ms). It does that twice a frame: before the HUD runs and again
 * as the main view is set up for drawing, after the frame's fixed 1/60 s
 * physics steps (MainLoop 00761150 runs 1 to 8 a frame), so the car is drawn
 * where it should be at that moment. After touching our car it is left to the
 * engine for a while instead (CONTACTS below). The
 * mod builds a car for a slot near the player (within MP_NEAR metres) and
 * removes it when the slot empties, the player leaves the world, or the car is
 * beyond MP_FAR. Each side of the block
 * guards its own part with a sequence number: odd while being written, and a
 * reader keeps a copy only if the number was even and unchanged across it.
 * NAMES. After the main view's matrices are made (an AFTER hook on 0071b780:
 * its `this` + 0x80 holds the view-projection matrix, row vectors, in render
 * space, which is (z, -x, y) of the physics space; checked by projecting the
 * player's own car to 0.50, 0.69 of the screen, 5 m away), the mod projects a
 * point MP_LABEL_UP metres above each car and writes it to labels[] for the
 * page, which draws the player's name there.
 * On the web the block's address goes to the page as globalThis.carbonMpBlock
 * ({memory, ptr}) with a "carbon-mp" event.
 * KEYS. The page's quick chat and horn (web/chat.js) must not take a key the
 * player has bound, so every frame the mod writes `keys` (+0x58): bit n set =
 * the letter A + n is bound to something in the game's keyboard bindings (both
 * keyboard slots of all PAD_ROWS rows, pad.h; a key there is {1, DIK, 0}, as
 * the binding reader 0067a340 takes it), and MP_KEYS_KNOWN (bit 31) to say the
 * word is filled in, so 0 means "not known" (an older mod, or the table not
 * made yet). Letters are by position, as DIK codes and the page's
 * KeyboardEvent.code both are (the A key of a French keyboard is bit Q). A
 * remap on Options -> Controls shows on the next frame.
 *
 * TEST CAR (setting mp_test, off; RECOMP_MP_TEST in the URL overrides it): a
 * copy of the player's car that records the player's first MP_TAKE frames of
 * driving and replays them over and over, the way a remote car moves.
 * RECOMP_MP_TEST=2 instead keeps it MP_SIDE metres to the left and MP_AHEAD in
 * front, so the chase camera always sees it.
 *
 * CARS. Every PVehicle is linked into the list at 0x00a9e728 by a node at +0xd0
 * (006c6190 walks it). In a PVehicle: ISimable at +0x24, IRigidBody * at +0x70,
 * IPlayer * at +0x7c (0 for AI), its pvehicle attributes (an Attrib::Instance,
 * 0x10 bytes; [collection + 0x10] is the model key, IVehicle::GetVehicleKey
 * 006d7f30 -> 004613e0) at +0xd8 and its driver class at +0x148.
 *
 * BUILDING ONE. PVehicle::Construct 006daa80 (cdecl: UCrc32 type, VehicleParams *)
 * returns the new car's ISimable, or 0 when the vehicle budget (0x1e cars, which
 * 006d86f0 frees through the params' vehicle cache), the model (006c08f0) or the
 * spawn point (00761270) refuses. VehicleParams, 0x44 bytes: +0 type crc
 * (00404820 writes it), +4 data pointer (the params themselves), +0x10 driver
 * class, +0x14 attributes (00412520, thiscall (instance, model key, 0), builds
 * them), +0x24 direction *, +0x28 position *, +0x2c customization * (0 = the
 * stock look; look.h passes the owner's), +0x30 IVehicleCache *, +0x34 performance match *, +0x38 flags
 * (1 spooled, 2 snap to ground, 8 work out performance). Other players' cars
 * are driver class 7, the game's own class for online opponents ("remote
 * racer"): 006d0d40 gives it the RBRemote rigid body and SuspensionSimple, and
 * the class table at 0x00a63370 (filled at 009b6edc) gives it AIVehicleEmpty, so
 * nothing in the game drives it. It is drawn solid. RBRemote's CanCollideWith
 * (006ecca0) asks the original online session (0x00bba41c) before letting cars
 * touch. Our relay does not create that session, so each solid car we create
 * gets a private copy of RBRemote's method table with just CanCollideWith
 * replaced by RBVehicle's normal rule (006ec160). The engine still owns
 * contact detection, impulses and crash effects. RBRemote is RigidBody with
 * only those two methods and its destructors changed, so with the same model
 * it has the same mass and contact response as any car.
 * CONTACTS. Hitting another player's car must feel like hitting an AI car, and
 * mp_hit.h measures that (RECOMP_MP_HIT). An AI car is pushed, spun and slowed
 * by the engine's own response (a nearly inelastic one: at 80 km/h nose to nose
 * both cars come out at about half the striker's speed). So every game solves
 * each touch of its own car with another player's car at once, as for an AI
 * car (mp_resolve_pair), and then leaves that car to the engine. Measured
 * before (2026-10-25): writing its next pose over the response made it as heavy
 * as a wall, and the game with the higher relay id skipped its own solve and
 * waited for the other's impact, so its car drove into the other (centres
 * 0.9 m apart at 80 km/h) and was stopped late by the message, while the struck
 * player's car moved up to 0.4 s before the striker reached it. Its owner's game
 * solves the same touch for the owner's car when the cars meet on that screen,
 * so both players see the crash, and as soon as the owner's poses show that
 * push (or after MP_FREE_MS) the car is pulled back onto them by a spring on
 * its speed (mp_follow), never put: the owner's car brakes, steers and drives
 * on as its owner does, which a car left to coast does not. Near
 * our car the playback delay shrinks to MP_NEAR_DELAY, so the cars meet in both
 * games at nearly the same moment and place. With a version-2 page and capable
 * relay the lower connection id also sends the other car's change to its owner
 * through bounded queues; the owner applies it only when its own game saw no
 * touch with that car (mp_contacts), and its pose acknowledges it.
 * With setting mp_ghost_cars, or the page's menu asking for it (flags bit 0),
 * they are class 8 instead: RBGhost (a plain
 * RigidBody; only its destructor differs) and AIVehicleGhost, drawn see-through.
 * Either way the physics still runs, so between two frames a car can be pushed
 * (a test car put inside a wall turned sideways); on open road the push is
 * 2-4 cm. The cache is GRaceStatus
 * (*0x00a98284; its IVehicleCache is at +0), as for racers. Addresses and
 * layouts were found with berkayylmao's NFSPluginSDK (BSD-3) and checked in the
 * listing.
 *
 * MOVING IT. Every rigid body class (eight IRigidBody vtables, RBGhost's 009e7aa8)
 * shares the same getters and setters, which work on a state block at
 * [[body + 0x30]]: quaternion +0x00, position +0x10, linear velocity +0x20,
 * angular velocity +0x30, right/up/forward rows +0x70/+0x80/+0x90. The plain
 * copies (SetPosition 006e8210, SetLinearVelocity 006e8230, SetAngularVelocity
 * 006e8250, the row getters) are not listed functions, so the mod copies the same
 * fields itself; SetOrientation(quaternion) 006e8060 (vtable +0x74) also rebuilds
 * the rows and the world inertia, so it is called. The car's Kill (ISimable +0x08,
 * 006d43a0) is not listed either; mp_kill does its steps, which are.
 * The work runs before cFEng::Service (00600c90), once a frame while the HUD runs.
 */
#include <stdlib.h>
#include <time.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define MP_CARS 0x00a9e728u
#define MP_CONSTRUCT 0x006daa80u
#define MP_PARAMS_TYPE 0x00404820u
#define MP_ATTRIBUTES 0x00412520u      /* thiscall (Attrib::Gen::pvehicle *, model key, 0) */
#define MP_RACE_STATUS 0x00a98284u
#define MP_FE_SERVICE 0x00600c90u
#define MP_SET_ORIENTATION 0x006e8060u /* thiscall (body, const float quaternion[4]) */
#define MP_SIM_REMOVE 0x006cb2b0u      /* thiscall (0x00aa7810, PVehicle *) */
#define MP_END_SERVICE 0x007625b0u     /* thiscall (PVehicle *, body service) */
#define MP_DROP_MECHANIC 0x006d1ad0u   /* thiscall (PVehicle *, const UCrc32 *) */
#define MP_MAIN_LOOP 0x00761150u       /* cdecl (float seconds); AL = steps were queued */
#define MP_STEP_LEFT 0x00b4d7ecu       /* float: real time not yet stepped */
#define MP_REMOTE_VTABLE 0x009e7d88u  /* 22 RigidBody methods, ending before float data */
#define MP_REMOTE_CAN_COLLIDE 0x006ecca0u /* thiscall (RigidBody *other), returns AL; RET 4 */
#define MP_VEHICLE_CAN_COLLIDE 0x006ec160u /* the ordinary RBVehicle collision rule */
#define MP_RESOLVE_PAIR 0x006e9890u /* cdecl (RigidBody *a, *b, ...): contact response */
#define MP_WAKE_BODY 0x006e82b0u
#define MP_IMPACTS 32u
#define MP_REMOTE_HUMAN 6u /* driver classes: online human (RBRemote, AIVehicleHuman; can be chased) */
#define MP_REMOTE 7u /* online opponent */
#define MP_GHOST 8u  /* see-through ghost */
#define MP_SLOTS 8
#define MP_NEAR 300.0f /* metres: a remote player's car is built within this */
#define MP_FAR 500.0f  /* and removed beyond this */
#define MP_SIDE 4.0f   /* test car: metres beside the player where it first appears */
#define MP_AHEAD 7.0f  /* escort test: metres in front of the player */
#define MP_TAKE 900    /* test car: frames recorded, then replayed */
#define MP_RING 4      /* poses per slot */
#define MP_KEEP 8      /* poses the mod keeps per car */
#define MP_DELAY 150.0 /* ms behind the sender */
#define MP_EXTRAPOLATE 300.0
/* Touching another player's car (CONTACTS in the header comment). */
#define MP_NEAR_DELAY 50.0   /* ms behind the sender for a solid car close to ours */
#define MP_NEAR_FROM 45.0f   /* metres: closer than this the delay starts to shrink */
#define MP_NEAR_TO 20.0f     /* and is MP_NEAR_DELAY from here in */
#define MP_FREE_MIN_MS 40.0  /* after a touch the engine alone moves the other car at least this */
#define MP_FREE_MS 300.0     /* and at most this, or until its owner's poses show the push */
#define MP_RETURN_MS 400.0   /* then the pull to its owner's poses grows to full over this */
#define MP_RETURN_RATE 8.0f  /* rad/s: that pull (a critically damped spring), at full */
#define MP_RETURN_ACCEL 30.0f /* m/s^2: the most it pulls */
#define MP_SNAP 8.0f         /* metres: further off than this, the car is put back at once, */
#define MP_SNAP_CLEAR 6.0f   /* unless its place is this close to ours (it would land on us) */
#define MP_SNAP_FAR 40.0f    /* and always when it is this far off */
#define MP_IMPACT_HOLD 250.0 /* ms a received impact waits for this game's own contact */
#define MP_IMPACT_SEEN 600.0 /* ms: an own contact this recent already gave this car its push */
enum { MP_FOLLOWING, MP_FREE, MP_RETURNING }; /* MpCar.follow */
#define MP_LABEL_UP 1.7f /* metres above a car's centre where its name goes */

typedef struct MpPose {
    float q[4], p[3], v[3], w[3]; /* quaternion, position, linear and angular velocity */
} MpPose;

typedef struct MpSnap { /* 0x38 bytes */
    uint32_t ms; /* the sender's clock */
    MpPose pose;
} MpSnap;

typedef struct MpSlot { /* 0x100 bytes; written by the page */
    uint32_t id, seq, key, mode; /* id 0 = empty; mode 1 = driving */
    uint32_t count;              /* poses written; the newest is ring[(count - 1) % MP_RING] */
    uint32_t pad[3];
    MpSnap ring[MP_RING];
} MpSlot;

typedef struct MpLabel { /* written by the game for the page */
    float x, y;  /* 0..1 across and down the screen */
    float depth; /* metres in front of the camera; 0 = not shown */
    uint32_t applied; /* last impact from this slot's player applied to our car */
} MpLabel;

typedef struct MpImpact { /* 0x40 bytes, one engine-calculated contact response */
    uint32_t id, serial, key, ms; /* target when sent, source when received */
    float at[3], dp[3], dv[3], dw[3];
} MpImpact;

typedef struct MpShared {
    uint32_t magic, version; /* MP_MAGIC, 2; the version-1 prefix stays intact */
    uint32_t page;           /* the page: nonzero while the relay link is up */
    uint32_t seq;            /* the game's part from here to pose */
    uint32_t state, key, frame, ms; /* state 1 = driving a car; ms: when the pose was read */
    MpPose pose;
    uint32_t flags;          /* the page's choices and capabilities: MP_PAGE_* */
    uint32_t keys;           /* the game's: letters bound on the keyboard (KEYS above); 0 = not known */
    uint32_t me;             /* the page: our relay connection's id, 0 offline */
    MpSlot slots[MP_SLOTS];
    MpLabel labels[MP_SLOTS];
    uint32_t tx_head, tx_tail, rx_head, rx_tail;
    MpImpact tx[MP_IMPACTS], rx[MP_IMPACTS]; /* single-producer/single-consumer queues */
} MpShared;
#define MP_MAGIC 0x31504d43u /* "CMP1" */
#define MP_PAGE_GHOST 1u      /* flags: show other players' cars see-through */
#define MP_PAGE_CONTACTS 2u   /* relay and page support shared collision responses */
#define MP_KEYS_KNOWN 0x80000000u /* keys: the mod filled it in */
/* The page reads these by offset (web/net.js, chat.js). */
_Static_assert(offsetof(MpShared, flags) == 0x54 && offsetof(MpShared, keys) == 0x58 &&
                   offsetof(MpShared, slots) == 0x60 && offsetof(MpShared, tx_head) == 0x8e0 &&
                   sizeof(MpImpact) == 0x40 && sizeof(MpShared) == 0x18f0,
               "MpShared layout");

typedef struct MpCar {
    uint32_t simable, id, key; /* the car's ISimable (0 = none) and whose it is */
    uint32_t kind;             /* the driver class it was built with */
    int wait;                  /* frames before the next build attempt */
    int tries;
    int visual_only; /* encounter copies: owner poses, no player inputs or contacts */
    int spawn_overlap; /* no contact with our car until a shared spawn has cleared */
    uint32_t taken;            /* slot poses already kept */
    double play, played_at;    /* the playback clock (sender ms) and when it last moved (our ms) */
    int kept;
    double at[MP_KEEP];        /* each kept pose's sender time, ms */
    MpPose pose[MP_KEEP];      /* oldest first */
    float drawn[3];            /* where it was last placed */
    uint32_t contacts, ack, applied, sent;
    int pending;               /* impact (below) being gathered for its owner */
    int follow;                /* MP_FOLLOWING its poses, MP_FREE after a touch, MP_RETURNING */
    double contact_at;         /* our ms: its last touch with our car */
    double free_at;            /* our ms: the touch that set it free */
    float hit_v0[3], hit_dv[3]; /* its owner's newest speed then, and the push our game gave it */
    double return_at, pull_at; /* when the pull back began, and its last step */
    double lag;                /* ms it plays back behind its newest pose; 0 = MP_DELAY */
    MpImpact impact;           /* contacts accumulated during one game frame */
    MpImpact held;             /* a received impact waiting for our own contact (holding) */
    int holding;
    double held_at;
} MpCar;

static uint32_t g_mp_buf;   /* 0x100 guest bytes: params at +0, vectors from +0x60 */
static uint32_t g_mp_block; /* guest address of the MpShared */
static uint32_t g_mp_solid_vtable; /* shared only by the solid cars this mod creates */
static MpCar g_mp_remote[MP_SLOTS];
static MpSlot g_mp_seen_slot[MP_SLOTS]; /* the last whole copy of each slot */
static MpCar g_mp_test;
static int (*g_mp_visual_car)(uint32_t simable);
static uint32_t g_mp_seen;  /* frames the player's car has existed */
static uint32_t g_mp_frame;
static MpPose g_mp_take[MP_TAKE], g_mp_hold, g_mp_last;
static int g_mp_recorded, g_mp_played;
static float g_mp_drift;    /* furthest the physics moved the test car between two frames */
static uint64_t g_mp_steps; /* 1/60 s physics steps since start */
static uint32_t g_mp_player_simable, g_mp_impact_serial;

static int g_mp_page_ghost; /* the page's menu asked for see-through cars */
/* race.h: 1 for a player whose car is one of its racers (it builds and moves that car itself). */
static int (*g_mp_racer)(uint32_t id);
/* race.h (optional): the MpCar of its racer whose ISimable is owner, or NULL. */
static MpCar *(*g_mp_racer_car)(uint32_t owner);
/* look.h: a guest copy of player id's car look (VehicleParams + 0x2c) for slot's car in model
 * key (0 = this player's own model: no look), 0 = the stock look; and whether slot's car was
 * built before the newest look of its owner. */
static uint32_t (*g_mp_look)(int slot, uint32_t id, uint32_t key);
static int (*g_mp_relook)(int slot, uint32_t id, uint32_t key);
/* cops.h (a friend's police chase): the MpCar of a friend's cop shown here whose ISimable is
 * owner, or NULL; moving its cars where mp_place_all moves the others (both calls a frame);
 * each solved touch of our car with one of them (before/after: the cop car's state); and every
 * pair solve that involves neither our car nor a car of ours with contacts (after 0, then 1). */
static MpCar *(*g_mp_cop_car)(uint32_t owner);
static void (*g_mp_cop_place)(double now, int late);
static void (*g_mp_cop_touch)(MpCar *cop, const MpPose *before, const MpPose *after);
static void (*g_mp_cop_pair)(uint32_t a, uint32_t b, int after);
/* Step 3, a shared chase (pursuit.h): the driver class for a friend's own car, player id, when it
 * is built solid (7 when unset; 6 makes it a remote human the police can chase; see-through cars
 * stay 8), and a call each time a friend's own car is built (simable = its ISimable) or goes
 * (simable = 0: removed, or gone with the world), with its slot and its owner's relay id. */
static uint32_t (*g_mp_friend_kind)(uint32_t id);
static void (*g_mp_friend_car)(int slot, uint32_t id, uint32_t simable);
/* mp_drive.h (friends' cars driven by their owners' own controls): once a frame after this
 * player's pose is published, and for every car mp_follow moves, with its playback time (late:
 * the call as the view is set up). */
static void (*g_mp_drive)(MpShared *sh, uint32_t player);
static void (*g_mp_drive_car)(MpCar *c, double play, int late);
static void (*g_mp_follow_target)(MpCar *c, MpPose *target); /* cops.h: keeps a remote car's place off our car */

static uint32_t mp_kind(void) {
    return setting("mp_ghost_cars", 0) || g_mp_page_ghost ? MP_GHOST : MP_REMOTE;
}

/* The driver class for player id's own car (g_mp_friend_kind: 6 or 7 when solid). */
static uint32_t mp_friend_kind(uint32_t id) {
    const uint32_t kind = mp_kind(), want = kind == MP_REMOTE && g_mp_friend_kind ? g_mp_friend_kind(id) : kind;
    return want == MP_REMOTE_HUMAN || want == MP_REMOTE || want == MP_GHOST ? want : kind;
}

/* Whether car still has the driver class it was built with (6 and 7 count as one: the game may
 * move a remote car between them). */
static int mp_kind_kept(uint32_t car, uint32_t kind) {
    const uint32_t now = get_u32(car + 0x148);
    return now == kind || ((now == MP_REMOTE || now == MP_REMOTE_HUMAN) && (kind == MP_REMOTE || kind == MP_REMOTE_HUMAN));
}

/* 0 off, 1 record and replay, 2 escort. */
static int mp_test_mode(void) {
    const char *e = getenv("RECOMP_MP_TEST");
    if (e && *e)
        return atoi(e);
    return setting("mp_test", 0) != 0;
}

static void mp_put(uint32_t addr, uint32_t v) {
    g_api->guest_write_u32(g_api, addr, v);
}

static void mp_copy(uint32_t dst, uint32_t src, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; i += 4)
        mp_put(dst + i, get_u32(src + i));
}

static uint32_t mp_call(uint32_t fn, uint32_t ecx, const uint32_t *args, uint32_t n) {
    uint32_t eax = 0;
    return fn && g_api->guest_call(g_api, fn, ecx, args, n, &eax) == POP_OK ? eax : 0;
}

/* RBRemote's primary table has 22 methods; the following bytes are float
 * constants. IRigidBody is at RigidBody + 0x40, and GetOwner (006c6c70)
 * reads the ISimable at IRigidBody - 0x14. Verify both before changing an
 * instance. Preserve its destructor and every other method, and never edit
 * the shared game table. Normal vehicles and class-8 ghosts are untouched. */
static void mp_enable_contacts(uint32_t simable) {
    const uint32_t body = get_u32(simable - 0x24 + 0x70);
    if (body < 0x40u || get_u32(body - 0x40u) != MP_REMOTE_VTABLE ||
        get_u32(body - 0x14u) != simable ||
        get_u32(MP_REMOTE_VTABLE + 0x48u) != MP_REMOTE_CAN_COLLIDE) {
        log_line("core.nfsc: mp collision table did not match RBRemote");
        return;
    }
    if (!g_mp_solid_vtable) {
        if (g_api->guest_alloc(g_api, 0x58, &g_mp_solid_vtable) != POP_OK) {
            log_line("core.nfsc: mp collision table unavailable (no guest memory)");
            return;
        }
        mp_copy(g_mp_solid_vtable, MP_REMOTE_VTABLE, 0x58);
        mp_put(g_mp_solid_vtable + 0x48u, MP_VEHICLE_CAN_COLLIDE);
    }
    mp_put(body - 0x40u, g_mp_solid_vtable);
    log_line("core.nfsc: mp solid car uses the normal vehicle collision rule");
}

static MpShared *mp_shared(void) {
    void *p = NULL;
    return g_mp_block && g_api->guest_ptr(g_api, g_mp_block, sizeof(MpShared), &p) == POP_OK ? p : NULL;
}

/* A rigid body's state block, or 0. */
static uint32_t mp_state(uint32_t body) {
    const uint32_t p = body ? get_u32(body + 0x30) : 0;
    return p ? get_u32(p) : 0;
}

/* The player's PVehicle, or 0: the car with a player attached. Its driver
 * class is 0 while the player drives and changes when the game takes over
 * (after the finish line); it is still the player's car. */
static uint32_t mp_player(void) {
    uint32_t node = get_u32(MP_CARS), other = 0;
    for (int n = 0; node && node != MP_CARS && n < 64; ++n, node = get_u32(node)) {
        const uint32_t car = node - 0xd0;
        if (!get_u32(car + 0x7c) || !get_u32(car + 0x70))
            continue;
        if (get_u32(car + 0x148) == 0)
            return car;
        if (!other)
            other = car;
    }
    return other;
}

static int mp_listed(uint32_t car) {
    uint32_t node = get_u32(MP_CARS);
    for (int n = 0; node && node != MP_CARS && n < 64; ++n, node = get_u32(node))
        if (node - 0xd0 == car)
            return 1;
    return 0;
}

/* The model every game has. A car built with performance parts (a customized one, an AI
 * racer) gets a "dynamic" copy of its model's attributes (Instance flag 1 at +0xc, as 00469a20
 * undoes it) under a key made in that game alone; the model is its parent (collection + 0xc). */
static uint32_t mp_model_key(uint32_t car) {
    uint32_t collection = get_u32(car + 0xd8);
    if (collection && (get_u32(car + 0xd8 + 0xc) & 1u))
        collection = get_u32(collection + 0xc);
    return collection ? get_u32(collection + 0x10) : 0;
}

static double mp_now(void);
static uint32_t mp_ms(void);

static void mp_read(uint32_t state, MpPose *out) {
    for (int i = 0; i < 4; ++i)
        out->q[i] = bits_float(get_u32(state + 4u * i));
    for (int i = 0; i < 3; ++i) {
        out->p[i] = bits_float(get_u32(state + 0x10 + 4u * i));
        out->v[i] = bits_float(get_u32(state + 0x20 + 4u * i));
        out->w[i] = bits_float(get_u32(state + 0x30 + 4u * i));
    }
}

/* v turned by the unit quaternion q, (x, y, z, w), as v' = q v q*. */
static void mp_rotate(const float q[4], const float v[3], float out[3]) {
    const float t[3] = {2.0f * (q[1] * v[2] - q[2] * v[1]), 2.0f * (q[2] * v[0] - q[0] * v[2]),
                        2.0f * (q[0] * v[1] - q[1] * v[0])};
    out[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
    out[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
    out[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}

/* Whether a pose can be given to the game, its rotation made exactly unit
 * length. A car that has only just been made reports a zero rotation for a
 * moment, and setting a car to that froze the game for good (a player
 * joining froze the others). */
static int mp_pose_ok(MpPose *p) {
    float len = 0;
    for (int i = 0; i < 4; ++i) {
        if (!isfinite(p->q[i]))
            return 0;
        len += p->q[i] * p->q[i];
    }
    if (!(len > 0.81f && len < 1.21f))
        return 0;
    len = sqrtf(len);
    for (int i = 0; i < 4; ++i)
        p->q[i] /= len;
    for (int i = 0; i < 3; ++i)
        if (!isfinite(p->p[i]) || fabsf(p->p[i]) > 100000.0f || !isfinite(p->v[i]) ||
            fabsf(p->v[i]) > 300.0f || !isfinite(p->w[i]) || fabsf(p->w[i]) > 100.0f)
            return 0;
    return 1;
}

static void mp_write(uint32_t body, uint32_t state, const MpPose *pose) {
    for (int i = 0; i < 3; ++i) {
        put_float(state + 0x10 + 4u * i, pose->p[i]);
        put_float(state + 0x20 + 4u * i, pose->v[i]);
        put_float(state + 0x30 + 4u * i, pose->w[i]);
    }
    for (int i = 0; i < 4; ++i)
        put_float(g_mp_buf + 0x90 + 4u * i, pose->q[i]);
    const uint32_t arg = g_mp_buf + 0x90;
    mp_call(MP_SET_ORIENTATION, body, &arg, 1);
}

/* Moves a car we built to pose; 0 if the car is gone. */
static int mp_place(uint32_t simable, const MpPose *pose) {
    const uint32_t body = get_u32(simable - 0x24 + 0x70), state = mp_state(body);
    if (!state)
        return 0;
    mp_write(body, state, pose);
    return 1;
}

/* Builds a car of driver class `kind` and model `key` (0 = a copy of the
 * player's) at pos, facing the player's way until its first pose arrives, in
 * look (a VehicleCustomizations, which the car copies; 0 = the stock look).
 * Returns its ISimable or 0. */
static uint32_t mp_build(uint32_t player, uint32_t kind, uint32_t key, const float pos[3], uint32_t flags,
                         uint32_t look) {
    const uint32_t b = g_mp_buf, state = mp_state(get_u32(player + 0x70));
    if (!state)
        return 0;
    for (uint32_t i = 0; i < 0x60; i += 4)
        mp_put(b + i, 0);
    uint32_t arg = b;
    mp_call(MP_PARAMS_TYPE, 0, &arg, 1);
    mp_put(b + 0x04, b);
    mp_put(b + 0x10, kind);
    if (key) {
        const uint32_t args[2] = {key, 0};
        mp_call(MP_ATTRIBUTES, b + 0x14, args, 2);
    } else {
        mp_copy(b + 0x14, player + 0xd8, 0x10);
    }
    mp_copy(b + 0x60, state + 0x90, 0x0c); /* direction: the player's forward row */
    for (int i = 0; i < 3; ++i)
        put_float(b + 0x70 + 4u * i, pos[i]);
    mp_put(b + 0x24, b + 0x60);
    mp_put(b + 0x28, b + 0x70);
    mp_put(b + 0x2c, look);
    mp_put(b + 0x30, get_u32(MP_RACE_STATUS));
    mp_put(b + 0x38, flags);
    const uint32_t args[2] = {get_u32(b), b};
    uint32_t car = mp_call(MP_CONSTRUCT, 0, args, 2);
    if (car && !mp_listed(car - 0x24)) {
        char line[96];
        snprintf(line, sizeof line, "core.nfsc: mp car %08x is not in the car list", car);
        log_line(line);
        car = 0;
    }
    if (car && (kind == MP_REMOTE || kind == MP_REMOTE_HUMAN)) /* both are RBRemote (006d0d40) */
        mp_enable_contacts(car);
    return car;
}

/* PVehicle's ISimable::Kill (006d43a0), step by step. */
static void mp_kill(uint32_t simable) {
    const uint32_t car = simable - 0x24, crc = g_mp_buf + 0xc0;
    g_api->guest_write_u8(g_api, simable + 0x1c, 1);
    mp_call(MP_SIM_REMOVE, 0x00aa7810u, &car, 1);
    const uint32_t service = get_u32(simable + 0x5c);
    if (service) {
        mp_call(MP_END_SERVICE, car, &service, 1);
        mp_put(simable + 0x5c, 0);
    }
    mp_put(crc, get_u32(0x00a9e7d4u));
    mp_call(MP_DROP_MECHANIC, car, &crc, 1);
    mp_put(crc, get_u32(0x00a9e7a8u));
    mp_call(MP_DROP_MECHANIC, car, &crc, 1);
    mp_put(simable + 0x188, get_u32(simable + 0x188) & ~1u);
}

static MpCar *mp_remote_owner(uint32_t owner) {
    for (int i = 0; i < MP_SLOTS; ++i)
        if (owner && g_mp_remote[i].simable == owner)
            return &g_mp_remote[i];
    /* race.h may hand over its racers' cars too (set g_mp_racer_car, move them with
     * mp_follow); until then they are placed every frame and stay as heavy as walls. */
    MpCar *racer = owner && g_mp_racer_car ? g_mp_racer_car(owner) : NULL;
    return racer || !owner || !g_mp_cop_car ? racer : g_mp_cop_car(owner); /* cops.h's cars */
}

#include "mp_hit.h" /* RECOMP_MP_HIT: a repeatable car-to-car collision test */

/* This function surrounds the actual pair solver, not a whole physics frame.
 * 006e95a0 separates the bodies, then 006e9890 writes their resulting linear
 * and angular velocities. The two RigidBody pointers are cdecl args 1 and 2.
 * Our car against another player's: every game solves the contact for its own
 * car at once, as for an AI car, and then leaves the other car to the engine
 * (MP_FREE), so it is pushed and spun like one until its owner's poses, where
 * the owner's game did the same, catch up (mp_follow). The lower relay id also
 * sends the other car's change to its owner, who uses it only if its own game
 * saw no contact (mp_contacts). Two other players' cars touching are left to
 * their owners. NPC/world collisions keep the original path. */
static const char *mp_resolve_pair_body(const PopModApi *api, pop_cpu_v1 *cpu);

static void mp_resolve_pair(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv,
                            void *user) {
    (void)inv;
    (void)user;
    const uint32_t a = get_u32(cpu->esp + 4), b = get_u32(cpu->esp + 8);
    mp_hit_pair_before(a, b);
    const char *how = mp_resolve_pair_body(api, cpu);
    mp_hit_pair_after(a, b, how);
}

static const char *mp_resolve_pair_body(const PopModApi *api, pop_cpu_v1 *cpu) {
    MpShared *sh = mp_shared();
    const uint32_t flags = sh ? __atomic_load_n(&sh->flags, __ATOMIC_ACQUIRE) : 0;
    const uint32_t me = sh ? __atomic_load_n(&sh->me, __ATOMIC_ACQUIRE) : 0;
    const uint32_t a = get_u32(cpu->esp + 4), b = get_u32(cpu->esp + 8);
    const uint32_t oa = a ? get_u32(a + 0x2c) : 0, ob = b ? get_u32(b + 0x2c) : 0;
    MpCar *ra = mp_remote_owner(oa), *rb = mp_remote_owner(ob);
    MpCar *remote = oa == g_mp_player_simable ? rb : ob == g_mp_player_simable ? ra : NULL;
    const int shared = me && (flags & (MP_PAGE_GHOST | MP_PAGE_CONTACTS)) == MP_PAGE_CONTACTS;
    if (shared && ra && rb && ra->contacts && rb->contacts) {
        api->hook_return(api, cpu, cpu->eax & 0xffffff00u, 0);
        return "skip";
    }
    /* A solid car of ours: a friend's (class 7) or cops.h's copy of a friend's cop (class 4). */
    const uint32_t state = remote && remote->kind != MP_GHOST ? mp_state((ra == remote ? a : b) + 0x40u) : 0;
    if (!state) {
        if (g_mp_cop_pair)
            g_mp_cop_pair(a, b, 0);
        api->call_original(api, cpu->target, cpu);
        if (g_mp_cop_pair)
            g_mp_cop_pair(a, b, 1);
        return remote ? "remote-other" : "orig";
    }
    MpPose before, after;
    mp_read(state, &before);
    api->call_original(api, cpu->target, cpu);
    mp_read(state, &after);
    if (!mp_pose_ok(&before) || !mp_pose_ok(&after))
        return "local";
    float change = 0;
    for (int k = 0; k < 3; ++k)
        change += fabsf(after.p[k] - before.p[k]) + fabsf(after.v[k] - before.v[k]) +
                  fabsf(after.w[k] - before.w[k]);
    if (change < 0.0001f)
        return "near"; /* the solver found no contact */
    /* From now the engine moves the other car (mp_follow): writing its next pose over
     * this change would make it as heavy as a wall for our car. A touch while it is
     * being pulled back only makes the pull start gently again: a car kept free through
     * a long push drifted metres from where its owner's car really went (2026-10-25). */
    const double now = mp_now();
    remote->contact_at = now;
    if (remote->follow == MP_FOLLOWING && remote->kept) {
        remote->follow = MP_FREE;
        remote->free_at = now;
        memcpy(remote->hit_v0, remote->pose[remote->kept - 1].v, sizeof remote->hit_v0);
        memset(remote->hit_dv, 0, sizeof remote->hit_dv);
    } else if (remote->follow == MP_RETURNING) {
        remote->return_at = now;
    }
    for (int k = 0; k < 3 && remote->follow == MP_FREE; ++k)
        remote->hit_dv[k] += after.v[k] - before.v[k];
    if (g_mp_cop_touch)
        g_mp_cop_touch(remote, &before, &after); /* a friend's cop: cops.h sends the hit to its owner */
    if (!shared || !remote->contacts || me >= remote->id)
        return "local";
    if (!remote->pending) {
        memset(&remote->impact, 0, sizeof remote->impact);
        memcpy(remote->impact.at, before.p, sizeof before.p);
        remote->pending = 1;
    }
    for (int k = 0; k < 3; ++k) {
        remote->impact.dp[k] += after.p[k] - before.p[k];
        remote->impact.dv[k] += after.v[k] - before.v[k];
        remote->impact.dw[k] += after.w[k] - before.w[k];
    }
    return "solve";
}

static int mp_impact_vector_ok(const float v[3], float limit) {
    float length = 0;
    for (int k = 0; k < 3; ++k) {
        if (!isfinite(v[k]))
            return 0;
        length += v[k] * v[k];
    }
    return length <= limit * limit;
}

/* Our car's velocity and spin changed by a received impact (its push alone: its
 * position change came from where the cars stood in the other game). Wake a parked car
 * exactly as the solver does; just changing velocity leaves a sleeping body fixed in
 * place. */
static void mp_take_impact(uint32_t body, uint32_t state, const MpImpact *hit) {
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

static void mp_impact_log(const char *what, const MpImpact *hit) {
    if (hit->serial <= 3u) {
        char line[96];
        snprintf(line, sizeof line, "core.nfsc: received shared car impact %u: %s", hit->serial, what);
        log_line(line);
    }
    if (g_hit.mode) {
        char line[200];
        snprintf(line, sizeof line, "hitI r=%d t=%.1f %s serial %u dv=%.3f,%.3f,%.3f dp=%.3f,%.3f,%.3f", g_hit.run,
                 mp_now() - g_hit.rec_at, what, hit->serial, hit->dv[0], hit->dv[1], hit->dv[2], hit->dp[0],
                 hit->dp[1], hit->dp[2]);
        log_line(line);
    }
}

/* Received impacts, before publishing our next pose. Our own game solves every touch
 * of our car (mp_resolve_pair), so an impact from the lower id is the same push seen
 * from the other game, and is dropped when our game saw a contact with that car too
 * (MP_IMPACT_SEEN before it came, or MP_IMPACT_HOLD after: that player's car reaches
 * ours on this screen a little later than ours reached theirs on theirs). Only a touch
 * our game missed is applied. Acknowledgements travel with the resulting owner pose. */
static void mp_contacts(MpShared *sh, uint32_t player) {
    const uint32_t me = __atomic_load_n(&sh->me, __ATOMIC_ACQUIRE);
    const uint32_t flags = __atomic_load_n(&sh->flags, __ATOMIC_ACQUIRE);
    const int enabled = me && (flags & (MP_PAGE_GHOST | MP_PAGE_CONTACTS)) == MP_PAGE_CONTACTS;
    const uint32_t body = player ? get_u32(player + 0x70) : 0, state = mp_state(body);
    const double now = mp_now();
    uint32_t tail = __atomic_load_n(&sh->rx_tail, __ATOMIC_RELAXED);
    const uint32_t head = __atomic_load_n(&sh->rx_head, __ATOMIC_ACQUIRE);
    for (; tail != head; ++tail) {
        const MpImpact hit = sh->rx[tail % MP_IMPACTS];
        MpCar *from = NULL;
        for (int i = 0; i < MP_SLOTS; ++i)
            if (g_mp_remote[i].id == hit.id)
                from = &g_mp_remote[i];
        if (!enabled || !state || !from || from->spawn_overlap || !from->contacts || hit.id >= me ||
            hit.serial <= from->applied || hit.key != mp_model_key(player) ||
            (uint32_t)(mp_ms() - hit.ms) > 1000u ||
            !mp_impact_vector_ok(hit.dp, 6.0f) || !mp_impact_vector_ok(hit.dv, 100.0f) ||
            !mp_impact_vector_ok(hit.dw, 50.0f))
            continue;
        MpPose own;
        mp_read(state, &own);
        float distance[3];
        for (int k = 0; k < 3; ++k)
            distance[k] = own.p[k] - hit.at[k];
        if (!mp_pose_ok(&own) || !mp_impact_vector_ok(distance, 20.0f))
            continue;
        if (from->contact_at > 0 && now - from->contact_at < MP_IMPACT_SEEN) {
            from->applied = hit.serial;
            mp_impact_log("seen here already", &hit);
            continue;
        }
        if (from->holding) { /* two in one wait: one push of both */
            for (int k = 0; k < 3; ++k) {
                from->held.dv[k] += hit.dv[k];
                from->held.dw[k] += hit.dw[k];
            }
            from->held.serial = hit.serial;
        } else {
            from->held = hit;
            from->held_at = now;
            from->holding = 1;
        }
    }
    __atomic_store_n(&sh->rx_tail, tail, __ATOMIC_RELEASE);
    for (int i = 0; i < MP_SLOTS; ++i) {
        MpCar *from = &g_mp_remote[i];
        if (!from->holding)
            continue;
        if (!enabled || !state || !from->simable) {
            from->holding = 0;
        } else if (from->contact_at >= from->held_at) {
            from->holding = 0;
            from->applied = from->held.serial;
            mp_impact_log("seen here too", &from->held);
        } else if (now - from->held_at >= MP_IMPACT_HOLD) {
            mp_take_impact(body, state, &from->held);
            from->holding = 0;
            from->applied = from->held.serial;
            mp_impact_log("applied (not seen here)", &from->held);
        }
    }
    for (int i = 0; i < MP_SLOTS; ++i) {
        MpCar *c = &g_mp_remote[i];
        if (!c->pending)
            continue;
        if (!enabled || !c->contacts || !c->simable || mp_now() - c->contact_at > 500.0) {
            c->pending = 0;
            continue;
        }
        const uint32_t tx = __atomic_load_n(&sh->tx_head, __ATOMIC_RELAXED);
        if (tx - __atomic_load_n(&sh->tx_tail, __ATOMIC_ACQUIRE) >= MP_IMPACTS)
            break;
        c->impact.id = c->id;
        c->impact.serial = ++g_mp_impact_serial;
        c->impact.key = c->key;
        c->impact.ms = mp_ms();
        sh->tx[tx % MP_IMPACTS] = c->impact;
        c->sent = c->impact.serial;
        c->pending = 0;
        __atomic_store_n(&sh->tx_head, tx + 1u, __ATOMIC_RELEASE);
        if (c->sent <= 3u)
            log_line("core.nfsc: sent shared car impact");
        if (g_hit.mode) {
            char line[200];
            snprintf(line, sizeof line, "hitI r=%d t=%.1f sent serial %u dv=%.3f,%.3f,%.3f", g_hit.run,
                     mp_now() - g_hit.rec_at, c->sent, c->impact.dv[0], c->impact.dv[1], c->impact.dv[2]);
            log_line(line);
        }
    }
}

static void mp_drop(MpCar *c, const char *why) {
    if (c->simable && mp_listed(c->simable - 0x24))
        mp_kill(c->simable);
    if (c->simable && why) {
        char line[96];
        snprintf(line, sizeof line, "core.nfsc: mp car of player %u removed (%s)", c->id, why);
        log_line(line);
    }
    c->simable = 0;
    c->pending = c->holding = 0;
    c->follow = MP_FOLLOWING;
}

/* POLICE-MODEL CARS ARE KEPT. Removing (Kill) a car built in a police model ended the game now
 * and then (plans/police-sync.md "Known problems"), and a friend who turns police, or back,
 * changes model: its car here was removed twice (for the new model, then again ~0.6 s later for
 * its look). So a friend's car of a police model is not removed but kept out of sight (light bar
 * off, its controls cleared, MP_PARK_DOWN below our car) and taken by the next friend's car of
 * that model and class that has no look of its own, as cops.h keeps its cop copies. Only beyond
 * MP_SPARES, or when see-through cars change the class, is one removed. A police model is one
 * named in mp_cop_model, by the key 00606b60 makes of its name (mp_model_key gives the same). */
#define MP_SPARES 4
#define MP_PARK_DOWN 400.0f       /* metres below our car where kept cars wait */
#define MP_SET_PHYSICS 0x006d6950u /* IVehicle +0x38 SetPhysicsMode, thiscall (IVehicle *, 0 inactive / 1 active) */
#define MP_STRING_KEY 0x00606b60u /* cdecl (const char *name): the attribute key of a name */
#define MP_LIGHT_BAR 0x7000u      /* PVehicle + 0x150 light bits: the police light bar */
typedef struct MpSpare { uint32_t simable, kind, key; } MpSpare;
static MpSpare g_mp_spares[MP_SPARES];
static uint32_t g_mp_cop_keys[16];
static int g_mp_cop_keys_n = -1;

/* Whether key is a police car's model. */
static int mp_cop_model(uint32_t key) {
    static const char *const names[] = {"copcross", "copghost", "copgto", "copgtoghost", "copmidsize",
                                        "copmidsize_ce", "copmidsize_weak", "copsport", "copsportghost",
                                        "copsporthench", "copsuvl", "copsuvpatrol"};
    if (g_mp_cop_keys_n < 0 && g_mp_buf) {
        g_mp_cop_keys_n = 0;
        for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
            uint32_t at = g_mp_buf + 0xe0, k = 0; /* 0x20 spare bytes at the end of the buffer */
            for (; names[i][k] && k < 0x1f; ++k)
                g_api->guest_write_u8(g_api, at + k, (uint8_t)names[i][k]);
            g_api->guest_write_u8(g_api, at + k, 0);
            const uint32_t got = mp_call(MP_STRING_KEY, 0, &at, 1);
            if (got)
                g_mp_cop_keys[g_mp_cop_keys_n++] = got;
        }
    }
    for (int i = 0; key && i < g_mp_cop_keys_n; ++i)
        if (g_mp_cop_keys[i] == key)
            return 1;
    return 0;
}

/* Whether simable is a car in use here now (a kept one then is not ours any more). */
static int mp_spare_in_use(uint32_t simable) {
    for (int i = 0; i < MP_SLOTS; ++i)
        if (g_mp_remote[i].simable == simable)
            return 1;
    return g_mp_test.simable == simable || (g_mp_racer_car && g_mp_racer_car(simable)) ||
           (g_mp_cop_car && g_mp_cop_car(simable));
}

/* A friend's car goes: kept when it is of a police model and there is room, else removed. */
static void mp_drop_friend(MpCar *c, const char *why) {
    const uint32_t car = c->simable ? c->simable - 0x24 : 0;
    if (car && c->kind != MP_GHOST && mp_listed(car) && mp_kind_kept(car, c->kind) && mp_cop_model(mp_model_key(car))) {
        for (int i = 0; i < MP_SPARES; ++i) {
            MpSpare *sp = &g_mp_spares[i];
            if (sp->simable)
                continue;
            mp_put(car + 0x150, get_u32(car + 0x150) & ~MP_LIGHT_BAR);
            const uint32_t in = get_u32(car + 0xec); /* IInput::ClearInput: no gas or nitrous left on */
            if (in && get_u32(in))
                mp_call(get_u32(get_u32(in) + 4), in, NULL, 0);
            sp->simable = c->simable;
            sp->kind = c->kind;
            sp->key = mp_model_key(car);
            { /* parked the game's way at once (inactive: not drawn, not solid), before the friend's
               * next car is built where this one stands; a car put below our car came back up at
               * our car (2026-10-26, run c23: the police copy under the friend's own car) */
                const uint32_t off = 0;
                mp_call(MP_SET_PHYSICS, car + 0xb8, &off, 1);
            }
            char line[128];
            snprintf(line, sizeof line, "core.nfsc: mp car of player %u kept for reuse (police model %08x; %s)", c->id,
                     sp->key, why ? why : "gone");
            log_line(line);
            c->simable = 0;
            break;
        }
    }
    mp_drop(c, why);
}

/* A kept car of model key and class kind, or 0. */
static uint32_t mp_spare_take(uint32_t key, uint32_t kind) {
    for (int i = 0; i < MP_SPARES; ++i) {
        MpSpare *sp = &g_mp_spares[i];
        if (sp->simable && sp->key == key && sp->kind == kind && mp_listed(sp->simable - 0x24) &&
            mp_kind_kept(sp->simable - 0x24, kind) && !mp_spare_in_use(sp->simable)) {
            const uint32_t simable = sp->simable;
            sp->simable = 0;
            return simable;
        }
    }
    return 0;
}

/* The kept cars, every frame: out of sight below our car (here, when we drive), standing still;
 * one gone with the world is forgotten, one of a class no longer used removed. */
static void mp_spares(const float here[3], int driving) {
    for (int i = 0; i < MP_SPARES; ++i) {
        MpSpare *sp = &g_mp_spares[i];
        if (!sp->simable)
            continue;
        if (!mp_listed(sp->simable - 0x24) || !mp_kind_kept(sp->simable - 0x24, sp->kind) ||
            mp_spare_in_use(sp->simable)) {
            sp->simable = 0;
            continue;
        }
        if (mp_kind() == MP_GHOST) {
            mp_kill(sp->simable);
            sp->simable = 0;
            continue;
        }
        if (get_u32(sp->simable - 0x24 + 0x160) == 0)
            continue; /* parked inactive (mp_drop_friend): it stays where it is, unseen */
        const uint32_t state = driving ? mp_state(get_u32(sp->simable - 0x24 + 0x70)) : 0;
        MpPose park;
        if (!state)
            continue;
        mp_read(state, &park);
        if (!mp_pose_ok(&park))
            continue;
        park.p[0] = here[0] + 6.0f * (float)i;
        park.p[1] = here[1] - MP_PARK_DOWN;
        park.p[2] = here[2];
        memset(park.v, 0, sizeof park.v);
        memset(park.w, 0, sizeof park.w);
        mp_place(sp->simable, &park);
    }
}

/* The physics time in ms: the page sends it with each pose, so the other
 * players place the pose at the moment it describes, however many steps the
 * frame took and however late the page gets to it. */
static uint32_t mp_ms(void) {
    return (uint32_t)((g_mp_steps * 1000u + 30u) / 60u);
}

/* MainLoop: counts the physics steps it queues, as it works them out. */
static void mp_main_loop(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    const float before = bits_float(get_u32(MP_STEP_LEFT)) + bits_float(get_u32(cpu->esp + 4));
    api->call_original(api, cpu->target, cpu);
    if (cpu->eax & 0xffu) {
        long n = lroundf((before - bits_float(get_u32(MP_STEP_LEFT))) * 60.0f);
        g_mp_steps += (uint64_t)(n < 1 ? 1 : n > 8 ? 8 : n);
    }
}

/* This player's car, for the page to send. */
static void mp_publish(MpShared *sh, uint32_t player) {
    static MpPose last;
    static uint32_t last_ms;
    static double last_real;
    static float sim_sum, real_sum;
    static double span_sim, span_real;
    static int n;
    __atomic_fetch_add(&sh->seq, 1u, __ATOMIC_SEQ_CST);
    sh->frame = g_mp_frame;
    sh->ms = mp_ms();
    sh->state = 0;
    for (int i = 0; i < MP_SLOTS; ++i)
        sh->labels[i].applied = g_mp_remote[i].applied;
    if (player) {
        const uint32_t state = mp_state(get_u32(player + 0x70));
        if (state) {
            mp_read(state, &sh->pose);
            if (mp_pose_ok(&sh->pose)) {
                sh->key = mp_model_key(player);
                sh->state = 1;
            }
        }
    }
    __atomic_fetch_add(&sh->seq, 1u, __ATOMIC_SEQ_CST);
    /* How well each clock explains the motion: the pose against the last one
     * moved on by its speed over the physics time, and over the real time. */
    const double real = mp_now();
    if (sh->state && sh->ms != last_ms && g_mp_block && sh->page) {
        if (last_ms && real - last_real < 200) {
            const float ds = (float)(sh->ms - last_ms) / 1000.0f, dr = (float)(real - last_real) / 1000.0f;
            float es = 0, er = 0;
            for (int k = 0; k < 3; ++k) {
                const float a = sh->pose.p[k] - (last.p[k] + last.v[k] * ds);
                const float b = sh->pose.p[k] - (last.p[k] + last.v[k] * dr);
                es += a * a;
                er += b * b;
            }
            sim_sum += sqrtf(es);
            real_sum += sqrtf(er);
            span_sim += ds;
            span_real += dr;
            if (++n == 600) {
                char line[160];
                snprintf(line, sizeof line,
                         "core.nfsc: mp sent poses off their speed by %.3f m (physics clock), %.3f m (real clock); physics ran at %.0f%% of real time",
                         sim_sum / n, real_sum / n, span_real > 0 ? 100.0 * span_sim / span_real : 0.0);
                log_line(line);
                n = 0;
                sim_sum = real_sum = 0;
                span_sim = span_real = 0;
            }
        }
        last = sh->pose;
        last_ms = sh->ms;
        last_real = real;
    }
}

static double mp_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* Keeps the slot's poses this car has not taken yet. */
static void mp_take(MpCar *c, const MpSlot *s, double now) {
    (void)now;
    if (s->count < c->taken || s->count - c->taken > 1000u) { /* a new player in the slot */
        c->taken = s->count > MP_RING ? s->count - MP_RING : 0;
        c->kept = 0;
        c->played_at = 0;
    }
    if (s->count - c->taken > MP_RING)
        c->taken = s->count - MP_RING;
    for (; c->taken < s->count; ++c->taken) {
        const MpSnap *n = &s->ring[c->taken % MP_RING];
        const double at = (double)n->ms;
        MpPose pose = n->pose;
        if (!mp_pose_ok(&pose))
            continue;
        if (c->kept && at < c->at[c->kept - 1] - 1000.0) {
            c->kept = 0; /* the sender's clock went back: it restarted */
            c->played_at = 0;
        }
        if (c->kept && at <= c->at[c->kept - 1])
            continue;
        if (c->kept == MP_KEEP) {
            memmove(c->at, c->at + 1, sizeof c->at[0] * (MP_KEEP - 1));
            memmove(c->pose, c->pose + 1, sizeof c->pose[0] * (MP_KEEP - 1));
            --c->kept;
        }
        c->at[c->kept] = at;
        c->pose[c->kept] = pose;
        ++c->kept;
    }
}

/* Where the car was at sender time t. */
static void mp_sample(const MpCar *c, double t, MpPose *out) {
    int i = c->kept - 1;
    while (i > 0 && c->at[i] > t)
        --i;
    const MpPose *a = &c->pose[i];
    if (i + 1 >= c->kept || c->at[i] > t) {
        double dt = (t - c->at[i]) / 1000.0;
        dt = dt < 0 ? 0 : dt > MP_EXTRAPOLATE / 1000.0 ? MP_EXTRAPOLATE / 1000.0 : dt;
        *out = *a;
        for (int k = 0; k < 3; ++k)
            out->p[k] += a->v[k] * (float)dt;
        return;
    }
    const MpPose *b = &c->pose[i + 1];
    const float f = (float)((t - c->at[i]) / (c->at[i + 1] - c->at[i]));
    float dot = 0, len = 0;
    for (int k = 0; k < 4; ++k)
        dot += a->q[k] * b->q[k];
    const float sign = dot < 0 ? -1.0f : 1.0f;
    for (int k = 0; k < 4; ++k) {
        out->q[k] = a->q[k] * (1 - f) + sign * b->q[k] * f;
        len += out->q[k] * out->q[k];
    }
    len = len > 0 ? sqrtf(len) : 1.0f;
    for (int k = 0; k < 4; ++k)
        out->q[k] /= len;
    for (int k = 0; k < 3; ++k) {
        out->p[k] = a->p[k] * (1 - f) + b->p[k] * f;
        out->v[k] = a->v[k] * (1 - f) + b->v[k] * f;
        out->w[k] = a->w[k] * (1 - f) + b->w[k] * f;
    }
}

/* Moves a car's playback clock to now: real time passes, sped up or slowed
 * down by up to 10% to stay its lag (MP_DELAY, or less near our car) behind the
 * newest pose. Catch up when behind; never rewind an unchanged packet when
 * delivery pauses. Only a new sender timeline (mp_take) resets playback. */
static double mp_playback(MpCar *c, double now) {
    const double target = c->at[c->kept - 1] - (c->lag > 0 ? c->lag : MP_DELAY);
    if (!c->played_at || c->play < target - 500.0) {
        c->play = target;
    } else {
        double rate = 1.0 + (target - c->play) * 0.001;
        rate = rate < 0.9 ? 0.9 : rate > 1.1 ? 1.1 : rate;
        c->play += (now - c->played_at) * rate;
    }
    const double limit = c->at[c->kept - 1] + MP_EXTRAPOLATE;
    if (c->play > limit)
        c->play = limit;
    c->played_at = now;
    return c->play;
}

/* Sets a car's speed, spin and rotation, leaving its position to the physics. */
static void mp_steer(uint32_t simable, const MpPose *pose) {
    const uint32_t body = get_u32(simable - 0x24 + 0x70), state = mp_state(body);
    if (!state)
        return;
    for (int k = 0; k < 3; ++k) {
        put_float(state + 0x20 + 4u * k, pose->v[k]);
        put_float(state + 0x30 + 4u * k, pose->w[k]);
    }
    for (int k = 0; k < 4; ++k)
        put_float(g_mp_buf + 0x90 + 4u * k, pose->q[k]);
    const uint32_t arg = g_mp_buf + 0x90;
    mp_call(MP_SET_ORIENTATION, body, &arg, 1);
    /* A body that came to rest is asleep, and a sleeping body ignores its speed. */
    g_api->guest_write_u8(g_api, state + 0x5d, 0);
    g_api->guest_write_u8(g_api, state + 0x1c, (get_u32(state + 0x1c) & 0xffu) | 0x40u);
    mp_call(MP_WAKE_BODY, body - 0x40u, NULL, 0);
}

/* Moves one built car for this moment, into *out (as drawn). late: the call after the
 * frame's physics, as the view is set up (the pull back runs there, once a frame).
 * Following, it is put exactly where its owner's poses say. After touching our car it
 * is left to the engine, as an AI car would be, until its owner's poses show the same
 * push (MP_FREE_MS at most), then pulled to those poses by a critically damped spring
 * on its speed (so it drives back onto them, never jumps), gentle at first; once there
 * it follows again. Only a car more than MP_SNAP off is put back at once, and never
 * onto our car (a snap 8 m back landed inside it after a long push, 2026-10-25). */
/* Whether the owner's newest pose already shows the push our game gave its car: its
 * speed changed along the push by at least half of it (the owner's game solved the
 * same touch). Then its poses are the better guide to where the car goes next. */
static int mp_owner_pushed(const MpCar *c) {
    float push2 = 0, along = 0;
    for (int k = 0; k < 3; ++k) {
        push2 += c->hit_dv[k] * c->hit_dv[k];
        along += (c->pose[c->kept - 1].v[k] - c->hit_v0[k]) * c->hit_dv[k];
    }
    return push2 > 0.01f && along >= 0.5f * push2;
}

static void mp_follow(MpCar *c, double now, int late, MpPose *out) {
    MpPose target, car;
    if (c->follow == MP_FREE && now - c->free_at >= MP_FREE_MIN_MS &&
        (now - c->free_at >= MP_FREE_MS || mp_owner_pushed(c))) {
        c->follow = MP_RETURNING;
        c->return_at = c->pull_at = now;
        /* The playback clock to the newest pose: its owner's car reacted to the same
         * touch, later than ours did by about the delay, so the poses after the touch
         * are the ones to drive back onto. mp_playback then eases the delay back. */
        c->play = c->at[c->kept - 1];
        c->played_at = now;
    }
    mp_sample(c, mp_playback(c, now), &target);
    if (!c->visual_only && g_mp_drive_car)
        g_mp_drive_car(c, c->play, late); /* its owner's controls of that moment */
    if (!c->visual_only && g_mp_follow_target)
        g_mp_follow_target(c, &target);
    const uint32_t state = c->follow != MP_FOLLOWING ? mp_state(get_u32(c->simable - 0x24 + 0x70)) : 0;
    if (state)
        mp_read(state, &car);
    float error = 0;
    for (int k = 0; state && k < 3; ++k)
        error += (target.p[k] - car.p[k]) * (target.p[k] - car.p[k]);
    float clear = 1e9f; /* how far its place is from our car */
    const uint32_t ours = g_mp_player_simable ? mp_state(get_u32(g_mp_player_simable - 0x24 + 0x70)) : 0;
    if (ours && error > MP_SNAP * MP_SNAP) {
        clear = 0;
        for (int k = 0; k < 3; ++k) {
            const float d = target.p[k] - bits_float(get_u32(ours + 0x10 + 4u * k));
            clear += d * d;
        }
    }
    if (!state || !mp_pose_ok(&car) || error > MP_SNAP_FAR * MP_SNAP_FAR ||
        (error > MP_SNAP * MP_SNAP && clear > MP_SNAP_CLEAR * MP_SNAP_CLEAR)) {
        c->follow = MP_FOLLOWING;
        mp_place(c->simable, &target);
        *out = target;
        return;
    }
    *out = car;
    if (c->follow == MP_FREE || !late)
        return;
    double dt = (now - c->pull_at) / 1000.0;
    dt = dt < 0 ? 0 : dt > 0.1 ? 0.1 : dt;
    c->pull_at = now;
    float ramp = (float)((now - c->return_at) / MP_RETURN_MS);
    ramp = ramp > 1.0f ? 1.0f : ramp;
    const float rate = MP_RETURN_RATE * (0.3f + 0.7f * ramp);
    float a[3], a2 = 0, dv2 = 0, dot = 0, len = 0;
    for (int k = 0; k < 3; ++k) {
        a[k] = rate * rate * (target.p[k] - car.p[k]) + 2.0f * rate * (target.v[k] - car.v[k]);
        a2 += a[k] * a[k];
        dv2 += (target.v[k] - car.v[k]) * (target.v[k] - car.v[k]);
    }
    const float scale = a2 > MP_RETURN_ACCEL * MP_RETURN_ACCEL ? MP_RETURN_ACCEL / sqrtf(a2) : 1.0f;
    const float f = 1.0f - expf(-2.0f * rate * (float)dt);
    for (int k = 0; k < 4; ++k)
        dot += target.q[k] * car.q[k];
    if (error < 0.03f * 0.03f && dv2 < 0.3f * 0.3f && fabsf(dot) > 0.99985f) {
        c->follow = MP_FOLLOWING; /* back on its poses: the last few cm are placed */
        mp_place(c->simable, &target);
        *out = target;
        return;
    }
    for (int k = 0; k < 3; ++k) {
        car.v[k] += a[k] * scale * (float)dt;
        car.w[k] += (target.w[k] - car.w[k]) * f;
    }
    for (int k = 0; k < 4; ++k) {
        car.q[k] = car.q[k] * (1.0f - f) + target.q[k] * (dot < 0 ? -f : f);
        len += car.q[k] * car.q[k];
    }
    for (int k = 0; k < 4; ++k)
        car.q[k] /= sqrtf(len);
    mp_steer(c->simable, &car);
    *out = car;
}

/* Places every built car where it belongs now. */
static void mp_place_all(MpPose *first, int late) {
    const double now = mp_now();
    for (int i = 0; i < MP_SLOTS; ++i) {
        MpCar *c = &g_mp_remote[i];
        if (!c->simable || !c->kept)
            continue;
        MpPose pose;
        mp_follow(c, now, late, &pose);
        memcpy(c->drawn, pose.p, sizeof c->drawn);
        if (first) {
            *first = pose;
            first = NULL;
        }
    }
    if (g_mp_cop_place)
        g_mp_cop_place(now, late);
}

/* Smoothness of what is drawn: how far each frame's position is from where
 * the previous frame's position and speed said it would be. */
static double g_mp_drawn_at;
static MpPose g_mp_drawn;
static float g_mp_jitter_max, g_mp_jitter_sum;
static int g_mp_jitter_n;

static uint32_t g_mp_view; /* the main view's matrix block (this of 0071b780), for the AFTER hook */

/* Projects a sim-space point with a 4x4 row-vector matrix at m; swizzle: the
 * render space is (z, -x, y) of the sim space. Writes x, y (0..1 of the
 * screen) and w; returns w. */
static float mp_project(uint32_t m, const float p[3], int swizzle, float out[2]) {
    const float r[4] = {swizzle ? p[2] : p[0], swizzle ? -p[0] : p[1], swizzle ? p[1] : p[2], 1.0f};
    float c[4];
    for (int j = 0; j < 4; ++j) {
        c[j] = 0;
        for (int i = 0; i < 4; ++i)
            c[j] += r[i] * bits_float(get_u32(m + 4u * (4u * i + j)));
    }
    if (c[3] == 0)
        c[3] = 1e-6f;
    out[0] = (c[0] / c[3] + 1.0f) * 0.5f;
    out[1] = (1.0f - c[1] / c[3]) * 0.5f;
    return c[3];
}

/* After the main view's matrices are made: where each car's name goes. */
static void mp_view_ready(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)cpu;
    (void)inv;
    (void)user;
    MpShared *sh = mp_shared();
    if (!sh || !g_mp_view)
        return;
    for (int i = 0; i < MP_SLOTS; ++i) {
        const MpCar *c = &g_mp_remote[i];
        MpLabel *l = &sh->labels[i];
        if (g_mp_racer && g_mp_racer(g_mp_seen_slot[i].id))
            continue; /* race.h labels its racers' cars */
        float s[2];
        const float above[3] = {c->drawn[0], c->drawn[1] + MP_LABEL_UP, c->drawn[2]};
        const float depth = c->simable && c->kept ? mp_project(g_mp_view + 0x80, above, 1, s) : 0.0f;
        l->x = s[0];
        l->y = s[1];
        l->depth = depth > 0.5f && s[0] > -0.2f && s[0] < 1.2f && s[1] > -0.2f && s[1] < 1.2f ? depth : 0.0f;
    }
}

/* Called as each view is set up for drawing (00071b780); the main view is 1. */
static void mp_render_view(uint32_t id, uint32_t self) {
    if (id == 1)
        g_mp_view = self;
    if (id != 1 || !g_mp_block)
        return;
    MpPose pose;
    pose.p[0] = 1e30f;
    mp_place_all(&pose, 1);
    mp_hit_frame();
    if (pose.p[0] > 1e29f)
        return;
    const double now = mp_now();
    if (g_mp_drawn_at > 0 && now - g_mp_drawn_at < 200) {
        const float dt = (float)((now - g_mp_drawn_at) / 1000.0);
        float e = 0;
        for (int k = 0; k < 3; ++k) {
            const float d = pose.p[k] - (g_mp_drawn.p[k] + g_mp_drawn.v[k] * dt);
            e += d * d;
        }
        e = sqrtf(e);
        g_mp_jitter_sum += e;
        if (e > g_mp_jitter_max)
            g_mp_jitter_max = e;
        if (++g_mp_jitter_n == 600) {
            char line[128];
            snprintf(line, sizeof line, "core.nfsc: mp drawn motion off its own speed by %.3f m a frame on average, %.3f m at most",
                     g_mp_jitter_sum / g_mp_jitter_n, g_mp_jitter_max);
            log_line(line);
            g_mp_jitter_n = 0;
            g_mp_jitter_sum = g_mp_jitter_max = 0;
        }
    }
    g_mp_drawn = pose;
    g_mp_drawn_at = now;
}

/* The other players' cars, from the page's slots. */
static void mp_remotes(MpShared *sh, uint32_t player) {
    const double now = mp_now();
    float here[3] = {0, 0, 0};
    const uint32_t state = player ? mp_state(get_u32(player + 0x70)) : 0;
    g_mp_page_ghost = (__atomic_load_n(&sh->flags, __ATOMIC_RELAXED) & MP_PAGE_GHOST) != 0;
    for (int i = 0; i < 3 && state; ++i)
        here[i] = bits_float(get_u32(state + 0x10 + 4u * i));
    mp_spares(here, state != 0);
    for (int i = 0; i < MP_SLOTS; ++i) {
        MpSlot *src = &sh->slots[i], copy;
        const uint32_t before = __atomic_load_n(&src->seq, __ATOMIC_ACQUIRE);
        if (!(before & 1)) {
            memcpy(&copy, src, sizeof copy);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (__atomic_load_n(&src->seq, __ATOMIC_ACQUIRE) == before)
                g_mp_seen_slot[i] = copy;
        }
        const MpSlot *s = &g_mp_seen_slot[i];
        MpCar *c = &g_mp_remote[i];
        /* Gone with the world, or gone and its memory given to another car: a car built while
         * this player's race was loading was replaced by an AI racer at the same address
         * (2026-10-24), which was then moved as the friend; ours keep their driver class. */
        if (c->simable && (!mp_listed(c->simable - 0x24) || !mp_kind_kept(c->simable - 0x24, c->kind))) {
            if (g_mp_friend_car)
                g_mp_friend_car(i, c->id, 0);
            c->simable = 0;
        }
        const int active = sh->page && s->id && s->mode == 1 && state && !(g_mp_racer && g_mp_racer(s->id));
        const int relook = c->simable && active && c->id == s->id && g_mp_relook && g_mp_relook(i, s->id, s->key);
        if (c->simable && (!active || c->id != s->id || c->key != s->key || c->kind != mp_friend_kind(s->id) || relook)) {
            if (g_mp_friend_car)
                g_mp_friend_car(i, c->id, 0);
            mp_drop_friend(c, active ? "another player, car or look" : "left or stopped driving");
        }
        if (relook)
            c->tries = 0; /* it was built, so its model is here: build it again in its own model */
        if (!active) {
            c->id = 0;
            continue;
        }
        if (c->id != s->id || c->key != s->key) {
            c->id = s->id;
            c->key = s->key;
            c->wait = 0;
            c->tries = 0;
            c->taken = s->count + 1; /* start over in mp_take */
            c->applied = c->sent = c->ack = 0;
            c->pending = c->holding = 0;
            c->follow = MP_FOLLOWING;
            c->contact_at = 0;
        }
        c->contacts = s->pad[0];
        c->ack = s->pad[1];
        mp_take(c, s, now);
        if (!c->kept)
            continue;
        const MpPose *last = &c->pose[c->kept - 1];
        float d2 = 0;
        for (int k = 0; k < 3; ++k)
            d2 += (last->p[k] - here[k]) * (last->p[k] - here[k]);
        /* Two players can leave the same safehouse at the same coordinates. A solid
         * copy there pins the real car before either player can drive away. Keep
         * only this pair non-contact until the owners are safely apart; do not use
         * a timeout that can make two still-overlapping cars solid again. */
        if (c->spawn_overlap && d2 > 7.0f * 7.0f) {
            c->spawn_overlap = 0;
            log_line("core.nfsc: mp shared spawn cleared: player collisions restored");
        }
        /* Near our car a solid car is played back closer to its owner's now, so a touch
         * happens where both games have the two cars (MP_NEAR_DELAY); further away the
         * longer delay rides out the network's hiccups. mp_playback eases into a new
         * delay at 10% of real time. */
        float near = (MP_NEAR_FROM - sqrtf(d2)) / (MP_NEAR_FROM - MP_NEAR_TO);
        near = c->kind == MP_GHOST || near < 0 ? 0 : near > 1 ? 1 : near;
        c->lag = MP_DELAY + (MP_NEAR_DELAY - MP_DELAY) * near;
        if (c->simable) {
            if (d2 > MP_FAR * MP_FAR) {
                if (g_mp_friend_car)
                    g_mp_friend_car(i, c->id, 0);
                mp_drop_friend(c, "far away");
            }
            continue;
        }
        if (d2 > MP_NEAR * MP_NEAR || (c->wait && --c->wait))
            continue;
        /* Another model may not be loaded: the first try builds it as it is, the second asks
         * for it to be spooled. Both were refused for a friend whose car this game had not
         * loaded (2026-10-24: one window never saw the other), so from the third try the
         * friend is shown in this player's own model, which is always loaded, rather than
         * not at all. */
        const int own_model = c->tries >= 2;
        const uint32_t flags = 2u | 8u | (!own_model && (c->tries & 1) ? 1u : 0u);
        const uint32_t look = g_mp_look ? g_mp_look(i, s->id, own_model ? 0u : s->key) : 0;
        c->kind = mp_friend_kind(s->id);
        c->follow = MP_FOLLOWING;
        c->spawn_overlap = d2 < 6.0f * 6.0f;
        const uint32_t model = own_model ? mp_model_key(player) : s->key;
        const uint32_t kept = !look && c->kind != MP_GHOST && mp_cop_model(model) ? mp_spare_take(model, c->kind) : 0;
        c->simable = kept ? kept : mp_build(player, c->kind, own_model ? 0u : s->key, last->p, flags, look);
        if (kept) {
            mp_place(kept, last); /* at the friend's pose, then active again (IVehicle::Activate) */
            if (get_u32(kept - 0x24 + 0xb8 + 0xa8) == 0) {
                const uint32_t on = 1;
                mp_call(MP_SET_PHYSICS, kept - 0x24 + 0xb8, &on, 1);
            }
            char line[128];
            snprintf(line, sizeof line, "core.nfsc: mp car of player %u took a kept car (police model %08x)", s->id, model);
            log_line(line);
            c->tries = 0;
        }
        if (c->simable && g_mp_friend_car)
            g_mp_friend_car(i, s->id, c->simable);
        ++c->tries;
        if (!kept && (c->tries <= 4 || c->simable)) {
            char line[160];
            snprintf(line, sizeof line, "core.nfsc: mp car of player %u (model %08x) %s%s%s%s", s->id, s->key,
                     c->simable ? "built" : "refused", flags & 1 ? " (spooled)" : "", look ? " in its owner's look" : "",
                     own_model ? " in this player's own model (theirs is not loaded here)" : "");
            log_line(line);
        }
        if (!c->simable)
            c->wait = 120;
    }
    mp_place_all(NULL, 0);
}

/* Records the player's driving, parks the test car meanwhile, then replays the take. */
static void mp_test_follow(uint32_t player, int mode) {
    const uint32_t to = get_u32(g_mp_test.simable - 0x24 + 0x70);
    const uint32_t from = mp_state(get_u32(player + 0x70)), state = mp_state(to);
    if (!from || !state)
        return;
    MpPose now, escort;
    mp_read(state, &now);
    if (g_mp_recorded || g_mp_played) {
        float d = 0.0f;
        for (int i = 0; i < 3; ++i)
            d += (now.p[i] - g_mp_last.p[i]) * (now.p[i] - g_mp_last.p[i]);
        if (d > g_mp_drift)
            g_mp_drift = d; /* squared metres, against where its own speed would take it at 60 fps */
    }
    const MpPose *next = &g_mp_hold;
    if (mode == 2) {
        mp_read(from, &escort);
        for (int i = 0; i < 3; ++i)
            escort.p[i] += MP_AHEAD * bits_float(get_u32(from + 0x90 + 4u * i)) -
                           MP_SIDE * bits_float(get_u32(from + 0x70 + 4u * i));
        next = &escort;
        g_mp_played = 1;
    } else if (g_mp_recorded < MP_TAKE) {
        mp_read(from, &g_mp_take[g_mp_recorded++]);
    } else {
        next = &g_mp_take[g_mp_played++ % MP_TAKE];
    }
    g_mp_last = *next;
    for (int i = 0; i < 3; ++i)
        g_mp_last.p[i] += next->v[i] / 60.0f;
    mp_write(to, state, next);
    if ((g_mp_recorded + g_mp_played) % 300 == 0) {
        char line[128];
        snprintf(line, sizeof line, "core.nfsc: mp test car %s, frame %d, largest push %.2f m",
                 g_mp_played ? "replaying" : "recording", g_mp_played ? g_mp_played % MP_TAKE : g_mp_recorded,
                 sqrtf(g_mp_drift));
        log_line(line);
        g_mp_drift = 0.0f;
    }
}

static void mp_test(uint32_t player) {
    const int mode = mp_test_mode();
    if (g_mp_test.simable && !mp_listed(g_mp_test.simable - 0x24))
        g_mp_test.simable = 0;
    if (g_mp_test.simable && (!mode || !player || g_mp_test.kind != mp_kind())) {
        mp_drop(&g_mp_test, NULL);
        log_line("core.nfsc: mp test car removed");
    }
    if (!mode || !player) {
        g_mp_seen = 0;
        return;
    }
    if (g_mp_test.simable) {
        mp_test_follow(player, mode);
        return;
    }
    if (++g_mp_seen < 180 || g_mp_test.tries >= 3) /* 3 s after the player's car appears */
        return;
    if (g_mp_test.wait) {
        --g_mp_test.wait;
        return;
    }
    const uint32_t state = mp_state(get_u32(player + 0x70));
    float pos[3];
    for (int i = 0; i < 3; ++i)
        pos[i] = bits_float(get_u32(state + 0x10 + 4u * i)) + MP_SIDE * bits_float(get_u32(state + 0x70 + 4u * i));
    ++g_mp_test.tries;
    g_mp_test.kind = mp_kind();
    g_mp_test.simable = mp_build(player, g_mp_test.kind, 0, pos, 2u | 8u, 0);
    if (g_mp_test.simable) {
        mp_read(state, &g_mp_hold);
        for (int i = 0; i < 3; ++i) {
            g_mp_hold.p[i] = pos[i];
            g_mp_hold.v[i] = g_mp_hold.w[i] = 0.0f;
        }
        g_mp_recorded = g_mp_played = 0;
    } else {
        g_mp_test.wait = 300;
    }
    char line[96];
    snprintf(line, sizeof line, "core.nfsc: mp test car %s at %.1f %.1f %.1f",
             g_mp_test.simable ? "built" : "refused", pos[0], pos[1], pos[2]);
    log_line(line);
}

/* The letters the game's keyboard bindings use, for MpShared.keys; 0 while the
 * binding table is not made. 144 slots a frame, as cheap as pad_watch. */
static uint32_t mp_keys(void) {
    static const char by_dik[] = "QWERTYUIOP....ASDFGHJKL.....ZXCVBNM"; /* DIK 0x10..0x32 */
    uint32_t keys = MP_KEYS_KNOWN;
    for (unsigned r = 0; r < PAD_ROWS; ++r)
        for (unsigned s = 0; s < 2; ++s) {
            const uint32_t at = pad_triple(r, s);
            if (!at)
                return 0;
            const uint32_t dik = get_u32(at + 4) - 0x10u;
            if (get_u32(at) == 1u && dik < sizeof by_dik - 1 && by_dik[dik] != '.')
                keys |= 1u << (by_dik[dik] - 'A');
        }
    return keys;
}

/* The game's own Online mode (Custom / Quick Match through the server's race host; LAN mode only,
 * online_net.h) races with its own copies of the other players' cars. While it is in a game
 * (IsOnlineGame 0064b980, cdecl, AL) this file treats the player as not driving: it builds no car
 * from the relay and sends no pose, so nobody gets a second copy of a racer (as g_mp_racer does
 * for race.h's racers). Free roam comes back when that game ends. */
#define MP_IS_ONLINE_GAME 0x0064b980u
#define MP_IS_PAUSED 0x00699f30u /* GameFlowManager::IsPaused (cdecl), AL */
static int mp_online_game(void) {
    static int lan = -1, was;
    if (lan < 0) {
        const char *v = getenv("RECOMP_LAN");
        lan = v && *v && strcmp(v, "0") != 0;
    }
    const int now = lan && (mp_call(MP_IS_ONLINE_GAME, 0, NULL, 0) & 0xffu) != 0;
    if (now != was) {
        was = now;
        log_line(now ? "core.nfsc: mp: in the game's own online race: no free-roam cars"
                     : "core.nfsc: mp: out of the game's own online race: free-roam cars again");
    }
    return now;
}

static void mp_service(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)cpu;
    (void)inv;
    (void)user;
    ++g_mp_frame;
    uint32_t player = mp_player();
    if (player && mp_online_game())
        player = 0;
    const int paused = player && (mp_call(MP_IS_PAUSED, 0, NULL, 0) & 0xffu) != 0;
    g_mp_player_simable = player ? player + 0x24u : 0;
    MpShared *sh = mp_shared();
    if (sh) {
        mp_remotes(sh, player);
        mp_contacts(sh, player);
        mp_publish(sh, paused ? 0 : player);
        if (g_mp_drive)
            g_mp_drive(sh, paused ? 0 : player);
        __atomic_store_n(&sh->keys, mp_keys(), __ATOMIC_RELAXED);
    }
    mp_test(player);
    mp_hit_service(player);
}

static void mp_install(void) {
    if (g_api->guest_alloc(g_api, 0x100, &g_mp_buf) != POP_OK ||
        g_api->guest_alloc(g_api, sizeof(MpShared), &g_mp_block) != POP_OK) {
        log_line("core.nfsc: other players' cars unavailable (no guest memory)");
        return;
    }
    MpShared *sh = mp_shared();
    if (!sh) {
        g_mp_block = 0;
        return;
    }
    memset(sh, 0, sizeof *sh);
    sh->magic = MP_MAGIC;
    sh->version = 2;
    if (install(MP_FE_SERVICE, mp_service, POP_HOOK_BEFORE, 26) != POP_OK) {
        log_line("core.nfsc: other players' cars unavailable");
        return;
    }
    if (install(0x0071b780u, mp_view_ready, POP_HOOK_AFTER, 28) != POP_OK)
        log_line("core.nfsc: no view test");
    if (install(MP_MAIN_LOOP, mp_main_loop, POP_HOOK_REPLACE, 27) != POP_OK)
        log_line("core.nfsc: other players' cars use the real clock (no physics step count)");
    mp_hit_install();
    if (install(MP_RESOLVE_PAIR, mp_resolve_pair, POP_HOOK_REPLACE, 29) != POP_OK) {
        sh->version = 1; /* do not negotiate a response we cannot calculate */
        log_line("core.nfsc: shared collision response unavailable");
    } else {
        log_line("core.nfsc: shared car impact response ready");
    }
#ifdef __EMSCRIPTEN__
    MAIN_THREAD_ASYNC_EM_ASM({
        var block = {};
        block.memory = wasmMemory;
        block.ptr = $0;
        globalThis.carbonMpBlock = block;
        globalThis.dispatchEvent(new Event("carbon-mp"));
    }, (uint32_t)(uintptr_t)sh);
#endif
}
