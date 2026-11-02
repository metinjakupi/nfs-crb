/* Private additive vector vinyl support. Include after advanced-customization.h.
 * Native call sites/ABI traced against pinned Carbon1.4 NFSC.exe. This header
 * changes no stock transforms, vectors, prices, ownership, saves or career flags.
 * Both hooks must install before the extra level14choices are appended.
 * User policy: newly imported cosmetics purchasable at ordinary native prices.
 */
#include "native-vector-resource-support.h"
static uint32_t g_cust_tv_hooks[3];
static int g_cust_tv_ready;
static int cust_tv_name(uint32_t hash) {
    static const uint32_t imported[] = {
        0xd69e6a20u, /* 2T_ZONDA */
        0xff87af6eu, /* MANUFACTURER_AUDI */
        0x3e0be1d1u, /* MANUFACTURER_BMW */
        0xa6af99d8u, /* MANUFACTURER_CADILLAC */
        0xff8a39efu, /* MANUFACTURER_FIAT */
        0xf5f527eeu, /* MANUFACTURER_KOENIGSEGG */
        0xf13c3b9cu, /* MANUFACTURER_LEXUS */
        0x21c647bbu, /* MANUFACTURER_PAGANI */
        0x7bc07899u, /* MANUFACTURER_PONTIAC */
        0x72070e66u, /* ST_CAMARON */
        0x497dc6d6u, /* ST_CUDA4 */
        0x499d2017u, /* ST_ELISE */
        0x7d412329u, /* ST_ELISE2 */
        0x8909cc1fu, /* CUSTOM_GALLARDO */
        0xc10416a0u, /* PINSTRIPE12 */
        0xdda7159bu, /* DARIUSHENCHMAN2 */
        0x97b92efbu, /* BOSS_21ST_GENERIC */
        0xc573ffd2u, /* NEVILLE_DEMO */
        0xf27348c3u, /* NEVILLE_DEMO_2 */
        0xf27348c4u, /* NEVILLE_DEMO_3 */
        0x78c80116u, /* AFTERMARKET_APR_PERFOMANCE */
        0xfecc9584u, /* AFTERMARKET_CENTERFORCE */
        0xc0e547c0u, /* AFTERMARKET_CRANE_CAMS */
        0xe4c9449fu, /* AFTERMARKET_DART */
        0xd6549095u, /* AFTERMARKET_ENERGYSUSPENSION */
        0x7e1daa30u, /* AFTERMARKET_FOOSE */
        0xbb0f685cu, /* AFTERMARKET_G-RACING */
        0xe55e96f1u, /* AFTERMARKET_HOTCHKIS */
        0xe4cc4e34u, /* AFTERMARKET_ISKY */
        0x1a7a1d33u, /* AFTERMARKET_JEPISTONS */
        0x4f219335u, /* AFTERMARKET_LEXANI */
        0x51290bbau, /* AFTERMARKET_MANLEY */
        0x52298f79u, /* AFTERMARKET_MOTEGI */
        0x2ac91288u, /* AFTERMARKET_RICHMOND */
        0xe5558f04u, /* AFTERMARKET_STREETGLOW */
        0x691005deu, /* AFTERMARKET_WISECO */
    };
    for(uint32_t i=0;i<sizeof(imported)/sizeof(imported[0]);++i)
        if(imported[i]==hash)return 1;
    return 0;
}
static int cust_tv_enabled(void) {
    const char *flag=getenv("RECOMP_CUSTOM_COSMETICS_SHOP");
    return g_cust_tv_ready && flag && !strcmp(flag,"1") &&
        cust_pilot_enabled() && cust_catalog_ready() && cust_supported() &&
        cust_vector_resources_install();
}
static int cust_tv_part(uint32_t part) {
    if(!part || !cust_extension_part(part))return 0;
    const uint32_t id=cust_attr(part,0x9239cf16u,0u);
    if(((id>>8u)&255u)!=99u)return 0; /* PartGroup VECTORVINYL */
    return cust_tv_name(cust_call(0x007cd930u,part,NULL,0u));
}
/* VinylSelectablePart vtable9d18a8+30 =>00577e40 calls004b0e70.
 * StandardSelectablePart's004b0b70 hook does not cover this separate path. */
static void cust_tv_availability(const PopModApi *api,pop_cpu_v1 *cpu,
                                PopHookInvocation *inv,void *user) {
    (void)user;
    const uint32_t object=cpu->ecx;
    const int eligible=cust_tv_enabled() && object &&
        get_u32(object)==0x009d18a8u && cust_tv_part(get_u32(object+0x18u));
    api->call_next(api,inv,cpu);
    if(eligible)cpu->eax=(cpu->eax&0xffffff00u)|1u;
}
static int cust_tv_in_list(uint32_t head,uint32_t part) {
    for(uint32_t n=get_u32(head),seen=0;n && n!=head && seen<4096u;n=get_u32(n),++seen)
        if(get_u32(n+0x14u)==part)return 1; /* node=object+4, part=object+18 */
    return 0;
}
/* The native list preserves BRAND_NAME/SPECIFICCARNAME/ONLINE predicates,
 * but00588ba5..bad unconditionally hides levels14/15. Append only imported
 * level14choices after the native list and keep the same filters/real class.
 * cdecl00588af0 args: layer,head,brandHash,carNameHash,levelFilter,unused.
 */
