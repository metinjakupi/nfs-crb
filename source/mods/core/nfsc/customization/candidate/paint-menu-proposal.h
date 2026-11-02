/* Include after paint storage helpers. Existing main39 integration must add
 * section7 PAINTS and route it to native VISUALS choice3 in cust_main_choice.
 * Existing cust_main_choice BEFORE hook is the sole owner of that rewrite.
 */
static void cust_paint_add_text(uint32_t menu,const char*label,uint32_t id) {
 cust_add_option(menu,cust_hash(label),id);
}
static void cust_paint_visual_setup(const PopModApi*api,pop_cpu_v1*cpu,PopHookInvocation*inv,void*user) {
 (void)user;const uint32_t menu=cpu->ecx;
 if(!cust_paint_enabled()||g_cust_section!=7u){api->call_next(api,inv,cpu);return;}
 const uint32_t title[1]={cust_hash("CUST_MAINMENU_VISUALS_TITLE")};cust_call(0x005a3710u,menu,title,1u);
 cust_paint_add_text(menu,"CUST_MAINMENU_PAINT",0u);
 cust_paint_add_text(menu,"CUST_MAINMENU_HOOD_PAINT",9u);
 cust_paint_add_text(menu,"CUST_MAINMENU_SPOILER_PAINT",10u);
 cust_paint_add_text(menu,"CUST_MAINMENU_FRONT_WHEELS",1u);
 cust_paint_add_text(menu,"CUST_MAINMENU_FRONT_RIMS",6u);
 const uint32_t car=get_u32(0x00b77970u),ride=car&&car!=0x00b77970u?car+8u:0u,rear=ride?cust_bt_part(ride,0x55u):0u;
 if(!(rear&&cust_attr(rear,0x2c420d64u,0u))){
  cust_paint_add_text(menu,"CUST_MAINMENU_REAR_WHEELS",8u);
  cust_paint_add_text(menu,"CUST_MAINMENU_REAR_RIMS",7u);
 }
 cust_paint_add_text(menu,"CUST_MAINMENU_CALIPER_PAINT",5u);
 cust_call(0x005c5ef0u,menu,NULL,0u);
 const uint32_t initial[1]={0u};cust_call(0x005c5ed0u,menu,initial,1u);cust_call(0x005a32c0u,menu,NULL,0u);
 api->hook_return(api,cpu,0u,0u);
}
/* Existing main_choice BEFORE already owns whole868390. Integrate this helper
 * at start of that callback, before its state9-only return. No second hook. */
static void cust_paint_visual_choice(const PopModApi*api,pop_cpu_v1*cpu) {
 if(!cust_paint_enabled()||get_u32(cpu->ecx+4u)!=16u)return;
 if(g_cust_section!=7u){g_cust_paint_menu=-1;return;}
 const uint32_t id=get_u32(cpu->esp+4u);
 g_cust_paint_menu=(int)id;
 const int channel=cust_paint_channel_from_menu(id);
 if(channel>=0)api->guest_write_u32(api,cpu->esp+4u,channel>3?0u:1u);
}
