/* look.h - other players' cars in their owners' look: paint, body kit, rims, spoiler, hood,
 * vinyls, decals, tint (included by nfsc.c after mp.h).
 *
 * THE RECORD. A car's whole look is one VehicleCustomizations, LOOK_BYTES (0x42c) bytes.
 * PVehicle::Construct (006daa80) takes it at VehicleParams + 0x2c and the car's constructor
 * (006da530) copies it into a block of its own at PVehicle + 0xe8, so the caller's copy may be
 * reused at once. 0 there means the model's stock look, or its preset's when the model names
 * one (Construct then fills the global 0x00a9e908 from the preset; that is how the AI racers and
 * the Quick Race bonus cars get theirs). Layout, from ReadFrom(RideInfo) 007c4040 and
 * WriteTo(RideInfo) 007dbe50:
 *   +0x000 int16 part[0x6a]  each slot's part (body kit, hood, spoiler, rims, paint, tint,
 *                            decals ...); negative = none
 *   +0x0d4 u8 on[2], +0x0d8 2 x {int32 part, int32 part, u32, u32} (read as int16 parts)
 *   +0x0f8 the performance package, 7 x {level, ?}; Construct applies it to the car's
 *          attributes (007b2fd0 -> 006cd770), which refuses the whole car if a level is above
 *          what the model allows, and leaves a "dynamic" copy of the attributes (see mp_model_key)
 *   +0x130 u32
 *   +0x138 20 vinyl layers of 0x1a: int16 part, 8 bytes of placement, 4 x {int16 colour part, u8, u8}
 *   +0x340 u32 layer mirror bits, +0x344 u8 layers used, +0x345 21 x 11 bytes of autosculpt
 * Nothing in it points into memory. A part is an index into the car part database: the packs
 * listed at LOOK_PART_DB ({next, ..., +0x34 records of 4 bytes, +0x38 count}; 007c4000 turns
 * an index into a part), which every copy of the game loads from its own data, the same way
 * (one pack of 20734 parts in v1.4). So the bytes travel as they are, with a fingerprint of the
 * database (LookRecord.db) so a game with other data never takes them.
 * The receiver zeroes the performance package: a friend's car is moved by its poses, the
 * upgrades would only change how the engine pushes it in a collision, and a package this game
 * refused would refuse the whole car.
 * LOADING. The car's render connection (007e6580) turns the record into a RideInfo (007dbe50)
 * and the car loader fetches what it names, as for the AI racers' preset looks.
 *
 * THE PAGE. web/net.js shares LookShared below (published as globalThis.carbonLookBlock
 * {memory, ptr} with a "carbon-look" event). The game writes this player's look there when it
 * changes (checked twice a second; seq odd while written, as in mp.h): the base model key, the
 * record, and a stamp that changes with it. The page sends it to the room ({t:"look", k, data})
 * and writes each other player's look into a peer entry by relay id, with a stamp of its own.
 * mp.h asks for a look when it builds a car (g_mp_look) and rebuilds a car once when a newer
 * look of its owner arrives (g_mp_relook); a car shown in this player's own model, because the
 * friend's is not loaded here, never takes a look.
 */

#define LOOK_BYTES 0x42cu
#define LOOK_FORMAT 1u
#define LOOK_REC (8u + LOOK_BYTES) /* LookRecord as it travels */
#define LOOK_SPACE 0x440u
#define LOOK_PEERS 8
#define LOOK_MAGIC 0x4b4c4d43u /* "CMLK" */
#define LOOK_PART_DB 0x00b76690u
#define LOOK_LAYERS 20u
#define LOOK_EVERY 30 /* frames between two looks at this player's car */

typedef struct LookRecord {
    uint32_t format; /* LOOK_FORMAT */
    uint32_t db;     /* look_db() of the sender's game */
    uint8_t bytes[LOOK_BYTES];
} LookRecord;

typedef struct LookPeer { /* 0x460 bytes; written by the page */
    uint32_t id, seq;     /* relay id, 0 = empty; seq odd while written */
    uint32_t key, size;   /* the model the look is for; bytes in rec (0 = the stock look) */
    uint32_t stamp;       /* new with each look the page writes */
    uint32_t pad[3];
    uint8_t rec[LOOK_SPACE];
} LookPeer;

typedef struct LookShared {
    uint32_t magic, version;   /* LOOK_MAGIC, 1 */
    uint32_t seq;              /* the game's part from here to rec: odd while written */
    uint32_t key, size, stamp; /* this player's car: base model, bytes in rec (0 = stock), changes */
    uint32_t parts, db;        /* this game's part database: count and fingerprint */
    uint8_t rec[LOOK_SPACE];
    LookPeer peers[LOOK_PEERS];
} LookShared;
/* The page reads these by offset (web/net.js). */
_Static_assert(offsetof(LookShared, rec) == 0x20 && offsetof(LookShared, peers) == 0x460 &&
                   sizeof(LookPeer) == 0x460 && sizeof(LookRecord) == LOOK_REC && LOOK_REC <= LOOK_SPACE,
               "LookShared layout");