static void cust_tv_list(const PopModApi *api,pop_cpu_v1 *cpu,
                         PopHookInvocation *inv,void *user) {
    (void)user;
    const uint32_t layer=get_u32(cpu->esp+4u),head=get_u32(cpu->esp+8u);
    const uint32_t brand=get_u32(cpu->esp+12u),car=get_u32(cpu->esp+16u);
    const uint32_t filter=get_u32(cpu->esp+20u);
    const int eligible=cust_tv_enabled() && head &&
        get_u32(head) && get_u32(head+4u);
    api->call_next(api,inv,cpu);
    if(!eligible || (filter!=0xffffffffu && filter!=14u))return;
    for(uint32_t node=get_u32(0x00b76690u),pack=0;
        node && node!=0x00b76690u && pack<64u;node=get_u32(node),++pack) {
        const uint32_t records=get_u32(node+0x34u),count=get_u32(node+0x38u),base=get_u32(node+0x3cu);
        if(base+count<=21441u || base>21476u)continue;
        for(uint32_t i=0;i<count;++i) {
            const uint32_t part=records+i*4u,index=base+i;
            if(index<21441u || index>21476u || !cust_tv_part(part))continue;
            const uint32_t id=cust_attr(part,0x9239cf16u,0u);
            if((id&255u)!=14u || cust_tv_in_list(head,part))continue;
            const uint32_t partBrand=cust_attr(part,0xebb03e66u,0u);
            if(partBrand!=cust_hash("GENERIC") && partBrand!=brand)continue;
            const uint32_t specific=cust_attr(part,0x8e73b5dcu,0xffffffffu);
            if(specific!=0xffffffffu && specific!=car)continue;
            const uint32_t predicate[1]={part};
            if(!(cust_call(0x00577620u,0u,predicate,1u)&255u))continue;
            const uint32_t allocate[1]={0x24u};
            const uint32_t object=cust_call(0x006a1560u,0u,allocate,1u);
            void *memory=NULL;
            if(!object || api->guest_ptr(api,object,0x24u,&memory)!=POP_OK || !memory)return;
            memset(memory,0,0x24u);
            api->guest_write_u32(api,object,0x009d18a8u);
            api->guest_write_u32(api,object+0xcu,0x881f8efau);
            api->guest_write_u32(api,object+0x10u,layer);
            api->guest_write_u32(api,object+0x14u,0xffffu);
            api->guest_write_u32(api,object+0x18u,part);
            api->guest_write_u32(api,object+0x1cu,0x009d1890u);
            const uint32_t item=object+4u,tail=get_u32(head+4u);
            api->guest_write_u32(api,item,head);
            api->guest_write_u32(api,item+4u,tail);
            api->guest_write_u32(api,tail,item);
            api->guest_write_u32(api,head+4u,item);
        }
    }
}
/* Register before the first streamed vector, including a saved imported vinyl
 * loaded before any customization menu is opened. The resource index is global. */
static void cust_tv_resource_stream(const PopModApi *api, pop_cpu_v1 *cpu,
                                    PopHookInvocation *inv, void *user) {
    (void)user;
    if (cust_pilot_enabled() && cust_catalog_ready()) cust_vector_resources_install();
    api->call_next(api, inv, cpu);
}
static int cust_tv_install(void) {
    if(!cust_pilot_enabled())return 0;
    const uint32_t addresses[3]={0x00577e40u,0x00588af0u,0x007e43f0u};
    PopHookFn hooks[3]={cust_tv_availability,cust_tv_list,cust_tv_resource_stream};
    for(uint32_t i=0;i<3u;++i) {
        const PopModStatus status=g_api->hook_install(g_api,addresses[i],hooks[i],POP_HOOK_WRAP,NULL,&g_cust_tv_hooks[i]);
        if(status==POP_OK)continue;
        while(i){--i;g_api->hook_remove(g_api,g_cust_tv_hooks[i]);g_cust_tv_hooks[i]=0u;}
        log_line("customization pilot: imported vinyl hooks unavailable");return 0;
    }
    g_cust_tv_ready=1;
    log_line("customization pilot: imported vector vinyl availability ready");return 1;
}

static void cust_tv_shutdown(void) {
    g_cust_tv_ready = 0;
    for (uint32_t i = 0; i < 3u; ++i) {
        if (g_cust_tv_hooks[i]) g_api->hook_remove(g_api, g_cust_tv_hooks[i]);
        g_cust_tv_hooks[i] = 0u;
    }
    cust_vector_resources_shutdown();
}
