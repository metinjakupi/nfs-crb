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
 * where it should be at that moment. Near our car it is driven there through its
 * speed instead, and after touching our car it is left to the engine for a while
 * (CONTACTS and NEAR OUR CAR below). The
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
 * With setting mp_ghost_cars 1 (drive through) they are built solid as above but
 * keep RBRemote's own CanCollideWith, so no car touches them, and the touches
 * other games report are not applied (mp_drive_through).
 * With the page's menu asking for it (flags bit 0; no setting since 2026-11-04),
 * they are class 8 instead: RBGhost (a plain
 * RigidBody; only its destructor differs) and AIVehicleGhost, drawn see-through.
 * Either way the physics still runs, so between two frames a car can be pushed
 * (a test car put inside a wall turned sideways); on open road the push is
 * 2-4 cm. The cache is GRaceStatus
 * (*0x00a98284; its IVehicleCache is at +0), as for racers. Addresses and
 * layouts were found with berkayylmao's NFSPluginSDK (BSD-3) and checked in the
 * listing.
 *
 * NEAR OUR CAR. A solid friend's car within MP_SPRING_NEAR of ours (until it is
 * MP_SPRING_FAR away again, so it does not flip every frame) is never put on its
 * poses: it is driven onto them through its speed (MP_NEARBY), as NFSCO moves every
 * copy (logs/nfsco-2888/collisions.md): each frame its speed and spin are its
 * owner's, plus a pull onto its place of at most MP_NEAR_CATCHUP. Put there, a pose
 * inside or against our car (where our car was in its owner's game some 50-150 ms
 * before, or where ours has been pushed since) overlapped the two bodies and the
 * physics pinned, hopped or launched them. A touch still sets it free and it is
 * pulled back as above, then driven again; out of the band it is put on its poses
 * once within MP_NEAR_LEAVE of them. Only a snap (mp_follow: past MP_SNAP with its
 * place clear of ours, or past MP_SNAP_FAR) still puts it anywhere near us. Cop
 * copies (cops.h), race cars (race.h) and see-through cars keep their own rules.
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
 *
 * MORE FRIENDS (Step A, 2026-11-05). MP_SLOTS slots (20; the relay sends the 24 nearest players).
 * The first MP_SLOTS_V2 (8) stay where the version-2 block has them, so pages and scripts that know
 * only those (chat.js, radar.js, streaming.js) still read them right; the rest follow the block's old
 * end (MpMore), with a magic word that tells a new page they are there. The game holds 30 cars (list 0,
 * count at 0x00a9f164) and its car memory (006d41d0 against [0x00b77084]); its gate 006d86f0 does not
 * refuse a car over either: it frees another one through the vehicle cache, which silently removed
 * friends' cars (logs/crowd-test-20261105). So every car mp_build makes is first checked against both,
 * with MP_ROOM_CARS places and MP_ROOM_KB of memory left over for the game's own cars (mp_room_fits); a car
 * that does not fit is not asked for. Which friends get a car is decided here, nearest first: a friend
 * without one shows only a name tag at its poses. A friend's car that leaves the slots (not among the
 * nearest any more, beyond MP_FAR, or not heard from) is KEPT, parked (SetPhysicsMode 0: not simulated,
 * not drawn), by its owner's relay id, model and look, and taken again when that friend comes back in
 * the same model and look (KEPT CARS below). A friend's car is never built in another model: one whose
 * model is not loaded here is asked for again, spooled, every MP_RETRY_MS, with only the name tag
 * meanwhile (MODELS below).
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
#define MP_SLOTS 64
#define MP_SLOTS_V2 8  /* slots where the version-2 block has them; the rest are in MpMore */
#define MP_SLOTS_V3 20 /* slots 8..19 in MpMore (Step A); 20..63 in MpMore2 (Step B, carlimit.h) */
#define MP_GONE 16     /* MpMore.gone: players who left the room, as the page last wrote them */
#define MP_NEAR 300.0f /* metres: a remote player's car is built within this */
#define MP_FAR 500.0f  /* and removed beyond this */
#define MP_SIDE 4.0f   /* test car: metres beside the player where it first appears */
#define MP_AHEAD 7.0f  /* escort test: metres in front of the player */
#define MP_TAKE 900    /* test car: frames recorded, then replayed */
#define MP_RING 4      /* poses per slot */
#define MP_KEEP 48     /* poses kept per car: enough history to smooth a recovered link */
#define MP_DELAY 150.0 /* ms behind the sender */
#define MP_EXTRAPOLATE 300.0
#define MP_RECOVER_GAP 100.0  /* ms behind the target before catch-up becomes faster */
#define MP_RECOVER_GAIN 100.0 /* ms: catch-up gain after a delivery interruption */
#define MP_RECOVER_RATE 4.0   /* fastest playback while catching up */
#define MP_RECOVER_SPEED 60.0f /* m/s added to the owner's speed while closing position */
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
#define MP_SPRING_NEAR 20.0f /* metres: a solid friend's car this near ours is driven, never put (NEAR OUR CAR) */
#define MP_SPRING_FAR 25.0f  /* until it is this far away again */
#define MP_NEAR_GAIN 4.0f    /* 1/s: such a car's speed is its owner's plus this times the distance to its place, */
#define MP_NEAR_CATCHUP 8.0f /* m/s: at most this */
#define MP_NEAR_LEAVE 0.25f  /* metres: out of the band, it is put on its poses again from this close */
#define MP_IMPACT_HOLD 250.0 /* ms a received impact waits for this game's own contact */
#define MP_IMPACT_SEEN 600.0 /* ms: an own contact this recent already gave this car its push */
enum { MP_FOLLOWING, MP_FREE, MP_RETURNING, MP_NEARBY }; /* MpCar.follow; MP_NEARBY: driven near our car */
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

typedef struct MpMore { /* after the version-2 block (MORE FRIENDS in the header comment) */
    uint32_t magic;          /* MP_MORE_MAGIC: the page may use all `count` slots */
    uint32_t count;          /* MP_SLOTS: slots in all, the first MP_SLOTS_V2 of them in MpShared.slots */
    uint32_t gone_head;      /* the page: relay ids written to gone[] so far */
    uint32_t spare;
    uint32_t gone[MP_GONE];  /* the page: players who left the room, gone[(gone_head - 1) % MP_GONE] the newest */
    MpSlot slots[MP_SLOTS_V3 - MP_SLOTS_V2];
    MpLabel labels[MP_SLOTS_V3 - MP_SLOTS_V2];
} MpMore;

typedef struct MpMore2 { /* after MpMore (+0x2600): slots 20 .. MP_SLOTS - 1; MpMore.count says how many in all */
    MpSlot slots[MP_SLOTS - MP_SLOTS_V3];
    MpLabel labels[MP_SLOTS - MP_SLOTS_V3];
} MpMore2;

typedef struct MpShared {
    uint32_t magic, version; /* MP_MAGIC, 2; the version-1 prefix stays intact */
    uint32_t page;           /* the page: nonzero while the relay link is up */
    uint32_t seq;            /* the game's part from here to pose */
    uint32_t state, key, frame, ms; /* state 1 = driving a car; ms: when the pose was read */
    MpPose pose;
    uint32_t flags;          /* the page's choices and capabilities: MP_PAGE_* */
    uint32_t keys;           /* the game's: letters bound on the keyboard (KEYS above); 0 = not known */
    uint32_t me;             /* the page: our relay connection's id, 0 offline */
    MpSlot slots[MP_SLOTS_V2]; /* slot i: mp_slot(sh, i) */
    MpLabel labels[MP_SLOTS_V2]; /* mp_label(sh, i) */
    uint32_t tx_head, tx_tail, rx_head, rx_tail;
    MpImpact tx[MP_IMPACTS], rx[MP_IMPACTS]; /* single-producer/single-consumer queues */
    MpMore more;             /* +0x18f0, where the version-2 block ended */
    MpMore2 more2;           /* +0x2600, where Step A's block ended */
} MpShared;
#define MP_MAGIC 0x31504d43u /* "CMP1" */
#define MP_MORE_MAGIC 0x2b504d43u /* "CMP+" */
#define MP_PAGE_GHOST 1u      /* flags: show other players' cars see-through */
#define MP_PAGE_CONTACTS 2u   /* relay and page support shared collision responses */
#define MP_KEYS_KNOWN 0x80000000u /* keys: the mod filled it in */
/* The page reads these by offset (web/net.js, chat.js). */
_Static_assert(offsetof(MpShared, flags) == 0x54 && offsetof(MpShared, keys) == 0x58 &&
                   offsetof(MpShared, slots) == 0x60 && offsetof(MpShared, tx_head) == 0x8e0 &&
                   sizeof(MpImpact) == 0x40 && offsetof(MpShared, more) == 0x18f0 &&
                   offsetof(MpShared, more.gone) == 0x1900 && offsetof(MpShared, more.slots) == 0x1940 &&
                   offsetof(MpShared, more.labels) == 0x2540 && offsetof(MpShared, more2) == 0x2600 &&
                   offsetof(MpShared, more2.labels) == 0x2600 + 0x100 * (MP_SLOTS - MP_SLOTS_V3) &&
                   sizeof(MpShared) == 0x2600 + 0x110 * (MP_SLOTS - MP_SLOTS_V3),
               "MpShared layout");

