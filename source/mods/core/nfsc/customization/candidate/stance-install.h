static uint32_t g_cust_stance_extra_hooks[5];
static void cust_stance_slider_act(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
 (void)user;const uint32_t current=cust_stance_as_current();
 const PopModStatus status=api->call_next(api,inv,cpu);
 if(status!=POP_OK || !current || get_u32(0x00bbb050u)!=current)return;
 const uint32_t node=get_u32(0x00b77970u),ride=node && node!=0x00b77970u?node+8u:0u;
 const uint32_t render=ride?get_u32(ride+0xbf0u):0u;
 if(cust_stance_ride_part(ride) && render && get_u32(render+0x3f0u)==ride)cust_call(0x007cb6f0u,render,NULL,0u);
}
static void cust_stance_title(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
 (void)user;const uint32_t screen=cpu->ecx;api->call_next(api,inv,cpu);
 if(cust_stance_as_current()){const uint32_t args[1]={cust_hash("CUST_PARTS_STANCE_TITLE")};cust_call(0x005a3710u,screen,args,1u);}
}
static void cust_stance_shutdown(void) {
 for(unsigned i=0;i<sizeof(g_cust_stance_extra_hooks)/sizeof(g_cust_stance_extra_hooks[0]);++i)if(g_cust_stance_extra_hooks[i]){g_api->hook_remove(g_api,g_cust_stance_extra_hooks[i]);g_cust_stance_extra_hooks[i]=0u;}
 for(unsigned i=0;i<13u;++i)if(g_cust_stance_as_hooks[i]){g_api->hook_remove(g_api,g_cust_stance_as_hooks[i]);g_cust_stance_as_hooks[i]=0u;}
 g_cust_stance_ready=0;
}
static int cust_stance_install(void) {
 if(!cust_pilot_enabled() || !cust_stance_as_install())return 0;
 const struct{uint32_t addr;PopHookFn fn;int mode;uint32_t return_pc;int filtered_only;}hooks[]={
  {0x007cb6f0u,cust_stance_wheel_layout,POP_HOOK_WRAP},
  /* _ftol2 is also used outside wheel rendering. Select the two native
   * camber callers before copying CPU state. The callback always delegates,
   * so use wraps for the disjoint filters; two REPLACE hooks are exclusive.
   * Older hosts retain the original single REPLACE registration. */
  {0x0087eac0u,cust_stance_camber_ftol,POP_HOOK_WRAP,0x007df79bu,0},
  {0x0087eac0u,cust_stance_camber_ftol,POP_HOOK_WRAP,0x007df7d1u,1},
  {0x0085b020u,cust_stance_slider_act,POP_HOOK_WRAP},
  {0x0085b1d0u,cust_stance_title,POP_HOOK_WRAP}
 };
 const int filtered=g_api->size>=offsetof(PopModApi,hook_install_at_callsite)+sizeof g_api->hook_install_at_callsite && g_api->hook_install_at_callsite;
 for(unsigned i=0;i<sizeof(hooks)/sizeof(hooks[0]);++i){
  if(!filtered && hooks[i].filtered_only)continue;
  const PopModStatus status=filtered && hooks[i].return_pc?
   g_api->hook_install_at_callsite(g_api,hooks[i].addr,hooks[i].return_pc,hooks[i].fn,hooks[i].mode,NULL,&g_cust_stance_extra_hooks[i]):
   g_api->hook_install(g_api,hooks[i].addr,hooks[i].fn,hooks[i].return_pc?POP_HOOK_REPLACE:hooks[i].mode,NULL,&g_cust_stance_extra_hooks[i]);
  if(status!=POP_OK){cust_stance_shutdown();return 0;}
 }
 log_line("customization: source stance controls and scoped wheel drawing installed");return 1;
}
