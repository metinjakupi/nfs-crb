/* cops.h - a player's police chase, shown in the other players' games (included by nfsc.c after mp.h).
 *
 * Each game runs its own city and its own police; mp.h only sends each player's own car. This
 * file sends the cop cars that chase a player, so the friends near that player see the chase,
 * and sends a friend's hits on those cops back, so ramming them counts in the real chase.
 *
 * THE CHASED PLAYER'S GAME (step 1). Every frame, while the page has the relay link up, the mod
 * lists the cop cars of the player's pursuit (the COP_MAX nearest within COP_RANGE) and writes
 * each one's pose, model, light bar and siren into CopShared, for the page (web/net.js), which
 * sends them to the players near this one about 18 times a second ("cops" frames, the relay
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
 * while the other tab's cops boxed it in). The same holds for this game's own
 * cops, which step 3's pursuit.h sends after the same friend: two sets of police in one
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
 * contact" for that pair, and its caller then skips the solve too) until they are apart again.
 *
 * WATCHING A CHASE. A cop copy never touches another replayed car (the chased friend's, other
 * copies) or this city's traffic: its owner's game already settled those touches, and this
 * city's traffic was never in that game. Solid, a cop pressing on the car it chases went through
 * the pull back of NEAR OUR CAR, was pushed off the car and snapped back, again and again
 * (seen 2026-11-04 as cops shuddering around another player). It stays solid to our car, this
 * game's own cops and the world.
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
#define COP_MAX 16            /* cops (and shared traffic) sent per player: 8 in CopShared.out, 8 more in CopMore (Step B) */
#define COP_OUT_V1 8
#define COP_SLOTS 32          /* friends' cops shown here: 16 in CopShared.slots, 16 more in CopMore */
#define COP_SLOTS_V1 16
#define COP_MORE_MAGIC 0x2b504f43u /* "COP+" */
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
#define COP_DRV_SPAN 120.0    /* ms of playback a copy's change in speed is measured over */
#define COP_DRV_LATERAL 20.0f /* m/s^2 sideways at full steering (a player's car at 27 m/s, measured 2026-11-04) */
#define COP_DRV_WHEELBASE 2.7f /* metres, for steering at low speed */
#define COP_DRV_GEAR 18.0f    /* m/s each gear covers (first held to about 20 m/s, measured) */
#define COP_DRV_SMOOTH 0.3f   /* share of the way to the new controls each frame */
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
#define COP_F_TRAFFIC 8u      /*        a traffic car of the sender's game, not a cop (see SHARED TRAFFIC) */
/* bits 4-7: the siren state + 1 (0 = not known) */

/* SHARED TRAFFIC (plans/shared-traffic.md). The same frames carry the sender's traffic: with a
 * friend's car within TRF_FRIEND, the list is topped up to COP_MAX with the traffic cars (driver
 * class 1) nearest the player within TRF_RANGE, flagged COP_F_TRAFFIC, after the cops of a chase.
 * A friend's game builds them as it builds cop copies (in a traffic model it has when their own
 * is not here, never a cop model). Two games near each other share one street: a game that gets
 * traffic from a player with a lower relay id stops spawning its own (traffic.h, the spawn check
 * 00444d90) and sends none, so the lower id's traffic is the one both see; with none coming any
 * more (that player left or drove away) it spawns again. Few cars, as the user wants: COP_MAX.
 * A truck's trailer is a car of its own (driver class 4, the TRAILER type): it goes with the traffic
 * (flag COP_F_TRAILER) and is built only in its own model, never in a car's. */
#define TRF_CLASS 1u          /* driver class: traffic */
#define TRF_TOWED 4u          /* driver class of a truck's trailer: a car of its own (2026-11-05, both trucks
                               * of a test game: class 4, PVehicle + 0x11c = the "TRAILER" key at 0x00a9e7bc) */
#define TRF_TYPE 0x11cu       /* PVehicle: its vehicle type key (cars 336fcacf, trucks 89e87652 seen) */
#define TRF_TRAILER_KEY 0x00a9e7bcu /* the "TRAILER" type key (filled at 009b6ca0) */
#define COP_F_TRAILER 0x10u   /* flags, with COP_F_TRAFFIC (no siren bits then): a truck's trailer */
/* TRUCKS WHOLE (trucks-20261106: "the trucks come without the head sometimes"). A truck's cab (class 1) and its trailer
 * (class 4) were two cars to this file: chosen one by one (nearest COP_MAX, range, RANGE KEEP, the sent-already bonus
 * TRF_KEEP), built one by one in a friend's game (one build per MP_BUILD_GAP_MS, in slot order, each in its own model
 * or a kept copy) and removed one by one (out of the list, far away, lingering). So a friend's game showed a trailer
 * with no cab: the trailer was built or taken from the kept copies before its cab, the cab's copy went first, or the
 * owner sent the trailer and not its cab (the cab one place too far for the nearest COP_MAX, or out of range first).
 * Now:
 *   - The owner knows which trailer a cab tows: PVehicle + TRF_ARTIC is the cab's IArticulatedVehicle (table
 *     TRF_ARTIC_TABLE; its slot 1, 00709ce0, returns +0x10: the trailer's IVehicle = trailer PVehicle + 0xb8; 0 on
 *     every other car; measured on both truck models, 0a92cc67 + dcfbf79e and a9b509c4 + 1caa5b90).
 *   - A trailer is never chosen alone: the cab is chosen as any traffic car, and its trailer is sent right after it
 *     in the same frame (COP_F_TOWING on the cab), or neither when only one place is left. A trailer with no cab
 *     towing it is not sent.
 *   - The trailer's number is its cab's number + TRF_TRAILER_NO (cabs and all other cars are numbered 1-127), so a
 *     friend's game knows exactly which cab a trailer belongs to.
 *   - A friend's game builds (or takes back) a trailer's copy only while its cab's copy is here, and the same frame
 *     the cab's copy goes (removed, kept, lingered out, gone with the world) the trailer's copy goes too. */
#define COP_F_TOWING 0x20u    /* flags, with COP_F_TRAFFIC: a truck's cab towing a trailer, sent with it (TRUCKS WHOLE) */
#define TRF_TRAILER_NO 0x80u  /* a trailer's number: its cab's number (1-127) + this */
#define TRF_ARTIC 0x108u      /* PVehicle: IArticulatedVehicle * (a truck's hitch), 0 for other cars */
#define TRF_ARTIC_TABLE 0x009e72d4u
/* OLDER GAMES' TRAILERS (logs/duel-trucks-20261106: trucks without their cab still seen on the live release 20261106-m5).
 * A game from before TRUCKS WHOLE (the public 20261106-hud build bfdc5ddf and older; players who have not reloaded)
 * sends a trailer as a car of its own, numbered below 128, often without its cab. Shown with "that owner's nearest
 * traffic car within 20 m", it stood behind any car passing near it, and stayed while that cab copy lingered still
 * (cop_cab_here counted lingering copies) as the trailer drove on. Now an older game's trailer is shown only behind a
 * copy of its own truck's cab (TRF_CAB_OF; any traffic car only when the trailer model is not one of the two known)
 * whose newest pose is within TRF_HITCH_NEAR of the trailer's, in a slot (not lingering), and it goes the frame that
 * cab copy goes or is farther than TRF_HITCH_KEEP from it. */
#define TRF_HITCH_NEAR 12.0f  /* metres: an older game's trailer is built behind its owner's cab copy this near (7-8 m measured) */
#define TRF_HITCH_KEEP 16.0f  /* metres: and goes when that cab copy is farther than this */
#define TRF_RANGE 800.0f      /* metres: traffic cars this near the player are shared (the game spawns them
                               * out of sight, 160 m and more away: seen 2026-11-04). 300 until no-vanish-20261106: a
                               * friend's game built each copy as it came within 300 m, in plain view (4 a minute on a
                               * straight road); now a friend's game has the copy where the owner has the car, out to
                               * MP_BUILD_FAR (800 m) */
#define TRF_FRIEND 300.0f     /* metres: and only while a friend's car is this near */
#define TRF_QUIET_HOLD 120000.0 /* ms: a game showing a friend's traffic stays quiet this long after it was last here (QUIET HOLD) */
#define TRF_RANGE_KEEP 960.0f /* metres: a car already sent stays sent out to this (RANGE KEEP; only where keep < 1; 360 with TRF_RANGE 300) */
#define TRF_KEEP 0.5f         /* a traffic car sent already counts as this times as far, squared (cop_nearest) */
static int g_trf_owner;       /* this game sends its traffic now */
static int g_trf_quiet;       /* this game spawns no traffic: it shows a lower id's */
static uint32_t g_trf_from;   /* that player's relay id */