/* Slot i (0 .. MP_SLOTS - 1) and its label, wherever the layout keeps them. */
static MpSlot *mp_slot(MpShared *sh, int i) {
    return i < MP_SLOTS_V2 ? &sh->slots[i] : i < MP_SLOTS_V3 ? &sh->more.slots[i - MP_SLOTS_V2] : &sh->more2.slots[i - MP_SLOTS_V3];
}
static MpLabel *mp_label(MpShared *sh, int i) {
    return i < MP_SLOTS_V2 ? &sh->labels[i] : i < MP_SLOTS_V3 ? &sh->more.labels[i - MP_SLOTS_V2] : &sh->more2.labels[i - MP_SLOTS_V3];
}

typedef struct MpCar {
    uint32_t simable, id, key; /* the car's ISimable (0 = none) and whose it is */
    uint32_t kind;             /* the driver class it was built with */
    int wait;                  /* frames before the next build attempt (cops.h, rivals.h) */
    int tries;
    double retry_at;           /* our ms: a friend's car is not asked for again before this (MODELS) */
    double built_at;           /* our ms: when its car was built or taken back */
    uint32_t look_hash;        /* the look its car was built in (mp_look_hash; 0 = stock) */
    int tag_only;              /* a friend with no car here: only the name tag, at its poses */
    int visual_only; /* encounter copies: owner poses, no player inputs or contacts */
    int paused;      /* connected but in a menu: keep the car, stationary and non-colliding */
    int spawn_overlap; /* no contact with our car until a shared spawn has cleared */
    uint32_t taken;            /* slot poses already kept */
    double play, played_at;    /* the playback clock (sender ms) and when it last moved (our ms) */
    int recovering;            /* a delivery gap is still being closed without a position jump */
    int kept;
    double at[MP_KEEP];        /* each kept pose's sender time, ms */
    MpPose pose[MP_KEEP];      /* oldest first */
    float drawn[3];            /* where it was last placed */
    uint32_t contacts, ack, applied, sent;
    int pending;               /* impact (below) being gathered for its owner */
    int follow;                /* MP_FOLLOWING its poses, MP_FREE after a touch, MP_RETURNING, MP_NEARBY */
    int spring_near;           /* within MP_SPRING_NEAR of our car (until MP_SPRING_FAR): never put */
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
static int g_mp_local_paused; /* Escape pause: keep the received world at its last drawn frame */

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
    return g_mp_page_ghost ? MP_GHOST : MP_REMOTE; /* see-through: only the page's own flag now */
}

/* Setting mp_ghost_cars 1, drive through (the user's wish, 2026-11-04; it replaced the see-through
 * ghost choice, so a player who had ghost mode on now drives through): other players' cars look
 * as solid as ever, but this player drives through them. They are built as solid cars without
 * mp_enable_contacts, so RBRemote's own CanCollideWith (006ecca0) keeps every car off them (it
 * asks for the original online session, which our relay never creates), and the pushes other
 * games send are not applied (mp_contacts). Walls and the road still hold them. */
