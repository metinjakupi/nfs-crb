/* EC InitializeEverythingHooks.h: request permanent tyre resources after the
 * native InitializeEverything return. Resource loading and lifetime belong to
 * the engine. The exact catalog is checked later by the tyre model/draw paths;
 * it is not guaranteed to exist at this initialization boundary. */
static uint32_t g_cust_tire_resource_requested;
static uint32_t g_cust_tire_resources[2];
static uint32_t cust_tire_request_resource(const char *path) {
    const uint32_t length = (uint32_t)strlen(path) + 1u;
    uint32_t guest_path = 0u, resource = 0u;
    void *memory = NULL;
    if (g_api->guest_alloc(g_api, length, &guest_path) != POP_OK) return 0u;
    if (g_api->guest_ptr(g_api, guest_path, length, &memory) != POP_OK || !memory) {
        g_api->guest_free(g_api, guest_path);
        return 0u;
    }
    memcpy(memory, path, length);
    const uint32_t args[7] = {guest_path, 6u, 0u, 0u, 0u, 0u, 0u};
    const PopModStatus status = g_api->guest_call(g_api, 0x006b5980u, 0u, args, 7u, &resource);
    /* Native ResourceFile duplicates the path into its own storage. */
    g_api->guest_free(g_api, guest_path);
    const uint32_t bytes = status == POP_OK && resource ? get_u32(resource + 0x40u) : 0u;
    char line[240];
    snprintf(line, sizeof line, "customization tyres: resource requested path=%s status=%d handle=%08x bytes=%u pending=%u",
             path, (int)status, resource, bytes, get_u32(0x00a995d8u));
    log_line(line);
    return bytes ? resource : 0u;
}
static void cust_tire_after_initialize(const PopModApi *api, pop_cpu_v1 *cpu,
                                      PopHookInvocation *inv, void *user) {
    (void)user;
    if (api->call_next(api, inv, cpu) != POP_OK || !cust_pilot_enabled() ||
        g_cust_tire_resource_requested) return;
    g_cust_tire_resource_requested = 1u;
    g_cust_tire_resources[0] = cust_tire_request_resource("CARS\\TIRES\\GEOMETRY.BIN");
    g_cust_tire_resources[1] = cust_tire_request_resource("CARS\\TIRES\\TEXTURES.BIN");
    if (!g_cust_tire_resources[0] || !g_cust_tire_resources[1])
        log_line("customization tyres: resource request failed; original wheel fallback retained");
}
