static uint32_t g_cust_plate_extra_hooks[3];
static void cust_plate_title(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user){(void)user;uint32_t screen=cpu->ecx;api->call_next(api,inv,cpu);if(cust_plate_as_current()){const uint32_t args[1]={cust_hash("CUST_PARTS_PLATES_TITLE")};cust_call(0x005a3710u,screen,args,1u);}}
static void cust_plate_shutdown(void){for(unsigned i=0;i<3u;++i)if(g_cust_plate_extra_hooks[i]){g_api->hook_remove(g_api,g_cust_plate_extra_hooks[i]);g_cust_plate_extra_hooks[i]=0u;}for(unsigned i=0;i<13u;++i)if(g_cust_plate_as_hooks[i]){g_api->hook_remove(g_api,g_cust_plate_as_hooks[i]);g_cust_plate_as_hooks[i]=0u;}g_cust_plate_ready=0;}
static int cust_plate_install(void){
 if(!cust_pilot_enabled() || !cust_plate_as_install())return 0;
 const struct{uint32_t addr;PopHookFn fn;int mode;uint32_t return_pc;}hooks[]={
 {0x00729320u,cust_plate_render_model,POP_HOOK_WRAP,0x007df490u},{0x007cda40u,cust_plate_model_name,POP_HOOK_WRAP},{0x0085b1d0u,cust_plate_title,POP_HOOK_WRAP}};
 /* Only the native plate callsite uses this wrapper. Other model draws keep
  * their original path without copying callback CPU state or a game view. */
 const int filtered=g_api->size>=offsetof(PopModApi,hook_install_at_callsite)+sizeof g_api->hook_install_at_callsite && g_api->hook_install_at_callsite;
 for(unsigned i=0;i<3u;++i){
  const PopModStatus status=filtered && hooks[i].return_pc?
   g_api->hook_install_at_callsite(g_api,hooks[i].addr,hooks[i].return_pc,hooks[i].fn,hooks[i].mode,NULL,&g_cust_plate_extra_hooks[i]):
   g_api->hook_install(g_api,hooks[i].addr,hooks[i].fn,hooks[i].mode,NULL,&g_cust_plate_extra_hooks[i]);
  if(status!=POP_OK){cust_plate_shutdown();return 0;}
 }
 return 1;
}
