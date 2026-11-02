/* Private EC neon Autosculpt bridge. Include after underglow-render.h.
 * Runtime verification PENDING. Root owns main menu addition; add CV category
 * with isAutosculpt=1, slot0x68, labelCUST_PARTS_NEON, filter=current car.
 * This header extends only CV; original parts forward to their native functions.
 */
static uint32_t g_cust_neon_as_hooks[13];
static int cust_neon_as_enabled(void) {
    const uint32_t car=get_u32(0x00b77970u);
    return cust_pilot_enabled() && cust_catalog_ready() && car && car!=0x00b77970u && CUST_NEON_CAR_SUPPORTED(get_u32(car+8u));
}
static uint32_t cust_neon_as_current(void) {
    uint32_t cur=get_u32(0x00bbb050u);
    return cust_neon_as_enabled() && cur && get_u32(cur+0x18u)==0x68u ? cur : 0u;
}
static void cust_neon_as_region(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
    (void)user;if(cust_neon_as_enabled() && get_u32(cpu->esp+4u)==0x68u){api->hook_return(api,cpu,14u,0u);return;}api->call_next(api,inv,cpu);
}
static void cust_neon_as_fng(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
    (void)user;if(cust_neon_as_enabled() && get_u32(cpu->esp+4u)==0x68u){api->hook_return(api,cpu,0x009f79c0u,4u);return;}api->call_next(api,inv,cpu);
}
static void cust_neon_as_state(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
    (void)user;if(cust_neon_as_enabled() && get_u32(cpu->esp+4u)==0x68u){api->hook_return(api,cpu,4u,4u);return;}api->call_next(api,inv,cpu);
}
static void cust_neon_as_active(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
    (void)user;uint32_t cur=cust_neon_as_current();if(cur && get_u32(cpu->esp)==0x0085b3a5u){uint32_t count=cust_attr(get_u32(cur+0x10u),cust_hash("MORPHTARGET_NUM"),0u);if(count>0u && count<=7u){api->hook_return(api,cpu,count,4u);return;}}api->call_next(api,inv,cpu);
}
static void cust_neon_as_fabricatable(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
    (void)user;if(cust_neon_as_enabled() && (get_u32(cpu->ecx+0x18u)==0x68u || (get_u32(cpu->ecx+0x18u)==0x2fu && cust_extension_part(get_u32(cpu->ecx+0x10u)) && getenv("RECOMP_CUSTOM_COSMETICS_SHOP") && !strcmp(getenv("RECOMP_CUSTOM_COSMETICS_SHOP"),"1")))){api->hook_return(api,cpu,1u,0u);return;}api->call_next(api,inv,cpu);
}
static void cust_neon_as_category(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
    (void)user;if(cust_neon_as_enabled() && get_u32(cpu->ecx+0x18u)==0x68u){api->hook_return(api,cpu,cust_hash("CUST_PARTS_NEON"),0u);return;}api->call_next(api,inv,cpu);
}
static void cust_neon_as_select_region(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
    (void)user;uint32_t cur=cust_neon_as_current();api->call_next(api,inv,cpu);if(cur && get_u32(0x00bbb050u)==cur)api->guest_write_u32(api,cur+0x1cu,14u);
}
static void cust_neon_as_zone_name(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
    (void)user;uint32_t cur=cust_neon_as_current();if(cur && get_u32(cpu->esp)==0x0085b3dbu){uint32_t seed=cust_attr(get_u32(cur+0x10u),cust_hash("ZONE_HASH"),0u),num=get_u32(cpu->esp+8u);if(seed && num>=1u && num<=7u){char s[16];snprintf(s,sizeof(s),"%u",num);api->hook_return(api,cpu,cust_hash_seed(s,seed),0u);return;}}api->call_next(api,inv,cpu);
}
static float cust_neon_as_float(uint32_t part,const char *name,float fallback) {
    uint32_t args[2]={cust_hash(name),0u},p=cust_call(0x007b00e0u,part,args,2u);float f=p?bits_float(get_u32(p+4u)):fallback;return isfinite(f)?f:fallback;
}
static void cust_neon_as_slider(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
    (void)user;uint32_t cur=cust_neon_as_current(),slider=cpu->ecx,caller=get_u32(cpu->esp);api->call_next(api,inv,cpu);if(!cur || caller!=0x0085afe1u)return;
    uint32_t num=get_u32(slider+0x48u)+1u,part=get_u32(cur+0x10u);if(num<1u || num>7u)return;
    char name[32];float amount=cust_neon_as_float(part,"ZONE_AMOUNT",1.0f);snprintf(name,sizeof(name),"ZONE_AMOUNT%u",num);amount=cust_neon_as_float(part,name,amount);snprintf(name,sizeof(name),"ZONE_STEP%u",num);float step=cust_neon_as_float(part,name,0.01f);
    if(amount>0.0f && amount<=1.0f)put_float(slider+0x20u,amount);
    /* Author uses zero step for continuous hue sliders. */
    if(step>=0.0f && step<=1.0f)put_float(slider+0x54u,step);
}
static uint32_t cust_neon_as_zones(uint32_t selectable,uint32_t part) {
    const uint32_t slotArgs[1]={0x68u};uint32_t cart=cust_call(0x00850190u,0x00bbaba8u,slotArgs,1u);
    if(cart){uint32_t existing=get_u32(cart+12u);if(existing && get_u32(existing+0x10u)==part && cust_attr(part,cust_hash("MORPHTARGET_NUM"),0u)){cust_call(0x008434a0u,existing,NULL,0u);uint32_t cur=get_u32(0x00bbb050u);return cur?cust_call(0x008433d0u,cur,NULL,0u):0u;}}
    else {uint32_t installed=cust_call(0x0083f850u,0x00bbaba8u,slotArgs,1u);if(installed==part && !cust_attr(part,cust_hash("STOCK"),0u)){const uint32_t regionArgs[1]={14u};return cust_call(0x0049c860u,0x00bbabc0u,regionArgs,1u);}}
    (void)selectable;return 0u;
}
static void cust_neon_as_list(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
    (void)user;uint32_t slot=get_u32(cpu->esp+4u),head=get_u32(cpu->esp+8u);if(!cust_neon_as_enabled() || slot!=0x68u){api->call_next(api,inv,cpu);return;}
    uint32_t car=get_u32(get_u32(0x00b77970u)+8u),part=0u;
    for(unsigned seen=0;seen<4096u;++seen){part=cust_next(0x68u,car,part);if(!part)break;
        int stock=cust_attr(part,cust_hash("STOCK"),0u)!=0u;uint32_t morph=cust_attr(part,cust_hash("MORPHTARGET_NUM"),0u);
        if(!stock && (!morph || morph>7u))continue;
        const uint32_t allocation[1]={0x24u};uint32_t obj=cust_call(0x006a1560u,0u,allocation,1u);void *mem=NULL;if(!obj || api->guest_ptr(api,obj,0x24u,&mem)!=POP_OK || !mem)break;memset(mem,0,0x24u);
        api->guest_write_u32(api,obj,0x009f9f48u);api->guest_write_u32(api,obj+0xcu,0x881f8efau);api->guest_write_u32(api,obj+0x10u,part);api->guest_write_u32(api,obj+0x14u,0x009f9cd8u);api->guest_write_u32(api,obj+0x18u,0x68u);api->guest_write_u32(api,obj+0x1cu,14u);api->guest_write_u32(api,obj+0x20u,cust_neon_as_zones(obj,part));
        uint32_t node=obj+4u,tail=get_u32(head+4u);api->guest_write_u32(api,node,head);api->guest_write_u32(api,node+4u,tail);api->guest_write_u32(api,tail,node);api->guest_write_u32(api,head+4u,node);
    }api->hook_return(api,cpu,0u,0u);
}
/* Preserve original roof-family pose semantics while excluding neon controls
 * from geometry deformation. Native region14 is a copy of roof region7.
 * Restrict to a RideInfo with an actual staged neon texture, not every car. */
