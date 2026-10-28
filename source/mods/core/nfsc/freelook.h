/* freelook.h - free look for the chase camera (core.nfsc).
 *
 * The right stick swings the chase camera around the car: full left or right
 * puts it in front of the car, looking back at it; up and down tilt it a bit.
 * Holding the right (or middle) mouse button and dragging does the same. Let
 * go and it eases back behind the car. After ThirteenAG's NFSCarbon
 * WidescreenFix Camera.ixx (MIT; see NOTICE), with the camera turning with
 * the car rather than staying put in the world, and the stick's position
 * (not its speed) giving the angle.
 *
 * CubicCameraMover::Update (the player's chase camera) builds its view with
 * eCreateLookAtMatrix 0071b430 (cdecl: bMatrix4 *out, bVector3 *from,
 * bVector3 *to, bVector3 *up; z up), called at 00492e5b; the hook runs only
 * for that call (return address 00492e60) and moves `from` around `to` at
 * the game's own distance. At the call ESI is the mover: +0xac its view
 * (2, 3, 4 the chase views; bumper and hood are fixed views and stay so),
 * byte +0xb1 set while the game's look-back button is held. Other cameras
 * (menus, the race intro, the world map) use other movers and are never
 * touched. Nothing is changed while the game is paused (00699f30
 * GameFlowManager::IsPaused) or while look-back is held; a new race, another
 * view or the car put back on the road starts behind the car again.
 *
 * The stick is the game's own controller state (port 0, read as lights.h and
 * fe_pad.h read it): Z (+0x8) is the right stick's x, Rz (+0x214) its y,
 * 0..65535. The console layout gives the right stick no driving action,
 * except up and down to shift with a manual gearbox; "Left and right only"
 * leaves those alone. The F9 menu takes the controller from the game while it
 * is open, so the camera eases back there too. Mouse presses and motion are
 * taken from the game only while the chase camera runs and a drag is on, and
 * the game's mouse arrow (00585290, as the fix's DrawCursor) is not drawn
 * during a drag. */
#define FL_LOOKAT 0x0071b430u     /* eCreateLookAtMatrix */
#define FL_LOOKAT_RET 0x00492e60u /* after its call in CubicCameraMover::Update */
#define FL_IS_PAUSED 0x00699f30u  /* GameFlowManager::IsPaused (cdecl) -> al */
#define FL_FRAME_TIME 0x00b4d7e8u /* Sim::Internal::mLastFrameTime, seconds */
#define FL_VIEW 0xacu             /* CubicCameraMover: which view */
#define FL_LOOK_BACK 0xb1u        /* CubicCameraMover: byte, look-back held */
#define FL_STATE_RIGHT_X 0x8u     /* the controller state: Z */
#define FL_STATE_RIGHT_Y 0x214u   /* the controller state: Rz */
#define FL_DRAW_CURSOR 0x00585290u /* the game's mouse arrow (cdecl: int x, int y) */
#define FL_DEG 0.0174532925f

static uint32_t g_fl_hooks[2];
static float g_fl_yaw, g_fl_pitch;          /* degrees from the game's camera, as shown */
static float g_fl_mouse_yaw, g_fl_mouse_pitch; /* degrees, the mouse drag's aim */
static float g_fl_to[3];
static int g_fl_to_valid, g_fl_logged;
static uint64_t g_fl_seen_ms;               /* the chase camera last ran (not paused) */
static atomic_int g_fl_drag;                /* right (2) / middle (4) button held for free look */
static atomic_int g_fl_mouse_dx, g_fl_mouse_dy;