static uint32_t g_look_block, g_look_buf; /* the shared block; a record to build with */
static uint32_t g_look_hooks[1];
static uint32_t g_look_used[MP_SLOTS]; /* the peer stamp each slot's car was built with, 0 none */
static int g_look_own[MP_SLOTS];       /* that car is in its owner's model, so a look may apply */
static uint32_t g_look_frame, g_look_logged[MP_SLOTS];
static uint32_t g_look_db, g_look_db_parts; /* the fingerprint and the part count it is for */

static LookShared *look_shared(void) {
    void *p = NULL;
    return g_look_block && g_api->guest_ptr(g_api, g_look_block, sizeof(LookShared), &p) == POP_OK ? p : NULL;
}

/* Parts in the database (0 before it is loaded); *fp gets its FNV-1a fingerprint, worked out
 * again only when the count changes. */
static uint32_t look_db(uint32_t *fp) {
    uint32_t parts = 0, packs = 0;
    for (uint32_t n = get_u32(LOOK_PART_DB); n && n != LOOK_PART_DB && packs < 64; n = get_u32(n), ++packs)
        parts += get_u32(n + 0x38);
    if (parts && parts != g_look_db_parts) {
        uint32_t h = 0x811c9dc5u ^ packs;
        for (uint32_t n = get_u32(LOOK_PART_DB), k = 0; n && n != LOOK_PART_DB && k < packs; n = get_u32(n), ++k) {
            const uint32_t count = get_u32(n + 0x38);
            const uint8_t *p = NULL;
            if (!count || g_api->guest_ptr(g_api, get_u32(n + 0x34), count * 4u, (void **)&p) != POP_OK || !p)
                return 0;
            for (uint32_t i = 0; i < count * 4u; ++i)
                h = (h ^ p[i]) * 0x01000193u;
        }
        g_look_db = h ? h : 1u;
        g_look_db_parts = parts;
    }
    if (fp)
        *fp = parts ? g_look_db : 0;
    return parts;
}

static int16_t look_i16(const uint8_t *b, uint32_t at) {
    int16_t v;
    memcpy(&v, b + at, 2);
    return v;
}

/* Whether a record sent by another game can be built here: same format and part database,
 * every part one this database has, at most LOOK_LAYERS vinyl layers. Returns the layers. */
static int look_valid(const LookRecord *r, uint32_t size) {
    uint32_t db = 0;
    const uint32_t parts = look_db(&db);
    if (size != LOOK_REC || r->format != LOOK_FORMAT || !parts || r->db != db)
        return -1;
    const uint8_t *b = r->bytes;
    const uint32_t layers = b[0x344];
    if (layers > LOOK_LAYERS)
        return -1;
    for (uint32_t i = 0; i < 0x6a; ++i)
        if (look_i16(b, 2 * i) >= (int32_t)parts)
            return -1;
    for (uint32_t i = 0; i < 4; ++i)
        if (look_i16(b, 0xd8 + 0x10 * (i / 2) + 4 * (i % 2)) >= (int32_t)parts)
            return -1;
    for (uint32_t l = 0; l < layers; ++l) {
        const uint32_t at = 0x138 + 0x1a * l;
        if (look_i16(b, at) >= (int32_t)parts)
            return -1;
        for (uint32_t c = 0; c < 4; ++c)
            if (look_i16(b, at + 0xa + 4 * c) >= (int32_t)parts)
                return -1;
    }
    return (int)layers;
}

/* The page's entry for player id with model key, copied whole, or 0 (none, or being written). */
static int look_peer(const LookShared *sh, uint32_t id, uint32_t key, LookPeer *out) {
    for (int i = 0; id && i < LOOK_PEERS; ++i) {
        const LookPeer *p = &sh->peers[i];
        const uint32_t before = __atomic_load_n(&p->seq, __ATOMIC_ACQUIRE);
        if ((before & 1) || p->id != id)
            continue;
        memcpy(out, p, sizeof *out);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&p->seq, __ATOMIC_ACQUIRE) != before || out->id != id)
            return 0;
        return out->key == key && out->size <= LOOK_SPACE;
    }
    return 0;
}

/* mp.h, building slot's car for player id: a guest copy of their look for model key (0 = the
 * car is in this player's own model), or 0 for the stock look. */
