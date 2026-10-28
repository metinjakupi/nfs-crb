/* Nearby native AI racers are visible to friends who remain in free roam.
 * The source game owns AI, collisions and career progress. Replicas are ordinary
 * opaque RBRemote cars with their original non-contact rule, never native racers.
 * All movement uses mp_follow. Appearance uses look.h's validated record.
 */
#define RV_MAGIC 0x31564952u
#define RV_MAX 4
#define RV_SLOTS 8
#define RV_RANGE 400.0f

typedef struct RvOut {
    uint32_t id, key, size, stamp;
    MpPose pose;
    LookRecord look;
} RvOut;
typedef struct RvSlot {
    MpSlot poses; /* pad[0]: actor number, independent of owner id */
    uint32_t size, stamp;
    LookRecord look;
} RvSlot;
typedef struct RvShared {
    uint32_t magic, version, page, seq, ms, count, pad[2];
    RvOut out[RV_MAX];
    RvSlot slots[RV_SLOTS];
} RvShared;
typedef struct RvCar { MpCar car; uint32_t actor, stamp; } RvCar;
_Static_assert(sizeof(RvOut) == 0x478 && sizeof(RvSlot) == 0x53c &&
               offsetof(RvShared, slots) == 0x1200, "Rival shared layout");
static uint32_t g_rv_block, g_rv_hooks[2];
static RvCar g_rv_cars[RV_SLOTS];
static RvSlot g_rv_seen[RV_SLOTS];

static RvShared *rv_shared(void) {
    void *p = NULL;
    return g_rv_block && g_api->guest_ptr(g_api, g_rv_block, sizeof(RvShared), &p) == POP_OK ? p : NULL;
}

static void rv_publish(RvShared *sh, uint32_t player) {
    RvOut found[RV_MAX];
    float distances[RV_MAX];
    int count = 0;
    const uint32_t state = player && sh->page ? mp_state(get_u32(player + 0x70)) : 0;
    MpPose own;
    if (state) mp_read(state, &own);
    uint32_t node = state ? get_u32(MP_CARS) : 0;
    for (int guard = 0; node && node != MP_CARS && guard < 64; ++guard, node = get_u32(node)) {
        const uint32_t car = node - 0xd0;
        if (get_u32(car + 0x148) != 3u) continue;
        const uint32_t body = mp_state(get_u32(car + 0x70));
        const uint32_t key = mp_model_key(car);
        if (!body || !key || mp_cop_model(key)) continue;
        RvOut out = {0};
        mp_read(body, &out.pose);
        if (!mp_pose_ok(&out.pose)) continue;
        float d = 0;
        for (int k = 0; k < 3; ++k) d += (out.pose.p[k] - own.p[k]) * (out.pose.p[k] - own.p[k]);
        if (d > RV_RANGE * RV_RANGE) continue;
        if (count == RV_MAX && distances[RV_MAX - 1] <= d) continue;
        int at = count < RV_MAX ? count++ : RV_MAX - 1;
        out.id = car; out.key = key;
        const void *look = NULL;
        uint32_t db = 0;
        if (get_u32(car + 0xe8) && look_db(&db) &&
            g_api->guest_ptr(g_api, get_u32(car + 0xe8), LOOK_BYTES, (void **)&look) == POP_OK && look) {
            out.look.format = LOOK_FORMAT; out.look.db = db;
            memcpy(out.look.bytes, look, LOOK_BYTES);
            if (look_valid(&out.look, LOOK_REC) >= 0) out.size = LOOK_REC;
        }
        uint32_t hash = 0x811c9dc5u;
        const uint8_t *bytes = (const uint8_t *)&out.look;
        for (uint32_t j = 0; j < out.size; ++j) hash = (hash ^ bytes[j]) * 0x01000193u;
        out.stamp = hash ? hash : 1;
        for (; at > 0 && distances[at - 1] > d; --at) {
            distances[at] = distances[at - 1]; found[at] = found[at - 1];
        }
        distances[at] = d; found[at] = out;
    }
    __atomic_add_fetch(&sh->seq, 1, __ATOMIC_ACQ_REL);
    sh->ms = (uint32_t)mp_ms(); sh->count = count;
    if (count) memcpy(sh->out, found, sizeof(RvOut) * count);
    __atomic_add_fetch(&sh->seq, 1, __ATOMIC_RELEASE);
}

static int rv_live(MpCar *c) {
    return c->simable && mp_listed(c->simable - 0x24) &&
           mp_kind_kept(c->simable - 0x24, MP_REMOTE) && mp_model_key(c->simable - 0x24) == c->key;
}

