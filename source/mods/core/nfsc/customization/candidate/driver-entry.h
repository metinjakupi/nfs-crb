/* Source driver data/menu port. Engine owns the resource, fitting, pose,
 * standard selectable purchase/cancel and save. No playable CarType is added. */
static uint32_t g_cust_driver_hooks[2],g_cust_driver_allowed,g_cust_driver_resource;
static int cust_driver_assets_ready(void){
 if(!g_cust_driver_ready || !cust_pilot_enabled() || !cust_catalog_ready() || !g_cust_driver_resource || !get_u32(g_cust_driver_resource+0x40u))return 0;
 for(unsigned style=1u;style<=4u;++style)for(unsigned lod=0;lod<2u;++lod){char name[48];snprintf(name,sizeof(name),"DRIVERS_KIT%02u_DRIVER_%c",style,lod?'C':'A');const uint32_t args[1]={cust_hash(name)};if(!cust_call(0x0055df00u,0u,args,1u))return 0;}
 const char *names[]={"DRIVER_RYAN","DRIVER_FEMALE","DRIVER_UC","DRIVER_UC_FEMALE","DRIVER_CUTOUT_HELMET","DRIVER_CUTOUT_FEMALE"};
 for(unsigned i=0;i<6u;++i){const uint32_t args[3]={cust_hash(names[i]),0u,0u};if(!cust_call(0x0055cfd0u,0u,args,3u))return 0;}
 return 1;
}
static int cust_driver_menu_supported(void){return cust_driver_assets_ready() && cust_car_supported(cust_car_id());}
/* Native007b0360 cdecl(slot,car) returns seven hashes, resolving stock -1 to
 * the current car. Preserve all seven and use a zero slot for source DRIVERS.
 * This is the actual SlotMainOverride effect without appending a CarTypeInfo. */
static void cust_driver_allowed_models(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user){
 (void)user;uint32_t slot=get_u32(cpu->esp+4u),car=get_u32(cpu->esp+8u);
 if(api->call_next(api,inv,cpu)!=POP_OK || slot!=0x2du || !g_cust_driver_ready || !cust_pilot_enabled() || !cust_catalog_ready() || !cust_car_supported(car) || !g_cust_driver_allowed || !cpu->eax)return;
 uint32_t hashes[7],freeSlot=7u;for(unsigned i=0;i<7u;++i){hashes[i]=get_u32(cpu->eax+i*4u);if(hashes[i]==cust_hash("DRIVERS"))return;if(!hashes[i] && freeSlot==7u)freeSlot=i;}
 if(freeSlot==7u)return;hashes[freeSlot]=cust_hash("DRIVERS");for(unsigned i=0;i<7u;++i)api->guest_write_u32(api,g_cust_driver_allowed+i*4u,hashes[i]);cpu->eax=g_cust_driver_allowed;
}
static void cust_driver_after_initialize(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user){
 (void)user;if(api->call_next(api,inv,cpu)!=POP_OK || !cust_pilot_enabled() || !g_cust_driver_ready || g_cust_driver_resource)return;
 g_cust_driver_resource=cust_tire_request_resource("CARS\\DRIVERS\\GEOMETRY.BIN");
 if(!g_cust_driver_resource)log_line("customization drivers: resource request failed; stock driver path retained");
}
static void cust_driver_shutdown(void){
 for(unsigned i=0;i<2u;++i)if(g_cust_driver_hooks[i]){g_api->hook_remove(g_api,g_cust_driver_hooks[i]);g_cust_driver_hooks[i]=0u;}
 if(g_cust_driver_allowed){g_api->guest_free(g_api,g_cust_driver_allowed);g_cust_driver_allowed=0u;}g_cust_driver_ready=0;
}
static int cust_driver_install(void){
 if(!cust_pilot_enabled() || g_api->guest_alloc(g_api,28u,&g_cust_driver_allowed)!=POP_OK || !g_cust_driver_allowed)return 0;
 const struct{uint32_t address;PopHookFn fn;}hooks[]={{0x007b0360u,cust_driver_allowed_models},{0x006b7620u,cust_driver_after_initialize}};
 for(unsigned i=0;i<2u;++i)if(g_api->hook_install(g_api,hooks[i].address,hooks[i].fn,POP_HOOK_WRAP,NULL,&g_cust_driver_hooks[i])!=POP_OK){cust_driver_shutdown();return 0;}return 1;
}
