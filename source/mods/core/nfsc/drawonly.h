/* drawonly.h - draw-only far cars (level L2 of plans/many-cars-design.md), included by nfsc.c after mp.h,
 * look.h, paint.h and carlimit.h. Research: logs/drawonly-cars-20261106/RESULT.md. Merged on top of the no-vanish mp.h
 * (LINGERING CARS, RADII) in logs/drawonly-merge-20261106: mp.h's DRAW-ONLY CARS gives the radii while this is on
 * (full cars within MP_L0_BUILD, handed down beyond MP_L0_KEEP, draw-only cars out to MP_VIEW_FAR); a full car that
 * lingers in view (mp.h) keeps its twin hidden; a draw-only car whose friend left the slots stays while in view
 * within MP_VIEW_FAR (no time limit, as mp.h's lingering cars), and goes once off screen.
 *
 * WHAT DRAWS A CAR IN CARBON. The renderer does not walk vehicles: it walks the render connections of
 * the list at 0x00b77db0 (begin +4, count +0xc) and calls each one's OnRender (vtable +0xc, 007e1160)
 * per view; their Update (vtable +0x10, 007d62d0) runs once a frame from 007bf5f0. A car's render
 * connection is a CarRenderConn (0x1c0 bytes, vtable 0x009f25ec, constructor 007e66c0 (this,
 * ConnectionData *, car type index, Pkt_Car_Open *), deleting destructor 007e48e0 (this, 1)):
 *   +0x08 the sim side's service object (Sim::Connection, 00762690 copies ConnectionData +0);
 *   +0x24 state: 1 its car is loading (007e6410 makes the CarRenderInfo when the car loader has it),
 *         2 ready, 3 drawn (OnRender draws only 3); +0x60 byte 1 = hidden (007c20f0(1));
 *   +0x28 car type index; +0x2c a WorldConn reference {id, matrix *, velocity *, +0x50 *} (00815970 /
 *         008159f0: the WorldConn server [0x00b77e98] 00814280 finds or makes a 0x70-byte entry for the
 *         id: +0 the world matrix in render space, +0x40 the velocity); the car is drawn where that
 *         entry says (OnRender, 007d60e0 reads [+0x30] and [+0x34]);
 *   +0x3c RideInfo (0xbf0; 007e6580: 007da590 init, 007c2a10 its paint id, 007dbe50 the owner's look,
 *         007e6500 asks the car loader 0x00b77018 for it); +0x40 CarRenderInfo (0x3260, 007e55e0);
 *   +0x68 composite paint id (mask 0x00a732a4, paint.h 31-72); +0x110 13 matrices (body, 4 wheels,
 *         4 brakes, 4 more) that the update 007d60e0 makes from the entry and the service packet.
 * Its Update asks the sim side for a Pkt_Car_Service (007bd680 makes one on the stack; Service
 * 0075e1a0 calls [[conn+8]] vtable[0](conn, pkt)); with no answer the car is hidden (007c20f0(1)),
 * with one 007d60e0 builds the matrices and sets state 3. A PVehicle's behaviour (00708820) opens this
 * connection with a Pkt_Car_Open (0x1c, 006dd720: +0 vtable 0x009e5504, +4 type name hash, +8 world id,
 * +0xc kind (0 the player, 1 a racer/friend, 4 traffic as read from running cars' +0x1a4), +0x10
 * VehicleCustomizations * (0 stock), +0x14 model key, +0x18 byte (0: 007e6500 queues the event that kicks the loader),
 * +0x19 lite (RideInfo +6: lite LOD ranges and DUMMY_SKIN_LITE)).
 *
 * A DRAW-ONLY CAR here is that same CarRenderConn with no vehicle behind it: made directly with the
 * constructor, a world id of our own (so its WorldConn entry is ours alone, written every frame from
 * the friend's interpolated pose), and a service object whose vtable[0] is a game function that only
 * returns true (00561630: mov al,1 / ret 8; 00405530 returns false and hides it). The service packet
 * then keeps the defaults of its constructor (wheels at rest, lights off). No PVehicle, no rigid body,
 * no car-list entry, no AI, no sound connection, no collision. It still takes: a render-list entry,
 * a RideInfo registry entry (128 with the bodies patch), a car-loader slot (128), a paint id (72 with
 * paint.h) and its model, textures and paint in the Cars pool.
 *
 * LEVELS. mp.h decides who gets a full car (L0: within MP_NEAR, the nearest first, up to "Players' cars
 * shown"); every other friend in the slots within MP_FAR had only a name tag (tag_only). Here such a
 * friend gets a draw-only car (L2), up to DO_BUDGET of them, nearest first, at most one built a frame.
 * NO POP: L2 -> L0: the draw-only car stays until the full car's own render connection is drawn (state 3,
 * not hidden), then goes. L0 -> L2: a full car farther than DO_SHADOW_M (and the two farthest) gets a
 * hidden draw-only twin at its pose, shown the moment the full car is parked, removed or hidden.
 * A friend that leaves the slots keeps its car, still moving on its last speed for DO_COAST_MS, and
 * it is removed only once off screen (or after DO_LINGER_MS). Over the budget, the farthest off-screen
 * car goes first. Every removal while on screen, of either level, is counted (the sampler).
 * L3 (low detail): RECOMP_DO_LITE=<m>: cars farther than that are built lite (RideInfo +6).
 *
 * Test switches (URL): RECOMP_DRAWONLY=0 off; RECOMP_DO_BUDGET=<n>; RECOMP_DO_TEST=<n>: n draw-only copies
 * of this player's car in a grid ahead (no network); RECOMP_DO_AB=<s>: hide/show every L2 car by turns
 * every s seconds (A/B cost); RECOMP_DO_LITE=<m>; RECOMP_DO_SHADOW=<m>; RECOMP_DO_LOD=<n> force LOD >= n.
 */

#define DO_MAX 112
#define DO_CONN_SIZE 0x1c0u
#define DO_SMALL_HEAP 0x00a99720u
#define DO_SMALL_ALLOC 0x0060ba70u /* thiscall (heap, size, 0) */
#define DO_CONN_CTOR 0x007e66c0u
#define DO_CONN_DTOR 0x007e48e0u   /* thiscall (conn, 1) */
#define DO_TYPE_INDEX 0x007b0240u  /* thiscall (0x00b76690, type name hash) -> index or -1 */
#define DO_PART_DB 0x00b76690u
#define DO_ATTR 0x00412520u        /* thiscall (Attrib instance, model key, 0) */
#define DO_ATTR_FREE 0x00469870u   /* thiscall (Attrib instance) */
#define DO_PKT_VT 0x009e5504u
#define DO_SERVICE_TRUE 0x00561630u
#define DO_SERVICE_FALSE 0x00405530u
#define DO_RENDER_LIST 0x00b77db0u
#define DO_CONN_VT 0x009f25ecu
#define DO_ON_RENDER 0x007e1160u
#define DO_UPDATE_ALL 0x007bf5f0u
#define DO_FLOW 0x00a99bbcu        /* 6: in the world */
#define DO_WORLD_ID0 0x7d000000u   /* our WorldConn ids; the game's start at 0x01000001 */
#define DO_RIDES_MAX 118u          /* render connections in all: the loader pool and ride registry hold 128 */
#define DO_SHADOW_M 250.0f
/* THE CAR LOADER'S RIDE SLOTS. Every car the game draws holds one CarLoadedRideInfoSlotPool entry (0x117c bytes;
 * pool [0x00b74d04]: +0x18 in use, +0x2c entries; 007b1710 makes 60, the bodies patch 128; flags 0: it never grows).
 * With none free 007e44c0 waits for ever (the game freezes; logs/step-b-20261105 "60-car wall", and here run cyc2
 * at 128). And the loader's next-load step 007e4bb0 starts unloading (007e46c0) when fewer than 30 are free (0x1e,
 * the game's 30-car world). So a draw-only car is made only while more than DO_RIDES_FREE stay free.
 * MERGE (logs/drawonly-merge-20261106, runs/spread40): the pool's "in use" (+0x18) also counts rides the loader keeps
 * loaded for cars that are gone (it unloads them only below 30 free): it climbed to 95-99 of 128 with 45-48 render
 * connections alive, and from then on no draw-only car (and no twin) was made at all. Every car the game draws holds
 * one render connection, so the live count is the render list's: a draw-only car is made while the render list
 * leaves more than DO_RIDES_FREE rides of the pool (and, as before, the pool itself is not completely full). */