static void rv_service(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api; (void)cpu; (void)inv; (void)user;
    RvShared *sh = rv_shared();
    if (!sh) return;
    const uint32_t player = g_mp_player_simable ? g_mp_player_simable - 0x24 : 0;
    rv_publish(sh, player);
    const uint32_t state = player && sh->page && !pp_in_race() ? mp_state(get_u32(player + 0x70)) : 0;
    MpPose own;
    if (state) mp_read(state, &own);
    for (int i = 0; i < RV_SLOTS; ++i) {
        const RvSlot *src = &sh->slots[i];
        const uint32_t before = __atomic_load_n(&src->poses.seq, __ATOMIC_ACQUIRE);
        if (!(before & 1)) {
            RvSlot copy;
            memcpy(&copy, src, sizeof copy);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (__atomic_load_n(&src->poses.seq, __ATOMIC_ACQUIRE) == before) g_rv_seen[i] = copy;
        }
        RvSlot *slot = &g_rv_seen[i];
        MpSlot *s = &slot->poses;
        RvCar *r = &g_rv_cars[i];
        MpCar *c = &r->car;
        if (c->simable && !rv_live(c)) c->simable = 0;
        const int active = state && s->id && s->pad[0] && s->mode == 1 && s->key && !mp_cop_model(s->key);
        const int same = c->id == s->id && c->key == s->key && r->actor == s->pad[0] && r->stamp == slot->stamp;
        if (!active || !same) {
            mp_drop(c, NULL);
            memset(r, 0, sizeof *r);
            if (!active) continue;
            c->id = s->id; c->key = s->key; c->kind = MP_REMOTE; c->visual_only = 1;
            c->taken = s->count + 1; r->actor = s->pad[0]; r->stamp = slot->stamp;
        }
        mp_take(c, s, mp_now());
        if (!c->kept) continue;
        const MpPose *last = &c->pose[c->kept - 1];
        float d = 0;
        for (int k = 0; k < 3; ++k) d += (last->p[k] - own.p[k]) * (last->p[k] - own.p[k]);
        if (d > MP_FAR * MP_FAR) { mp_drop(c, NULL); continue; }
        if (c->simable || d > MP_NEAR * MP_NEAR || (c->wait && --c->wait)) continue;
        uint32_t look = 0;
        void *dst = NULL;
        if (slot->size) {
            if (look_valid(&slot->look, slot->size) < 0) continue;
            if (g_api->guest_ptr(g_api, g_look_buf, LOOK_BYTES, &dst) != POP_OK || !dst) continue;
            memcpy(dst, slot->look.bytes, LOOK_BYTES);
            memset((uint8_t *)dst + 0xf8, 0, 0x38);
            look = g_look_buf;
        }
        c->simable = mp_build(player, MP_REMOTE, s->key, last->p, 2u | 8u | (c->tries++ & 1u), look);
        c->wait = 120;
        if (c->simable) {
            /* mp_build normally enables contacts. Restore RBRemote's own rule before physics runs. */
            const uint32_t body = get_u32(c->simable - 0x24 + 0x70);
            if (body) mp_put(body - 0x40, MP_REMOTE_VTABLE);
            mp_place(c->simable, last);
            log_line("core.nfsc: friend's rival car visible");
        }
    }
}

static void rv_view(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api; (void)cpu; (void)inv; (void)user;
    for (int i = 0; i < RV_SLOTS; ++i) {
        MpCar *c = &g_rv_cars[i].car;
        if (c->kept && rv_live(c)) { MpPose pose; mp_follow(c, mp_now(), 1, &pose); }
    }
}

static int rv_of(uint32_t simable) {
    for (int i = 0; simable && i < RV_SLOTS; ++i)
        if (g_rv_cars[i].car.simable == simable && rv_live(&g_rv_cars[i].car)) return 1;
    return 0;
}

static void rivals_install(void) {
    if (!g_mp_block || g_api->guest_alloc(g_api, sizeof(RvShared), &g_rv_block) != POP_OK) return;
    RvShared *sh = rv_shared();
    if (!sh) return;
    g_mp_visual_car = rv_of;
    memset(sh, 0, sizeof *sh); sh->magic = RV_MAGIC; sh->version = 1;
    if (g_api->hook_install(g_api, MP_FE_SERVICE, rv_service, POP_HOOK_BEFORE, NULL, &g_rv_hooks[0]) != POP_OK ||
        g_api->hook_install(g_api, 0x0071b780u, rv_view, POP_HOOK_AFTER, NULL, &g_rv_hooks[1]) != POP_OK) return;
#ifdef __EMSCRIPTEN__
    MAIN_THREAD_ASYNC_EM_ASM({
        var block = {}; block.memory = wasmMemory; block.ptr = $0;
        globalThis.carbonRivalBlock = block;
        globalThis.dispatchEvent(new Event("carbon-rivals"));
    }, (uint32_t)(uintptr_t)sh);
#endif
}
