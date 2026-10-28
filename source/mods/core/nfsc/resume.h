/* Refresh recovery for Carbon v1.4. Included after race.h/worldmap.h.
 * This is a small free-roam checkpoint, never a guest-memory save state.
 * Career files remain authoritative. Restore only through the game's menu,
 * IVehicle::SetPosition and world-streamer paths; no live race is rewound.
 *
 * Private mailbox CSR1: game seqlock +8, status +0x10, checkpoint +0x40;
 * page seqlock +0x100, command +0x104 (1 restore, 2 cancel), point +0x108.
 * A checkpoint is 72 bytes: profile id, name[32], model, track, p[3], q[4].
 * All game accesses/calls run on the guest thread before cFEng::Service.
 */
#define RESUME_MAGIC 0x31525343u
#define RESUME_PAUSED 4u
#define RESUME_FAILED 5u

typedef struct ResumePoint {
    uint32_t profile_id;
    char name[32];
    uint32_t model, track;
    float p[3], q[4];
} ResumePoint;
typedef struct ResumeShared {
    uint32_t magic, version, game_seq, valid;
    uint32_t status, reason, flow, menu_vt, menu_state, frame;
    uint32_t pad[6];
    ResumePoint point;
    uint8_t reserved[0x100 - 0x40 - sizeof(ResumePoint)];
    uint32_t page_seq, command;
    ResumePoint wanted;
} ResumeShared;
static uint32_t g_resume_block, g_resume_buf, g_resume_hook, g_resume_seq;
static uint32_t g_resume_status, g_resume_reason, g_resume_stage, g_resume_frame;
static uint32_t g_resume_last_menu, g_resume_last_state;
static double g_resume_started, g_resume_action_at, g_resume_world_at;
static double g_resume_pause_at, g_resume_paused_at;
static ResumePoint g_resume_wanted;