#define DO_RIDE_POOL 0x00b74d04u
#define DO_RIDES_FREE 34u
#define DO_LOADING_MAX 6 /* draw-only cars waiting for the loader at once: the near full cars come first */
#define DO_COAST_MS 1500.0
#define DO_LINGER_MS 20000.0 /* beyond the far radius only: in view out there it may stay this long */
#define DO_FAR MP_VIEW_FAR   /* metres: draw-only cars out to this (mp.h tag_only with draw-only cars on) */
#define DO_NOTE_MS 5000.0

enum { DO_FREE, DO_LIVE };
typedef struct DoCar {
    int used;
    uint32_t conn, mem;  /* the connection; guest block: pkt +0 (0x20), data +0x20 (8), look +0x40 */
    uint32_t id, key, type;
    uint32_t look_stamp; /* the owner's look it was made with (look.h peer stamp; 0 stock) */
    int slot;            /* mp slot this frame, -1 none */
    int shown;           /* service object: shown (true) or hidden */
    int want;            /* this frame: 0 remove, 1 shown, 2 hidden twin */
    int lite;
    double born, heard;  /* our ms: made, last pose */
    double drawn_at;     /* first frame seen in state 3 */
    double handing_at;   /* our ms: mp.h asked to take its full car away; the twin is shown first (do_demote_ready) */
    MpPose pose;
    int have;
    int onscreen;
    float d;             /* metres from our car */
} DoCar;

static DoCar g_do[DO_MAX];
static int g_do_on = 1, g_do_budget = 64, g_do_test, g_do_ab, g_do_lod = -1;
static float g_do_l3_m; /* RECOMP_DO_L3: our cars farther than this draw with the lowlod technique (flags 0x400) at LOD g_do_l3_lod */
static int g_do_l3_lod = 2;
static float g_do_lite_m, g_do_shadow_m = DO_SHADOW_M;
static uint32_t g_do_handler_show, g_do_handler_hide, g_do_stats_block, g_do_scratch;
static uint32_t g_do_next_id;
static int g_do_world; /* frames in the world with a player car */
static int g_do_ab_hidden;
static int g_do_ab_l3; /* RECOMP_DO_AB_L3=1: the A/B phases switch L3 drawing on and off instead of hiding the cars */
/* the render list this frame */
static uint32_t g_do_list[160], g_do_list_n, g_do_list_state[160], g_do_list_hidden[160];
/* per slot: the full car's connection (0 none or not found) and whether it is drawn */
static uint32_t g_do_full_conn[MP_SLOTS];
static int g_do_full_drawn[MP_SLOTS], g_do_full_onscreen[MP_SLOTS];
static uint32_t g_do_full_id[MP_SLOTS];
static double g_do_urgent[MP_SLOTS]; /* our ms: mp.h asked to take this slot's full car away (twin first) */
static int g_do_seen_drawn[MP_SLOTS]; /* the sampler: this friend was drawn and on screen last frame */
static uint32_t g_do_seen_id[MP_SLOTS];
static float g_do_seen_d[MP_SLOTS];
/* type index cache */
static struct { uint32_t key; int type; } g_do_types[64];
static int g_do_ntypes;

typedef struct DoStats { /* exported to the page as globalThis.carbonDrawOnly {memory, ptr}; read by doprobe.js */
    uint32_t magic, frame;
    uint32_t live, shown, drawn, loading, twins, full_drawn, slots_active, budget;
    uint32_t created, removed, removed_onscreen, vanished_onscreen, refused, ab_hidden;
    float fps, frame_ms_p95;
    float us_update_all, us_render_ours, us_render_full, us_render_other, us_manage; /* per frame, 5 s means */
    uint32_t renders_ours, renders_full, renders_other; /* OnRender calls per frame (5 s means x 1) */
    uint32_t list_n, pool_free_kb;
    float near_l2_m, far_l2_m;
    uint32_t view;          /* guest address of the main view's matrix block (+0x80 view-projection), for page samplers */
    float us_main_loop;     /* MainLoop 00761150 (physics, AI, the world's simulation) per frame */
    uint32_t demote_waits;  /* frames mp.h waited for a twin before taking a full car away */
    uint32_t rides_used, rides_total; /* CarLoadedRideInfoSlotPool ([0x00b74d04] +0x18 / +0x2c) */
} DoStats;
static DoStats g_do_s;
_Static_assert(0x20 + sizeof(DoStats) <= 0x400, "drawonly stats block");

static double g_do_t_main, g_do_main_t0;
static double g_do_t_update, g_do_t_ours, g_do_t_full, g_do_t_other, g_do_t_manage;
static uint32_t g_do_n_ours, g_do_n_full, g_do_n_other, g_do_frames_acc;
static double g_do_render_t0, g_do_update_t0;
static int g_do_render_class;
static double g_do_last_frame_at, g_do_note_at;
static float g_do_ft[512];
static int g_do_ft_n;

static double do_now(void) { return mp_now(); }

static uint32_t do_call(uint32_t fn, uint32_t ecx, const uint32_t *args, uint32_t n) {
    uint32_t eax = 0;
    if (g_api->guest_call(g_api, fn, ecx, args, n, &eax) != POP_OK)
        return 0;
    return eax;
}

static DoCar *do_by_conn(uint32_t conn) {
    for (int i = 0; i < DO_MAX; ++i)
        if (g_do[i].used && g_do[i].conn == conn)
            return &g_do[i];
    return NULL;
}
static DoCar *do_by_id(uint32_t id) {
    for (int i = 0; i < DO_MAX; ++i)
        if (g_do[i].used && g_do[i].id == id)
            return &g_do[i];
    return NULL;
}

/* The car type index of a model key: its Attrib layout's +0x20 is the type's name ("RX8"); the part
 * database finds the index by its bStringHash, as the factory 007e6fc0 does. -1: unknown. */