/* SHARED PATROLS (plans/shared-traffic.md, "Police"). The game that owns the traffic owns the
 * police near it too: after the cops of its own chase it sends its other cop cars within
 * TRF_RANGE (patrols, and cops chasing a friend), before its traffic. A game that shows a lower
 * id's traffic runs no police of its own (pursuit.h sets the cop manager's maximum to 0) while
 * nobody here is chased and none of that player's cops is chasing; a chase brings this game's
 * police back, so a chase is shared exactly as before (cops.h, pursuit.h). NFSCO lets one game
 * spawn all police (its gate at 00413386); here it is one game per area, as for traffic.
 *
 * FRIENDS COUNT AS NEAR (as NFSCO does). While this game shares its traffic, three places that ask
 * how far a car is from the player (00761550: 00412d70 lets traffic go, 004248b0 a vehicle's AI
 * check against 80 m, 00624c40) get a distance of about 0 for a car within TRF_NEAR of a friend's
 * car: the call's position argument is pointed at our own car (BEFORE hooks at those three call
 * sites; a float result changed after the call does not reach the caller, see traffic.h), so the
 * owner's traffic and police are not let go near a friend who drives a little away.
 *
 * WRECKED COPS (as NFSCO's destroyed flag, ee894489). A cop wrecked in its owner's game (damage 1.0:
 * IDamageable at PVehicle + 0xfc, damage at +0x34) goes with COP_F_WRECKED, and stays sent for
 * COP_WRECK_KEEP while it is near, though the game drops a wrecked cop from its pursuit (UpdatePursuits
 * 00445c70): before, its copy went to the kept ones the moment it was taken out, so friends saw the
 * cop vanish. Only a cop sent already before the wreck is sent wrecked. A friend's game gives its copy
 * the game's own take-out, IDamageable::Destroy 006edae0 (every damage zone full, damage 1.0, the
 * EVehicleDestroyed event), let through cop_no_destroy for that call alone; the copy is still in no
 * pursuit (AIVehicleEmpty), so this game's chase does not count it. A copy that is wrecked while its
 * cop is not (a kept one taken by another cop) is made as new (ResetDamage 006edef0, as police.h).
 * The game gives a car's memory to a new car with its damage cleared (006edc00 from 0041862b and
 * 00419785, seen for cops and traffic 2026-11-05), and a wrecked cop once came back undamaged under its
 * number 3 frames after its wreck: a sent car wrecked and then not is taken as a new car with a new
 * number, so friends drop the wreck and build the new car. */
#define COP_F_WRECKED 4u      /* flags: the cop is wrecked in the sender's game */
#define COP_RESET 0x006edef0u /* IDamageable::ResetDamage, thiscall on the IDamageable, RET 0 */
#define COP_WRECK_KEEP 20000.0 /* ms: a wrecked cop stays sent this long after its wreck */
static int g_cop_wrecking;    /* cop_no_destroy lets this Destroy through: a copy's wreck */

#define TRF_SPAWN_COP 0x00435300u /* ICopMgr::SpawnCop (table 009c3b30 +0x20), thiscall, RET 0x14: queues a
                               * cop request (ICopMgr + 0x14 list, made into cars by 00435630 -> 004353b0).
                               * The cop manager's maximum (+0x94) = 0 did not stop patrols (a patrol cop came
                               * with it, 2026-11-05), so while patrol-quiet no request is queued (REPLACE). */
#define TRF_MAKE_COP 0x00422ef0u /* the cop manager's cop factory, thiscall (1 argument), RET 4: the car or 0.
                               * SpawnCop alone did not stop patrols either (2026-11-05: a patrol came with it):
                               * they are made here from other paths (00423610 and 00423980 from 00445b20,
                               * roadblocks 0042d2a0, 0043a9a0). Every caller handles 0 (checked in the code),
                               * so while patrol-quiet it makes none (REPLACE). */
#define COP_SHARED_PATROLS 0  /* 0 (2026-11-07): every game runs its own police again. With SHARED PATROLS on, a game
                               * near a lower-numbered player made no cops at all, and the patrol copies it showed
                               * cannot notice anyone, so players near others were never chased (player reports). */
#define TRF_NEAR 150.0f       /* metres: NFSCO's distance, the game's own 150 (009c17a8) */
static uint32_t g_trf_spawn_hook, g_trf_make_hook;
#define TRF_DISTANCE 0x00761550u /* cdecl (const float pos[3]) -> distance to the player, ST0 */
static const uint32_t g_trf_near_ret[3] = {0x00412e2fu, 0x004248d4u, 0x00624ce4u}; /* after its three calls */
static uint32_t g_trf_near_hooks[3];
static uint32_t g_trf_near_buf; /* our car's position, 12 guest bytes */
static int g_cop_quiet;       /* this game runs no police of its own now (pursuit.h) */

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
    CopOut out[COP_OUT_V1];
    MpSlot slots[COP_SLOTS_V1]; /* the page's: friends' cops (see A FRIEND'S GAME) */
    uint32_t tx_head, tx_tail, rx_head, rx_tail;
    CopHit tx[COP_HITS], rx[COP_HITS]; /* single-producer/single-consumer queues */
    /* CopMore (+0x1a30, Step B): with its magic, `count` may go to COP_MAX (out 8.. here) and the page may
     * fill COP_SLOTS slots (16.. here). A page that does not know it uses the first 8 and 16. */
    uint32_t more_magic, more_out, more_slots, more_spare;
    CopOut out2[COP_MAX - COP_OUT_V1];
    MpSlot slots2[COP_SLOTS - COP_SLOTS_V1];
} CopShared;
/* The page reads these by offset (web/net.js). */
_Static_assert(sizeof(CopOut) == 0x40 && sizeof(CopHit) == 0x40 && offsetof(CopShared, out) == 0x20 &&
                   offsetof(CopShared, slots) == 0x220 && offsetof(CopShared, tx_head) == 0x1220 &&
                   offsetof(CopShared, more_magic) == 0x1a30 && offsetof(CopShared, out2) == 0x1a40 &&
                   offsetof(CopShared, slots2) == 0x1a40 + 0x40 * (COP_MAX - COP_OUT_V1),
               "CopShared layout");

static CopOut *cop_out(CopShared *sh, uint32_t i) {
    return i < COP_OUT_V1 ? &sh->out[i] : &sh->out2[i - COP_OUT_V1];
}
static MpSlot *cop_slot(CopShared *sh, int i) {
    return i < COP_SLOTS_V1 ? &sh->slots[i] : &sh->slots2[i - COP_SLOTS_V1];
}

