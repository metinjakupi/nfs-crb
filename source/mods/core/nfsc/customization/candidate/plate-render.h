/* Private plate proposal: slot56, pose15, source replacement rows24..34.
 * Original factory mappings are restored when the custom source part leaves. */
static float cust_plate_clamp(float f){return isfinite(f)?fminf(1.0f,fmaxf(0.0f,f)):0.0f;}
static int cust_plate_part_kind(uint32_t part){
 if(!part)return -1;
 uint32_t table=get_u32(0x00b74cb8u),modelIndex=get_u32(part+1u)&255u;if(!table || get_u32(table+modelIndex*4u)!=1931923208u)return -1;
 uint32_t name=cust_call(0x007cd930u,part,NULL,0u);
 const char *names[]={"LICENSE_PLATE_STYLE01","LICENSE_PLATE_NONE","LICENSE_PLATE_STYLE02","LICENSE_PLATE_STYLE03","LICENSE_PLATE_STYLE04","LICENSE_PLATE_STYLE05"};
 for(unsigned i=0;i<6u;++i)if(name==cust_hash(names[i]))return (int)i;
 return -1;
}
static uint32_t cust_plate_render_ride(uint32_t render){uint32_t ride=render?get_u32(render+0x3f0u):0u;return ride && cust_car_supported(get_u32(ride))?ride:0u;}
static int cust_plate_supported_ride(uint32_t ride){
 if(!cust_pilot_enabled() || !cust_catalog_ready() || !ride || !cust_car_supported(get_u32(ride)))return 0;
 uint32_t render=get_u32(ride+0xbf0u);return render && cust_plate_render_ride(render)==ride && (get_u32(render+0x440u) || get_u32(render+0x444u));
}
/* CarRI+3F0 is the native owner in both garage and world. Ride+BF0 is
 * only a frontend reciprocal link; never require it for world drawing. */
static uint32_t cust_plate_ride_part(uint32_t ride){
 return cust_pilot_enabled() && cust_catalog_ready() && ride && cust_car_supported(get_u32(ride))?
  get_u32(ride+0x54u+0x56u*4u):0u;
}
static float cust_plate_zone(uint32_t ride,unsigned i){uint32_t part=cust_plate_ride_part(ride);return cust_plate_part_kind(part)>=2 && i<11u && i<cust_attr(part,cust_hash("MORPHTARGET_NUM"),0u)?cust_plate_clamp(bits_float(get_u32(ride+0xad0u+i*4u))):0.0f;}
/* Native plate render:7DF48B CALL729320, return7DF490. ESI is
 * CarRenderInfo and stack arg1 is the actual plate eModel. The native
 * replacement layout is NOT the EC Constants.h layout. Borrow eleven rows
 * only while this one plate draw consumes them; restore every word before
 * any subsequent model or native update can observe the table. */
static uint32_t cust_plate_draw_table(uint32_t render,uint32_t model){
 if(!render || !model || !cust_plate_render_ride(render))return 0u;
 int actual=0;for(unsigned i=0;i<4u;++i)
  if((get_u32(render+0x1378u+i*4u)&0xfffffffcu)==model)actual=1;
 if(!actual)return 0u;
 const uint32_t table=get_u32(model+0x10u),count=get_u32(model+0x14u)&0xffffu;
 /* Both tables are native74-entry allocations owned by this same RI. */
 return count==74u && (table==render+0x588u || table==render+0x900u)?table:0u;
}
static void cust_plate_render_model(const PopModApi *api,pop_cpu_v1 *cpu,
                                    PopHookInvocation *inv,void *user){
 (void)user;
 const uint32_t render=cpu->esi,model=get_u32(cpu->esp+4u)&0xfffffffcu;
 uint32_t table=0u,part=0u,ride=0u;
 if(get_u32(cpu->esp)==0x007df490u){
  ride=cust_plate_render_ride(render);part=cust_plate_ride_part(ride);
  if(cust_plate_part_kind(part)>=2)table=cust_plate_draw_table(render,model);
 }
 if(!table){api->call_next(api,inv,cpu);return;}
 uint32_t saved[33],replacement[33];
 for(unsigned i=0;i<33u;++i)saved[i]=get_u32(table+24u*12u+i*4u);
 for(unsigned i=0;i<11u;++i){
  char name[48];uint32_t oldHash,newHash;
  const unsigned index=(unsigned)(cust_plate_zone(ride,i)*100.0f+0.5f);
  if(i==0u){const uint32_t seed=cust_attr(part,cust_hash("TEXTURE_NAME"),0u);
   snprintf(name,sizeof(name),"%u",index);oldHash=cust_hash("LICENSEPLATE");newHash=cust_hash_seed(name,seed);}
  else if(i==1u){snprintf(name,sizeof(name),"LICENSEPLATE_FRAME%u",index);
   newHash=cust_hash(name);oldHash=cust_hash("LICENSEPLATE_FRAME");}
  else{snprintf(name,sizeof(name),"LICENSEPLATE_TEXT%u",index);newHash=cust_hash(name);
   snprintf(name,sizeof(name),"LICENSEPLATE_TEXT_ZONE%u",i-2u);oldHash=cust_hash(name);}
  const uint32_t args[3]={newHash,0u,0u};
  const uint32_t texture=cust_call(0x0055cfd0u,0u,args,3u);
  replacement[i*3u]=texture?oldHash:0u;
  replacement[i*3u+1u]=texture?newHash:0u;
  replacement[i*3u+2u]=texture?texture:0xffffffffu;
 }
 unsigned written=0u;
 for(;written<33u;++written)
  if(g_api->guest_write_u32(g_api,table+24u*12u+written*4u,replacement[written])!=POP_OK)break;
 if(written!=33u){
  for(unsigned i=0;i<written;++i)g_api->guest_write_u32(g_api,table+24u*12u+i*4u,saved[i]);
  api->call_next(api,inv,cpu);return;
 }
 api->call_next(api,inv,cpu);
 /* Native result/CPU state and model ownership are untouched. Local saved
  * rows make two-marker and nested draws independent without a cache. */
 for(unsigned i=0;i<33u;++i)g_api->guest_write_u32(g_api,table+24u*12u+i*4u,saved[i]);
}
static void cust_plate_model_name(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user){
 (void)user;uint32_t part=get_u32(cpu->esp+4u),as=get_u32(cpu->esp+16u);api->call_next(api,inv,cpu);
 if(!cust_pilot_enabled() || !cust_catalog_ready() || as || cust_plate_part_kind(part)!=3)return;
 for(unsigned i=0;i<4u;++i){char old[48],name[48];snprintf(old,sizeof(old),"LICENSE_PLATE_STYLE01_%c",'A'+i);if(cpu->eax==cust_hash(old)){snprintf(name,sizeof(name),"CBPLATE_TEXT7_%c",'A'+i);cpu->eax=cust_hash(name);break;}}
}

static int cust_plate_menu_supported(void){uint32_t n=get_u32(0x00b77970u);return n && n!=0x00b77970u && cust_plate_supported_ride(n+8u);}
