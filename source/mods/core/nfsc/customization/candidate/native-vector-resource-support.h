#include "native-vector-resource-entries.h"
static uint32_t g_cust_vector_resource_index;
static int g_cust_vector_resource_failure_logged;
static int cust_vector_resources_failure(const char *message) {
    if (!g_cust_vector_resource_failure_logged) {
        log_line(message);
        g_cust_vector_resource_failure_logged = 1;
    }
    return 0;
}
static int cust_vector_resources_install(void) {
    if (g_cust_vector_resource_index) return 1;
    if (!cust_catalog_ready() || !cust_pilot_enabled()) return 0;
    const char *asset = getenv("RECOMP_CUSTOM_VECTOR_RESOURCE_SHA256");
    if (!asset || strcmp(asset, "aa21d7944dea5e72c28aba39d3a936d7179866a7d6aec504f0c78666fba51a81"))
        return cust_vector_resources_failure("customization pilot: vector resource asset SHA guard is absent or mismatched");
    const uint32_t old_index = get_u32(0x00a7aad4u);
    if (old_index != 0x00a77850u || get_u32(0x00a7aad8u) != 661u)
        return cust_vector_resources_failure("customization pilot: vector resource index rejected unexpected original pointer or count");
    void *old_memory = NULL;
    if (g_api->guest_ptr(g_api, old_index, 661u * 12u, &old_memory) != POP_OK || !old_memory)
        return cust_vector_resources_failure("customization pilot: vector resource original index is unreadable");
    uint32_t checksum = 2166136261u;
    for (uint32_t i = 0; i < 661u * 12u; ++i)
        checksum = (checksum ^ ((const uint8_t *)old_memory)[i]) * 16777619u;
    if (checksum != 0xceaa159au)
        return cust_vector_resources_failure("customization pilot: vector resource original index signature mismatch");

    uint32_t new_index = 0;
    if (g_api->guest_alloc(g_api, 698u * 12u, &new_index) != POP_OK || !new_index)
        return cust_vector_resources_failure("customization pilot: vector resource index allocation failed");
    void *new_memory = NULL;
    if (g_api->guest_ptr(g_api, new_index, 698u * 12u, &new_memory) != POP_OK || !new_memory) {
        g_api->guest_free(g_api, new_index);
        return cust_vector_resources_failure("customization pilot: vector resource allocated index is inaccessible");
    }
    memcpy(new_memory, old_memory, 661u * 12u);
    for (uint32_t i = 0; i < 37u; ++i)
        for (uint32_t field = 0; field < 3u; ++field)
            if (g_api->guest_write_u32(g_api, new_index + (661u + i) * 12u + field * 4u,
                                      cust_vector_resources[i][field]) != POP_OK) {
                g_api->guest_free(g_api, new_index);
                return cust_vector_resources_failure("customization pilot: vector resource index population failed");
            }
    if (g_api->guest_write_u32(g_api, 0x00a7aad4u, new_index) != POP_OK) {
        g_api->guest_free(g_api, new_index);
        return cust_vector_resources_failure("customization pilot: vector resource index pointer publication failed");
    }
    if (g_api->guest_write_u32(g_api, 0x00a7aad8u, 698u) != POP_OK) {
        g_api->guest_write_u32(g_api, 0x00a7aad4u, old_index);
        g_api->guest_free(g_api, new_index);
        return cust_vector_resources_failure("customization pilot: vector resource index count publication failed");
    }
    g_cust_vector_resource_index = new_index;
    log_line("customization pilot: vector resource index preserves 661 entries and adds 37 aligned resources");
    return 1;
}

static void cust_vector_resources_shutdown(void) {
    const uint32_t owned_index = g_cust_vector_resource_index;
    if (!owned_index) return;
    if (get_u32(0x00a7aad4u) == owned_index) {
        /* Lower the count before restoring the shorter original table. */
        if (g_api->guest_write_u32(g_api, 0x00a7aad8u, 661u) != POP_OK ||
            g_api->guest_write_u32(g_api, 0x00a7aad4u, 0x00a77850u) != POP_OK) {
            log_line("customization pilot: vector resource index restoration failed; allocation retained");
            return;
        }
    }
    if (g_api->guest_free(g_api, owned_index) == POP_OK)
        g_cust_vector_resource_index = 0u;
}
