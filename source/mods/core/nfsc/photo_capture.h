/* Replace the original Photo Mode shutter's EA sign-in/upload transition.
 * Camera movement and Photo Mode entry/exit remain owned by the game. */
#ifdef __EMSCRIPTEN__
static uint32_t g_photo_capture_hooks[4];
static uint32_t g_photo_capture_pending;
static uint32_t g_photo_capture_owner, g_photo_capture_manager, g_photo_capture_serial;
static uint64_t g_photo_capture_started;

static void photo_capture_exit(const PopModApi *api, pop_cpu_v1 *cpu,
                               PopHookInvocation *inv, void *user) {
    (void)api; (void)inv; (void)user;
    if (cpu->ecx == g_photo_capture_owner || cpu->ecx == g_photo_capture_manager)
        __atomic_store_n(&g_photo_capture_pending, 0, __ATOMIC_RELEASE);
}

static int photo_capture_active(void) {
    if (!__atomic_load_n(&g_photo_capture_pending, __ATOMIC_ACQUIRE)) return 0;
    if (!g_photo_capture_owner || get_u32(PHOTO_ZOOM_OWNER) != g_photo_capture_owner ||
        fl_now_ms() - g_photo_capture_started > 2000u) {
        __atomic_store_n(&g_photo_capture_pending, 0, __ATOMIC_RELEASE);
        return 0;
    }
    return 1;
}

static void photo_capture_ui(const PopModApi *api, pop_cpu_v1 *cpu,
                             PopHookInvocation *inv, void *user) {
    (void)user;
    if (photo_capture_active()) {
        api->hook_return(api, cpu, cpu->eax, 4u);
        return;
    }
    api->call_next(api, inv, cpu);
}

static void photo_capture_take(const PopModApi *api, pop_cpu_v1 *cpu,
                               PopHookInvocation *inv, void *user) {
    (void)inv; (void)user;
    /* 0057b8f0 accepts the shutter only in state 0. Its original body
     * selects EA login (state 8) or the online upload path (state 6). */
    if (!get_u32(cpu->ecx + 4u) && !photo_capture_active()) {
        g_photo_capture_started = fl_now_ms();
        g_photo_capture_owner = get_u32(PHOTO_ZOOM_OWNER);
        g_photo_capture_manager = cpu->ecx;
        if (!++g_photo_capture_serial) ++g_photo_capture_serial;
        __atomic_store_n(&g_photo_capture_pending, g_photo_capture_serial, __ATOMIC_RELEASE);
        MAIN_THREAD_ASYNC_EM_ASM({
            var memory = wasmMemory;
            var ptr = $0 >>> 2;
            var serial = $1 >>> 0;
            var pending = function() { return Atomics.load(new Uint32Array(memory.buffer), ptr) === serial; };
            var release = function() { Atomics.compareExchange(new Uint32Array(memory.buffer), ptr, serial, 0); };
            /* Let the game present a frame with its Photo Mode controls hidden.
             * Copy the pixels before releasing the native UI suppression. */
            setTimeout(function() {
                requestAnimationFrame(function() {
                    if (!pending()) return;
                    try {
                        var canvas = document.getElementById('canvas');
                        if (!canvas || !canvas.width || !canvas.height)
                            throw new Error('The game view is not ready.');
                        var out = document.createElement('canvas');
                        out.width = canvas.width; out.height = canvas.height;
                        out.getContext('2d').drawImage(canvas, 0, 0);
                        release();
                        out.toBlob(function(blob) {
                            if (!blob) { alert('Could not save the photo. Please try again.'); return; }
                            var url = URL.createObjectURL(blob);
                            var link = document.createElement('a');
                            link.href = url;
                            link.download = 'Carbon-' + new Date().toISOString().replace(/[:.]/g, '-') + '.png';
                            document.body.appendChild(link); link.click(); link.remove();
                            setTimeout(function() { URL.revokeObjectURL(url); }, 30000);
                        }, 'image/png');
                    } catch (error) {
                        release();
                        console.error('Photo Mode capture failed', error);
                        alert('Could not save the photo. Please try again.');
                    }
                });
            }, 500);
        }, (uint32_t)(uintptr_t)&g_photo_capture_pending, g_photo_capture_serial);
    }
    api->hook_return(api, cpu, cpu->eax, 0u);
}

static void photo_capture_install(void) {
    if (g_api->hook_install(g_api, 0x005bc010u, photo_capture_exit,
            POP_HOOK_BEFORE, NULL, &g_photo_capture_hooks[2]) != POP_OK) return;
    /* Back starts a screen transition before the camera owner is destroyed. */
    if (g_api->hook_install(g_api, 0x005b3d40u, photo_capture_exit,
            POP_HOOK_BEFORE, NULL, &g_photo_capture_hooks[3]) != POP_OK) goto fail;
    if (g_api->hook_install(g_api, 0x005b0920u, photo_capture_ui,
            POP_HOOK_WRAP, NULL, &g_photo_capture_hooks[0]) != POP_OK) goto fail;
    if (g_api->hook_install(g_api, 0x0057b8f0u, photo_capture_take,
            POP_HOOK_WRAP, NULL, &g_photo_capture_hooks[1]) != POP_OK) goto fail;
    return;
fail:
    for (unsigned i = 0; i < 4; ++i) {
        if (g_photo_capture_hooks[i]) g_api->hook_remove(g_api, g_photo_capture_hooks[i]);
        g_photo_capture_hooks[i] = 0;
    }
    log_line("core.nfsc: local Photo Mode capture unavailable");
}
static void photo_capture_shutdown(void) {
    __atomic_store_n(&g_photo_capture_pending, 0, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < 4; ++i) {
        if (g_photo_capture_hooks[i])
            g_api->hook_remove(g_api, g_photo_capture_hooks[i]);
        g_photo_capture_hooks[i] = 0;
    }
}
#else
static void photo_capture_install(void) {}
static void photo_capture_shutdown(void) {}
#endif