static int do_type_of(uint32_t key) {
    for (int i = 0; i < g_do_ntypes; ++i)
        if (g_do_types[i].key == key)
            return g_do_types[i].type;
    int type = -1;
    uint32_t inst = g_do_scratch; /* 0x10 bytes for the Attrib instance */
    const uint32_t a[2] = {key, 0};
    do_call(DO_ATTR, inst, a, 2);
    const uint32_t layout = get_u32(inst + 4), name = layout ? get_u32(layout + 0x20) : 0;
    if (name) {
        char s[40];
        int n = 0;
        for (; n < 39; ++n) {
            uint8_t ch = 0;
            g_api->guest_read_u8(g_api, name + (uint32_t)n, &ch);
            if (!ch)
                break;
            s[n] = (char)ch;
        }
        s[n] = 0;
        const uint32_t h = paint_hash(s), b[1] = {h};
        const uint32_t idx = do_call(DO_TYPE_INDEX, DO_PART_DB, b, 1);
        type = (int32_t)idx >= 0 && idx < 0x5f ? (int)idx : -1;
    }
    do_call(DO_ATTR_FREE, inst, NULL, 0);
    if (g_do_ntypes < 64) {
        g_do_types[g_do_ntypes].key = key;
        g_do_types[g_do_ntypes].type = type;
        ++g_do_ntypes;
    }
    return type;
}

/* The friend's look as mp.h's builder would get it, without look.h's per-slot bookkeeping
 * (look_for marks the slot's full car as built with that look). 0: stock. */
#define DO_NEW_LOOK 0x80000000u /* id bit: a hidden replacement in the owner's newer look, swapped in once loaded */
static uint32_t do_look_stamp(uint32_t id, uint32_t key) {
    LookShared *sh = look_shared();
    LookPeer peer;
    return sh && key && look_peer(sh, id, key, &peer) && peer.size ? peer.stamp : 0;
}
static uint32_t do_look(uint32_t id, uint32_t key, uint32_t dst) {
    LookShared *sh = look_shared();
    LookPeer peer;
    if (!sh || !key || !look_peer(sh, id, key, &peer) || !peer.size)
        return 0;
    const LookRecord *r = (const LookRecord *)peer.rec;
    uint8_t *out = NULL;
    if (look_valid(r, peer.size) < 0 || g_api->guest_ptr(g_api, dst, LOOK_BYTES, (void **)&out) != POP_OK || !out)
        return 0;
    memcpy(out, r->bytes, LOOK_BYTES);
    memset(out + 0xf8, 0, 0x38);
    return dst;
}

/* sim space (x, y, z) -> render space (z, -x, y) */
static void do_render_vec(const float v[3], float out[3]) {
    out[0] = v[2];
    out[1] = -v[0];
    out[2] = v[1];
}

/* The pose into the car's WorldConn entry: rows = forward, left, up, position (render space). */
static void do_write(DoCar *d) {
    if (!d->conn || !d->have)
        return;
    const uint32_t e = get_u32(d->conn + 0x30), vel = get_u32(d->conn + 0x34);
    if (!e)
        return;
    static const float ez[3] = {0, 0, 1}, ex[3] = {-1, 0, 0}, ey[3] = {0, 1, 0};
    float r[3], m[16];
    mp_rotate(d->pose.q, ez, r);
    do_render_vec(r, m + 0);
    mp_rotate(d->pose.q, ex, r);
    do_render_vec(r, m + 4);
    mp_rotate(d->pose.q, ey, r);
    do_render_vec(r, m + 8);
    do_render_vec(d->pose.p, m + 12);
    m[3] = m[7] = m[11] = 0;
    m[15] = 1;
    uint32_t *g = NULL;
    if (g_api->guest_ptr(g_api, e, 64, (void **)&g) == POP_OK && g)
        memcpy(g, m, 64);
    if (vel) {
        float v[3];
        do_render_vec(d->pose.v, v);
        for (int k = 0; k < 3; ++k)
            put_float(vel + 4u * k, v[k]);
    }
}