static ResumeShared *resume_shared(void) {
    void *p = NULL;
    return g_resume_block &&
                   g_api->guest_ptr(g_api, g_resume_block, sizeof(ResumeShared), &p) == POP_OK
               ? (ResumeShared *)p
               : NULL;
}
static uint32_t resume_profile(void) {
    const uint32_t fe = get_u32(WM_FE_MANAGER), table = fe ? get_u32(fe + 0xd4) : 0;
    return table ? get_u32(table) : 0;
}
static void resume_identity(ResumePoint *p) {
    const uint32_t profile = resume_profile();
    p->profile_id = profile ? get_u32(profile + 0xc) : 0;
    memset(p->name, 0, sizeof p->name);
    if (profile) {
        for (uint32_t i = 0; i < sizeof p->name - 1; ++i) {
            const char ch = (char)(get_u32(profile + 0x10 + (i & ~3u)) >> ((i & 3u) * 8));
            p->name[i] = ch;
            if (!ch)
                break;
        }
    }
}
static int resume_same_profile(const ResumePoint *p) {
    ResumePoint now = {0};
    resume_identity(&now);
    return now.profile_id == p->profile_id && !memcmp(now.name, p->name, sizeof now.name);
}
static int resume_valid(const ResumePoint *p) {
    MpPose pose = {0};
    memcpy(pose.q, p->q, sizeof pose.q);
    memcpy(pose.p, p->p, sizeof pose.p);
    return p->profile_id && p->name[0] && !p->name[31] && p->model && p->track && mp_pose_ok(&pose);
}
static int resume_roam(uint32_t player) {
    const uint32_t rs = race_status();
    if (get_u32(RACE_FLOW) != 6 || !player || !rs || get_u32(rs + 0x6a14) != 0 || mp_online_game())
        return 0;
    /* IVehicleAI::mPursuit: shared layout independently identified by cops.h.
   * A pursuit must not become a rewind to the last quiet road. */
    const uint32_t ai = get_u32(player + 0x104);
    return !ai || !get_u32(ai + 0x98);
}
static uint32_t resume_menu(void) {
    uint32_t a = get_u32(WM_FE_MANAGER);
    for (int i = 0; a && i < 12; ++i) {
        const uint32_t child = get_u32(a + 0xb0);
        if (!child || get_u32(child + 0x18) == 0xffffffffu)
            break;
        a = child;
    }
    return a;
}
static void resume_fail(uint32_t reason) {
    g_resume_status = RESUME_FAILED;
    g_resume_reason = reason;
    char line[128];
    snprintf(line, sizeof line,
             "core.nfsc: recovery stopped (reason %u, stage %u); normal career loading remains "
             "available",
             reason, g_resume_stage);
    log_line(line);
}
static void resume_select(uint32_t menu, uint32_t item) {
    /* FEStateManager::Select (00589140), used by the menu itself. It records
   * the selection and calls the current manager's +0x74 with (item, arg). */
    const uint32_t args[2] = {item, 0};
    mp_call(0x00589140u, menu, args, 2);
}
static int resume_move(uint32_t player) {
    const uint32_t list = get_u32(0x00a9f1e4u), vehicle = list ? get_u32(list) : 0;
    if (vehicle != player + 0xb8 || mp_model_key(player) != g_resume_wanted.model ||
        get_u32(0x00a7a1a8u) != g_resume_wanted.track || !resume_same_profile(&g_resume_wanted))
        return 0;
    /* The game's hot-position load (007d5150): IVehicle +0x7c takes physics
   * position/direction; 00764d40 updates the world streamer at that position.
   * Unlike writing a rigid body, this resets vehicle/camera state correctly. */
    const float forward[3] = {0, 0, 1};
    float direction[3];
    mp_rotate(g_resume_wanted.q, forward, direction);
    for (uint32_t i = 0; i < 3; ++i) {
        put_float(g_resume_buf + 4 * i, g_resume_wanted.p[i]);
        put_float(g_resume_buf + 16 + 4 * i, direction[i]);
    }
    const uint32_t args[2] = {g_resume_buf, g_resume_buf + 16};
    const uint32_t set_position = get_u32(get_u32(vehicle) + 0x7c);
    if (!set_position || g_api->guest_call(g_api, set_position, vehicle, args, 2, NULL) != POP_OK)
        return 0;
    const uint32_t stream[2] = {g_resume_buf, 1};
    if (g_api->guest_call(g_api, 0x00764d40u, 0, stream, 2, NULL) != POP_OK)
        return 0;
    /* Hot-position loading restores speed too. Recovery always starts stopped. */
    const uint32_t zero = 0;
    mp_call(get_u32(get_u32(vehicle) + 0xa8), vehicle, &zero, 1);
    return 1;
}
static void resume_step(uint32_t menu, uint32_t player) {
    if (g_resume_status < 1 || g_resume_status >= RESUME_PAUSED)
        return;
    const double now = mp_now();
    if (now - g_resume_started > 180000.0) {
        resume_fail(1);
        return;
    }
    const uint32_t vt = menu ? get_u32(menu) : 0, state = menu ? get_u32(menu + 4) : 0;
    if (g_resume_status == 3) {
        const uint32_t manager = get_u32(WM_FE_MANAGER);
        const uint32_t parent_state = manager ? get_u32(manager + 4) : 0;
        /* State 5 alone is only a transition. The normal pause child must be
         * active and stable before the page exposes the restored game. */
        if (parent_state == 5 && vt == 0x009d2a40u && state == 0 && get_u32(menu + 0x18) == 0) {
            if (!g_resume_paused_at)
                g_resume_paused_at = now;
            if (now - g_resume_paused_at >= 500.0) {
                g_resume_status = RESUME_PAUSED;
                log_line("core.nfsc: free-roam recovery complete, paused");
            }
        } else {
            g_resume_paused_at = 0;
            /* World streaming can replace a pause requested during its own
             * transition. Request only from the normal driving state. */
            if ((parent_state == 0 || parent_state == 9) && now - g_resume_action_at >= 500.0 &&
                now - g_resume_pause_at >= 1000.0) {
                const uint32_t pause[3] = {0, 0, 0};
                mp_call(WM_PAUSE_REQUEST, manager, pause, 3);
                g_resume_pause_at = now;
            }
        }
        if (now - g_resume_action_at > 10000.0 && g_resume_status != RESUME_PAUSED)
            resume_fail(7);
        return;
    }
    if (resume_roam(player)) {
        if (!g_resume_world_at)
            g_resume_world_at = now;
        /* Flow 6 starts before the world has registered its player/camera and
     * before the loading transition accepts pause requests. Wait for its
     * first settled second, then let the normal readiness checks decide. */
        if (now - g_resume_world_at < 1000.0 ||
            (get_u32(get_u32(WM_FE_MANAGER) + 4) != 0 && get_u32(get_u32(WM_FE_MANAGER) + 4) != 9))
            return;
        if (g_resume_stage < 4) {
            resume_fail(2);
            return;
        }
        if (!resume_move(player)) {
            if (now - g_resume_world_at > 5000.0)
                resume_fail(3);
            return;
        }
        g_resume_status = 3;
        g_resume_action_at = now;
        return;
    }
    if (get_u32(RACE_FLOW) != 3)
        return; /* world loading */
    if (now - g_resume_action_at < 800.0)
        return;
    /* Only known states from a normal boot. Never accept an arbitrary dialog
   * (a missing/corrupt profile, new career or online/race invitation). */
    if (vt == g_resume_last_menu && state == g_resume_last_state)
        return;
    if (vt == 0x009e8b30u && state == 17 && g_resume_stage == 0) {
        mp_call(0x00579e60u, menu, NULL, 0); /* press-start OnAccept */
        g_resume_stage = 1;
    } else if (vt == 0x009d4430u && state == 36 && g_resume_stage == 1) {
        /* The actual Continue button completes the memory-card load job.
     * Destroying the dialog directly would discard the loaded profile. */
        const uint32_t dialog = get_u32(0x00a97b14u);
        const uint32_t buttons = dialog ? get_u32(dialog + 0x2c) : 0;
        const uint32_t button = buttons ? get_u32(buttons) : 0;
        if (!button)
            return;
        mp_call(0x0058e900u, menu, &button, 1);
    } else if (vt == 0x009f7e68u && state == 0 && g_resume_stage <= 1) {
        if (!resume_same_profile(&g_resume_wanted)) {
            resume_fail(4);
            return;
        }
        resume_select(menu, 0); /* Main menu: Career */
        g_resume_stage = 2;
    } else if (vt == 0x009d2688u && state == 1 && g_resume_stage == 2) {
        resume_select(menu, 1); /* Career: Resume career */
        g_resume_stage = 3;
    } else if (vt == 0x009d2688u && state == 0 && g_resume_stage == 3) {
        resume_select(menu, 4); /* Safehouse: Free roam */
        g_resume_stage = 4;
        g_resume_status = 2;
    } else
        return;
    g_resume_last_menu = vt;
    g_resume_last_state = state;
    g_resume_action_at = now;
}
static void resume_service(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv,
                           void *user) {
    (void)api;
    (void)cpu;
    (void)inv;
    (void)user;
    ResumeShared *sh = resume_shared();
    if (!sh)
        return;
    const uint32_t seq = __atomic_load_n(&sh->page_seq, __ATOMIC_SEQ_CST);
    if (!(seq & 1u) && seq != g_resume_seq) {
        ResumePoint wanted;
        const uint32_t command = sh->command;
        memcpy(&wanted, &sh->wanted, sizeof wanted);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (__atomic_load_n(&sh->page_seq, __ATOMIC_SEQ_CST) == seq) {
            g_resume_seq = seq;
            if (command == 2)
                resume_fail(6);
            else if (command == 1 && !g_resume_status && resume_valid(&wanted)) {
                g_resume_wanted = wanted;
                g_resume_status = 1;
                g_resume_started = mp_now();
                log_line("core.nfsc: recovering free-roam session");
            } else if (command == 1)
                resume_fail(5);
        }
    }
    const uint32_t player = mp_player(), menu = resume_menu();
    resume_step(menu, player);
    ResumePoint point = {0};
    int valid = 0;
    if (resume_roam(player)) {
        const uint32_t state = mp_state(get_u32(player + 0x70));
        MpPose pose = {0};
        if (state) {
            mp_read(state, &pose);
            if (mp_pose_ok(&pose)) {
                resume_identity(&point);
                point.model = mp_model_key(player);
                point.track = get_u32(0x00a7a1a8u);
                memcpy(point.p, pose.p, sizeof point.p);
                memcpy(point.q, pose.q, sizeof point.q);
                valid = resume_valid(&point);
            }
        }
    }
    __atomic_fetch_add(&sh->game_seq, 1u, __ATOMIC_SEQ_CST);
    sh->valid = valid;
    sh->status = g_resume_status;
    sh->reason = g_resume_reason;
    sh->flow = get_u32(RACE_FLOW);
    sh->menu_vt = menu ? get_u32(menu) : 0;
    sh->menu_state = menu ? get_u32(menu + 4) : 0;
    sh->frame = ++g_resume_frame;
    sh->pad[0] = g_resume_stage;
    sh->point = point;
    __atomic_fetch_add(&sh->game_seq, 1u, __ATOMIC_SEQ_CST);
}
static void resume_install(void) {
#ifdef __EMSCRIPTEN__
    if (g_api->guest_alloc(g_api, sizeof(ResumeShared), &g_resume_block) != POP_OK ||
        g_api->guest_alloc(g_api, 64, &g_resume_buf) != POP_OK)
        return;
    ResumeShared *sh = resume_shared();
    if (!sh)
        return;
    memset(sh, 0, sizeof *sh);
    sh->magic = RESUME_MAGIC;
    sh->version = 1;
    if (g_api->hook_install(g_api, MP_FE_SERVICE, resume_service, POP_HOOK_BEFORE, NULL,
                            &g_resume_hook) != POP_OK)
        return;
    MAIN_THREAD_ASYNC_EM_ASM(
        {
            var block = {};
            block.memory = wasmMemory;
            block.ptr = $0;
            globalThis.carbonResumeBlock = block;
            globalThis.dispatchEvent(new Event("carbon-resume"));
        },
        (uint32_t)(uintptr_t)sh);
#endif
}
