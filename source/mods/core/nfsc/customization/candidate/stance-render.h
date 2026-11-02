/* Private stance42 render contract. All edits are per-car drawing data.
 * Native stock/factory camber, track and width remain the baseline. Unlike the
 * Windows mod's global CamberMult=0 patch, this scoped port adds stance only
 * when one of the three actual source stage records is installed. */
static float cust_stance_clamp(float value){return isfinite(value)?fminf(1.0f,fmaxf(0.0f,value)):0.0f;}
static int cust_stance_part_kind(uint32_t part) {
 if(!part)return -1;
 const uint32_t name=cust_call(0x007cd930u,part,NULL,0u);
 if(name==cust_hash("STANCE_STANCE_STOCK"))return 0;
 if(name==cust_hash("STANCE_STANCE_STAGE1"))return 1;
 if(name==cust_hash("STANCE_STANCE_STAGE2"))return 2;
 if(name==cust_hash("STANCE_STANCE_STAGE3"))return 3;
 return -1;
}
static uint32_t cust_stance_ride_part(uint32_t ride) {
 if(!cust_pilot_enabled() || !cust_catalog_ready() || !ride || !cust_car_supported(get_u32(ride)))return 0u;
 const uint32_t part=get_u32(ride+0x54u+0x69u*4u);
 return cust_stance_part_kind(part)>0?part:0u;
}
static uint32_t cust_stance_render_ride(uint32_t render) {
 const uint32_t ride=render?get_u32(render+0x3f0u):0u;
 /* Native CarRI+3F0 remains authoritative in FreeRoam. Ride+BF0
  * is a frontend-only reciprocal field and contains world data in-race. */
 return ride && cust_car_supported(get_u32(ride))?ride:0u;
}
static float cust_stance_zone(uint32_t ride,uint32_t zone) {
 const uint32_t part=cust_stance_ride_part(ride);
 if(!part || zone>=6u || zone>=cust_attr(part,cust_hash("MORPHTARGET_NUM"),0u))return 0.0f;
 const float value=bits_float(get_u32(ride+0xa70u+zone*4u));
 return isfinite(value)?fminf(1.0f,fmaxf(0.0f,value)):0.0f;
}
static void cust_stance_wheel_layout(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
 (void)user;const uint32_t render=cpu->ecx;
 const uint32_t ride=cust_stance_render_ride(render);
 const int active=cust_stance_ride_part(ride)!=0u && get_u32(render+0x3f4u)!=0u;
 const PopModStatus status=api->call_next(api,inv,cpu);
 if(status!=POP_OK || !active || get_u32(render+0x3f0u)!=ride)return;
 for(uint32_t wheel=0;wheel<4u;++wheel) {
  const uint32_t axle=wheel>=2u?1u:0u;
  const float track=cust_stance_zone(ride,2u+axle)/7.0f+cust_stance_zone(ride,axle)/20.0f;
  const uint32_t at=render+0x460u+wheel*4u;const float baseline=bits_float(get_u32(at));
  if(track!=0.0f && isfinite(baseline))put_float(at,baseline+(baseline>0.0f?track:-track));
  const uint32_t scaleAt=render+0x480u+wheel*4u;const float scale=bits_float(get_u32(scaleAt));
  const float multiplier=1.0f+cust_stance_zone(ride,4u+axle)/2.0f;
  if(multiplier!=1.0f && isfinite(scale))put_float(scaleAt,scale*multiplier);
 }
}
/* Exact native007de460 callsites. ESI is CarRenderInfo. Native has pushed
 * angle conversion inST0 and kept its unscaled camber copy inST1. Update both
 * before native0087eac0 consumes ST0, so float and fixed-angle drawing agree.
 * pop_mod_api.h exposes physical st[8]; hooks.cpp snapshots/restores the same
 * physical entries (fpu_top selects ST0). No guest stack locals are guessed.
 * Current native skips this pair for LOD>1; parent must retain that explicit
 * limit until a safe equivalent of the source ASI branch removal is verified. */
static void cust_stance_camber_ftol(const PopModApi *api,pop_cpu_v1 *cpu,PopHookInvocation *inv,void *user) {
 (void)user;const uint32_t caller=get_u32(cpu->esp);
 if((caller==0x007df79bu || caller==0x007df7d1u) &&
    cpu->size>=offsetof(pop_cpu_v1,fpu_tag)+sizeof(cpu->fpu_tag) && cpu->fpu_top<8u) {
  const uint32_t render=cpu->esi,ride=cust_stance_render_ride(render);
  const uint32_t axle=caller==0x007df7d1u?1u:0u;
  const float zone=cust_stance_zone(ride,axle);
  const uint32_t top=cpu->fpu_top,next=(top+1u)&7u;
  const double scale=(double)bits_float(get_u32(0x009f2588u));
  if(zone!=0.0f && ((cpu->fpu_tag>>(top*2u))&3u)!=3u &&
     ((cpu->fpu_tag>>(next*2u))&3u)!=3u &&
     isfinite(cpu->st[top]) && isfinite(cpu->st[next]) && isfinite(scale)) {
   const double delta=(double)(zone*1.3f);
   cpu->st[top]+=delta*scale;cpu->st[next]+=delta;
  }
 }
 api->call_next(api,inv,cpu);
}