static float do_dist(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static int do_on_screen(const float p[3]) {
    if (!g_mp_view)
        return 0;
    float s[2];
    const float w = mp_project(g_mp_view + 0x80, p, 1, s);
    return w > 0.5f && w < 1500.0f && s[0] > -0.05f && s[0] < 1.05f && s[1] > -0.05f && s[1] < 1.05f;
}

static int do_in_list(uint32_t conn) {
    for (uint32_t i = 0; i < g_do_list_n; ++i)
        if (g_do_list[i] == conn)
            return 1;
    return 0;
}

static void do_set_shown(DoCar *d, int shown) {
    if (!d->conn || d->shown == shown)
        return;
    d->shown = shown;
    g_api->guest_write_u32(g_api, d->conn + 8, shown ? g_do_handler_show : g_do_handler_hide);
}

static uint32_t g_do_logged;
static void do_log(const char *s) {
    if (++g_do_logged < 400u || g_do_logged % 200u == 0)
        log_line(s);
}

/* Makes the draw-only car (after mp.h's builder: same model, same look, never another model). */
static int do_make(DoCar *d, uint32_t id, uint32_t key, int lite, const MpPose *pose) {
    const int type = do_type_of(key);
    if (type < 0) {
        ++g_do_s.refused;
        return 0;
    }
    uint32_t mem = 0;
    if (g_api->guest_alloc(g_api, 0x40 + LOOK_BYTES + 0x10, &mem) != POP_OK || !mem)
        return 0;
    const uint32_t pkt = mem, data = mem + 0x20, look = mem + 0x40;
    const uint32_t cust = do_look(id & ~DO_NEW_LOOK, key, look), stamp = cust ? do_look_stamp(id & ~DO_NEW_LOOK, key) : 0;
    const uint32_t wid = DO_WORLD_ID0 + (++g_do_next_id);
    /* +0x18 = 0: 007e6500 then queues the 8-byte event (00561590, vtable 0x009e140c, handler 0066c7a0) that kicks the
     * car loader (007e6e70 -> 007e4bb0). With 1 our load waited until something else kicked it: 70 s for one car
     * with its owner's look in run drive-l2b (do7). */
    const uint32_t P[8] = {DO_PKT_VT, 0, wid, 1u, cust, key, (lite ? 1u : 0u) << 8, 0};
    for (int k = 0; k < 8; ++k)
        g_api->guest_write_u32(g_api, pkt + 4u * k, P[k]);
    g_api->guest_write_u32(g_api, data, g_do_handler_hide); /* hidden until its first pose is written */
    g_api->guest_write_u32(g_api, data + 4, pkt);
    const uint32_t a[2] = {DO_CONN_SIZE, 0};
    const uint32_t conn = do_call(DO_SMALL_ALLOC, DO_SMALL_HEAP, a, 2);
    if (!conn) {
        g_api->guest_free(g_api, mem);
        return 0;
    }
    const uint32_t c[3] = {data, (uint32_t)type, pkt};
    do_call(DO_CONN_CTOR, conn, c, 3);
    memset(d, 0, sizeof *d);
    d->used = 1;
    d->conn = conn;
    d->mem = mem;
    d->id = id;
    d->key = key;
    d->type = (uint32_t)type;
    d->lite = lite;
    d->look_stamp = stamp;
    d->slot = -1;
    d->shown = 0;
    d->born = d->heard = do_now();
    if (pose) {
        d->pose = *pose;
        d->have = 1;
        do_write(d);
    }
    if (g_do_lod >= 0) { /* nothing to do until its CarRenderInfo exists (do_frame) */ }
    ++g_do_s.created;
    char line[160];
    snprintf(line, sizeof line, "core.nfsc: drawonly car of player %u (model %08x, type %d%s%s) made: conn %08x, world id %08x", id, key,
             type, cust ? ", owner's look" : ", stock look", lite ? ", lite" : "", conn, wid);
    do_log(line);
    return 1;
}

static void do_free(DoCar *d, const char *why, int onscreen) {
    if (!d->used)
        return;
    if (d->conn && do_in_list(d->conn)) {
        const uint32_t a[1] = {1};
        do_call(DO_CONN_DTOR, d->conn, a, 1);
    }
    if (d->mem)
        g_api->guest_free(g_api, d->mem);
    ++g_do_s.removed;
    if (onscreen)
        ++g_do_s.removed_onscreen;
    char line[160];
    snprintf(line, sizeof line, "core.nfsc: drawonly car of player %u removed (%s%s), %.0f m", d->id, why, onscreen ? ", ON SCREEN" : "", d->d);
    do_log(line);
    memset(d, 0, sizeof *d);
}

static void do_free_all(const char *why) {
    for (int i = 0; i < DO_MAX; ++i)
        if (g_do[i].used)
            do_free(&g_do[i], why, 0);
}

/* mp.h: a friend without a full car, placed where its name tag goes (mp_place_all, twice a frame). */
static void do_tag_pose(int slot, const MpCar *c, const MpPose *pose) {
    DoCar *d = do_by_id(c->id);
    if (!d || (d->slot >= 0 && d->slot != slot))
        return; /* only the slot this friend has now */
    d->pose = *pose;
    d->have = 1;
    d->heard = do_now();
    do_write(d);
}
/* mp.h: a full car's pose, for its hidden twin. */
static void do_car_pose(int slot, const MpCar *c, const MpPose *pose) {
    do_tag_pose(slot, c, pose);
}

/* The render list now; and each full friend's car's connection, found by position (the WorldConn entry
 * of a PVehicle holds its body's matrix, render space). */
static void do_read_list(void) {
    const uint32_t list = get_u32(DO_RENDER_LIST + 4), n = get_u32(DO_RENDER_LIST + 0xc);
    g_do_list_n = 0;
    for (uint32_t i = 0; list && i < n && g_do_list_n < 160; ++i) {
        const uint32_t conn = get_u32(list + 4u * i);
        if (!conn)
            continue;
        g_do_list[g_do_list_n] = conn;
        g_do_list_state[g_do_list_n] = get_u32(conn + 0x24);
        uint8_t h = 0;
        g_api->guest_read_u8(g_api, conn + 0x60, &h);
        g_do_list_hidden[g_do_list_n] = h;
        ++g_do_list_n;
    }
}

static uint32_t do_conn_of_car(uint32_t simable, int slot) {
    const uint32_t st = mp_state(get_u32(simable - 0x24 + 0x70));
    if (!st)
        return 0;
    float p[3], rp[3], v2 = 0;
    for (int k = 0; k < 3; ++k) {
        p[k] = bits_float(get_u32(st + 0x10 + 4u * k));
        const float vk = bits_float(get_u32(st + 0x20 + 4u * k));
        v2 += vk * vk;
    }
    do_render_vec(p, rp);
    /* The connection's WorldConn entry is written from the body once a frame, so a moving car's entry trails its body by
     * up to a frame or two: 0.5 m plus 3 frames at its speed (logs/drawonly-merge-20261106: with 0.5 m alone a bot at 35 m/s
     * was "not drawn" every other frame and its hidden twin was shown over it). Its own connection, once found, is kept
     * while it is within twice that. */
    const float tol = 0.5f + sqrtf(v2) * 0.05f;
    const uint32_t cached = g_do_full_conn[slot];
    for (int pass = 0; pass < 2; ++pass)
        for (uint32_t i = 0; i < g_do_list_n; ++i) {
            const uint32_t conn = g_do_list[i];
            if ((pass == 0) != (conn == cached) || do_by_conn(conn))
                continue;
            const uint32_t e = get_u32(conn + 0x30);
            if (!e)
                continue;
            float dd = 0;
            for (int k = 0; k < 3; ++k) {
                const float x = bits_float(get_u32(e + 0x30 + 4u * k)) - rp[k];
                dd += x * x;
            }
            const float t = pass == 0 ? 2.0f * tol : tol;
            if (dd < t * t)
                return conn;
        }
    return 0;
}

/* mp.h's LINGERING CARS: a full car of player id that left the slots and is still shown in view. */
static int do_full_lingers(uint32_t id) {
    for (int i = 0; id && i < MP_LINGER; ++i)
        if (g_mp_linger[i].car.simable && g_mp_linger[i].car.id == id)
            return 1;
    return 0;
}

static int do_list_drawn(uint32_t conn) {
    for (uint32_t i = 0; i < g_do_list_n; ++i)
        if (g_do_list[i] == conn)
            return g_do_list_state[i] == 3 && !g_do_list_hidden[i];
    return 0;
}

static void do_note(double now) {
    if (now - g_do_note_at < DO_NOTE_MS)
        return;
    const double frames = g_do_frames_acc ? (double)g_do_frames_acc : 1.0;
    g_do_s.us_update_all = (float)(g_do_t_update * 1000.0 / frames);
    g_do_s.us_render_ours = (float)(g_do_t_ours * 1000.0 / frames);
    g_do_s.us_render_full = (float)(g_do_t_full * 1000.0 / frames);
    g_do_s.us_render_other = (float)(g_do_t_other * 1000.0 / frames);
    g_do_s.us_manage = (float)(g_do_t_manage * 1000.0 / frames);
    g_do_s.us_main_loop = (float)(g_do_t_main * 1000.0 / frames);
    {
        uint32_t fk = 0, bk = 0;
        cl_pool_real(&fk, &bk);
        g_do_s.pool_free_kb = fk;
    }
    g_do_s.renders_ours = (uint32_t)(g_do_n_ours / frames + 0.5);
    g_do_s.renders_full = (uint32_t)(g_do_n_full / frames + 0.5);
    g_do_s.renders_other = (uint32_t)(g_do_n_other / frames + 0.5);
    g_do_s.fps = (float)(frames * 1000.0 / (now - g_do_note_at));
    /* p95 frame time */
    float sorted[512];
    const int n = g_do_ft_n < 512 ? g_do_ft_n : 512;
    memcpy(sorted, g_do_ft, sizeof(float) * (size_t)n);
    for (int i = 1; i < n; ++i) { /* insertion sort, n <= 512 */
        const float v = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > v) { sorted[j + 1] = sorted[j]; --j; }
        sorted[j + 1] = v;
    }
    g_do_s.frame_ms_p95 = n ? sorted[(int)(n * 0.95f)] : 0;
    g_do_ft_n = 0;
    if (g_do_note_at > 0) {
        char line[512];
        snprintf(line, sizeof line,
                 "core.nfsc: drawonly (5 s): %.1f fps, p95 %.1f ms; L2 live %u (shown %u, drawn %u, loading %u, twins %u), L0 drawn %u, friends %u, budget %u; "
                 "made %u, removed %u, removed on screen %u, vanished on screen %u, refused %u; per frame: conn updates %.0f us, OnRender L2 %.0f us (%u calls) "
                 "L0 %.0f us (%u) other %.0f us (%u), manager %.0f us, main loop %.0f us; Cars pool free %u KB; list %u; rides %u/%u; demote waits %u%s",
                 g_do_s.fps, g_do_s.frame_ms_p95, g_do_s.live, g_do_s.shown, g_do_s.drawn, g_do_s.loading, g_do_s.twins, g_do_s.full_drawn,
                 g_do_s.slots_active, g_do_s.budget, g_do_s.created, g_do_s.removed, g_do_s.removed_onscreen, g_do_s.vanished_onscreen,
                 g_do_s.refused, g_do_s.us_update_all, g_do_s.us_render_ours, g_do_s.renders_ours, g_do_s.us_render_full, g_do_s.renders_full,
                 g_do_s.us_render_other, g_do_s.renders_other, g_do_s.us_manage, g_do_s.us_main_loop, g_do_s.pool_free_kb, g_do_s.list_n,
                 g_do_s.rides_used, g_do_s.rides_total, g_do_s.demote_waits, g_do_ab_hidden ? (g_do_ab_l3 ? ", A/B: L2 HIDDEN (L3 on)" : ", A/B: L2 HIDDEN") : "");
        log_line(line);
    }
    g_do_note_at = now;
    g_do_t_update = g_do_t_ours = g_do_t_full = g_do_t_other = g_do_t_manage = g_do_t_main = 0;
    g_do_n_ours = g_do_n_full = g_do_n_other = 0;
    g_do_frames_acc = 0;
}

