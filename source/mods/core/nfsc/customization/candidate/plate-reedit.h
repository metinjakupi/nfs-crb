/* Native853350 re-edit clears Zones. Copy through native8433D0 so editor
 * owns its44-byte zones, while list/cart ownership remains unchanged. */
static void cust_plate_as_edit_copy(const PopModApi *api,pop_cpu_v1 *cpu,
                                   PopHookInvocation *inv,void *user) {
    (void)user;
    const uint32_t destination=cpu->ecx,source=get_u32(cpu->esp+4u);
    const uint32_t caller=get_u32(cpu->esp);
    uint32_t source_part=0u,source_slot=0xffffffffu,source_region=0u,source_zones=0u;
    int eligible=0;
    if(cust_plate_as_enabled() && caller==0x00859320u && source) {
        source_part=get_u32(source+0x10u);
        source_slot=get_u32(source+0x18u);
        source_region=get_u32(source+0x1cu);
        source_zones=get_u32(source+0x20u);
        eligible=source_zones && source_slot==0x56u && source_region==15u && cust_plate_part_kind(source_part)>=0;
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
