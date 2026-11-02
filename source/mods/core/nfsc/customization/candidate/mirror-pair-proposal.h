/* Authored LegacyCustomization HandleSideMirrors normal native flow.
 * No extra hook: existing8597F0 garage wrapper owns preview/cancel/commit.
 */
static uint32_t g_cust_body_setup_menu;
static int g_cust_body_setup_added;
static int cust_body_mirror_supported(uint32_t car){
 return cust_pilot_enabled()&&cust_catalog_ready()&&cust_car_supported(car)&&cust_extension_next(0x22u,0u,0u,0xffffffffu,car)!=0u;
}
static void cust_body_pair_mirrors(uint32_t ride,uint32_t record){
 if(!ride||!cust_body_mirror_supported(get_u32(ride)))return;
 const uint32_t left=get_u32(ride+0x54u+0x22u*4u);if(!left)return;
 const uint32_t arg[1]={get_u32(ride)},nameptr=cust_call(0x007b0290u,0u,arg,1u);if(!nameptr)return;
 char carname[48];uint32_t i=0u;
 for(;i<sizeof(carname)-1u;++i){const uint32_t at=nameptr+i;carname[i]=(char)((get_u32(at&~3u)>>((at&3u)*8u))&255u);if(!carname[i])break;}
 if(i==sizeof(carname)-1u)return;
 char name[96];const uint32_t kit=cust_attr(left,0x796c0cb0u,0u);
 if(kit>99u)return;
 if(kit)snprintf(name,sizeof name,"%s_KIT%02u_RIGHT_SIDE_MIRROR",carname,kit);
 else snprintf(name,sizeof name,"%s_RIGHT_SIDE_MIRROR",carname);
 /* Native Install deliberately stores null when the named right part is absent.
  * This is the authored one-sided CHARGER69 behavior, not an invented mesh. */
 const uint32_t args[5]={ride,record,0x2cu,1u,cust_hash(name)};
 cust_call(0x0084efe0u,0u,args,5u);
}

static int cust_body_imported(uint32_t slot,uint32_t car){return cust_pilot_enabled()&&cust_catalog_ready()&&cust_car_supported(car)&&cust_extension_next(slot,0u,0u,0xffffffffu,car)!=0u;}
static int cust_body_tail_supported(uint32_t car){return cust_body_imported(0x1eu,car);}
/* Source FeCustomizePartsMenu.h17: the three default-only source models
 * require live racingClass==2. The other13 explicitly configured models keep
 * their enable. Resolve the actual eligible roof part model, not guessed IDs. */
static int cust_body_default_roof_gate(void){
 const uint32_t args[2]={0x247e0956u,0u};
 const uint32_t attr=cust_call(0x00463a80u,0x00bbabb0u,args,2u);
 return attr&&get_u32(attr)==2u;
}
static int cust_body_roof_supported(uint32_t car){
 if(!cust_pilot_enabled()||!cust_catalog_ready()||!cust_car_supported(car))return 0;
 const uint32_t part=cust_extension_next(0x4cu,0u,0u,0xffffffffu,car);
 const uint32_t models=get_u32(0x00b74cb8u);
 if(!part||!models)return 0;
 const uint32_t model=get_u32(models+((get_u32(part)>>8u)&255u)*4u);
 if(model==cust_hash("300C")||model==cust_hash("CHARGER06")||model==cust_hash("MUSTANGGT"))return cust_body_default_roof_gate();
 return 1;
}
/* Proven vector scroller descriptor list: menu+28, listpointer+114,
 * intrusive next node at descriptor+4; descriptor+30 is native Parts slot. */
static int cust_body_has_category(uint32_t menu,uint32_t slot){
 const uint32_t list=get_u32(menu+0x28u+0x114u);if(!list)return 1;
 uint32_t node=get_u32(list),seen=0u;
 while(node&&node!=list&&seen++<4096u){if(node<4u)return 1;const uint32_t option=node-4u;if(get_u32(option+0x3cu)!=4u&&get_u32(option+0x30u)==slot)return 1;node=get_u32(node);}
 return node!=list;
}
static int cust_body_car_name(uint32_t ride,char*name,uint32_t capacity){
 const uint32_t arg[1]={get_u32(ride)},ptr=cust_call(0x007b0290u,0u,arg,1u);if(!ptr||capacity<2u)return 0;
 for(uint32_t i=0;i<capacity;++i){const uint32_t at=ptr+i;name[i]=(char)((get_u32(at&~3u)>>((at&3u)*8u))&255u);if(!name[i])return i!=0u;}return 0;
}
static void cust_body_named_pair(uint32_t ride,uint32_t record,uint32_t slot,const char*car,uint32_t kit,const char*suffix){
 if(kit>99u)return;char name[112];
 if(kit)snprintf(name,sizeof name,"%s_KIT%02u_%s",car,kit,suffix);else snprintf(name,sizeof name,"%s_%s",car,suffix);
 const uint32_t args[5]={ride,record,slot,1u,cust_hash(name)};cust_call(0x0084efe0u,0u,args,5u);
}
static void cust_body_tail_badging(uint32_t ride,uint32_t record){
 if(!ride||!cust_pilot_enabled()||!cust_catalog_ready()||!cust_car_supported(get_u32(ride)))return;
 char car[48];if(!cust_body_car_name(ride,car,sizeof car))return;
 if(cust_body_tail_supported(get_u32(ride))){
  const uint32_t left=get_u32(ride+0x54u+0x1eu*4u);
  if(left){const uint32_t kit=cust_attr(left,0x796c0cb0u,0u);
   cust_body_named_pair(ride,record,0x28u,car,kit,"RIGHT_BRAKELIGHT");
   cust_body_named_pair(ride,record,0x1fu,car,kit,"LEFT_BRAKELIGHT_GLASS");
   cust_body_named_pair(ride,record,0x29u,car,kit,"RIGHT_BRAKELIGHT_GLASS");
  }
 }
 /* Version1 source: these sourcecars do not enable separate fender menus.
  * Each imported badging companion follows its actual adjacent bumper kit. */
 for(uint32_t side=0u;side<2u;++side){const uint32_t slot=side?0x4bu:0x49u;
  if(!cust_body_imported(slot,get_u32(ride)))continue;
  const uint32_t body=get_u32(ride+0x54u+0x17u*4u);
  if(body&&!cust_attr(body,0x03b83203u,0u)){const uint32_t args[3]={ride,record,slot};cust_call(0x0083eeb0u,0u,args,3u);continue;}
  const uint32_t bumper=get_u32(ride+0x54u+(slot-1u)*4u);if(!bumper)continue;
  const uint32_t kit=cust_attr(bumper,0x796c0cb0u,0u);char name[112];if(kit>99u)continue;
  snprintf(name,sizeof name,"%s_KIT%02u_%s_BUMPER_BADGING_SET",car,kit,side?"REAR":"FRONT");
  const uint32_t args[5]={ride,record,slot,1u,cust_hash(name)};cust_call(0x0084efe0u,0u,args,5u);
 }
}