/* Test copies: n of this player's car in a grid 20 m ahead (RECOMP_DO_TEST). */
static void do_test_frame(uint32_t player, double now) {
    const uint32_t st = mp_state(get_u32(player + 0x70));
    if (!st)
        return;
    MpPose me;
    mp_read(st, &me);
    const uint32_t key = mp_model_key(player);
    static const float fz[3] = {0, 0, 1}, fx[3] = {1, 0, 0};
    float fwd[3], side[3];
    mp_rotate(me.q, fz, fwd);
    mp_rotate(me.q, fx, side);
    int made = 0;
    for (int i = 0; i < g_do_test && i < DO_MAX; ++i) {
        DoCar *d = do_by_id(0x40000000u + (uint32_t)i);
        MpPose p = me;
        const float along = 20.0f + 8.0f * (float)(i / 4), across = ((float)(i % 4) - 1.5f) * 4.0f;
        for (int k = 0; k < 3; ++k) {
            p.p[k] = me.p[k] + fwd[k] * along + side[k] * across;
            p.v[k] = 0;
        }
        if (!d) {
            if (made)
                continue;
            DoCar *f = NULL;
            for (int j = 0; j < DO_MAX && !f; ++j)
                if (!g_do[j].used)
                    f = &g_do[j];
            const uint32_t rp = get_u32(DO_RIDE_POOL);
            if (!rp || get_u32(rp + 0x2c) <= g_do_list_n + DO_RIDES_FREE || get_u32(rp + 0x18) + 2u >= get_u32(rp + 0x2c))
                continue;
            if (f && do_make(f, 0x40000000u + (uint32_t)i, key, g_do_lite_m > 0 && along > g_do_lite_m, &p))
                made = 1;
            continue;
        }
        if (!d->have || do_dist(d->pose.p, p.p) > 30.0f) { /* placed once, then left standing */
            d->pose = p;
            d->have = 1;
        }
        d->heard = now;
        d->want = 1;
        do_write(d);
    }
}

