/* Private EC camera bridge. Requires authored camera rows staged together.
 * Diagnostic mode uses the existing wheel camera only for fault isolation. */
#ifndef CUST_CAMERA_DISCRIMINATOR
#define CUST_CAMERA_DISCRIMINATOR 0
#endif
static uint32_t g_cust_camera_hooks[2];
static const char *cust_camera_name(uint32_t slot) {
    switch(slot) {
    case 0x56u:return g_cust_plate_ready&&cust_plate_menu_supported()?"FeLicensePlate.fng":NULL;
    case 0x1eu:return cust_body_tail_supported(cust_car_id())?"FeBrakeLights.fng":NULL;
    case 0x22u:return cust_body_mirror_supported(cust_car_id())?"FeLeftMirror.fng":NULL;
    case 0x18u:case 0x19u:return "FeFrontBrake.fng";
    case 0x55u:return "FeRearWheels.fng";
    case 0x64u:return "FeFrontDecal.fng";
    case 0x65u:return "FeRearDecal.fng";
    case 0x67u:return "FeTires.fng";
    case 0x68u:return "FeNeon.fng";
    case 0x2du:return "FeNeon.fng";
    case 0x5eu:return "FeNeon.fng";
    default:return NULL;
    }
}
static int cust_camera_write(const PopModApi *api,uint32_t dst,uint32_t capacity,const char *name) {
    if(!dst || !capacity || capacity>4096u || !name)return 0;
    void *p=NULL;if(api->guest_ptr(api,dst,capacity,&p)!=POP_OK || !p)return 0;
    size_t n=strlen(name);if(n>=capacity)n=capacity-1u;memcpy(p,name,n);((char*)p)[n]=0;return 1;
}
static void cust_camera_parts(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
    (void)user;
    const uint32_t menu=cpu->ecx,option=get_u32(menu+0x34u),capacity=get_u32(cpu->esp+8u);
    if(cust_pilot_enabled() && cust_catalog_ready() && cust_supported() && option && capacity>0u &&
       (get_u32(menu+0x2e0u)&255u) && (int32_t)get_u32(menu+0x2c4u)<=(int32_t)get_u32(0x00a99c60u)) {
        const char *name=cust_camera_name(get_u32(option+0x30u));
        if(name && cust_camera_write(api,get_u32(cpu->esp+4u),capacity,CUST_CAMERA_DISCRIMINATOR?"FeAutosculptWheel.fng":name)) {
            api->hook_return(api,cpu,0u,8u);return;
        }
    }
    api->call_next(api,inv,cpu);
}
static void cust_camera_autosculpt(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
    (void)user;
    const char *name=NULL;
    if(get_u32(cpu->esp)==0x0084eb3fu){
        if(cust_neon_as_current())name="FeNeon.fng";
        else if(g_cust_plate_ready&&cust_plate_as_current())name="FeLicensePlate.fng";
    }
    if(name) {
        uint32_t dst=get_u32(cpu->esp+4u),capacity=get_u32(cpu->esp+12u);
        if(cust_camera_write(api,dst,capacity,CUST_CAMERA_DISCRIMINATOR?"FeAutosculptWheel.fng":name)){
            api->hook_return(api,cpu,dst,0u);return;
        }
    }
    api->call_next(api,inv,cpu);
}
static int cust_camera_install(void) {
    if(!cust_pilot_enabled())return 0;
    if(g_api->hook_install(g_api,0x00840040u,cust_camera_parts,POP_HOOK_REPLACE,NULL,&g_cust_camera_hooks[0])!=POP_OK)return 0;
    if(g_api->hook_install(g_api,0x00471230u,cust_camera_autosculpt,POP_HOOK_REPLACE,NULL,&g_cust_camera_hooks[1])!=POP_OK){g_api->hook_remove(g_api,g_cust_camera_hooks[0]);g_cust_camera_hooks[0]=0;return 0;}
    log_line(CUST_CAMERA_DISCRIMINATOR?"customization: diagnostic existing wheel cameras installed":"customization: authored parts and underglow cameras installed");return 1;
}