static uint32_t look_for(int slot, uint32_t id, uint32_t key) {
    LookShared *sh = look_shared();
    LookPeer peer;
    if (slot < 0 || slot >= MP_SLOTS)
        return 0;
    g_look_used[slot] = 0;
    g_look_own[slot] = key != 0;
    if (!sh || !key || !g_look_buf || !look_peer(sh, id, key, &peer))
        return 0;
    g_look_used[slot] = peer.stamp;
    if (!peer.size)
        return 0; /* the owner's car has no look of its own: stock */
    const LookRecord *r = (const LookRecord *)peer.rec;
    const int layers = look_valid(r, peer.size);
    uint8_t *dst = NULL;
    if (layers < 0 || g_api->guest_ptr(g_api, g_look_buf, LOOK_BYTES, (void **)&dst) != POP_OK || !dst) {
        if (g_look_logged[slot] != peer.stamp)
            log_line("core.nfsc: look of another player refused (other game data or a damaged record)");
        g_look_logged[slot] = peer.stamp;
        return 0;
    }
    memcpy(dst, r->bytes, LOOK_BYTES);
    memset(dst + 0xf8, 0, 0x38); /* the performance package: see the header */
    if (g_look_logged[slot] != peer.stamp) {
        int used = 0;
        for (uint32_t i = 0; i < 0x6a; ++i)
            used += look_i16(r->bytes, 2 * i) >= 0;
        char line[128];
        snprintf(line, sizeof line, "core.nfsc: look of player %u (model %08x): %d parts, %d vinyl layers", id, key,
                 used, layers);
        log_line(line);
        g_look_logged[slot] = peer.stamp;
    }
    return g_look_buf;
}

/* mp.h: whether slot's car, built for player id and model key, was built before the newest
 * look the page has for them (then it is built again, once per look). */
static int look_stale(int slot, uint32_t id, uint32_t key) {
    LookShared *sh = look_shared();
    LookPeer peer;
    if (slot < 0 || slot >= MP_SLOTS || !sh || !g_look_own[slot])
        return 0;
    const uint32_t now = look_peer(sh, id, key, &peer) ? peer.stamp : 0;
    return now && now != g_look_used[slot];
}

/* This player's car's look, for the page: published when it changes. */
static void look_publish(LookShared *sh) {
    static uint8_t last[LOOK_BYTES];
    static uint32_t last_key, last_size, last_db;
    const uint32_t player = mp_player();
    if (!player)
        return; /* in the menus: the last look stays, as the page sends no poses meanwhile */
    const uint32_t cust = get_u32(player + 0xe8), key = mp_model_key(player);
    uint32_t db = 0;
    const uint32_t parts = look_db(&db);
    const uint8_t *src = NULL;
    if (cust && (!parts || g_api->guest_ptr(g_api, cust, LOOK_BYTES, (void **)&src) != POP_OK))
        return;
    const uint32_t size = src ? LOOK_REC : 0;
    if (key == last_key && size == last_size && db == last_db && (!src || !memcmp(src, last, LOOK_BYTES)))
        return;
    last_key = key;
    last_size = size;
    last_db = db;
    if (src)
        memcpy(last, src, LOOK_BYTES);
    __atomic_fetch_add(&sh->seq, 1u, __ATOMIC_SEQ_CST);
    sh->key = key;
    sh->size = size;
    sh->parts = parts;
    sh->db = db;
    if (src) {
        LookRecord r = {LOOK_FORMAT, db, {0}};
        memcpy(r.bytes, src, LOOK_BYTES);
        memcpy(sh->rec, &r, sizeof r);
    }
    ++sh->stamp;
    __atomic_fetch_add(&sh->seq, 1u, __ATOMIC_SEQ_CST);
    int used = 0;
    for (uint32_t i = 0; src && i < 0x6a; ++i)
        used += look_i16(src, 2 * i) >= 0;
    char line[128];
    snprintf(line, sizeof line, "core.nfsc: this car's look (model %08x): %s%d parts, %d vinyl layers", key,
             src ? "" : "stock, ", used, src ? src[0x344] : 0);
    log_line(line);
}

static void look_service(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)cpu;
    (void)inv;
    (void)user;
    LookShared *sh = look_shared();
    if (sh && ++g_look_frame % LOOK_EVERY == 0)
        look_publish(sh);
}

static void look_install(void) {
    if (!g_mp_block || g_api->guest_alloc(g_api, LOOK_BYTES, &g_look_buf) != POP_OK ||
        g_api->guest_alloc(g_api, sizeof(LookShared), &g_look_block) != POP_OK) {
        log_line("core.nfsc: other players' car looks unavailable (no guest memory)");
        g_look_block = 0;
        return;
    }
    LookShared *sh = look_shared();
    if (!sh) {
        g_look_block = 0;
        return;
    }
    memset(sh, 0, sizeof *sh);
    sh->magic = LOOK_MAGIC;
    sh->version = 1;
    if (g_api->hook_install(g_api, MP_FE_SERVICE, look_service, POP_HOOK_BEFORE, NULL, &g_look_hooks[0]) != POP_OK) {
        log_line("core.nfsc: other players' car looks unavailable");
        return;
    }
    g_mp_look = look_for;
    g_mp_relook = look_stale;
#ifdef __EMSCRIPTEN__
    MAIN_THREAD_ASYNC_EM_ASM({
        var block = {};
        block.memory = wasmMemory;
        block.ptr = $0;
        globalThis.carbonLookBlock = block;
        globalThis.dispatchEvent(new Event("carbon-look"));
    }, (uint32_t)(uintptr_t)sh);
#endif
}