typedef struct CopTrack { /* a cop chasing this player, as sent */
    uint32_t car;         /* PVehicle, 0 = free */
    uint32_t number;
    int seen;
    double touched[MP_SLOTS]; /* our ms: its last touch by the car of g_mp_remote[i] here */
    CopHit held;          /* a received hit waiting for our own touch */
    int holding, from;
    double held_at;
    double wrecked_at;    /* our ms: when it was first sent wrecked, 0 = not yet (see WRECKED COPS) */
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
    float steer, gas, brake; /* the controls cop_drive worked out last frame */
    int gear;             /* and its gear, 0 = none yet */
    int met;              /* with our car: 0 apart, 1 solid (we ran into it), 2 passing through (see PASSING THROUGH) */
    int wreck_tried;      /* this cop's wreck has been given to the copy (see WRECKED COPS) */
    uint32_t cab, cab_no; /* a trailer's copy: the copy of its cab it was built behind, and that cab's number (TRUCKS WHOLE) */
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

/* Whether car is a truck's trailer of this game (driver class 4 and the TRAILER type). */
static int cop_trailer(uint32_t car) {
    return get_u32(car + 0x148) == TRF_TOWED && get_u32(car + TRF_TYPE) == get_u32(TRF_TRAILER_KEY);
}

/* The trailer cab tows (TRUCKS WHOLE), or 0: cab is a traffic car of this game whose IArticulatedVehicle names a
 * listed trailer. */
static uint32_t cop_towed(uint32_t cab) {
    if (get_u32(cab + 0x148) != TRF_CLASS)
        return 0;
    const uint32_t artic = get_u32(cab + TRF_ARTIC), iv = artic && get_u32(artic) == TRF_ARTIC_TABLE ? get_u32(artic + 0x10) : 0;
    return iv > 0xb8u && cop_trailer(iv - 0xb8u) && mp_listed(iv - 0xb8u) ? iv - 0xb8u : 0;
}

/* Whether car's damage is full: wrecked (see WRECKED COPS). */
static int cop_wrecked(uint32_t car) {
    const uint32_t damageable = get_u32(car + 0xfc);
    return damageable && bits_float(get_u32(damageable + 0x34)) >= 1.0f;
}

/* Whether a cop car may be sent as far as its wreck goes: not wrecked, or wrecked after it was sent
 * and less than COP_WRECK_KEEP ago (see WRECKED COPS). */
static int cop_wreck_ok(uint32_t car) {
    if (!cop_wrecked(car))
        return 1;
    for (int t = 0; t < COP_MAX; ++t)
        if (g_cop_track[t].car == car)
            return !g_cop_track[t].wrecked_at || mp_now() - g_cop_track[t].wrecked_at < COP_WRECK_KEEP;
    return 0;
}

/* Whether car is one of the cop cars (not a helicopter) of pursuit. */
static int cop_of(uint32_t car, uint32_t pursuit) {
    if (get_u32(car + 0x148) != COP_CLASS)
        return 0;
    const uint32_t ai = get_u32(car + COP_AI);
    return ai && get_u32(ai) == COP_AI_TABLE && get_u32(ai + COP_AI_PURSUIT) == pursuit && cop_wreck_ok(car);
}

static uint32_t cop_flags(uint32_t car) {
    if (get_u32(car + 0x148) == TRF_CLASS)
        return COP_F_TRAFFIC | (cop_towed(car) ? COP_F_TOWING : 0u);
    if (cop_trailer(car))
        return COP_F_TRAFFIC | COP_F_TRAILER;
    const uint32_t ai = get_u32(car + COP_AI);
    const uint32_t siren = (get_u32(ai + COP_AI_SIREN) + 1u) & 15u;
    return (get_u32(car + COP_GLARE) & COP_LIGHTS ? COP_F_LIGHTS : 0) |
           (get_u32(ai + COP_AI_CHASING) & 0xffu ? COP_F_CHASING : 0) |
           (cop_wrecked(car) ? COP_F_WRECKED : 0) | siren << 4;
}

/* ---- the chased player's game ---- */

/* The cars of this game for which want(car, pursuit) says yes within range of here, nearest first,
 * into found[from..COP_MAX); returns the new count. A car sent already counts as keep times as far
 * (squared): without that, traffic cars at much the same distance traded places in the list every
 * few seconds and each change rebuilt a copy in the friends' games (16 in a minute, 2026-11-04). */
static float g_cop_pts[MP_SLOTS][3]; /* friends' cars (their newest poses) within TRF_FRIEND, for shared traffic */
static int g_cop_npts;
static int cop_traffic_of(uint32_t car, uint32_t unused);
static int cop_nearest(uint32_t player, const float here[3], uint32_t pursuit, int (*want)(uint32_t, uint32_t),
                       float range, float keep, uint32_t found[COP_MAX], int from) {
    float dist[COP_MAX];
    if (from >= COP_MAX)
        return from; /* full already: dist[] holds nothing of the earlier calls */
    int n = from;
    uint32_t node = get_u32(MP_CARS);
    for (int guard = 0; node && node != MP_CARS && guard < 256; ++guard, node = get_u32(node)) {
        const uint32_t car = node - 0xd0, state = car != player ? mp_state(get_u32(car + 0x70)) : 0;
        if (!state || !want(car, pursuit))
            continue;
        float d = 0;
        for (int k = 0; k < 3; ++k) {
            const float x = bits_float(get_u32(state + 0x10 + 4u * k)) - here[k];
            d += x * x;
        }
        /* Shared traffic (Step B): the distance to whoever is nearest of us and the friends near us, so a
         * friend driving ahead keeps the cars around it (g_cop_pts, filled by cop_find). */
        for (int p = 0; want == cop_traffic_of && p < g_cop_npts; ++p) {
            float e = 0;
            for (int k = 0; k < 3; ++k) {
                const float x = bits_float(get_u32(state + 0x10 + 4u * k)) - g_cop_pts[p][k];
                e += x * x;
            }
            if (e < d)
                d = e;
        }
        int sent = 0;
        for (int t = 0; keep < 1.0f && t < COP_MAX && !sent; ++t)
            sent = g_cop_track[t].car == car;
        const float reach = sent ? range * (TRF_RANGE_KEEP / TRF_RANGE) : range; /* RANGE KEEP (traffic-stable-20261105) */
        if (!(d <= reach * reach))
            continue;
        if (sent)
            d *= keep;
        int at = n < COP_MAX ? n++ : COP_MAX;
        if (at == COP_MAX && d >= dist[COP_MAX - 1])
            continue;
        if (at == COP_MAX)
            at = COP_MAX - 1;
        for (; at > from && dist[at - 1] > d; --at) {
            found[at] = found[at - 1];
            dist[at] = dist[at - 1];
        }
        found[at] = car;
        dist[at] = d;
    }
    return n;
}

/* A traffic car of this game (not a copy of ours: those are class 7 or 8). A truck's trailer (a car of its own,
 * without which friends saw trucks without their load, 2026-11-05) is not one: it goes with its cab (TRUCKS WHOLE). */
static int cop_traffic_of(uint32_t car, uint32_t unused) {
    (void)unused;
    return get_u32(car + 0x148) == TRF_CLASS;
}

/* TRUCKS WHOLE: the traffic cars cand[0..m) (nearest first) into found[n..COP_MAX), each cab followed by the trailer
 * it tows; a cab whose trailer does not fit is left out (the next car may). Returns the new count. */
static int cop_with_trailers(uint32_t found[COP_MAX], int n, const uint32_t cand[COP_MAX], int m) {
    for (int i = 0; i < m && n < COP_MAX; ++i) {
        const uint32_t trailer = cop_towed(cand[i]);
        int dup = 0;
        for (int j = 0; trailer && j < n && !dup; ++j)
            dup = found[j] == trailer;
        if (trailer && !dup && n + 2 > COP_MAX)
            continue;
        found[n++] = cand[i];
        if (trailer && !dup)
            found[n++] = trailer;
    }
    return n;
}

/* A cop car of this game (not a helicopter) that is not in our pursuit (SHARED PATROLS). */
static int cop_other_of(uint32_t car, uint32_t pursuit) {
    if (get_u32(car + 0x148) != COP_CLASS)
        return 0;
    const uint32_t ai = get_u32(car + COP_AI);
    return ai && get_u32(ai) == COP_AI_TABLE && (!pursuit || get_u32(ai + COP_AI_PURSUIT) != pursuit) &&
           !cop_wrecked(car);
}

/* A wrecked cop car of this game out of our pursuit (the game drops a wrecked cop from it), still sent
 * within COP_RANGE as in the chase it was in (see WRECKED COPS). */
static int cop_wreck_of(uint32_t car, uint32_t pursuit) {
    if (get_u32(car + 0x148) != COP_CLASS || !cop_wrecked(car))
        return 0;
    const uint32_t ai = get_u32(car + COP_AI);
    return ai && get_u32(ai) == COP_AI_TABLE && (!pursuit || get_u32(ai + COP_AI_PURSUIT) != pursuit) &&
           cop_wreck_ok(car);
}

/* The cops of the player's pursuit, nearest first, then (while this game shares its traffic) the
 * nearest other cops and the nearest traffic cars, into found[]; returns how many. */
static int cop_find(uint32_t player, uint32_t found[COP_MAX]) {
    const uint32_t ai = player ? get_u32(player + COP_AI) : 0;
    const uint32_t pursuit = ai ? get_u32(ai + COP_AI_PURSUIT) : 0;
    const uint32_t own = player ? mp_state(get_u32(player + 0x70)) : 0;
    if (!own)
        return 0;
    float here[3];
    for (int k = 0; k < 3; ++k)
        here[k] = bits_float(get_u32(own + 0x10 + 4u * k));
    int n = pursuit ? cop_nearest(player, here, pursuit, cop_of, COP_RANGE, 1.0f, found, 0) : 0;
    n = cop_nearest(player, here, pursuit, cop_wreck_of, COP_RANGE, 1.0f, found, n); /* sent ones only */
    g_cop_npts = 0;
    for (int i = 0; i < MP_SLOTS; ++i) {
        const MpCar *c = &g_mp_remote[i];
        if (!c->id || !c->kept)
            continue;
        float d = 0;
        for (int k = 0; k < 3; ++k)
            d += (c->pose[c->kept - 1].p[k] - here[k]) * (c->pose[c->kept - 1].p[k] - here[k]);
        if (d <= TRF_FRIEND * TRF_FRIEND)
            memcpy(g_cop_pts[g_cop_npts++], c->pose[c->kept - 1].p, sizeof g_cop_pts[0]);
    }
    if (g_trf_owner) {
        n = cop_nearest(player, here, pursuit, cop_other_of, TRF_RANGE, TRF_KEEP, found, n);
        uint32_t cand[COP_MAX];
        const int m = cop_nearest(player, here, 0, cop_traffic_of, TRF_RANGE, TRF_KEEP, cand, 0);
        n = cop_with_trailers(found, n, cand, m); /* TRUCKS WHOLE */
    }
    return n;
}

/* Who shows whose traffic (see SHARED TRAFFIC), once a frame before this game's list is made. */
static void cop_traffic_owner(uint32_t player) {
    const MpShared *mp = mp_shared();
    const uint32_t me = mp ? mp->me : 0;
    uint32_t from = 0;
    for (int i = 0; me && player && i < COP_SLOTS; ++i) {
        const MpSlot *s = &g_cop_seen[i];
        if (s->id && s->mode == 1 && (s->pad[1] & COP_F_TRAFFIC) && s->id < me && (!from || s->id < from))
            from = s->id;
    }
    const uint32_t own = player ? mp_state(get_u32(player + 0x70)) : 0;
    /* QUIET HOLD (traffic-stable-20261105): still that player's street for a while after its traffic was last here. */
    {
        static double from_at;
        const double now = mp_now();
        if (from) {
            from_at = now;
        } else if (g_trf_from && own && now - from_at < TRF_QUIET_HOLD) {
            for (int i = 0; i < MP_SLOTS; ++i) {
                const MpCar *c = &g_mp_remote[i];
                if (c->id != g_trf_from || !c->kept)
                    continue;
                float d = 0;
                for (int k = 0; k < 3; ++k) {
                    const float x = c->pose[c->kept - 1].p[k] - bits_float(get_u32(own + 0x10 + 4u * k));
                    d += x * x;
                }
                if (d <= TRF_FRIEND * TRF_FRIEND)
                    from = g_trf_from;
            }
        }
    }
    int near = 0;
    for (int i = 0; own && !from && i < MP_SLOTS && !near; ++i) {
        const MpCar *c = &g_mp_remote[i];
        if (!c->id || !c->kept)
            continue;
        float d = 0;
        for (int k = 0; k < 3; ++k) {
            const float x = c->pose[c->kept - 1].p[k] - bits_float(get_u32(own + 0x10 + 4u * k));
            d += x * x;
        }
        near = d <= TRF_FRIEND * TRF_FRIEND;
    }
    const int owner = !from && near, quiet = from != 0;
    if (quiet != g_trf_quiet || owner != g_trf_owner || (quiet && from != g_trf_from)) {
        char line[128];
        if (quiet)
            snprintf(line, sizeof line, "core.nfsc: traffic: showing player %u's traffic, ours spawns no more", from);
        else
            snprintf(line, sizeof line, owner ? "core.nfsc: traffic: sharing ours with the friends near us"
                                              : "core.nfsc: traffic: our own, not shared (no friend near)");
        log_line(line);
    }
    g_trf_owner = owner;
    g_trf_quiet = quiet;
    g_trf_from = from;
    /* SHARED PATROLS: no police of our own while that player's are the ones here and no chase. */
    const uint32_t ai = player ? get_u32(player + COP_AI) : 0;
    int chase = ai && get_u32(ai + COP_AI_PURSUIT) != 0;
    for (int i = 0; quiet && !chase && i < COP_SLOTS; ++i) {
        const MpSlot *s = &g_cop_seen[i];
        chase = s->id == from && s->mode == 1 && (s->pad[1] & COP_F_CHASING) != 0;
    }
    const int cop_quiet = COP_SHARED_PATROLS && quiet && !chase;
    if (cop_quiet != g_cop_quiet) {
        char line[128];
        if (cop_quiet)
            snprintf(line, sizeof line, "core.nfsc: police: player %u's police patrol here, ours spawn none", from);
        else
            snprintf(line, sizeof line, "core.nfsc: police: our own again (%s)", chase ? "a chase" : "no friend's police here");
        log_line(line);
    }
    g_cop_quiet = cop_quiet;
}

/* Whether this game's traffic spawning is off now (traffic.h). */
static int cop_traffic_quiet(void) {
    return g_trf_quiet;
}

/* Whether this game's police are off now (pursuit.h sets the cop manager's maximum to 0). */
static int cop_patrol_quiet(void) {
    return g_cop_quiet;
}

/* SHARED PATROLS: no new cop asked for while a friend's police patrol here (REPLACE SpawnCop). */
static void cop_no_spawn(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    if (g_cop_quiet)
        api->hook_return(api, cpu, cpu->eax, 0x14);
    else
        api->call_original(api, cpu->target, cpu);
}

/* SHARED PATROLS: no cop car made while a friend's police patrol here (REPLACE the cop factory). */
static void cop_no_make(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    if (g_cop_quiet)
        api->hook_return(api, cpu, 0, 4);
    else
        api->call_original(api, cpu->target, cpu);
}

/* FRIENDS COUNT AS NEAR: BEFORE 00761550 at its three call sites. */
static void cop_near_friends(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)inv;
    (void)user;
    const uint32_t pos = get_u32(cpu->esp + 4);
    const uint32_t me = g_mp_player_simable ? mp_state(get_u32(g_mp_player_simable - 0x24u + 0x70)) : 0;
    if (!g_trf_owner || !g_trf_near_buf || !pos || !me)
        return;
    float p[3];
    for (int k = 0; k < 3; ++k)
        p[k] = bits_float(get_u32(pos + 4u * k));
    for (int i = 0; i < MP_SLOTS; ++i) {
        const MpCar *c = &g_mp_remote[i];
        if (!c->id || !c->kept)
            continue;
        float d = 0;
        for (int k = 0; k < 3; ++k)
            d += (c->pose[c->kept - 1].p[k] - p[k]) * (c->pose[c->kept - 1].p[k] - p[k]);
        if (d <= TRF_NEAR * TRF_NEAR) {
            mp_copy(g_trf_near_buf, me + 0x10, 12);
            mp_put(cpu->esp + 4, g_trf_near_buf);
            return;
        }
    }
}