static int mp_drive_through(void) {
    return setting("mp_ghost_cars", 0) != 0 && !g_mp_page_ghost;
}
/* No touching other players' cars on this screen: see-through or drive through. */
static int mp_no_contacts(void) {
    return mp_kind() == MP_GHOST || mp_drive_through();
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

/* HUMANS. A class-6 car (AIVehicleHuman) is pushed, as the player's car is, onto an 8-entry list
 * at 0x00a9ffe8 (method table, begin +4, capacity +8, count +0xc; 0042b370 pushes past the capacity:
 * its grow, table +0xc, keeps the inline buffer). Seen 2026-11-05 (logs/step-a-20261105/runs/bots-1):
 * the player and 9 friends' class-6 cars made the count 10, entries 9 and 10 overwrote the list after
 * it (0x00aa0018: its method table and its buffer pointer) and the game ended 30 ms later
 * (ExitProcess 0xC0000005 in 00763ca0's walk of that list). With 8 slots, 8 class-6 friends would
 * already have overwritten its method table. A kept (parked) class-6 car keeps its entry. So a
 * friend's car is class 6 only while the list has room; otherwise it is class 7 (AIVehicleEmpty: this
 * game's police do not chase it), and a class-7 car is good enough for a friend who would be class 6
 * (mp_kind_fits), so it is not rebuilt when room comes back. */
#define MP_HUMANS 0x00a9ffe8u
static uint32_t g_mp_humans_max = 8u; /* carlimit.h raises it once it has moved this list */
#define MP_HUMANS_MAX g_mp_humans_max
static uint32_t mp_humans(void) {
    return get_u32(MP_HUMANS + 0xc);
}
static int mp_humans_room(void) {
    const uint32_t cap = get_u32(MP_HUMANS + 8);
    return mp_humans() + 1u <= (cap && cap < MP_HUMANS_MAX ? cap : MP_HUMANS_MAX);
}
static int mp_kind_fits(uint32_t have, uint32_t want) {
    return have == want || (have == MP_REMOTE && want == MP_REMOTE_HUMAN);
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

/* A solid car (class 6 or 7) given or freed of the collision rule when drive through is switched
 * while it exists: its own table back (RBRemote's, no touching) or the private copy again. */
static void mp_sync_contacts(uint32_t simable) {
    const uint32_t body = get_u32(simable - 0x24 + 0x70);
    if (body < 0x40u || get_u32(body - 0x14u) != simable)
        return;
    const uint32_t table = get_u32(body - 0x40u), through = mp_drive_through();
    if (through && g_mp_solid_vtable && table == g_mp_solid_vtable) {
        mp_put(body - 0x40u, MP_REMOTE_VTABLE);
        log_line("core.nfsc: mp solid car: drive through (no collisions)");
    } else if (!through && table == MP_REMOTE_VTABLE) {
        mp_enable_contacts(simable);
    }
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
    for (int n = 0; node && node != MP_CARS && n < 256; ++n, node = get_u32(node)) {
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
    for (int n = 0; node && node != MP_CARS && n < 256; ++n, node = get_u32(node))
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
/* How long one car build held up the game thread, in real time (a build runs on it, and includes
 * any wait for other guest threads). Built cars, police models and refused cars are counted apart, so one kind cannot use up the
 * others' log lines. A refusal is the model
 * not being loaded and says nothing about what a build costs. Logged for the first MP_BUILD_LOG of
 * each kind, at each new worst, and every MP_BUILD_EVERY after, so reports carry it without flooding. */
#define MP_BUILD_LOG 24u
#define MP_BUILD_EVERY 50u
static int mp_cop_model(uint32_t key);

static void mp_build_time(uint32_t kind, uint32_t key, uint32_t flags, const char *look, double ms, int built) {
    static struct { uint32_t count; double total, worst; } t[3]; /* refused, built, built police models */
    const int i = !built ? 0 : mp_cop_model(key) ? 2 : 1;
    const int worst = ms > t[i].worst;
    t[i].total += ms;
    ++t[i].count;
    if (worst)
        t[i].worst = ms;
    if (t[i].count > MP_BUILD_LOG && !worst && t[i].count % MP_BUILD_EVERY != 0)
        return;
    char line[160];
    snprintf(line, sizeof line, "core.nfsc: mp build took %.1f ms (%s, model %08x, class %u%s%s): %u so far, average %.1f, worst %.1f",
             ms, i == 2 ? "built police" : i ? "built" : "refused", key, kind, flags & 1u ? ", spooled" : "", look, t[i].count,
             t[i].total / t[i].count, t[i].worst);
    log_line(line);
}

/* ROOM (MORE FRIENDS in the header comment). 006d86f0 adds the new cars' estimates to the sum
 * 006d41d0 returns (each car's [car + 0x1a8], in bytes; a car whose model's memory class is traffic
 * or police, [0x00b74ccc] + model index * 0xd0 + 0x94 = 1 or 2, counted once per model) and their
 * number to list 0's count, and when either is over [0x00b77084] (007b11e0) or 30, frees cars through
 * the params' cache instead of refusing (read in the listing, 006d87ce..006d880c; seen in the crowd
 * test). mp_room_fits asks the same before a build, with room left for the game's own cars; a car
 * killed for room stays in list 0 until the game destroys it, so no second one is killed for room
 * while the first is still listed (g_mp_dying). */
#define MP_LIST0_COUNT 0x00a9f164u  /* list 0 of the 12 vehicle lists at 0x00a9f158: every car */
static uint32_t g_mp_list0_max = 30u; /* 006d87f8 cmp eax, 0x1e; carlimit.h raises it once its lists are moved */
#define MP_LIST0_MAX g_mp_list0_max
#define MP_MEMORY_SUM 0x006d41d0u   /* cdecl (): the cars' memory estimates, bytes */
#define MP_MEMORY_BUDGET 0x00b77084u /* bytes */
#define MP_CAR_KB 6397u   /* a player's or racer's car, each (the class table at 0x00a77440; crowd test) */
#define MP_COP_KB 1023u   /* a police car (driver class 2 or 4), once per model */
#define MP_ROOM_CARS 3u   /* places of list 0 left free for the game's own traffic and police */
#define MP_ROOM_KB 1024u  /* and memory */
#define MP_DYING 8
static uint32_t g_mp_dying[MP_DYING]; /* cars killed for room, until the game has destroyed them */
static double g_mp_dying_at[MP_DYING];
static int mp_listed(uint32_t car);

static uint32_t mp_list0(void) {
    return get_u32(MP_LIST0_COUNT);
}

/* carlimit.h (Step B): the gate's budget is the pool times g_mp_budget_x (the estimates are about three times what a
 * car takes from the pool), and g_mp_real_room says whether the pool itself has room for one more car. */
static uint32_t g_mp_budget_x = 1u;
static int (*g_mp_real_room)(void);

/* The car memory in use and the budget, KB. */
static uint32_t mp_memory_kb(uint32_t *budget) {
    if (budget)
        *budget = get_u32(MP_MEMORY_BUDGET) / 1024u * g_mp_budget_x;
    return mp_call(MP_MEMORY_SUM, 0, NULL, 0) / 1024u;
}

/* Whether a car killed for room is still in the car list (then list 0 has not dropped yet). */
static int mp_dying_pending(void) {
    int n = 0;
    const double now = mp_now();
    for (int i = 0; i < MP_DYING; ++i) {
        if (g_mp_dying[i] && (!mp_listed(g_mp_dying[i] - 0x24) || now - g_mp_dying_at[i] > 5000.0))
            g_mp_dying[i] = 0;
        n += g_mp_dying[i] != 0;
    }
    return n;
}

static void mp_dying_add(uint32_t simable) {
    int at = 0;
    for (int i = 0; i < MP_DYING; ++i)
        if (!g_mp_dying[i] || g_mp_dying_at[i] < g_mp_dying_at[at])
            at = i;
    g_mp_dying[at] = simable;
    g_mp_dying_at[at] = mp_now();
}

/* The estimate 006d86f0 adds for a new car of driver class kind, KB (cautious: a police model's car
 * already here would add nothing). */
/* LITE CARS (Step B, carlimit.h): a friend farther than g_mp_lite_from metres (0 = never) is built as
 * RacingLite (VehicleParams + 0x38 flag 0x20: the 1600 KB estimate and the _LITE textures); once it comes
 * within MP_LITE_NEAR of that it is built again as a full car. */
#define MP_LITE_FLAG 0x20u
#define MP_LITE_KB 1600u
#define MP_LITE_NEAR 0.5f
static float g_mp_lite_from;
static int g_mp_build_lite; /* the next build is lite (mp_estimate_kb, mp_build's caller) */
static uint32_t g_mp_lite_built, g_mp_lite_rebuilt;
static int mp_car_lite(uint32_t simable) {
    return simable && get_u32(simable - 0x24 + 0x1a8) == MP_LITE_KB * 1024u;
}

static uint32_t mp_estimate_kb(uint32_t kind) {
    return kind == 2u || kind == 4u ? MP_COP_KB : g_mp_build_lite ? MP_LITE_KB : MP_CAR_KB;
}

/* Whether one more car of class kind fits in list 0 and the car memory with room to spare; why gets
 * the figures. */
static int mp_room_fits(uint32_t kind, char *why, size_t n) {
    uint32_t budget = 0;
    const uint32_t list0 = mp_list0(), used = mp_memory_kb(&budget), est = mp_estimate_kb(kind);
    const int fits = list0 + 1u + MP_ROOM_CARS <= MP_LIST0_MAX && budget > MP_ROOM_KB &&
                     used + est <= budget - MP_ROOM_KB && (!g_mp_real_room || g_mp_real_room());
    if (why)
        snprintf(why, n, "list 0 %u/%u, memory %u + %u / %u KB", list0, MP_LIST0_MAX, used, est, budget);
    return fits;
}

static int mp_kept_free_oldest(const char *why); /* KEPT CARS below: 1 if a kept car was removed */

/* The kept cars the game counts and this file could remove: removes the oldest when the game's own
 * cars have used up the room left for them (every MP_ROOM_EVERY ms, or at once when asked). */
#define MP_ROOM_EVERY 250.0
static void mp_room_keep(int now_too) {
    static double at;
    const double now = mp_now();
    if (!now_too && now - at < MP_ROOM_EVERY)
        return;
    at = now;
    uint32_t budget = 0;
    const uint32_t list0 = mp_list0(), used = mp_memory_kb(&budget);
    if ((list0 + MP_ROOM_CARS > MP_LIST0_MAX || used + MP_ROOM_KB > budget) && !mp_dying_pending()) {
        char why[96];
        snprintf(why, sizeof why, "room for the game's own cars: list 0 %u/%u, memory %u/%u KB", list0, MP_LIST0_MAX,
                 used, budget);
        mp_kept_free_oldest(why);
    }
}

/* One line per car built, while counted: the room before and after, and whether the game freed
 * another car to make it (list 0 grew by less than one). */
static uint32_t g_mp_builds_logged, g_mp_evictions;
static void mp_room_note(uint32_t kind, uint32_t key, uint32_t list0, uint32_t used, uint32_t budget) {
    uint32_t budget2 = 0;
    const uint32_t list1 = mp_list0(), used1 = mp_memory_kb(&budget2);
    const int evicted = list1 != list0 + 1u;
    g_mp_evictions += evicted;
    if (!evicted && ++g_mp_builds_logged > 200u && g_mp_builds_logged % 50u != 0)
        return;
    char line[200];
    snprintf(line, sizeof line, "core.nfsc: mp room: built model %08x class %u: list 0 %u -> %u/%u, memory %u -> %u/%u KB (+%d KB)%s",
             key, kind, list0, list1, MP_LIST0_MAX, used, used1, budget2, (int)used1 - (int)used,
             evicted ? "; EVICTION: list 0 did not grow by one" : "");
    log_line(line);
}

static int g_mp_no_room; /* the last mp_build was not tried: no room (mp_room_fits) */
/* PLAYERS' CARS SHOWN (Step B, carlimit.h): at most this many friends' cars at once, nearest first (the
 * setting, or its Auto steps); mp_remotes' last counts for Auto. */
static uint32_t g_mp_cars_cap = 64u;
static int g_mp_built_now, g_mp_waiting_now;

static uint32_t mp_build(uint32_t player, uint32_t kind, uint32_t key, const float pos[3], uint32_t flags,
                         uint32_t look) {
    const uint32_t b = g_mp_buf, state = mp_state(get_u32(player + 0x70));
    g_mp_no_room = 0;
    if (!state)
        return 0;
    char why[96];
    if (kind == MP_REMOTE_HUMAN && !mp_humans_room()) { /* HUMANS: never a ninth entry */
        static uint32_t told;
        if (++told <= 8u || told % 100u == 0) {
            char line[128];
            snprintf(line, sizeof line, "core.nfsc: mp: no class-6 car of model %08x: the human list holds %u: %u so far", key, mp_humans(), told);
            log_line(line);
        }
        return 0;
    }
    if (!mp_room_fits(kind, why, sizeof why)) {
        static uint32_t told;
        g_mp_no_room = 1;
        if (++told <= 24u || told % 100u == 0) {
            char line[200];
            snprintf(line, sizeof line, "core.nfsc: mp room: no car of model %08x class %u now (%s): %u so far", key, kind, why, told);
            log_line(line);
        }
        if (!mp_dying_pending())
            mp_kept_free_oldest("room for a nearer car");
        return 0;
    }
    uint32_t budget = 0;
    const uint32_t list0 = mp_list0(), used = mp_memory_kb(&budget);
    const double build_from = mp_now();
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
    if (car && (kind == MP_REMOTE || kind == MP_REMOTE_HUMAN) && !mp_drive_through()) /* both are RBRemote (006d0d40) */
        mp_enable_contacts(car);
    mp_build_time(kind, key, flags, look ? ", own look" : "", mp_now() - build_from, car != 0);
    if (car)
        mp_room_note(kind, key, list0, used, budget);
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
    if ((ra && ra->paused) || (rb && rb->paused)) {
        api->hook_return(api, cpu, cpu->eax & 0xffffff00u, 0);
        return "paused";
    }
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
    if ((remote->follow == MP_FOLLOWING || remote->follow == MP_NEARBY) && remote->kept) {
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
    /* Not while this player drives through other players' cars (or they are see-through): a
     * push from a car we pass through would shove ours out of nowhere. */
    const int enabled = me && (flags & (MP_PAGE_GHOST | MP_PAGE_CONTACTS)) == MP_PAGE_CONTACTS && !mp_no_contacts();
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
    if (c->simable && mp_listed(c->simable - 0x24)) {
        mp_kill(c->simable);
        mp_dying_add(c->simable); /* ROOM: its place is not free until the game destroys it */
    }
    if (c->simable && why) {
        char line[96];
        snprintf(line, sizeof line, "core.nfsc: mp car of player %u removed (%s)", c->id, why);
        log_line(line);
    }
    c->simable = 0;
    c->pending = c->holding = 0;
    c->follow = MP_FOLLOWING;
}

/* KEPT CARS. A friend's car is parked, not removed, when the friend leaves the slots (not among the
 * page's nearest any more, or not heard from) or drives beyond MP_FAR (mp_park_friend): light bar
 * off, its controls cleared, then SetPhysicsMode 0 (not simulated, not drawn, not solid) where it
 * stands (a car put below our car came back up at our car, 2026-10-26, run c23: the police copy
 * under the friend's own car). It is kept with its owner's relay id, model, driver class and look
 * (mp_look_hash), and when that friend's slot asks for a car again in the same model, class and look,
 * the kept car is put at its pose and its physics turned on again (mp_kept_take): no build, no
 * spooling, no new memory. A kept car still counts toward list 0 and the car memory (vehicle-limit
 * findings §7: parking frees nothing), so the oldest kept one is removed (Kill) when a nearer car
 * needs its place (mp_build) or the game's own cars have used up their room (mp_room_keep). Kept
 * cars also go when their friend leaves the room (MpMore.gone), when this player is not driving,
 * and when another file removes the friends' cars (a race starting: mp_drop_friend). A friend whose
 * model or look changed gets a new car: the old one is removed then, not kept.
 * POLICE MODELS. Removing (Kill) a car built in a police model ended the game now and then
 * (plans/police-sync.md "Known problems"), and a friend who turns police, or back, changes model:
 * its car here was removed twice (for the new model, then again ~0.6 s later for its look). So a
 * friend's car of a police model is never removed for room or a change but kept as above, and may
 * also be taken by the next friend's car of that model and class that has no look of its own, as
 * cops.h keeps its cop copies. Only beyond MP_SPARES of them, or when see-through cars change the
 * class, is one removed. A police model is one named in mp_cop_model, by the key 00606b60 makes of
 * its name (mp_model_key gives the same). Friends' cars of racer models were removed 56 times in
 * stress tests without a crash (cops.h, TAKING THEM AWAY). */
#define MP_KEPT 16
#define MP_SPARES 4               /* kept cars of police models at most */
#define MP_PARK_DOWN 400.0f       /* metres below our car where a kept car with its physics on waits */
#define MP_SET_PHYSICS 0x006d6950u /* IVehicle +0x38 SetPhysicsMode, thiscall (IVehicle *, 0 inactive / 1 active) */
#define MP_STRING_KEY 0x00606b60u /* cdecl (const char *name): the attribute key of a name */
#define MP_LIGHT_BAR 0x7000u      /* PVehicle + 0x150 light bits: the police light bar */
#define MP_LOOK_BYTES 0x42cu      /* a VehicleCustomizations (look.h LOOK_BYTES) */
typedef struct MpKept {
    uint32_t simable, kind, key; /* the car, its driver class and model */
    uint32_t id, look;           /* whose it was (0: a police-model spare of nobody now) and its look */
    int cop;                     /* a police model: never removed for room */
    double at;                   /* our ms: when it was parked */
} MpKept;
static MpKept g_mp_kept[MP_KEPT];
static uint32_t g_mp_cop_keys[16];
static int g_mp_cop_keys_n = -1;
static uint32_t g_mp_gone_tail; /* MpMore.gone read so far */
static uint32_t g_mp_reused, g_mp_parked, g_mp_kept_removed; /* for the log */
static uint32_t g_mp_kept_kind; /* the driver class of the car mp_kept_take gave last */

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

/* A look as a number (FNV-1a of the record look.h gave mp_build, at guest address look; 0 = stock),
 * so a kept car is taken back only in the look its owner has now. */
static uint32_t mp_look_hash(uint32_t look) {
    const uint8_t *p = NULL;
    if (!look || g_api->guest_ptr(g_api, look, MP_LOOK_BYTES, (void **)&p) != POP_OK || !p)
        return 0;
    uint32_t h = 0x811c9dc5u;
    for (uint32_t i = 0; i < MP_LOOK_BYTES; ++i)
        h = (h ^ p[i]) * 0x01000193u;
    return h ? h : 1u;
}

/* Whether simable is a car in use here now (a kept one then is not ours any more). */
static int mp_kept_in_use(uint32_t simable) {
    for (int i = 0; i < MP_SLOTS; ++i)
        if (g_mp_remote[i].simable == simable)
            return 1;
    return g_mp_test.simable == simable || (g_mp_racer_car && g_mp_racer_car(simable)) ||
           (g_mp_cop_car && g_mp_cop_car(simable));
}

/* Whether kept car k is still ours: in the car list, its class, and not (its memory given to) a car
 * in use. */
static int mp_kept_ok(const MpKept *k) {
    return k->simable && mp_listed(k->simable - 0x24) && mp_kind_kept(k->simable - 0x24, k->kind) &&
           !mp_kept_in_use(k->simable);
}

/* Removes kept car k (Kill), or only forgets it when it is not ours any more. */
static void mp_kept_remove(MpKept *k, const char *why) {
    if (mp_kept_ok(k)) {
        mp_kill(k->simable);
        mp_dying_add(k->simable);
        ++g_mp_kept_removed;
        char line[160];
        snprintf(line, sizeof line, "core.nfsc: mp kept car of player %u removed (model %08x, kept %.1f s; %s)", k->id, k->key,
                 (mp_now() - k->at) / 1000.0, why);
        log_line(line);
    }
    memset(k, 0, sizeof *k);
}

static int mp_kept_free_oldest(const char *why) {
    MpKept *oldest = NULL;
    for (int i = 0; i < MP_KEPT; ++i) {
        MpKept *k = &g_mp_kept[i];
        if (k->simable && !k->cop && mp_kept_ok(k) && (!oldest || k->at < oldest->at))
            oldest = k;
    }
    if (!oldest)
        return 0;
    mp_kept_remove(oldest, why);
    return 1;
}

/* Every kept car that is not of a police model goes. */
static void mp_kept_flush(const char *why) {
    for (int i = 0; i < MP_KEPT; ++i)
        if (g_mp_kept[i].simable && !g_mp_kept[i].cop)
            mp_kept_remove(&g_mp_kept[i], why);
}

/* A friend's car leaves the slots: kept for that friend (KEPT CARS), or removed (mp_drop: Kill) when
 * it cannot be (see-through, gone already, or a police model beyond MP_SPARES). */
static void mp_park_friend(MpCar *c, const char *why) {
    const uint32_t car = c->simable ? c->simable - 0x24 : 0;
    if (!car || c->kind == MP_GHOST || !mp_listed(car) || !mp_kind_kept(car, c->kind)) {
        mp_drop(c, why);
        return;
    }
    const uint32_t key = mp_model_key(car);
    const int cop = mp_cop_model(key);
    int cops = 0;
    MpKept *free_at = NULL;
    for (int i = 0; i < MP_KEPT; ++i) {
        MpKept *k = &g_mp_kept[i];
        if (k->simable && !mp_kept_ok(k))
            memset(k, 0, sizeof *k);
        if (k->simable && !k->cop && c->id && k->id == c->id)
            mp_kept_remove(k, "an older car of the same player"); /* never two of one friend */
        cops += k->simable && k->cop;
    }
    if (cop && cops >= MP_SPARES) {
        mp_drop(c, why);
        return;
    }
    for (int round = 0; round < 2 && !free_at; ++round) {
        for (int i = 0; i < MP_KEPT && !free_at; ++i)
            if (!g_mp_kept[i].simable)
                free_at = &g_mp_kept[i];
        if (!free_at && (round || !mp_kept_free_oldest("no place to keep another")))
            break;
    }
    if (!free_at) {
        mp_drop(c, why);
        return;
    }
    mp_put(car + 0x150, get_u32(car + 0x150) & ~MP_LIGHT_BAR);
    const uint32_t in = get_u32(car + 0xec); /* IInput::ClearInput: no gas or nitrous left on */
    if (in && get_u32(in))
        mp_call(get_u32(get_u32(in) + 4), in, NULL, 0);
    const uint32_t off = 0; /* parked the game's way at once (inactive: not drawn, not solid) */
    mp_call(MP_SET_PHYSICS, car + 0xb8, &off, 1);
    free_at->simable = c->simable;
    free_at->kind = c->kind;
    free_at->key = key;
    free_at->id = c->id;
    free_at->look = c->look_hash;
    free_at->cop = cop;
    free_at->at = mp_now();
    ++g_mp_parked;
    char line[192];
    snprintf(line, sizeof line, "core.nfsc: mp car of player %u kept (model %08x%s; %s): list 0 %u/%u, %u kept so far",
             c->id, key, cop ? ", police model" : "", why ? why : "gone", mp_list0(), MP_LIST0_MAX, g_mp_parked);
    log_line(line);
    c->simable = 0;
    c->pending = c->holding = 0;
    c->follow = MP_FOLLOWING;
}

/* A friend's car goes for good (its model, look or class changed, or this player stopped driving):
 * removed (Kill), or kept as a spare of nobody when it is of a police model (POLICE MODELS). */
static void mp_remove_friend(MpCar *c, const char *why) {
    const uint32_t car = c->simable ? c->simable - 0x24 : 0;
    if (car && c->kind != MP_GHOST && mp_listed(car) && mp_kind_kept(car, c->kind) && mp_cop_model(mp_model_key(car))) {
        const uint32_t id = c->id;
        c->id = 0;
        mp_park_friend(c, why);
        c->id = id;
        return;
    }
    mp_drop(c, why);
}

/* A friend's car goes, and every kept one with it: for other files (a race starting). */
static void mp_drop_friend(MpCar *c, const char *why) {
    mp_remove_friend(c, why);
    mp_kept_flush(why ? why : "friends' cars removed");
}

/* The kept car of player id in model key, class kind and look (0 = stock), or (has_look 0) a kept
 * police-model car of nobody; 0 if none. A kept car of that player in another model or look is
 * removed: that friend has a new one now. */
static uint32_t mp_kept_take(uint32_t id, uint32_t key, uint32_t kind, uint32_t look, int has_look) {
    MpKept *got = NULL;
    for (int i = 0; i < MP_KEPT; ++i) {
        MpKept *k = &g_mp_kept[i];
        if (!k->simable)
            continue;
        if (!mp_kept_ok(k)) {
            memset(k, 0, sizeof *k);
            continue;
        }
        if (k->id == id && id) {
            if (k->key == key && mp_kind_fits(k->kind, kind) && k->look == look)
                got = k;
            else if (!k->cop)
                mp_kept_remove(k, "its player has another car or look now");
            else
                k->id = 0;
        }
    }
    for (int i = 0; !got && !has_look && i < MP_KEPT; ++i) {
        MpKept *k = &g_mp_kept[i];
        if (k->simable && k->cop && !k->id && k->key == key && k->kind == kind)
            got = k;
    }
    if (!got)
        return 0;
    const uint32_t simable = got->simable;
    g_mp_kept_kind = got->kind;
    char line[160];
    snprintf(line, sizeof line, "core.nfsc: mp car of player %u reused, not rebuilt (model %08x, kept %.1f s%s): %u so far", id,
             key, (mp_now() - got->at) / 1000.0, got->id ? "" : ", a police-model spare", ++g_mp_reused);
    log_line(line);
    memset(got, 0, sizeof *got);
    return simable;
}

/* The kept cars, every frame: forgotten when gone with the world; removed when their friend left the
 * room (the page's MpMore.gone; all of them when more left than it holds), when this player is not
 * driving, or when see-through cars change the class; one with its physics on is kept out of sight
 * below our car (here). */
static void mp_kept_frame(MpShared *sh, const float here[3], int driving) {
    const uint32_t head = __atomic_load_n(&sh->more.gone_head, __ATOMIC_ACQUIRE);
    if (head - g_mp_gone_tail > MP_GONE) {
        mp_kept_flush("more players left than the page could say");
        g_mp_gone_tail = head;
    }
    for (; g_mp_gone_tail != head; ++g_mp_gone_tail) {
        const uint32_t id = sh->more.gone[g_mp_gone_tail % MP_GONE];
        for (int i = 0; id && i < MP_KEPT; ++i) {
            MpKept *k = &g_mp_kept[i];
            if (k->simable && k->id == id) {
                if (k->cop)
                    k->id = 0;
                else
                    mp_kept_remove(k, "its player left the room");
            }
        }
    }
    if (!driving)
        mp_kept_flush("this player is not driving");
    for (int i = 0; i < MP_KEPT; ++i) {
        MpKept *k = &g_mp_kept[i];
        if (!k->simable)
            continue;
        if (!mp_kept_ok(k)) {
            memset(k, 0, sizeof *k);
            continue;
        }
        if (mp_kind() == MP_GHOST) {
            mp_kill(k->simable);
            memset(k, 0, sizeof *k);
            continue;
        }
        if (get_u32(k->simable - 0x24 + 0x160) == 0)
            continue; /* parked inactive (mp_park_friend): it stays where it is, unseen */
        const uint32_t state = driving ? mp_state(get_u32(k->simable - 0x24 + 0x70)) : 0;
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
        mp_place(k->simable, &park);
    }
}

/* This game's own traffic: driver class 1 cars, and trucks' trailers (class 4 and the TRAILER type,
 * as cops.h's cop_trailer). */
#define MP_TRAILER_KEY 0x00a9e7bcu
static int mp_traffic_count(int *trailers) {
    int n = 0, t = 0;
    uint32_t node = get_u32(MP_CARS);
    for (int guard = 0; node && node != MP_CARS && guard < 256; ++guard, node = get_u32(node)) {
        const uint32_t car = node - 0xd0, kind = get_u32(car + 0x148);
        n += kind == 1u;
        t += kind == 4u && get_u32(car + 0x11c) == get_u32(MP_TRAILER_KEY);
    }
    if (trailers)
        *trailers = t;
    return n;
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
        mp_label(sh, i)->applied = g_mp_remote[i].applied;
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

/* Moves a car's playback clock to now. Ordinarily it changes speed by at most
 * 10% to keep its lag (MP_DELAY, or less near our car). After a delivery gap it
 * catches up at up to MP_RECOVER_RATE instead of jumping straight to the newest
 * pose; MP_KEEP preserves enough of the old timeline for that fast playback.
 * It never rewinds an unchanged packet. Only a new sender timeline (mp_take)
 * resets playback. */
static double mp_playback(MpCar *c, double now) {
    const double target = c->at[c->kept - 1] - (c->lag > 0 ? c->lag : MP_DELAY);
    if (!c->played_at) {
        c->play = target;
        c->recovering = 0;
    } else {
        const double behind = target - c->play;
        const int recovering = behind > MP_RECOVER_GAP;
        if (recovering)
            c->recovering = 1;
        const double gain = recovering ? 1.0 / MP_RECOVER_GAIN : 0.001;
        const double fastest = recovering ? MP_RECOVER_RATE : 1.1;
        double rate = 1.0 + behind * gain;
        rate = rate < 0.9 ? 0.9 : rate > fastest ? fastest : rate;
        c->play += (now - c->played_at) * rate;
        if (recovering && c->play > target)
            c->play = target;
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
 * it follows again. Near our car (MP_NEARBY, mp_place_all) it is driven instead: its
 * owner's speed plus a pull onto its place, never put back from the last few cm. Only a
 * car more than MP_SNAP off is put back at once, and never onto our car (a snap 8 m
 * back landed inside it after a long push, 2026-10-25). */
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

/* A car put back at once: the jump a player sees. First ones, then every 100th. */
static void mp_snap_note(const MpCar *c, float metres) {
    static uint32_t snaps;
    if (++snaps > 24u && snaps % 100u != 0)
        return;
    char line[112];
    snprintf(line, sizeof line, "core.nfsc: mp snap of %.1f m (model %08x, player %u): %u so far", metres, c->key, c->id, snaps);
    log_line(line);
}

/* How closely a driven car near ours (MP_NEARBY) keeps to its poses, over every MP_NEAR_NOTE frames of
 * them: how far off it was before each pull, and of the frames its owner went faster than 100 km/h,
 * the same and how far its speed was from its owner's. */
#define MP_NEAR_NOTE 600
static void mp_near_note(float off, const float want[3], const float has[3]) {
    static int n, fast;
    static float sum, most, fast_sum, fast_most, fast_dv;
    float v2 = 0, dv2 = 0;
    for (int k = 0; k < 3; ++k) {
        v2 += want[k] * want[k];
        dv2 += (want[k] - has[k]) * (want[k] - has[k]);
    }
    sum += off;
    most = off > most ? off : most;
    if (v2 > (100.0f / 3.6f) * (100.0f / 3.6f)) {
        ++fast;
        fast_sum += off;
        fast_most = off > fast_most ? off : fast_most;
        fast_dv += sqrtf(dv2);
    }
    if (++n < MP_NEAR_NOTE)
        return;
    char line[224];
    snprintf(line, sizeof line, "core.nfsc: mp near spring: off its poses by %.3f m a frame on average, %.3f m at most; "
             "%d frames above 100 km/h: %.3f m on average, %.3f m at most, speed off by %.2f m/s on average",
             sum / n, most, fast, fast ? fast_sum / fast : 0.0f, fast_most, fast ? fast_dv / fast : 0.0f);
    log_line(line);
    n = fast = 0;
    sum = most = fast_sum = fast_most = fast_dv = 0;
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
    const double previous_played_at = c->played_at;
    mp_sample(c, mp_playback(c, now), &target);
    if (!c->visual_only && !c->paused && g_mp_drive_car)
        g_mp_drive_car(c, c->play, late); /* its owner's controls of that moment */
    if (!c->visual_only && !c->paused && g_mp_follow_target)
        g_mp_follow_target(c, &target);
    /* A fast owner can cover many metres during a gap. Even fast playback must not
     * turn that distance into one rendered jump. Close the position at the owner's
     * current speed plus MP_RECOVER_SPEED; collision recovery has its own spring. */
    if (c->recovering && c->follow == MP_FOLLOWING && previous_played_at > 0) {
        double dt = (now - previous_played_at) / 1000.0;
        dt = dt < 0 ? 0 : dt > 0.05 ? 0.05 : dt;
        float distance2 = 0, speed2 = 0;
        for (int k = 0; k < 3; ++k) {
            const float d = target.p[k] - c->drawn[k];
            distance2 += d * d;
            speed2 += target.v[k] * target.v[k];
        }
        const float distance = sqrtf(distance2);
        const float step = (sqrtf(speed2) + MP_RECOVER_SPEED) * (float)dt;
        if (distance > step && step > 0) {
            const float scale = step / distance;
            for (int k = 0; k < 3; ++k)
                target.p[k] = c->drawn[k] + (target.p[k] - c->drawn[k]) * scale;
        } else if (distance <= step) {
            c->recovering = 0;
        }
    } else if (c->follow != MP_FOLLOWING) {
        c->recovering = 0;
    }
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
        if (state)
            mp_snap_note(c, sqrtf(error));
        c->follow = c->spring_near ? MP_NEARBY : MP_FOLLOWING;
        c->pull_at = now;
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
    float ramp = c->follow == MP_NEARBY ? 1.0f : (float)((now - c->return_at) / MP_RETURN_MS);
    ramp = ramp > 1.0f ? 1.0f : ramp;
    const float rate = MP_RETURN_RATE * (0.3f + 0.7f * ramp);
    if (c->follow == MP_NEARBY)
        mp_near_note(sqrtf(error), target.v, car.v);
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
    /* Near our car (MP_NEARBY) its speed is its owner's, plus a pull onto its place of MP_NEAR_GAIN
     * per second of the distance, at most MP_NEAR_CATCHUP, as NFSCO drives its copies. The spring
     * alone, whose MP_RETURN_ACCEL cap holds the speed as well, left it metres behind at 100+ km/h
     * and after any sudden change of its owner's speed (0.75 s to reach 80 km/h), so the car behind
     * ran into it or it was snapped forward (logs/near-spring-20261105). A touch still frees it. */
    float pull[3] = {0, 0, 0}, pull2 = 0;
    for (int k = 0; k < 3 && c->follow == MP_NEARBY; ++k) {
        pull[k] = MP_NEAR_GAIN * (target.p[k] - car.p[k]);
        pull2 += pull[k] * pull[k];
    }
    for (int k = 0; k < 3 && pull2 > MP_NEAR_CATCHUP * MP_NEAR_CATCHUP; ++k)
        pull[k] *= MP_NEAR_CATCHUP / sqrtf(pull2);
    if (c->follow == MP_RETURNING && c->spring_near && now - c->return_at >= MP_RETURN_MS) {
        c->follow = MP_NEARBY; /* pulled back at full strength near our car: driven from here (NEAR OUR CAR) */
        c->pull_at = now;
    } else if ((c->follow == MP_RETURNING && !c->spring_near && error < 0.03f * 0.03f && dv2 < 0.3f * 0.3f &&
                fabsf(dot) > 0.99985f) ||
               (c->follow == MP_NEARBY && !c->spring_near && error < MP_NEAR_LEAVE * MP_NEAR_LEAVE)) {
        c->follow = MP_FOLLOWING; /* back on its poses away from our car: the last few cm are placed */
        mp_place(c->simable, &target);
        *out = target;
        return;
    }
    for (int k = 0; k < 3; ++k) {
        car.v[k] = c->follow == MP_NEARBY ? target.v[k] + pull[k] : car.v[k] + a[k] * scale * (float)dt;
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
    /* Carbon stops advancing the world's render connections on Escape pause. Moving only our
     * rigid bodies then makes the web name tags follow new poses while the visible cars stay at
     * their old matrices. Freeze both here; queued poses catch up smoothly after resume. */
    if (g_mp_local_paused)
        return;
    const double now = mp_now();
    const uint32_t ours = g_mp_player_simable ? mp_state(get_u32(g_mp_player_simable - 0x24 + 0x70)) : 0;
    float here[3];
    for (int k = 0; ours && k < 3; ++k)
        here[k] = bits_float(get_u32(ours + 0x10 + 4u * k));
    for (int i = 0; i < MP_SLOTS; ++i) {
        MpCar *c = &g_mp_remote[i];
        if (!c->simable && c->tag_only && c->kept) { /* no car here: its name tag goes where its poses say */
            MpPose tag;
            mp_sample(c, mp_playback(c, now), &tag);
            memcpy(c->drawn, tag.p, sizeof c->drawn);
            continue;
        }
        if (!c->simable || !c->kept)
            continue;
        /* Near our car a solid car is driven onto its poses, never put there (NEAR OUR CAR): the
         * nearer of where it was drawn and its newest pose, in by MP_SPRING_NEAR, out by MP_SPRING_FAR.
         * Not a see-through or paused car, or one still clearing a shared spawn: none of those touches ours. */
        float d2 = 1e30f;
        for (int j = 0; ours && j < 2; ++j) {
            const float *p = j ? c->pose[c->kept - 1].p : c->drawn, dx = p[0] - here[0], dy = p[1] - here[1],
                        dz = p[2] - here[2];
            d2 = fminf(d2, dx * dx + dy * dy + dz * dz);
        }
        const float edge = c->spring_near ? MP_SPRING_FAR : MP_SPRING_NEAR;
        c->spring_near = c->kind != MP_GHOST && !c->paused && !c->visual_only && !c->spawn_overlap && d2 < edge * edge;
        if (c->spring_near && c->follow == MP_FOLLOWING) {
            c->follow = MP_NEARBY; /* out of the band it is put again once within MP_NEAR_LEAVE (mp_follow) */
            c->pull_at = now;
        }
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
        MpLabel *l = mp_label(sh, i);
        if (g_mp_racer && g_mp_racer(g_mp_seen_slot[i].id))
            continue; /* race.h labels its racers' cars */
        float s[2];
        const float above[3] = {c->drawn[0], c->drawn[1] + MP_LABEL_UP, c->drawn[2]};
        const float depth = (c->simable || c->tag_only) && c->kept ? mp_project(g_mp_view + 0x80, above, 1, s) : 0.0f;
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
/* Space expensive constructor attempts, including failed spool requests. Existing
 * cars still receive every service/physics update; native race.h owns its racers. */
static double g_mp_build_after;
#define MP_BUILD_GAP_MS 100.0

/* MODELS. A friend's car is built only in its owner's model. Until 2026-11-05 the third refused try
 * built it in this player's own model (always loaded here) and tried the real one again later, and a
 * car the gate 006d86f0 freed (ROOM) was built again that way: friends saw another player's car, the
 * "RX-8 instead of his Lotus" report. Now a refused model is asked for again every MP_RETRY_MS, every
 * other try spooled (VehicleParams flags 1), and until it is here the friend has only its name tag
 * (tag_only). There is no fall-back to another model at all: a wrong car says the wrong thing about
 * who is there, it costs the same place and memory as the right one, and a model this game does not
 * have would stay wrong for good. A refusal costs under 0.1 ms (measured 2026-11-04), so trying
 * again every 2 s costs nothing to speak of. */
#define MP_RETRY_MS 2000.0
#define MP_ROOM_RETRY_MS 500.0 /* no room now: asked again this soon (a kept car may be on its way out) */
#define MP_SWAP 0.7f           /* a friend without a car this much nearer than the farthest with one */
#define MP_SWAP_HOLD_MS 3000.0 /* takes its car's place, once that car has been here this long */
#define MP_NOTE_MS 5000.0      /* the room and the friends' cars, in the log this often */

/* PAINT (logs/paint-20261105/findings.md): a car of a model with a composite skin takes one of 30
 * skin ids (CarRenderConn + 0x68, list 0x00b77db0), and its RideInfo ([conn + 0x3c]) names the
 * texture DUMMY_SKIN<id> at +0x48, but the game data has only DUMMY_SKIN1-8. Which ids have their
 * texture (GetTextureInfo 0055cfd0, cdecl (hash, return_default 0, include_unloaded 1)), as a line
 * whenever that changes, so a run shows how many friends' cars can wear their own paint. */
#define MP_RENDER_LIST 0x00b77db0u
#define MP_TEXTURE_INFO 0x0055cfd0u
static void mp_paint_line(void) {
    static char said[160];
    char with[80] = "", without[80] = "";
    const uint32_t list = get_u32(MP_RENDER_LIST + 4), count = get_u32(MP_RENDER_LIST + 0xc);
    for (uint32_t i = 0; list && i < count && i < 40; ++i) {
        const uint32_t conn = get_u32(list + 4u * i), id = conn ? get_u32(conn + 0x68) : 0;
        const uint32_t ride = id ? get_u32(conn + 0x3c) : 0, skin = ride ? get_u32(ride + 0x48) : 0;
        if (!skin)
            continue;
        const uint32_t args[3] = {skin, 0, 1};
        uint32_t info = 0;
        if (g_api->guest_call(g_api, MP_TEXTURE_INFO, 0, args, 3, &info) != POP_OK)
            info = 0;
        char *to = info ? with : without;
        const size_t at = strlen(to);
        if (at + 8 < sizeof with)
            snprintf(to + at, sizeof with - at, "%s%u", at ? "," : "", id);
    }
    char line[160];
    snprintf(line, sizeof line, "core.nfsc: mp paint: skin ids with their texture [%s], without [%s]", with, without);
    if (strcmp(line, said)) {
        memcpy(said, line, sizeof said);
        log_line(line);
    }
}

/* Every MP_NOTE_MS: what the friends' cars and the game's own use of the room are now. */
static void mp_room_line(double now, int slots, int built, int tags) {
    static double at;
    if (now - at < MP_NOTE_MS)
        return;
    at = now;
    uint32_t budget = 0;
    const uint32_t used = mp_memory_kb(&budget);
    int kept = 0, cops = 0, trailers = 0;
    for (int i = 0; i < MP_KEPT; ++i) {
        kept += g_mp_kept[i].simable != 0;
        cops += g_mp_kept[i].simable && g_mp_kept[i].cop;
    }
    const int traffic = mp_traffic_count(&trailers);
    char line[320];
    snprintf(line, sizeof line, "core.nfsc: mp cars: list 0 %u/%u, humans %u/%u, memory %u/%u KB; friends %d in slots, %d with a car, "
             "%d name tag only, %d kept (%d police); traffic %d + %d trailers; %u reused, %u kept, %u kept removed, %u evictions",
             mp_list0(), MP_LIST0_MAX, mp_humans(), MP_HUMANS_MAX, used, budget, slots, built, tags, kept, cops, traffic, trailers, g_mp_reused,
             g_mp_parked, g_mp_kept_removed, g_mp_evictions);
    log_line(line);
    mp_paint_line();
}

static void mp_remotes(MpShared *sh, uint32_t player) {
    const double now = mp_now();
    int build_slot = -1, far_slot = -1, slots = 0, built = 0, tags = 0, waiting = 0;
    float build_d2 = MP_NEAR * MP_NEAR, far_d2 = 0;
    float here[3] = {0, 0, 0};
    const uint32_t state = player ? mp_state(get_u32(player + 0x70)) : 0;
    g_mp_page_ghost = (__atomic_load_n(&sh->flags, __ATOMIC_RELAXED) & MP_PAGE_GHOST) != 0;
    for (int i = 0; i < 3 && state; ++i)
        here[i] = bits_float(get_u32(state + 0x10 + 4u * i));
    if (!state) g_mp_build_after = 0;
    for (int i = 0; i < MP_SLOTS; ++i) {
        MpSlot *src = mp_slot(sh, i), copy;
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
        if (c->simable && (c->kind == MP_REMOTE || c->kind == MP_REMOTE_HUMAN))
            mp_sync_contacts(c->simable); /* drive through switched while it exists */
        /* Mode 0 is a connected player in a menu. Keep its last car visible; only an empty
         * slot (`left` from the page) removes it. The page zeros its speed, and the collision
         * hook treats it as display-only until mode 1 returns. */
        const int active = sh->page && s->id && s->count && s->mode <= 1 && state &&
                           !(g_mp_racer && g_mp_racer(s->id));
        const int freeze = g_mp_local_paused && c->simable && active && c->id == s->id;
        const int relook = !freeze && c->simable && active && c->id == s->id && g_mp_relook &&
                           g_mp_relook(i, s->id, s->key);
        if (c->simable && (!active || c->id != s->id || (!freeze &&
            (c->key != s->key || !mp_kind_fits(c->kind, mp_friend_kind(s->id)) || relook)))) {
            if (g_mp_friend_car)
                g_mp_friend_car(i, c->id, 0);
            /* Out of the slots while this player drives: kept for when that friend comes back (KEPT
             * CARS). A new model, look or class, a race taking the friend, or this player not driving:
             * gone for good. */
            if (state && (!s->id || c->id != s->id) && !(g_mp_racer && g_mp_racer(c->id)))
                mp_park_friend(c, s->id ? "another player took its slot" : "out of the slots");
            else
                mp_remove_friend(c, active ? "another car or look" : "left or stopped driving");
        }
        if (relook)
            c->tries = 0; /* it was built, so its model is here: build it again in its own model */
        if (!active) {
            c->id = 0;
            c->tag_only = 0;
            continue;
        }
        if (!freeze && (c->id != s->id || c->key != s->key)) {
            c->id = s->id;
            c->key = s->key;
            c->wait = 0;
            c->tries = 0;
            c->retry_at = 0;
            c->tag_only = 0;
            c->taken = s->count + 1; /* start over in mp_take */
            c->applied = c->sent = c->ack = 0;
            c->pending = c->holding = 0;
            c->follow = MP_FOLLOWING;
            c->contact_at = 0;
        }
        c->paused = s->mode == 0;
        c->contacts = c->paused ? 0 : s->pad[0];
        c->ack = s->pad[1];
        mp_take(c, s, now);
        if (!c->kept)
            continue;
        ++slots;
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
        /* Keep the paused frame's existing cars. Do not remove one because its owner drove far,
         * rebuild it for a model/look change, or introduce a new car behind the frozen menu. */
        if (g_mp_local_paused) {
            built += c->simable != 0;
            tags += !c->simable && c->tag_only;
            continue;
        }
        if (c->simable) {
            if (d2 > MP_FAR * MP_FAR) {
                if (g_mp_friend_car)
                    g_mp_friend_car(i, c->id, 0);
                mp_park_friend(c, "far away");
            } else if (g_mp_lite_from > 0 && d2 < (g_mp_lite_from * MP_LITE_NEAR) * (g_mp_lite_from * MP_LITE_NEAR) &&
                       mp_car_lite(c->simable) && !mp_dying_pending()) {
                if (g_mp_friend_car)
                    g_mp_friend_car(i, c->id, 0);
                mp_remove_friend(c, "a lite car came near: built again in full");
                c->retry_at = now;
                ++g_mp_lite_rebuilt;
            } else {
                ++built;
                if (d2 > far_d2 && now - c->built_at >= MP_SWAP_HOLD_MS) {
                    far_slot = i;
                    far_d2 = d2;
                }
                continue;
            }
        }
        c->tag_only = d2 <= MP_FAR * MP_FAR; /* no car: the name tag alone (MODELS) */
        tags += c->tag_only;
        if (d2 > MP_NEAR * MP_NEAR)
            continue;
        ++waiting; /* near, no car */
        if (now < c->retry_at)
            continue;
        if (build_slot < 0 || d2 < build_d2) {
            build_slot = i;
            build_d2 = d2;
        }
    }
    mp_kept_frame(sh, here, state != 0);
    if (state)
        mp_room_keep(0);
    /* Select after reading every slot. Slot order must not make a farther car
     * consume the construction budget before a newly arrived nearby racer. */
    if (build_slot >= 0 && now >= g_mp_build_after) {
        const int i = build_slot;
        const MpSlot *s = &g_mp_seen_slot[i];
        MpCar *c = &g_mp_remote[i];
        const MpPose *last = &c->pose[c->kept - 1];
        const float d2 = build_d2;
        g_mp_build_after = now + MP_BUILD_GAP_MS;
        c->kind = mp_friend_kind(s->id);
        const uint32_t look = g_mp_look ? g_mp_look(i, s->id, s->key) : 0;
        const uint32_t hash = mp_look_hash(look);
        /* The friend's own kept car first (no build, no new place), then a new one if there is room. */
        /* Not over "Players' cars shown": a kept car taken back past it was parked again at once (cycle-64b). */
        uint32_t car = c->kind != MP_GHOST && built < (int)g_mp_cars_cap ? mp_kept_take(s->id, s->key, c->kind, hash, look != 0) : 0;
        if (car)
            c->kind = g_mp_kept_kind;
        else if (c->kind == MP_REMOTE_HUMAN && !mp_humans_room())
            c->kind = MP_REMOTE; /* HUMANS: the list is full */
        char why[96];
        if (car) {
            mp_place(car, last); /* at the friend's pose, then active again (IVehicle::Activate) */
            if (get_u32(car - 0x24 + 0xb8 + 0xa8) == 0) {
                const uint32_t on = 1;
                mp_call(MP_SET_PHYSICS, car - 0x24 + 0xb8, &on, 1);
            }
            c->tries = 0;
        } else if ((g_mp_build_lite = g_mp_lite_from > 0 && d2 > g_mp_lite_from * g_mp_lite_from && c->kind != MP_GHOST),
                   built >= (int)g_mp_cars_cap ? (snprintf(why, sizeof why, "players' cars shown: %u", g_mp_cars_cap), 1)
                                               : !mp_room_fits(c->kind, why, sizeof why)) {
            /* Room first: the oldest kept car goes; with none, the farthest friend's car goes when
             * this one is clearly nearer and its model has not been refused twice (a model that never
             * comes would otherwise take every car in turn; seen 2026-11-05, runs/bots-2: parking the
             * farthest car instead let it be taken back at once, every half second). The farthest
             * friend then waits MP_SWAP_HOLD_MS with its name tag. */
            c->retry_at = now + (c->tries >= 2 ? MP_RETRY_MS : MP_ROOM_RETRY_MS); /* a refused model: not so often */
            static uint32_t told;
            const int pending = mp_dying_pending();
            const int freed = !pending && mp_kept_free_oldest("room for a nearer friend's car");
            int swapped = 0;
            if (!pending && !freed && far_slot >= 0 && far_slot != i && c->tries < 2 && d2 < MP_SWAP * MP_SWAP * far_d2) {
                MpCar *f = &g_mp_remote[far_slot];
                if (g_mp_friend_car)
                    g_mp_friend_car(far_slot, f->id, 0);
                mp_remove_friend(f, "a nearer friend needs its place");
                f->retry_at = now + MP_SWAP_HOLD_MS;
                c->retry_at = now; /* first in line once the place is free */
                swapped = 1;
            }
            if (++told <= 24u || told % 100u == 0) {
                char line[224];
                snprintf(line, sizeof line, "core.nfsc: mp car of player %u (model %08x, %.0f m) waits for room (%s)%s: %u so far",
                         s->id, s->key, sqrtf(d2), why, pending ? ", a removed car is still going" : freed ? ", a kept car removed"
                         : swapped ? ", the farthest friend's car removed" : "", told);
                log_line(line);
            }
        } else {
            /* Another model may not be loaded: the first try builds it as it is, the next asks for
             * it to be spooled, and so on every MP_RETRY_MS (MODELS). */
            const uint32_t flags = 2u | 8u | (c->tries & 1 ? 1u : 0u) | (g_mp_build_lite ? MP_LITE_FLAG : 0u);
            car = mp_build(player, c->kind, s->key, last->p, flags, look);
            g_mp_lite_built += car && g_mp_build_lite;
            if (!g_mp_no_room)
                ++c->tries;
            c->retry_at = now + (g_mp_no_room ? MP_ROOM_RETRY_MS : MP_RETRY_MS);
            if (!car && g_mp_look)
                g_mp_look(i, s->id, 0); /* not built: no look in use for this slot */
            if (!g_mp_no_room && (c->tries <= 4 || car || c->tries % 15 == 0)) {
                char line[192];
                snprintf(line, sizeof line, "core.nfsc: mp car of player %u (model %08x) %s%s%s, try %d", s->id, s->key,
                         car ? "built" : "refused, its name tag only until its model is here", flags & 1 ? " (spooled)" : "",
                         look ? " in its owner's look" : "", c->tries);
                log_line(line);
            }
        }
        if (car) {
            c->simable = car;
            c->built_at = now;
            c->look_hash = hash;
            c->tag_only = 0;
            c->follow = MP_FOLLOWING;
            c->spawn_overlap = d2 < 6.0f * 6.0f;
            if (g_mp_friend_car)
                g_mp_friend_car(i, s->id, car);
        }
    }

    g_mp_build_lite = 0; /* only friends' cars are built lite */
    /* The setting (or Auto) went down: the farthest friend's car goes, one every MP_ROOM_EVERY. */
    static double lowered_at;
    if (state && built > (int)g_mp_cars_cap && far_slot >= 0 && now - lowered_at >= MP_ROOM_EVERY) {
        lowered_at = now;
        MpCar *f = &g_mp_remote[far_slot];
        if (g_mp_friend_car)
            g_mp_friend_car(far_slot, f->id, 0);
        mp_park_friend(f, "players' cars shown lowered");
        f->retry_at = now + MP_SWAP_HOLD_MS;
    }
    g_mp_built_now = built;
    g_mp_waiting_now = waiting;
    if (state)
        mp_room_line(now, slots, built, tags);
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
    g_mp_local_paused = paused;
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

/* CAR MEMORY. Construct's gate 006d86f0 sums each car's memory estimate (006d41d0: 6397 KB for a
 * player's or racer's car) against the "Cars" pool, [0x00a62c48] KB (65000). Over it, the game does
 * not refuse the new car: it frees another one through the vehicle cache, often a friend's car.
 * Player and traffic models take 13011 KB, so 8 friends' cars fill it exactly and a ninth car of any
 * kind removed a friend, who was built again, sometimes in this player's model (crowd test 2026-11-05,
 * logs/crowd-test-20261105). The pool is raised at mod start, before the game sizes it (006bbdf0 reads
 * it at each world load), to NFSCO's 102400 KB: room for about 13 such cars. The track streaming pool
 * grows by the same amount (006b6eb1). */
#define MP_CARS_POOL 0x00a62c48u
#define MP_CARS_POOL_KB 102400u

static void mp_install(void) {
    /* The Cars pool size is carlimit.h's now (Step B, 2026-11-05): carlimit_install, which runs after this, is the
     * only writer of MP_CARS_POOL (a phone keeps the game's 65000 KB; logs/mobile-crash-20261105). */
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
    sh->more.magic = MP_MORE_MAGIC; /* MORE FRIENDS: a page that knows MpMore uses all the slots */
    sh->more.count = MP_SLOTS;
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
