/* Extend native exhaust AS enumeration only for the seven newly imported
 * center styles whose current car actually has its native center attachment.
 * Include after cust_body_model_matches and customization helpers.
 * Native EXHAUST menu/region8/state7/FNG/storage/pose remain unchanged.
 * Runtime verification pending. No part identity, price or stored attr changes.
 */
static uint32_t g_cust_exhaust_center_hook;
static int cust_exhaust_new_center(uint32_t part) {
    const uint32_t name=cust_call(0x007cd930u,part,NULL,0u);
    char text[64];
    for(unsigned style=18u;style<=24u;++style){
        snprintf(text,sizeof(text),"EXHAUST_STYLE%u_CENTER_LEVEL1",style);
        if(name==cust_hash(text))return 1;
    }
    return 0;
}
static int cust_exhaust_center_attached(uint32_t part) {
    if(!cust_pilot_enabled() || !cust_catalog_ready() || !cust_supported() || !cust_exhaust_new_center(part))return 0;
    const uint32_t front=get_u32(0x00b77970u);
    if(!front || front==0x00b77970u)return 0;
    const uint32_t ride=front+8u,car=get_u32(ride),ri=get_u32(front+0xbf8u);
    if(!ri || get_u32(ri+0x3f0u)!=ride || !cust_body_model_matches(part,0x2fu,car))return 0;
    /* Original007da15d/007da164 finds marker94cec8a1 and stores it at
     * CarRenderInfo+434; source CarRenderInfo::Markers.CenterExhaust agrees.
     * A marker from an unrelated car or missing bumper is not sufficient. */
    const uint32_t marker=get_u32(ri+0x434u);
    if(!marker || get_u32(marker)!=0x94cec8a1u)return 0;
    for(unsigned axis=0;axis<3u;++axis){
        float f=bits_float(get_u32(marker+0x40u+axis*4u));
        if(!isfinite(f) || f < -20.0f || f > 20.0f)return 0;
    }
    return 1;
}
static void cust_exhaust_center_attribute(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
    (void)user;
    const uint32_t part=cpu->ecx;
    const int listCheck=get_u32(cpu->esp)==0x0085361du && get_u32(cpu->esp+4u)==cust_hash("CENTER");
    api->call_next(api,inv,cpu);
    /* Native AS list's blanket CENTER==1 rejection is the only override.
     * All rendering/attachment/serialization callers still see CENTER1. */
    if(listCheck && cpu->eax==1u && cust_exhaust_center_attached(part))cpu->eax=0u;
}
static int cust_exhaust_center_install(void) {
    if(!cust_pilot_enabled())return 0;
    if(g_api->hook_install(g_api,0x007c27c0u,cust_exhaust_center_attribute,POP_HOOK_WRAP,NULL,&g_cust_exhaust_center_hook)!=POP_OK)return 0;
    log_line("customization: new center exhaust AS choices require native attachment");return 1;
}
