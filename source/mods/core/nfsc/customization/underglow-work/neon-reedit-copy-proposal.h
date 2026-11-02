/* Private imported exhaust re-edit repair. Replaces existing cust_neon_as_edit_copy
 * callback; keep existing853350 registration. No new hook slot.
 * All private/catalog/car gates must pass before imported-part lookup.
 * Native853350 copies region then clears destination+20 Zones. Saved imported
 * BORRILLO source list has region8/Zone1=1; editor copy Zones=NULL/UI0.
 * Native8433D0 returns independent44-byte Zones copy; native8533F0 frees it.
 * Preserve original native exhaust behavior; only importedslot2F/region8 added.
 * No LOD changes: author optionalLOD fix also expands count11→45 and target
 * index mapping. This proposal addresses proven re-edit hydration only.
 * Parent review/build and normalUI proof required. No runtime PASS claimed.
 */
static void cust_neon_as_edit_copy(const PopModApi *api,pop_cpu_v1 *cpu,
                                   PopHookInvocation *inv,void *user) {
    (void)user;
    const uint32_t destination=cpu->ecx,source=get_u32(cpu->esp+4u);
    const uint32_t caller=get_u32(cpu->esp);
    uint32_t source_part=0u,source_slot=0xffffffffu,source_region=0u,source_zones=0u;
    int eligible=0;
    if(cust_neon_as_enabled() && caller==0x00859320u && source) {
        source_part=get_u32(source+0x10u);
        source_slot=get_u32(source+0x18u);
        source_region=get_u32(source+0x1cu);
        source_zones=get_u32(source+0x20u);
        eligible=source_zones && (source_slot==0x68u ||
            (source_slot==0x2fu && source_region==8u && source_part &&
             cust_extension_part(source_part)));
    }
    const PopModStatus status=api->call_next(api,inv,cpu);
    if(status!=POP_OK || !eligible || !destination ||
       get_u32(destination+0x18u)!=source_slot ||
       get_u32(destination+0x10u)!=source_part ||
       get_u32(destination+0x1cu)!=source_region ||
       get_u32(destination+0x20u) ||
       get_u32(source+0x10u)!=source_part ||
       get_u32(source+0x20u)!=source_zones)return;
    /* Native helper allocates an owned copy; source and destination never alias.
     * Native source remains list-owned and its pointer was captured before copy. */
    const uint32_t copied=cust_call(0x008433d0u,source,NULL,0u);
    if(copied)api->guest_write_u32(api,destination+0x20u,copied);
}