static uint64_t fl_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
static float fl_float(uint32_t addr) {
    return bits_float(get_u32(addr));
}
static void fl_reset(void) {
    g_fl_yaw = g_fl_pitch = g_fl_mouse_yaw = g_fl_mouse_pitch = 0.0f;
    g_fl_to_valid = 0;
    atomic_store(&g_fl_mouse_dx, 0);
    atomic_store(&g_fl_mouse_dy, 0);
}
static float fl_clamp(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

/* The right stick, x and y in -1..1 (+y down), outside a small dead zone. */
static void fl_stick(const PopModApi *api, float *x, float *y) {
    *x = *y = 0.0f;
    uint32_t device = get_u32(PAD_DEVICES), state = 0;
    if (!device)
        return;
    const uint32_t getter = get_u32(get_u32(device) + 4);
    if (!getter || api->guest_call(api, getter, device, NULL, 0, &state) != POP_OK || !state)
        return;
    float sx = ((float)get_u32(state + FL_STATE_RIGHT_X) - 32767.5f) / 32767.5f;
    float sy = ((float)get_u32(state + FL_STATE_RIGHT_Y) - 32767.5f) / 32767.5f;
    const float m = sqrtf(sx * sx + sy * sy);
    if (m < 0.2f)
        return;
    const float scale = fminf((m - 0.2f) / 0.8f, 1.0f) / m; /* the dead zone's edge starts at 0 */
    *x = fl_clamp(sx * scale, -1.0f, 1.0f);
    *y = fl_clamp(sy * scale, -1.0f, 1.0f);
}

/* BEFORE eCreateLookAtMatrix, only from CubicCameraMover::Update. */
static void fl_look(const PopModApi *api, pop_cpu_v1 *cpu) {
    const int64_t mode = setting("freelook", 1);
    const uint32_t mover = cpu->esi, from = get_u32(cpu->esp + 8), to = get_u32(cpu->esp + 12);
    const uint64_t now = fl_now_ms();
    if (now - g_fl_seen_ms > 250)
        fl_reset(); /* a new race, or the chase camera back after a menu or a movie */
    const uint32_t view = mover ? get_u32(mover + FL_VIEW) : 0;
    const int chase = mode > 0 && mover && from && to && (view == 2 || view == 3 || view == 4) &&
                      !(get_u32(mover + FL_LOOK_BACK) & 0xffu);
    if (!chase) {
        fl_reset();
        atomic_store(&g_fl_drag, 0);
        return;
    }
    uint32_t paused = 0;
    if (api->guest_call(api, FL_IS_PAUSED, 0, NULL, 0, &paused) == POP_OK && (paused & 0xffu))
        return; /* the game's camera as it is; the look carries on after the pause */
    g_fl_seen_ms = now;

    const float t[3] = {fl_float(to), fl_float(to + 4), fl_float(to + 8)};
    if (g_fl_to_valid && hypotf(t[0] - g_fl_to[0], t[1] - g_fl_to[1]) > 20.0f)
        fl_reset(); /* the car was put somewhere else, e.g. back on the road */
    memcpy(g_fl_to, t, sizeof t);
    g_fl_to_valid = 1;
    const float dt = fl_clamp(fl_float(FL_FRAME_TIME), 0.0f, 0.1f);

    /* Where the look should be: the stick's position, plus the mouse drag while one is held. */
    float sx, sy;
    fl_stick(api, &sx, &sy);
    if (mode == 2)
        sy = 0.0f;
    const float invert = setting("freelook_invert", 0) ? -1.0f : 1.0f;
    if (atomic_load(&g_fl_drag)) {
        const int mdx = atomic_exchange(&g_fl_mouse_dx, 0), mdy = atomic_exchange(&g_fl_mouse_dy, 0);
        g_fl_mouse_yaw = fl_clamp(g_fl_mouse_yaw - (float)mdx * 0.35f, -180.0f, 180.0f);
        g_fl_mouse_pitch = fl_clamp(g_fl_mouse_pitch + invert * (float)mdy * 0.25f, -12.0f, 35.0f);
    } else {
        g_fl_mouse_yaw = g_fl_mouse_pitch = 0.0f;
    }
    /* Stick right swings the camera so the view turns right; full deflection is fully round. Stick
     * down (+y) lifts the camera to look down on the car, up lowers it a little. */
    const float aim_yaw = fl_clamp(-sx * 180.0f + g_fl_mouse_yaw, -180.0f, 180.0f);
    const float sy_deg = invert * sy * (invert * sy > 0.0f ? 35.0f : 12.0f);
    const float aim_pitch = fl_clamp(sy_deg + g_fl_mouse_pitch, -12.0f, 35.0f);
    /* Quick towards a look, gentler on the way back behind the car. */
    const float k_yaw = fabsf(aim_yaw) > fabsf(g_fl_yaw) ? 12.0f : 5.0f;
    const float k_pitch = fabsf(aim_pitch) > fabsf(g_fl_pitch) ? 12.0f : 5.0f;
    g_fl_yaw += (aim_yaw - g_fl_yaw) * (1.0f - expf(-k_yaw * dt));
    g_fl_pitch += (aim_pitch - g_fl_pitch) * (1.0f - expf(-k_pitch * dt));
    if (fabsf(aim_yaw) < 0.01f && fabsf(g_fl_yaw) < 0.05f)
        g_fl_yaw = 0.0f;
    if (fabsf(aim_pitch) < 0.01f && fabsf(g_fl_pitch) < 0.05f)
        g_fl_pitch = 0.0f;
    if (setting("input_trace", 0) && (sx != 0.0f || sy != 0.0f || g_fl_yaw != 0.0f)) {
        static uint64_t said;
        if (now - said > 500) {
            said = now;
            char line[128];
            snprintf(line, sizeof line, "core.nfsc: free look stick %.2f,%.2f look %.0f,%.0f deg", sx, sy, g_fl_yaw,
                     g_fl_pitch);
            log_line(line);
        }
    }
    if (g_fl_yaw == 0.0f && g_fl_pitch == 0.0f)
        return; /* exactly the game's camera */
    if (!g_fl_logged) {
        g_fl_logged = 1;
        log_line("core.nfsc: free look in use");
    }

    /* The game's camera around the car, turned by the look at the same distance. */
    const float f[3] = {fl_float(from), fl_float(from + 4), fl_float(from + 8)};
    const float dx = f[0] - t[0], dy = f[1] - t[1], dz = f[2] - t[2];
    const float flat = hypotf(dx, dy), dist = sqrtf(flat * flat + dz * dz);
    if (dist < 0.01f)
        return;
    const float yaw = atan2f(dy, dx) + g_fl_yaw * FL_DEG;
    const float pitch = fl_clamp(atan2f(dz, flat) + g_fl_pitch * FL_DEG, -10.0f * FL_DEG, 75.0f * FL_DEG);
    put_float(from, t[0] + dist * cosf(pitch) * cosf(yaw));
    put_float(from + 4, t[1] + dist * cosf(pitch) * sinf(yaw));
    put_float(from + 8, t[2] + dist * sinf(pitch));
}

/* The hook, timed for the log while "Log rumble (testing)" (input_trace) is on: what it costs a frame. */
static void fl_lookat(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    if (!setting("input_trace", 0)) {
        fl_look(api, cpu);
        return;
    }
    static uint64_t total_ns, worst_ns, calls, since_ms;
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    fl_look(api, cpu);
    clock_gettime(CLOCK_MONOTONIC, &b);
    const uint64_t ns = (uint64_t)(b.tv_sec - a.tv_sec) * 1000000000u + (uint64_t)b.tv_nsec - (uint64_t)a.tv_nsec;
    total_ns += ns;
    worst_ns = ns > worst_ns ? ns : worst_ns;
    const uint64_t now = fl_now_ms();
    if (!since_ms)
        since_ms = now;
    if (++calls && now - since_ms >= 5000) {
        char line[128];
        snprintf(line, sizeof line, "core.nfsc: free look hook %llu calls, %.1f us each, worst %.1f us (look %.0f deg)",
                 (unsigned long long)calls, (double)total_ns / (double)calls / 1000.0, (double)worst_ns / 1000.0,
                 g_fl_yaw);
        log_line(line);
        total_ns = worst_ns = calls = 0;
        since_ms = now;
    }
}

/* The right or middle mouse button held while the chase camera runs: a free-look drag. The press,
 * the motion while it lasts and the release are the camera's, not the game's. A release (of any
 * button) comes with no button bits and no motion. */
static int32_t fl_mouse(const PopModApi *api, int32_t x, int32_t y, int32_t dx, int32_t dy, int32_t buttons,
                        int32_t wheel, void *user) {
    (void)api;
    (void)x;
    (void)y;
    (void)user;
    if (buttons & 6) {
        if (setting("freelook", 1) > 0 && fl_now_ms() - g_fl_seen_ms < 250) {
            atomic_fetch_or(&g_fl_drag, buttons & 6);
            return 1;
        }
        return 0;
    }
    if (!buttons && !dx && !dy && !wheel) {
        atomic_store(&g_fl_drag, 0);
        return 0;
    }
    if ((dx || dy) && atomic_load(&g_fl_drag)) {
        atomic_fetch_add(&g_fl_mouse_dx, dx);
        atomic_fetch_add(&g_fl_mouse_dy, dy);
        return 1;
    }
    return 0;
}

/* REPLACE 00585290: the game's mouse arrow, left out while the mouse turns the camera. */
static void fl_cursor(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)inv;
    (void)user;
    if (atomic_load(&g_fl_drag)) {
        api->hook_return(api, cpu, cpu->eax, 0);
        return;
    }
    api->call_original(api, cpu->target, cpu);
}

static void freelook_install(const PopModApi *api) {
    uint32_t id;
    if (!api->hook_install_at_callsite ||
        api->hook_install_at_callsite(api, FL_LOOKAT, FL_LOOKAT_RET, fl_lookat, POP_HOOK_BEFORE, NULL,
                                      &g_fl_hooks[0]) != POP_OK) {
        log_line("core.nfsc: free look unavailable (hook 0071b430)");
        return;
    }
    api->on_mouse(api, fl_mouse, NULL, &id);
    if (api->hook_install(api, FL_DRAW_CURSOR, fl_cursor, POP_HOOK_REPLACE, NULL, &g_fl_hooks[1]) != POP_OK)
        log_line("core.nfsc: free look: the mouse arrow stays during a drag (hook 00585290)");
    log_line("core.nfsc: free look ready (right stick, or drag with the right mouse button)");
}
