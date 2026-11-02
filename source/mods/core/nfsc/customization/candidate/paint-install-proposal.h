#include "paint-stock-menu-proposal.h"
static uint32_t g_cust_paint_hooks[6];
static int cust_paint_hooks_install(void){
 const uint32_t addresses[6]={0x0083fcd0u,0x008531a0u,0x00853080u,0x00842da0u,0x0049c540u,0x0083f720u};
 const PopHookFn functions[6]={cust_paint_visual_setup,cust_paint_list_identity,cust_paint_install,cust_paint_name,cust_paint_hydrate,cust_paint_stock_menu};
 for(uint32_t i=0;i<6u;++i){
  if(g_api->hook_install(g_api,addresses[i],functions[i],POP_HOOK_WRAP,NULL,&g_cust_paint_hooks[i])==POP_OK)continue;
  while(i){--i;g_api->hook_remove(g_api,g_cust_paint_hooks[i]);g_cust_paint_hooks[i]=0u;}return 0;
 }
 g_cust_paint_ready=1;return 1;
}
static void cust_paint_shutdown(void){
 for(uint32_t i=0;i<6u;++i)if(g_cust_paint_hooks[i]){g_api->hook_remove(g_api,g_cust_paint_hooks[i]);g_cust_paint_hooks[i]=0u;}
 for(uint32_t i=0;i<32u;++i)if(g_cust_paints[i].render)cust_paint_drop(g_cust_paints[i].render);
 g_cust_paint_ready=0;g_cust_paint_menu=-1;
}
