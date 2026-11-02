/* Private EC underglow renderer. Include after customization helpers.
 * Requires exact staged catalog gate and per-car eligibility supplied by owner.
 * EC v4.0.5: Neon.h / AmbientShadow.cpp. No shared renderer changes.
 * RUNTIME VERIFICATION PENDING. Does not add menu entries or grant parts.
 */
#ifndef CUST_NEON_CAR_SUPPORTED
#define CUST_NEON_CAR_SUPPORTED(car) ((car) == 17u)
#endif
static uint32_t g_cust_neon_hooks[2], g_cust_neon_draw_color;
static int g_cust_neon_drawing;
typedef struct { uint32_t ri, ride, part; float value, direction; } CustNeonPulse;
static CustNeonPulse g_cust_neon_pulses[32];
static unsigned g_cust_neon_cursor;
static float cust_neon_clamp(float f) {
    return !isfinite(f) || f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
}
static uint32_t cust_neon_byte(float f) {
    return !isfinite(f) || f <= 0.0f ? 0u : f >= 255.0f ? 255u : (uint32_t)f;
}
/* EC Color::Bytes[0..2] are set to r/g/b, so retain that byte convention. */
static void cust_neon_hsv(float h, float s, float v, float out[3]) {
    h = cust_neon_clamp(h) * 0.9999f * 6.0f;
    s = 1.0f - cust_neon_clamp(s); v = 1.0f - cust_neon_clamp(v);
    int sector = (int)h; float f = h - (float)sector;
    float p = v * (1.0f-s), q = v * (1.0f-s*f), t = v * (1.0f-s*(1.0f-f));
    switch (sector) {
    case 0: out[0]=v;out[1]=t;out[2]=p;break;
    case 1: out[0]=q;out[1]=v;out[2]=p;break;
    case 2: out[0]=p;out[1]=v;out[2]=t;break;
    case 3: out[0]=p;out[1]=q;out[2]=v;break;
    case 4: out[0]=t;out[1]=p;out[2]=v;break;
    default:out[0]=v;out[1]=p;out[2]=q;break;
    }
    for (int i=0;i<3;++i) out[i]*=255.0f;
}
static float cust_neon_zone(uint32_t ride, unsigned zone) {
    /* RideInfo Autosculpt at +0x7fc, IsInitialized 4, region14 at +0xaa0.
     * Source struct: 21 regions of 48 bytes; native 007ca119/007de6a5
     * independently address RideInfo+0x7fc and 007ce167 uses +0x800. */
    return cust_neon_clamp(bits_float(get_u32(ride + 0xaa0u + zone*4u)));
}
static float cust_neon_dt(void) {
    uint32_t sim=get_u32(0x00b4cfe0u);
    float dt=bits_float(get_u32(get_u32(0x00a99bbcu)==6u && sim ? sim+20u : 0x00a99a5cu));
    return isfinite(dt) && dt>=0.0f && dt<=0.25f ? dt : 0.0f;
}
static CustNeonPulse *cust_neon_pulse(uint32_t ri,uint32_t ride,uint32_t part) {
    for(unsigned i=0;i<32u;++i) {
        CustNeonPulse *p=&g_cust_neon_pulses[i];
        if(p->ri==ri && p->ride==ride && p->part==part) return p;
    }
    CustNeonPulse *p=&g_cust_neon_pulses[g_cust_neon_cursor++%32u];
    *p=(CustNeonPulse){ri,ride,part,1.0f,-1.0f};return p;
}
static uint32_t cust_neon_color(uint32_t ri,uint32_t ride,uint32_t part) {
    const uint32_t morph=cust_attr(part,cust_hash("MORPHTARGET_NUM"),0u);
    float a[3],b[3],speed=0.0f,min=0.3f;
    int dual=0;
    if(morph) {
        cust_neon_hsv(cust_neon_zone(ride,0),cust_neon_zone(ride,1),cust_neon_zone(ride,2),a);
        speed=morph>=4u?cust_neon_zone(ride,3):0.0f;
        if(morph>4u) {dual=1;min=0.0f;cust_neon_hsv(cust_neon_zone(ride,4),cust_neon_zone(ride,5),cust_neon_zone(ride,6),b);}
    } else {
        a[0]=(float)cust_attr(part,cust_hash("RED"),0u);
        a[1]=(float)cust_attr(part,cust_hash("GREEN"),0u);
        a[2]=(float)cust_attr(part,cust_hash("BLUE"),0u);
        const uint32_t attrArgs[2]={cust_hash("RED2"),0u};
        dual=cust_call(0x007b00e0u,part,attrArgs,2u)!=0u;
        if(dual){b[0]=(float)cust_attr(part,cust_hash("RED2"),0u);b[1]=(float)cust_attr(part,cust_hash("GREEN2"),0u);b[2]=(float)cust_attr(part,cust_hash("BLUE2"),0u);}
        if(cust_attr(part,cust_hash("PULSE"),0u))speed=0.5f;
    }
    CustNeonPulse *p=cust_neon_pulse(ri,ride,part);
    if(speed>0.0f){p->value+=cust_neon_dt()*p->direction*speed*3.0f;if(p->value>1.0f){p->value=1.0f;p->direction=-1.0f;}if(p->value<min){p->value=min;p->direction=1.0f;}}
    else p->value=1.0f;
    float alpha=255.0f;
    if(dual){float t=1.0f-p->value;for(int i=0;i<3;++i)a[i]=a[i]+(b[i]-a[i])*t;}
    else {for(int i=0;i<3;++i)a[i]*=p->value;alpha*=p->value;}
    return cust_neon_byte(a[0]) | (cust_neon_byte(a[1])<<8u) | (cust_neon_byte(a[2])<<16u) | (cust_neon_byte(alpha)<<24u);
}
static void cust_neon_vertex_color(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
    (void)inv;(void)user;
    const uint32_t caller=get_u32(cpu->esp);
    if(!g_cust_neon_drawing || (caller!=0x007beb00u && caller!=0x007beb20u))return;
    uint32_t original=get_u32(cpu->esp+4u);
    uint32_t alpha=((original>>24u)*(g_cust_neon_draw_color>>24u))/255u;
    api->guest_write_u32(api,cpu->esp+4u,(g_cust_neon_draw_color&0x00ffffffu)|(alpha<<24u));
}
static void cust_neon_shadow(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
    (void)user;
    const uint32_t ri=cpu->ecx;
    uint32_t args[6];for(unsigned i=0;i<6u;++i)args[i]=get_u32(cpu->esp+4u+i*4u);
    api->call_next(api,inv,cpu); /* Always retain original ambient shadow. */
    if(g_cust_neon_drawing || !cust_pilot_enabled() || !cust_catalog_ready() || !ri)return;
    const uint32_t ride=get_u32(ri+0x3f0u);
    if(!ride || !CUST_NEON_CAR_SUPPORTED(get_u32(ride)))return;
    const uint32_t part=get_u32(ride+0x54u+0x68u*4u);
    if(!part)return;
    const uint32_t textureHash=cust_attr(part,cust_hash("TEXTURE_NAME"),0u);
    /* Reject stock/missing/foreign slot contents instead of drawing garbage. */
    if(textureHash!=cust_hash("CARSHADOW_NEON") && textureHash!=cust_hash("CARSHADOW_LED"))return;
    const uint32_t textureArgs[3]={textureHash,0u,0u};
    const uint32_t texture=cust_call(0x0055cfd0u,0u,textureArgs,3u);
    if(!texture)return;
    uint32_t original=get_u32(ri+0x3f8u);
    g_cust_neon_draw_color=cust_neon_color(ri,ride,part);
    g_cust_neon_drawing=1;
    api->guest_write_u32(api,ri+0x3f8u,texture);
    (void)cust_call(0x007be4c0u,ri,args,6u);
    api->guest_write_u32(api,ri+0x3f8u,original);
    g_cust_neon_drawing=0;
}
static int cust_neon_install(void) {
    if(!cust_pilot_enabled())return 0;
    if(g_api->hook_install(g_api,0x0070f580u,cust_neon_vertex_color,POP_HOOK_BEFORE,NULL,&g_cust_neon_hooks[0])!=POP_OK)return 0;
    if(g_api->hook_install(g_api,0x007be4c0u,cust_neon_shadow,POP_HOOK_WRAP,NULL,&g_cust_neon_hooks[1])!=POP_OK){g_api->hook_remove(g_api,g_cust_neon_hooks[0]);g_cust_neon_hooks[0]=0;return 0;}
    log_line("customization: native projected underglow renderer installed");return 1;
}