/* TRAFFIC NUMBER DIAGNOSIS (traffic-stable-20261105, test only: RECOMP_TRF_DIAG=1 in the URL). In the
 * owner's game, each frame: every track (sent car) that lost its number and every new number, with the car's
 * address, class, model, distance, whether it is still in the car list, and whether the same address had
 * a number before (memory given to a new car, or the same car back). */
typedef struct CopDiagOld { uint32_t car, number, key, cls; float p[3]; double at; } CopDiagOld;
static CopDiagOld g_cdg_hist[64];
static int g_cdg_next;
static int cop_diag_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_TRF_DIAG");
        on = e && e[0] == '1';
    }
    return on;
}
static int cop_diag_pos(uint32_t car, float p[3]) {
    const uint32_t st = car ? mp_state(get_u32(car + 0x70)) : 0;
    for (int k = 0; st && k < 3; ++k)
        p[k] = bits_float(get_u32(st + 0x10 + 4u * k));
    return st != 0;
}
static void cop_diag(uint32_t player, const uint32_t old_car[COP_MAX], const uint32_t old_num[COP_MAX], int n,
                     const uint32_t found[COP_MAX]) {
    static int last_n = -1;
    static uint32_t frames;
    ++frames;
    float here[3] = {0, 0, 0};
    cop_diag_pos(player, here);
    const double now = mp_now();
    if ((n == 0) != (last_n == 0) || last_n < 0) {
        char line[200];
        snprintf(line, sizeof line, "core.nfsc: trfdiag: n %d -> %d (page %d, owner %d, quiet %d, player %x)", last_n, n,
                 cop_shared() ? (int)__atomic_load_n(&cop_shared()->page, __ATOMIC_ACQUIRE) : -1, g_trf_owner, g_trf_quiet,
                 player);
        log_line(line);
    }
    last_n = n;
    /* Freed numbers. */
    for (int t = 0; t < COP_MAX; ++t) {
        if (!old_car[t])
            continue;
        int still = 0;
        for (int u = 0; u < COP_MAX && !still; ++u)
            still = g_cop_track[u].car == old_car[t] && g_cop_track[u].number == old_num[t];
        if (still)
            continue;
        const uint32_t car = old_car[t];
        float p[3] = {0, 0, 0};
        const int listed = mp_listed(car), st = listed && cop_diag_pos(car, p);
        float d = 0;
        for (int k = 0; k < 3; ++k)
            d += (p[k] - here[k]) * (p[k] - here[k]);
        int rank = -1;
        for (int i = 0; i < n; ++i)
            if (found[i] == car)
                rank = i;
        char line[240];
        snprintf(line, sizeof line,
                 "core.nfsc: trfdiag: f%u free %u car %x listed %d state %d cls %u key %08x dist %.0f found-rank %d n %d", frames,
                 old_num[t], car, listed, st, listed ? get_u32(car + 0x148) : 0u, listed ? mp_model_key(car) : 0u,
                 st ? sqrtf(d) : -1.0f, rank, n);
        log_line(line);
        CopDiagOld *h = &g_cdg_hist[g_cdg_next++ % 64];
        h->car = car;
        h->number = old_num[t];
        h->key = listed ? mp_model_key(car) : 0;
        h->cls = listed ? get_u32(car + 0x148) : 0;
        memcpy(h->p, p, sizeof h->p);
        h->at = now;
    }
    /* New numbers. */
    for (int u = 0; u < COP_MAX; ++u) {
        const CopTrack *tr = &g_cop_track[u];
        if (!tr->car)
            continue;
        int had = 0;
        for (int t = 0; t < COP_MAX && !had; ++t)
            had = old_car[t] == tr->car && old_num[t] == tr->number;
        if (had)
            continue;
        float p[3] = {0, 0, 0};
        cop_diag_pos(tr->car, p);
        float d = 0;
        for (int k = 0; k < 3; ++k)
            d += (p[k] - here[k]) * (p[k] - here[k]);
        const CopDiagOld *prev = NULL;
        for (int i = 0; i < 64; ++i) {
            const CopDiagOld *h = &g_cdg_hist[(g_cdg_next - 1 - i + 128) % 64];
            if (h->car == tr->car) {
                prev = h;
                break;
            }
        }
        float moved = 0;
        for (int k = 0; prev && k < 3; ++k)
            moved += (p[k] - prev->p[k]) * (p[k] - prev->p[k]);
        char line[260];
        snprintf(line, sizeof line,
                 "core.nfsc: trfdiag: f%u new %u car %x cls %u key %08x dist %.0f n %d; same address before: %s %u key %s moved %.0f m %.0f ms ago",
                 frames, tr->number, tr->car, get_u32(tr->car + 0x148), mp_model_key(tr->car), sqrtf(d), n, prev ? "number" : "none",
                 prev ? prev->number : 0u, prev ? (prev->key == mp_model_key(tr->car) ? "same" : "other") : "-",
                 prev ? sqrtf(moved) : 0.0f, prev ? now - prev->at : 0.0);
        log_line(line);
    }
}

/* STABLE NUMBERS (traffic-stable-20261105). A car's number used to live in its track (CopTrack), and a track
 * lived only while the car was in this frame's list: a traffic car that left the list for one frame (the
 * nearest COP_MAX traded places, it passed out of range and back, the list was empty for a frame) came back
 * under a new number, and every friend's game removed its copy and built a new one. Now each car of this game
 * that can be sent (police class 2, traffic class 1, trucks' trailers) gets its number when it first appears
 * in the game's car list and keeps it for its whole life, sent or not. A car's memory given to a new car
 * (the game frees a car and builds the next in the same place: the same address) is a new car: a new model or
 * class, a jump no car can drive (COP_ID_JUMP + COP_ID_SPEED x the time since we last saw it), or a wrecked
 * cop no longer wrecked (see WRECKED COPS). Measured (d15-fixA-1): the game moves a patrol cop's car 100-300 m
 * about every 10 s (the same address: its patrol given a new place), which is a new car here, so a friend's
 * game takes its copy away (out of view, or lingering in view) and builds the new one where it now is. A car
 * gone from the list frees its number. */
#define COP_IDS 128
#define COP_ID_JUMP 40.0f   /* metres, plus */
#define COP_ID_SPEED 120.0f /* metres a second since it was last seen */
typedef struct CopId {
    uint32_t car, number, key, cls;
    uint32_t towed;       /* the trailer this cab tows, this frame (TRUCKS WHOLE), 0 = none */
    float p[3];
    int has_p, wrecked;
    uint32_t frame;
    double at;
} CopId;
static CopId g_cop_ids[COP_IDS];
static uint32_t g_cop_id_frame, g_cop_id_next, g_cop_ids_born, g_cop_ids_reborn, g_cop_ids_full;