/* Once a frame (end of mp_service). */
static void do_frame(MpShared *sh, uint32_t player) {
    const double t0 = emscripten_get_now(), now = do_now();
    if (g_do_last_frame_at > 0 && g_do_ft_n < 512)
        g_do_ft[g_do_ft_n++] = (float)(t0 - g_do_last_frame_at);
    g_do_last_frame_at = t0;
    ++g_do_frames_acc;
    ++g_do_s.frame;
    do_read_list();
    /* Ours that the game itself removed (a world unload) are forgotten, never destroyed twice. */
    for (int i = 0; i < DO_MAX; ++i)
        if (g_do[i].used && !do_in_list(g_do[i].conn)) {
            do_log("core.nfsc: drawonly car gone from the render list (the game removed it)");
            g_do[i].conn = 0;
            do_free(&g_do[i], "not in the render list", 0);
        }
    const int in_world = player && get_u32(DO_FLOW) == 6u;
    if (!in_world || !g_do_on) {
        if (g_do_world)
            do_free_all(in_world ? "off" : "left the world");
        g_do_world = 0;
        do_note(now);
        return;
    }
    ++g_do_world;
    if (g_do_world < 30 || g_mp_local_paused) { /* let the world settle; paused (Escape): everything stays as it is */
        do_note(now);
        return;
    }
    const uint32_t st = mp_state(get_u32(player + 0x70));
    float here[3] = {0, 0, 0};
    for (int k = 0; st && k < 3; ++k)
        here[k] = bits_float(get_u32(st + 0x10 + 4u * k));
    for (int i = 0; i < DO_MAX; ++i)
        g_do[i].want = 0;

    /* Each friend in the slots: full car drawn? draw-only wanted? */
    uint32_t slots_active = 0, full_drawn = 0;
    float far_full[2] = {0, 0};
    int far_slot[2] = {-1, -1};
    for (int i = 0; sh && i < MP_SLOTS; ++i) {
        const MpCar *c = &g_mp_remote[i];
        g_do_full_drawn[i] = 0;
        if (!c->id || !c->kept) {
            g_do_full_conn[i] = 0;
            continue;
        }
        ++slots_active;
        if (c->simable) {
            if (g_do_full_id[i] != c->id)
                g_do_full_conn[i] = 0;
            g_do_full_id[i] = c->id;
            g_do_full_conn[i] = do_conn_of_car(c->simable, i);
            g_do_full_drawn[i] = g_do_full_conn[i] && do_list_drawn(g_do_full_conn[i]);
            full_drawn += g_do_full_drawn[i];
            const float d = do_dist(c->drawn, here);
            for (int k = 0; k < 2; ++k)
                if (d > far_full[k]) {
                    if (k == 0) { far_full[1] = far_full[0]; far_slot[1] = far_slot[0]; }
                    far_full[k] = d;
                    far_slot[k] = i;
                    break;
                }
        } else {
            g_do_full_conn[i] = 0;
        }
    }
    /* Wanted: 1 shown, 2 a hidden twin of a full car. Candidates sorted by distance (budget). */
    int cand[MP_SLOTS], want[MP_SLOTS], nc = 0;
    float cd[MP_SLOTS];
    for (int i = 0; sh && i < MP_SLOTS; ++i) {
        const MpCar *c = &g_mp_remote[i];
        if (!c->id || !c->kept || (g_mp_racer && g_mp_racer(c->id)))
            continue;
        const float d = do_dist(c->drawn, here);
        int w = 0;
        if (!c->simable && c->tag_only)
            w = 1;
        else if (c->simable && !g_do_full_drawn[i]) {
            DoCar *e = do_by_id(c->id);
            w = e ? 1 : 0; /* a full car not drawn yet (loading) or hidden: keep showing ours */
            if (!e && (d > g_do_shadow_m || i == far_slot[0] || i == far_slot[1]))
                w = 2;
        } else if (c->simable && (d > g_do_shadow_m || i == far_slot[0] || i == far_slot[1]))
            w = 2;
        const int urgent = c->simable && g_do_urgent[i] > 0 && now - g_do_urgent[i] < 4000.0;
        if (urgent && !w)
            w = 2;
        if (!w)
            continue;
        cand[nc] = i;
        want[nc] = w;
        cd[nc] = urgent ? -1.0f : d; /* first in line, inside the budget */
        ++nc;
    }
    for (int a = 1; a < nc; ++a) { /* nearest first */
        const int ci = cand[a], wi = want[a];
        const float di = cd[a];
        int b = a - 1;
        while (b >= 0 && cd[b] > di) { cand[b + 1] = cand[b]; want[b + 1] = want[b]; cd[b + 1] = cd[b]; --b; }
        cand[b + 1] = ci; want[b + 1] = wi; cd[b + 1] = di;
    }
    int made = 0, live = 0, loading_now = 0;
    for (int i = 0; i < DO_MAX; ++i) {
        live += g_do[i].used;
        loading_now += g_do[i].used && get_u32(g_do[i].conn + 0x24) < 2;
    }
    const uint32_t rpool = get_u32(DO_RIDE_POOL);
    g_do_s.rides_used = rpool ? get_u32(rpool + 0x18) : 0;
    g_do_s.rides_total = rpool ? get_u32(rpool + 0x2c) : 0;
    const int rides_ok = rpool && g_do_s.rides_total > g_do_list_n + DO_RIDES_FREE && g_do_s.rides_used + 2u < g_do_s.rides_total;
    for (int a = 0; a < nc; ++a) {
        const int i = cand[a];
        const MpCar *c = &g_mp_remote[i];
        DoCar *d = do_by_id(c->id);
        if (d && d->key != c->key) { /* another model: made again */
            do_free(d, "another model", d->onscreen);
            d = NULL;
            --live;
        }
        if (!d) {
            if (a >= g_do_budget || made || live >= g_do_budget + 2 || g_do_list_n >= DO_RIDES_MAX || !rides_ok ||
                (loading_now >= DO_LOADING_MAX && cd[a] >= 0))
                continue;
            DoCar *f = NULL;
            for (int j = 0; j < DO_MAX && !f; ++j)
                if (!g_do[j].used)
                    f = &g_do[j];
            if (!f)
                continue;
            MpPose p;
            mp_sample(c, c->play, &p);
            if (!do_make(f, c->id, c->key, g_do_lite_m > 0 && cd[a] > g_do_lite_m, &p))
                continue;
            made = 1;
            ++live;
            d = f;
        }
        d->slot = i;
        d->want = a < g_do_budget || d->onscreen ? want[a] : 0;
        d->d = do_dist(c->drawn, here);
    }
    if (g_do_test)
        do_test_frame(player, now);
    /* Apply: show, hide, coast, linger, remove. */
    uint32_t shown = 0, drawn = 0, loading = 0, twins = 0;
    float near_m = 1e9f, far_m = 0;
    for (int i = 0; i < DO_MAX; ++i) {
        DoCar *d = &g_do[i];
        if (!d->used)
            continue;
        const uint32_t state = get_u32(d->conn + 0x24);
        if (d->have)
            d->d = do_dist(d->pose.p, here); /* every car, in the slots or not */
        if (state == 3 && d->drawn_at == 0)
            d->drawn_at = now;
        if (g_do_lod >= 0 && state >= 2) {
            const uint32_t info = get_u32(d->conn + 0x40);
            if (info)
                g_api->guest_write_u32(g_api, info + 0x30c8, (uint32_t)g_do_lod);
        }
        /* LOOKS. A car made before its owner's look arrived (or before a newer one) gets a hidden replacement in
         * that look; once it is loaded it takes the old one's place in the same frame (no pop). */
        if (d->id & DO_NEW_LOOK) {
            DoCar *o = do_by_id(d->id & ~DO_NEW_LOOK);
            if (!o) {
                do_free(d, "its car is gone", 0);
                continue;
            }
            if (state < 2 && now - d->born < 15000.0) {
                d->want = 2;
                d->d = o->d;
                goto apply;
            }
            if (state >= 2) {
                d->pose = o->pose;
                d->have = o->have;
                d->slot = o->slot;
                d->want = o->want;
                d->d = o->d;
                do_write(d);
                do_set_shown(d, o->shown);
                do_free(o, "replaced by its owner's newer look", 0);
                d->id &= ~DO_NEW_LOOK;
            } else {
                do_free(d, "its new look did not load in 15 s", 0);
                continue;
            }
        } else if (d->slot >= 0 && d->id < 0x40000000u && (g_do_s.frame % 15u) == 0 && !made && rides_ok &&
                   !do_by_id(d->id | DO_NEW_LOOK)) {
            const uint32_t st = do_look_stamp(d->id, d->key);
            if (st && st != d->look_stamp) {
                DoCar *f = NULL;
                for (int j = 0; j < DO_MAX && !f; ++j)
                    if (!g_do[j].used)
                        f = &g_do[j];
                if (f && do_make(f, d->id | DO_NEW_LOOK, d->key, d->lite, d->have ? &d->pose : NULL))
                    made = 1;
            }
        }
        /* HAND-DOWN: mp.h waits to take the full car away until this twin is drawn; meanwhile both are drawn at one pose. */
        if (d->want == 2 && d->handing_at > 0 && now - d->handing_at < 500.0)
            d->want = 1;
        if (!d->want) {
            /* Not wanted here: its friend got a drawn full car, left the slots, or is over the budget. */
            const int slot = d->slot;
            const MpCar *c = slot >= 0 ? &g_mp_remote[slot] : NULL;
            const int full_now = c && c->id == d->id && c->simable && g_do_full_drawn[slot];
            if (full_now) {
                do_free(d, "its full car is drawn", 0);
                continue;
            }
            const int in_slots = c && c->id == d->id && c->kept;
            if (in_slots && slot >= 0 && !c->tag_only && !c->simable && do_dist(c->drawn, here) > DO_FAR) { /* beyond the radius */
                if (!d->onscreen || now - d->heard > DO_LINGER_MS) {
                    do_free(d, "beyond the far radius", d->onscreen && d->shown);
                    continue;
                }
                d->want = 1; /* in view: shown (a twin too: its full car just went), until out of view */
                goto apply;
            } else if (!in_slots) {
                /* Left the slots. A hidden twin whose full car lingers in view (mp.h) is not wanted: that car is still
                 * drawn. Otherwise: coast on its last speed, then stand; removed once off screen (no time limit while in
                 * view within the far radius, as mp.h's lingering cars), or beyond the far radius after DO_LINGER_MS. */
                const double since = now - d->heard;
                if (!d->shown && do_full_lingers(d->id)) {
                    do_free(d, "its full car lingers", 0);
                    continue;
                }
                if (since < DO_COAST_MS && d->have) {
                    const float dt = 1.0f / 60.0f;
                    for (int k = 0; k < 3; ++k)
                        d->pose.p[k] += d->pose.v[k] * dt;
                    do_write(d);
                }
                if ((!d->onscreen && since > 1000.0) || (d->d > DO_FAR && since > DO_LINGER_MS)) {
                    do_free(d, "left the slots", d->onscreen && d->shown);
                    continue;
                }
            } else if (!d->onscreen) { /* over the budget, off screen */
                do_free(d, "over the budget", 0);
                continue;
            }
            /* Kept: shown as it is; a hidden twin whose full car is no longer drawn is shown while in view. */
            d->want = d->shown || (d->onscreen && !(c && c->id == d->id && c->simable && g_do_full_drawn[slot])) ? 1 : 2;
        }
    apply:;
        const int show = d->want == 1 && d->have && !(g_do_ab_hidden && !g_do_ab_l3);
        do_set_shown(d, show);
        d->onscreen = d->have && do_on_screen(d->pose.p);
        shown += show;
        drawn += show && state == 3;
        loading += state < 2;
        twins += d->want == 2;
        if (show) {
            if (d->d < near_m) near_m = d->d;
            if (d->d > far_m) far_m = d->d;
        }
    }
    /* The sampler: a friend that was drawn and on screen last frame and is drawn by neither level now. */
    for (int i = 0; sh && i < MP_SLOTS; ++i) {
        const MpCar *c = &g_mp_remote[i];
        const DoCar *d = c->id ? do_by_id(c->id) : NULL;
        const int l2 = d && d->shown && get_u32(d->conn + 0x24) == 3;
        const int any = c->id && (g_do_full_drawn[i] || l2);
        const int onscreen = c->id && c->kept && do_on_screen(c->drawn);
        const float dist = c->id ? do_dist(c->drawn, here) : 0;
        if (g_do_seen_drawn[i] && g_do_seen_id[i] == c->id && !any && !(g_do_ab_hidden && !g_do_ab_l3)) {
            ++g_do_s.vanished_onscreen;
            char line[160];
            snprintf(line, sizeof line, "core.nfsc: drawonly sampler: player %u vanished on screen at %.0f m (full car %s, slot %s)", c->id, dist,
                     c->simable ? "built, not drawn" : "none", c->kept ? "kept" : "empty");
            do_log(line);
        }
        g_do_seen_drawn[i] = any && onscreen && dist < 600.0f;
        g_do_seen_id[i] = c->id;
        g_do_seen_d[i] = dist;
    }
    int live_n = 0;
    for (int i = 0; i < DO_MAX; ++i)
        live_n += g_do[i].used;
    g_do_s.live = (uint32_t)live_n;
    g_do_s.shown = shown;
    g_do_s.drawn = drawn;
    g_do_s.loading = loading;
    g_do_s.twins = twins;
    g_do_s.full_drawn = full_drawn;
    g_do_s.slots_active = slots_active;
    g_do_s.budget = (uint32_t)g_do_budget;
    g_do_s.list_n = g_do_list_n;
    g_do_s.near_l2_m = near_m < 1e8f ? near_m : 0;
    g_do_s.far_l2_m = far_m;
    g_do_s.ab_hidden = (uint32_t)g_do_ab_hidden;
    g_do_s.view = g_mp_view;
    if (g_do_ab > 0) {
        const int phase = (int)(now / (g_do_ab * 1000.0)) & 1;
        if (phase != g_do_ab_hidden) {
            g_do_ab_hidden = phase;
            g_do_note_at = now - DO_NOTE_MS; /* a 5 s line closes the phase */
        }
    }
    g_do_t_manage += emscripten_get_now() - t0;
    if (g_do_stats_block) {
        uint8_t *out = NULL;
        if (g_api->guest_ptr(g_api, g_do_stats_block, sizeof g_do_s, (void **)&out) == POP_OK && out)
            memcpy(out, &g_do_s, sizeof g_do_s);
    }
    do_note(now);
}