static void cust_neon_as_pose(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
    (void)user;uint32_t ride=get_u32(cpu->esp+4u),as=cpu->ecx;
    int eligible=cust_pilot_enabled() && cust_catalog_ready() && ride && as==ride+0x7fcu && CUST_NEON_CAR_SUPPORTED(get_u32(ride));
    uint32_t part=eligible?get_u32(ride+0x54u+0x68u*4u):0u,texture=part?cust_attr(part,cust_hash("TEXTURE_NAME"),0u):0u;
    if(texture!=cust_hash("CARSHADOW_NEON") && texture!=cust_hash("CARSHADOW_LED")){api->call_next(api,inv,cpu);return;}
    uint32_t saved[12];for(unsigned i=0;i<12u;++i){saved[i]=get_u32(as+4u+14u*48u+i*4u);api->guest_write_u32(api,as+4u+14u*48u+i*4u,get_u32(as+4u+7u*48u+i*4u));}
    api->call_next(api,inv,cpu);
    for(unsigned i=0;i<12u;++i)api->guest_write_u32(api,as+4u+14u*48u+i*4u,saved[i]);
}
static void cust_neon_as_pack(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
    (void)user;uint32_t out=cpu->ecx,ride=get_u32(cpu->esp+4u);int eligible=cust_pilot_enabled() && cust_catalog_ready() && ride && CUST_NEON_CAR_SUPPORTED(get_u32(ride));uint32_t part=eligible?get_u32(ride+0x54u+0x68u*4u):0u;api->call_next(api,inv,cpu);if(!part || !cust_attr(part,cust_hash("MORPHTARGET_NUM"),0u))return;
    void *p=NULL;if(api->guest_ptr(api,out+0x345u+14u*11u,11u,&p)!=POP_OK || !p)return;for(unsigned i=0;i<11u;++i)((uint8_t*)p)[i]=(uint8_t)(cust_neon_clamp(bits_float(get_u32(ride+0xaa0u+i*4u)))*100.0f+0.5f);
}
#include "neon-reedit-copy-proposal.h"
static int cust_neon_as_install(void) {
    if(!cust_pilot_enabled())return 0;
    const struct{uint32_t addr;PopHookFn fn;int mode;}hooks[]={
        {0x00843180u,cust_neon_as_region,POP_HOOK_REPLACE},{0x00846f20u,cust_neon_as_fng,POP_HOOK_REPLACE},{0x00846fe0u,cust_neon_as_state,POP_HOOK_REPLACE},{0x007b9ae0u,cust_neon_as_active,POP_HOOK_REPLACE},{0x00843270u,cust_neon_as_fabricatable,POP_HOOK_REPLACE},{0x008432f0u,cust_neon_as_category,POP_HOOK_REPLACE},{0x00854700u,cust_neon_as_select_region,POP_HOOK_WRAP},{0x00853350u,cust_neon_as_edit_copy,POP_HOOK_WRAP},{0x005711c0u,cust_neon_as_zone_name,POP_HOOK_REPLACE},{0x005733e0u,cust_neon_as_slider,POP_HOOK_WRAP},{0x008534a0u,cust_neon_as_list,POP_HOOK_REPLACE},{0x007d8990u,cust_neon_as_pose,POP_HOOK_WRAP},{0x007c4040u,cust_neon_as_pack,POP_HOOK_WRAP}
    };
    for(unsigned i=0;i<sizeof(hooks)/sizeof(hooks[0]);++i){if(g_api->hook_install(g_api,hooks[i].addr,hooks[i].fn,hooks[i].mode,NULL,&g_cust_neon_as_hooks[i])==POP_OK)continue;while(i){--i;g_api->hook_remove(g_api,g_cust_neon_as_hooks[i]);g_cust_neon_as_hooks[i]=0u;}return 0;}
    log_line("customization: native underglow Autosculpt bridge installed");return 1;
}