static int cop_id_wanted(uint32_t car) {
    const uint32_t cls = get_u32(car + 0x148);
    return cls == TRF_CLASS || cls == COP_CLASS; /* a trailer's number is its cab's (TRUCKS WHOLE) */
}

static uint32_t cop_id_number_free(void) {
    for (int guard = 0; guard < 128; ++guard) {
        g_cop_id_next = g_cop_id_next % (TRF_TRAILER_NO - 1u) + 1u; /* 1-127: + TRF_TRAILER_NO is its trailer's */
        int used = 0;
        for (int i = 0; i < COP_IDS && !used; ++i)
            used = g_cop_ids[i].car && g_cop_ids[i].number == g_cop_id_next;
        if (!used)
            return g_cop_id_next;
    }
    return 0;
}

/* Once a frame, before the list is made: every car's number, kept or new. */
static void cop_ids_frame(void) {
    const uint32_t frame = ++g_cop_id_frame;
    const double now = mp_now();
    uint32_t node = get_u32(MP_CARS);
    for (int guard = 0; node && node != MP_CARS && guard < 256; ++guard, node = get_u32(node)) {
        const uint32_t car = node - 0xd0;
        if (!cop_id_wanted(car))
            continue;
        const uint32_t key = mp_model_key(car), cls = get_u32(car + 0x148), state = mp_state(get_u32(car + 0x70));
        float p[3] = {0, 0, 0};
        for (int k = 0; state && k < 3; ++k)
            p[k] = bits_float(get_u32(state + 0x10 + 4u * k));
        const int wrecked = cls == COP_CLASS && cop_wrecked(car);
        CopId *id = NULL, *free_id = NULL;
        for (int i = 0; i < COP_IDS && !id; ++i) {
            if (g_cop_ids[i].car == car)
                id = &g_cop_ids[i];
            else if (!g_cop_ids[i].car && !free_id)
                free_id = &g_cop_ids[i];
        }
        int born = 0;
        if (id) {
            float jump = 0;
            for (int k = 0; state && id->has_p && k < 3; ++k)
                jump += (p[k] - id->p[k]) * (p[k] - id->p[k]);
            const float may = COP_ID_JUMP + COP_ID_SPEED * (float)((now - id->at) / 1000.0);
            /* A new car is in the list at the world's origin for a frame or two before the game puts it on
             * the road (every traffic car, 3280-3370 m jumps in d15-fixA-1): that first move is not a new car. */
            const int placed = fabsf(id->p[0]) + fabsf(id->p[1]) + fabsf(id->p[2]) > 5.0f;
            if (id->key != key || id->cls != cls || (id->wrecked && !wrecked) || (placed && jump > may * may)) {
                if (cop_diag_on()) {
                    char line[200];
                    snprintf(line, sizeof line,
                             "core.nfsc: trfdiag: number %u's car %x is a new car (key %08x -> %08x, class %u -> %u, wrecked %d -> %d, "
                             "jump %.0f m in %.0f ms)",
                             id->number, car, id->key, key, id->cls, cls, id->wrecked, wrecked, sqrtf(jump), now - id->at);
                    log_line(line);
                }
                ++g_cop_ids_reborn;
                id->number = 0;
                born = 1;
            }
        } else if (free_id) {
            id = free_id;
            memset(id, 0, sizeof *id);
            id->car = car;
            born = 1;
        } else {
            ++g_cop_ids_full;
            continue;
        }
        if (born) {
            id->number = cop_id_number_free();
            ++g_cop_ids_born;
        }
        id->key = key;
        id->cls = cls;
        id->towed = cop_towed(car);
        id->wrecked = wrecked;
        id->frame = frame;
        if (state) {
            memcpy(id->p, p, sizeof id->p);
            id->has_p = 1;
            id->at = now;
        }
    }
    for (int i = 0; i < COP_IDS; ++i)
        if (g_cop_ids[i].car && g_cop_ids[i].frame != frame)
            g_cop_ids[i].car = 0; /* gone from the car list */
    static double said_at;
    if (now - said_at >= 30000.0) {
        said_at = now;
        int live = 0;
        for (int i = 0; i < COP_IDS; ++i)
            live += g_cop_ids[i].car != 0;
        char line[160];
        snprintf(line, sizeof line, "core.nfsc: cops: car numbers: %d cars now, %u numbered so far, %u of them memory given to a new car%s",
                 live, g_cop_ids_born, g_cop_ids_reborn, g_cop_ids_full ? " (table full at times)" : "");
        log_line(line);
    }
}

/* car's number for its life (cop_ids_frame), 0 = none. A trailer's is its cab's + TRF_TRAILER_NO while that cab
 * tows it (TRUCKS WHOLE). */
static uint32_t cop_id_number(uint32_t car) {
    for (int i = 0; car && i < COP_IDS; ++i)
        if (g_cop_ids[i].car == car)
            return g_cop_ids[i].number;
    for (int i = 0; car && i < COP_IDS; ++i)
        if (g_cop_ids[i].car && g_cop_ids[i].towed == car && g_cop_ids[i].number && g_cop_ids[i].number < TRF_TRAILER_NO)
            return g_cop_ids[i].number + TRF_TRAILER_NO;
    return 0;
}

/* A track for car (a car sent this frame), with the car's own number (STABLE NUMBERS). */
static CopTrack *cop_track(uint32_t car) {
    CopTrack *free_track = NULL;
    for (int i = 0; i < COP_MAX; ++i) {
        if (g_cop_track[i].car == car)
            return &g_cop_track[i];
        if (!g_cop_track[i].car && !free_track)
            free_track = &g_cop_track[i];
    }
    const uint32_t number = cop_id_number(car);
    if (!free_track || !number)
        return NULL;
    memset(free_track, 0, sizeof *free_track);
    free_track->car = car;
    free_track->number = number;
    return free_track;
}

/* TEST (RECOMP_COP_WRECK=1 in the URL): the first cop sent to friends for 15 s is taken out with the
 * game's own Destroy, as a ram that wrecks it would (WRECKED COPS). With no cop sent 20 s after the
 * first frame sent, a chase of our car is started (ICopMgr +0x3c PursueAtHeatLevel 004358d0, heat 1). */
static void cop_wreck_test(int n, const uint32_t found[COP_MAX]) {
    static int test = -1, done, chased;
    static double since, first;
    if (test < 0) {
        const char *e = getenv("RECOMP_COP_WRECK");
        test = e && e[0] == '1';
    }
    if (!test || done || !n)
        return;
    if (!first)
        first = mp_now();
    const uint32_t mgr = get_u32(0x00a9e874u);
    if (!chased && !since && mp_now() - first > 20000.0 && mgr) {
        const uint32_t heat = 1;
        mp_call(get_u32(get_u32(mgr) + 0x3c), mgr, &heat, 1);
        chased = 1;
        log_line("core.nfsc: cops: test: a chase of our car started");
    }
    for (int i = 0; i < n; ++i) {
        const uint32_t damageable = get_u32(found[i] + 0xfc);
        if (get_u32(found[i] + 0x148) != COP_CLASS || !damageable)
            continue;
        if (!since)
            since = mp_now();
        if (mp_now() - since < 15000.0)
            return;
        mp_call(COP_DESTROY, damageable, NULL, 0);
        done = 1;
        log_line(cop_wrecked(found[i]) ? "core.nfsc: cops: test: a cop sent to friends taken out"
                                       : "core.nfsc: cops: test: Destroy left the cop as it was");
        return;
    }
}