/* mp.h asks before it takes a friend's full car away for room, the cap or distance (not when the friend
 * leaves or changes car): yes once a twin of it is loaded (state >= 2: shown the same frame), else a twin is
 * asked for and mp.h tries again next frame. A model with no type here, or 4 s of waiting, says yes. */
static int do_demote_ready(int slot, const MpCar *c, int soft) {
    if (!g_do_on || g_do_world < 30 || !c->id)
        return !soft;
    const double now = do_now();
    DoCar *d = do_by_id(c->id);
    if (d && d->key == c->key && get_u32(d->conn + 0x24) >= 2) {
        /* Loaded. Shown first (the frame's apply: HAND-DOWN), and the full car may go only once the twin is drawn (state 3,
         * not hidden, in the render list read this frame): a twin switched on in the frame its full car went was not drawn
         * yet for a frame or more, seen as a gap at 398 m by the page sampler (runs/spread40). */
        d->handing_at = now;
        if (d->shown && do_list_drawn(d->conn)) {
            g_do_urgent[slot] = 0;
            return 1;
        }
        if (!soft && g_do_urgent[slot] > 0 && now - g_do_urgent[slot] > 4000.0) {
            g_do_urgent[slot] = 0;
            return 1;
        }
        if (!soft && g_do_urgent[slot] <= 0)
            g_do_urgent[slot] = now;
        ++g_do_s.demote_waits;
        return 0;
    }
    /* soft (mp.h's L0 radius): only with a loaded twin; the twin is asked for anyway (beyond DO_SHADOW_M), so the
     * full car simply stays while there is none (budget, ride slots, an unknown model). */
    if (soft)
        return 0;
    if (do_type_of(c->key) < 0)
        return 1;
    if (g_do_urgent[slot] <= 0)
        g_do_urgent[slot] = now;
    else if (now - g_do_urgent[slot] > 4000.0) {
        g_do_urgent[slot] = 0;
        return 1;
    }
    ++g_do_s.demote_waits;
    return 0;
}

static void do_main_before(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api; (void)cpu; (void)inv; (void)user;
    g_do_main_t0 = emscripten_get_now();
}
static void do_main_after(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api; (void)cpu; (void)inv; (void)user;
    if (g_do_main_t0 > 0)
        g_do_t_main += emscripten_get_now() - g_do_main_t0;
    g_do_main_t0 = 0;
}

