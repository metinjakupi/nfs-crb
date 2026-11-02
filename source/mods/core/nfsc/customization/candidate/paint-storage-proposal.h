/* Private paint39 storage/list identity/name/hydration.
 * Include with the corresponding menu/render/install helpers.
 * Include after full-customization helpers and paint-channel-contract.h.
 * Parent must maintain selected-menu lifetime via whole menu route hook.
 */
static int g_cust_paint_menu = -1;
static int cust_paint_enabled(void) {
 return g_cust_paint_ready && cust_pilot_enabled() && cust_catalog_ready() && cust_supported();
}
static int cust_paint_channel_from_slot(uint32_t slot) {
 for(uint32_t i=0;i<6u;++i)if(cust_paint_slot_from_channel(i)==slot)return (int)i;
 return -1;
}
static uint32_t cust_paint_ordinal(uint32_t part,uint32_t slot) {
 if(!part)return 0u;
 uint32_t previous=0u;
 for(uint32_t n=1u;n<=4096u;++n){uint32_t p=cust_next(slot,0xffffffffu,previous);if(!p||p==previous)return 0u;if(p==part)return n;previous=p;}
 return 0u;
}
static uint32_t cust_paint_by_ordinal(uint32_t number,uint32_t slot) {
 if(!number||number>4096u)return 0u;
 uint32_t previous=0u;
 for(uint32_t n=1u;n<=number;++n){uint32_t p=cust_next(slot,0xffffffffu,previous);if(!p||p==previous)return 0u;previous=p;}
 return previous;
}
static void cust_paint_float_write(uint32_t at,float value) {
 uint32_t bits;memcpy(&bits,&value,4);g_api->guest_write_u32(g_api,at,bits);
}
static float cust_paint_float_read(uint32_t at) {
 uint32_t bits=get_u32(at);float value;memcpy(&value,&bits,4);return value;
}
static void cust_paint_list_identity(const PopModApi*api,pop_cpu_v1*cpu,PopHookInvocation*inv,void*user) {
 (void)user;const int channel=cust_paint_channel_from_menu((uint32_t)g_cust_paint_menu);
 const uint32_t slot=get_u32(cpu->esp+4u),head=get_u32(cpu->esp+8u),caller=get_u32(cpu->esp);
 const int list_context=(caller==0x00861b0eu&&slot==(channel>3?0x5bu:0x61u)) ||
                        (caller==0x0085c408u&&slot==(channel>3?0x5cu:0x62u));
 const int apply=cust_paint_enabled()&&g_cust_section==7u&&channel>=0&&list_context;
 api->call_next(api,inv,cpu);
 if(!apply||!head)return;
 for(uint32_t n=get_u32(head),count=0u;n&&n!=head&&count<4096u;n=get_u32(n),++count){
  const uint32_t object=n-4u;
  if(get_u32(object)==0x009f9e20u&&get_u32(object+0x18u)==slot)
   api->guest_write_u32(api,object+0x18u,cust_paint_slot_from_channel((uint32_t)channel));
 }
}
static void cust_paint_install(const PopModApi*api,pop_cpu_v1*cpu,PopHookInvocation*inv,void*user) {
 (void)user;const uint32_t object=cpu->ecx,record=get_u32(cpu->esp+4u);
 const int ch=cust_paint_channel_from_slot(get_u32(object+0x18u));
 if(!cust_paint_enabled()||get_u32(object)!=0x009f9e20u||ch<0){api->call_next(api,inv,cpu);return;}
 if(!record){api->hook_return(api,cpu,0u,8u);return;}
 const uint32_t one=cust_paint_ordinal(get_u32(object+0x10u),ch>3?0x5cu:0x62u);
 const uint32_t two=cust_paint_ordinal(get_u32(object+0x20u),ch>3?0x5bu:0x61u);
 if(!one||!two){api->hook_return(api,cpu,0u,8u);return;}
 const uint32_t at=cust_paint_saved_color(record,(uint32_t)ch),packed=get_u32(object+0x1cu);
 cust_paint_float_write(at,(float)one/100.0f);cust_paint_float_write(at+4u,(float)two/100.0f);
 cust_paint_float_write(at+8u,(float)(packed&255u)/100.0f);cust_paint_float_write(at+12u,(float)((packed>>8u)&255u)/100.0f);
 api->hook_return(api,cpu,1u,8u);
}
static void cust_paint_name(const PopModApi*api,pop_cpu_v1*cpu,PopHookInvocation*inv,void*user) {
 (void)user;const uint32_t object=cpu->ecx;const int ch=cust_paint_channel_from_slot(get_u32(object+0x18u));
 if(!cust_paint_enabled()||get_u32(object)!=0x009f9e20u||ch<0){api->call_next(api,inv,cpu);return;}
 static const char*names[6]={"CART_ITEM_NAME_CALIPER_PAINT","CART_ITEM_NAME_FRONT_RIM_PAINT","CART_ITEM_NAME_REAR_RIM_PAINT","CART_ITEM_NAME_REAR_WHEEL_PAINT","CART_ITEM_NAME_HOOD_PAINT","CART_ITEM_NAME_SPOILER_PAINT"};
 api->hook_return(api,cpu,cust_hash(names[ch]),0u);
}
/* Hydration retains native defaults for empty/invalid old channels. */
static void cust_paint_hydrate(const PopModApi*api,pop_cpu_v1*cpu,PopHookInvocation*inv,void*user) {
 (void)user;const uint32_t record=cpu->ecx,out=get_u32(cpu->esp+8u),caller=get_u32(cpu->esp);
 const int ch=cust_paint_channel_from_menu((uint32_t)g_cust_paint_menu);
 const int apply=cust_paint_enabled()&&g_cust_section==7u&&ch>=0&&out&&caller==0x00861c58u;
 api->call_next(api,inv,cpu);
 if(!apply)return;
 const uint32_t at=cust_paint_saved_color(record,(uint32_t)ch);
 const float i1=cust_paint_float_read(at),i2=cust_paint_float_read(at+4u),b=cust_paint_float_read(at+8u),s=cust_paint_float_read(at+12u);
 /* Empty old saves keep the native current body/wheel fallback supplied above.
  * Valid saved custom state replaces all four fields together. */
 if(!(i1>0.0f&&i1<=40.96f&&i2>0.0f&&i2<=40.96f&&b>=0.0f&&b<=1.0f&&s>=0.0f&&s<=1.0f))return;
 const uint32_t p1=cust_paint_by_ordinal((uint32_t)(i1*100.0f+0.5f),ch>3?0x5cu:0x62u);
 const uint32_t p2=cust_paint_by_ordinal((uint32_t)(i2*100.0f+0.5f),ch>3?0x5bu:0x61u);
 if(!p1||!p2)return;
 const uint32_t color=((uint32_t)(b*100.0f+0.5f)&255u)|(((uint32_t)(s*100.0f+0.5f)&255u)<<8u);
 api->guest_write_u32(api,out,(get_u32(out)&0xffff0000u)|color);
 api->guest_write_u32(api,out+4u,p1);api->guest_write_u32(api,out+8u,p2);
}