/* This player's chase, for the page to send. */
static void cop_publish(CopShared *sh, uint32_t player) {
    static uint32_t last_count;
    uint32_t found[COP_MAX];
    uint32_t dg_car[COP_MAX], dg_num[COP_MAX];
    for (int i = 0; i < COP_MAX; ++i) {
        dg_car[i] = g_cop_track[i].car;
        dg_num[i] = g_cop_track[i].number;
    }
    cop_ids_frame();
    const int n = __atomic_load_n(&sh->page, __ATOMIC_ACQUIRE) ? cop_find(player, found) : 0;
    for (int i = 0; i < COP_MAX; ++i) {
        g_cop_track[i].seen = 0;
        if (g_cop_track[i].car && g_cop_track[i].wrecked_at && !cop_wrecked(g_cop_track[i].car))
            g_cop_track[i].car = 0; /* its car given to a new one (see WRECKED COPS): a new number */
    }
    CopTrack *tracks[COP_MAX];
    /* Cars no longer listed give up their track first, so a new one gets it this frame. */
    for (int t = 0; t < COP_MAX; ++t) {
        int listed = 0;
        for (int i = 0; i < n && !listed; ++i)
            listed = g_cop_track[t].car == found[i];
        if (!listed || (g_cop_track[t].car && g_cop_track[t].number != cop_id_number(g_cop_track[t].car)))
            g_cop_track[t].car = 0; /* not sent now, or its memory is a new car's (STABLE NUMBERS) */
    }
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
    uint32_t count = 0, sent_cab = 0; /* sent_cab: the trailer the car sent just before tows (TRUCKS WHOLE) */
    for (int i = 0; i < n; ++i) {
        CopOut *o = cop_out(sh, count);
        const uint32_t state = mp_state(get_u32(found[i] + 0x70));
        const uint32_t tows = sent_cab;
        sent_cab = 0;
        if (!tracks[i] || !state || (cop_trailer(found[i]) && tows != found[i]))
            continue; /* a trailer goes only right after its cab, in the same frame */
        mp_read(state, &o->pose);
        if (!mp_pose_ok(&o->pose))
            continue;
        sent_cab = get_u32(found[i] + 0x148) == TRF_CLASS ? cop_towed(found[i]) : 0;
        o->id = tracks[i]->number;
        o->key = mp_model_key(found[i]);
        o->flags = cop_flags(found[i]);
        if ((o->flags & COP_F_WRECKED) && !tracks[i]->wrecked_at) {
            tracks[i]->wrecked_at = mp_now();
            char line[96];
            snprintf(line, sizeof line, "core.nfsc: cops: cop %u wrecked, shown wrecked to friends", o->id);
            log_line(line);
        }
        ++count;
    }
    if (cop_diag_on())
        cop_diag(player, dg_car, dg_num, n, found);
    cop_wreck_test(n, found);
    sh->count = count;
    __atomic_fetch_add(&sh->seq, 1u, __ATOMIC_SEQ_CST);
    uint32_t cops = 0;
    for (uint32_t i = 0; i < count; ++i)
        cops += !(cop_out(sh, i)->flags & COP_F_TRAFFIC);
    if (!cops != !last_count) {
        char line[96];
        snprintf(line, sizeof line, cops ? "core.nfsc: cops: in a chase, %u cops sent to friends"
                                         : "core.nfsc: cops: chase over (was %u cops)", cops ? cops : last_count);
        log_line(line);
    }
    last_count = cops;
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

/* Whether simable is a copy of a friend's cop, not of their traffic (race.h's minimap). */
static int cop_chase_copy(uint32_t simable) {
    for (int i = 0; simable && i < COP_SLOTS; ++i)
        if (g_cop_cars[i].car.simable == simable && !(g_cop_cars[i].flags & COP_F_TRAFFIC))
            return 1;
    return 0;
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
        snprintf(line, sizeof line, "core.nfsc: cops: %s %u of player %u removed (%s)",
                 cc->flags & COP_F_TRAILER && cc->flags & COP_F_TRAFFIC ? "trailer" : cc->flags & COP_F_TRAFFIC ? "traffic car" : "cop",
                 cc->number, c->id, why);
        log_line(line);
    }
    for (int i = 0; c->simable && i < COP_SPARES; ++i) {
        CopSpare *sp = &g_cop_spares[i];
        if (sp->simable || !mp_listed(c->simable - 0x24) || !mp_kind_kept(c->simable - 0x24, c->kind))
            continue;
        const uint32_t car = c->simable - 0x24, state = mp_state(get_u32(car + 0x70));
        mp_put(car + COP_GLARE, get_u32(car + COP_GLARE) & ~COP_LIGHTS);
        const uint32_t in = get_u32(car + DRV_INPUT); /* IInput::ClearInput: no gas left on */
        if (in && get_u32(in))
            mp_call(drv_method(in, 1), in, NULL, 0);
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
static int cop_spare_ok(const CopSpare *sp);
/* Whether a kept copy of this model is free: taking one is cheap, a build is not. */
static int cop_spare_free(uint32_t key, uint32_t kind) {
    for (int i = 0; i < COP_SPARES; ++i)
        if (g_cop_spares[i].simable && g_cop_spares[i].key == key && g_cop_spares[i].kind == kind && cop_spare_ok(&g_cop_spares[i]))
            return 1;
    return 0;
}

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
    for (int guard = 0; node && node != MP_CARS && guard < 256; ++guard, node = get_u32(node)) {
        const uint32_t car = node - 0xd0, ai = get_u32(car + COP_AI);
        const uint32_t key = get_u32(car + 0x148) == COP_CLASS && ai && get_u32(ai) == COP_AI_TABLE ? mp_model_key(car) : 0;
        if (key && key != not)
            return key;
    }
    return 0;
}

/* A traffic model this game has now (one of its own traffic cars), other than not, or 0. Never a truck cab
 * that tows: as a stand-in it would show a friend's ordinary car as a cab without its trailer. */
static uint32_t traffic_model_here(uint32_t not) {
    uint32_t node = get_u32(MP_CARS);
    for (int guard = 0; node && node != MP_CARS && guard < 256; ++guard, node = get_u32(node)) {
        const uint32_t car = node - 0xd0;
        const uint32_t key = get_u32(car + 0x148) == TRF_CLASS ? mp_model_key(car) : 0;
        if (key && key != not && !cop_towed(car))
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


/* LINGERING COPIES (Step B, 2026-11-05: "the traffic disappears in 10 seconds and then another"). A traffic
 * copy whose car its owner no longer sends (it left the owner's list) is not taken away in front of this
 * player: while it is in view (cop_in_view: on screen, within MP_VIEW_FAR) it stays where it is, its controls cleared (it
 * rolls to a stop), its slot free for the next car; it goes (cop_drop) once it is farther or out of view,
 * (COP_LINGER_MS no longer applies in view: logs/no-vanish-20261106). When its owner sends the same car again it
 * takes the lingering copy back.
 * Patrol cops (not chasing) linger too (traffic-stable-20261105): the game moves a patrol's car 100-300 m about
 * every 10 s, which is a new car (STABLE NUMBERS), and its old copy should not vanish in front of this player. */
#define COP_LINGER 16
#define COP_LINGER_NEAR 150.0f
#define COP_LINGER_MS 30000.0
#define COP_VIEW_COS 0.34f /* about 70 degrees each side of our car's heading */
typedef struct CopLinger {
    uint32_t simable, kind, key, built, number, owner, flags;
    double since;
    uint32_t cab, cab_no; /* a trailer's: as CopCar's (TRUCKS WHOLE) */
} CopLinger;
static CopLinger g_cop_linger[COP_LINGER];
static uint32_t g_cop_lingered, g_cop_linger_back, g_cop_linger_gone;

static int cop_car_pos(uint32_t simable, float out[3]) {
    const uint32_t st = mp_state(get_u32(simable - 0x24 + 0x70));
    for (int k = 0; st && k < 3; ++k)
        out[k] = bits_float(get_u32(st + 0x10 + 4u * k));
    return st != 0;
}

/* Whether p is in view: on this player's screen (mp.h mp_seen_at: the main view's projection, or within
 * MP_VIEW_NEAR) and within MP_VIEW_FAR (logs/no-vanish-20261106). It was "within COP_LINGER_NEAR (150 m) and within
 * 70 degrees of our car's heading": a copy leaves its owner's list at TRF_RANGE_KEEP (360 m) from the owner, so in
 * a friend's game it was nearly always farther than 150 m then, and no copy ever lingered (trucks-20261106). fwd is
 * no longer used. */
static int cop_in_view(const float here[3], const float fwd[3], const float p[3]) {
    (void)fwd;
    float len = 0;
    for (int k = 0; k < 3; ++k)
        len += (p[k] - here[k]) * (p[k] - here[k]);
    return len <= MP_VIEW_FAR * MP_VIEW_FAR && mp_seen_at(p, here);
}

static void cop_drop(CopCar *cc, const char *why);
/* A traffic copy leaving its slot: kept lingering (1) or not (0). */
static int cop_linger(CopCar *cc, const float here[3], const float fwd[3]) {
    MpCar *c = &cc->car;
    float p[3];
    if ((!(cc->flags & COP_F_TRAFFIC) && (cc->flags & COP_F_CHASING)) || !c->simable || !mp_listed(c->simable - 0x24) ||
        !cop_car_pos(c->simable, p) ||
        !cop_in_view(here, fwd, p))
        return 0;
    for (int i = 0; i < COP_LINGER; ++i) {
        CopLinger *l = &g_cop_linger[i];
        if (l->simable)
            continue;
        const uint32_t car = c->simable - 0x24, in = get_u32(car + DRV_INPUT);
        if (in && get_u32(in))
            mp_call(drv_method(in, 1), in, NULL, 0); /* IInput::ClearInput: it rolls to a stop */
        *l = (CopLinger){c->simable, c->kind, c->key, cc->built, cc->number, c->id, cc->flags, mp_now(), cc->cab, cc->cab_no};
        c->simable = 0;
        c->pending = c->holding = 0;
        ++g_cop_lingered;
        return 1;
    }
    return 0;
}

/* The lingering copy of owner's car number (model key), taken back, or 0. */
static uint32_t cop_linger_take(uint32_t owner, uint32_t number, uint32_t key, uint32_t kind) {
    for (int i = 0; i < COP_LINGER; ++i) {
        CopLinger *l = &g_cop_linger[i];
        if (l->simable && l->owner == owner && l->number == number && l->key == key && l->kind == kind &&
            mp_listed(l->simable - 0x24)) {
            const uint32_t simable = l->simable;
            l->simable = 0;
            ++g_cop_linger_back;
            return simable;
        }
    }
    return 0;
}

/* A lingering copy goes (cop_drop). */
static void cop_linger_drop(CopLinger *l, const char *why) {
    CopCar tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.car.simable = l->simable;
    tmp.car.kind = l->kind;
    tmp.car.id = l->owner;
    tmp.car.key = l->key;
    tmp.built = l->built;
    tmp.number = l->number;
    tmp.flags = l->flags;
    l->simable = 0;
    ++g_cop_linger_gone;
    cop_drop(&tmp, why);
}

/* OLDER GAMES' TRAILERS: the cab model of a trailer model (both truck models of the game, trucks-20261106), 0 = unknown. */
static uint32_t cop_cab_of(uint32_t trailer_key) {
    return trailer_key == 0xdcfbf79eu ? 0x0a92cc67u : trailer_key == 0x1caa5b90u ? 0xa9b509c4u : 0;
}
/* The squared distance between two copies' newest poses, or -1. */
static float cop_pose_d2(const MpCar *a, const MpCar *b) {
    if (!a->kept || !b->kept)
        return -1.0f;
    float d = 0;
    for (int k = 0; k < 3; ++k)
        d += (a->pose[a->kept - 1].p[k] - b->pose[b->kept - 1].p[k]) * (a->pose[a->kept - 1].p[k] - b->pose[b->kept - 1].p[k]);
    return d;
}

/* TRUCKS WHOLE: the slot of trailer slot i's cab (the same owner, the trailer's number - TRF_TRAILER_NO, flagged
 * COP_F_TOWING), or -1. A trailer numbered below TRF_TRAILER_NO comes from an older game: it goes with that owner's
 * copy of its truck's cab model (OLDER GAMES' TRAILERS) nearest to it within TRF_HITCH_NEAR. */
static int cop_cab_slot(int i) {
    const MpSlot *s = &g_cop_seen[i];
    const MpCar *a = &g_cop_cars[i].car;
    int best = -1;
    float best_d = TRF_HITCH_NEAR * TRF_HITCH_NEAR;
    for (int j = 0; j < COP_SLOTS; ++j) {
        const MpSlot *t = &g_cop_seen[j];
        if (j == i || t->id != s->id || t->mode != 1 || !t->pad[0] || !(t->pad[1] & COP_F_TRAFFIC) || (t->pad[1] & COP_F_TRAILER))
            continue;
        if (s->pad[0] & TRF_TRAILER_NO) {
            if ((t->pad[1] & COP_F_TOWING) && t->pad[0] == s->pad[0] - TRF_TRAILER_NO)
                return j;
            continue;
        }
        const MpCar *b = &g_cop_cars[j].car;
        const uint32_t cab_key = cop_cab_of(s->key);
        if (cab_key && t->key != cab_key)
            continue; /* OLDER GAMES' TRAILERS: only its own truck's cab, never a car passing by */
        const float d = cop_pose_d2(a, b);
        if (d >= 0 && d < best_d) {
            best_d = d;
            best = j;
        }
    }
    return best;
}

/* TRUCKS WHOLE: whether the cab copy cab (owner's car number no) is still here: in a slot of that car, or lingering. */
static int cop_cab_here(uint32_t owner, uint32_t cab, uint32_t no) {
    for (int j = 0; cab && j < COP_SLOTS; ++j)
        if (g_cop_cars[j].car.simable == cab && g_cop_cars[j].car.id == owner && g_cop_cars[j].number == no)
            return 1;
    for (int j = 0; cab && j < COP_LINGER; ++j)
        if (g_cop_linger[j].simable == cab && g_cop_linger[j].owner == owner && g_cop_linger[j].number == no)
            return 1;
    return 0;
}

/* TRUCKS WHOLE, at the end of every cop_cars: a trailer's copy (in a slot or lingering) whose cab's copy is no longer
 * here goes, in the same frame as its cab's. */
static uint32_t g_cop_trailers_dropped;
static void cop_trailers_follow(void) {
    for (int i = 0; i < COP_SLOTS; ++i) {
        CopCar *cc = &g_cop_cars[i];
        if (!cc->car.simable || !(cc->flags & COP_F_TRAFFIC) || !(cc->flags & COP_F_TRAILER))
            continue;
        const char *why = !cop_cab_here(cc->car.id, cc->cab, cc->cab_no) ? "its truck's copy is gone" : NULL;
        if (!why && cc->number < TRF_TRAILER_NO) {
            /* OLDER GAMES' TRAILERS: its cab copy in a slot (a lingering one stands still), and still hitched. */
            int j = 0;
            while (j < COP_SLOTS && !(g_cop_cars[j].car.simable == cc->cab && g_cop_cars[j].car.id == cc->car.id &&
                                      g_cop_cars[j].number == cc->cab_no))
                ++j;
            const float d = j < COP_SLOTS ? cop_pose_d2(&cc->car, &g_cop_cars[j].car) : -1.0f;
            if (j == COP_SLOTS)
                why = "an older game's trailer, its truck's copy is not following it";
            else if (d < 0 || d > TRF_HITCH_KEEP * TRF_HITCH_KEEP)
                why = "an older game's trailer, its truck's copy is not hitched to it";
        }
        if (why) {
            ++g_cop_trailers_dropped;
            cop_drop(cc, why);
            cc->car.wait = 0;
        }
    }
    for (int i = 0; i < COP_LINGER; ++i) {
        CopLinger *l = &g_cop_linger[i];
        if (l->simable && (l->flags & COP_F_TRAFFIC) && (l->flags & COP_F_TRAILER) && !cop_cab_here(l->owner, l->cab, l->cab_no)) {
            ++g_cop_trailers_dropped;
            cop_linger_drop(l, "lingering, its truck's copy is gone");
        }
    }
}

/* Every frame: lingering copies out of view, farther than COP_LINGER_NEAR, or too old go. */
static void cop_linger_frame(const float here[3], const float fwd[3], int have_player) {
    const double now = mp_now();
    static double said_at;
    static uint32_t said;
    if (now - said_at >= 10000.0 && g_cop_lingered + g_cop_linger_back + g_cop_linger_gone != said) {
        said_at = now;
        said = g_cop_lingered + g_cop_linger_back + g_cop_linger_gone;
        int n = 0;
        for (int i = 0; i < COP_LINGER; ++i)
            n += g_cop_linger[i].simable != 0;
        char line[160];
        snprintf(line, sizeof line, "core.nfsc: cops: traffic copies lingering %d now; %u lingered, %u taken back, %u gone so far", n,
                 g_cop_lingered, g_cop_linger_back, g_cop_linger_gone);
        log_line(line);
    }
    for (int i = 0; i < COP_LINGER; ++i) {
        CopLinger *l = &g_cop_linger[i];
        float p[3];
        if (!l->simable)
            continue;
        if (!mp_listed(l->simable - 0x24) || get_u32(l->simable - 0x24 + 0x148) != l->kind) {
            l->simable = 0; /* gone with the world */
            continue;
        }
        if (have_player && cop_car_pos(l->simable, p) && cop_in_view(here, fwd, p))
            continue; /* no time limit in view (logs/no-vanish-20261106: COP_LINGER_MS took copies away in plain view) */
        cop_linger_drop(l, now - l->since >= COP_LINGER_MS ? "lingered 30 s" : "lingered until out of view or 150 m away");
    }
}

/* Friends' cops from the page's slots: built near us, moved in mp_place_all (cop_place). */
static void cop_cars(CopShared *sh, uint32_t player) {
    const double now = mp_now();
    const uint32_t state = player ? mp_state(get_u32(player + 0x70)) : 0;
    float here[3] = {0, 0, 0};
    float fwd[3] = {0, 0, 1};
    for (int k = 0; k < 3 && state; ++k) {
        here[k] = bits_float(get_u32(state + 0x10 + 4u * k));
        fwd[k] = bits_float(get_u32(state + 0x90 + 4u * k));
    }
    cop_linger_frame(here, fwd, state != 0);
    for (int i = 0; i < COP_SLOTS; ++i) {
        MpSlot *src = cop_slot(sh, i), copy;
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
        if (c->simable && (!active || !same || c->kind != cop_kind()) &&
            !(c->kind == cop_kind() && state && !g_mp_local_paused && cop_linger(cc, here, fwd)))
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
            cc->steer = cc->gas = cc->brake = 0;
            cc->gear = 0;
            c->taken = s->count + 1; /* start over in mp_take */
            c->follow = MP_FOLLOWING;
            c->contact_at = 0;
            cc->pending = 0;
            cc->met = 0;
            cc->wreck_tried = 0;
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
        if (g_mp_local_paused)
            continue; /* as mp_remotes: no new or removed cars behind the frozen menu */
        /* TRUCKS WHOLE: a trailer's copy is built and removed with its cab's, not by its own distance from us (7 m
         * from its cab's: at MP_NEAR and MP_FAR one of the two was built or removed alone, measured 2026-11-06). */
        const int towed = (s->pad[1] & COP_F_TRAFFIC) && (s->pad[1] & COP_F_TRAILER);
        if (c->simable) { /* RADII (logs/no-vanish-20261106): as a friend's car, built within MP_BUILD_FAR, gone past MP_VIEW_FAR */
            if (d2 > MP_VIEW_FAR * MP_VIEW_FAR && !towed)
                cop_drop(cc, "far away");
            continue;
        }
        if ((d2 > MP_BUILD_FAR * MP_BUILD_FAR && !towed) || (c->wait && --c->wait))
            continue;
        if (d2 < COP_BUILD_CLEAR * COP_BUILD_CLEAR) { /* not on our car (see NEAR OUR CAR) */
            c->wait = 6;
            continue;
        }
        /* Its own model, then spooled, then a cop (or, for a traffic car, a traffic) model this game
         * has (see the header). */
        const int step = c->tries % 3, traffic = (s->pad[1] & COP_F_TRAFFIC) != 0;
        const int trailer = traffic && (s->pad[1] & COP_F_TRAILER) != 0; /* only its own model: never a car */
        /* TRUCKS WHOLE: a trailer's copy only behind its cab's, built in the cab's own model (never behind a car
         * standing in for a truck this game does not have), and then at once: not after the build gap. */
        const int cab_slot = trailer ? cop_cab_slot(i) : -1;
        if (trailer && (cab_slot < 0 || !g_cop_cars[cab_slot].car.simable || g_cop_cars[cab_slot].built != g_cop_seen[cab_slot].key))
            continue;
        const uint32_t key = step < 2 ? s->key : trailer ? 0 : traffic ? traffic_model_here(s->key) : cop_model_here(s->key);
        /* 0 = model 0 is the player's own */
        /* One build per MP_BUILD_GAP_MS, shared with friends' cars: a chase can bring eight at once. */
        if (key && now < g_mp_build_after && !cop_spare_free(key, cop_kind()) && !trailer)
            continue;
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
        cc->cab = trailer ? g_cop_cars[cab_slot].car.simable : 0; /* TRUCKS WHOLE: the cab copy it goes with */
        cc->cab_no = trailer ? g_cop_cars[cab_slot].number : 0;
        c->simable = cop_linger_take(c->id, cc->number, s->key, c->kind);
        if (c->simable) {
            cc->built = key;
            continue; /* still where it lingered: the playback moves it on */
        }
        c->simable = cop_spare_take(key, c->kind);
        if (c->simable) {
            mp_place(c->simable, last);
            cc->built = key;
            if (++g_cop_reused <= 40u || g_cop_reused % 100u == 0) {
                char line[128];
                snprintf(line, sizeof line, "core.nfsc: cops: %s %u of player %u (model %08x) took a kept copy (%u so far)",
                         trailer ? "trailer" : traffic ? "traffic car" : "cop", cc->number, c->id, key, g_cop_reused);
                log_line(line);
            }
            continue;
        }
        g_mp_build_after = now + MP_BUILD_GAP_MS;
        c->simable = mp_build(player, c->kind, key, last->p, flags, 0);
        cc->built = key;
        if (cc->round < 1 || c->simable) {
            char line[160];
            snprintf(line, sizeof line, "core.nfsc: cops: %s %u of player %u (model %08x) %s%s%s",
                     trailer ? "trailer" : traffic ? "traffic car" : "cop", cc->number, c->id, s->key, c->simable ? "built" : "refused",
                     flags & 1 ? " (spooled)" : "",
                     key == s->key ? "" : traffic ? " in a traffic model this game has (its own is not here)"
                                                  : " in a cop model this game has (its own is not here)");
            log_line(line);
        }
        if (!c->simable) {
            c->wait = step == 2 ? COP_RETRY : 30;
            if (step == 2)
                ++cc->round;
        }
    }
    cop_trailers_follow(); /* TRUCKS WHOLE */
}

/* mp_drive.h's g_drv_cop: what a driver must be doing to make a copy move as it does at playback
 * time play. A positive turn rate about the up axis goes with positive (right) steering when
 * driving forward: 53 of 54 samples of a player's car turning at speed, 2026-11-04. */
static int cop_forward(const float q[4], float out[2]);
static int cop_drive(MpCar *c, double play, DrvCtl *out) {
    CopCar *cc = cop_car(c);
    float f[2];
    MpPose a, b;
    if (!cc || c->kept < 2)
        return 0;
    mp_sample(c, play, &a);
    mp_sample(c, play - COP_DRV_SPAN, &b);
    if (!cop_forward(a.q, f))
        return 0;
    const float along = a.v[0] * f[0] + a.v[2] * f[1], before = b.v[0] * f[0] + b.v[2] * f[1];
    const float speed = sqrtf(a.v[0] * a.v[0] + a.v[2] * a.v[2]);
    const float accel = (fabsf(along) - fabsf(before)) / (float)(COP_DRV_SPAN / 1000.0);
    const float dir = along < 0 ? -1.0f : 1.0f, yaw = a.w[1];
    /* Sideways force at speed; the wheels' geometry when slow. */
    float steer = dir * yaw * speed / COP_DRV_LATERAL;
    if (speed < 10.0f) {
        const float low = dir * yaw * COP_DRV_WHEELBASE / (speed > 2.0f ? speed : 2.0f) / 0.6f;
        if (fabsf(low) > fabsf(steer))
            steer = low;
    }
    const int moving = speed > 0.5f;
    float gas = moving ? 0.35f + accel / 6.0f : 0.0f, brake = accel < -4.0f ? -accel / 14.0f : 0.0f;
    gas = brake > 0.0f ? 0.0f : gas;
    steer = !isfinite(steer) ? 0.0f : steer < -1.0f ? -1.0f : steer > 1.0f ? 1.0f : steer;
    cc->steer += (steer - cc->steer) * COP_DRV_SMOOTH;
    cc->gas += (drv_unit(gas) - cc->gas) * COP_DRV_SMOOTH;
    cc->brake += (drv_unit(brake) - cc->brake) * COP_DRV_SMOOTH;
    /* A gear for the speed, up at the top of its range and down well below it. */
    int gear = cc->gear < 2 ? 2 : cc->gear;
    const float top = (float)(gear - 1) * COP_DRV_GEAR;
    if (speed > top)
        ++gear;
    else if (gear > 2 && speed < top - COP_DRV_GEAR - 4.0f)
        --gear;
    cc->gear = gear;
    memset(out, 0, sizeof *out);
    out->steer = cc->steer;
    out->gas = cc->gas;
    out->brake = cc->brake;
    out->gear = along < -1.0f ? 0 : gear;
    return 1; /* rpm 0: its own engine works its revs out from the gas and gear */
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
    /* Two replayed cars (a friend's, a copy of a friend's cop) follow their owners' poses, and the
     * owners' games have already settled how they touch; solved again here, a cop pressing on the
     * car it chases was thrown off it and put back, over and over (see WATCHING A CHASE). Traffic
     * and native racers belong to this city alone, so a replayed car cannot have hit them in its
     * owner's game. Reject before contact separation, not just in the later impulse solver. This
     * game's own cops stay interactive. */
    MpCar *ra = oa ? mp_remote_owner(oa) : NULL;
    MpCar *rb = ob ? mp_remote_owner(ob) : NULL;
    const uint32_t local = ra && !rb ? ob : rb && !ra ? oa : 0;
    const uint32_t cls = local && mp_listed(local - 0x24) ? get_u32(local - 0x24 + 0x148) : 0;
    if ((ra && rb) || (cls == 1u || cls == 3u)) {
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
    for (int guard = 0; node && node != MP_CARS && guard < 256; ++guard, node = get_u32(node)) {
        const uint32_t car = node - 0xd0, simable = car + 0x24, state = mp_state(get_u32(car + 0x70));
        if (!state || simable == g_mp_player_simable || cop_car_of(simable) || cop_spare_of(simable))
            continue;
        const uint32_t cls = get_u32(car + 0x148);
        if (mp_remote_owner(simable) || cls == 1u || cls == 3u)
            continue; /* passes through the copies (cop_contact): not near anything */
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
    if (owner && !g_cop_wrecking && (cop_car_of(owner) || cop_spare_of(owner)))
        api->hook_return(api, cpu, cpu->eax, 0);
    else
        api->call_original(api, cpu->target, cpu);
}

/* WRECKED COPS: each copy wrecked while its cop is wrecked in its owner's game, as new when not. */
static void cop_wrecks(void) {
    for (int i = 0; i < COP_SLOTS; ++i) {
        CopCar *cc = &g_cop_cars[i];
        const uint32_t simable = cc->car.simable, car = simable ? simable - 0x24 : 0;
        const uint32_t damageable = car ? get_u32(car + 0xfc) : 0;
        if (!damageable || !cc->car.id)
            continue;
        const int want = (cc->flags & COP_F_WRECKED) != 0, is = cop_wrecked(car);
        if (want ? is || cc->wreck_tried : !is)
            continue;
        cc->wreck_tried = want;
        if (want) {
            g_cop_wrecking = 1;
            mp_call(COP_DESTROY, damageable, NULL, 0);
            g_cop_wrecking = 0;
        } else {
            mp_call(COP_RESET, damageable, NULL, 0);
        }
        char line[128];
        snprintf(line, sizeof line, "core.nfsc: cops: copy of cop %u of player %u %s", cc->number, cc->car.id,
                 !want ? "made as new" : cop_wrecked(car) ? "wrecked, as in its owner's game" : "not wrecked (Destroy refused)");
        log_line(line);
    }
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
    cop_traffic_owner(__atomic_load_n(&sh->page, __ATOMIC_ACQUIRE) ? player : 0);
    cop_publish(sh, player);
    cop_received(sh, player);
    cop_cars(sh, player);
    cop_wrecks();
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
    sh->more_magic = COP_MORE_MAGIC;
    sh->more_out = COP_MAX;
    sh->more_slots = COP_SLOTS;
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
    g_drv_cop = cop_drive;
    g_mp_cop_place = cop_place;
    g_mp_cop_touch = cop_touch;
    g_mp_cop_pair = cop_pair;
    g_mp_follow_target = cop_keep_off;
    /* FRIENDS COUNT AS NEAR: three call-site hooks (see SHARED PATROLS). */
    int near_hooks = 0;
    if (g_api->hook_install_at_callsite && g_api->guest_alloc(g_api, 12, &g_trf_near_buf) == POP_OK) {
        for (int i = 0; i < 3; ++i)
            near_hooks += g_api->hook_install_at_callsite(g_api, TRF_DISTANCE, g_trf_near_ret[i], cop_near_friends,
                                                          POP_HOOK_BEFORE, NULL, &g_trf_near_hooks[i]) == POP_OK;
    } else {
        g_trf_near_buf = 0;
    }
    if (near_hooks != 3) {
        char line[96];
        snprintf(line, sizeof line, "core.nfsc: traffic: friends count as near at %d of 3 places", near_hooks);
        log_line(line);
    }
    if (g_api->hook_install(g_api, TRF_SPAWN_COP, cop_no_spawn, POP_HOOK_REPLACE, NULL, &g_trf_spawn_hook) != POP_OK) {
        g_trf_spawn_hook = 0;
        log_line("core.nfsc: police: shared patrols unavailable (could not hook SpawnCop)");
    }
    if (g_api->hook_install(g_api, TRF_MAKE_COP, cop_no_make, POP_HOOK_REPLACE, NULL, &g_trf_make_hook) != POP_OK) {
        g_trf_make_hook = 0;
        log_line("core.nfsc: police: shared patrols unavailable (could not hook the cop factory)");
    }
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