/* Costs: the render connections' updates (007bf5f0), and OnRender per connection by kind. */
static void do_update_before(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api; (void)cpu; (void)inv; (void)user;
    g_do_update_t0 = emscripten_get_now();
}
static void do_update_after(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api; (void)cpu; (void)inv; (void)user;
    if (g_do_update_t0 > 0)
        g_do_t_update += emscripten_get_now() - g_do_update_t0;
    g_do_update_t0 = 0;
}
static void do_render_before(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api; (void)inv; (void)user;
    const uint32_t conn = cpu->ecx;
    int cls = 2;
    if (do_by_conn(conn))
        cls = 0;
    else
        for (int i = 0; i < MP_SLOTS; ++i)
            if (g_do_full_conn[i] == conn) {
                cls = 1;
                break;
            }
    g_do_render_class = cls;
    g_do_render_t0 = emscripten_get_now();
}
static void do_render_after(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api; (void)cpu; (void)inv; (void)user;
    if (g_do_render_t0 <= 0)
        return;
    const double dt = emscripten_get_now() - g_do_render_t0;
    g_do_render_t0 = 0;
    if (g_do_render_class == 0) { g_do_t_ours += dt; ++g_do_n_ours; }
    else if (g_do_render_class == 1) { g_do_t_full += dt; ++g_do_n_full; }
    else { g_do_t_other += dt; ++g_do_n_other; }
}

/* L3 (RECOMP_DO_L3=<m>): CarRenderInfo::Render (007de460, thiscall (view, pos, matrix, wheels, brakes, more,
 * flags, 0, reflection, 1.0f, lod min, lod max)) for one of our cars beyond g_do_l3_m: flags | 0x400 picks an
 * effect's "lowlod" technique (00727930: a technique found at 0072f030 is used when flags & 0xc00; the game sets
 * 0x400 in its reflection pass, OnRender 007e1160), and the LOD range is raised to g_do_l3_lod. */
static void do_draw_before(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api; (void)inv; (void)user;
    if (g_do_l3_m <= 0 || (g_do_ab_l3 && !g_do_ab_hidden))
        return;
    for (int i = 0; i < DO_MAX; ++i) {
        const DoCar *d = &g_do[i];
        if (!d->used || d->d < g_do_l3_m || get_u32(d->conn + 0x40) != cpu->ecx)
            continue;
        g_api->guest_write_u32(g_api, cpu->esp + 0x1c, get_u32(cpu->esp + 0x1c) | 0x400u);
        for (uint32_t a = 0x2c; a <= 0x30; a += 4)
            if ((int32_t)get_u32(cpu->esp + a) < g_do_l3_lod)
                g_api->guest_write_u32(g_api, cpu->esp + a, (uint32_t)g_do_l3_lod);
        return;
    }
}

static int do_env(const char *name, int fallback) {
    const char *e = getenv(name);
    return e && *e ? atoi(e) : fallback;
}

/* mp.h (mp_gone_note): player id's draw-only car is loaded, so it is shown in the frame its full car goes. */
static int do_covers(uint32_t id) {
    const DoCar *d = do_by_id(id);
    return d && g_do_world >= 30 && get_u32(d->conn + 0x24) >= 2;
}

/* mp.h: whether draw-only cars are on (its radii, kept cars). */
static int do_l2_on(void) {
    return g_do_on && g_do_world >= 30;
}

static uint32_t g_do_hooks[7];
static void drawonly_install(void) {
    g_do_on = do_env("RECOMP_DRAWONLY", 1);
    g_do_budget = do_env("RECOMP_DO_BUDGET", 64);
    if (g_do_budget > DO_MAX - 4)
        g_do_budget = DO_MAX - 4;
    g_do_test = do_env("RECOMP_DO_TEST", 0);
    g_do_ab = do_env("RECOMP_DO_AB", 0);
    g_do_lite_m = (float)do_env("RECOMP_DO_LITE", 0);
    g_do_shadow_m = (float)do_env("RECOMP_DO_SHADOW", (int)DO_SHADOW_M);
    g_do_lod = do_env("RECOMP_DO_LOD", -1);
    g_do_l3_m = (float)do_env("RECOMP_DO_L3", 0);
    g_do_l3_lod = do_env("RECOMP_DO_L3_LOD", 2);
    g_do_ab_l3 = do_env("RECOMP_DO_AB_L3", 0);
    if (!g_do_on)
        return;
    uint32_t b = 0;
    if (g_api->guest_alloc(g_api, 0x500, &b) != POP_OK || !b) {
        log_line("core.nfsc: drawonly unavailable (no guest memory)");
        g_do_on = 0;
        return;
    }
    /* 0x500 bytes: two service objects and their one-entry vtables, the stats block at +0x20, scratch at +0x400 */
    g_api->guest_write_u32(g_api, b + 0x00, b + 0x10);
    g_api->guest_write_u32(g_api, b + 0x08, b + 0x14);
    g_api->guest_write_u32(g_api, b + 0x10, DO_SERVICE_TRUE);
    g_api->guest_write_u32(g_api, b + 0x14, DO_SERVICE_FALSE);
    g_do_handler_show = b + 0x00;
    g_do_handler_hide = b + 0x08;
    g_do_stats_block = b + 0x20;
    g_do_scratch = b + 0x400; /* the stats end long before (static assert below) */
    g_do_s.magic = 0x594c4f44u; /* "DOLY" */
    g_mp_tag_pose = do_tag_pose;
    g_mp_car_pose = do_car_pose;
    g_mp_frame_hook = do_frame;
    g_mp_demote_ready = do_demote_ready;
    g_mp_l2_on = do_l2_on;
    g_mp_l2_covers = do_covers;
    /* The cost timers (a hook on every OnRender call) only when asked: RECOMP_DO_TIMING=1. */
    if (do_env("RECOMP_DO_TIMING", 0)) {
        g_api->hook_install(g_api, 0x00761150u, do_main_before, POP_HOOK_BEFORE, NULL, &g_do_hooks[5]);
        g_api->hook_install(g_api, 0x00761150u, do_main_after, POP_HOOK_AFTER, NULL, &g_do_hooks[6]);
        g_api->hook_install(g_api, DO_UPDATE_ALL, do_update_before, POP_HOOK_BEFORE, NULL, &g_do_hooks[0]);
        g_api->hook_install(g_api, DO_UPDATE_ALL, do_update_after, POP_HOOK_AFTER, NULL, &g_do_hooks[1]);
        g_api->hook_install(g_api, DO_ON_RENDER, do_render_before, POP_HOOK_BEFORE, NULL, &g_do_hooks[2]);
        g_api->hook_install(g_api, DO_ON_RENDER, do_render_after, POP_HOOK_AFTER, NULL, &g_do_hooks[3]);
    }
    if (g_do_l3_m > 0)
        g_api->hook_install(g_api, 0x007de460u, do_draw_before, POP_HOOK_BEFORE, NULL, &g_do_hooks[4]);
    char line[200];
    snprintf(line, sizeof line, "core.nfsc: drawonly ready: budget %d, twins from %.0f m, lite from %.0f m%s, test %d, A/B %d s", g_do_budget,
             g_do_shadow_m, g_do_lite_m, g_do_lite_m > 0 ? "" : " (off)", g_do_test, g_do_ab);
    log_line(line);
#ifdef __EMSCRIPTEN__
    void *host = NULL;
    if (g_api->guest_ptr(g_api, g_do_stats_block, sizeof g_do_s, &host) == POP_OK && host)
        MAIN_THREAD_ASYNC_EM_ASM({
            var block = {};
            block.memory = wasmMemory;
            block.ptr = $0;
            globalThis.carbonDrawOnly = block;
        }, (uint32_t)(uintptr_t)host);
#endif
}
